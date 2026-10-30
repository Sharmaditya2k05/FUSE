#include "request_queue.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_uint_fast64_t next_id = 1;

int request_init(disk_request_t *r, rq_op_t op, uint32_t block, void *buffer) {
    memset(r, 0, sizeof(*r));
    r->id = atomic_fetch_add(&next_id, 1);
    r->op = op; r->block = block; r->buffer = buffer; r->producer = pthread_self();
    clock_gettime(CLOCK_MONOTONIC, &r->arrival);
    int e = pthread_mutex_init(&r->mutex, NULL);
    if (e) return -e;
    e = pthread_cond_init(&r->completed_cv, NULL);
    if (e) { pthread_mutex_destroy(&r->mutex); return -e; }
    return 0;
}

void request_destroy(disk_request_t *r) {
    pthread_cond_destroy(&r->completed_cv);
    pthread_mutex_destroy(&r->mutex);
}

int request_wait(disk_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    while (!r->completed) pthread_cond_wait(&r->completed_cv, &r->mutex);
    int result = r->error ? -r->error : (r->result < 0 ? -EIO : 0);
    pthread_mutex_unlock(&r->mutex);
    return result;
}

int rq_init(request_queue_t *q, size_t capacity) {
    memset(q, 0, sizeof(*q));
    if (!capacity) return -EINVAL;
    q->items = calloc(capacity, sizeof(*q->items));
    if (!q->items) return -ENOMEM;
    q->capacity = capacity;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return 0;
}

int rq_submit(request_queue_t *q, disk_request_t *r) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == q->capacity && !q->stopping) pthread_cond_wait(&q->not_full, &q->mutex);
    if (q->stopping) { pthread_mutex_unlock(&q->mutex); return -ESHUTDOWN; }
    q->items[q->count++] = r;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

disk_request_t *rq_take(request_queue_t *q, const scheduler_t *scheduler) {
    pthread_mutex_lock(&q->mutex);
    while (!q->count && !q->stopping) pthread_cond_wait(&q->not_empty, &q->mutex);
    if (!q->count) { pthread_mutex_unlock(&q->mutex); return NULL; }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    size_t index = scheduler_pick(scheduler, q->items, q->count, now);
    disk_request_t *r = q->items[index];
    memmove(&q->items[index], &q->items[index + 1],
            (q->count - index - 1) * sizeof(*q->items));
    --q->count;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return r;
}

void rq_stop(request_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->stopping = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}

void rq_destroy(request_queue_t *q) {
    free(q->items);
    pthread_cond_destroy(&q->not_empty); pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->mutex);
}
