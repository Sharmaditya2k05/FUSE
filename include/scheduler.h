/*
 * scheduler.h
 *
 * Disk scheduling policies and the modeled head movement they produce.
 *
 *   FCFS      oldest request first.
 *   SSTF      nearest block to the head first.
 *   SCAN      elevator: nearest block in the current direction; when none is left the
 *             head travels to the disk edge (block 0 or N-1) and reverses. Movement to
 *             the edge is charged.
 *   LOOK      same selection as SCAN, but the head reverses at the last request, so only
 *             the distance between served blocks is charged.
 *   ADAPTIVE  heuristic baseline: lowest alpha*distance/N - beta*wait/max_wait. It has no
 *             starvation guarantee (see docs/scheduling.md).
 *   DEADLINE  deadline-aware locality: every request gets a service-start deadline,
 *             arrival + read_deadline (reads) or arrival + write_deadline (writes).
 *             If any pending request is overdue, the earliest deadline is served
 *             (ties by arrival, id); otherwise the nearest block is served (SSTF).
 *             Deadlines of 0 make it FCFS; very long deadlines make it SSTF.
 *             This follows the established deadline I/O scheduler idea (separate read
 *             and write expiry times); it is not a real-time guarantee: the dispatcher
 *             is non-preemptive and a deadline only says when a request becomes urgent.
 *
 * Every tie is broken by arrival time, then request id, so a given input always gives
 * the same order. scheduler_pick() takes the current time as an argument, so tests can
 * drive it with a simulated clock.
 */
#ifndef SCHEDFS_SCHEDULER_H
#define SCHEDFS_SCHEDULER_H
#include <stddef.h>
#include <stdint.h>
#include "request.h"

typedef enum {
    SCHED_FCFS,
    SCHED_SSTF,
    SCHED_SCAN,
    SCHED_LOOK,
    SCHED_ADAPTIVE,
    SCHED_DEADLINE_LOCAL,
    SCHED_POLICY_COUNT
} sched_policy_t;

typedef struct {
    sched_policy_t policy;
    uint32_t total_blocks;
    uint32_t head;
    int scan_direction; /* +1 toward higher blocks, -1 toward block 0 (SCAN, LOOK) */
    double alpha, beta; /* ADAPTIVE weights */
    uint64_t deadline_read_ns;  /* DEADLINE: service-start deadline of a read */
    uint64_t deadline_write_ns; /* DEADLINE: service-start deadline of a write */
} scheduler_t;

int scheduler_parse(const char *name, sched_policy_t *out);
const char *scheduler_name(sched_policy_t p);
/* Head at block 0 moving up, alpha 0.7, beta 0.3, deadlines 0 ms (read), 2 ms (write). */
void scheduler_init(scheduler_t *s, sched_policy_t p, uint32_t total_blocks);
/* Index of the request to serve next; count must be at least 1. */
size_t scheduler_pick(const scheduler_t *s, disk_request_t *const *items, size_t count,
                      struct timespec now);
/* Move the head to `next` and return the modeled movement in blocks. This is the one
 * definition of movement used by the dispatcher, the simulator and the tests. */
uint64_t scheduler_advance(scheduler_t *s, uint32_t next);
/* Absolute service-start deadline of a request, in ns of its arrival clock. */
int64_t scheduler_deadline(const scheduler_t *s, const disk_request_t *r);
/* True if the request's service-start deadline has passed at `now`. */
int scheduler_overdue(const scheduler_t *s, const disk_request_t *r, struct timespec now);
#endif
