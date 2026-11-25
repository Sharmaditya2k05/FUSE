/*
 * request.h
 *
 * A single block I/O request and its per-request completion condition.
 */
#ifndef SCHEDFS_REQUEST_H
#define SCHEDFS_REQUEST_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#ifdef _WIN32
typedef void *pthread_t;

typedef struct {
    uintptr_t opaque[8];
} pthread_mutex_t;

typedef struct {
    uintptr_t opaque[8];
} pthread_cond_t;
#else
#include <pthread.h>
#endif
typedef enum { RQ_READ = 0, RQ_WRITE = 1 } rq_op_t;

typedef struct disk_request {
    uint64_t id;
    rq_op_t op;
    uint32_t block;
    void *buffer;
    struct timespec arrival, service_start, completion;
    /* Measurement only (never used for scheduling): the process that issued the
     * file operation (FUSE caller pid, 0 inside the daemon) and the number of
     * requests that were pending when the scheduler picked this one. */
    uint32_t client;
    uint32_t queue_depth;
    pthread_t producer;
    pthread_mutex_t mutex;
    pthread_cond_t completed_cv;
    bool completed;
    ssize_t result;
    int error;
} disk_request_t;

/* Caller identity recorded in new requests; set per thread by the FUSE adapter. */
extern _Thread_local uint32_t sfs_request_client;

int request_init(disk_request_t *r, rq_op_t op, uint32_t block, void *buffer);
void request_destroy(disk_request_t *r);
int request_wait(disk_request_t *r);
#endif
