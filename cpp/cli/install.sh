#!/usr/bin/env bash
# mlc 安装脚本
# 用法:
#   bash install.sh                                  # 【默认】自动选源下载（GitHub 优先，失败自动切 Gitee 镜像）
#   bash install.sh --beta                           # 安装最新预发布版（源同样自动降级）
#   bash install.sh ./mlc-linux-x86_64.tar.xz       # 安装本地包（自己 make package-tar 的产物——不含内嵌 key）
#   curl -fsSL <发布地址>/install.sh | bash           # 一键安装
#
# 安装内容:
#   ~/.local/lib/mlc/      — mlc 主程序 + lib/（收编的 Qt/第三方库）+ plugins/（TLS 插件）
#   ~/.local/bin/mlc       — 指向主程序的符号链接（rpath=$ORIGIN 自动找到库）
#   ~/.local/bin/mlc-gui   — 包内含 mlc-gui 时注册（GUI 测试版入口）
set -euo pipefail

# ---- 可配置项 ----
# 发布包下载地址（GitHub Releases 或自建服务器；也可用环境变量覆盖）
MLC_RELEASE_URL="${MLC_RELEASE_URL:-https://github.com/recallrw80-afk/MLC/releases/latest/download}"
MLC_REPO="${MLC_REPO:-recallrw80-afk/MLC}"
INSTALL_LIB="${MLC_INSTALL_LIB:-$HOME/.local/lib/mlc}"
INSTALL_BIN="${MLC_INSTALL_BIN:-$HOME/.local/bin}"

# ---- 旧版（lpcl 时代）安装迁移 ----
# 旧安装目录存在且新目录不存在时整体迁移，保留游戏内容与配置
OLD_LIB="$HOME/.local/lib/lpcl"
if [ -d "$OLD_LIB" ] && [ ! -e "$INSTALL_LIB" ]; then
    echo "==> 迁移旧安装目录 lpcl → mlc"
    mv "$OLD_LIB" "$INSTALL_LIB"
    if [ -f "$INSTALL_LIB/LPCL.ini" ]; then
        sed -i -e 's/^\[LPCL\]/[MLC]/' -e "s|$OLD_LIB|$INSTALL_LIB|g" "$INSTALL_LIB/LPCL.ini" 2>/dev/null || true
        mv "$INSTALL_LIB/LPCL.ini" "$INSTALL_LIB/MLC.ini"
    fi
fi

# ---- 平台检测 ----
OS_NAME="$(uname -s)"
if [ "$OS_NAME" = "Darwin" ]; then
    # macOS：预编译包只有 Apple Silicon（liblzma 单架构出不了 universal 包）
    case "$(uname -m)" in
        arm64)  PKG_NAME="mlc-macos-aarch64.tar.xz" ;;
        *)      echo "macOS 预编译包目前仅支持 Apple Silicon（M 系列）；Intel Mac 请走源码编译" >&2; exit 1 ;;
    esac
else
    case "$(uname -m)" in
        x86_64|amd64)    ARCH="x86_64" ;;
        aarch64|arm64)   ARCH="aarch64" ;;
        *) echo "不支持的架构: $(uname -m)" >&2; exit 1 ;;
    esac
    PKG_NAME="mlc-linux-${ARCH}.tar.xz"
fi

# ---- 获取安装包：本地文件优先，否则下载 ----
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 参数解析：--beta（预发布）；--cn 已废弃（源选择已自动化，兼容吞掉）
BETA=0
LOCAL_FILE=""
for a in "$@"; do
    case "$a" in
        --cn) echo "（--cn 已废弃：现在自动 GitHub 优先、失败降级 Gitee，无需指定）" ;;
        --beta|--pre) BETA=1 ;;
        *) [ -z "$LOCAL_FILE" ] && LOCAL_FILE="$a" ;;
    esac
done

PKG="$PKG_NAME"
GITEE_REPO="${MLC_GITEE_REPO:-recall80/mlc}"

download_github() {
    local URL
    if [ "$BETA" = 1 ]; then
        # releases/latest 跳过 pre-release，走列表接口取最新一条（含预发布）
        local API="https://api.github.com/repos/${MLC_REPO}/releases?per_page=1"
        URL="$(curl -fsSL "$API" 2>/dev/null | grep -oE '"browser_download_url": *"[^"]*/'"${PKG}"'"' | head -1 | cut -d'"' -f4 || true)"
        [ -n "$URL" ] || return 1
    else
        URL="${MLC_RELEASE_URL%/}/${PKG}"
    fi
    curl -fSL --retry 2 -o "$PKG_PATH" "$URL" 2>/dev/null
}

download_gitee() {
    local API="https://gitee.com/api/v5/repos/${GITEE_REPO}/releases?per_page=10&direction=desc" TAG
    if [ "$BETA" = 1 ]; then
        # 最新一条（含预发布）
        TAG="$(curl -fsSL "$API" 2>/dev/null | grep -oE '"tag_name": *"[^"]+"' | head -1 | cut -d'"' -f4 || true)"
    else
        # 首个非预发布：tag 含 '-' 即预发布（与 release.sh 约定一致）
        TAG="$(curl -fsSL "$API" 2>/dev/null | grep -oE '"tag_name": *"[^"]+"' | cut -d'"' -f4 | grep -v '-' | head -1 || true)"
    fi
    [ -n "$TAG" ] || return 1
    curl -fSL --retry 2 -o "$PKG_PATH" \
        "https://gitee.com/${GITEE_REPO}/releases/download/${TAG}/${PKG}" 2>/dev/null
}

if [ -n "$LOCAL_FILE" ]; then
    PKG_PATH="$LOCAL_FILE"
    if [ ! -f "$PKG_PATH" ]; then
        echo "安装包不存在: $PKG_PATH" >&2
        exit 1
    fi
    echo "==> 使用本地安装包 $PKG_PATH"
else
    PKG_PATH="${TMP}/${PKG}"
    echo "==> 尝试 GitHub 下载 $PKG"
    if ! download_github; then
        echo "==> GitHub 不可用，自动切换 Gitee 镜像"
        if ! download_gitee; then
            echo "下载失败（GitHub 与 Gitee 均不可用）" >&2
            if [ "$BETA" != 1 ]; then
                echo "提示：如果目前各渠道只有预发布版本，装测试版请用 --beta" >&2
            fi
            echo "也可到 Releases 页面手动下载后用本地包模式安装" >&2
            exit 1
        fi
    fi
fi

echo "==> 安装到 ${INSTALL_LIB}"
mkdir -p "$INSTALL_LIB" "$INSTALL_BIN"
# 清掉旧版库目录与改名前遗留，防止残留过期文件（保留 MLC.ini 等配置）
rm -rf "${INSTALL_LIB}/lib" "${INSTALL_LIB}/plugins"
rm -f "${INSTALL_LIB}/mlc" "${INSTALL_LIB}/mlc-gui" "${INSTALL_LIB}/libmlccore.so" \
      "${INSTALL_LIB}/lpcl" "${INSTALL_LIB}/lpcl-cli" "${INSTALL_LIB}/lpcl-gui" "${INSTALL_LIB}/liblpclcore.so"
# GNU/bsd tar 均自动识别 gz/xz，无需 -z/-J 参数（兼容旧 .tar.gz 本地包）
tar -xf "$PKG_PATH" -C "$INSTALL_LIB"
chmod +x "${INSTALL_LIB}/mlc"

# macOS：清掉下载带来的 quarantine 属性，否则 Gatekeeper 会拦首次运行
if [ "$OS_NAME" = "Darwin" ]; then
    xattr -dr com.apple.quarantine "$INSTALL_LIB" 2>/dev/null || true
fi

echo "==> 注册命令 mlc → ${INSTALL_BIN}/mlc"
ln -sf "${INSTALL_LIB}/mlc" "${INSTALL_BIN}/mlc"
rm -f "${INSTALL_BIN}/lpcl" "${INSTALL_BIN}/lpcl-cli" "${INSTALL_BIN}/lpcl-gui"   # 改名前（lpcl 时代）遗留链接

# 包内含 GUI 时注册 mlc-gui
if [ -x "${INSTALL_LIB}/mlc-gui" ]; then
    echo "==> 注册命令 mlc-gui → ${INSTALL_BIN}/mlc-gui"
    ln -sf "${INSTALL_LIB}/mlc-gui" "${INSTALL_BIN}/mlc-gui"

    # ---- Linux 桌面集成（图标 / 启动器项） ----
    # Wayland 用 app_id（程序内 setDesktopFileName），X11 用 StartupWMClass；
    # 两者都指向本 .desktop 文件，文件名与 app_id 一致才能被匹配到。
    if [ "$OS_NAME" != "Darwin" ]; then
        APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
        ICON_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
        SRC_APPS="${INSTALL_LIB}/share/applications"
        SRC_ICONS="${INSTALL_LIB}/share/icons/hicolor/scalable/apps"

        if [ -f "${SRC_APPS}/org.mlc.MinecraftLauncher.desktop" ]; then
            mkdir -p "$APPS_DIR" "$ICON_DIR"
            # Exec 指向最终命令名（PATH 中的 mlc-gui），不写死安装目录
            sed 's|^Exec=.*|Exec=mlc-gui|' \
                "${SRC_APPS}/org.mlc.MinecraftLauncher.desktop" > "${APPS_DIR}/org.mlc.MinecraftLauncher.desktop"
            if [ -f "${SRC_ICONS}/org.mlc.MinecraftLauncher.svg" ]; then
                cp -f "${SRC_ICONS}/org.mlc.MinecraftLauncher.svg" "$ICON_DIR/"
            fi
            echo "==> 已注册桌面项 ${APPS_DIR}/org.mlc.MinecraftLauncher.desktop"
            command -v update-desktop-database >/dev/null 2>&1 && \
                update-desktop-database "$APPS_DIR" 2>/dev/null || true
            command -v gtk-update-icon-cache >/dev/null 2>&1 && \
                gtk-update-icon-cache -q -t "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" 2>/dev/null || true
        fi
    fi
fi

# ---- 验证 ----
if "${INSTALL_LIB}/mlc" version >/dev/null 2>&1; then
    echo "==> 已安装: $("${INSTALL_LIB}/mlc" version | head -1)"
else
    echo "警告: mlc 无法运行（架构不匹配？），请检查安装包" >&2
fi

# ---- PATH 检查 ----
case ":${PATH}:" in
    *":${INSTALL_BIN}:"*) ;;
    *)
        echo ""
        echo "注意: ${INSTALL_BIN} 不在 PATH 中。请把下面这行加入你的 shell 配置（~/.bashrc 或 ~/.zshrc）："
        echo ""
        echo "    export PATH=\"${INSTALL_BIN}:\$PATH\""
        echo ""
        ;;
esac

echo ""
echo "安装完成！运行 'mlc help' 开始使用。"
