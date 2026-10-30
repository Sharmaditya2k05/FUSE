#include "logger.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

static FILE *log_file;
static bool log_verbose;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

int logger_open(const char *path, bool verbose) {
    log_verbose = verbose;
    log_file = fopen(path, "a");
    return log_file ? 0 : -1;
}

void logger_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_file) fclose(log_file);
    log_file = NULL;
    pthread_mutex_unlock(&log_mutex);
}

void sfs_log(log_level_t level, const char *fmt, ...) {
    static const char *names[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
    if (level == LOG_DEBUG && !log_verbose) return;
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    pthread_mutex_lock(&log_mutex);
    FILE *out = log_file ? log_file : stderr;
    fprintf(out, "%04d-%02d-%02dT%02d:%02d:%02d.%03ld [%s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
            tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L, names[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
    pthread_mutex_unlock(&log_mutex);
}
