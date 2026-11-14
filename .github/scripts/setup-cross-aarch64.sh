#!/usr/bin/env bash
# Linux aarch64 交叉编译前置：安装目标 C/C++ 工具链与 cmake，并导出 CC/CXX/AR。
# 必要性：TLS 走 reqwest 0.13 的 rustls feature → provider 是 aws-lc-rs，
# 而 aws-lc-sys 用 cmake 编译 C 源码。只设 linker 不够，必须让 cmake 也用
# aarch64 的 C 编译器，否则会拿宿主机 gcc 编出 x86_64 目标文件导致链接失败。
set -euo pipefail

sudo apt-get update
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake

{
    echo "CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc"
    echo "CC_aarch64_unknown_linux_gnu=aarch64-linux-gnu-gcc"
    echo "CXX_aarch64_unknown_linux_gnu=aarch64-linux-gnu-g++"
    echo "AR_aarch64_unknown_linux_gnu=aarch64-linux-gnu-ar"
    echo "CMAKE_C_COMPILER=aarch64-linux-gnu-gcc"
    echo "CMAKE_CXX_COMPILER=aarch64-linux-gnu-g++"
} >> "$GITHUB_ENV"
