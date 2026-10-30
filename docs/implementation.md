# Implementation notes

- `mkfs/mkfs.c` is the only tool that initializes the image with direct writes.
- `src/storage/virtual_disk.c` owns the file descriptor and dispatcher.
- `src/sync/request_queue.c` implements the bounded multi-producer/single-consumer queue.
- `src/sched/scheduler.c` implements FCFS, SSTF, SCAN and adaptive selection with deterministic arrival/ID tie-breaking.
- `src/fs/filesystem.c` implements path lookup, directories, inodes, allocation, sparse reads, direct/indirect mapping, truncation and persistence.
- `src/fuse/fuse_main.c` maps libfuse3 callbacks to the internal API and returns negative errno values.

The adaptive score is `alpha * distance / total_blocks - beta * wait / max_wait`. A zero maximum wait produces a zero waiting term. With beta zero its ordering is SSTF.
