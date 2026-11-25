# Demo: setup, five-minute script, and prepared answers

## 1. Setup (once)

SchedFS needs Linux with FUSE 3. On Ubuntu 22.04/24.04 (or WSL 2 with FUSE):

```sh
sudo apt install -y build-essential pkg-config libfuse3-dev fuse3 python3 python3-matplotlib
git clone https://github.com/Sharmaditya2k05/FUSE.git && cd FUSE
make
make unit          # scheduler, core and regression tests (no mount needed)
make test          # the same plus real FUSE mounts under every policy
```

On Windows, Docker Desktop works. Build the image once (the Dockerfile is four lines:
`ubuntu:24.04`, then `apt install build-essential pkg-config libfuse3-dev fuse3 python3
python3-matplotlib python3-numpy git`, then `useradd -m student`), and run with FUSE
enabled, as a normal user:

```sh
docker run -it --rm --device /dev/fuse --cap-add SYS_ADMIN \
  --security-opt apparmor:unconfined -v "$PWD:/src" -w /src schedfs-dev su student
```

Do not run SchedFS as root. Everything below uses disposable images in temporary
directories; nothing else on the machine is touched.

## 2. The five-minute demo

Start it with:

```sh
scripts/demo.sh            # pauses between steps; NOPAUSE=1 to run straight through
```

| Time | Step | What to say |
|---|---|---|
| 0:00 | **The problem.** | "Importing a large dataset into a shared workspace while you open a config file or save notes: the small operation should not stall behind the bulk copy. SchedFS is a mountable file system that schedules its storage requests to keep small operations responsive." |
| 0:30 | **Step 1: normal file operations** (`mkdir`, `echo`, `cat`, a 100 kB copy checked with `cmp`, `ls`, `df`). | "It is a real file system: the kernel sends every call to our program through FUSE, and every block it reads or writes goes through one queue and one scheduler." |
| 1:15 | **Step 2: live run, FCFS then deadline.** Two background writers copy 2 MB files; every 20 ms a small file is opened, read and closed. One line per second shows the mean and worst small-file latency, then a summary with p50/p95/p99. | "Same workload, same seed, fresh image. The disk here has a modeled seek time so that request order matters; latency is measured end to end. Under FCFS a small read queues behind every background write; under the deadline policy reads are served first, oldest first, and the background writes are ordered by locality until they reach their own 2 ms deadline." |
| 3:00 | **Step 3: the longer repeated runs** (3 × 10 s per cell, from `results/mounted/summary.md`; charts in `results/mounted/*.png`). | Read the p95/p99 and MB/s for FCFS, SSTF and deadline on `bg2_read`, in both configurations. "Six seconds live can be noisy, so the claims come from these repeated runs." Point at the cost: background throughput compared with SSTF. |
| 4:00 | **Step 4: persistence.** Clean unmount, mount again under a different policy, the files are still there and identical. | "No journal, so a clean unmount is required; the regression tests also check persistence across remounts." |
| 4:30 | **Limitations and OS topics.** | See section 4 and [evaluation.md](evaluation.md). |

The live step prints, for example (values vary from run to run):

```
  t= 1.0s  small-file ops  50  mean  12.31 ms  worst  19.80 ms
```

## 3. Prepared answers

**Why use FUSE?** It lets the whole file system run as an ordinary process: we can trace
every request, change the scheduler with a flag, and a bug cannot crash the kernel. The
kernel still delivers real system calls (`open`, `read`, `write`, `rename` ...) from real
programs. The cost is extra context switches per operation, which is why we compare
policies inside SchedFS rather than claiming SchedFS is faster than a kernel file system.

**What is modeled and what is measured?** Measured: end-to-end latency of every small-file
operation (the workload's own clock), background bytes per second, and per block request
its arrival, start and end time, queue depth and caller (the trace). Modeled: "head
movement" (distance in block numbers) and, in the "model" configuration, the service time
of each request (100 µs + 0.25 µs per block moved), which stands in for a mechanical
disk. The "direct" configuration has no modeled delay: requests go to the (virtual) disk
with `O_DIRECT`. The simulator (`schedfs-bench`) is a model end to end.

**Why might the policy help?** A small-file operation is a chain of about 11 dependent
block reads (path lookup, inode, data). Each one waits behind whatever the queue holds.
With background writers, FCFS makes each read wait for the writes ahead of it, and SSTF
can make a distant read wait for many nearby writes. The deadline policy serves reads
first (read deadline 0 ms) and keeps the writes ordered by locality, so reads stop
queueing behind writes while the writes keep most of SSTF's short seeks.

**Can a request still wait a long time?** Yes, within limits. The dispatcher is
non-preemptive, so an overdue request still waits for the request in service and for any
request with an earlier deadline (at most queue capacity minus one, plus producers
blocked at admission). A write can wait up to its deadline (2 ms) plus that queue,
because reads go first. A file *operation* can take longer than any one deadline because
it is a chain of requests. Deadlines are not real-time guarantees.

**What happens under overload?** Every request becomes overdue, so the policy becomes
earliest-deadline-first: reads in arrival order, then writes in arrival order. Queues and
waits grow as they would under FCFS; the bounded queue (128) blocks producers instead of
using unbounded memory. Nothing is dropped.

**What is the throughput trade-off?** Measured in [evaluation.md](evaluation.md): against
SSTF, the deadline policy gives up 20-60% of background throughput on the modeled-seek
disk (SSTF keeps the head near the bulk transfer and starves the foreground); against
FCFS, background throughput was within −13% to +15%. On real `O_DIRECT` I/O on this
machine the differences between policies are small (1-15% against SSTF) and partly within
run-to-run variation, because seek distance costs little there.

**What does this improve over, exactly?** Over SchedFS's own FCFS and SSTF policies (and
SCAN, LOOK, Adaptive), on the same mounted file system and identical workloads: the p99
latency of small foreground *reads* while background processes write or copy large files
fell by 22-56% against FCFS and 30-79% against SSTF. Foreground *updates* improved on the
modeled disk (−16% against FCFS) but were 24% worse than FCFS at p99 on real `O_DIRECT`
I/O, and with four writers on the modeled disk LOOK was slightly better than the deadline
policy. It is not compared with, and makes no claim about, kernel file systems or kernel
I/O schedulers; the host-file-system rows are only a reference for how far FUSE plus this
design is from native.

**Which claims are supported by which tests?**

| Claim | Evidence |
|---|---|
| Policies choose exactly the documented order | `tests/test_scheduler.c` (textbook trace, ties, deadline order) |
| SCAN 236 / LOOK 208 movement, same in tests and mounted metrics | `test_scheduler`, `test_regress` (real dispatcher) |
| An overdue request is not bypassed by later arrivals of its type | `test_starvation`, `test_deadline_order` |
| Adaptive can starve; deadline does not in the same scenario | `test_starvation`, `test_adaptive_bound` |
| truncate, create, unlink, rename defects fixed | `tests/test_regress.c`; `scripts/check_old_defects.sh` shows each failing on the old code |
| No data races found in concurrent tests | `make tsan` (supporting evidence, not proof) |
| Works as a mounted file system under every policy, persists across remount | `tests/system.sh` |
| Lower foreground tail latency than FCFS/SSTF, at a measured throughput cost | `scripts/experiments.py matrix`, `results/mounted/summary.md` |

## 4. Mapping to the OS topics

| Topic | Where it is in SchedFS |
|---|---|
| File systems, directories, inodes, indexed allocation | `filesystem.c`: superblock, bitmap, 128-byte inodes, 12 direct + 1 indirect pointers, directory entries |
| Disk scheduling | `scheduler.c`: FCFS, SSTF, SCAN, LOOK, Adaptive, Deadline; `scheduler_advance()` movement model |
| Processes and threads | FUSE worker threads, one dispatcher thread, `fork()` when the daemon starts (dispatcher created after it), workload processes |
| Producer-consumer queues | `request_queue.c`: bounded queue, `not_full`/`not_empty` |
| Mutexes, condition variables, reader-writer locks | per-request completion condvar, inode rwlocks, namespace rwlock, metadata mutexes |
| Starvation and fairness | SSTF and Adaptive starvation, deadline bound, read/write deadlines |
| Deadlocks and synchronization | one global lock order ([locking.md](locking.md)); regression tests for races |
| Performance measurement | traces, percentiles, repetitions, modeled versus measured ([evaluation.md](evaluation.md)) |
