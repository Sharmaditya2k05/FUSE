#ifndef SCHEDFS_FILESYSTEM_H
#define SCHEDFS_FILESYSTEM_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "layout.h"
#include "virtual_disk.h"
typedef struct {
    virtual_disk_t disk;
    sfs_superblock_t sb;
    uint8_t *bitmap;
    pthread_mutex_t bitmap_lock, super_lock, inode_alloc_lock;
    pthread_rwlock_t inode_locks[SFS_DEFAULT_INODES + 1u];
    pthread_mutex_t dir_locks[SFS_DEFAULT_INODES + 1u];
    bool verbose;
} sfs_t;
int sfs_mount_image(sfs_t *fs, const char *path, sched_policy_t policy,
                    double alpha, double beta, bool verbose);
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
