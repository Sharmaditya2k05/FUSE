# Build and test environment for SchedFS (used for every result in results/mounted).
#   docker build -t schedfs-dev docker
#   docker run -it --rm --device /dev/fuse --cap-add SYS_ADMIN \
#     --security-opt apparmor:unconfined --security-opt seccomp=unconfined \
#     -v "$PWD:/src" -w /src schedfs-dev su student
# (seccomp=unconfined is only needed for `make tsan`, which disables address randomization.)
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential pkg-config libfuse3-dev fuse3 python3 python3-matplotlib python3-numpy git ca-certificates time procps && rm -rf /var/lib/apt/lists/*
RUN useradd -m -s /bin/bash student
