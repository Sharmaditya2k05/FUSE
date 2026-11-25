/*
 * sim.c
 *
 * Discrete-event replay of timed block requests through the real scheduler.
 */
#include "sim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct timespec ts(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)};
    return t;
}

int sim_run(const scheduler_t *start, const sim_arrival_t *a, size_t n, const sim_model_t *m,
            size_t *order, double *wait_ms, sim_result_t *res) {
    memset(res, 0, sizeof(*res));
    if (!n)
        return 0;
    scheduler_t s = *start;
    disk_request_t *req = calloc(n, sizeof(*req));
    disk_request_t **pending = calloc(n, sizeof(*pending));
    if (!req || !pending) {
        free(req);
        free(pending);
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        req[i].id = i + 1;
        req[i].block = a[i].block;
        req[i].op = a[i].op;
        req[i].arrival = ts(a[i].arrival_ns);
    }
    size_t cap = m->queue_capacity ? m->queue_capacity : n;
    size_t next = 0, count = 0;
    uint64_t t = 0;
    double sum = 0, sq = 0;
    while (res->served < n) {
        /* admit everything that has arrived, as long as the queue has space */
        while (next < n && a[next].arrival_ns <= t && count < cap)
            pending[count++] = &req[next++];
        if (!count) {
            t = a[next].arrival_ns; /* idle until the next arrival */
            continue;
        }
        if (count > res->max_depth)
            res->max_depth = count;
        size_t k = scheduler_pick(&s, pending, count, ts(t));
        disk_request_t *r = pending[k];
        memmove(&pending[k], &pending[k + 1], (count - k - 1) * sizeof(*pending));
        --count;
        size_t idx = (size_t)(r->id - 1);
        double w = (double)(t - a[idx].arrival_ns) / 1e6;
        uint64_t move = scheduler_advance(&s, r->block);
        res->movement += move;
        sum += w;
        sq += w * w;
        if (w > res->wait_max_ms)
            res->wait_max_ms = w;
        if (order)
            order[res->served] = idx;
        if (wait_ms)
            wait_ms[idx] = w;
        res->served++;
        t += (uint64_t)llround((m->base_us + m->per_block_us * (double)move) * 1000.0);
    }
    res->makespan_ns = t;
    res->wait_mean_ms = sum / (double)n;
    double var = sq / (double)n - res->wait_mean_ms * res->wait_mean_ms;
    res->wait_stddev_ms = var > 0 ? sqrt(var) : 0;
    free(req);
    free(pending);
    return 0;
}
