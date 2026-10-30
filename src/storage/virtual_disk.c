#include "virtual_disk.h"
#include "layout.h"
#include "logger.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static double diff_ms(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec-a.tv_sec)*1000.0+(double)(b.tv_nsec-a.tv_nsec)/1000000.0;
}
static ssize_t full_pio(int fd, void *buf, size_t n, off_t off, bool write_op) {
    size_t done = 0;
    while (done < n) {
        ssize_t rc = write_op ? pwrite(fd, (char *)buf + done, n - done, off + (off_t)done)
                              : pread(fd, (char *)buf + done, n - done, off + (off_t)done);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return rc < 0 ? -errno : -EIO;
        done += (size_t)rc;
    }
    return (ssize_t)done;
}
static void *dispatcher(void *arg) {
    virtual_disk_t *d = arg;
    while (true) {
        disk_request_t *r = rq_take(&d->queue, &d->scheduler);
        if (!r) break;
        clock_gettime(CLOCK_MONOTONIC, &r->service_start);
        uint32_t from = d->scheduler.head;
        r->result = full_pio(d->fd, r->buffer, SFS_BLOCK_SIZE,
                             (off_t)r->block * SFS_BLOCK_SIZE, r->op == RQ_WRITE);
        r->error = r->result < 0 ? (int)-r->result : 0;
        clock_gettime(CLOCK_MONOTONIC, &r->completion);
        metrics_record(&d->metrics, from, r->block, diff_ms(r->arrival, r->service_start));
        if (d->scheduler.policy == SCHED_SCAN) {
            if (d->scheduler.scan_direction > 0 && r->block < from) d->scheduler.scan_direction = -1;
            else if (d->scheduler.scan_direction < 0 && r->block > from) d->scheduler.scan_direction = 1;
        }
        d->scheduler.head = r->block;
        pthread_mutex_lock(&r->mutex); r->completed = true;
        pthread_cond_signal(&r->completed_cv); pthread_mutex_unlock(&r->mutex);
    }
    return NULL;
}
int vdisk_open(virtual_disk_t *d, const char *path, uint32_t blocks,
               sched_policy_t policy, double alpha, double beta, size_t capacity) {
    memset(d, 0, sizeof(*d));
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) return -errno;
    d->blocks = blocks;
    d->scheduler = (scheduler_t){policy, blocks, 0, 1, alpha, beta};
    int rc = rq_init(&d->queue, capacity);
    if (rc) { close(d->fd); return rc; }
    metrics_init(&d->metrics);
    rc = pthread_create(&d->thread, NULL, dispatcher, d);
    if (rc) { rq_destroy(&d->queue); close(d->fd); return -rc; }
    d->running = 1;
    return 0;
}
static int transfer(virtual_disk_t *d, uint32_t block, void *buf, rq_op_t op) {
    if (block >= d->blocks) return -EINVAL;
    disk_request_t r; int rc = request_init(&r, op, block, buf);
    if (rc) return rc;
    rc = rq_submit(&d->queue, &r);
    if (!rc) rc = request_wait(&r);
    request_destroy(&r);
    return rc;
}
int vdisk_read(virtual_disk_t *d, uint32_t b, void *buf) { return transfer(d,b,buf,RQ_READ); }
int vdisk_write(virtual_disk_t *d, uint32_t b, const void *buf) { return transfer(d,b,(void *)buf,RQ_WRITE); }
void vdisk_close(virtual_disk_t *d) {
    if (!d->running) return;
    rq_stop(&d->queue); pthread_join(d->thread, NULL); metrics_finish(&d->metrics);
    fsync(d->fd); close(d->fd); rq_destroy(&d->queue); d->running = 0;
}
