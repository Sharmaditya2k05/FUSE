#include "metrics.h"
#include <math.h>
#include <string.h>

void metrics_init(sfs_metrics_t *m) {
    memset(m, 0, sizeof(*m)); pthread_mutex_init(&m->mutex, NULL);
    clock_gettime(CLOCK_MONOTONIC, &m->started);
}
void metrics_record(sfs_metrics_t *m, uint32_t from, uint32_t to, double wait_ms) {
    pthread_mutex_lock(&m->mutex);
    m->requests++; m->total_seek += from > to ? from - to : to - from;
    m->wait_sum_ms += wait_ms; m->wait_sq_sum_ms += wait_ms * wait_ms;
    if (wait_ms > m->wait_max_ms) m->wait_max_ms = wait_ms;
    pthread_mutex_unlock(&m->mutex);
}
void metrics_finish(sfs_metrics_t *m) { clock_gettime(CLOCK_MONOTONIC, &m->ended); }
