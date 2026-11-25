# Testing

| Command | What runs | Needs FUSE? |
|---|---|---|
| `make unit` | `tests/test_scheduler` (simulated clock), `tests/test_core`, `tests/test_regress` | no |
| `make test` | `make unit`, then `tests/system.sh` (real mounts) | yes |
| `make tsan` | core and regression tests built with ThreadSanitizer | no |
| `scripts/check_old_defects.sh` | the regression tests against the original `filesystem.c` (each defect should fail there) | no |
| `make bench` | deterministic scheduler simulation, `benchmark.csv` | no |
| `python3 scripts/experiments.py ...` | mounted experiments ([evaluation.md](evaluation.md)) | yes |

## What each test covers

**`tests/test_scheduler.c`** (deterministic; time is simulated):
textbook trace for FCFS 640, SSTF 236, SCAN 236, LOOK 208, deadline with long deadlines
(= SSTF) and zero deadlines (= FCFS); `scheduler_advance()` movement for each policy;
continuous nearby arrivals while a distant request waits (FCFS serves it next, SSTF and
Adaptive leave it until the stream ends, the deadline policy serves it 5.05 ms after
arrival, a distant write 50.05 ms after arrival); Adaptive's bound (0 picks in 10,000
decisions); deadline ordering across reads and writes; tie-breaking; the movement-versus-
wait trade-off; bounded-queue admission in the simulator; queue saturation (a producer
blocks on a full queue until a take frees space) and shutdown (a blocked producer gets
`-ESHUTDOWN`, queued requests are still drained).

**`tests/test_core.c`**: format, create, write, read, sparse truncate, 4 threads writing
64 KiB each at once, remount persistence.

**`tests/test_regress.c`** (review findings; an fsck-style image checker runs after each
phase and verifies no double-referenced block, bitmap = referenced blocks, free counters):
truncate within a block then extend (ABC + five zero bytes), write past the end after a
shrink, partial-block and exact-boundary cases, sparse extension, persistence across
remount; 8 threads creating one name (exactly one succeeds, one entry); unlink racing with
writers and readers for 3 s (readers never see another file's bytes, an unrelated file is
intact); 8 threads allocating at once with the on-disk bitmap and superblock equal to
memory afterwards; rename into its own subtree (`EINVAL`); SCAN 236, LOOK 208 and FCFS 640
through the real dispatcher.

**`tests/system.sh`**: a real FUSE mount under all six policies with `mkdir`, `echo`,
`cp`, `cmp`, `truncate`, `stat`, `mv`, `rm`, `rmdir`; the truncate check through the
mount; rename into a subtree refused; persistence of a truncated file across a clean
unmount and remount; a background (daemonized) mount with a relative image path.

## Environment notes

FUSE mount tests need `/dev/fuse` and `fusermount3`, and must not run as root. In
Docker: `--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined`.
ThreadSanitizer on recent kernels needs address randomization off; `make tsan` runs the
tests under `setarch -R`, which in Docker also needs `--security-opt seccomp=unconfined`.
