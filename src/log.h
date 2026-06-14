#ifndef _LOG_H
#define _LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

enum LogLevel {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_SILENT   /* 关日志 */
};

#ifndef LOG_DEFAULT_LEVEL
#define LOG_DEFAULT_LEVEL LOG_DEBUG
#endif

extern int g_log_level;

/* 设置日志级别 */
static inline void logSetLevel(int level) {
    if (level < LOG_DEBUG) level = LOG_DEBUG;
    if (level > LOG_SILENT) level = LOG_SILENT;
    g_log_level = level;
}

/* 核心写函数（裸用不推荐，建议用宏） */
static inline void logWrite(int level, const char *fmt, ...) {
    if (level < g_log_level) return;

    const char *labels[] = {
        [LOG_DEBUG] = "DEBUG",
        [LOG_INFO]  = "INFO",
        [LOG_WARN]  = "WARN",
        [LOG_ERROR] = "ERROR",
    };

    FILE *fp = (level == LOG_ERROR) ? stderr : stdout;

    fprintf(fp, "[%s] ", labels[level]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fflush(fp);
}

/*
 * 宏接口 —— 使用时可以像 printf 一样传参
 *
 *   LOG_INFO("server started on port %d", 6379);
 *   LOG_ERROR("accept failed: %s", strerror(errno));
 *
 * 编译时关日志：CFLAGS += -DLOG_DEFAULT_LEVEL=LOG_SILENT
 */
#define LOG_DEBUG(fmt, ...) \
    do { if (LOG_DEBUG >= g_log_level) \
        logWrite(LOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)

#define LOG_INFO(fmt, ...) \
    do { if (LOG_INFO >= g_log_level) \
        logWrite(LOG_INFO,  fmt, ##__VA_ARGS__); } while (0)

#define LOG_WARN(fmt, ...) \
    do { if (LOG_WARN >= g_log_level) \
        logWrite(LOG_WARN,  fmt, ##__VA_ARGS__); } while (0)

#define LOG_ERROR(fmt, ...) \
    do { logWrite(LOG_ERROR, fmt, ##__VA_ARGS__); } while (0)

#endif /* _LOG_H */
