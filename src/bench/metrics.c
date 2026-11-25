/*
 * metrics.c
 *
 * Metric accumulation for the mounted filesystem's dispatcher.
 */
#include "metrics.h"
#include <string.h>

void metrics_init(sfs_metrics_t *m) {
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->mutex, NULL);
    clock_gettime(CLOCK_MONOTONIC, &m->started);
}

void metrics_record(sfs_metrics_t *m, uint64_t move, double wait_ms, uint32_t depth) {
    pthread_mutex_lock(&m->mutex);
    m->requests++;
    m->total_seek += move;
    m->depth_sum += depth;
    if (depth > m->depth_max)
        m->depth_max = depth;
    m->wait_sum_ms += wait_ms;
    m->wait_sq_sum_ms += wait_ms * wait_ms;
    if (wait_ms > m->wait_max_ms)
        m->wait_max_ms = wait_ms;
    pthread_mutex_unlock(&m->mutex);
}

void metrics_finish(sfs_metrics_t *m) {
    pthread_mutex_lock(&m->mutex);
    clock_gettime(CLOCK_MONOTONIC, &m->ended);
    pthread_mutex_unlock(&m->mutex);
}
