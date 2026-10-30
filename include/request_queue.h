#ifndef SCHEDFS_REQUEST_QUEUE_H
#define SCHEDFS_REQUEST_QUEUE_H
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include "request.h"
#include "scheduler.h"
typedef struct {
    disk_request_t **items;
    size_t count, capacity;
    bool stopping;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} request_queue_t;
int rq_init(request_queue_t *q, size_t capacity);
int rq_submit(request_queue_t *q, disk_request_t *r);
disk_request_t *rq_take(request_queue_t *q, const scheduler_t *scheduler);
void rq_stop(request_queue_t *q);
void rq_destroy(request_queue_t *q);
#endif
