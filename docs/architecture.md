# Architecture

SchedFS uses a strict layered request path:

`Linux VFS -> libfuse callbacks -> inode/directory layer -> block mapping -> request queue -> scheduler -> dispatcher -> disk.img`

After the mount-time bootstrap read, only the dispatcher calls `pread` or `pwrite`. A
request owns its caller-provided 4096-byte buffer until its per-request condition
variable is signalled. The bounded queue uses `not_empty` and `not_full`; every
condition wait rechecks its predicate.

The default image uses 4096-byte blocks: block 0 is the superblock, the next blocks hold
the allocation bitmap, the following eight blocks hold 256 fixed 128-byte inodes, and the
rest are data blocks. Direct and single-indirect addressing provide 1036 logical blocks
per file.

## Where a file operation enters the scheduler

1. The kernel sends a FUSE request; libfuse runs the callback on one of its worker
   threads (`src/fuse/fuse_main.c`). The callback records the caller's pid for the trace
   (`sfs_request_client`, measurement only).
2. The file-system layer (`src/fs/filesystem.c`) resolves the path, locks the inode and
   turns the operation into block reads and writes (path lookup alone reads the root
   inode and directory blocks).
3. Each block access calls `vdisk_read`/`vdisk_write` (`src/storage/virtual_disk.c`),
   which creates a request (arrival time, block, read/write), puts it in the bounded
   queue and sleeps on the request's own condition variable.
4. The dispatcher thread calls `rq_take()`, which asks `scheduler_pick()` for the next
   request among those pending (`src/sched/scheduler.c`), serves it, applies the modeled
   service time if one is configured, records metrics and the trace row, and signals the
   caller.

The caller keeps its inode lock while it waits (step 3), so lock scope and queue order
both shape the latency of a file operation. See [locking.md](locking.md).

## Options that affect the block layer

| Option | Effect |
|---|---|
| `--sched=fcfs\|sstf\|scan\|look\|adaptive\|deadline` | scheduling policy ([scheduling.md](scheduling.md)) |
| `--read-deadline-ms`, `--write-deadline-ms`, `--deadline-ms` | deadline policy (defaults 0 ms and 2 ms) |
| `--alpha`, `--beta` | Adaptive weights |
| `--direct-io` | open the image with `O_DIRECT`, so requests reach the (virtual) disk instead of the host page cache |
| `--model-base-us`, `--model-us-per-block` | modeled service time: the dispatcher holds each request for at least `base + per_block x movement` |
| `--queue=N` | queue capacity (default 128) |
| `--trace=FILE` | one CSV row per served request (arrival, start, end, block, movement, queue depth, caller pid), written at unmount |

## Locking

See [locking.md](locking.md) for the full lock order, the inode-lifetime rule, and the
concurrency findings of the review. The format is educational: it has no journal or
crash-recovery pass, so a clean unmount is required for persistence guarantees.
