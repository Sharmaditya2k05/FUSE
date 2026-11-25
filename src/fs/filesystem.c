/*
 * filesystem.c
 *
 * Inodes, directories, path lookup, block allocation and direct/indirect mapping.
 * Every block access goes through vdisk_read/vdisk_write, i.e. through the scheduler.
 */
#include "filesystem.h"
#include "logger.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_sec(void) {
    return (uint64_t)time(NULL);
}

static int bit_get(const uint8_t *b, uint32_t n) {
    return (b[n / 8u] >> (n % 8u)) & 1u;
}

static void bit_set(uint8_t *b, uint32_t n, int v) {
    if (v)
        b[n / 8u] |= (uint8_t)(1u << (n % 8u));
    else
        b[n / 8u] &= (uint8_t)~(1u << (n % 8u));
}

static int block_read(sfs_t *fs, uint32_t b, void *p) {
    return vdisk_read(&fs->disk, b, p);
}

static int block_write(sfs_t *fs, uint32_t b, const void *p) {
    return vdisk_write(&fs->disk, b, p);
}

/*
 * Metadata write-back. The snapshot is taken and written while holding the *_io_lock,
 * so writes of the same metadata block are queued in snapshot order. Without it, two
 * threads could snapshot {A} and {A,B}, and the scheduler could serve the {A,B} write
 * first, leaving the older {A} on disk (it would only be repaired at unmount).
 */
static int sync_sb(sfs_t *fs) {
    uint8_t block[SFS_BLOCK_SIZE] = {0};
    pthread_mutex_lock(&fs->super_io_lock);
    pthread_mutex_lock(&fs->super_lock);
    memcpy(block, &fs->sb, sizeof(fs->sb));
    pthread_mutex_unlock(&fs->super_lock);
    int rc = block_write(fs, 0, block);
    pthread_mutex_unlock(&fs->super_io_lock);
    return rc;
}

static int sync_bitmap(sfs_t *fs) {
    int rc = 0;
    uint8_t block[SFS_BLOCK_SIZE];
    pthread_mutex_lock(&fs->bitmap_io_lock);
    for (uint32_t i = 0; i < fs->sb.bitmap_blocks && !rc; ++i) {
        /* Snapshot under the lock so the dispatcher never reads a bitmap being modified. */
        pthread_mutex_lock(&fs->bitmap_lock);
        memcpy(block, fs->bitmap + (size_t)i * SFS_BLOCK_SIZE, SFS_BLOCK_SIZE);
        pthread_mutex_unlock(&fs->bitmap_lock);
        rc = block_write(fs, fs->sb.bitmap_start + i, block);
    }
    pthread_mutex_unlock(&fs->bitmap_io_lock);
    return rc;
}

static int inode_read(sfs_t *fs, uint32_t ino, sfs_inode_t *out) {
    if (!ino || ino > fs->sb.total_inodes)
        return -EINVAL;
    uint32_t per = SFS_BLOCK_SIZE / (uint32_t)sizeof(*out), idx = ino - 1u;
    uint8_t block[SFS_BLOCK_SIZE];
    int rc = block_read(fs, fs->sb.inode_start + idx / per, block);
    if (!rc)
        memcpy(out, block + (idx % per) * sizeof(*out), sizeof(*out));
    return rc;
}

static int inode_write(sfs_t *fs, uint32_t ino, const sfs_inode_t *in) {
    uint32_t per = SFS_BLOCK_SIZE / (uint32_t)sizeof(*in), idx = ino - 1u;
    uint8_t block[SFS_BLOCK_SIZE];
    pthread_mutex_lock(&fs->inode_table_lock);
    int rc = block_read(fs, fs->sb.inode_start + idx / per, block);
    if (!rc) {
        memcpy(block + (idx % per) * sizeof(*in), in, sizeof(*in));
        rc = block_write(fs, fs->sb.inode_start + idx / per, block);
    }
    pthread_mutex_unlock(&fs->inode_table_lock);
    return rc;
}

static int alloc_block(sfs_t *fs, uint32_t hint, uint32_t *out) {
    pthread_mutex_lock(&fs->bitmap_lock);
    uint32_t start = hint >= fs->sb.data_start ? hint : fs->sb.data_start, found = 0;
    for (uint32_t pass = 0; pass < 2 && !found; ++pass) {
        uint32_t a = pass ? fs->sb.data_start : start, z = pass ? start : fs->sb.total_blocks;
        for (uint32_t b = a; b < z; ++b)
            if (!bit_get(fs->bitmap, b)) {
                bit_set(fs->bitmap, b, 1);
                found = b;
                break;
            }
    }
    if (found) {
        pthread_mutex_lock(&fs->super_lock);
        fs->sb.free_blocks--;
        pthread_mutex_unlock(&fs->super_lock);
    }
    pthread_mutex_unlock(&fs->bitmap_lock);
    if (!found)
        return -ENOSPC;
    uint8_t zero[SFS_BLOCK_SIZE] = {0};
    int rc = sync_bitmap(fs);
    if (!rc)
        rc = sync_sb(fs);
    if (!rc)
        rc = block_write(fs, found, zero);
    if (rc) {
        pthread_mutex_lock(&fs->bitmap_lock);
        bit_set(fs->bitmap, found, 0);
        pthread_mutex_unlock(&fs->bitmap_lock);
        pthread_mutex_lock(&fs->super_lock);
        fs->sb.free_blocks++;
        pthread_mutex_unlock(&fs->super_lock);
        return rc;
    }
    *out = found;
    sfs_log(LOG_DEBUG, "allocated block %u", found);
    return 0;
}

static int free_block(sfs_t *fs, uint32_t b) {
    if (b < fs->sb.data_start || b >= fs->sb.total_blocks)
        return -EINVAL;
    pthread_mutex_lock(&fs->bitmap_lock);
    if (!bit_get(fs->bitmap, b)) {
        pthread_mutex_unlock(&fs->bitmap_lock);
        return -EIO;
    }
    bit_set(fs->bitmap, b, 0);
    pthread_mutex_unlock(&fs->bitmap_lock);
    pthread_mutex_lock(&fs->super_lock);
    fs->sb.free_blocks++;
    pthread_mutex_unlock(&fs->super_lock);
    int rc = sync_bitmap(fs);
    if (!rc)
        rc = sync_sb(fs);
    return rc;
}

static int inode_alloc(sfs_t *fs, mode_t mode, uint32_t *out) {
    pthread_mutex_lock(&fs->inode_alloc_lock);
    sfs_inode_t n;
    for (uint32_t i = 2; i <= fs->sb.total_inodes; ++i) {
        if (inode_read(fs, i, &n))
            continue;
        if (n.mode)
            continue;
        memset(&n, 0, sizeof(n));
        n.mode = (uint16_t)mode;
        n.links = 1;
        n.uid = (uint32_t)getuid();
        n.gid = (uint32_t)getgid();
        n.atime = n.mtime = n.ctime = now_sec();
        int rc = inode_write(fs, i, &n);
        if (!rc) {
            pthread_mutex_lock(&fs->super_lock);
            fs->sb.free_inodes--;
            pthread_mutex_unlock(&fs->super_lock);
            rc = sync_sb(fs);
        }
        pthread_mutex_unlock(&fs->inode_alloc_lock);
        if (!rc)
            *out = i;
        return rc;
    }
    pthread_mutex_unlock(&fs->inode_alloc_lock);
    return -ENOSPC;
}

static int inode_release(sfs_t *fs, uint32_t ino) {
    sfs_inode_t z = {0};
    int rc = inode_write(fs, ino, &z);
    if (!rc) {
        pthread_mutex_lock(&fs->super_lock);
        fs->sb.free_inodes++;
        pthread_mutex_unlock(&fs->super_lock);
        rc = sync_sb(fs);
    }
    return rc;
}

static int bmap(sfs_t *fs, sfs_inode_t *n, uint32_t logical, bool create, uint32_t *physical) {
    if (logical >= SFS_DIRECT_BLOCKS + SFS_INDIRECT_BLOCKS)
        return -EFBIG;
    if (logical < SFS_DIRECT_BLOCKS) {
        if (!n->direct[logical] && create) {
            uint32_t allocated = 0;
            uint32_t hint = logical ? n->direct[logical - 1] : fs->sb.data_start;
            int rc = alloc_block(fs, hint, &allocated);
            if (rc)
                return rc;
            n->direct[logical] = allocated;
        }
        *physical = n->direct[logical];
        return 0;
    }
    if (!n->indirect) {
        if (!create) {
            *physical = 0;
            return 0;
        }
        uint32_t allocated = 0;
        int rc = alloc_block(fs, fs->sb.data_start, &allocated);
        if (rc)
            return rc;
        n->indirect = allocated;
    }
    uint32_t ptrs[SFS_INDIRECT_BLOCKS];
    int rc = block_read(fs, n->indirect, ptrs);
    if (rc)
        return rc;
    uint32_t idx = logical - SFS_DIRECT_BLOCKS;
    if (!ptrs[idx] && create) {
        rc = alloc_block(fs, n->indirect, &ptrs[idx]);
        if (rc)
            return rc;
        rc = block_write(fs, n->indirect, ptrs);
        if (rc)
            return rc;
    }
    *physical = ptrs[idx];
    return 0;
}

static int dir_find(sfs_t *fs, uint32_t dir, const char *name, uint32_t *ino, uint64_t *offset) {
    sfs_inode_t n;
    int rc = inode_read(fs, dir, &n);
    if (rc)
        return rc;
    if (!S_ISDIR(n.mode))
        return -ENOTDIR;
    uint8_t block[SFS_BLOCK_SIZE];
    for (uint64_t off = 0; off < n.size; off += sizeof(sfs_dirent_t)) {
        uint32_t p;
        rc = bmap(fs, &n, (uint32_t)(off / SFS_BLOCK_SIZE), false, &p);
        if (rc)
            return rc;
        if (!p)
            continue;
        rc = block_read(fs, p, block);
        if (rc)
            return rc;
        sfs_dirent_t *e = (sfs_dirent_t *)(block + (off % SFS_BLOCK_SIZE));
        if (e->ino && !strncmp(e->name, name, sizeof(e->name))) {
            if (ino)
                *ino = e->ino;
            if (offset)
                *offset = off;
            return 0;
        }
    }
    return -ENOENT;
}

static int dir_add(sfs_t *fs, uint32_t dir, const char *name, uint32_t ino) {
    if (!name[0] || strlen(name) > SFS_NAME_MAX || strchr(name, '/'))
        return -ENAMETOOLONG;
    if (!dir_find(fs, dir, name, NULL, NULL))
        return -EEXIST;
    pthread_mutex_lock(&fs->dir_locks[dir]);
    sfs_inode_t n;
    int rc = inode_read(fs, dir, &n);
    uint64_t off = n.size;
    uint8_t block[SFS_BLOCK_SIZE];
    uint32_t p = 0;
    for (uint64_t x = 0; !rc && x < n.size; x += sizeof(sfs_dirent_t)) {
        rc = bmap(fs, &n, (uint32_t)(x / SFS_BLOCK_SIZE), false, &p);
        if (rc || !p)
            continue;
        rc = block_read(fs, p, block);
        if (rc)
            break;
        sfs_dirent_t *e = (sfs_dirent_t *)(block + (x % SFS_BLOCK_SIZE));
        if (!e->ino) {
            off = x;
            break;
        }
    }
    if (!rc) {
        rc = bmap(fs, &n, (uint32_t)(off / SFS_BLOCK_SIZE), true, &p);
    }
    if (!rc) {
        rc = block_read(fs, p, block);
        sfs_dirent_t *e = (sfs_dirent_t *)(block + (off % SFS_BLOCK_SIZE));
        memset(e, 0, sizeof(*e));
        e->ino = ino;
        strncpy(e->name, name, SFS_NAME_MAX);
        rc = block_write(fs, p, block);
    }
    if (!rc) {
        if (off == n.size)
            n.size += sizeof(sfs_dirent_t);
        n.mtime = n.ctime = now_sec();
        rc = inode_write(fs, dir, &n);
    }
    pthread_mutex_unlock(&fs->dir_locks[dir]);
    return rc;
}

static int dir_remove(sfs_t *fs, uint32_t dir, const char *name, uint32_t *removed) {
    pthread_mutex_lock(&fs->dir_locks[dir]);
    uint64_t off;
    uint32_t ino;
    int rc = dir_find(fs, dir, name, &ino, &off);
    if (!rc) {
        sfs_inode_t n;
        rc = inode_read(fs, dir, &n);
        uint32_t p;
        if (!rc)
            rc = bmap(fs, &n, (uint32_t)(off / SFS_BLOCK_SIZE), false, &p);
        uint8_t block[SFS_BLOCK_SIZE];
        if (!rc)
            rc = block_read(fs, p, block);
        if (!rc) {
            memset(block + (off % SFS_BLOCK_SIZE), 0, sizeof(sfs_dirent_t));
            rc = block_write(fs, p, block);
        }
        if (!rc) {
            n.mtime = n.ctime = now_sec();
            rc = inode_write(fs, dir, &n);
        }
    }
    pthread_mutex_unlock(&fs->dir_locks[dir]);
    if (!rc && removed)
        *removed = ino;
    return rc;
}

static int resolve(sfs_t *fs, const char *path, uint32_t *ino) {
    if (!path || path[0] != '/')
        return -EINVAL;
    if (!strcmp(path, "/")) {
        *ino = SFS_ROOT_INO;
        return 0;
    }
    char *copy = strdup(path);
    if (!copy)
        return -ENOMEM;
    uint32_t cur = SFS_ROOT_INO;
    char *save = NULL, *part = strtok_r(copy, "/", &save);
    int rc = 0;
    while (part && !rc) {
        rc = dir_find(fs, cur, part, &cur, NULL);
        part = strtok_r(NULL, "/", &save);
    }
    free(copy);
    if (!rc)
        *ino = cur;
    return rc;
}

static int parent_name(sfs_t *fs, const char *path, uint32_t *parent, char name[60]) {
    if (!path || path[0] != '/' || !strcmp(path, "/"))
        return -EINVAL;
    char *copy = strdup(path);
    if (!copy)
        return -ENOMEM;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';
    char *slash = strrchr(copy, '/');
    if (strlen(slash + 1) > SFS_NAME_MAX) {
        free(copy);
        return -ENAMETOOLONG;
    }
    strcpy(name, slash + 1);
    if (slash == copy)
        slash[1] = '\0';
    else
        *slash = '\0';
    int rc = resolve(fs, copy, parent);
    free(copy);
    return rc;
}

static int free_inode_blocks(sfs_t *fs, uint32_t ino, sfs_inode_t *n, uint32_t first) {
    uint32_t total = (uint32_t)((n->size + SFS_BLOCK_SIZE - 1u) / SFS_BLOCK_SIZE);
    int rc = 0;
    uint32_t ptrs[SFS_INDIRECT_BLOCKS] = {0};
    if (n->indirect) {
        rc = block_read(fs, n->indirect, ptrs);
        if (rc)
            return rc;
    }
    for (uint32_t l = first; l < total; ++l) {
        uint32_t p = l < SFS_DIRECT_BLOCKS ? n->direct[l] : ptrs[l - SFS_DIRECT_BLOCKS];
        if (p) {
            rc = free_block(fs, p);
            if (rc)
                return rc;
            if (l < SFS_DIRECT_BLOCKS)
                n->direct[l] = 0;
            else
                ptrs[l - SFS_DIRECT_BLOCKS] = 0;
        }
    }
    if (n->indirect) {
        bool any = false;
        for (size_t i = 0; i < SFS_INDIRECT_BLOCKS; ++i)
            if (ptrs[i]) {
                any = true;
                break;
            }
        if (any)
            rc = block_write(fs, n->indirect, ptrs);
        else {
            rc = free_block(fs, n->indirect);
            n->indirect = 0;
        }
    }
    if (!rc)
        rc = inode_write(fs, ino, n);
    return rc;
}

int sfs_mount_image(sfs_t *fs, const char *path, const vdisk_options_t *opts, bool verbose) {
    memset(fs, 0, sizeof(*fs));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    uint8_t first[SFS_BLOCK_SIZE];
    ssize_t n = pread(fd, first, sizeof(first), 0);
    close(fd);
    if (n != (ssize_t)sizeof(first))
        return -EIO;
    memcpy(&fs->sb, first, sizeof(fs->sb));
    if (fs->sb.magic != SFS_MAGIC || fs->sb.version != SFS_VERSION ||
        fs->sb.block_size != SFS_BLOCK_SIZE || fs->sb.root_inode != SFS_ROOT_INO)
        return -EINVAL;
    if (fs->sb.total_inodes > SFS_DEFAULT_INODES || fs->sb.data_start >= fs->sb.total_blocks)
        return -EINVAL;
    pthread_rwlock_init(&fs->ns_lock, NULL);
    pthread_mutex_init(&fs->bitmap_lock, NULL);
    pthread_mutex_init(&fs->super_lock, NULL);
    pthread_mutex_init(&fs->bitmap_io_lock, NULL);
    pthread_mutex_init(&fs->super_io_lock, NULL);
    pthread_mutex_init(&fs->inode_alloc_lock, NULL);
    pthread_mutex_init(&fs->inode_table_lock, NULL);
    for (uint32_t i = 0; i <= fs->sb.total_inodes; ++i) {
        pthread_rwlock_init(&fs->inode_locks[i], NULL);
        pthread_mutex_init(&fs->dir_locks[i], NULL);
    }
    vdisk_options_t defaults;
    if (!opts) {
        vdisk_default_options(&defaults);
        opts = &defaults;
    }
    int rc = vdisk_open(&fs->disk, path, fs->sb.total_blocks, opts);
    if (rc)
        return rc;
    fs->bitmap = calloc(fs->sb.bitmap_blocks, SFS_BLOCK_SIZE);
    if (!fs->bitmap) {
        vdisk_close(&fs->disk);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < fs->sb.bitmap_blocks && !rc; ++i)
        rc = block_read(fs, fs->sb.bitmap_start + i, fs->bitmap + (size_t)i * SFS_BLOCK_SIZE);
    fs->verbose = verbose;
    sfs_log(LOG_INFO, "mounted image: %u blocks, scheduler=%s", fs->sb.total_blocks,
            scheduler_name(opts->policy));
    return rc;
}

void sfs_unmount_image(sfs_t *fs) {
    if (!fs)
        return;
    sync_sb(fs);
    sync_bitmap(fs);
    vdisk_close(&fs->disk);
    const sfs_metrics_t *m = &fs->disk.metrics;
    sfs_log(LOG_INFO,
            "unmounted image: %llu requests, total seek %llu blocks, mean wait %.3f ms, "
            "max wait %.3f ms, max queue depth %llu",
            (unsigned long long)m->requests, (unsigned long long)m->total_seek,
            m->requests ? m->wait_sum_ms / (double)m->requests : 0.0, m->wait_max_ms,
            (unsigned long long)m->depth_max);
    free(fs->bitmap);
    for (uint32_t i = 0; i <= fs->sb.total_inodes; ++i) {
        pthread_rwlock_destroy(&fs->inode_locks[i]);
        pthread_mutex_destroy(&fs->dir_locks[i]);
    }
    pthread_mutex_destroy(&fs->inode_alloc_lock);
    pthread_mutex_destroy(&fs->inode_table_lock);
    pthread_mutex_destroy(&fs->bitmap_lock);
    pthread_mutex_destroy(&fs->super_lock);
    pthread_mutex_destroy(&fs->bitmap_io_lock);
    pthread_mutex_destroy(&fs->super_io_lock);
    pthread_rwlock_destroy(&fs->ns_lock);
}

/*
 * Look up `path` and lock its inode (read or write). The namespace lock is held only
 * until the inode lock is taken: after that, unlink/rmdir cannot free the inode until
 * this operation unlocks it, and no new lookup can find a name that is being removed.
 */
static int lookup_locked(sfs_t *fs, const char *path, bool write, uint32_t *ino,
                         sfs_inode_t *n) {
    pthread_rwlock_rdlock(&fs->ns_lock);
    int rc = resolve(fs, path, ino);
    if (!rc) {
        if (write)
            pthread_rwlock_wrlock(&fs->inode_locks[*ino]);
        else
            pthread_rwlock_rdlock(&fs->inode_locks[*ino]);
    }
    pthread_rwlock_unlock(&fs->ns_lock);
    if (rc)
        return rc;
    rc = inode_read(fs, *ino, n);
    if (!rc && !n->mode)
        rc = -ENOENT; /* defensive: never use a released inode */
    if (rc)
        pthread_rwlock_unlock(&fs->inode_locks[*ino]);
    return rc;
}

int sfs_getattr(sfs_t *fs, const char *path, struct stat *st) {
    uint32_t ino;
    sfs_inode_t n;
    int rc = lookup_locked(fs, path, false, &ino, &n);
    if (rc)
        return rc;
    pthread_rwlock_unlock(&fs->inode_locks[ino]);
    memset(st, 0, sizeof(*st));
    st->st_ino = ino;
    st->st_mode = n.mode;
    st->st_nlink = n.links;
    st->st_uid = n.uid;
    st->st_gid = n.gid;
    st->st_size = (off_t)n.size;
    st->st_blksize = SFS_BLOCK_SIZE;
    st->st_blocks = (blkcnt_t)((n.size + 511u) / 512u);
    st->st_atime = (time_t)n.atime;
    st->st_mtime = (time_t)n.mtime;
    st->st_ctime = (time_t)n.ctime;
    return 0;
}

int sfs_create(sfs_t *fs, const char *path, mode_t mode, uint32_t *out) {
    uint32_t parent, ino = 0;
    char name[60];
    pthread_rwlock_wrlock(&fs->ns_lock);
    int rc = parent_name(fs, path, &parent, name);
    if (!rc && !dir_find(fs, parent, name, NULL, NULL))
        rc = -EEXIST;
    if (!rc)
        rc = inode_alloc(fs, S_IFREG | (mode & 07777), &ino);
    if (!rc)
        rc = dir_add(fs, parent, name, ino);
    if (rc && ino)
        inode_release(fs, ino);
    pthread_rwlock_unlock(&fs->ns_lock);
    if (!rc && out)
        *out = ino;
    return rc;
}

int sfs_mkdir(sfs_t *fs, const char *path, mode_t mode) {
    uint32_t parent, ino = 0;
    char name[60];
    pthread_rwlock_wrlock(&fs->ns_lock);
    int rc = parent_name(fs, path, &parent, name);
    if (!rc && !dir_find(fs, parent, name, NULL, NULL))
        rc = -EEXIST;
    if (!rc)
        rc = inode_alloc(fs, S_IFDIR | (mode & 07777), &ino);
    if (!rc)
        rc = dir_add(fs, ino, ".", ino);
    if (!rc)
        rc = dir_add(fs, ino, "..", parent);
    if (!rc)
        rc = dir_add(fs, parent, name, ino);
    if (rc && ino) {
        sfs_inode_t n;
        if (!inode_read(fs, ino, &n))
            free_inode_blocks(fs, ino, &n, 0);
        inode_release(fs, ino);
    }
    pthread_rwlock_unlock(&fs->ns_lock);
    return rc;
}

ssize_t sfs_read(sfs_t *fs, const char *path, void *buf, size_t size, off_t off) {
    if (off < 0)
        return -EINVAL;
    uint32_t ino;
    sfs_inode_t n;
    int rc = lookup_locked(fs, path, false, &ino, &n);
    if (rc)
        return rc;
    if (S_ISDIR(n.mode)) {
        pthread_rwlock_unlock(&fs->inode_locks[ino]);
        return -EISDIR;
    }
    if ((uint64_t)off >= n.size) {
        pthread_rwlock_unlock(&fs->inode_locks[ino]);
        return 0;
    }
    if (size > n.size - (uint64_t)off)
        size = (size_t)(n.size - (uint64_t)off);
    size_t done = 0;
    uint8_t block[SFS_BLOCK_SIZE];
    while (done < size) {
        uint64_t pos = (uint64_t)off + done;
        uint32_t p;
        rc = bmap(fs, &n, (uint32_t)(pos / SFS_BLOCK_SIZE), false, &p);
        if (rc)
            break;
        size_t chunk = SFS_BLOCK_SIZE - (size_t)(pos % SFS_BLOCK_SIZE);
        if (chunk > size - done)
            chunk = size - done;
        if (p) {
            rc = block_read(fs, p, block);
            if (rc)
                break;
            memcpy((char *)buf + done, block + pos % SFS_BLOCK_SIZE, chunk);
        } else
            memset((char *)buf + done, 0, chunk);
        done += chunk;
    }
    pthread_rwlock_unlock(&fs->inode_locks[ino]);
    return rc ? rc : (ssize_t)done;
}

ssize_t sfs_write(sfs_t *fs, const char *path, const void *buf, size_t size, off_t off) {
    if (off < 0 ||
        (uint64_t)off + size > (uint64_t)(SFS_DIRECT_BLOCKS + SFS_INDIRECT_BLOCKS) * SFS_BLOCK_SIZE)
        return -EFBIG;
    uint32_t ino;
    sfs_inode_t n;
    int rc = lookup_locked(fs, path, true, &ino, &n);
    if (rc)
        return rc;
    if (S_ISDIR(n.mode)) {
        pthread_rwlock_unlock(&fs->inode_locks[ino]);
        return -EISDIR;
    }
    size_t done = 0;
    uint8_t block[SFS_BLOCK_SIZE];
    while (done < size) {
        uint64_t pos = (uint64_t)off + done;
        uint32_t p;
        rc = bmap(fs, &n, (uint32_t)(pos / SFS_BLOCK_SIZE), true, &p);
        if (rc)
            break;
        size_t chunk = SFS_BLOCK_SIZE - (size_t)(pos % SFS_BLOCK_SIZE);
        if (chunk > size - done)
            chunk = size - done;
        if (chunk != SFS_BLOCK_SIZE) {
            rc = block_read(fs, p, block);
            if (rc)
                break;
        }
        memcpy(block + pos % SFS_BLOCK_SIZE, (const char *)buf + done, chunk);
        rc = block_write(fs, p, block);
        if (rc)
            break;
        done += chunk;
    }
    if (done) {
        uint64_t end = (uint64_t)off + done;
        if (end > n.size)
            n.size = end;
        n.mtime = n.ctime = now_sec();
        int wr = inode_write(fs, ino, &n);
        if (!rc)
            rc = wr;
    }
    pthread_rwlock_unlock(&fs->inode_locks[ino]);
    return rc && done == 0 ? rc : (ssize_t)done;
}

int sfs_truncate(sfs_t *fs, const char *path, off_t size) {
    if (size < 0 ||
        (uint64_t)size > (uint64_t)(SFS_DIRECT_BLOCKS + SFS_INDIRECT_BLOCKS) * SFS_BLOCK_SIZE)
        return -EFBIG;
    uint32_t ino;
    sfs_inode_t n;
    int rc = lookup_locked(fs, path, true, &ino, &n);
    if (rc)
        return rc;
    if (S_ISDIR(n.mode)) {
        pthread_rwlock_unlock(&fs->inode_locks[ino]);
        return -EISDIR;
    }
    uint64_t keep = (uint64_t)size;
    if (keep < n.size && keep % SFS_BLOCK_SIZE) {
        /* Shrinking inside a block: zero the discarded tail of that block, so a later
         * extension (truncate up, or a write past the end) reads zeros, not old bytes.
         * Invariant: bytes of the last block beyond the file size are always zero. */
        uint32_t p = 0;
        rc = bmap(fs, &n, (uint32_t)(keep / SFS_BLOCK_SIZE), false, &p);
        if (!rc && p) {
            uint8_t block[SFS_BLOCK_SIZE];
            rc = block_read(fs, p, block);
            if (!rc) {
                size_t from = (size_t)(keep % SFS_BLOCK_SIZE);
                memset(block + from, 0, SFS_BLOCK_SIZE - from);
                rc = block_write(fs, p, block);
            }
        }
    }
    if (!rc && keep < n.size) {
        uint32_t first = (uint32_t)((keep + SFS_BLOCK_SIZE - 1u) / SFS_BLOCK_SIZE);
        rc = free_inode_blocks(fs, ino, &n, first);
    }
    if (!rc) {
        n.size = keep;
        n.mtime = n.ctime = now_sec();
        rc = inode_write(fs, ino, &n);
    }
    pthread_rwlock_unlock(&fs->inode_locks[ino]);
    return rc;
}

/* Free the blocks and the inode of a name already removed from its directory. Waits
 * for operations that still hold the inode (they resolved it before the removal). */
static int destroy_inode(sfs_t *fs, uint32_t ino) {
    pthread_rwlock_wrlock(&fs->inode_locks[ino]);
    sfs_inode_t n;
    int rc = inode_read(fs, ino, &n);
    if (!rc)
        rc = free_inode_blocks(fs, ino, &n, 0);
    if (!rc)
        rc = inode_release(fs, ino);
    pthread_rwlock_unlock(&fs->inode_locks[ino]);
    return rc;
}

int sfs_unlink(sfs_t *fs, const char *path) {
    uint32_t parent, ino = 0;
    char name[60];
    pthread_rwlock_wrlock(&fs->ns_lock);
    int rc = parent_name(fs, path, &parent, name);
    if (!rc)
        rc = dir_find(fs, parent, name, &ino, NULL);
    sfs_inode_t n;
    if (!rc)
        rc = inode_read(fs, ino, &n);
    if (!rc && S_ISDIR(n.mode))
        rc = -EISDIR;
    if (!rc)
        rc = dir_remove(fs, parent, name, NULL);
    pthread_rwlock_unlock(&fs->ns_lock);
    return rc ? rc : destroy_inode(fs, ino);
}

static int dir_empty(sfs_t *fs, uint32_t ino) {
    sfs_inode_t n;
    int rc = inode_read(fs, ino, &n);
    if (rc)
        return rc;
    for (uint64_t off = 0; off < n.size; off += sizeof(sfs_dirent_t)) {
        uint32_t p;
        rc = bmap(fs, &n, (uint32_t)(off / SFS_BLOCK_SIZE), false, &p);
        uint8_t b[SFS_BLOCK_SIZE];
        if (rc)
            return rc;
        if (!p)
            continue;
        rc = block_read(fs, p, b);
        if (rc)
            return rc;
        sfs_dirent_t *e = (sfs_dirent_t *)(b + off % SFS_BLOCK_SIZE);
        if (e->ino && strcmp(e->name, ".") && strcmp(e->name, ".."))
            return 0;
    }
    return 1;
}

int sfs_rmdir(sfs_t *fs, const char *path) {
    uint32_t parent, ino = 0;
    char name[60];
    pthread_rwlock_wrlock(&fs->ns_lock);
    int rc = parent_name(fs, path, &parent, name);
    if (!rc)
        rc = dir_find(fs, parent, name, &ino, NULL);
    sfs_inode_t n;
    if (!rc)
        rc = inode_read(fs, ino, &n);
    if (!rc && !S_ISDIR(n.mode))
        rc = -ENOTDIR;
    if (!rc) {
        int empty = dir_empty(fs, ino);
        rc = empty < 0 ? empty : (empty ? 0 : -ENOTEMPTY);
    }
    if (!rc)
        rc = dir_remove(fs, parent, name, NULL);
    pthread_rwlock_unlock(&fs->ns_lock);
    return rc ? rc : destroy_inode(fs, ino);
}

/* True if directory `dir` is `ancestor` or lies below it (walks ".." up to the root). */
static int is_within(sfs_t *fs, uint32_t dir, uint32_t ancestor) {
    for (int depth = 0; depth < (int)SFS_DEFAULT_INODES + 1; ++depth) {
        if (dir == ancestor)
            return 1;
        if (dir == SFS_ROOT_INO)
            return 0;
        uint32_t up;
        if (dir_find(fs, dir, "..", &up, NULL))
            return 0;
        dir = up;
    }
    return 0;
}

int sfs_rename(sfs_t *fs, const char *from, const char *to) {
    uint32_t fp = 0, tp = 0, ino = 0;
    char fn[60], tn[60];
    bool target_added = false;
    pthread_rwlock_wrlock(&fs->ns_lock);
    int rc = parent_name(fs, from, &fp, fn);
    if (!rc)
        rc = parent_name(fs, to, &tp, tn);
    if (!rc)
        rc = dir_find(fs, fp, fn, &ino, NULL);
    if (!rc && !dir_find(fs, tp, tn, NULL, NULL))
        rc = -EEXIST;
    sfs_inode_t n;
    bool is_dir = false;
    if (!rc && !inode_read(fs, ino, &n))
        is_dir = S_ISDIR(n.mode);
    if (!rc && is_dir && is_within(fs, tp, ino))
        rc = -EINVAL; /* a directory cannot be moved into itself or its own subtree */
    if (!rc) {
        rc = dir_add(fs, tp, tn, ino);
        target_added = !rc;
    }
    if (!rc && fp != tp && is_dir) {
        uint32_t old_parent = 0;
        rc = dir_remove(fs, ino, "..", &old_parent);
        if (!rc)
            rc = dir_add(fs, ino, "..", tp);
        if (rc && old_parent)
            dir_add(fs, ino, "..", old_parent);
    }
    if (!rc) {
        rc = dir_remove(fs, fp, fn, NULL);
        if (rc)
            dir_remove(fs, tp, tn, NULL);
    } else if (target_added)
        dir_remove(fs, tp, tn, NULL);
    pthread_rwlock_unlock(&fs->ns_lock);
    return rc;
}

int sfs_statfs(sfs_t *fs, struct statvfs *st) {
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = SFS_BLOCK_SIZE;
    pthread_mutex_lock(&fs->super_lock);
    st->f_blocks = fs->sb.total_blocks - fs->sb.data_start;
    st->f_bfree = st->f_bavail = fs->sb.free_blocks;
    st->f_files = fs->sb.total_inodes;
    st->f_ffree = st->f_favail = fs->sb.free_inodes;
    pthread_mutex_unlock(&fs->super_lock);
    st->f_namemax = SFS_NAME_MAX;
    return 0;
}

int sfs_readdir(sfs_t *fs, const char *path, sfs_dir_cb cb, void *arg) {
    uint32_t ino;
    /* Hold the namespace lock for reading during the listing, so entries are not
     * added or removed while they are being read. */
    pthread_rwlock_rdlock(&fs->ns_lock);
    int rc = resolve(fs, path, &ino);
    sfs_inode_t n;
    if (!rc)
        rc = inode_read(fs, ino, &n);
    if (!rc && !S_ISDIR(n.mode))
        rc = -ENOTDIR;
    for (uint64_t off = 0; !rc && off < n.size; off += sizeof(sfs_dirent_t)) {
        uint32_t p;
        uint8_t b[SFS_BLOCK_SIZE];
        rc = bmap(fs, &n, (uint32_t)(off / SFS_BLOCK_SIZE), false, &p);
        if (rc || !p)
            continue;
        rc = block_read(fs, p, b);
        if (rc)
            break;
        sfs_dirent_t *e = (sfs_dirent_t *)(b + off % SFS_BLOCK_SIZE);
        if (e->ino) {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = e->ino;
            sfs_inode_t child;
            if (!inode_read(fs, e->ino, &child))
                st.st_mode = child.mode;
            if (cb(e->name, &st, arg))
                break;
        }
    }
    pthread_rwlock_unlock(&fs->ns_lock);
    return rc;
}
