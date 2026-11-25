/*
 * fuse_main.c
 *
 * schedfs: libfuse3 adapter that maps VFS callbacks onto the SchedFS API.
 */
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include "filesystem.h"
#include "logger.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sfs_t g_fs;
static bool g_mounted;

/* Mount settings, kept until op_init can start the file system. */
static struct {
    char *image;
    vdisk_options_t disk;
    bool verbose;
} g_cfg;

/* Record which process issued the current operation (measurement only). */
static void who(void) {
    struct fuse_context *c = fuse_get_context();
    sfs_request_client = c ? (uint32_t)c->pid : 0;
}

/*
 * Runs after fuse_main() has daemonized. The dispatcher thread must be created
 * here: threads started before the fork() would not exist in the daemon.
 */
static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    (void)cfg;
    int rc = sfs_mount_image(&g_fs, g_cfg.image, &g_cfg.disk, g_cfg.verbose);
    if (rc) {
        sfs_log(LOG_ERROR, "cannot start file system: %s", strerror(-rc));
        fuse_exit(fuse_get_context()->fuse);
    } else
        g_mounted = true;
    return NULL;
}

static void op_destroy(void *private_data) {
    (void)private_data;
    if (g_mounted)
        sfs_unmount_image(&g_fs);
    g_mounted = false;
}

static int op_getattr(const char *p, struct stat *s, struct fuse_file_info *fi) {
    who();
    (void)fi;
    return sfs_getattr(&g_fs, p, s);
}

typedef struct {
    void *buf;
    fuse_fill_dir_t fill;
} rd_ctx_t;

static int rd_add(const char *n, const struct stat *s, void *a) {
    rd_ctx_t *c = a;
    return c->fill(c->buf, n, s, 0, 0);
}

static int op_readdir(const char *p, void *b, fuse_fill_dir_t f, off_t o, struct fuse_file_info *i,
                      enum fuse_readdir_flags fl) {
    who();
    (void)o;
    (void)i;
    (void)fl;
    rd_ctx_t c = {b, f};
    return sfs_readdir(&g_fs, p, rd_add, &c);
}

static int op_create(const char *p, mode_t m, struct fuse_file_info *i) {
    who();
    (void)i;
    return sfs_create(&g_fs, p, m, NULL);
}

static int op_open(const char *p, struct fuse_file_info *i) {
    who();
    (void)i;
    struct stat s;
    int rc = sfs_getattr(&g_fs, p, &s);
    return !rc && S_ISDIR(s.st_mode) ? -EISDIR : rc;
}

static int op_read(const char *p, char *b, size_t n, off_t o, struct fuse_file_info *i) {
    who();
    (void)i;
    ssize_t rc = sfs_read(&g_fs, p, b, n, o);
    return rc < 0 ? (int)rc : (int)rc;
}

static int op_write(const char *p, const char *b, size_t n, off_t o, struct fuse_file_info *i) {
    who();
    (void)i;
    ssize_t rc = sfs_write(&g_fs, p, b, n, o);
    return rc < 0 ? (int)rc : (int)rc;
}

static int op_truncate(const char *p, off_t n, struct fuse_file_info *i) {
    who();
    (void)i;
    return sfs_truncate(&g_fs, p, n);
}

static int op_unlink(const char *p) {
    who();
    return sfs_unlink(&g_fs, p);
}

static int op_mkdir(const char *p, mode_t m) {
    who();
    return sfs_mkdir(&g_fs, p, m);
}

static int op_rmdir(const char *p) {
    who();
    return sfs_rmdir(&g_fs, p);
}

static int op_rename(const char *a, const char *b, unsigned int flags) {
    who();
    return flags ? -EINVAL : sfs_rename(&g_fs, a, b);
}

static int op_statfs(const char *p, struct statvfs *s) {
    who();
    (void)p;
    return sfs_statfs(&g_fs, s);
}

static int op_flush(const char *p, struct fuse_file_info *i) {
    (void)p;
    (void)i;
    return 0;
}

static int op_release(const char *p, struct fuse_file_info *i) {
    (void)p;
    (void)i;
    return 0;
}

static int op_fsync(const char *p, int d, struct fuse_file_info *i) {
    who();
    (void)p;
    (void)d;
    (void)i;
    return vdisk_sync(&g_fs.disk);
}
static struct fuse_operations ops = {.init = op_init,
                                     .destroy = op_destroy,
                                     .getattr = op_getattr,
                                     .readdir = op_readdir,
                                     .create = op_create,
                                     .open = op_open,
                                     .read = op_read,
                                     .write = op_write,
                                     .truncate = op_truncate,
                                     .unlink = op_unlink,
                                     .mkdir = op_mkdir,
                                     .rmdir = op_rmdir,
                                     .rename = op_rename,
                                     .statfs = op_statfs,
                                     .flush = op_flush,
                                     .release = op_release,
                                     .fsync = op_fsync};

static void usage(const char *n) {
    fprintf(stderr,
            "Usage: %s IMAGE MOUNTPOINT [options] [FUSE options]\n"
            "  --sched=fcfs|sstf|scan|look|adaptive|deadline   (default fcfs)\n"
            "  --read-deadline-ms=N --write-deadline-ms=N\n"
            "                         deadline policy: service-start deadlines (default 0, 2)\n"
            "  --deadline-ms=N        set both deadlines to N\n"
            "  --alpha=N --beta=N     adaptive weights (default 0.7, 0.3)\n"
            "  --model-base-us=N --model-us-per-block=N\n"
            "                         modeled service time per request (default 0 = off)\n"
            "  --direct-io            open the image with O_DIRECT (bypass host page cache)\n"
            "  --queue=N              request-queue capacity (default 128)\n"
            "  --trace=FILE           write one CSV row per served block request at unmount\n"
            "  --verbose              DEBUG lines in filesystem.log\n",
            n);
}

static int parse_double(const char *text, double *out) {
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text || *end || v < 0)
        return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *image = argv[1];
    vdisk_default_options(&g_cfg.disk);
    const char *trace = NULL;
    bool verbose = false;
    char **fa = calloc((size_t)argc + 1, sizeof(*fa));
    if (!fa)
        return 1;
    int fc = 0, bad = 0;
    fa[fc++] = argv[0];
    fa[fc++] = argv[2];
    for (int i = 3; i < argc && !bad; ++i) {
        const char *a = argv[i];
        double q = 0;
        if (!strncmp(a, "--sched=", 8))
            bad = scheduler_parse(a + 8, &g_cfg.disk.policy);
        else if (!strncmp(a, "--alpha=", 8))
            bad = parse_double(a + 8, &g_cfg.disk.alpha);
        else if (!strncmp(a, "--beta=", 7))
            bad = parse_double(a + 7, &g_cfg.disk.beta);
        else if (!strncmp(a, "--deadline-ms=", 14)) {
            bad = parse_double(a + 14, &g_cfg.disk.read_deadline_ms);
            g_cfg.disk.write_deadline_ms = g_cfg.disk.read_deadline_ms;
        } else if (!strncmp(a, "--read-deadline-ms=", 19))
            bad = parse_double(a + 19, &g_cfg.disk.read_deadline_ms);
        else if (!strncmp(a, "--write-deadline-ms=", 20))
            bad = parse_double(a + 20, &g_cfg.disk.write_deadline_ms);
        else if (!strncmp(a, "--model-base-us=", 16))
            bad = parse_double(a + 16, &g_cfg.disk.model_base_us);
        else if (!strncmp(a, "--model-us-per-block=", 21))
            bad = parse_double(a + 21, &g_cfg.disk.model_us_per_block);
        else if (!strncmp(a, "--queue=", 8)) {
            bad = parse_double(a + 8, &q) || q < 1 || q > 65536;
            g_cfg.disk.queue_capacity = (size_t)q;
        } else if (!strncmp(a, "--trace=", 8))
            trace = a + 8;
        else if (!strcmp(a, "--direct-io"))
            g_cfg.disk.direct_io = true;
        else if (!strcmp(a, "--verbose"))
            verbose = true;
        else
            fa[fc++] = argv[i];
    }
    if (bad) {
        usage(argv[0]);
        free(fa);
        return 2;
    }
    /* FUSE changes directory to / when it daemonizes, so keep absolute paths. */
    g_cfg.image = realpath(image, NULL);
    if (!g_cfg.image) {
        fprintf(stderr, "cannot open image: %s\n", strerror(errno));
        free(fa);
        return 1;
    }
    char *trace_abs = NULL;
    if (trace) {
        char *dir = strdup(trace), *slash = strrchr(dir, '/');
        const char *base = slash ? slash + 1 : trace;
        if (slash)
            *slash = '\0';
        char *rdir = realpath(slash ? (dir[0] ? dir : "/") : ".", NULL);
        if (rdir && (trace_abs = malloc(strlen(rdir) + strlen(base) + 2)))
            sprintf(trace_abs, "%s/%s", rdir, base);
        free(rdir);
        free(dir);
        if (!trace_abs) {
            fprintf(stderr, "bad trace path: %s\n", trace);
            free(g_cfg.image);
            free(fa);
            return 1;
        }
    }
    g_cfg.verbose = verbose;
    logger_open("filesystem.log", verbose);
    /* Validate the image now so errors reach the terminal; op_init mounts it for real. */
    vdisk_options_t check = g_cfg.disk;
    check.trace_path = NULL;
    int rc = sfs_mount_image(&g_fs, g_cfg.image, &check, verbose);
    if (rc) {
        fprintf(stderr, "cannot open image: %s\n", strerror(-rc));
        free(g_cfg.image);
        free(trace_abs);
        free(fa);
        logger_close();
        return 1;
    }
    printf("SchedFS: %u blocks x %u bytes, %u inodes, scheduler=%s\n", g_fs.sb.total_blocks,
           g_fs.sb.block_size, g_fs.sb.total_inodes, scheduler_name(g_cfg.disk.policy));
    sfs_unmount_image(&g_fs);
    fflush(stdout);
    g_cfg.disk.trace_path = trace_abs;
    rc = fuse_main(fc, fa, &ops, NULL);
    logger_close();
    free(g_cfg.image);
    free(trace_abs);
    free(fa);
    return rc;
}
