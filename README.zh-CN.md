# MLC — MinecraftLauncherCLI

[English](README.md) | **简体中文**

一个用 C++20 / Qt 6 编写的跨平台 Minecraft 启动器，以轻量命令行前端 `mlc`（RSS ~4MB）为核心，适合桌面、服务器与无显示环境使用。

## 目录结构（2026-11-14 交换）

仓库以 **Rust 为主**（Rust 在根目录），C++ 实现收进 `cpp/` 作为参考/过渡：

```
MLC/
├── Cargo.toml crates/       # Rust workspace（主开发线）：mlccore + mlc + mlccore-ffi
├── README.md release.sh     # 根层与 Rust 主线相关的文件
└── cpp/                     # C++/Qt 实现（迁移期保留，功能冻结）
    ├── sdk/ cli/ gui/       # 原 C++ 核心、CLI、QML GUI
    └── CMakeLists.txt Makefile
```

> 构建 C++ 版需先 `cd cpp`；Rust 版直接在根目录 `cargo build`。

## 特性

- **CLI 优先**：整合包导入、实例管理、版本下载、游戏启动全部可在终端完成；TTY 下有交互式选择器与向导（create-vite 风格）
- **整合包导入**：CurseForge / Modrinth / MultiMC / HMCL / PCL 外壳包 / 压缩 `.minecraft` / 纯 Mod 包；自动完成游戏本体、Modloader（Forge / NeoForge / Fabric）与 Mod 下载
- **多实例隔离**：每个实例独立目录；`.incomplete` 标记 + 失败整体回滚，不留半成品
- **原版下载**：任意 MC 版本一键安装，不带参数下载最新正式版；重复执行即校验补齐
- **Java 管理**：自动检测系统 Java、按 MC 版本兼容矩阵选择；缺失时自动从 Adoptium 下载 JRE
- **多玩家档案**：多个离线玩家配置（名称 / 头像 / 皮肤类型），交互式增删改选；支持 authlib-injector 外置登录（如 LittleSkin，登录态加密持久化，启动自动在线刷新）
- **启动稳定性**：自动内存分配（可用内存 50%，上限 16G）、GC 档位、fcitx/ibus XIM 崩溃自动修复（GLFW 3.4 替换）、每次启动落盘完整日志
- **本地开服**：原版/Forge/Fabric/NeoForge 服务端一键安装与前台启动（`server-install`/`server-start`），控制台直通，headless 环境友好
- **QML GUI**（开发暂停，见下）：源码已保留，未进发布包

> **GUI 开发状态：已暂停（2026-11-14）**
>
> 图形界面（`gui/` 下的 QML App，二进制名 `mlc-gui`）目前**暂停开发**，当前重心是 CLI 与 SDK。
>
> - **未进入发布流程**：`make package-tar` / `release.sh` 只打包命令行版 `mlc`，预编译包里**没有** `mlc-gui`，普通用户装不到它。
> - **仅源码可用**：开发者仍可 `make run` 在本地构建运行，功能为测试版（启动页、下载页、设置页、更多页的基础框架）。
> - **暂停并不等于放弃**：代码、QML 组件、C++ 桥接层全部保留在仓库中，随时可恢复。
>
> 恢复开发前需要先解决 GUI 的零依赖分发问题（详见 `CLAUDE.md` 的「GUI 开发暂停」一节）：当前 `bundle-dist.sh` 只收编了 `plugins/tls/`，没有任何 Qt **平台插件**（`platforms/`）、Wayland shell 集成插件或 QML 模块，直接打包 GUI 会因找不到平台插件而无法启动。
>
> 日常使用请走 CLI：`mlc help` 查看全部命令。

## 安装

### 一键安装（推荐）

官方预编译包（内嵌 CurseForge key，完整体验），支持 x86_64（即 amd64）与 macOS（Apple Silicon）。Linux aarch64 用户请走下方源码编译（Qt 官方没有 6.11 的 Linux ARM64 安装包，暂无预编译）：

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/latest/download/install.sh | bash
```

国内网络：下载源自动降级——GitHub 优先，不可用自动切 Gitee 镜像，无需参数（测试版加 `--beta`）。

> Gitee 镜像只提供预编译二进制（不提供源码包，也不支持从它源码构建）。源码构建请走 GitHub：`git clone https://github.com/recallrw80-afk/MLC.git`

装最新**预发布版**（Beta）：注意 `latest` 地址不含预发布，install.sh 需从 tag 地址下载：

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/download/v0.1.3-beta/install.sh | bash -s -- --beta
```

（把 `v0.1.3-beta` 换成最新预发布 tag；`--beta` 让脚本走列表接口找到预发布包。）

也可到 [Releases](https://github.com/recallrw80-afk/MLC/releases/latest) 手动下载压缩包，与 `install.sh` 放一起后 `bash install.sh mlc-linux-<arch>.tar.xz`。

### 源码编译安装

适合改代码/自定义构建。注意：本地编译的包不内嵌 CF key（CurseForge 自动走 MCIM 镜像），完整体验请用官方包。

```bash
git clone https://github.com/recallrw80-afk/MLC.git
cd MLC/cpp
make install        # 编译 Release → 零依赖打包 → 安装到本机
```

依赖：Qt 6.11+（Core/Network）、CMake 3.16+、Ninja、C++20 编译器、nlohmann-json 3.11+、ZLIB。Qt 路径默认 `$HOME/Qt/6.11.1/gcc_64`，可用 `make install QT_PREFIX=/path/to/Qt/6.x/gcc_64` 覆盖。

其他电脑编译的包拷到本机：`bash cpp/cli/install.sh mlc-linux-<arch>.tar.xz`。

### 更新与卸载

```bash
mlc update         # 检查 GitHub Releases 新版本并原地更新（仅 install.sh 安装的副本）
mlc uninstall      # 卸载（清空游戏目录）
mlc uninstall -r   # 卸载但保留游戏目录内容
```

### 常用构建命令

```bash
cd cpp              # C++ 工程在 cpp/ 下（Rust 主线在根目录用 cargo）
make cli            # 构建 mlc + libmlccore.so（Debug）
make package-tar    # 生成零依赖发布包 cpp/cli/dist/mlc-linux-<arch>.tar.xz
make run            # 构建并启动 QML GUI（开发暂停，仅源码可用，二进制名 mlc-gui）
```

## 快速上手

```bash
# 导入整合包（游戏目录默认程序旁的 ./mc/，可用 set-folder 修改）
mlc inpack ~/Downloads/某某整合包.zip

# 列出实例并交互式选择启动
mlc launch

# 或者玩原版
mlc mc-install        # 最新正式版
mlc launch 1.20.1
```

完整命令参考见 [cpp/cli/README.md](cpp/cli/README.md)。任何命令加 `-h` 可打印该命令的参数详解（如 `mlc inpack -h`）；`mlc help` 查看命令总表。

## 文档

- [cpp/cli/README.md](cpp/cli/README.md) — CLI 命令参考、目录结构、FAQ
- [CONTRIBUTING.md](CONTRIBUTING.md) — 开发约定与贡献指南

## 贡献

欢迎 Issue 与 PR。提交前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 免责声明

- 本项目不是 Minecraft 官方产品，与 Mojang Studios / Microsoft 没有任何关联，亦未获得其认可或授权。"Minecraft" 是 Mojang Synergies AB 的商标。
- 本项目与 Plain Craft Launcher (PCL) 官方无隶属或授权关系。
- 本软件按"原样"提供，作者不对使用本软件造成的任何损失承担责任（详见许可证第 15、16 条）。

## 致谢

- [Plain Craft Launcher (PCL)](https://github.com/Hex-Dragon/PCL2) — 实现思路参考
- [PCL Community Edition](https://github.com/PCL-Community/PCL2-CE) — 部分方案参考（如 MCIM 镜像）
- Qt、nlohmann-json、zlib、Adoptium、BMCLAPI/MCIM 镜像等上游项目

## 许可证

[GNU General Public License v3.0](LICENSE)

版权所有 (C) 2026 MLC 作者。本程序为自由软件：你可以在自由软件基金会发布的 GNU 通用公共许可证第 3 版条款下再分发和/或修改它。衍生作品必须以相同许可证开源。
