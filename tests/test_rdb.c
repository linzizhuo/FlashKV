#include "rdb.h"
#include "kvdb.h"
#include "sds.h"
#include "val_obj.h"
#include "zset.h"
#include "config.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int readn(int fd, void *buf, size_t n) {
    size_t t = 0;
    while (t < n) {
        ssize_t r = read(fd, (char *)buf + t, n - t);
        if (r <= 0) return -1;
        t += (size_t)r;
    }
    return 0;
}

int main(void) {
    const char *path = "/tmp/test_rdb.rdb";
    unlink(path);

    kvdb *kv = kvdbNew();
    assert(kv);

    sds k1 = sdsnew("k1"), k2 = sdsnew("k2"), k3 = sdsnew("k3");

    /* string */
    ValObj *vs = malloc(sizeof(*vs));
    vs->type = DATA_STRING; vs->val.str = sdsnew("hello");
    assert(kvdbSet(kv, k1, vs) == NULL);

    /* int + TTL */
    ValObj *vi = malloc(sizeof(*vi));
    vi->type = DATA_INT; vi->val.ll = 99;
    assert(kvdbSet(kv, k2, vi) == NULL);
    assert(kvdbExpire(kv, k2, 2000000000) == 1);

    /* zset: 2 members */
    ValObj *vz = malloc(sizeof(*vz));
    vz->type = DATA_ZSET; vz->val.zs = zsetNew();
    zsetAdd(vz->val.zs, 1.0, sdsnew("x"));
    zsetAdd(vz->val.zs, 2.0, sdsnew("y"));
    assert(kvdbSet(kv, k3, vz) == NULL);

    /* verify dict state */
    struct dict *dict = kvdbGetDict(kv);
    fprintf(stderr, "ht[0].used=%lu ht[1].used=%lu\n", dict->ht[0].used, dict->ht[1].used);

    assert(rdbSave(kv, path) == OK);
    kvdbFree(kv);

    /* verify file */
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);

    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    fprintf(stderr, "file size: %ld bytes\n", (long)sz);

    char buf[256], keybuf[64];
    uint32_t u32; uint64_t u64; uint8_t u8; double d;

    /* magic + version: 7 + 4 = 11 bytes */
    assert(readn(fd, buf, 7) == 0);
    assert(memcmp(buf, RDB_MAGIC, 7) == 0);
    assert(readn(fd, &u32, 4) == 0);
    assert(u32 == RDB_VERSION);

    /* dbcount */
    assert(readn(fd, &u32, 4) == 0);
    assert(u32 == 1);

    /* key_count */
    assert(readn(fd, &u32, 4) == 0);
    assert(u32 == 3);

    int found = 0;

    while (readn(fd, &u8, 1) == 0) {
        int has_expire = (u8 & RDB_HAS_EXPIRE) != 0;
        int dtype = u8 & RDB_TYPE_MASK;

        /* key */
        assert(readn(fd, &u32, 4) == 0);
        assert(u32 < sizeof(keybuf));
        assert(readn(fd, keybuf, u32) == 0);
        keybuf[u32] = '\0';
        fprintf(stderr, "entry: key='%s' type=%d expire=%d\n", keybuf, dtype, has_expire);

        if (strcmp(keybuf, "k1") == 0) {
            assert(!has_expire && dtype == DATA_STRING);
            assert(readn(fd, &u32, 4) == 0); assert(u32 == 5);
            assert(readn(fd, buf, 5) == 0); assert(memcmp(buf, "hello", 5) == 0);
            found |= 1;
        } else if (strcmp(keybuf, "k2") == 0) {
            assert(has_expire && dtype == DATA_INT);
            long long ll;
            assert(readn(fd, &ll, 8) == 0); assert(ll == 99);
            /* expire after val (wire format: type → key → val → expire) */
            assert(readn(fd, &u64, 8) == 0);
            assert(u64 == 2000000000ULL);
            found |= 2;
        } else if (strcmp(keybuf, "k3") == 0) {
            assert(!has_expire && dtype == DATA_ZSET);
            assert(readn(fd, &u32, 4) == 0); assert(u32 == 2);  /* count */
            /* node 0 */
            assert(readn(fd, &d, 8) == 0); assert(d == 1.0);
            assert(readn(fd, &u32, 4) == 0); assert(u32 == 1);
            assert(readn(fd, buf, 1) == 0); assert(*buf == 'x');
            /* node 1 */
            assert(readn(fd, &d, 8) == 0); assert(d == 2.0);
            assert(readn(fd, &u32, 4) == 0); assert(u32 == 1);
            assert(readn(fd, buf, 1) == 0); assert(*buf == 'y');
            found |= 4;
        } else {
            fprintf(stderr, "UNEXPECTED key='%s'\n", keybuf);
            assert(0);
        }
    }

    close(fd);
    assert(found == 7); /* 1|2|4 = all 3 found */
    printf("PASS: test_rdb\n");
    unlink(path);
    return 0;
}
