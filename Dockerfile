# syntax=docker/dockerfile:1

FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

ARG VCPKG_REF=master

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf \
    bison \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    flex \
    git \
    libtool \
    linux-libc-dev \
    ninja-build \
    pkg-config \
    python3 \
    tar \
    unzip \
    wget \
    zip \
    && rm -rf /var/lib/apt/lists/*


RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && cd /opt/vcpkg \
    && git checkout "${VCPKG_REF}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && ln -sf /opt/vcpkg/vcpkg /usr/local/bin/vcpkg

WORKDIR /workspace

# This image is a reusable Linux build base.
# It intentionally avoids exporting project-specific preset variables.
# Derived images or user presets should decide how build tools are wired in.
# Restore Qigao.Re2c.Binary from GitHub Packages and supply its host RE2C_ROOT.
