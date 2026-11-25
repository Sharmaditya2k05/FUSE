/*
 * virtual_disk.c
 *
 * Image-file block device. Only the dispatcher thread calls pread/pwrite.
 */
#define _GNU_SOURCE
#include "virtual_disk.h"
#include "layout.h"
#include "logger.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int64_t ns_between(struct timespec a, struct timespec b) {
    return (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL + (int64_t)(b.tv_nsec - a.tv_nsec);
}

static double diff_ms(struct timespec a, struct timespec b) {
    return (double)ns_between(a, b) / 1e6;
}

static ssize_t full_pio(int fd, void *buf, size_t n, off_t off, bool write_op) {
    size_t done = 0;
    while (done < n) {
        ssize_t rc = write_op ? pwrite(fd, (char *)buf + done, n - done, off + (off_t)done)
                              : pread(fd, (char *)buf + done, n - done, off + (off_t)done);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            return rc < 0 ? -errno : -EIO;
        done += (size_t)rc;
    }
    return (ssize_t)done;
}

/* Serve one request with the image I/O; O_DIRECT needs an aligned buffer. */
static ssize_t serve(virtual_disk_t *d, disk_request_t *r) {
    off_t off = (off_t)r->block * SFS_BLOCK_SIZE;
    bool w = r->op == RQ_WRITE;
    if (!d->bounce)
        return full_pio(d->fd, r->buffer, SFS_BLOCK_SIZE, off, w);
    if (w)
        memcpy(d->bounce, r->buffer, SFS_BLOCK_SIZE);
    ssize_t rc = full_pio(d->fd, d->bounce, SFS_BLOCK_SIZE, off, w);
    if (!w && rc >= 0)
        memcpy(r->buffer, d->bounce, SFS_BLOCK_SIZE);
    return rc;
}

/* Hold the dispatcher until the modeled service time has elapsed since start. */
static void model_delay(const virtual_disk_t *d, struct timespec start, uint64_t move) {
    double us = d->opts.model_base_us + d->opts.model_us_per_block * (double)move;
    if (us <= 0)
        return;
    int64_t ns = (int64_t)(us * 1000.0);
    struct timespec until = start;
    until.tv_sec += (time_t)(ns / 1000000000LL);
    until.tv_nsec += (long)(ns % 1000000000LL);
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
        ;
}

static void trace_add(virtual_disk_t *d, const disk_request_t *r, uint32_t from, uint64_t move) {
    if (!d->trace_path)
        return;
    if (d->trace_len == d->trace_cap) {
        size_t cap = d->trace_cap ? d->trace_cap * 2 : 65536;
        vdisk_trace_t *t = realloc(d->trace, cap * sizeof(*t));
        if (!t)
            return; /* keep serving; the trace just stops growing */
        d->trace = t;
        d->trace_cap = cap;
    }
    d->trace[d->trace_len++] = (vdisk_trace_t){r->id,
                                               r->client,
                                               r->block,
                                               from,
                                               r->queue_depth,
                                               move,
                                               (uint8_t)r->op,
                                               ns_between(d->metrics.started, r->arrival),
                                               ns_between(d->metrics.started, r->service_start),
                                               ns_between(d->metrics.started, r->completion)};
}

static void *dispatcher(void *arg) {
    virtual_disk_t *d = arg;
    pthread_mutex_lock(&d->gate_mutex);
    while (!d->gate_open)
        pthread_cond_wait(&d->gate_cv, &d->gate_mutex);
    pthread_mutex_unlock(&d->gate_mutex);
    while (true) {
        disk_request_t *r = rq_take(&d->queue, &d->scheduler);
        if (!r)
            break;
        clock_gettime(CLOCK_MONOTONIC, &r->service_start);
        uint32_t from = d->scheduler.head;
        uint64_t move = scheduler_advance(&d->scheduler, r->block);
        r->result = serve(d, r);
        r->error = r->result < 0 ? (int)-r->result : 0;
        model_delay(d, r->service_start, move);
        clock_gettime(CLOCK_MONOTONIC, &r->completion);
        metrics_record(&d->metrics, move, diff_ms(r->arrival, r->service_start),
                       r->queue_depth);
        trace_add(d, r, from, move);
        pthread_mutex_lock(&r->mutex);
        r->completed = true;
        pthread_cond_signal(&r->completed_cv);
        pthread_mutex_unlock(&r->mutex);
    }
    return NULL;
}

void vdisk_default_options(vdisk_options_t *o) {
    memset(o, 0, sizeof(*o));
    o->policy = SCHED_FCFS;
    o->alpha = .7;
    o->beta = .3;
    o->read_deadline_ms = 0.0; /* chosen on the tuning run, see docs/scheduling.md */
    o->write_deadline_ms = 2.0; /* tuned on seed 101, see docs/evaluation.md */
    o->queue_capacity = 128;
    o->initial_direction = 1;
}

void vdisk_resume(virtual_disk_t *d) {
    pthread_mutex_lock(&d->gate_mutex);
    d->gate_open = true;
    pthread_cond_broadcast(&d->gate_cv);
    pthread_mutex_unlock(&d->gate_mutex);
}

size_t vdisk_pending(virtual_disk_t *d) {
    pthread_mutex_lock(&d->queue.mutex);
    size_t n = d->queue.count;
    pthread_mutex_unlock(&d->queue.mutex);
    return n;
}

int vdisk_open(virtual_disk_t *d, const char *path, uint32_t blocks, const vdisk_options_t *opts) {
    memset(d, 0, sizeof(*d));
    if (opts)
        d->opts = *opts;
    else
        vdisk_default_options(&d->opts);
    if (!d->opts.queue_capacity)
        d->opts.queue_capacity = 128;
    int flags = O_RDWR | O_CLOEXEC;
    if (d->opts.direct_io) {
        flags |= O_DIRECT;
        if (posix_memalign(&d->bounce, SFS_BLOCK_SIZE, SFS_BLOCK_SIZE))
            return -ENOMEM;
    }
    d->fd = open(path, flags);
    if (d->fd < 0) {
        int e = errno;
        free(d->bounce);
        d->bounce = NULL;
        return -e;
    }
    d->blocks = blocks;
    scheduler_init(&d->scheduler, d->opts.policy, blocks);
    d->scheduler.alpha = d->opts.alpha;
    d->scheduler.beta = d->opts.beta;
    d->scheduler.deadline_read_ns = (uint64_t)(d->opts.read_deadline_ms * 1e6);
    d->scheduler.deadline_write_ns = (uint64_t)(d->opts.write_deadline_ms * 1e6);
    d->scheduler.head = d->opts.initial_head < blocks ? d->opts.initial_head : 0;
    d->scheduler.scan_direction = d->opts.initial_direction < 0 ? -1 : 1;
    pthread_mutex_init(&d->gate_mutex, NULL);
    pthread_cond_init(&d->gate_cv, NULL);
    d->gate_open = !d->opts.start_paused;
    if (d->opts.trace_path) {
        d->trace_path = strdup(d->opts.trace_path);
        d->opts.trace_path = d->trace_path;
    }
    int rc = rq_init(&d->queue, d->opts.queue_capacity);
    if (rc) {
        close(d->fd);
        free(d->bounce);
        free(d->trace_path);
        return rc;
    }
    metrics_init(&d->metrics);
    rc = pthread_create(&d->thread, NULL, dispatcher, d);
    if (rc) {
        rq_destroy(&d->queue);
        close(d->fd);
        free(d->bounce);
        free(d->trace_path);
        return -rc;
    }
    d->running = 1;
    return 0;
}

static int transfer(virtual_disk_t *d, uint32_t block, void *buf, rq_op_t op) {
    if (block >= d->blocks)
        return -EINVAL;
    disk_request_t r;
    int rc = request_init(&r, op, block, buf);
    if (rc)
        return rc;
    rc = rq_submit(&d->queue, &r);
    if (!rc)
        rc = request_wait(&r);
    request_destroy(&r);
    return rc;
}

int vdisk_read(virtual_disk_t *d, uint32_t b, void *buf) {
    return transfer(d, b, buf, RQ_READ);
}

int vdisk_write(virtual_disk_t *d, uint32_t b, const void *buf) {
    return transfer(d, b, (void *)buf, RQ_WRITE);
}

int vdisk_sync(virtual_disk_t *d) {
    return fsync(d->fd) ? -errno : 0;
}

static void trace_write(virtual_disk_t *d) {
    FILE *f = fopen(d->trace_path, "w");
    if (!f) {
        sfs_log(LOG_WARNING, "cannot write trace %s: %s", d->trace_path, strerror(errno));
        return;
    }
    fprintf(f, "# policy=%s read_deadline_ms=%.3f write_deadline_ms=%.3f "
               "model_base_us=%.3f model_us_per_block=%.4f "
               "direct_io=%d queue_capacity=%zu started_monotonic_ns=%lld\n",
            scheduler_name(d->opts.policy), d->opts.read_deadline_ms, d->opts.write_deadline_ms,
            d->opts.model_base_us,
            d->opts.model_us_per_block, d->opts.direct_io ? 1 : 0, d->opts.queue_capacity,
            (long long)d->metrics.started.tv_sec * 1000000000LL + d->metrics.started.tv_nsec);
    fputs("id,client,op,block,from,move,depth,arrival_ns,start_ns,end_ns\n", f);
    for (size_t i = 0; i < d->trace_len; ++i) {
        const vdisk_trace_t *t = &d->trace[i];
        fprintf(f, "%" PRIu64 ",%u,%c,%u,%u,%" PRIu64 ",%u,%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                t->id, t->client, t->op ? 'W' : 'R', t->block, t->from, t->move, t->depth,
                t->arrival_ns, t->start_ns, t->end_ns);
    }
    fclose(f);
}

void vdisk_close(virtual_disk_t *d) {
    if (!d->running)
        return;
    vdisk_resume(d); /* a paused dispatcher must still drain and exit */
    rq_stop(&d->queue);
    pthread_join(d->thread, NULL);
    pthread_cond_destroy(&d->gate_cv);
    pthread_mutex_destroy(&d->gate_mutex);
    metrics_finish(&d->metrics);
    fsync(d->fd);
    close(d->fd);
    rq_destroy(&d->queue);
    if (d->trace_path)
        trace_write(d);
    free(d->trace);
    free(d->trace_path);
    free(d->bounce);
    d->trace = NULL;
    d->trace_path = NULL;
    d->bounce = NULL;
    d->running = 0;
}
