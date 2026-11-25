#!/usr/bin/env bash
# Five-minute SchedFS demo (script for docs/demo.md). Run from the repository root as a
# normal user, after `make`. Everything happens in a temporary directory that is
# removed at the end; nothing outside it is touched.
#
#   scripts/demo.sh            pauses between steps (press Enter)
#   NOPAUSE=1 scripts/demo.sh  runs straight through (about 1.5 minutes)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
for f in schedfs mkfs.schedfs schedfs-workload; do
    [[ -x $f ]] || { echo "run make first ($f missing)"; exit 1; }
done
tmp=$(mktemp -d)
img="$tmp/workspace.img"
mnt="$tmp/mnt"
mkdir "$mnt"
pid=""
cleanup() {
    if mountpoint -q "$mnt"; then fusermount3 -u "$mnt" || true; fi
    if [[ -n "$pid" ]]; then wait "$pid" 2>/dev/null || true; fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT
pause() { if [[ -t 0 && -z ${NOPAUSE:-} ]]; then read -rp "    [Enter] " _; fi; }
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
mount_fs() { # image, options...
    local image=$1
    shift
    (cd "$tmp" && exec "$root/schedfs" "$image" "$mnt" "$@" -f) >"$tmp/schedfs.out" 2>&1 &
    pid=$!
    for _ in $(seq 1 100); do mountpoint -q "$mnt" && return; sleep 0.05; done
    echo "mount failed"; cat "$tmp/schedfs.out"; exit 1
}
unmount_fs() { fusermount3 -u "$mnt"; wait "$pid" || true; pid=""; }
MODEL="--direct-io --model-base-us=100 --model-us-per-block=0.25"

say "1. A real file system you can mount (deadline policy)"
./mkfs.schedfs "$img" 64M
mount_fs "$img" --sched=deadline
mount | grep "$mnt"
mkdir "$mnt/notes"
echo "Meeting at 3 pm" >"$mnt/notes/today.txt"
cat "$mnt/notes/today.txt"
head -c 100000 /dev/urandom >"$tmp/photo.bin"
cp "$tmp/photo.bin" "$mnt/notes/photo.bin" && cmp "$tmp/photo.bin" "$mnt/notes/photo.bin" && echo "100 kB copy reads back identical"
ls -l "$mnt/notes"
df -h "$mnt" | tail -1
pause

say "2. Live: small-file reads while two background writers copy 2 MB files"
echo "   Same workload, same seed, a fresh image each time; disk = modeled seek time"
echo "   (100 us + 0.25 us per block moved) on O_DIRECT. Times are measured end to end."
for policy in fcfs deadline; do
    echo
    echo "   --- policy: $policy ---"
    out="$tmp/run_$policy"
    run_img="$tmp/run_$policy.img"
    ./mkfs.schedfs "$run_img" 64M >/dev/null
    unmount_fs
    mount_fs "$run_img" --sched="$policy" $MODEL --trace="$tmp/trace_$policy.csv"
    ./schedfs-workload --dir "$mnt" --out "$out" --duration 6 --warmup 1 --bg 2 --seed 7 --live
    unmount_fs
    mv "$tmp/trace_$policy.csv" "$out/trace.csv"
    python3 scripts/analyze_run.py "$out" | sed 's/^/   /' | grep -E "fg ops|bg throughput|fg queue wait"
    mount_fs "$img" --sched=deadline
done
pause

say "3. Longer repeated runs (3 x 10 s per cell, measured; from results/mounted/summary.md)"
if [[ -f results/mounted/summary.md ]]; then
    grep -E "^\| (workload|---|bg2_read) " results/mounted/summary.md | head -20
else
    echo "   results/mounted/summary.md not found (run scripts/experiments.py matrix ...)"
fi
pause

say "4. Persistence: clean unmount, mount again, the files are still there"
unmount_fs
mount_fs "$img" --sched=fcfs
cat "$mnt/notes/today.txt"
cmp "$tmp/photo.bin" "$mnt/notes/photo.bin" && echo "photo.bin unchanged after remount"
unmount_fs
echo
echo "Done. Temporary files are removed."
