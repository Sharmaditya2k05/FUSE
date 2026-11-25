/*
 * virtual_disk.h
 *
 * Block device backed by an image file, serviced by one dispatcher thread.
 */
#ifndef SCHEDFS_VIRTUAL_DISK_H
#define SCHEDFS_VIRTUAL_DISK_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "metrics.h"
#include "request_queue.h"

/* Mount-time settings of the block layer. Zero values select the defaults. */
typedef struct {
    sched_policy_t policy;
    double alpha, beta;       /* Adaptive weights */
    double read_deadline_ms;  /* Deadline policy: service-start deadline of a read */
    double write_deadline_ms; /* Deadline policy: service-start deadline of a write */
    double model_base_us;     /* Modeled service time: base + per_block x movement; */
    double model_us_per_block; /*   both 0 = no modeled delay (real image I/O only) */
    bool direct_io;           /* open the image with O_DIRECT (bypass the host page cache) */
    size_t queue_capacity;    /* bounded queue size (default 128) */
    const char *trace_path;   /* if set, one CSV row per served request is written at close */
    /* Test hooks: initial head position and direction, and a dispatcher that waits for
     * vdisk_resume() so a test can fill the queue before the first pick. */
    uint32_t initial_head;
    int initial_direction;
    bool start_paused;
} vdisk_options_t;

/* One served request, kept in memory by the dispatcher and written at close. */
typedef struct {
    uint64_t id;
    uint32_t client, block, from, depth;
    uint64_t move;
    uint8_t op;
    int64_t arrival_ns, start_ns, end_ns;
} vdisk_trace_t;

typedef struct {
    int fd;
    uint32_t blocks;
    request_queue_t queue;
    scheduler_t scheduler;
    sfs_metrics_t metrics;
    vdisk_options_t opts;
    void *bounce; /* aligned buffer for O_DIRECT */
    vdisk_trace_t *trace;
    size_t trace_len, trace_cap;
    char *trace_path;
    pthread_mutex_t gate_mutex; /* start_paused: dispatcher waits until opened */
    pthread_cond_t gate_cv;
    bool gate_open;
    pthread_t thread;
    int running;
} virtual_disk_t;

void vdisk_default_options(vdisk_options_t *o);
int vdisk_open(virtual_disk_t *d, const char *path, uint32_t blocks, const vdisk_options_t *opts);
int vdisk_read(virtual_disk_t *d, uint32_t block, void *buffer);
int vdisk_write(virtual_disk_t *d, uint32_t block, const void *buffer);
int vdisk_sync(virtual_disk_t *d);
void vdisk_resume(virtual_disk_t *d);
/* Number of requests currently waiting in the queue (for tests). */
size_t vdisk_pending(virtual_disk_t *d);
void vdisk_close(virtual_disk_t *d);
#endif
