#ifndef SCHEDFS_VIRTUAL_DISK_H
#define SCHEDFS_VIRTUAL_DISK_H
#include <pthread.h>
#include <stdint.h>
#include "metrics.h"
#include "request_queue.h"
typedef struct {
    int fd;
    uint32_t blocks;
    request_queue_t queue;
    scheduler_t scheduler;
    sfs_metrics_t metrics;
    pthread_t thread;
    int running;
} virtual_disk_t;
int vdisk_open(virtual_disk_t *d, const char *path, uint32_t blocks,
               sched_policy_t policy, double alpha, double beta, size_t queue_capacity);
int vdisk_read(virtual_disk_t *d, uint32_t block, void *buffer);
int vdisk_write(virtual_disk_t *d, uint32_t block, const void *buffer);
void vdisk_close(virtual_disk_t *d);
#endif
