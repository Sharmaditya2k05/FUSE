#!/usr/bin/env bash
# One mounted SchedFS experiment on a fresh, disposable image.
#
#   scripts/run_mounted.sh OUTDIR "SCHEDFS OPTIONS" "WORKLOAD OPTIONS" [IMAGE_SIZE]
#
# Example:
#   scripts/run_mounted.sh /tmp/r1 "--sched=deadline --model-base-us=100 --model-us-per-block=1" \
#       "--duration 10 --bg 2 --fg-op read" 64M
#
# Writes into OUTDIR: fg.csv, bg_*.csv, roles.csv, workload.json (from schedfs-workload),
# trace.csv (one row per block request, from schedfs --trace), schedfs.log and run.json.
# The image and mount point live in a temporary directory that is removed afterwards.
set -euo pipefail
out=$1
fsopts=$2
wlopts=$3
size=${4:-64M}
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
tmp=$(mktemp -d)
img="$tmp/disk.img"
mnt="$tmp/mnt"
mkdir "$mnt"
pid=""
cleanup() {
    if mountpoint -q "$mnt"; then fusermount3 -u "$mnt" || fusermount3 -uz "$mnt" || true; fi
    if [[ -n "$pid" ]]; then wait "$pid" 2>/dev/null || true; fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT

"$root/mkfs.schedfs" "$img" "$size" >/dev/null
# -f keeps schedfs in the foreground of this script, so we can wait for it to write
# the trace after the unmount.
(cd "$tmp" && exec "$root/schedfs" "$img" "$mnt" $fsopts --trace="$out/trace.csv" -f) \
    >"$out/schedfs.stdout" 2>&1 &
pid=$!
for _ in $(seq 1 100); do mountpoint -q "$mnt" && break; sleep 0.05; done
mountpoint -q "$mnt" || { echo "mount failed"; cat "$out/schedfs.stdout"; exit 1; }

"$root/schedfs-workload" --dir "$mnt" --out "$out" $wlopts

fusermount3 -u "$mnt"
wait "$pid" || true
pid=""
cp "$tmp/filesystem.log" "$out/schedfs.log" 2>/dev/null || true
cat >"$out/run.json" <<JSON
{"schedfs_options": "$fsopts", "workload_options": "$wlopts", "image_size": "$size",
 "kernel": "$(uname -r)", "cpus": $(nproc), "date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
 "commit": "$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"}
JSON
echo "done: $out"
