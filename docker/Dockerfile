# Ubuntu build/test environment for LockSpy (LD_PRELOAD and /proc need Linux).
# Usage (from repo root):  docker build -t lockspy-dev docker/
#                           docker run --rm -it -v "$PWD":/lockspy -w /lockspy lockspy-dev
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential g++ make gdb valgrind binutils graphviz python3 git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /lockspy
