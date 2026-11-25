# Locking and inode lifetime

FUSE runs file operations on several threads at once, and the dispatcher thread serves
their block requests. This page lists every lock, the order in which locks are taken,
how long an inode number stays valid, and what was found and fixed during the review.

## Locks

| Lock | Type | Protects | Held during block I/O? |
|---|---|---|---|
| `ns_lock` | reader-writer | the directory tree (names, entries, parent links) | create/mkdir/unlink/rmdir/rename: yes (write); lookups: only until the target inode is locked |
| `inode_locks[i]` | reader-writer, one per inode | an inode and its data blocks | yes: reads hold it shared, write/truncate/unlink hold it exclusive |
| `dir_locks[d]` | mutex, one per directory | entry slots of one directory | yes (inner lock; all callers already hold `ns_lock` for writing) |
| `inode_alloc_lock` | mutex | choosing a free inode | yes |
| `inode_table_lock` | mutex | read-modify-write of an inode-table block (32 inodes share one) | yes |
| `bitmap_io_lock`, `super_io_lock` | mutex | the order in which bitmap and superblock snapshots are written | yes |
| `bitmap_lock`, `super_lock` | mutex | the in-memory bitmap and superblock counters | no (copy only) |
| queue mutex | mutex | the pending-request array | no |
| per-request mutex | mutex | one request's completion flag | no, never nested |

## Lock order

```
ns_lock -> inode_locks[i] -> dir_locks[d] -> inode_alloc_lock -> inode_table_lock
  -> bitmap_io_lock -> bitmap_lock -> super_io_lock -> super_lock
  -> request-queue mutex -> per-request mutex
```

Every path takes locks in this order (a lock later in the list is never held while an
earlier one is requested), so a cycle of waiting threads, and therefore a deadlock,
cannot form. The dispatcher takes only the queue mutex and a request's own mutex, so a
file-system thread that holds file locks while it waits for its I/O never blocks the
dispatcher.

## Inode lifetime

A path is turned into an inode number by `resolve()`. That number is only safe to use
while nothing can free it:

1. A lookup (`lookup_locked`) takes `ns_lock` for reading, resolves the path, takes the
   inode's lock, and only then releases `ns_lock`.
2. `unlink` and `rmdir` take `ns_lock` for writing, remove the directory entry, release
   `ns_lock`, and then take the inode's **write** lock before freeing its blocks and
   clearing the inode (`destroy_inode`).

Because of step 1, every operation that found the name before it was removed already
holds the inode lock when `unlink` gets `ns_lock`; `unlink` then waits for them in step 2.
No new lookup can find the removed name. The inode number is cleared (mode 0) only after
that, so `inode_alloc` cannot hand it out while an old user still holds it.

Operations do not keep inode numbers between FUSE calls (no file handles are used), so
there is no longer-lived reference to protect.

## Persistence order

When a block is allocated, the bitmap and the superblock are written (in that order)
before the new block is zero-filled and handed to the file; the inode is written at the
end of the operation. A crash in between can leak a block but cannot make two files
share one. There is no journal, so crash consistency is not claimed: a clean unmount is
required (`sfs_unmount_image` writes the final bitmap and superblock).

## Review findings

| Finding | Status | Evidence | Fix |
|---|---|---|---|
| `unlink` freed blocks without the inode lock held by reads and writes | **Confirmed defect** | `test_regress unlink` on the old code: a reader saw another file's bytes after the inode was reused | `unlink`/`rmdir` take the inode write lock (`destroy_inode`); lookups hold `ns_lock` until the inode is locked |
| Two threads creating the same name | **Confirmed defect** | `test_regress create`: both creates succeeded, two entries with one name | create/mkdir/unlink/rmdir/rename serialized by `ns_lock` (write) |
| Inode reuse while an operation is pending | **Confirmed** (same root cause as unlink) | as above | as above |
| Rename and directory mutations racing | **Confirmed risk, fixed** | duplicate-entry race above; rename could also move a directory into its own subtree (`test_regress rename`) | `ns_lock`; rename checks `is_within()` and returns `EINVAL` |
| Stale bitmap/superblock snapshot written after a newer one | **Suspected risk, fixed defensively** | the old code allowed it (snapshot under the lock, write after unlocking, scheduler may reorder); not reproduced in 5 rounds of 8 threads | `bitmap_io_lock`/`super_io_lock` keep snapshot and write together, so writes reach the queue in snapshot order |
| Shared counters | **Confirmed race (benign)**: `statfs` read `free_blocks`/`free_inodes` without `super_lock` | code review | `statfs` reads them under `super_lock` |
| Allocation and metadata persistence ordering | **Checked, no defect** | see "Persistence order" | none needed |
| Stale inode copy overwriting newer metadata | **Checked, no defect after the fixes** | `sfs_write`/`truncate` hold the inode write lock from reading the inode to writing it back; directory inodes are changed only under `ns_lock` (write) | none beyond the above |

ThreadSanitizer (`make tsan`) reports no data race in the core and regression tests,
which run concurrent writers, readers, creates, unlinks and allocations. A clean TSan run
is supporting evidence only: it sees the interleavings that happened, not all of them.
That is why the targeted tests in `tests/test_regress.c` exist, and why
`scripts/check_old_defects.sh` shows each of them failing on the original code.
