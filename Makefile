CC ?= gcc
PKG_CONFIG ?= pkg-config
CPPFLAGS := -Iinclude -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion
PTHREAD := -pthread
FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3)
CORE := src/utils/logger.c src/sched/scheduler.c src/sync/request_queue.c src/bench/metrics.c src/storage/virtual_disk.c src/fs/filesystem.c

.PHONY: all clean test bench format install
all: schedfs mkfs.schedfs schedfs-bench
schedfs: src/fuse/fuse_main.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $^ $(PTHREAD) $(FUSE_LIBS) -lm
mkfs.schedfs: mkfs/mkfs.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<
schedfs-bench: src/bench/benchmark_main.c src/sched/scheduler.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm
tests/test_scheduler: tests/test_scheduler.c src/sched/scheduler.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm
tests/test_core: tests/test_core.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(PTHREAD) -lm
test: all tests/test_scheduler tests/test_core
	./tests/test_scheduler
	rm -f tests/core.img
	./mkfs.schedfs tests/core.img 16M
	./tests/test_core tests/core.img
	rm -f tests/core.img
	./tests/system.sh
bench: schedfs-bench
	./schedfs-bench --all
format: mkfs.schedfs
	./mkfs.schedfs disk.img 16M
install: all
	install -m 0755 schedfs mkfs.schedfs schedfs-bench /usr/local/bin/
clean:
	rm -f schedfs mkfs.schedfs schedfs-bench tests/test_scheduler tests/test_core tests/core.img *.o benchmark.csv
