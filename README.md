# SchedFS

SchedFS is an educational, mountable FUSE 3 file system written in GNU C11. It stores all files and directories in a custom block image and routes runtime block reads and writes through a synchronized request queue serviced by one dispatcher. FCFS, SSTF, SCAN, and an age-aware adaptive policy are selectable at mount time.

## Features

- Fixed-size, explicitly laid-out superblock, bitmap, 128-byte inodes, 64-byte directory entries, and data blocks
- Twelve direct pointers plus one 1024-entry single-indirect block
- Sparse reads, partial block I/O, truncate, unlink, mkdir/rmdir, rename, stat and statfs
- Bounded pthread producer-consumer queue with request-specific completion conditions
- Deterministic scheduling tie-breaking and monotonic-clock metrics
- Reproducible workloads, CSV export, and Pandas/Matplotlib graphs

## Prerequisites

Ubuntu 22.04 or 24.04 is recommended:

```sh
sudo apt update
sudo apt install -y build-essential pkg-config libfuse3-dev fuse3 python3-pandas python3-matplotlib
```

The runtime user must be able to access `/dev/fuse`. Do not mount or test as root.

## Build and format

```sh
make
./mkfs.schedfs disk.img 16M
mkdir -p mnt
```

The formatter refuses to overwrite an existing image. Remove or rename an old image deliberately before reformatting.

## Mount and use

```sh
./schedfs disk.img mnt --sched=fcfs
./schedfs disk.img mnt --sched=sstf
./schedfs disk.img mnt --sched=scan
./schedfs disk.img mnt --sched=adaptive --alpha=0.7 --beta=0.3
```

In another terminal:

```sh
mkdir mnt/project
echo 'Operating Systems' > mnt/project/demo.txt
cat mnt/project/demo.txt
cp mnt/project/demo.txt mnt/project/copy.txt
stat mnt/project/copy.txt
truncate -s 8192 mnt/project/copy.txt
mv mnt/project/copy.txt mnt/project/result.txt
rm mnt/project/demo.txt mnt/project/result.txt
rmdir mnt/project
fusermount3 -u mnt
```

Use `-f` to keep FUSE in the foreground and `--verbose` for DEBUG logging. `filesystem.log` records initialization and allocator activity.

## Scheduling

- **FCFS** preserves arrival order.
- **SSTF** chooses the smallest block distance and can starve remote requests.
- **SCAN** continues in its current direction and reverses at a disk boundary.
- **Adaptive** minimizes normalized distance minus normalized age. Larger beta favors older requests; it is experimental and is not assumed to win every workload.

## Benchmark and graphs

```sh
make bench
python3 scripts/plot_results.py
```

`benchmark.csv` contains per-workload measurements for all policies. Graphs are written to `results/`. The benchmark replays one fixed trace per workload so policies see identical inputs; timings are measured, not pre-filled. A small 2 microsecond-per-block seek-delay model makes policy differences observable on SSDs and virtual machines. The concurrent trace is the deterministic interleaving used for policy comparison; the pthread integration test separately exercises four real producers through the queue. The mounted filesystem remains the authoritative integration path for real I/O.

## Tests

```sh
make test
```

The scheduler unit test checks the classic head-53 trace. The system test formats and mounts a fresh image under every policy, compares data, truncates, renames, removes, and unmounts. See [testing notes](docs/testing.md).

## Limitations

SchedFS is a teaching file system, not a production filesystem. It has no journal, recovery scanner, hard links, symlinks, extended attributes, ACL enforcement, cache, or crash consistency. Seek distance models block-number movement and does not represent SSD behavior, rotational latency, or device caches. The current rename refuses to replace an existing target. Image format fields are native little-endian and intended for Linux/x86-64 demonstrations.

## Repository map

- `include/`: on-disk and module interfaces
- `src/fs/`: inode, directory, path, and allocation implementation
- `src/sync/`, `src/sched/`, `src/storage/`: queue, policies, dispatcher, image I/O
- `src/fuse/`: libfuse callback adapter
- `mkfs/`: image formatter
- `src/bench/`, `scripts/`: workload replay and visualization
- `tests/`: scheduler and mounted system tests
- `docs/`: architecture, implementation, and testing details

Potential next steps are journaling, a consistency checker, per-block metadata generations, C-SCAN/LOOK variants, and a real concurrent benchmark harness with controlled producer arrival barriers.
