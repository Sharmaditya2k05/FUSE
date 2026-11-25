# SchedFS — How to Showcase the Project to Faculty

A 10-minute demo plan, the exact commands, what to say at each step, and answers to the questions faculty are likely to ask. Every number below comes from the measured results in `results/mounted/summary.md`, the tuning tables in `docs/evaluation.md`, or the deterministic tests, as recorded in the project report. Facts match the `feature/deadline-scheduler` branch.

---

## 1. What you are showing (the 30-second pitch)

**Aim:** *Build a mountable Linux filesystem that keeps everyday file operations responsive during competing bulk transfers by scheduling storage requests with deadlines, while maintaining useful throughput for background work.*

> "SchedFS is a responsiveness-focused file system with deadline-aware request scheduling. It is a real Linux file system — you can `mkdir`, `cp` and `cat` inside it — and every block it reads or writes waits in one queue that a single dispatcher thread serves. Our deadline policy gives reads a deadline of arrival + 0 ms and writes arrival + 2 ms; overdue requests go first, otherwise the nearest block. On one Windows + WSL 2 machine, small reads during background writes had a 22–56 % lower 99th-percentile latency than under our own FCFS policy, with background throughput within −13 % to +8 % of FCFS. The costs: SSTF keeps more throughput when seek time is modeled, and small foreground *updates* with real I/O got 24 % slower."

Be precise about the contribution: deadline scheduling is an established approach (the Linux deadline I/O scheduler also gives reads and writes separate expiry times). Our work is implementing it, together with FCFS, SSTF, SCAN, LOOK and a heuristic Adaptive policy, in a working file system's request path, and evaluating all of them on mounted workloads with the same instrumentation.

OS topics in one project: **file systems** (superblock, bitmap, inodes, indexed allocation), **disk scheduling** (six policies, starvation, deadlines), **synchronization** (bounded producer–consumer queue, condition variables, reader–writer locks, a namespace lock, one global lock order) and **performance measurement** (traces, percentiles, repetitions, measured versus modeled).

---

## 2. Before the day (one-time setup, needs internet)

You need a Linux environment with FUSE. On your Windows laptop, Docker Desktop works; it is what every test and measurement in the report used.

**Build the environment once** (from a clone of the repository, on the `feature/deadline-scheduler` branch):

```bash
git clone https://github.com/Sharmaditya2k05/FUSE.git && cd FUSE
git checkout feature/deadline-scheduler
docker build -t schedfs-dev docker
```

**On the demo day, start it with** (from the repository folder):

```bash
docker run -it --rm --device /dev/fuse --cap-add SYS_ADMIN \
  --security-opt apparmor:unconfined --security-opt seccomp=unconfined \
  -v "$PWD:/src" -w /src schedfs-dev su student
make
```

(Run as `student`, not root — FUSE should not run as root and `make test` refuses to. `seccomp=unconfined` is only needed for `make tsan`.)

**Also open before you start:**

1. The report PDF (`report/OS_Project_Report.pdf`) — Table 22 (direct) and Table 23 (model) are the measured results; Fig. 30–32 are the charts.
2. `results/mounted/summary.md` — the full measured tables for every policy.
3. The repository page and, if you like, the interactive scheduler page (<https://sharmaditya2k05.github.io/FUSE/>).

**Day-before checklist**

- [ ] Docker Desktop starts and `schedfs-dev` runs; `make` builds four programs.
- [ ] Run `NOPAUSE=1 scripts/demo.sh` once end to end (about 1.5 minutes without pauses).
- [ ] Terminal font size large enough for a projector (Ctrl + mouse wheel).
- [ ] Print the report (title page, course details and supervisors match the approved synopsis).
- [ ] Every member knows which part they present (section 5).

---

## 3. The live demo, step by step (≈10 minutes)

The core of the demo is `scripts/demo.sh`: four steps with a pause between them (press Enter). Everything happens in a temporary folder that is removed at the end.

### Step 0 — The problem (30 s) · *talk*

Say: *"Importing a large dataset into a shared workspace while you open a config file or save notes: the small operation should not stall behind the bulk copy. A small file operation is a chain of about 11 block requests — path lookup, inode, data — and under FCFS each one waits behind the queued background writes."*

### Step 1 — A real file system (45 s) · `scripts/demo.sh`

```bash
scripts/demo.sh
```

It formats a 64 MiB image, mounts it with `--sched=deadline`, and runs `mkdir`, `echo`, `cat`, a 100 kB copy checked with `cmp`, `ls -l` and `df -h`.

Say: *"The kernel sends every call to our program through FUSE, and every block it reads or writes goes through one queue and one scheduler. The 100 kB file needs 25 blocks, more than the 12 direct pointers, so it also uses the single-indirect block."*

### Step 2 — Live: FCFS, then deadline (2 min)

Two background writers copy 2 MB files while a small file is opened, read and closed every 20 ms. Same workload and seed, fresh image for each policy, on the modeled disk (100 µs + 0.25 µs per block moved, on `O_DIRECT`). One line per second shows mean and worst small-file latency, then a summary with p50/p95/p99.

Say: *"Latency is measured end to end. Under FCFS a small read queues behind every background write. Under the deadline policy reads are overdue the moment they arrive, so they go first, oldest first, and the writes are ordered by locality until they reach their own 2 ms deadline."*

Values vary run to run — say so. A six-second live run is a demonstration, not the evidence.

### Step 3 — The repeated runs (1.5 min)

The script prints the `bg2_read` rows of `results/mounted/summary.md` (3 runs × 10 s per cell, about 1,500 foreground samples per cell). Read these out:

| 2 writers + small reads | FCFS | SSTF | Deadline |
|---|---|---|---|
| model: foreground p99 | 20.64 ms | 33.85 ms | **15.14 ms (−27 %)** |
| model: background | 3.40 MB/s | 4.56 MB/s | 3.66 MB/s |
| direct: foreground p99 | 4.98 ms | 11.29 ms | **3.89 ms (−22 %)** |
| direct: background | 21.84 MB/s | 22.44 MB/s | 19.05 MB/s (−13 %) |

Then point at the other cells in Table 22/23 of the report: with 4 writers, deadline's p99 is 30.76 vs 70.20 ms (model, −56 %) and 4.22 vs 8.73 ms (direct, −52 %); with 2 copiers, 26.93 vs 40.34 (−33 %) and 7.42 vs 10.64 ms (−30 %).

Say the cost straight away: *"SSTF keeps more background throughput on the modeled disk (4.56 vs 3.66 MB/s here), but its tail latency is the worst. With 4 writers on the modeled disk LOOK was slightly better than us (28.80 ms, 1.91 MB/s). And for small foreground updates with real I/O we were 24 % worse than FCFS (7.22 vs 5.83 ms) — a regression we report."*

### Step 4 — Persistence (30 s)

Clean unmount, mount again under another policy: the files are still there and identical.

Say: *"There is no journal, so a clean unmount is required; the regression tests also check persistence across remounts, including after a truncate."*

### Step 5 — Proof it is tested (1.5 min) · *terminal*

```bash
make unit                      # scheduler (simulated clock), core and regression tests
scripts/check_old_defects.sh   # each fixed defect fails on the original code
```

Expected: `scheduler tests passed (119 checks)`, `core persistence and concurrency tests passed`, `regression tests passed`; then `old code FAILED` for `truncate`, `create`, `unlink` and `rename`, and `meta: old code PASSED (defect not reproduced)`.

Say: *"The regression tests reproduce the review findings: a truncate that kept old bytes, an unlink that freed an inode another thread was still reading, two creates of the same name both succeeding, and a rename into a folder's own subtree. They fail on the original code and pass now. ThreadSanitizer is clean in repeated runs of the core and regression tests."* (If there is time, `make test` adds real mounts under all six policies; the line "Ignoring invalid max threads value…" is a harmless libfuse 3.14 message.)

### Step 6 — Close (30 s)

Say: *"Limitations: all measurements are from one Windows + WSL 2 machine; no physical hard disk was measured, and the modeled seek time is a model. The policy helps reads, not updates; deadlines are not real-time guarantees; FUSE makes every operation much slower than a kernel file system. No journal, files up to about 4 MiB."*

---

## 4. If something goes wrong (backup plan)

| Problem | What to do |
|---|---|
| `fuse: device not found` / mount fails in Docker | You forgot `--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined`. Restart the container with the full command. |
| `mountpoint is not empty` or "Transport endpoint is not connected" | `fusermount3 -u mnt` (or `fusermount3 -uz mnt`), then mount again. `demo.sh` cleans up its own temporary folder. |
| `open (…): File exists` from `mkfs.schedfs` | That is the safety check working. Use a new image name. |
| The live step 2 shows a small difference | Expected for 6 s runs. Go straight to step 3: the claims come from the 3 × 10 s runs. |
| Docker will not start at all | Present from the report: Fig. 19–24 are terminal captures of build, format, mount, file operations, unmount and verbose logging (captured at the earlier commit `a7e489c`); Table 22, Table 23 and Fig. 30–32 are the measured results; Table 25 lists findings A–E. |

---

## 5. Suggested split for four presenters

(Presentation split chosen by the team; it does not follow the module allocation in the report.)

| Presenter | Talks about | Demo steps |
|---|---|---|
| Prankit Goel | Aim and problem, FUSE adapter and options, formatter, on-disk layout, mount life cycle (the `fork()`/`op_init` fix) | 0, 1 |
| Saanvi Grover | Request queue, the six policies, the deadline rule and its bound, the movement model (SCAN vs LOOK), Adaptive's bounded age term, dispatcher and modeled service time | 2 |
| Aditya Sharma | File-system core: inodes, directories, allocation, truncate fix, namespace lock and inode lifetime, unlink/create/rename fixes | 4, part of 5 |
| Aaradhya Shankar Kulashri | Workload and experiments, measured vs modeled, results and trade-offs, tuning, tests and ThreadSanitizer, limitations | 3, 5, 6 |

---

## 6. Questions faculty are likely to ask (and good answers)

**Why FUSE?**
The whole file system runs as an ordinary process: we can trace every request, change the scheduler with a flag, and a bug cannot crash the kernel. The kernel still delivers real system calls from real programs. The cost is extra context switches per operation, which is why we compare policies inside SchedFS rather than claiming SchedFS is faster than a kernel file system. (The host Linux file system had a p99 of about 0.05 ms on the same workload, with its page cache and no FUSE — a reference only.)

**What is modeled and what is measured?**
Measured: end-to-end latency of every small operation (the workload's own clock), background bytes per second, and per block request its arrival, start and end, queue depth and caller (`--trace`). Modeled: "movement" (block distance) and, in the *model* configuration, the service time of each request (100 µs + 0.25 µs per block moved). The *direct* configuration has no modeled delay: requests reach the WSL 2 virtual disk with `O_DIRECT`. The simulator `schedfs-bench` is a model end to end and is labelled as such. Reduced movement is never presented as a speed-up.

**Why might the policy help?**
A small file operation is a chain of about 11 dependent block requests, and each waits behind whatever the queue holds. Under FCFS each read waits for the writes ahead of it; under SSTF a distant read can wait for many nearby writes. The deadline policy serves reads first and keeps the writes ordered by locality, so reads stop queueing behind writes while writes keep much of SSTF's short movement.

**Can a request still wait a long time?**
Yes, within limits. The dispatcher is non-preemptive, so an overdue request still waits for the request in service and for requests with earlier deadlines — at most queue capacity − 1, plus producers blocked at admission. A write can wait its 2 ms deadline plus that. A file *operation* can take longer than any one deadline because it is a chain of requests. Deadlines are not real-time guarantees.

**What happens under overload?**
Every request becomes overdue, so the policy becomes earliest-deadline-first: FCFS within each type. Queues and waits grow as they would under FCFS; the bounded queue (128) blocks producers instead of using unbounded memory. Nothing is dropped.

**What is the throughput trade-off?**
Against FCFS it is small: −13 % to +6 % in the direct configuration (mostly within run-to-run ranges) and −9 % to +15 % in the model configuration. Against SSTF, deadline gives up 20–60 % of background throughput on the modeled disk, because it uses locality only within the 2 ms window; in return its foreground p99 was 30–79 % lower than SSTF's.

**What does this improve over, exactly?**
Over SchedFS's own FCFS, SSTF, SCAN, LOOK and Adaptive policies, on the same mounted file system and identical workloads, for small foreground reads while background processes write or copy large files: 22–56 % lower p99 than FCFS in all six read cells, with non-overlapping run-to-run ranges. It does not improve small foreground updates with real I/O (24 % worse than FCFS), and with 4 writers on the modeled disk LOOK did slightly better. No claim is made about kernel file systems or kernel I/O schedulers, or about physical hard disks.

**How were the deadlines chosen?**
On separate tuning runs with seed 101, never used in the evaluation (seeds 1001–1003). A read deadline of 0 ms gave the lowest tail (mean p99/FCFS 0.69). An earlier 50 ms write deadline made foreground updates worse, so writes were re-tuned; 2 ms was chosen, because 1 ms cost 22 % of background throughput in one cell.

**Which claims are supported by which tests?**

| Claim | Evidence |
|---|---|
| Policies choose exactly the documented order | `tests/test_scheduler.c` (textbook trace, ties, deadline order; 119 checks, simulated clock) |
| SCAN 236 / LOOK 208 movement, the same in tests and mounted metrics | `test_scheduler`, `test_regress` (through the real dispatcher) |
| An overdue request is not bypassed by later arrivals of its type | `test_starvation`, `test_deadline_order` |
| Adaptive can starve; deadline does not in the same scenario | `test_starvation`, `test_adaptive_bound` (0 picks in 10,000 decisions) |
| truncate, create, unlink, rename defects fixed | `tests/test_regress.c`; `scripts/check_old_defects.sh` shows each failing on the old code |
| No data races found in concurrent tests | `make tsan` (supporting evidence, not proof) |
| Works as a mounted file system under every policy, persists across remount | `tests/system.sh` |
| Lower foreground tail latency than FCFS/SSTF, at a measured throughput cost | `scripts/experiments.py matrix`, `results/mounted/summary.md` |

**Does your Adaptive policy prevent starvation?**
No — and our first report wrongly said it did. Its age term is divided by the longest wait in the queue, so it is never worth more than β = 0.3. A request more than (β/α)·N ≈ 0.43·N blocks farther than nearby work never wins while nearby work keeps arriving (0 picks in 10,000 decisions). We kept Adaptive as a documented heuristic baseline.

**What is the difference between your SCAN and LOOK?**
They choose the same next request. SCAN is charged for travelling to the disk edge before it reverses (textbook trace: 53 + 183 = 236 blocks); LOOK reverses at the last request (39 + 169 = 208). One function, `scheduler_advance()`, now defines movement for the dispatcher, the simulator and the tests — before, the mounted metrics and the test disagreed (finding A). On the modeled disk SCAN's edge trips cost real (modeled) time, so its p99 was 82–119 ms with background load.

**How do you prevent deadlock?**
One global lock order: `ns_lock` → inode locks → directory locks → inode-alloc → inode-table → bitmap I/O → bitmap → superblock I/O → superblock → queue mutex → per-request mutex. The dispatcher only takes the queue mutex and a request's own mutex, so a thread that holds file-system locks while waiting for its I/O cannot block it.

**What was the hardest bug?**
`unlink` freed a file's blocks and inode without the inode lock that readers and writers hold, so a reader that had already looked up the name could read another file's data after the inode was reused. Now a lookup holds the namespace lock until it holds the inode lock, and `unlink` frees only under the inode's write lock. The regression test fails on the old code and passes now.

**What happens if the machine crashes while mounted?**
There is no journal, so the image could be inconsistent; a clean unmount is required. A crash can leak a block but the write order prevents two files from sharing one.

**How big is the code and how did you test it?**
4,205 non-blank, non-comment lines (C, shell, Python, Makefile; counted by `report/tools/count_loc.py`). 40 documented test cases: all 38 correctness cases pass; of the 2 performance goals, "deadline beats FCFS for foreground reads" is met and "deadline is not worse than FCFS for foreground updates" is not met with real I/O. ThreadSanitizer was clean in repeated runs (one early run failed once with an uncaptured message, which we report as unexplained).

---

## 7. Two-line summary to end on

> "SchedFS is a working Linux file system whose storage requests are all scheduled. With its deadline policy, small reads during background writes had a 22–56 % lower tail latency than under FCFS on one machine, at a small throughput cost against FCFS and a larger one against SSTF — and it did not help foreground updates, which we report."
