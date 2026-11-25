CC ?= gcc
PKG_CONFIG ?= pkg-config
CPPFLAGS := -Iinclude -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion
PTHREAD := -pthread
FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3)
SCHED := src/sched/scheduler.c src/sched/sim.c
CORE := src/utils/logger.c src/sched/scheduler.c src/sync/request_queue.c src/bench/metrics.c src/storage/virtual_disk.c src/fs/filesystem.c
TSAN := -std=gnu11 -O1 -g -fsanitize=thread

.PHONY: all clean test unit tsan bench format install
all: schedfs mkfs.schedfs schedfs-bench schedfs-workload
schedfs: src/fuse/fuse_main.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $^ $(PTHREAD) $(FUSE_LIBS) -lm
mkfs.schedfs: mkfs/mkfs.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<
schedfs-bench: src/bench/benchmark_main.c $(SCHED)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm
schedfs-workload: src/bench/workload.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<
tests/test_scheduler: tests/test_scheduler.c $(SCHED) src/sync/request_queue.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(PTHREAD) -lm
tests/test_core: tests/test_core.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(PTHREAD) -lm
tests/test_regress: tests/test_regress.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(PTHREAD) -lm

# Unit, core and regression tests (no FUSE mount needed).
unit: tests/test_scheduler tests/test_core tests/test_regress mkfs.schedfs
	./tests/test_scheduler
	rm -f tests/core.img tests/regress.img
	./mkfs.schedfs tests/core.img 16M
	./tests/test_core tests/core.img
	./mkfs.schedfs tests/regress.img 16M
	./tests/test_regress tests/regress.img
	rm -f tests/core.img tests/regress.img

# Everything, including a real FUSE mount under every policy.
test: all unit
	./tests/system.sh

# Core and regression tests rebuilt with ThreadSanitizer. Address randomization is
# switched off for the run (setarch -R): newer kernels map memory where TSan does not
# expect it. In Docker this needs --security-opt seccomp=unconfined.
NORAND := $(shell command -v setarch >/dev/null && echo setarch $$(uname -m) -R)
tsan: mkfs.schedfs
	$(CC) $(CPPFLAGS) $(TSAN) -o tests/test_core_tsan tests/test_core.c $(CORE) $(PTHREAD) -lm
	$(CC) $(CPPFLAGS) $(TSAN) -o tests/test_regress_tsan tests/test_regress.c $(CORE) $(PTHREAD) -lm
	rm -f tests/tsan.img && ./mkfs.schedfs tests/tsan.img 16M
	$(NORAND) ./tests/test_core_tsan tests/tsan.img
	rm -f tests/tsan.img && ./mkfs.schedfs tests/tsan.img 16M
	$(NORAND) ./tests/test_regress_tsan tests/tsan.img
	rm -f tests/tsan.img tests/test_core_tsan tests/test_regress_tsan

bench: schedfs-bench
	./schedfs-bench --all
format: mkfs.schedfs
	./mkfs.schedfs disk.img 16M
install: all
	install -m 0755 schedfs mkfs.schedfs schedfs-bench schedfs-workload /usr/local/bin/
clean:
	rm -f schedfs mkfs.schedfs schedfs-bench schedfs-workload tests/test_scheduler tests/test_core tests/test_regress tests/*.img *.o
