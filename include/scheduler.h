#ifndef SCHEDFS_SCHEDULER_H
#define SCHEDFS_SCHEDULER_H
#include <stddef.h>
#include <stdint.h>
#include "request.h"
typedef enum { SCHED_FCFS, SCHED_SSTF, SCHED_SCAN, SCHED_ADAPTIVE } sched_policy_t;
typedef struct {
    sched_policy_t policy;
    uint32_t total_blocks;
    uint32_t head;
    int scan_direction;
    double alpha, beta;
} scheduler_t;
int scheduler_parse(const char *name, sched_policy_t *out);
const char *scheduler_name(sched_policy_t p);
size_t scheduler_pick(const scheduler_t *s, disk_request_t *const *items, size_t count,
                      struct timespec now);
#endif
