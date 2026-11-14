#!/usr/bin/env bash
# 把 CF key Secret 写入 .env，供 crates/mlccore/build.rs 在编译期嵌入。
# 无 Secret 时不写文件 → 产物退回 MCIM 镜像（与 C++ 侧 MLC_EMBED_CF_KEY=OFF 语义一致）。
set -euo pipefail

if [ -n "${CF_KEY:-}" ]; then
    printf 'MLC_CURSEFORGE_API_KEY=%s\n' "$CF_KEY" > .env
    echo "已配置 CF key：编译期将嵌入官方 API key"
else
    echo "未配置 CF key Secret：产物走 MCIM 镜像"
fi
