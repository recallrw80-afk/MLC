#!/bin/bash
# bundle-dist-macos.sh — 把 mlc 运行所需的 Qt/第三方 dylib 收进 dist/，实现零依赖分发
# 用法: bundle-dist-macos.sh <dist目录> <Qt前缀>
# 产物布局: dist/mlc + dist/lib/*.dylib + dist/plugins/tls/*.dylib
# 系统库（/usr/lib、/System）属 macOS 基线，一律不带；TLS 用系统 SecureTransport，无需 OpenSSL
set -euo pipefail

DIST="$1"
QT_PREFIX="$2"
BIN="$DIST/mlc"
LIBDIR="$DIST/lib"
mkdir -p "$LIBDIR" "$DIST/plugins/tls"

# 解析 @rpath 引用到 Qt lib 目录（Qt 官方 dylib 的 install name 都是 @rpath 形式）
resolve() {
    case "$1" in
        @rpath/*|@loader_path/*|@executable_path/*)
            echo "$QT_PREFIX/lib/${1#@*/}" ;;
        *) echo "$1" ;;
    esac
}

# 把一个二进制（主程序或库）的非系统依赖收进 lib/，并把引用改写为 @loader_path/lib
collect() {
    local target="$1"
    otool -L "$target" | awk 'NR>1 {gsub(/^[ \t]+/,"",$1); print $1}' | while read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/*) continue ;;
        esac
        src="$(resolve "$dep")"
        [ -f "$src" ] || continue
        base="$(basename "$src")"
        dst="$LIBDIR/$base"
        if [ ! -f "$dst" ]; then
            cp "$src" "$dst"
            chmod +w "$dst"
        fi
        install_name_tool -change "$dep" "@loader_path/lib/$base" "$target" 2>/dev/null || true
    done
}

collect "$BIN"

# 收编进来的库自身：改 install id，并递归收它们自己的依赖（如 ICU 互相引用）。
# 注意遍历所有文件而非 *.dylib：macOS 的 Qt 以 framework 形态链接，
# 收到的是无扩展名的 Mach-O（如 lib/QtCore）
for lib in "$LIBDIR"/*; do
    [ -f "$lib" ] || continue
    install_name_tool -id "@loader_path/lib/$(basename "$lib")" "$lib" 2>/dev/null || true
    collect "$lib"
done

# TLS 插件（QNAM 走 HTTPS 必需；Qt 默认在 <程序目录>/plugins 下找）
if [ -d "$QT_PREFIX/plugins/tls" ]; then
    cp "$QT_PREFIX"/plugins/tls/*.dylib "$DIST/plugins/tls/" 2>/dev/null || true
    for p in "$DIST"/plugins/tls/*.dylib; do
        [ -f "$p" ] || continue
        install_name_tool -id "@loader_path/../plugins/tls/$(basename "$p")" "$p" 2>/dev/null || true
        collect "$p"
    done
fi

echo "bundle: 已收集 $(ls "$LIBDIR" | wc -l | tr -d ' ') 个库到 $LIBDIR"
