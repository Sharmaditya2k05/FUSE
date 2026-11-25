/*
 * test_scheduler.c
 *
 * Deterministic scheduler tests. Time is simulated (sim.c or explicit timespecs), so
 * every result is exact and repeatable. Covers the textbook trace for every policy,
 * SCAN versus LOOK movement, deadline activation and its limits, continuous nearby
 * arrivals while distant work waits, read/write deadlines, tie-breaking, the Adaptive
 * starvation bound, the movement-versus-wait trade-off, and queue saturation/shutdown.
 */
#include "request_queue.h"
#include "scheduler.h"
#include "sim.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS 1000000ULL
#define US 1000ULL

static _Atomic int checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static scheduler_t sched(sched_policy_t p, uint32_t blocks, uint32_t head, int dir) {
    scheduler_t s;
    scheduler_init(&s, p, blocks);
    s.head = head;
    s.scan_direction = dir;
    s.deadline_read_ns = 5 * MS; /* the tests use 5 ms / 50 ms so overdue moments are visible */
    s.deadline_write_ns = 50 * MS;
    return s;
}

/* ---- textbook trace: head 53 moving toward 0, 200 blocks ------------------------ */
static const uint32_t book[] = {98, 183, 37, 122, 14, 124, 65, 67};

static uint64_t textbook(scheduler_t s, const uint32_t *expected) {
    sim_arrival_t a[8];
    for (int i = 0; i < 8; ++i)
        a[i] = (sim_arrival_t){0, book[i], RQ_READ}; /* all pending at once, id = order */
    sim_model_t m = {0, 0, 0};
    size_t order[8];
    sim_result_t r;
    CHECK(sim_run(&s, a, 8, &m, order, NULL, &r) == 0);
    for (int i = 0; i < 8; ++i)
        CHECK(book[order[i]] == expected[i]);
    return r.movement;
}

static void test_textbook(void) {
    const uint32_t fcfs[] = {98, 183, 37, 122, 14, 124, 65, 67};
    const uint32_t near[] = {65, 67, 37, 14, 98, 122, 124, 183};
    const uint32_t sweep[] = {37, 14, 65, 67, 98, 122, 124, 183};
    CHECK(textbook(sched(SCHED_FCFS, 200, 53, -1), fcfs) == 640);
    CHECK(textbook(sched(SCHED_SSTF, 200, 53, -1), near) == 236);
    /* SCAN runs down to block 0 before turning: 53 + 183 = 236. */
    CHECK(textbook(sched(SCHED_SCAN, 200, 53, -1), sweep) == 236);
    /* LOOK turns at the last request (14): 39 + 169 = 208. */
    CHECK(textbook(sched(SCHED_LOOK, 200, 53, -1), sweep) == 208);
    /* Deadline with very long deadlines is SSTF; with zero deadlines it is FCFS. */
    scheduler_t lazy = sched(SCHED_DEADLINE_LOCAL, 200, 53, -1);
    lazy.deadline_read_ns = lazy.deadline_write_ns = 1000000 * MS;
    CHECK(textbook(lazy, near) == 236);
    scheduler_t eager = sched(SCHED_DEADLINE_LOCAL, 200, 53, -1);
    eager.deadline_read_ns = eager.deadline_write_ns = 0;
    CHECK(textbook(eager, fcfs) == 640);
}

/* ---- the movement model on its own ----------------------------------------------- */
static void test_advance(void) {
    scheduler_t s = sched(SCHED_SCAN, 100, 40, 1);
    CHECK(scheduler_advance(&s, 60) == 20 && s.scan_direction == 1);
    CHECK(scheduler_advance(&s, 10) == (99 - 60) + (99 - 10) && s.scan_direction == -1);
    CHECK(scheduler_advance(&s, 5) == 5);
    CHECK(scheduler_advance(&s, 30) == 5 + 30 && s.scan_direction == 1);
    scheduler_t l = sched(SCHED_LOOK, 100, 40, 1);
    CHECK(scheduler_advance(&l, 60) == 20);
    CHECK(scheduler_advance(&l, 10) == 50 && l.scan_direction == -1);
    CHECK(scheduler_advance(&l, 30) == 20 && l.scan_direction == 1);
    scheduler_t f = sched(SCHED_FCFS, 100, 40, 1);
    CHECK(scheduler_advance(&f, 10) == 30 && f.scan_direction == 1);
}

/* ---- continuous nearby arrivals while one distant request waits ----------------- */
/* Head at 1000 of 4096. A nearby request arrives every 100 us (2000 of them) and each
 * service takes 100 us, so fresh nearby work is always waiting. One far request
 * (block `far`) arrives at 50 us, while the first nearby request is being served.
 * Returns the far request's wait in ms and its position in the service order. */
static double far_wait(scheduler_t s, uint32_t far, rq_op_t far_op, rq_op_t near_op,
                       size_t *far_position) {
    enum { N = 2001 };
    static sim_arrival_t a[N];
    a[0] = (sim_arrival_t){0, 1000u, near_op};
    a[1] = (sim_arrival_t){50 * US, far, far_op};
    for (int i = 2; i < N; ++i)
        a[i] = (sim_arrival_t){(uint64_t)(i - 1) * 100 * US, 1000u + (uint32_t)(i % 8), near_op};
    sim_model_t m = {100, 0, 0};
    static size_t order[N];
    static double wait[N];
    sim_result_t r;
    CHECK(sim_run(&s, a, N, &m, order, wait, &r) == 0);
    for (size_t k = 0; k < N; ++k)
        if (order[k] == 1)
            *far_position = k;
    return wait[1];
}

static void test_starvation(void) {
    size_t pos;
    /* FCFS serves it next. */
    CHECK(far_wait(sched(SCHED_FCFS, 4096, 1000, 1), 4000, RQ_READ, RQ_READ, &pos) < 0.1 &&
          pos == 1);
    /* SSTF keeps choosing nearby work: the far request waits for the whole stream. */
    CHECK(far_wait(sched(SCHED_SSTF, 4096, 1000, 1), 4000, RQ_READ, RQ_READ, &pos) > 190.0);
    CHECK(pos == 2000);
    /* Deadline (read deadline 5 ms): served at the first decision after it becomes
     * overdue (decisions happen every 100 us here), despite fresh nearby arrivals. */
    double w = far_wait(sched(SCHED_DEADLINE_LOCAL, 4096, 1000, 1), 4000, RQ_READ, RQ_READ, &pos);
    CHECK(w >= 5.0 && w <= 5.1);
    /* Adaptive: the age term is normalized by the oldest wait, so its weight never
     * exceeds beta. A request farther than (beta/alpha) x N = 0.43 x 4096 = 1755 blocks
     * beyond the nearby work is never chosen while nearby work keeps arriving. */
    CHECK(far_wait(sched(SCHED_ADAPTIVE, 4096, 1000, 1), 4000, RQ_READ, RQ_READ, &pos) > 190.0);
    CHECK(pos == 2000);
    /* ... while a request inside that bound (1200 blocks away) is chosen at once. */
    CHECK(far_wait(sched(SCHED_ADAPTIVE, 4096, 1000, 1), 2200, RQ_READ, RQ_READ, &pos) < 0.1);
    /* Separate deadlines: a far write (50 ms) amid nearby reads is served at ~50 ms. */
    w = far_wait(sched(SCHED_DEADLINE_LOCAL, 4096, 1000, 1), 4000, RQ_WRITE, RQ_READ, &pos);
    CHECK(w >= 50.0 && w <= 50.1);
}

/* Adaptive over 10,000 decisions with replenished nearby work (the review's check). */
static void test_adaptive_bound(void) {
    scheduler_t s = sched(SCHED_ADAPTIVE, 4096, 1000, 1);
    disk_request_t far = {0}, near = {0};
    far.id = 1;
    far.block = 4000;
    near.id = 2;
    near.block = 1001;
    int far_picked = 0;
    for (int i = 1; i <= 10000; ++i) {
        struct timespec now = {i / 1000, (long)(i % 1000) * 1000000L};
        near.arrival = now; /* a fresh nearby request every decision */
        disk_request_t *items[2] = {&far, &near};
        far_picked += scheduler_pick(&s, items, 2, now) == 0;
    }
    CHECK(far_picked == 0);
}

/* ---- read and write deadlines, tie-breaking --------------------------------------- */
static void test_deadline_order(void) {
    scheduler_t s = sched(SCHED_DEADLINE_LOCAL, 1000, 0, 1);
    s.deadline_read_ns = 5 * MS;
    s.deadline_write_ns = 50 * MS;
    disk_request_t w = {0}, r1 = {0}, r2 = {0}, nearby = {0};
    w.id = 1, w.op = RQ_WRITE, w.block = 900, w.arrival = (struct timespec){0, 0};
    r1.id = 2, r1.op = RQ_READ, r1.block = 800, r1.arrival = (struct timespec){0, 46 * (long)MS};
    r2.id = 3, r2.op = RQ_READ, r2.block = 700, r2.arrival = (struct timespec){0, 40 * (long)MS};
    nearby.id = 4, nearby.op = RQ_READ, nearby.block = 1, nearby.arrival = (struct timespec){0, 59 * (long)MS};
    struct timespec now = {0, 60 * (long)MS};
    /* W (deadline 50 ms) beats R1 (deadline 51 ms): earliest deadline, not op type. */
    disk_request_t *a[3] = {&nearby, &r1, &w};
    CHECK(scheduler_pick(&s, a, 3, now) == 2);
    /* R2 (deadline 45 ms) beats W (deadline 50 ms) although W arrived first. */
    disk_request_t *b[3] = {&w, &nearby, &r2};
    CHECK(scheduler_pick(&s, b, 3, now) == 2);
    /* Nothing overdue: nearest block wins. */
    struct timespec early = {0, 0};
    disk_request_t *c[2] = {&w, &nearby};
    nearby.arrival = early;
    CHECK(scheduler_pick(&s, c, 2, early) == 1);
    /* Ties: equal distance and equal arrival -> lower id; equal distance -> older. */
    scheduler_t t = sched(SCHED_SSTF, 200, 53, 1);
    disk_request_t x = {0}, y = {0};
    x.id = 7, x.block = 50;
    y.id = 3, y.block = 56;
    disk_request_t *d[2] = {&x, &y};
    CHECK(scheduler_pick(&t, d, 2, early) == 1);
    x.arrival = (struct timespec){0, 1};
    y.arrival = (struct timespec){0, 2};
    CHECK(scheduler_pick(&t, d, 2, early) == 0);
    /* Two overdue requests with the same deadline -> older, then lower id. */
    s.deadline_read_ns = s.deadline_write_ns = 0;
    x.arrival = y.arrival = early;
    CHECK(scheduler_pick(&s, d, 2, now) == 1);
}

/* ---- movement versus waiting time ---------------------------------------------------
 * A saturated nearby stream (one request per 100 us at blocks 2000..2063, each service
 * 100 us) and a distant read every 20 ms (blocks 100..). Service time does not depend on
 * movement here, so every policy carries the same load and only the order differs. */
static void tradeoff_run(scheduler_t s, sim_result_t *r, double *far_max) {
    enum { N = 4000 };
    static sim_arrival_t a[N];
    static double wait[N];
    static int is_far[N];
    int n = 0;
    for (int i = 0; n < N; ++i) {
        uint64_t t = (uint64_t)i * 100 * US;
        if (i % 200 == 50) { /* every 20 ms, 5 ms into the period */
            is_far[n] = 1;
            a[n++] = (sim_arrival_t){t, 100u + (uint32_t)(i / 200) * 7u, RQ_READ};
        }
        if (n < N) {
            is_far[n] = 0;
            a[n++] = (sim_arrival_t){t, 2000u + (uint32_t)(i % 64), RQ_READ};
        }
    }
    sim_model_t m = {100, 0, 128};
    CHECK(sim_run(&s, a, N, &m, NULL, wait, r) == 0 && r->served == N);
    *far_max = 0;
    for (int i = 0; i < N; ++i)
        if (is_far[i] && wait[i] > *far_max)
            *far_max = wait[i];
}

static void test_tradeoff(void) {
    sim_result_t f, s, d, empty;
    double ff, sf, df;
    scheduler_t dl = sched(SCHED_DEADLINE_LOCAL, 4096, 2000, 1);
    CHECK(sim_run(&dl, NULL, 0, &(sim_model_t){0, 0, 0}, NULL, NULL, &empty) == 0);
    CHECK(empty.served == 0);
    tradeoff_run(sched(SCHED_FCFS, 4096, 2000, 1), &f, &ff);
    tradeoff_run(sched(SCHED_SSTF, 4096, 2000, 1), &s, &sf);
    tradeoff_run(dl, &d, &df);
    CHECK(s.movement < f.movement); /* SSTF saves movement ... */
    CHECK(sf > 100.0);              /* ... by leaving distant reads waiting a long time */
    CHECK(ff < 5.0);                /* FCFS serves them behind a short backlog */
    CHECK(df >= 5.0 && df <= 5.3);  /* deadline: about the 5 ms read deadline */
    CHECK(d.movement <= f.movement); /* and moves no more than FCFS */
    /* Same input, same output: the simulation is deterministic. */
    sim_result_t again;
    double af;
    tradeoff_run(dl, &again, &af);
    CHECK(again.movement == d.movement && af == df);
    /* Bounded queue: never more than the capacity pending. */
    sim_model_t small = {100, 0.5, 4};
    sim_arrival_t burst[20];
    for (int i = 0; i < 20; ++i)
        burst[i] = (sim_arrival_t){0, (uint32_t)i * 100u, RQ_READ};
    scheduler_t ss = sched(SCHED_SSTF, 4096, 0, 1);
    CHECK(sim_run(&ss, burst, 20, &small, NULL, NULL, &again) == 0);
    CHECK(again.served == 20 && again.max_depth == 4);
}

/* ---- request queue: saturation and shutdown --------------------------------------- */
typedef struct {
    request_queue_t *q;
    disk_request_t *r;
    volatile int done, rc;
} submit_arg_t;

static void *submitter(void *p) {
    submit_arg_t *s = p;
    s->rc = rq_submit(s->q, s->r);
    s->done = 1;
    return NULL;
}

static void test_queue(void) {
    request_queue_t q;
    CHECK(rq_init(&q, 2) == 0);
    disk_request_t r[4];
    for (int i = 0; i < 4; ++i) {
        CHECK(request_init(&r[i], RQ_READ, (uint32_t)i, NULL) == 0);
        r[i].arrival = (struct timespec){0, i};
    }
    CHECK(rq_submit(&q, &r[0]) == 0 && rq_submit(&q, &r[1]) == 0);
    submit_arg_t s = {&q, &r[2], 0, 0};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, submitter, &s) == 0);
    usleep(50000);
    CHECK(!s.done); /* full queue: the producer waits on not_full */
    scheduler_t f = sched(SCHED_FCFS, 100, 0, 1);
    CHECK(rq_take(&q, &f) == &r[0]);
    pthread_join(t, NULL);
    CHECK(s.done && s.rc == 0); /* space freed: the producer got in */
    /* Shutdown wakes a blocked producer with -ESHUTDOWN ... */
    submit_arg_t s2 = {&q, &r[3], 0, 0};
    CHECK(pthread_create(&t, NULL, submitter, &s2) == 0);
    usleep(50000);
    CHECK(!s2.done);
    rq_stop(&q);
    pthread_join(t, NULL);
    CHECK(s2.rc == -ESHUTDOWN);
    /* ... and the dispatcher still drains what was queued, then gets NULL. */
    CHECK(rq_take(&q, &f) == &r[1]);
    CHECK(rq_take(&q, &f) == &r[2]);
    CHECK(rq_take(&q, &f) == NULL);
    for (int i = 0; i < 4; ++i)
        request_destroy(&r[i]);
    rq_destroy(&q);
}

int main(void) {
    test_textbook();
    test_advance();
    test_starvation();
    test_adaptive_bound();
    test_deadline_order();
    test_tradeoff();
    test_queue();
    printf("scheduler tests passed (%d checks)\n", checks);
    return 0;
}
