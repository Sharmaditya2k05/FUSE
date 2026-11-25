/*
 * sim.h
 *
 * Deterministic discrete-event simulation of the scheduling path with a simulated
 * clock. Requests arrive at given times, wait in a bounded queue, and are picked by
 * the real scheduler_pick(); each service lasts base + per_block x movement, where
 * movement comes from the real scheduler_advance(). Nothing reads the wall clock, so
 * every policy, including the time-dependent ones, gives the same result every run.
 *
 * This is a model of a disk, not a measurement of one. Mounted experiments
 * (scripts/run_mounted.sh) measure the real file system.
 */
#ifndef SCHEDFS_SIM_H
#define SCHEDFS_SIM_H
#include <stddef.h>
#include <stdint.h>
#include "scheduler.h"

typedef struct {
    uint64_t arrival_ns; /* simulated arrival time; the array must be sorted by it */
    uint32_t block;
    rq_op_t op;
} sim_arrival_t;

typedef struct {
    double base_us;      /* service time per request ... */
    double per_block_us; /* ... plus this much per block of modeled movement */
    size_t queue_capacity; /* 0 = unbounded; otherwise arrivals wait for space */
} sim_model_t;

typedef struct {
    size_t served;
    uint64_t movement;      /* modeled head movement in blocks */
    uint64_t makespan_ns;   /* simulated time when the last request finished */
    double wait_mean_ms, wait_max_ms, wait_stddev_ms; /* arrival -> service start */
    size_t max_depth;
} sim_result_t;

/* Runs the simulation from scheduler state *start (copied, not modified).
 * order (optional, n entries) receives arrival indices in service order;
 * wait_ms (optional, n entries) receives each request's wait, indexed by arrival.
 * Returns 0, or -1 if memory runs out. */
int sim_run(const scheduler_t *start, const sim_arrival_t *a, size_t n, const sim_model_t *m,
            size_t *order, double *wait_ms, sim_result_t *res);
#endif
