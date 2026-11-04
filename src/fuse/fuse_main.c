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
    sched_policy_t policy;
    double alpha, beta;
    bool verbose;
} g_cfg;

/*
 * Runs after fuse_main() has daemonized. The dispatcher thread must be created
 * here: threads started before the fork() would not exist in the daemon.
 */
static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    (void)cfg;
    int rc = sfs_mount_image(&g_fs, g_cfg.image, g_cfg.policy, g_cfg.alpha, g_cfg.beta,
                             g_cfg.verbose);
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
    (void)o;
    (void)i;
    (void)fl;
    rd_ctx_t c = {b, f};
    return sfs_readdir(&g_fs, p, rd_add, &c);
}

static int op_create(const char *p, mode_t m, struct fuse_file_info *i) {
    (void)i;
    return sfs_create(&g_fs, p, m, NULL);
}

static int op_open(const char *p, struct fuse_file_info *i) {
    (void)i;
    struct stat s;
    int rc = sfs_getattr(&g_fs, p, &s);
    return !rc && S_ISDIR(s.st_mode) ? -EISDIR : rc;
}

static int op_read(const char *p, char *b, size_t n, off_t o, struct fuse_file_info *i) {
    (void)i;
    ssize_t rc = sfs_read(&g_fs, p, b, n, o);
    return rc < 0 ? (int)rc : (int)rc;
}

static int op_write(const char *p, const char *b, size_t n, off_t o, struct fuse_file_info *i) {
    (void)i;
    ssize_t rc = sfs_write(&g_fs, p, b, n, o);
    return rc < 0 ? (int)rc : (int)rc;
}

static int op_truncate(const char *p, off_t n, struct fuse_file_info *i) {
    (void)i;
    return sfs_truncate(&g_fs, p, n);
}

static int op_unlink(const char *p) {
    return sfs_unlink(&g_fs, p);
}

static int op_mkdir(const char *p, mode_t m) {
    return sfs_mkdir(&g_fs, p, m);
}

static int op_rmdir(const char *p) {
    return sfs_rmdir(&g_fs, p);
}

static int op_rename(const char *a, const char *b, unsigned int flags) {
    return flags ? -EINVAL : sfs_rename(&g_fs, a, b);
}

static int op_statfs(const char *p, struct statvfs *s) {
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
    (void)p;
    (void)d;
    (void)i;
    return fsync(g_fs.disk.fd) ? -errno : 0;
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
            "Usage: %s IMAGE MOUNTPOINT [--sched=fcfs|sstf|scan|adaptive]\n"
            "       [--alpha=N] [--beta=N] [--verbose] [FUSE options]\n",
            n);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *image = argv[1];
    sched_policy_t policy = SCHED_FCFS;
    double alpha = .7, beta = .3;
    bool verbose = false;
    char **fa = calloc((size_t)argc + 1, sizeof(*fa));
    if (!fa)
        return 1;
    int fc = 0;
    fa[fc++] = argv[0];
    fa[fc++] = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (!strncmp(argv[i], "--sched=", 8)) {
            if (scheduler_parse(argv[i] + 8, &policy)) {
                usage(argv[0]);
                free(fa);
                return 2;
            }
        } else if (!strncmp(argv[i], "--alpha=", 8))
            alpha = strtod(argv[i] + 8, NULL);
        else if (!strncmp(argv[i], "--beta=", 7))
            beta = strtod(argv[i] + 7, NULL);
        else if (!strcmp(argv[i], "--verbose"))
            verbose = true;
        else
            fa[fc++] = argv[i];
    }
    if (alpha < 0 || beta < 0) {
        fprintf(stderr, "alpha and beta must be non-negative\n");
        free(fa);
        return 2;
    }
    /* FUSE changes directory to / when it daemonizes, so keep an absolute path. */
    g_cfg.image = realpath(image, NULL);
    if (!g_cfg.image) {
        fprintf(stderr, "cannot open image: %s\n", strerror(errno));
        free(fa);
        return 1;
    }
    g_cfg.policy = policy;
    g_cfg.alpha = alpha;
    g_cfg.beta = beta;
    g_cfg.verbose = verbose;
    logger_open("filesystem.log", verbose);
    /* Validate the image now so errors reach the terminal; op_init mounts it for real. */
    int rc = sfs_mount_image(&g_fs, g_cfg.image, policy, alpha, beta, verbose);
    if (rc) {
        fprintf(stderr, "cannot open image: %s\n", strerror(-rc));
        free(g_cfg.image);
        free(fa);
        logger_close();
        return 1;
    }
    printf("SchedFS: %u blocks x %u bytes, %u inodes, scheduler=%s\n", g_fs.sb.total_blocks,
           g_fs.sb.block_size, g_fs.sb.total_inodes, scheduler_name(policy));
    sfs_unmount_image(&g_fs);
    fflush(stdout);
    rc = fuse_main(fc, fa, &ops, NULL);
    logger_close();
    free(g_cfg.image);
    free(fa);
    return rc;
}
