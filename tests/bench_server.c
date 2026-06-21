/*
 * FlashKV Server Benchmark Tool
 *
 * Standalone TCP client that benchmarks the FlashKV server by sending
 * RESP-encoded commands and measuring round-trip latency.
 *
 * Zero FlashKV dependencies — only POSIX sockets + libc + libm.
 */

#define _POSIX_C_SOURCE 199309L

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ================================================================
 *  简易随机数 (内联 LCG, 避免 rand_r 的可移植性问题)
 * ================================================================ */

static unsigned int lcgRand(unsigned int *seed) {
    *seed = *seed * 1103515245U + 12345U;
    return *seed;
}

/* ================================================================
 *  延迟统计器 (复用 bench_dict.c 模式)
 * ================================================================ */

#define SAMPLE_CAP 1000000

typedef struct {
    long long *samples;
    int        count;
    int        cap;
} Latency;

static Latency latNew(void) {
    Latency l;
    l.samples = malloc(SAMPLE_CAP * sizeof(long long));
    if (!l.samples) { fprintf(stderr, "OOM for latency samples\n"); exit(1); }
    l.count = 0;
    l.cap   = SAMPLE_CAP;
    return l;
}

static void latRecord(Latency *l, long long ns) {
    if (l->count < l->cap) l->samples[l->count++] = ns;
}

static int latCmp(const void *a, const void *b) {
    long long va = *(const long long *)a;
    long long vb = *(const long long *)b;
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

static void latReport(Latency *l, const char *label) {
    if (l->count == 0) {
        printf("  %-12s (no samples)\n", label);
        return;
    }
    qsort(l->samples, l->count, sizeof(long long), latCmp);

    long long p50 = l->samples[l->count * 50 / 100];
    long long p95 = l->samples[l->count * 95 / 100];
    long long p99 = l->samples[l->count * 99 / 100];
    long long max = l->samples[l->count - 1];

    double sum = 0.0, sum2 = 0.0;
    for (int i = 0; i < l->count; i++) {
        sum  += (double)l->samples[i];
        sum2 += (double)l->samples[i] * (double)l->samples[i];
    }
    double avg = sum / l->count;
    double std = sqrt(sum2 / l->count - avg * avg);

    printf("  %-12s avg %8.0f ns   σ %5.0f ns   "
           "P50 %8lld ns   P95 %8lld ns   P99 %8lld ns   max %8lld ns  "
           "(n=%d)\n",
           label, avg, std, p50, p95, p99, max, l->count);

    free(l->samples);
}

static long long nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ================================================================
 *  网络层
 * ================================================================ */

static int tcpConnect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "gethostbyname: %s\n", host);
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* 完整写入（处理短写） */
static int writeAll(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n == -1) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("write");
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/*
 * 读取单行 RESP 响应。
 * 所有 benchmark 响应均为单行 (+OK\r\n, :N\r\n, $-1\r\n)，
 * 只有 GET integer 也返回 :N\r\n。
 * 读到 \r\n 即返回。
 */
static int readResponse(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r == 0) {
            fprintf(stderr, "Connection closed by server\n");
            return -1;
        }
        if (r == -1) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("read");
            return -1;
        }
        n += (size_t)r;
        buf[n] = '\0';
        if (n >= 2 && buf[n - 2] == '\r' && buf[n - 1] == '\n')
            return (int)n;
    }
    fprintf(stderr, "Response too large (cap=%zu)\n", cap);
    return -1;
}

/* ================================================================
 *  RESP 命令格式化
 *
 *  key 格式: k:%07d  (固定 9 字节)
 *  SET value 用 RESP 整数 :1\r\n (服务端走 VAL_INT 快速路径)
 * ================================================================ */

static int formatSet(char *buf, size_t cap, int keynum) {
    return snprintf(buf, cap,
        "*3\r\n$3\r\nSET\r\n$9\r\nk:%07d\r\n:1\r\n", keynum);
}

static int formatGet(char *buf, size_t cap, int keynum) {
    return snprintf(buf, cap,
        "*2\r\n$3\r\nGET\r\n$9\r\nk:%07d\r\n", keynum);
}

static int formatDel(char *buf, size_t cap, int keynum) {
    return snprintf(buf, cap,
        "*2\r\n$3\r\nDEL\r\n$9\r\nk:%07d\r\n", keynum);
}

/* 验证 RESP 响应首字节合法 */
static int isRespPrefix(char c) {
    return c == '+' || c == '-' || c == ':' || c == '$';
}

/* ================================================================
 *  各阶段函数
 * ================================================================ */

/*
 * 单次 round-trip: 写命令 + 读响应 + 计时。
 * 返回耗时 (ns)，出错返回 -1。
 */
static long long roundTrip(int fd, const char *cmdbuf, int cmdlen,
                           char *rspbuf, size_t rspcap) {
    long long t0 = nowNs();

    if (writeAll(fd, cmdbuf, (size_t)cmdlen) != 0)
        return -1;

    int rlen = readResponse(fd, rspbuf, rspcap);
    if (rlen < 0)
        return -1;

    if (rlen == 0 || !isRespPrefix(rspbuf[0])) {
        fprintf(stderr, "Bad response prefix: '%c' (rlen=%d)\n",
                rlen > 0 ? rspbuf[0] : '?', rlen);
        return -1;
    }

    return nowNs() - t0;
}

/* ---------- Populate ---------- */
static void doPopulate(int fd, int n, int keyStart, const char *label,
                       int progInterval) {
    printf("── %s: inserting %d keys (k:%07d..k:%07d) ──\n",
           label, n, keyStart, keyStart + n - 1);
    fflush(stdout);

    char cmdbuf[256], rspbuf[128];
    long long tStart = nowNs();
    long long tLast  = tStart;

    for (int i = 0; i < n; i++) {
        int keynum = keyStart + i;
        int cmdlen = formatSet(cmdbuf, sizeof(cmdbuf), keynum);

        long long dt = roundTrip(fd, cmdbuf, cmdlen, rspbuf, sizeof(rspbuf));
        if (dt < 0) {
            fprintf(stderr, "Error at POPULATE key %d\n", keynum);
            exit(1);
        }

        if ((i + 1) % progInterval == 0) {
            long long now = nowNs();
            double rate = (double)progInterval / ((now - tLast) / 1e9);
            long long elapsed = (now - tStart) / 1000000;
            printf("  %d/%d  (%.0f ops/s,  elapsed %lld ms)\n",
                   i + 1, n, rate, elapsed);
            fflush(stdout);
            tLast = now;
        }
    }

    long long totalNs = nowNs() - tStart;
    double rate = n / (totalNs / 1e9);
    printf("  Done.  %.0f SET/s  (%lld ms)\n\n",
           rate, totalNs / 1000000);
    fflush(stdout);
}

/* ---------- Delete ---------- */
static void doDelete(int fd, int n, int keyStart, const char *label,
                     int progInterval) {
    printf("── %s: deleting %d keys (k:%07d..k:%07d) ──\n",
           label, n, keyStart, keyStart + n - 1);
    fflush(stdout);

    char cmdbuf[256], rspbuf[128];
    long long tStart = nowNs();
    long long tLast  = tStart;

    for (int i = 0; i < n; i++) {
        int keynum = keyStart + i;
        int cmdlen = formatDel(cmdbuf, sizeof(cmdbuf), keynum);

        long long dt = roundTrip(fd, cmdbuf, cmdlen, rspbuf, sizeof(rspbuf));
        if (dt < 0) {
            fprintf(stderr, "Error at DELETE key %d\n", keynum);
            exit(1);
        }

        if ((i + 1) % progInterval == 0) {
            long long now = nowNs();
            double rate = (double)progInterval / ((now - tLast) / 1e9);
            long long elapsed = (now - tStart) / 1000000;
            printf("  %d/%d  (%.0f ops/s,  elapsed %lld ms)\n",
                   i + 1, n, rate, elapsed);
            fflush(stdout);
            tLast = now;
        }
    }

    long long totalNs = nowNs() - tStart;
    double rate = n / (totalNs / 1e9);
    printf("  Done.  %.0f DEL/s  (%lld ms)\n\n",
           rate, totalNs / 1000000);
    fflush(stdout);
}

/* ---------- GET benchmark ---------- */
static void doBenchGet(int fd, int n, int keyStart, int keyspace,
                       unsigned int seed, const char *label,
                       int warmup, int progInterval) {
    printf("── %s: GET benchmark (%d ops, keyspace=%d, keyStart=%d) ──\n",
           label, n, keyspace, keyStart);
    fflush(stdout);

    char cmdbuf[256], rspbuf[128];

    /* Warmup (不计时) */
    if (warmup > 0) {
        printf("  Warmup: %d iterations (untimed)\n", warmup);
        unsigned int ws = seed;
        for (int i = 0; i < warmup; i++) {
            int keynum = keyStart + (int)(lcgRand(&ws) % (unsigned int)keyspace);
            int cmdlen = formatGet(cmdbuf, sizeof(cmdbuf), keynum);
            if (roundTrip(fd, cmdbuf, cmdlen, rspbuf, sizeof(rspbuf)) < 0) {
                fprintf(stderr, "Error during warmup at iter %d\n", i);
                exit(1);
            }
        }
        printf("  Warmup complete.\n");
        fflush(stdout);
    }

    /* Timed benchmark */
    Latency lat     = latNew();
    long long tStart = nowNs();
    long long tLast  = tStart;
    unsigned int rs  = seed;

    for (int i = 0; i < n; i++) {
        int keynum = keyStart + (int)(lcgRand(&rs) % (unsigned int)keyspace);
        int cmdlen = formatGet(cmdbuf, sizeof(cmdbuf), keynum);

        long long dt = roundTrip(fd, cmdbuf, cmdlen, rspbuf, sizeof(rspbuf));
        if (dt < 0) {
            fprintf(stderr, "Error at GET iter %d (key %d)\n", i, keynum);
            exit(1);
        }
        latRecord(&lat, dt);

        if ((i + 1) % progInterval == 0) {
            long long now = nowNs();
            double rate = (double)progInterval / ((now - tLast) / 1e9);
            long long elapsed = (now - tStart) / 1000000;
            printf("  %d/%d  (%.0f ops/s,  elapsed %lld ms)\n",
                   i + 1, n, rate, elapsed);
            fflush(stdout);
            tLast = now;
        }
    }

    long long totalNs = nowNs() - tStart;
    double rate = n / (totalNs / 1e9);

    printf("\n  GET 吞吐量: %.0f ops/s  (%lld ms total)\n",
           rate, totalNs / 1000000);
    printf("  延迟分布 (网络 round-trip):\n");
    latReport(&lat, "GET");
    printf("\n");
    fflush(stdout);
}

/* ---------- SET overwrite benchmark ---------- */
static void doBenchSet(int fd, int n, int keyStart, int keyspace,
                       unsigned int seed, const char *label,
                       int warmup, int progInterval) {
    printf("── %s: SET overwrite benchmark (%d ops, keyspace=%d, keyStart=%d) ──\n",
           label, n, keyspace, keyStart);
    fflush(stdout);

    char cmdbuf[256], rspbuf[128];

    /* Warmup (不计时) */
    if (warmup > 0) {
        printf("  Warmup: %d iterations (untimed)\n", warmup);
        unsigned int ws = seed;
        for (int i = 0; i < warmup; i++) {
            int keynum = keyStart + (int)(lcgRand(&ws) % (unsigned int)keyspace);
            int cmdlen = formatSet(cmdbuf, sizeof(cmdbuf), keynum);
            if (roundTrip(fd, cmdbuf, cmdlen, rspbuf, sizeof(rspbuf)) < 0) {
                fprintf(stderr, "Error during warmup at iter %d\n", i);
                exit(1);
            }
        }
        printf("  Warmup complete.\n");
        fflush(stdout);
    }

    /* Timed benchmark */
    Latency lat      = latNew();
    long long tStart = nowNs();
    long long tLast  = tStart;
    unsigned int rs  = seed;

    for (int i = 0; i < n; i++) {
        int keynum = keyStart + (int)(lcgRand(&rs) % (unsigned int)keyspace);
        int cmdlen = formatSet(cmdbuf, sizeof(cmdbuf), keynum);

        long long dt = roundTrip(fd, cmdbuf, cmdlen, rspbuf, sizeof(rspbuf));
        if (dt < 0) {
            fprintf(stderr, "Error at SET iter %d (key %d)\n", i, keynum);
            exit(1);
        }
        latRecord(&lat, dt);

        if ((i + 1) % progInterval == 0) {
            long long now = nowNs();
            double rate = (double)progInterval / ((now - tLast) / 1e9);
            long long elapsed = (now - tStart) / 1000000;
            printf("  %d/%d  (%.0f ops/s,  elapsed %lld ms)\n",
                   i + 1, n, rate, elapsed);
            fflush(stdout);
            tLast = now;
        }
    }

    long long totalNs = nowNs() - tStart;
    double rate = n / (totalNs / 1e9);

    printf("\n  SET overwrite 吞吐量: %.0f ops/s  (%lld ms total)\n",
           rate, totalNs / 1000000);
    printf("  延迟分布 (网络 round-trip):\n");
    latReport(&lat, "SET");
    printf("\n");
    fflush(stdout);
}

/* ================================================================
 *  用法 & main
 * ================================================================ */

static void usage(const char *prog) {
    fprintf(stderr,
        "FlashKV Server Benchmark\n"
        "\n"
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --host HOST         Server host (default: 127.0.0.1)\n"
        "  --port PORT         Server port (default: 6379)\n"
        "  --populate N        Insert N keys sequentially\n"
        "  --delete N          Delete N keys sequentially (from key-start)\n"
        "  --get N             Benchmark N random GET operations\n"
        "  --set N             Benchmark N random SET overwrites\n"
        "  --keyspace K        Number of distinct keys for random sampling\n"
        "                      (default: same as --get/--set N)\n"
        "  --key-start S       Starting key number (default: 1)\n"
        "  --label STR         Label for this benchmark run\n"
        "  --seed S            Random seed (default: 42)\n"
        "  --warmup N          Untimed warmup iterations (default: 200000)\n"
        "  --prog-interval N   Progress report every N ops (default: 100000)\n"
        "\n"
        "Key naming: k:0000001, k:0000002, ... (fixed 9-byte keys)\n"
        "Only one of --populate/--delete/--get/--set is executed per invocation.\n"
        "\n"
        "Examples:\n"
        "  %s --populate 1000000 --label \"A-populate\"\n"
        "  %s --get 1000000 --keyspace 1000000 --key-start 1 --label \"A-GET\"\n"
        "  %s --delete 1000000 --key-start 1 --label \"B-delete\"\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    const char *host       = "127.0.0.1";
    int         port       = 6379;
    int         populate   = 0;
    int         deleteN    = 0;
    int         getN       = 0;
    int         setN       = 0;
    int         keyspace   = 0;
    int         keyStart   = 1;
    const char *label      = "default";
    int         seed       = 42;
    int         warmup     = 200000;
    int         progInterval = 100000;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--populate") && i + 1 < argc) {
            populate = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--delete") && i + 1 < argc) {
            deleteN = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--get") && i + 1 < argc) {
            getN = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            setN = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--keyspace") && i + 1 < argc) {
            keyspace = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--key-start") && i + 1 < argc) {
            keyStart = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--label") && i + 1 < argc) {
            label = argv[++i];
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) {
            warmup = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--prog-interval") && i + 1 < argc) {
            progInterval = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 默认 keyspace = get/set 的 N */
    int opN = populate ? populate : (deleteN ? deleteN : (getN ? getN : setN));
    if (keyspace == 0) keyspace = opN;

    /* 至少指定一个操作 */
    if (!populate && !deleteN && !getN && !setN) {
        fprintf(stderr, "Error: specify --populate, --delete, --get, or --set\n");
        usage(argv[0]);
        return 1;
    }

    /* 连接 */
    int fd = tcpConnect(host, port);
    if (fd == -1) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host, port);
        return 1;
    }

    printf("═══════════════════════════════════════════════════\n");
    printf("  FlashKV Server Benchmark: %s\n", label);
    printf("═══════════════════════════════════════════════════\n");
    printf("  Server: %s:%d   Seed: %d\n", host, port, seed);
    printf("  Key format: k:%%07d   key-start: %d\n", keyStart);
    printf("\n");
    fflush(stdout);

    if (populate) {
        doPopulate(fd, populate, keyStart, label, progInterval);
    } else if (deleteN) {
        doDelete(fd, deleteN, keyStart, label, progInterval);
    } else if (getN) {
        doBenchGet(fd, getN, keyStart, keyspace,
                   (unsigned int)seed, label, warmup, progInterval);
    } else if (setN) {
        doBenchSet(fd, setN, keyStart, keyspace,
                   (unsigned int)seed, label, warmup, progInterval);
    }

    close(fd);
    printf("═══════════════════════════════════════════════════\n");
    return 0;
}
