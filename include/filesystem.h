/*
 * filesystem.h
 *
 * Public API of the SchedFS inode/directory layer (mount, path ops, file I/O).
 */
#ifndef SCHEDFS_FILESYSTEM_H
#define SCHEDFS_FILESYSTEM_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "layout.h"
#include "virtual_disk.h"

/*
 * Locking (see docs/locking.md). Locks are always taken in this order:
 *
 *   ns_lock -> inode_locks[i] -> dir_locks[d] -> inode_alloc_lock -> inode_table_lock
 *     -> bitmap_io_lock -> bitmap_lock -> super_io_lock -> super_lock
 *     -> request-queue mutex -> per-request mutex
 *
 * ns_lock (reader-writer) protects the directory tree. create, mkdir, unlink, rmdir
 * and rename hold it for writing; a lookup holds it for reading only until it has the
 * target inode's lock, so a resolved inode number cannot be freed or reused while an
 * operation still uses it. Data operations hold the inode lock (read or write) for
 * their whole duration; unlink and rmdir take the inode write lock before freeing.
 */
typedef struct {
    virtual_disk_t disk;
    sfs_superblock_t sb;
    uint8_t *bitmap;
    pthread_rwlock_t ns_lock;
    pthread_mutex_t bitmap_lock, super_lock, inode_alloc_lock;
    /* Held across snapshot + write so metadata blocks reach the queue in snapshot order
     * and an older snapshot can never overwrite a newer one. */
    pthread_mutex_t bitmap_io_lock, super_io_lock;
    /* 32 inodes share one table block; serializes inode_write's read-modify-write. */
    pthread_mutex_t inode_table_lock;
    pthread_rwlock_t inode_locks[SFS_DEFAULT_INODES + 1u];
    pthread_mutex_t dir_locks[SFS_DEFAULT_INODES + 1u];
    bool verbose;
} sfs_t;

/* opts may be NULL for the defaults (FCFS, no modeled delay, no trace). */
int sfs_mount_image(sfs_t *fs, const char *path, const vdisk_options_t *opts, bool verbose);
void sfs_unmount_image(sfs_t *fs);
int sfs_getattr(sfs_t *fs, const char *path, struct stat *st);
int sfs_create(sfs_t *fs, const char *path, mode_t mode, uint32_t *ino);
int sfs_mkdir(sfs_t *fs, const char *path, mode_t mode);
ssize_t sfs_read(sfs_t *fs, const char *path, void *buf, size_t size, off_t off);
ssize_t sfs_write(sfs_t *fs, const char *path, const void *buf, size_t size, off_t off);
int sfs_truncate(sfs_t *fs, const char *path, off_t size);
int sfs_unlink(sfs_t *fs, const char *path);
int sfs_rmdir(sfs_t *fs, const char *path);
int sfs_rename(sfs_t *fs, const char *from, const char *to);
int sfs_statfs(sfs_t *fs, struct statvfs *st);
typedef int (*sfs_dir_cb)(const char *name, const struct stat *st, void *arg);
int sfs_readdir(sfs_t *fs, const char *path, sfs_dir_cb cb, void *arg);
#endif
