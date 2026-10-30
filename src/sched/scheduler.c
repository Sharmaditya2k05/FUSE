#include "scheduler.h"
#include <math.h>
#include <string.h>

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) * 1000.0 +
           (double)(b.tv_nsec - a.tv_nsec) / 1000000.0;
}

int scheduler_parse(const char *name, sched_policy_t *out) {
    if (!strcmp(name, "fcfs")) *out = SCHED_FCFS;
    else if (!strcmp(name, "sstf")) *out = SCHED_SSTF;
    else if (!strcmp(name, "scan")) *out = SCHED_SCAN;
    else if (!strcmp(name, "adaptive")) *out = SCHED_ADAPTIVE;
    else return -1;
    return 0;
}

const char *scheduler_name(sched_policy_t p) {
    static const char *names[] = {"fcfs", "sstf", "scan", "adaptive"};
    return p <= SCHED_ADAPTIVE ? names[p] : "unknown";
}

static uint32_t distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static int older(const disk_request_t *a, const disk_request_t *b) {
    if (a->arrival.tv_sec != b->arrival.tv_sec) return a->arrival.tv_sec < b->arrival.tv_sec;
    if (a->arrival.tv_nsec != b->arrival.tv_nsec) return a->arrival.tv_nsec < b->arrival.tv_nsec;
    return a->id < b->id;
}

size_t scheduler_pick(const scheduler_t *s, disk_request_t *const *items, size_t count,
                      struct timespec now) {
    size_t best = 0;
    if (count < 2 || s->policy == SCHED_FCFS) {
        for (size_t i = 1; i < count; ++i) if (older(items[i], items[best])) best = i;
        return best;
    }
    if (s->policy == SCHED_SSTF) {
        for (size_t i = 1; i < count; ++i) {
            uint32_t di = distance(s->head, items[i]->block);
            uint32_t db = distance(s->head, items[best]->block);
            if (di < db || (di == db && older(items[i], items[best]))) best = i;
        }
        return best;
    }
    if (s->policy == SCHED_SCAN) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            bool direction_ok = s->scan_direction > 0 ? items[i]->block >= s->head
                                                      : items[i]->block <= s->head;
            if (!direction_ok) continue;
            if (!found || distance(s->head, items[i]->block) < distance(s->head, items[best]->block) ||
                (distance(s->head, items[i]->block) == distance(s->head, items[best]->block) &&
                 older(items[i], items[best]))) {
                best = i;
                found = true;
            }
        }
        if (found) return best;
        for (size_t i = 1; i < count; ++i) {
            if (distance(s->head, items[i]->block) < distance(s->head, items[best]->block) ||
                (distance(s->head, items[i]->block) == distance(s->head, items[best]->block) &&
                 older(items[i], items[best]))) best = i;
        }
        return best;
    }
    double max_wait = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double w = elapsed_ms(items[i]->arrival, now);
        if (w > max_wait) max_wait = w;
    }
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
