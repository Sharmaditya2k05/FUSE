/*
 * metrics.h
 *
 * Counters collected by the dispatcher: requests, modeled head movement, queue
 * waiting time (arrival to service start) and queue depth at each pick.
 */
#ifndef SCHEDFS_METRICS_H
#define SCHEDFS_METRICS_H
#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    uint64_t requests, total_seek, depth_sum, depth_max;
    double wait_sum_ms, wait_sq_sum_ms, wait_max_ms;
    struct timespec started, ended;
} sfs_metrics_t;

void metrics_init(sfs_metrics_t *m);
void metrics_record(sfs_metrics_t *m, uint64_t move, double wait_ms, uint32_t depth);
void metrics_finish(sfs_metrics_t *m);
#endif
