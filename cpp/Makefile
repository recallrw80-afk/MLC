# Quick build & run for MLC

BUILD_DIR  := cmake-build-debug
QT_PREFIX  ?= $${HOME}/Qt/6.11.1/gcc_64
NPROC      := $(shell nproc)

# CLI 产物全部在 cli/ 下（libmlccore 在 MLC 根目录，二者解耦）
CLI_SRC       := cli
CLI_BUILD_DIR := cli/cmake-build-debug
CLI_PKG_DIR   := cli/dist
CLI_PKG_BUILD := cli/cmake-build-release

.PHONY: run cli package package-cli package-tar install

# ---- QML GUI App（开发暂停，仅源码可用，见 CLAUDE.md「GUI 开发暂停」） ----

run:
	@echo "注意: QML GUI 开发已暂停（2026-11-14），此目标仅供本地调试，不随发布包分发"
	rm -rf $(BUILD_DIR)
	cmake -B $(BUILD_DIR) -G Ninja -DCMAKE_PREFIX_PATH=$(QT_PREFIX) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR) --target mlc-gui
	./$(BUILD_DIR)/mlc-gui

# ---- CLI SDK ----

cli:
	rm -rf $(CLI_BUILD_DIR)
	rm -rf $(CLI_PKG_BUILD)
	cmake -B $(CLI_BUILD_DIR) -S . -G Ninja -DCMAKE_PREFIX_PATH=$(QT_PREFIX) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(CLI_BUILD_DIR) --target mlccore mlc

# ---- 打包发布 ----

package: package-cli
	@echo "打包完成，产物在 $(CLI_PKG_DIR)/"

# 发布构建默认不嵌入 CF key；CI 配了 Secret 时传 MLC_EMBED_CF_KEY=ON 嵌入
MLC_EMBED_CF_KEY ?= OFF

package-cli:
	rm -rf $(CLI_PKG_BUILD)
	cmake -B $(CLI_PKG_BUILD) -S . -G Ninja -DCMAKE_PREFIX_PATH=$(QT_PREFIX) -DCMAKE_BUILD_TYPE=Release -DMLC_EMBED_CF_KEY=$(MLC_EMBED_CF_KEY) -DMLC_STATIC_CORE=ON
	cmake --build $(CLI_PKG_BUILD) --target mlc
	mkdir -p $(CLI_PKG_DIR)
	cp $(CLI_PKG_BUILD)/cli/mlc $(CLI_PKG_DIR)/
	@echo "CLI 打包: $(CLI_PKG_DIR)/mlc (静态链接 libmlccore，单文件)"

# ---- 发布压缩包（零依赖：二进制 + 收编的 Qt/第三方库 + TLS 插件） ----
ARCH := $(shell uname -m | sed 's/arm64/aarch64/')
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PKG_OS := macos-$(ARCH)
BUNDLE_SCRIPT := bundle-dist-macos.sh
else
PKG_OS := linux-$(ARCH)
BUNDLE_SCRIPT := bundle-dist.sh
endif
package-tar: package-cli
	bash $(CLI_SRC)/$(BUNDLE_SCRIPT) $(CLI_PKG_DIR) $(QT_PREFIX)
	cp THIRD-PARTY-NOTICES.md $(CLI_PKG_DIR)/
	strip -s $(CLI_PKG_DIR)/mlc 2>/dev/null || true
	tar -cJf $(CLI_PKG_DIR)/mlc-$(PKG_OS).tar.xz -C $(CLI_PKG_DIR) mlc lib plugins THIRD-PARTY-NOTICES.md
	@echo "发布包: $(CLI_PKG_DIR)/mlc-$(PKG_OS).tar.xz"

# ---- 编译并安装到本机（~/.local/lib/mlc + ~/.local/bin/mlc） ----
# 其他电脑编译的场景：在那台机器 make package-tar，把 tar.gz 拷过来后
# 执行 bash cli/install.sh mlc-<os>.tar.gz 即可，无需克隆仓库
install: package-tar
	bash $(CLI_SRC)/install.sh $(CLI_PKG_DIR)/mlc-$(PKG_OS).tar.gz
