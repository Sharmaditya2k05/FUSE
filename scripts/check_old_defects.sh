#!/usr/bin/env bash
# Show that the regression tests detect the review findings in the original code.
# Builds tests/test_regress against filesystem.c from the given commit (default: the
# commit before this work), changed only to accept the new mount-options argument,
# then runs each focused test on a fresh image. Each defect should fail; the current
# code passes all of them (make unit).
set -uo pipefail
base=${1:-afacffa}
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$root" show "$base:src/fs/filesystem.c" >"$tmp/filesystem_old.c"
python3 - "$tmp/filesystem_old.c" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text()
s = s.replace("int sfs_mount_image(sfs_t *fs, const char *path, sched_policy_t policy, double alpha, double beta,\n                    bool verbose) {",
              "int sfs_mount_image(sfs_t *fs, const char *path, const vdisk_options_t *opts, bool verbose) {")
s = s.replace("vdisk_open(&fs->disk, path, fs->sb.total_blocks, policy, alpha, beta, 128);",
              "vdisk_open(&fs->disk, path, fs->sb.total_blocks, opts);")
s = s.replace("scheduler_name(policy)", "scheduler_name(opts->policy)")
p.write_text(s)
PY
core="src/utils/logger.c src/sched/scheduler.c src/sync/request_queue.c src/bench/metrics.c src/storage/virtual_disk.c"
(cd "$root" && gcc -Iinclude -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE -std=gnu11 -O2 -g \
    -o "$tmp/regress_old" tests/test_regress.c $core "$tmp/filesystem_old.c" -pthread -lm) || exit 1
for t in truncate create unlink meta rename; do
    rm -f "$tmp/img"
    "$root/mkfs.schedfs" "$tmp/img" 16M >/dev/null
    if timeout 120 "$tmp/regress_old" "$tmp/img" "$t" >"$tmp/out" 2>&1; then
        echo "$t: old code PASSED (defect not reproduced)"
    else
        echo "$t: old code FAILED: $(grep -m1 -E 'check failed|fsck' "$tmp/out" || tail -1 "$tmp/out")"
    fi
done
