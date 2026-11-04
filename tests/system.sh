#!/usr/bin/env bash
set -euo pipefail
for c in fusermount3 cmp; do command -v "$c" >/dev/null || { echo "SKIP system test: missing $c"; exit 0; }; done
[[ -e /dev/fuse ]] || { echo "SKIP system test: /dev/fuse is unavailable"; exit 0; }
if [[ ${EUID} -eq 0 ]]; then echo "Do not run FUSE system tests as root" >&2; exit 1; fi
tmp=$(mktemp -d); image="$tmp/disk.img"; mnt="$tmp/mnt"; mkdir "$mnt"
cleanup(){ mountpoint -q "$mnt" && fusermount3 -u "$mnt" || true; rm -rf -- "$tmp"; }
trap cleanup EXIT
./mkfs.schedfs "$image" 16M
for policy in fcfs sstf scan adaptive; do
  ./schedfs "$image" "$mnt" --sched="$policy" -f & pid=$!
  for _ in {1..50}; do mountpoint -q "$mnt" && break; sleep .1; done
  mountpoint -q "$mnt" || { echo "mount failed for $policy"; kill "$pid" || true; exit 1; }
  mkdir "$mnt/docs"; printf 'SchedFS-%s\n' "$policy" > "$mnt/docs/a.txt"
  cp "$mnt/docs/a.txt" "$tmp/actual"; printf 'SchedFS-%s\n' "$policy" > "$tmp/expected"; cmp "$tmp/expected" "$tmp/actual"
  truncate -s 20000 "$mnt/docs/a.txt"; test "$(stat -c %s "$mnt/docs/a.txt")" = 20000
  mv "$mnt/docs/a.txt" "$mnt/docs/b.txt"; rm "$mnt/docs/b.txt"; rmdir "$mnt/docs"
  fusermount3 -u "$mnt"; wait "$pid" || true
done
# Background (daemonized) mount, as documented in the README: no -f, relative image path.
cp "$image" "$tmp/bg.img"
(cd "$tmp" && "$OLDPWD/schedfs" bg.img mnt --sched=scan >/dev/null)
for _ in {1..50}; do mountpoint -q "$mnt" && break; sleep .1; done
mountpoint -q "$mnt" || { echo "background mount failed"; exit 1; }
timeout 10 mkdir "$mnt/bg" || { echo "background mount does not serve I/O"; exit 1; }
timeout 10 sh -c "echo persisted > '$mnt/bg/x.txt'"
test "$(timeout 10 cat "$mnt/bg/x.txt")" = persisted
fusermount3 -u "$mnt"
for _ in {1..50}; do mountpoint -q "$mnt" || break; sleep .1; done
(cd "$tmp" && "$OLDPWD/schedfs" bg.img mnt --sched=fcfs >/dev/null)
for _ in {1..50}; do mountpoint -q "$mnt" && break; sleep .1; done
test "$(timeout 10 cat "$mnt/bg/x.txt")" = persisted || { echo "data lost across remount"; exit 1; }
fusermount3 -u "$mnt"
echo "FUSE system tests passed"
