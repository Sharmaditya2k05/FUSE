/*
 * test_regress.c
 *
 * Regression tests for the review findings, against the core API (no FUSE mount):
 *   B  truncate keeps retained bytes and zeroes discarded ones (partial blocks, sparse
 *      extension, write past the end, persistence across remount);
 *   D  concurrent creation of one name, unlink racing with writers (inode lifetime),
 *      metadata write ordering, rename into its own subtree;
 *   A  SCAN and LOOK movement measured through the real dispatcher.
 * An image checker (fsck-style) verifies that no block is referenced twice, every used
 * block is marked in the bitmap, nothing leaks, and the free counters match.
 *
 * Usage: test_regress IMAGE   (a freshly formatted image; it is modified)
 */
#include "filesystem.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *image;
static _Atomic int checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void mount_fs(sfs_t *fs, sched_policy_t p) {
    vdisk_options_t o;
    vdisk_default_options(&o);
    o.policy = p;
    CHECK(sfs_mount_image(fs, image, &o, false) == 0);
}

/* ---- image checker ---------------------------------------------------------------- */
static void raw_read(int fd, uint32_t block, void *buf) {
    CHECK(pread(fd, buf, SFS_BLOCK_SIZE, (off_t)block * SFS_BLOCK_SIZE) == SFS_BLOCK_SIZE);
}

/* Checks the image file as it is on disk. Returns the number of used data blocks. */
static uint32_t fsck(void) {
    int fd = open(image, O_RDONLY);
    CHECK(fd >= 0);
    uint8_t blk[SFS_BLOCK_SIZE];
    raw_read(fd, 0, blk);
    sfs_superblock_t sb;
    memcpy(&sb, blk, sizeof(sb));
    uint8_t *bitmap = calloc(sb.bitmap_blocks, SFS_BLOCK_SIZE);
    uint8_t *seen = calloc(sb.total_blocks, 1);
    CHECK(bitmap && seen);
    for (uint32_t i = 0; i < sb.bitmap_blocks; ++i)
        raw_read(fd, sb.bitmap_start + i, bitmap + (size_t)i * SFS_BLOCK_SIZE);
#define BIT(b) ((bitmap[(b) / 8] >> ((b) % 8)) & 1)
    uint32_t used_inodes = 0, used_blocks = 0;
    for (uint32_t ino = 1; ino <= sb.total_inodes; ++ino) {
        uint32_t per = SFS_BLOCK_SIZE / sizeof(sfs_inode_t);
        raw_read(fd, sb.inode_start + (ino - 1) / per, blk);
        sfs_inode_t n;
        memcpy(&n, blk + ((ino - 1) % per) * sizeof(n), sizeof(n));
        if (!n.mode)
            continue;
        used_inodes++;
        uint32_t refs[SFS_DIRECT_BLOCKS + 1 + SFS_INDIRECT_BLOCKS], nr = 0;
        for (uint32_t i = 0; i < SFS_DIRECT_BLOCKS; ++i)
            if (n.direct[i])
                refs[nr++] = n.direct[i];
        if (n.indirect) {
            refs[nr++] = n.indirect;
            uint32_t ptrs[SFS_INDIRECT_BLOCKS];
            raw_read(fd, n.indirect, ptrs);
            for (uint32_t i = 0; i < SFS_INDIRECT_BLOCKS; ++i)
                if (ptrs[i])
                    refs[nr++] = ptrs[i];
        }
        for (uint32_t i = 0; i < nr; ++i) {
            uint32_t b = refs[i];
            if (b < sb.data_start || b >= sb.total_blocks || seen[b] || !BIT(b)) {
                fprintf(stderr, "fsck: inode %u block %u: %s\n", ino, b,
                        b < sb.data_start || b >= sb.total_blocks ? "out of range"
                        : seen[b]                                 ? "referenced twice"
                                                                  : "not marked in bitmap");
                exit(1);
            }
            seen[b] = 1;
            used_blocks++;
        }
    }
    uint32_t marked = 0;
    for (uint32_t b = sb.data_start; b < sb.total_blocks; ++b)
        marked += BIT(b);
    if (marked != used_blocks || sb.free_blocks != sb.total_blocks - sb.data_start - marked ||
        sb.free_inodes != sb.total_inodes - used_inodes) {
        fprintf(stderr, "fsck: marked %u used %u free_blocks %u free_inodes %u used_inodes %u\n",
                marked, used_blocks, sb.free_blocks, sb.free_inodes, used_inodes);
        exit(1);
    }
#undef BIT
    free(bitmap);
    free(seen);
    close(fd);
    return used_blocks;
}

/* ---- B: truncate ------------------------------------------------------------------- */
static void expect(sfs_t *fs, const char *path, const char *want, size_t n, off_t off) {
    char got[16384];
    CHECK(n <= sizeof(got));
    CHECK(sfs_read(fs, path, got, n, off) == (ssize_t)n);
    CHECK(!memcmp(got, want, n));
}

static void test_truncate(sfs_t *fs) {
    CHECK(sfs_create(fs, "/t1", 0644, NULL) == 0);
    CHECK(sfs_write(fs, "/t1", "ABCDEFGH", 8, 0) == 8);
    CHECK(sfs_truncate(fs, "/t1", 3) == 0);
    CHECK(sfs_truncate(fs, "/t1", 8) == 0);
    expect(fs, "/t1", "ABC\0\0\0\0\0", 8, 0);

    /* write past the end after shrinking: the gap reads as zeros */
    CHECK(sfs_create(fs, "/t2", 0644, NULL) == 0);
    CHECK(sfs_write(fs, "/t2", "ABCDEFGH", 8, 0) == 8);
    CHECK(sfs_truncate(fs, "/t2", 3) == 0);
    CHECK(sfs_write(fs, "/t2", "Z", 1, 6) == 1);
    expect(fs, "/t2", "ABC\0\0\0Z", 7, 0);

    /* partial second block, then sparse extension into a third block */
    static char x[4096 + 5], zeros[8192];
    memset(x, 'x', sizeof(x));
    CHECK(sfs_create(fs, "/t3", 0644, NULL) == 0);
    CHECK(sfs_write(fs, "/t3", x, sizeof(x), 0) == (ssize_t)sizeof(x));
    CHECK(sfs_truncate(fs, "/t3", 4096 + 2) == 0);
    CHECK(sfs_truncate(fs, "/t3", 8192 + 10) == 0);
    expect(fs, "/t3", x, 4096 + 2, 0);
    expect(fs, "/t3", zeros, 8192 + 10 - (4096 + 2), 4096 + 2);

    /* shrink to an exact block boundary, then extend */
    static char y[8192];
    memset(y, 'y', sizeof(y));
    CHECK(sfs_create(fs, "/t4", 0644, NULL) == 0);
    CHECK(sfs_write(fs, "/t4", y, sizeof(y), 0) == (ssize_t)sizeof(y));
    CHECK(sfs_truncate(fs, "/t4", 4096) == 0);
    CHECK(sfs_truncate(fs, "/t4", 8192) == 0);
    expect(fs, "/t4", y, 4096, 0);
    expect(fs, "/t4", zeros, 4096, 4096);

    /* shrink to zero and grow again: every byte is zero */
    CHECK(sfs_truncate(fs, "/t4", 0) == 0);
    CHECK(sfs_truncate(fs, "/t4", 100) == 0);
    expect(fs, "/t4", zeros, 100, 0);
}

static void check_truncate_persisted(sfs_t *fs) {
    static char x[4096 + 2], zeros[8192];
    memset(x, 'x', sizeof(x));
    expect(fs, "/t1", "ABC\0\0\0\0\0", 8, 0);
    expect(fs, "/t2", "ABC\0\0\0Z", 7, 0);
    expect(fs, "/t3", x, sizeof(x), 0);
    expect(fs, "/t3", zeros, 8192 + 10 - (4096 + 2), 4096 + 2);
    expect(fs, "/t4", zeros, 100, 0);
    struct stat st;
    CHECK(sfs_getattr(fs, "/t3", &st) == 0 && st.st_size == 8192 + 10);
}

/* ---- D: concurrent creation of one name ------------------------------------------- */
typedef struct {
    sfs_t *fs;
    pthread_barrier_t *bar;
    int rc;
} create_arg_t;

static void *create_same(void *p) {
    create_arg_t *a = p;
    pthread_barrier_wait(a->bar);
    a->rc = sfs_create(a->fs, "/same", 0644, NULL);
    return NULL;
}

static int count_name(const char *name, const struct stat *st, void *arg) {
    (void)st;
    if (!strcmp(name, "same"))
        ++*(int *)arg;
    return 0;
}

static void test_create_race(sfs_t *fs) {
    enum { T = 8 };
    for (int round = 0; round < 40; ++round) {
        pthread_barrier_t bar;
        pthread_barrier_init(&bar, NULL, T);
        pthread_t th[T];
        create_arg_t a[T];
        for (int i = 0; i < T; ++i) {
            a[i] = (create_arg_t){fs, &bar, 0};
            CHECK(pthread_create(&th[i], NULL, create_same, &a[i]) == 0);
        }
        int ok = 0;
        for (int i = 0; i < T; ++i) {
            pthread_join(th[i], NULL);
            CHECK(a[i].rc == 0 || a[i].rc == -EEXIST);
            ok += a[i].rc == 0;
        }
        pthread_barrier_destroy(&bar);
        int entries = 0;
        CHECK(sfs_readdir(fs, "/", count_name, &entries) == 0);
        CHECK(ok == 1 && entries == 1);
        CHECK(sfs_unlink(fs, "/same") == 0);
    }
}

/* ---- D: unlink racing with writers (inode lifetime) -------------------------------- */
typedef struct {
    sfs_t *fs;
    _Atomic int *stop;
    int id;
} race_arg_t;

static void *victim_writer(void *p) {
    race_arg_t *a = p;
    static _Thread_local char buf[64 * 1024];
    memset(buf, 'v', sizeof(buf));
    while (!*a->stop) {
        ssize_t rc = sfs_write(a->fs, "/victim", buf, sizeof(buf), (off_t)a->id * 4096);
        CHECK(rc == (ssize_t)sizeof(buf) || rc == -ENOENT);
    }
    return NULL;
}

static void *victim_reader(void *p) {
    race_arg_t *a = p;
    static _Thread_local char buf[64 * 1024];
    while (!*a->stop) {
        ssize_t rc = sfs_read(a->fs, "/victim", buf, sizeof(buf), 0);
        CHECK(rc >= 0 || rc == -ENOENT);
        for (ssize_t i = 0; i < rc; ++i)
            CHECK(buf[i] == 'v' || buf[i] == 0); /* never another file's data */
    }
    return NULL;
}

static void *unlinker(void *p) {
    race_arg_t *a = p;
    while (!*a->stop) {
        int rc = sfs_unlink(a->fs, "/victim");
        CHECK(rc == 0 || rc == -ENOENT);
        rc = sfs_create(a->fs, "/victim", 0644, NULL);
        CHECK(rc == 0 || rc == -EEXIST);
        /* another file allocated in between, so freed blocks get reused quickly */
        rc = sfs_create(a->fs, "/churn", 0644, NULL);
        CHECK(rc == 0 || rc == -EEXIST);
        CHECK(sfs_write(a->fs, "/churn", "c", 1, 40000) == 1);
        CHECK(sfs_unlink(a->fs, "/churn") == 0);
    }
    return NULL;
}

static void test_unlink_race(sfs_t *fs) {
    static char keep[128 * 1024];
    memset(keep, 'K', sizeof(keep));
    CHECK(sfs_create(fs, "/keep", 0644, NULL) == 0);
    CHECK(sfs_write(fs, "/keep", keep, sizeof(keep), 0) == (ssize_t)sizeof(keep));
    CHECK(sfs_create(fs, "/victim", 0644, NULL) == 0);
    _Atomic int stop = 0;
    pthread_t th[6];
    race_arg_t a[6];
    for (int i = 0; i < 6; ++i)
        a[i] = (race_arg_t){fs, &stop, i};
    CHECK(pthread_create(&th[0], NULL, victim_writer, &a[0]) == 0);
    CHECK(pthread_create(&th[1], NULL, victim_writer, &a[1]) == 0);
    CHECK(pthread_create(&th[2], NULL, victim_writer, &a[2]) == 0);
    CHECK(pthread_create(&th[3], NULL, victim_reader, &a[3]) == 0);
    CHECK(pthread_create(&th[4], NULL, unlinker, &a[4]) == 0);
    CHECK(pthread_create(&th[5], NULL, victim_reader, &a[5]) == 0);
    sleep(3);
    stop = 1;
    for (int i = 0; i < 6; ++i)
        pthread_join(th[i], NULL);
    static char got[128 * 1024];
    CHECK(sfs_read(fs, "/keep", got, sizeof(got), 0) == (ssize_t)sizeof(got));
    CHECK(!memcmp(got, keep, sizeof(keep)));
    sfs_unlink(fs, "/victim");
}

/* ---- D: metadata write ordering ---------------------------------------------------- */
static void *allocator(void *p) {
    race_arg_t *a = p;
    char path[32];
    snprintf(path, sizeof(path), "/alloc%d", a->id);
    static _Thread_local char buf[4096];
    memset(buf, 'a', sizeof(buf));
    CHECK(sfs_create(a->fs, path, 0644, NULL) == 0);
    for (int i = 0; i < 64; ++i) /* 64 separate allocations, each followed by a sync */
        CHECK(sfs_write(a->fs, path, buf, sizeof(buf), (off_t)i * 4096) == 4096);
    return NULL;
}

static void test_metadata_order(sfs_t *fs) {
    for (int round = 0; round < 5; ++round) {
        pthread_t th[8];
        race_arg_t a[8];
        for (int i = 0; i < 8; ++i) {
            a[i] = (race_arg_t){fs, NULL, round * 8 + i};
            CHECK(pthread_create(&th[i], NULL, allocator, &a[i]) == 0);
        }
        for (int i = 0; i < 8; ++i)
            pthread_join(th[i], NULL);
        /* Every allocation has returned, so its bitmap and superblock writes are done:
         * the on-disk copies must equal memory, without waiting for unmount. */
        int fd = open(image, O_RDONLY);
        CHECK(fd >= 0);
        uint8_t blk[SFS_BLOCK_SIZE];
        raw_read(fd, fs->sb.bitmap_start, blk);
        CHECK(!memcmp(blk, fs->bitmap, SFS_BLOCK_SIZE));
        raw_read(fd, 0, blk);
        sfs_superblock_t on_disk;
        memcpy(&on_disk, blk, sizeof(on_disk));
        CHECK(on_disk.free_blocks == fs->sb.free_blocks);
        close(fd);
        for (int i = 0; i < 8; ++i) {
            char path[32];
            snprintf(path, sizeof(path), "/alloc%d", round * 8 + i);
            CHECK(sfs_unlink(fs, path) == 0);
        }
    }
}

/* ---- D: rename into its own subtree ------------------------------------------------ */
static void test_rename(sfs_t *fs) {
    CHECK(sfs_mkdir(fs, "/a", 0755) == 0);
    CHECK(sfs_mkdir(fs, "/a/b", 0755) == 0);
    CHECK(sfs_rename(fs, "/a", "/a/b/c") == -EINVAL);
    CHECK(sfs_rename(fs, "/a", "/a/x") == -EINVAL);
    CHECK(sfs_rename(fs, "/a/b", "/b") == 0);
    struct stat st;
    CHECK(sfs_getattr(fs, "/b", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(sfs_getattr(fs, "/a/b", &st) == -ENOENT);
    CHECK(sfs_rmdir(fs, "/b") == 0);
    CHECK(sfs_rmdir(fs, "/a") == 0);
}

/* ---- A: SCAN and LOOK movement through the real dispatcher ------------------------ */
typedef struct {
    virtual_disk_t *d;
    uint32_t block;
} rd_arg_t;

static void *read_one(void *p) {
    rd_arg_t *a = p;
    static _Thread_local uint8_t buf[SFS_BLOCK_SIZE];
    CHECK(vdisk_read(a->d, a->block, buf) == 0);
    return NULL;
}

static uint64_t dispatcher_movement(sched_policy_t p) {
    static const uint32_t book[] = {98, 183, 37, 122, 14, 124, 65, 67};
    vdisk_options_t o;
    vdisk_default_options(&o);
    o.policy = p;
    o.initial_head = 53;
    o.initial_direction = -1;
    o.start_paused = true;
    virtual_disk_t d;
    CHECK(vdisk_open(&d, image, 200, &o) == 0); /* the first 200 blocks, like the book */
    pthread_t th[8];
    rd_arg_t a[8];
    for (int i = 0; i < 8; ++i) {
        a[i] = (rd_arg_t){&d, book[i]};
        CHECK(pthread_create(&th[i], NULL, read_one, &a[i]) == 0);
        while (vdisk_pending(&d) < (size_t)i + 1) /* fix the arrival order */
            usleep(1000);
    }
    vdisk_resume(&d);
    for (int i = 0; i < 8; ++i)
        pthread_join(th[i], NULL);
    uint64_t move = d.metrics.total_seek;
    vdisk_close(&d);
    return move;
}

/* Runs one named test on a fresh image (used to show each defect on the old code). */
static int run_one(const char *name) {
    sfs_t fs;
    mount_fs(&fs, SCHED_DEADLINE_LOCAL);
    if (!strcmp(name, "truncate")) {
        test_truncate(&fs);
        sfs_unmount_image(&fs);
        mount_fs(&fs, SCHED_FCFS);
        check_truncate_persisted(&fs);
    } else if (!strcmp(name, "create"))
        test_create_race(&fs);
    else if (!strcmp(name, "unlink"))
        test_unlink_race(&fs);
    else if (!strcmp(name, "meta"))
        test_metadata_order(&fs);
    else if (!strcmp(name, "rename"))
        test_rename(&fs);
    else {
        fprintf(stderr, "unknown test %s\n", name);
        return 2;
    }
    sfs_unmount_image(&fs);
    fsck();
    printf("%s: passed (%d checks)\n", name, checks);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s IMAGE [truncate|create|unlink|meta|rename]\n", argv[0]);
        return 2;
    }
    image = argv[1];
    if (argc == 3)
        return run_one(argv[2]);
    uint32_t base = fsck();
    sfs_t fs;
    mount_fs(&fs, SCHED_SSTF);
    test_truncate(&fs);
    sfs_unmount_image(&fs);
    fsck();
    mount_fs(&fs, SCHED_DEADLINE_LOCAL);
    check_truncate_persisted(&fs);
    test_create_race(&fs);
    test_rename(&fs);
    test_metadata_order(&fs);
    test_unlink_race(&fs);
    for (int i = 1; i <= 4; ++i) {
        char p[8];
        snprintf(p, sizeof(p), "/t%d", i);
        CHECK(sfs_unlink(&fs, p) == 0);
    }
    CHECK(sfs_unlink(&fs, "/keep") == 0);
    sfs_unmount_image(&fs);
    CHECK(fsck() == base); /* everything freed again, nothing leaked */
    CHECK(dispatcher_movement(SCHED_SCAN) == 236);
    CHECK(dispatcher_movement(SCHED_LOOK) == 208);
    CHECK(dispatcher_movement(SCHED_FCFS) == 640);
    printf("regression tests passed (%d checks)\n", checks);
    return 0;
}
