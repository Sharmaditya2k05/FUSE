# SchedFS — How to Showcase the Project to Faculty

A 10-minute demo plan, the exact commands, what to say at each step, and answers to the questions faculty are likely to ask. Every number below comes from real runs recorded in the project report.

---

## 1. What you are showing (the 30-second pitch)

> "Operating-systems courses teach file systems and disk scheduling as separate chapters. SchedFS joins them. It is a real Linux file system — you can `mkdir`, `cp` and `cat` inside it — and every block it reads or writes goes into a queue. One dispatcher thread serves that queue using FCFS, SSTF, SCAN or an Adaptive policy that we choose when mounting. So textbook scheduling runs on real file-system traffic, and we can measure the difference."

Three OS topics in one project: **file systems** (superblock, bitmap, inodes, indexed allocation), **disk scheduling** (four policies) and **synchronization** (bounded producer–consumer queue, condition variables, reader–writer locks, lock ordering).

---

## 2. Before the day (one-time setup, needs internet)

You need a Linux environment with FUSE. On your Windows laptop, Docker Desktop works; it is what we used for the report.

**Build a ready-to-use demo image once:**

```bash
docker run -it --name schedfs-setup ubuntu:24.04 bash
```

Inside that container:

```bash
apt update && apt install -y build-essential pkg-config libfuse3-dev fuse3 git
useradd -m -s /bin/bash student
su - student -c "git clone https://github.com/Sharmaditya2k05/FUSE.git && cd FUSE && make"
exit
```

Back on Windows, save it as an image so the demo works offline:

```bash
docker commit schedfs-setup schedfs-demo
```

**On the demo day, start it with:**

```bash
docker run -it --rm --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined schedfs-demo su - student
```

Then `cd FUSE`. (Run as `student`, not root — FUSE should not run as root and `make test` refuses to.)

**Also open in browser tabs before you start:**

1. The live demo page: <https://sharmaditya2k05.github.io/FUSE/>
2. The repository: <https://github.com/Sharmaditya2k05/FUSE>
3. The report PDF (`OS_Project_Report.pdf`) — for the architecture diagram and result tables.

**Day-before checklist**

- [ ] Docker Desktop starts and `schedfs-demo` runs.
- [ ] Run the whole demo once end to end (section 3).
- [ ] Terminal font size large enough for a projector (Ctrl + mouse wheel).
- [ ] Browser zoom ~125 % on the demo page; light theme (Theme button) is easier to read on a projector.
- [ ] Fill in the course code and faculty name on the report's title page (top of `OS_Project_Report.tex`, then run `build.sh`).
- [ ] Every member knows which part they present (section 5).

---

## 3. The live demo, step by step (≈10 minutes)

### Step 1 — The idea and architecture (1 min) · *browser*

Open the demo page, scroll to **"The path of one `cat file.txt`"**.

Say: *"A system call goes through the Linux VFS and the FUSE kernel module to our process. Our file-system layer splits it into 4 KiB blocks. Each block becomes a request in a bounded queue. Only the dispatcher thread touches the disk image, and only after the scheduler picks the request."*

Point at the two highlighted boxes: **Request queue** and **Scheduler** — that is where the OS concepts live.

### Step 2 — Build, format and mount (1.5 min) · *terminal*

```bash
make
./mkfs.schedfs disk.img 16M
mkdir -p mnt
./schedfs disk.img mnt --sched=sstf
mount | grep schedfs
```

Expected: `Formatted disk.img: 4096 blocks, 256 inodes, data starts at block 10`, then `SchedFS: 4096 blocks x 4096 bytes, 256 inodes, scheduler=sstf`, and the mount listed as `fuse.schedfs`.

Say: *"Block 0 is the superblock, block 1 the free-block bitmap, blocks 2–9 hold 256 inodes of 128 bytes, and data starts at block 10."*

### Step 3 — Use it like a normal folder (1.5 min)

```bash
mkdir mnt/project
echo 'Operating Systems' > mnt/project/demo.txt
cat mnt/project/demo.txt
head -c 100000 /dev/urandom > /tmp/big.bin
cp /tmp/big.bin mnt/project/big.bin && cmp /tmp/big.bin mnt/project/big.bin && echo identical
ls -l mnt/project
df -h mnt
```

Say about the big file: *"100,000 bytes needs 25 blocks, but an inode has only 12 direct pointers, so this file uses the single-indirect block. `cmp` proves it reads back byte-for-byte."*

### Step 4 — Error handling (45 s)

```bash
rmdir mnt/project
mv mnt/project/demo.txt mnt/project/big.bin
cat mnt/project/missing.txt
```

Expected: `Directory not empty`, `File exists` (rename onto an existing name is refused — a documented limitation), `No such file or directory`.

Say: *"Our functions return negative `errno` values, so the shell prints the normal Linux messages."*

### Step 5 — Unmount and show the scheduler's work (1 min)

```bash
fusermount3 -u mnt
cat filesystem.log
```

The last line looks like `unmounted image: 899 requests, total seek 4385 blocks`.

Say: *"Every block request went through the scheduler. The dispatcher counted requests and total head movement. (The first pair of log lines, with 3 requests, is the quick validation mount before FUSE starts.)"*

### Step 6 — The money shot: same workload, different policy (2 min)

Paste this once (it writes 4 files of 256 KiB in parallel, reads them back, deletes them, and prints the total seek):

```bash
cat > compare.sh <<'EOF'
for p in fcfs sstf scan; do
  cp disk.img p.img; : > filesystem.log
  ./schedfs p.img mnt --sched=$p > /dev/null
  for i in 1 2 3 4; do head -c 262144 /dev/urandom > mnt/w$i.bin & done; wait
  for i in 1 2 3 4; do cat mnt/w$i.bin > /dev/null & done; wait
  rm mnt/w*.bin; fusermount3 -u mnt; sleep 0.5
  echo "$p: $(grep unmounted filesystem.log | tail -1 | sed 's/.*image: //')"
done
EOF
bash compare.sh
```

What we measured (3 trials each, 4,927 requests every run): **FCFS ≈ 193,773 blocks, SSTF ≈ 116,180 (−40 %), SCAN ≈ 132,537 (−32 %)**. Your numbers will vary slightly run to run — say so; that is honest and expected because thread timing changes the order requests arrive.

Say: *"Same files, same number of requests — only the order of service changed. SSTF cut head movement by about 40 %."*

### Step 7 — Watch the head move (1.5 min) · *browser*

Scroll to **"Watch the head move"**, keep the *Textbook trace*, press **Step** a few times, then **Play**.

Expected totals: **FCFS 640, SSTF 236, SCAN 236** — the classic textbook answer (head at 53, queue 98 183 37 122 14 124 65 67), and the same values our C unit test asserts.

Then switch the trace to **Benchmark: concurrent** and press Play: FCFS zig-zags across the disk (**27,402 blocks**) while SSTF needs **4,029** — 6.8× less.

Optional: drag the **β (age)** slider to show the Adaptive policy moving from SSTF-like towards age-first behaviour.

### Step 8 — Proof it is tested (45 s) · *terminal*

```bash
make test
```

Expected endings: `scheduler tests passed`, `core persistence and concurrency tests passed`, `FUSE system tests passed`. (The line "Ignoring invalid max threads value…" is a harmless libfuse 3.14 message, not ours.)

Say: *"Three layers: the scheduler on the textbook trace, the core with four threads writing at once plus remount persistence, and a real FUSE mount under every policy — including background mode. We also ran ThreadSanitizer: no data races."*

### Step 9 — Close (30 s)

Say: *"Limitations: no journal, so a clean unmount is needed; files up to about 4 MiB; seek is modelled as block distance. Next steps: C-SCAN and LOOK, a block cache, and fixing the Adaptive policy's scaling — on the real mount it behaved almost like FCFS because the age term dominates."*

---

## 4. If something goes wrong (backup plan)

| Problem | What to do |
|---|---|
| `fuse: device not found` / mount fails in Docker | You forgot `--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined`. Restart the container with the full command. |
| `mountpoint is not empty` or "Transport endpoint is not connected" | `fusermount3 -u mnt` (or `fusermount3 -uz mnt`), then mount again. |
| `open (remove an old image…): File exists` | That is the safety check working. Use `rm disk.img` first, or a new name. |
| Docker will not start at all | Present from the report: Figures 19–24 are real terminal captures of build, format, mount, file operations, unmount and verbose logging; Figure 33 is `make test`, Figure 35 is ThreadSanitizer, Figure 37 is the policy comparison. The web demo works without Docker. |
| No internet | The Docker image works offline; open the demo page beforehand, or open `docs/index.html` from the repo directly in the browser. |

---

## 5. Suggested split for four presenters

(Matches the proposed module allocation in the report — adjust to what each person actually built.)

| Presenter | Talks about | Demo steps |
|---|---|---|
| Saanvi Grover | Idea, FUSE adapter, formatter, on-disk layout, mount life cycle (the `fork()`/`op_init` fix) | 1, 2 |
| Aditya Sharma | File-system core: inodes, directories, bitmap allocation, indirect blocks, error handling | 3, 4 |
| Prankit Goel | Request queue, scheduler policies, dispatcher, synchronization and lock order | 5, 6, 7 |
| Aaradhya Shankar Kulashri | Benchmark, metrics, graphs, testing (unit, core, system, ThreadSanitizer), limitations | 8, 9 |

---

## 6. Questions faculty are likely to ask (and good answers)

**Why FUSE instead of a kernel module?**
A FUSE file system is an ordinary process: it can be debugged, restarted and tested without risking the kernel. The cost is extra context switches per operation, which does not matter here because we study the *order* of block requests, not raw speed.

**Why only one dispatcher thread?**
"Head position" only means something if one place serves requests in sequence — like a real disk arm. Many producer threads, one consumer: the bounded-buffer problem.

**How does the bounded queue work?**
A mutex protects an array of up to 128 requests. Producers wait on `not_full` when it is full; the dispatcher waits on `not_empty` when it is empty. Every `pthread_cond_wait` is inside a `while` loop that re-checks the condition, so spurious wake-ups are harmless.

**How does a caller know its request is done?**
Each request has its own mutex, condition variable and `completed` flag. The dispatcher sets the flag under that mutex and then signals, so the wake-up cannot be lost.

**How do you prevent deadlock?**
One global lock order: directory → inode rwlock → inode-alloc → inode-table → bitmap → superblock → queue. Circular wait is impossible. The dispatcher only takes the queue mutex and the request's own mutex, so a thread holding file-system locks while waiting for I/O cannot block it.

**What is starvation and how does your Adaptive policy handle it?**
SSTF can keep serving nearby requests while a far one waits forever. Adaptive scores each request as `α·distance/N − β·wait/max_wait` and serves the lowest score, so old requests gain priority (ageing). With β = 0 it is exactly SSTF.

**Why is SCAN the worst on the clustered trace?**
SCAN travels all the way to the disk edge before reversing, even when every request is close together (164 vs 58 blocks). No single policy wins everywhere.

**What is the difference between your SCAN and LOOK?**
Our selection reverses when no request is left in the current direction, but SCAN is *charged* for travelling to the edge (block 0 or 4095) in the benchmark and unit test — that is why SCAN's textbook total is 236 (53 down to 0, then up to 183) and not LOOK's 208.

**What happens if the machine crashes while mounted?**
There is no journal, so the image could be inconsistent; a clean unmount is required. Journaling and an `fsck` are listed as future work.

**What was the hardest bug?**
Two: (1) 32 inodes share one 4 KiB block, so updating one is a read–modify–write — two threads could overwrite each other's inode. We added an inode-table mutex. (2) Without `-f`, FUSE daemonizes with `fork()`, and threads do not survive `fork()`, so the dispatcher vanished and the first `mkdir` hung forever. We now start the dispatcher in FUSE's `init` callback, inside the daemon.

**How big is the code and how did you test it?**
2,103 non-blank, non-comment lines (C, shell, Python, Makefile). 25 test cases, all passing: unit (textbook trace), integration (4 threads × 64 KiB, persistence across remount), system (real mount under every policy and in background mode), error handling, and ThreadSanitizer.

**Is seek distance real?**
It is the textbook model: |current block − next block|. The image lives on an SSD or file, so we measure the scheduler's decisions, not physical arm movement.

---

## 7. Two-line summary to end on

> "SchedFS is a working Linux file system where every block waits its turn in a scheduled queue. On real traffic SSTF cut head movement by 40 % versus FCFS, and on the concurrent benchmark FCFS travelled 6.8 times further than SSTF — the textbook results, reproduced on real I/O."
