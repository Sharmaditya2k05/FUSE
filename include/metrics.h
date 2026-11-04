/*
 * metrics.h
 *
 * Seek-distance and waiting-time counters collected by the dispatcher.
 */
#ifndef SCHEDFS_METRICS_H
#define SCHEDFS_METRICS_H
#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    uint64_t requests, total_seek;
    double wait_sum_ms, wait_sq_sum_ms, wait_max_ms;
    struct timespec started, ended;
} sfs_metrics_t;

void metrics_init(sfs_metrics_t *m);
void metrics_record(sfs_metrics_t *m, uint32_t from, uint32_t to, double wait_ms);
void metrics_finish(sfs_metrics_t *m);
#endif
