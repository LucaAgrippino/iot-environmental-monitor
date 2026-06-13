FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        clang-format \
        libsdl2-dev \
        pkg-config \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
