/*
 * benchmark_main.c
 *
 * schedfs-bench: deterministic scheduler SIMULATION (no file system, no real disk).
 *
 * Replays block traces through the real scheduler_pick()/scheduler_advance() with a
 * simulated clock (src/sched/sim.c) and writes benchmark.csv. Service time is MODELED
 * (base + per-block x movement), so waiting times and throughput here are properties
 * of the model, not measurements. Use scripts/run_mounted.sh for measurements of the
 * mounted file system.
 *
 * Scenarios
 *   static   sequential, random, clustered, mixed, interleaved: fixed lists that are
 *            all pending at time 0 (head at 53 moving toward 0). "interleaved" was
 *            called "concurrent" before; it is a fixed interleaving of near and far
 *            blocks, not a live concurrent workload.
 *   dynamic  stream_far: a saturated nearby stream (one request per 100 us, constant
 *            100 us service, so every policy carries the same load) plus a distant read
 *            every 20 ms. Shows starvation and the deadline policy's bound.
 *            two_regions: two sequential streams in distant regions plus periodic small
 *            reads near the start, with movement-dependent service time; shows the
 *            movement saved by locality and the waiting time it costs.
 */
#include "scheduler.h"
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define US 1000ULL

static const uint32_t seq[] = {10, 11, 12, 13, 14, 15, 16, 17};
static const uint32_t random_w[] = {145, 12, 98, 176, 32, 67, 4};
static const uint32_t clustered[] = {90, 93, 96, 101, 104, 108, 111};
static const uint32_t mixed[] = {10, 11, 12, 145, 13, 14, 67, 15, 16, 176, 17};
static const uint32_t interleaved[] = {300,  12, 900, 13, 1500, 14, 2200, 15,
                                       3010, 16, 44,  17, 1888, 18, 4000, 19};

typedef struct {
    const char *name, *kind;
    sim_arrival_t *a;
    size_t n;
    sim_model_t model;
    int *probe; /* requests whose waits are reported separately (may be NULL) */
    uint32_t head;
    int direction;
} scenario_t;

static sim_arrival_t *static_trace(const uint32_t *b, size_t n) {
    sim_arrival_t *a = calloc(n, sizeof(*a));
    for (size_t i = 0; i < n && a; ++i)
        a[i] = (sim_arrival_t){0, b[i], RQ_READ};
    return a;
}

static size_t stream_far(sim_arrival_t **out, int **probe) {
    enum { N = 4000 };
    sim_arrival_t *a = calloc(N, sizeof(*a));
    int *p = calloc(N, sizeof(*p));
    size_t n = 0;
    for (int i = 0; n < N; ++i) {
        uint64_t t = (uint64_t)i * 100 * US;
        if (i % 200 == 50) {
            p[n] = 1;
            a[n++] = (sim_arrival_t){t, 100u + (uint32_t)(i / 200) * 7u, RQ_READ};
        }
        if (n < N)
            a[n++] = (sim_arrival_t){t, 2000u + (uint32_t)(i % 64), RQ_WRITE};
    }
    *out = a;
    *probe = p;
    return n;
}

static size_t two_regions(sim_arrival_t **out, int **probe) {
    enum { N = 6000 };
    sim_arrival_t *a = calloc(N, sizeof(*a));
    int *p = calloc(N, sizeof(*p));
    size_t n = 0;
    for (int i = 0; n < N; ++i) {
        /* one write to each stream every 2.4 ms: FCFS runs at about 87% utilisation */
        uint64_t t = (uint64_t)i * 2400 * US;
        if (i % 10 == 7 && n < N) { /* a small read near the start every 24 ms */
            p[n] = 1;
            a[n++] = (sim_arrival_t){t, 20u + (uint32_t)(i % 16), RQ_READ};
        }
        if (n < N)
            a[n++] = (sim_arrival_t){t, 1000u + (uint32_t)(i % 1000), RQ_WRITE};
        if (n < N)
            a[n++] = (sim_arrival_t){t + 1200 * US, 3000u + (uint32_t)(i % 1000), RQ_WRITE};
    }
    *out = a;
    *probe = p;
    return n;
}

static void run(FILE *out, const scenario_t *sc, sched_policy_t p) {
    scheduler_t s;
    scheduler_init(&s, p, 4096);
    s.head = sc->head;
    s.scan_direction = sc->direction;
    double *wait = calloc(sc->n, sizeof(*wait));
    sim_result_t r;
    if (!wait || sim_run(&s, sc->a, sc->n, &sc->model, NULL, wait, &r)) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    double probe_max = 0, probe_sum = 0;
    size_t probes = 0;
    for (size_t i = 0; sc->probe && i < sc->n; ++i)
        if (sc->probe[i]) {
            probes++;
            probe_sum += wait[i];
            if (wait[i] > probe_max)
                probe_max = wait[i];
        }
    double elapsed = (double)r.makespan_ns / 1e6;
    fprintf(out, "%s,%s,%s,%llu,%.3f,%.6f,%.6f,%.6f,%zu,%.6f,%.3f,%zu,%.6f,%.6f\n", sc->name,
            sc->kind, scheduler_name(p), (unsigned long long)r.movement,
            (double)r.movement / (double)sc->n, r.wait_mean_ms, r.wait_max_ms, r.wait_stddev_ms,
            r.served, elapsed, elapsed > 0 ? (double)r.served / (elapsed / 1000.0) : 0.0, probes,
            probes ? probe_sum / (double)probes : 0.0, probe_max);
    free(wait);
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--all")) {
        fprintf(stderr, "Usage: %s --all\n", argv[0]);
        return 2;
    }
    sim_model_t trace_model = {10, 2, 0}; /* 10 us + 2 us per block moved */
    scenario_t sc[7] = {
        {"sequential", "static", static_trace(seq, 8), 8, trace_model, NULL, 53, -1},
        {"random", "static", static_trace(random_w, 7), 7, trace_model, NULL, 53, -1},
        {"clustered", "static", static_trace(clustered, 7), 7, trace_model, NULL, 53, -1},
        {"mixed", "static", static_trace(mixed, 11), 11, trace_model, NULL, 53, -1},
        {"interleaved", "static", static_trace(interleaved, 16), 16, trace_model, NULL, 53, -1},
        {"stream_far", "dynamic", NULL, 0, {100, 0, 128}, NULL, 2000, 1},
        {"two_regions", "dynamic", NULL, 0, {50, 0.5, 128}, NULL, 1000, 1},
    };
    sc[5].n = stream_far(&sc[5].a, &sc[5].probe);
    sc[6].n = two_regions(&sc[6].a, &sc[6].probe);
    FILE *f = fopen("benchmark.csv", "w");
    if (!f) {
        perror("benchmark.csv");
        return 1;
    }
    fputs("workload,kind,policy,total_seek,average_seek,average_wait_ms,maximum_wait_ms,"
          "wait_stddev_ms,total_requests,elapsed_ms,throughput_rps,probe_requests,"
          "probe_mean_wait_ms,probe_max_wait_ms\n",
          f);
    for (size_t i = 0; i < 7; ++i) {
        for (int p = 0; p < SCHED_POLICY_COUNT; ++p)
            run(f, &sc[i], (sched_policy_t)p);
        free(sc[i].a);
        free(sc[i].probe);
    }
    fclose(f);
    puts("Wrote benchmark.csv: deterministic SIMULATION with a modeled service time\n"
         "(not a measurement of the file system or of a disk). See docs/evaluation.md.");
    return 0;
}
