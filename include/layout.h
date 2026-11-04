/*
 * layout.h
 *
 * On-disk format: superblock, 128-byte inode and 64-byte directory entry layouts.
 */
#ifndef SCHEDFS_LAYOUT_H
#define SCHEDFS_LAYOUT_H

#include <stdint.h>

#define SFS_MAGIC           0x53465331u
#define SFS_VERSION         1u
#define SFS_BLOCK_SIZE      4096u
#define SFS_DEFAULT_INODES  256u
#define SFS_DIRECT_BLOCKS   12u
#define SFS_INDIRECT_BLOCKS (SFS_BLOCK_SIZE / sizeof(uint32_t))
#define SFS_NAME_MAX        59u
#define SFS_ROOT_INO        1u
#define SFS_MIN_BLOCKS      64u

typedef struct __attribute__((packed)) {
    uint32_t magic, version, block_size, total_blocks;
    uint32_t free_blocks, total_inodes, free_inodes;
    uint32_t bitmap_start, bitmap_blocks, inode_start, inode_blocks;
    uint32_t data_start, root_inode, checksum;
    uint8_t reserved[72];
} sfs_superblock_t;

typedef struct __attribute__((packed)) {
    uint16_t mode, links;
    uint32_t uid, gid;
    uint64_t size, atime, mtime, ctime;
    uint32_t direct[SFS_DIRECT_BLOCKS];
    uint32_t indirect, generation;
    uint8_t reserved[28];
} sfs_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t ino;
    char name[60];
} sfs_dirent_t;

_Static_assert(sizeof(sfs_superblock_t) == 128, "superblock header must be 128 bytes");
_Static_assert(sizeof(sfs_inode_t) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(sfs_dirent_t) == 64, "directory entry must be 64 bytes");

#endif
