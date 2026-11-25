# Implementation notes

- `mkfs/mkfs.c` is the only tool that initializes the image with direct writes.
- `src/storage/virtual_disk.c` owns the file descriptor, the dispatcher thread, the
  optional `O_DIRECT` bounce buffer, the modeled service time and the per-request trace.
- `src/sync/request_queue.c` implements the bounded multi-producer/single-consumer queue.
- `src/sched/scheduler.c` implements FCFS, SSTF, SCAN, LOOK, Adaptive and Deadline
  selection with deterministic arrival/ID tie-breaking, and `scheduler_advance()`, the one
  definition of modeled head movement.
- `src/sched/sim.c` replays timed arrivals through the real scheduler with a simulated
  clock (used by `schedfs-bench` and `tests/test_scheduler.c`).
- `src/fs/filesystem.c` implements path lookup, directories, inodes, allocation, sparse
  reads, direct/indirect mapping, truncation and persistence, with the locking described
  in [locking.md](locking.md).
- `src/fuse/fuse_main.c` maps libfuse3 callbacks to the internal API, returns negative
  errno values and parses the mount options.
- `src/bench/workload.c` (`schedfs-workload`) generates the foreground/background
  workload for the mounted experiments; `scripts/run_mounted.sh`,
  `scripts/experiments.py`, `scripts/analyze_run.py` and `scripts/report_results.py`
  run and summarize them.

The Adaptive score is `alpha * distance / total_blocks - beta * wait / max_wait`. A zero
maximum wait produces a zero waiting term. With beta zero its ordering is SSTF. It has no
starvation guarantee (see [scheduling.md](scheduling.md)).

The deadline policy gives each request the deadline `arrival + read_deadline` or
`arrival + write_deadline`, serves the earliest overdue deadline first, and otherwise the
nearest block.
