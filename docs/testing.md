# Testing

`make test` first checks the classic scheduler trace, then runs mounted system tests when `fusermount3` is present. The system script formats a new image and repeats create/write/read/compare/truncate/rename/delete operations under all four policies.

Recommended deeper checks on Ubuntu:

```sh
make clean && make CFLAGS='-std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined'
ASAN_OPTIONS=detect_leaks=1 make test
valgrind --leak-check=full ./tests/test_scheduler
```

FUSE mount tests may be unavailable inside restricted containers even when compilation succeeds. Ensure `/dev/fuse` exists and the process can invoke `fusermount3`.
