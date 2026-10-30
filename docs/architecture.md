# Architecture

SchedFS uses a strict layered request path:

`Linux VFS -> libfuse callbacks -> inode/directory layer -> block mapping -> request queue -> scheduler -> dispatcher -> disk.img`

After the mount-time bootstrap read, only the dispatcher calls `pread` or `pwrite`. A request owns its caller-provided 4096-byte buffer until its per-request condition variable is signalled. The bounded queue uses `not_empty` and `not_full`; every condition wait rechecks its predicate.

The default image uses 4096-byte blocks: block 0 is the superblock, the next blocks hold the allocation bitmap, the following eight blocks hold 256 fixed 128-byte inodes, and the rest are data blocks. Direct and single-indirect addressing provide 1036 logical blocks per file.

## Locking

The intended acquisition order is directory mutex, inode rwlock, bitmap mutex, superblock mutex, then queue mutex. Queue I/O is not executed while the queue mutex is held. Each request has a separate completion mutex and condition. File reads take a shared inode lock; writes and truncation take an exclusive inode lock.

The format is educational: it has no journal or crash-recovery pass. A clean unmount is required for persistence guarantees.
