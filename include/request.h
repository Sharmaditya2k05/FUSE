#ifndef SCHEDFS_REQUEST_H
#define SCHEDFS_REQUEST_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#ifdef _WIN32
typedef void *pthread_t;
typedef struct { uintptr_t opaque[8]; } pthread_mutex_t;
typedef struct { uintptr_t opaque[8]; } pthread_cond_t;
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
    pthread_t producer;
    pthread_mutex_t mutex;
    pthread_cond_t completed_cv;
    bool completed;
    ssize_t result;
    int error;
} disk_request_t;
int request_init(disk_request_t *r, rq_op_t op, uint32_t block, void *buffer);
void request_destroy(disk_request_t *r);
int request_wait(disk_request_t *r);
#endif
