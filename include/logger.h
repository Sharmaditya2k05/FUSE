/*
 * logger.h
 *
 * Thread-safe leveled logger writing to filesystem.log.
 */
#ifndef SCHEDFS_LOGGER_H
#define SCHEDFS_LOGGER_H
#include <stdbool.h>

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR } log_level_t;

int logger_open(const char *path, bool verbose);
void logger_close(void);
void sfs_log(log_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
