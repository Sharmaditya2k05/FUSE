/*
 * scheduler.c
 *
 * Request selection for each policy, with deterministic (arrival, id) tie-breaking,
 * and the head-movement model shared by the dispatcher, the simulator and the tests.
 */
#include "scheduler.h"
#include <math.h>
#include <string.h>

static const char *const names[SCHED_POLICY_COUNT] = {"fcfs", "sstf",     "scan",
                                                      "look", "adaptive", "deadline"};

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) * 1000.0 + (double)(b.tv_nsec - a.tv_nsec) / 1000000.0;
}

int scheduler_parse(const char *name, sched_policy_t *out) {
    for (int p = 0; p < SCHED_POLICY_COUNT; ++p)
        if (!strcmp(name, names[p])) {
            *out = (sched_policy_t)p;
            return 0;
        }
    return -1;
}

const char *scheduler_name(sched_policy_t p) {
    return (unsigned)p < SCHED_POLICY_COUNT ? names[p] : "unknown";
}

void scheduler_init(scheduler_t *s, sched_policy_t p, uint32_t total_blocks) {
    memset(s, 0, sizeof(*s));
    s->policy = p;
    s->total_blocks = total_blocks;
    s->head = 0;
    s->scan_direction = 1;
    s->alpha = .7;
    s->beta = .3;
    s->deadline_read_ns = 0;
    s->deadline_write_ns = 2000000ULL;
}

static uint32_t distance(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

static int older(const disk_request_t *a, const disk_request_t *b) {
    if (a->arrival.tv_sec != b->arrival.tv_sec)
        return a->arrival.tv_sec < b->arrival.tv_sec;
    if (a->arrival.tv_nsec != b->arrival.tv_nsec)
        return a->arrival.tv_nsec < b->arrival.tv_nsec;
    return a->id < b->id;
}

/* Nearest block to the head; ties by (arrival, id). */
static size_t nearest(const scheduler_t *s, disk_request_t *const *items, size_t count) {
    size_t best = 0;
    for (size_t i = 1; i < count; ++i) {
        uint32_t di = distance(s->head, items[i]->block);
        uint32_t db = distance(s->head, items[best]->block);
        if (di < db || (di == db && older(items[i], items[best])))
            best = i;
    }
    return best;
}

static size_t oldest(disk_request_t *const *items, size_t count) {
    size_t best = 0;
    for (size_t i = 1; i < count; ++i)
        if (older(items[i], items[best]))
            best = i;
    return best;
}

static int64_t ts_ns(struct timespec t) {
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

int64_t scheduler_deadline(const scheduler_t *s, const disk_request_t *r) {
    uint64_t rel = r->op == RQ_WRITE ? s->deadline_write_ns : s->deadline_read_ns;
    return ts_ns(r->arrival) + (int64_t)rel;
}

int scheduler_overdue(const scheduler_t *s, const disk_request_t *r, struct timespec now) {
    return scheduler_deadline(s, r) <= ts_ns(now);
}

size_t scheduler_pick(const scheduler_t *s, disk_request_t *const *items, size_t count,
                      struct timespec now) {
    if (count < 2 || s->policy == SCHED_FCFS)
        return oldest(items, count);
    switch (s->policy) {
    case SCHED_SSTF:
        return nearest(s, items, count);
    case SCHED_SCAN:
    case SCHED_LOOK: {
        /* Nearest block in the current direction; if none is left, the head reverses
         * and the nearest block overall is the first one met on the way back. */
        size_t best = 0;
        int found = 0;
        for (size_t i = 0; i < count; ++i) {
            int ahead = s->scan_direction > 0 ? items[i]->block >= s->head
                                              : items[i]->block <= s->head;
            if (!ahead)
                continue;
            uint32_t di = distance(s->head, items[i]->block);
            uint32_t db = distance(s->head, items[best]->block);
            if (!found || di < db || (di == db && older(items[i], items[best]))) {
                best = i;
                found = 1;
            }
        }
        return found ? best : nearest(s, items, count);
    }
    case SCHED_DEADLINE_LOCAL: {
        /* Overdue requests first, earliest deadline first, ties by (arrival, id).
         * Within one operation type all deadlines are arrival + the same constant, so
         * a later arrival of that type can never overtake an overdue request. */
        int64_t t = ts_ns(now), best_dl = 0;
        size_t best = 0;
        int found = 0;
        for (size_t i = 0; i < count; ++i) {
            int64_t dl = scheduler_deadline(s, items[i]);
            if (dl > t)
                continue;
            if (!found || dl < best_dl || (dl == best_dl && older(items[i], items[best]))) {
                best = i;
                best_dl = dl;
                found = 1;
            }
        }
        return found ? best : nearest(s, items, count);
    }
    case SCHED_ADAPTIVE:
    default: {
        double max_wait = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double w = elapsed_ms(items[i]->arrival, now);
            if (w > max_wait)
                max_wait = w;
        }
        size_t best = 0;
        double best_score = INFINITY;
        for (size_t i = 0; i < count; ++i) {
            double dn = (double)distance(s->head, items[i]->block) / (double)s->total_blocks;
            double wn = max_wait > 0.0 ? elapsed_ms(items[i]->arrival, now) / max_wait : 0.0;
            double score = s->alpha * dn - s->beta * wn;
            if (score < best_score - 1e-12 ||
                (fabs(score - best_score) <= 1e-12 && older(items[i], items[best]))) {
                best = i;
                best_score = score;
            }
        }
        return best;
    }
    }
}

uint64_t scheduler_advance(scheduler_t *s, uint32_t next) {
    uint32_t head = s->head, last = s->total_blocks ? s->total_blocks - 1u : 0u;
    uint64_t move = distance(head, next);
    if (s->policy == SCHED_SCAN || s->policy == SCHED_LOOK) {
        if (s->scan_direction < 0 && next > head) {
            if (s->policy == SCHED_SCAN)
                move = (uint64_t)head + next; /* down to block 0, then up */
            s->scan_direction = 1;
        } else if (s->scan_direction > 0 && next < head) {
            if (s->policy == SCHED_SCAN)
                move = (uint64_t)(last - head) + (last - next); /* up to the edge, back */
            s->scan_direction = -1;
        }
    }
    s->head = next;
    return move;
}
