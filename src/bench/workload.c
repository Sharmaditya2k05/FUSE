/*
 * workload.c
 *
 * schedfs-workload: a foreground/background file workload for a mounted directory.
 *
 *   - N background processes each write files of --bg-file-kb in --bg-chunk-kb writes,
 *     over and over (O_TRUNC each round, so blocks are freed and allocated again).
 *   - One foreground process, every --fg-interval-ms, opens a small file, reads or
 *     updates --fg-io-kb at a seeded random offset, and closes it. The time from
 *     open() to the end of close() is one sample of end-to-end operation latency.
 *
 * Every process writes its own CSV; nothing is shared between processes, so the
 * measurement adds no synchronization of its own. Works on any directory, so the
 * same run can be repeated on the host file system as an external reference.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *dir, *out;
    double duration_s, warmup_s, fg_interval_ms;
    int bg, fg_file_kb, fg_io_kb, bg_file_kb, bg_chunk_kb;
    unsigned seed;
    const char *fg_op;   /* read, update or mix */
    const char *bg_mode; /* write: write new data; copy: read a source file, write a copy */
    int live;            /* print a summary line every second (demo) */
} cfg_t;

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void sleep_until(int64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR)
        ;
}

static void die(const char *what) {
    fprintf(stderr, "schedfs-workload: %s: %s\n", what, strerror(errno));
    exit(1);
}

static FILE *open_out(const cfg_t *c, const char *name) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", c->out, name);
    FILE *f = fopen(path, "w");
    if (!f)
        die(path);
    return f;
}

static int write_all(int fd, const char *buf, size_t n, off_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t rc = pwrite(fd, buf + done, n - done, off + (off_t)done);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            return -1;
        done += (size_t)rc;
    }
    return 0;
}

static void background(const cfg_t *c, int id, int64_t start, int64_t end) {
    char name[64];
    snprintf(name, sizeof(name), "bg_%d.csv", id);
    FILE *out = open_out(c, name);
    /* One row per completed chunk, so throughput can be measured over any window
     * (a 2 MiB round can take seconds on a slow device). */
    fprintf(out, "worker,round,offset,bytes,end_ns,errors\n");
    size_t chunk = (size_t)c->bg_chunk_kb * 1024u, file = (size_t)c->bg_file_kb * 1024u;
    char *buf = malloc(chunk);
    if (!buf)
        die("malloc");
    memset(buf, 'a' + id % 26, chunk);
    int copy = !strcmp(c->bg_mode, "copy");
    char src[4096];
    snprintf(src, sizeof(src), "%s/bg_src_%d.dat", c->dir, id);
    sleep_until(start);
    for (int round = 0; now_ns() < end; ++round) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/bg_%d_%d.dat", c->dir, id, round & 1);
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644), errors = 0;
        int in = copy ? open(src, O_RDONLY) : -1;
        size_t done = 0;
        if (fd < 0 || (copy && in < 0))
            errors++;
        while (!errors && done < file && now_ns() < end) {
            size_t n = file - done < chunk ? file - done : chunk;
            if (copy && pread(in, buf, n, (off_t)done) != (ssize_t)n) {
                errors++;
                break;
            }
            if (write_all(fd, buf, n, (off_t)done)) {
                errors++;
                break;
            }
            fprintf(out, "%d,%d,%zu,%zu,%" PRId64 ",0\n", id, round, done, n, now_ns() - start);
            done += n;
        }
        if (in >= 0)
            close(in);
        if (fd >= 0 && close(fd))
            errors++;
        if (errors) {
            fprintf(out, "%d,%d,%zu,0,%" PRId64 ",%d\n", id, round, done, now_ns() - start, errors);
            fprintf(stderr, "schedfs-workload: background %d: %s\n", id, strerror(errno));
            break;
        }
    }
    fclose(out);
    free(buf);
}

static void foreground(const cfg_t *c, int64_t start, int64_t end) {
    FILE *out = open_out(c, "fg.csv");
    fprintf(out, "seq,op,offset,start_ns,open_ns,io_ns,close_ns,total_ns,ok\n");
    char path[4096];
    snprintf(path, sizeof(path), "%s/fg.dat", c->dir);
    size_t io = (size_t)c->fg_io_kb * 1024u;
    char *buf = malloc(io);
    if (!buf)
        die("malloc");
    unsigned seed = c->seed;
    int slots = c->fg_file_kb / c->fg_io_kb;
    int64_t period = (int64_t)(c->fg_interval_ms * 1e6), next = start;
    int64_t live_next = start + 1000000000LL;
    int live_n = 0;
    double live_sum = 0, live_max = 0;
    sleep_until(start);
    for (int seq = 0; now_ns() < end; ++seq) {
        int update = !strcmp(c->fg_op, "update") ||
                     (!strcmp(c->fg_op, "mix") && (rand_r(&seed) & 1));
        off_t off = (off_t)((unsigned)rand_r(&seed) % (unsigned)slots) * (off_t)io;
        memset(buf, 'A' + seq % 26, io);
        int64_t t0 = now_ns();
        int fd = open(path, update ? O_WRONLY : O_RDONLY), ok = fd >= 0;
        int64_t t1 = now_ns();
        if (ok)
            ok = update ? !write_all(fd, buf, io, off) : pread(fd, buf, io, off) == (ssize_t)io;
        int64_t t2 = now_ns();
        if (fd >= 0 && close(fd))
            ok = 0;
        int64_t t3 = now_ns();
        fprintf(out, "%d,%c,%lld,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d\n",
                seq, update ? 'U' : 'R', (long long)off, t0 - start, t1 - t0, t2 - t1, t3 - t2,
                t3 - t0, ok);
        if (c->live) { /* one line per second: what an interactive user would feel */
            double ms = (double)(t3 - t0) / 1e6;
            live_n++;
            live_sum += ms;
            if (ms > live_max)
                live_max = ms;
            if (t3 >= live_next) {
                fprintf(stderr, "  t=%4.1fs  small-file ops %3d  mean %6.2f ms  worst %6.2f ms\n",
                        (double)(t3 - start) / 1e9, live_n, live_sum / live_n, live_max);
                live_n = 0;
                live_sum = live_max = 0;
                live_next += 1000000000LL;
            }
        }
        next += period;
        if (next > now_ns())
            sleep_until(next);
        else
            next = now_ns(); /* the op overran its slot: start the next one now */
    }
    fclose(out);
    free(buf);
}

static void usage(void) {
    fputs("Usage: schedfs-workload --dir DIR --out OUTDIR [--duration S] [--warmup S]\n"
          "  [--bg N] [--bg-file-kb K] [--bg-chunk-kb K]\n"
          "  [--fg-interval-ms MS] [--fg-file-kb K] [--fg-io-kb K] [--fg-op read|update|mix]\n"
          "  [--bg-mode write|copy] [--seed N] [--live]\n",
          stderr);
    exit(2);
}

int main(int argc, char **argv) {
    cfg_t c = {NULL, NULL, 10.0, 1.0, 20.0, 2, 64, 4, 2048, 128, 312, "read", "write", 0};
    static struct option opts[] = {{"dir", 1, 0, 'd'},         {"out", 1, 0, 'o'},
                                   {"duration", 1, 0, 't'},    {"warmup", 1, 0, 'w'},
                                   {"bg", 1, 0, 'b'},          {"bg-file-kb", 1, 0, 'F'},
                                   {"bg-chunk-kb", 1, 0, 'C'}, {"fg-interval-ms", 1, 0, 'i'},
                                   {"fg-file-kb", 1, 0, 'f'},  {"fg-io-kb", 1, 0, 'x'},
                                   {"fg-op", 1, 0, 'p'},       {"seed", 1, 0, 's'},
                                   {"bg-mode", 1, 0, 'm'},     {"live", 0, 0, 'L'},
                                   {0, 0, 0, 0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (ch) {
        case 'd': c.dir = optarg; break;
        case 'o': c.out = optarg; break;
        case 't': c.duration_s = atof(optarg); break;
        case 'w': c.warmup_s = atof(optarg); break;
        case 'b': c.bg = atoi(optarg); break;
        case 'F': c.bg_file_kb = atoi(optarg); break;
        case 'C': c.bg_chunk_kb = atoi(optarg); break;
        case 'i': c.fg_interval_ms = atof(optarg); break;
        case 'f': c.fg_file_kb = atoi(optarg); break;
        case 'x': c.fg_io_kb = atoi(optarg); break;
        case 'p': c.fg_op = optarg; break;
        case 's': c.seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'm': c.bg_mode = optarg; break;
        case 'L': c.live = 1; break;
        default: usage();
        }
    }
    if (!c.dir || !c.out || c.bg < 0 || c.fg_io_kb <= 0 || c.fg_file_kb < c.fg_io_kb ||
        c.bg_chunk_kb <= 0 || c.bg_file_kb <= 0 || c.fg_interval_ms <= 0 ||
        (strcmp(c.fg_op, "read") && strcmp(c.fg_op, "update") && strcmp(c.fg_op, "mix")) ||
        (strcmp(c.bg_mode, "write") && strcmp(c.bg_mode, "copy")))
        usage();
    mkdir(c.out, 0755);
    if (!strcmp(c.bg_mode, "copy")) /* source files for the copies, written up front */
        for (int i = 0; i < c.bg; ++i) {
            char src[4096];
            snprintf(src, sizeof(src), "%s/bg_src_%d.dat", c.dir, i);
            size_t size = (size_t)c.bg_file_kb * 1024u;
            char *data = malloc(size);
            int sfd = open(src, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (!data || sfd < 0)
                die(src);
            memset(data, 's', size);
            if (write_all(sfd, data, size, 0) || close(sfd))
                die(src);
            free(data);
        }

    /* The foreground file is created and filled before the background starts. */
    char path[4096];
    snprintf(path, sizeof(path), "%s/fg.dat", c.dir);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        die(path);
    size_t fsize = (size_t)c.fg_file_kb * 1024u;
    char *fill = calloc(1, fsize);
    if (!fill || write_all(fd, fill, fsize, 0) || close(fd))
        die("fill foreground file");
    free(fill);

    int64_t start = now_ns() + 300000000LL; /* every process starts together */
    int64_t end = start + (int64_t)((c.warmup_s + c.duration_s) * 1e9);
    FILE *roles = open_out(&c, "roles.csv");
    fprintf(roles, "pid,role,worker\n");
    fflush(roles); /* a buffered line would otherwise be copied into every child */
    pid_t pids[256];
    int n = 0;
    for (int i = 0; i < c.bg && i < 255; ++i) {
        pid_t p = fork();
        if (p < 0)
            die("fork");
        if (!p) {
            fclose(roles);
            background(&c, i, start, end);
            _exit(0);
        }
        fprintf(roles, "%d,bg,%d\n", (int)p, i);
        fflush(roles);
        pids[n++] = p;
    }
    pid_t p = fork();
    if (p < 0)
        die("fork");
    if (!p) {
        fclose(roles);
        foreground(&c, start, end);
        _exit(0);
    }
    fprintf(roles, "%d,fg,0\n", (int)p);
    pids[n++] = p;
    fclose(roles);
    int failed = 0;
    for (int i = 0; i < n; ++i) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        failed |= !WIFEXITED(st) || WEXITSTATUS(st);
    }
    FILE *meta = open_out(&c, "workload.json");
    fprintf(meta,
            "{\"dir\": \"%s\", \"duration_s\": %.3f, \"warmup_s\": %.3f, \"bg\": %d, "
            "\"bg_file_kb\": %d, \"bg_chunk_kb\": %d, \"fg_interval_ms\": %.3f, "
            "\"fg_file_kb\": %d, \"fg_io_kb\": %d, \"fg_op\": \"%s\", \"bg_mode\": \"%s\", "
            "\"seed\": %u, \"t0_monotonic_ns\": %" PRId64 "}\n",
            c.dir, c.duration_s, c.warmup_s, c.bg, c.bg_file_kb, c.bg_chunk_kb, c.fg_interval_ms,
            c.fg_file_kb, c.fg_io_kb, c.fg_op, c.bg_mode, c.seed, start);
    fclose(meta);
    for (int i = 0; i < c.bg; ++i) {
        for (int k = 0; k < 2; ++k) {
            snprintf(path, sizeof(path), "%s/bg_%d_%d.dat", c.dir, i, k);
            unlink(path);
        }
        snprintf(path, sizeof(path), "%s/bg_src_%d.dat", c.dir, i);
        unlink(path);
    }
    return failed;
}
