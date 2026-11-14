# mlc — Linux 上的 Minecraft 启动器

[English](README.md) | **简体中文**

**MinecraftLauncherCLI（简称 MLC）** 是一个用 C++/Qt 编写的独立开发 Minecraft 启动器，开发过程中参考了 [Plain Craft Launcher (PCL)](https://github.com/Hex-Dragon/PCL2) 的公开实现思路。`mlc` 是它的命令行前端：不依赖图形界面，几 MB 内存即可运行，支持整合包导入、多版本管理和游戏启动。

## 功能特性

- **整合包导入**：CurseForge / Modrinth / MCBBS / MultiMC / HMCL / 启动器外壳包 / 压缩 `.minecraft` / 纯 Mod 包，自动完成游戏本体、Modloader（Forge/NeoForge/Fabric）、Mod 下载
- **多实例管理**：每个实例独立目录，互不干扰；支持批量删除
- **原版下载**：任意 MC 版本一键下载安装、校验补齐
- **Java 管理**：自动检测系统 Java，按 MC 版本自动选择；缺失时自动从 Adoptium 下载安装
- **多玩家配置**：多个离线玩家档案，支持皮肤类型（slim/wide）
- **外置登录**：authlib-injector（如 LittleSkin）账号登录，登录态加密持久化，启动自动在线刷新
- **本地开服**：原版/Forge/Fabric/NeoForge 服务端一键安装与前台启动，控制台直通，可从实例复制 mods/config
- **断点续传与校验**：所有下载按 SHA1 校验，重复操作自动跳过已有文件
- **中文/英文界面**：`set-lang zh` 一键切换中文

## 环境要求

- Linux（x86_64 / aarch64）
- 运行游戏需要 Java 运行时（没有也没关系，启动时会自动下载）

## 安装

### 一键安装（推荐）

下载并安装官方预编译包——**完整体验**（内嵌 CurseForge key，Mod 下载走官方 API）：

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/latest/download/install.sh | bash
```

**国内网络**：下载源自动降级——GitHub 优先，不可用（限流/超时）自动切 Gitee 镜像，无需任何参数：

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/latest/download/install.sh | bash
```

国内装**测试版**（加 `--beta`，源同样自动降级）：

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/download/v0.1.3-beta/install.sh | bash -s -- --beta
```

> Gitee 镜像只提供预编译二进制（不提供源码包，也不支持从它源码构建）。源码构建请走 GitHub 克隆。

装最新**预发布版**（Beta）：`latest` 地址不含预发布，install.sh 需从 tag 地址下载并加 `--beta`：

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/download/v0.1.3-beta/install.sh | bash -s -- --beta
```

预编译包支持 x86_64（即 amd64）与 macOS（Apple Silicon）；Linux aarch64 请走源码编译（Qt 官方无 6.11 Linux ARM64 安装包）。完成后任意目录直接：

```bash
mlc help
```

### 预编译包手动安装

从 [Releases](https://github.com/recallrw80-afk/MLC/releases/latest) 下载对应架构的 `mlc-linux-<arch>.tar.xz`，与 `install.sh` 放同一目录：

```bash
bash install.sh mlc-linux-x86_64.tar.xz
```

### 源码编译安装

适合改代码/自定义构建。**注意**：本地编译的包不内嵌 CF key（CurseForge 自动走 MCIM 镜像，功能可用但非最佳链路），追求完整体验请用上面的官方包。

```bash
git clone https://github.com/recallrw80-afk/MLC.git
cd MLC
make install        # 编译 Release → 零依赖打包 → 安装到本机
```

依赖：Qt 6.11+（Core/Network）、CMake 3.16+、Ninja、C++20 编译器、nlohmann-json 3.11+、ZLIB。Qt 路径默认 `$HOME/Qt/6.11.1/gcc_64`，可用 `make install QT_PREFIX=/path/to/Qt/6.x/gcc_64` 覆盖。

其他电脑编译的包拷到本机安装：编译机 `make package-tar` 生成 `cli/dist/mlc-linux-<arch>.tar.xz`，拷贝后 `bash cli/install.sh mlc-linux-<arch>.tar.xz`。

### 更新与卸载

```bash
mlc update         # 检查 GitHub Releases 新版本并原地更新（仅 install.sh 安装的副本）
mlc uninstall      # 卸载（清空游戏目录）
mlc uninstall -r   # 卸载但保留游戏目录内容
```

### 常用构建命令

```bash
make cli            # 构建 mlc + libmlccore.so（Debug）
make package-tar    # 生成零依赖发布包 cli/dist/mlc-linux-<arch>.tar.xz
make run            # 构建并启动 QML GUI（开发暂停，仅源码可用，二进制名 mlc-gui）
```

## 快速上手

```bash
# 1. 设置游戏目录（默认是程序旁的 ./mc/，可省略）
mlc set-folder /home/yourname/mc

# 2. 导入一个整合包
mlc inpack ~/Downloads/某某整合包.zip

# 3. 看看有哪些实例
mlc list

# 4. 启动！
mlc launch
# 或者直接指定：
mlc launch 实例名
```

没有整合包？先装个原版：

```bash
mlc mc-install          # 最新正式版
mlc mc-install 1.20.1   # 指定版本
mlc launch 1.20.1
```

## 本地开服

不需要图形界面，两条命令开一个能进人的服务端。

### 1. 安装服务端

```bash
# 原版
mlc server-install 1.20.1

# 加载器（Forge / Fabric / NeoForge）——裸写自动选最新加载器版本，也可带值指定
mlc server-install 1.20.1 --forge
mlc server-install 1.20.1 --fabric 0.16.9
```

安装产物在 `{游戏目录}/servers/<标识>/`：原版标识就是版本号（`1.20.1`），加载器标识带后缀（`1.20.1-forge-47.3.0`）。

要把你正在玩的整合包搬进服务端，加 `--from <实例名>`——会把该实例的 `mods/`、`config/`、`defaultconfigs/` 复制进服务端目录：

```bash
mlc server-install 1.20.1 --forge --from 我的整合包
```

> **注意**：客户端专属 mod（Sodium 等渲染/界面类）会让服务端启动崩溃。启动失败就先从服务端的 `mods/` 里删掉它们。

### 2. 启动

```bash
mlc server-start 1.20.1-forge-47.3.0
```

- **首次启动**要求同意 [Minecraft EULA](https://aka.ms/MinecraftEULA)：交互终端里回答确认，或加 `--eula` 参数书面同意（只问一次，写入 `eula.txt` 后不再问）
- 首次启动自动写最小 `server.properties`（含 `online-mode=false`，这是离线和外置登录玩家能进服的前提；文件已存在则不覆盖你的改动）
- 控制台直通终端：直接敲 `/stop`、`/op 名字`、`/whitelist ...` 等服务端命令
- Java 按版本矩阵自动选择；内存默认 2G（`set-mem` 可改）

### 3. 让别人连进来

- 同一局域网：直接连 `<你的内网IP>:25565`
- 公网：需要公网地址 + 路由器端口转发（TCP 25565），或 frp 类内网穿透（SakuraFrp / playit.gg 等）

### 4. 安全提醒（重要）

`online-mode=false` 的服在公网上**任何人输任何名字都能进，包括冒名顶替**。开服后请立刻开白名单：

```
whitelist on
whitelist add 朋友的名字
```

## 使用示例

四个常见场景的完整指令流。

### 场景 1：玩整合包

```bash
mlc set-folder ~/mc                          # 设置游戏目录（可选，默认程序旁的 ./mc/）
mlc inpack ~/Downloads/ATM9.zip              # 导入整合包（自动下齐游戏本体/Forge/Mod）
mlc list                                     # 查看已有实例
mlc mods "All the Mods 9"                    # 查看实例的 Mod 列表和启用状态
mlc launch "All the Mods 9"                  # 启动（也可省略名称，上下键选择）
```

### 场景 2：快速玩原版

```bash
mlc mc-install 1.20.1                        # 下载 1.20.1（无参 = 最新正式版）
mlc launch 1.20.1                            # 启动
```

### 场景 3：开服和朋友玩

```bash
mlc server-install 1.20.1 --forge            # 装 Forge 服务端（原版去掉 --forge）
mlc server-start 1.20.1-forge-47.3.0         # 首次会问 EULA（或加 --eula）

# 服务端跑起来后，直接在终端敲控制台命令：
whitelist on                                  # 开白名单（公网必做）
whitelist add 小明                            # 加朋友进白名单
op 小明                                       # 给管理权限
stop                                          # 关服
```

朋友在游戏里「多人游戏 → 添加服务器」填 `<你的IP>:25565`。

### 场景 4：外置登录（LittleSkin 等）

```bash
mlc login                                    # 向导：服务器地址 → 邮箱 → 密码（默认 LittleSkin）
mlc launch 我的整合包                         # 启动自动用外置账号（每次启动在线刷新）
mlc config                                   # 查看当前登录状态
mlc logout                                   # 退出登录，回退离线玩家
```

### 场景 5：日常维护

```bash
mlc update                                   # 检查并更新 mlc 自身
mlc test                                     # 全系统自检（出问题先跑这个）
mlc report 启动1.20.1闪退                     # 生成预填 Issue 链接（自动附环境+日志，已脱敏）
mlc uninstall -r                             # 卸载但保留游戏内容
```

## 命令参考

任何命令都可以加 `-h`（或 `--help`）打印该命令的参数详解，如 `mlc inpack -h`。

### 实例

| 命令 | 说明 |
|---|---|
| `list` | 列出所有实例 |
| `mods <名称>` | 列出实例的 Mod（文件大小 + 启用/禁用状态） |
| `list-rm [名称\|*]` | 删除实例（无参时上下键选择 + 二次确认；`*` 删除全部，注意加引号） |
| `mc-list` | 列出已下载的原版 MC 版本 |
| `launch [名称]` | 启动游戏；不填名称时**上下键选择**（列表含实例 + 原版/加载器版本；非 TTY 退回输序号） |

### 下载与导入

| 命令 | 说明 |
|---|---|
| `inpack <文件> [--r <名称>] [--to <实例>] [--folder <路径>]` | 导入整合包；`--r` 重命名实例；Mod 包需 `--to` 指定目标实例 |
| `mc-install [版本]` | 下载原版 MC 版本（不带参数为最新正式版） |
| `java-install <大版本>` | 下载安装 Java（Adoptium JRE） |
| `server-install [版本] [--forge\|--fabric\|--neoforge [版本]] [--from 实例]` | 下载/安装服务端（无参=最新正式版；加载器裸写=自动最新；`--from` 复制实例的 mods/config） |
| `server-start <标识> [--eula]` | 前台启动本地服务端（控制台直通，`/stop` 关服；`--eula` 表示同意 EULA；标识如 `1.20.1` 或 `1.20.1-forge-47.4.10`） |

### Java

| 命令 | 说明 |
|---|---|
| `list-javas` | 列出检测到的 Java |

### 玩家

| 命令 | 说明 |
|---|---|
| `player-add [名称] [--avatar <路径>] [--skin <slim\|wide\|default>]` | 添加玩家；不带参数进入交互向导（名字 → 皮肤 → 头像 → 高级配置自定义 UUID） |
| `player-edit [uuid\|序号]` | 修改玩家配置（交互向导，回车保留原值；无参时上下键选择玩家） |
| `player-rm [uuid\|序号]` | 删除玩家（无参时上下键选择 + 二次确认） |
| `player-list` | 列出玩家（带序号，`*` 为当前选中） |
| `player-select <uuid\|序号>` | 选择当前玩家（可用序号；无参时上下键选择） |

### 配置与其他

| 命令 | 说明 |
|---|---|
| `set-folder <路径>` | 设置默认游戏目录 |
| `set-lang <en\|zh>` | 设置界面语言（默认英文） |
| `set-mem <MB\|auto>` | 设置游戏最大内存；`auto`（默认）按可用内存 50% 自动分配（上限 16G） |
| `set-cf-key <key\|--clear>` | 设置/清除自定义 CurseForge key（无参查看当前来源；优先于编译期内嵌） |
| `login [服务器] [邮箱]` | 外置登录（authlib-injector，如 LittleSkin；token 加密持久保存，启动自动在线刷新） |
| `logout` | 退出外置登录，回退离线玩家 |
| `update [-beta] [-cn]` | 检查并原地更新（仅 install.sh 安装的副本；`-beta` 含预发布，`-cn` 走 Gitee 国内镜像） |
| `uninstall [-r]` | 卸载启动器；`-r` 保留游戏目录内容 |
| `config` | 查看当前配置 |
| `report [描述]` | 生成 GitHub Issue 预填链接（自动附环境信息+最近启动日志，已脱敏） |
| `test` | 全系统自检 |
| `help` / `version` | 帮助 / 版本号 |

## 游戏文件放在哪

默认在启动器旁的 `mc/` 目录（可用 `set-folder` 修改）：

```
mc/
├── instances/      # 你导入的每个整合包实例（独立目录）
├── versions/       # 下载的 MC 版本
├── libraries/      # 游戏依赖库（多实例共享）
├── assets/         # 游戏资源（多实例共享）
├── javas/          # 自动下载的 Java
├── servers/        # 本地服务端（server-install 的产物，每个版本/加载器一个目录）
└── logs/           # 启动器日志（mlc-launch-*.log，滚动保留 10 份）
```

游戏自己的日志在 `<实例>/logs/latest.log`（MC/mod 标准位置）；启动器每次 `launch` 另写一份完整会话日志（启动命令行 + 全部游戏输出 + 退出码）到 `mc/logs/mlc-launch-<时间戳>.log`，排查启动问题看这个。

## 常见问题

**导入 CurseForge 整合包需要 API key 吗？**
不需要。官方发布的预编译包内嵌了 key（走官方 API，最快最稳）；源码构建默认不带 key，自动使用 MCIM 镜像下载。key 失效或被吊销时会自动回退镜像重试，无需手动处理。

**界面怎么变成中文？**
`mlc set-lang zh`（一次设置，永久生效）。

**有图形界面吗？**
QML GUI（`gui/`，二进制 `mlc-gui`）目前**开发暂停**（2026-11-14 决定），重心是 CLI + SDK。

- **不在发布包里**：`make package-tar` 只打包命令行版 `mlc`，预编译包没有 `mlc-gui`，所以安装脚本不会注册该命令。
- **源码仍可用**：开发者可以 `make run` 在本地构建启动（需要本机完整 Qt 6.11）。
- 代码全部保留，恢复开发前需先解决 GUI 的零依赖分发问题（缺 Qt 平台插件、QML 模块等），详见仓库根目录 `CLAUDE.md` 的「GUI 开发暂停」一节。

日常使用请走 CLI：`mlc help` 查看全部命令。

**导入失败会怎样？**
任一下载环节失败（游戏文件/Modloader/Mod）都会整体回滚，不会留下装了一半的实例，重试即可。

**没有图形界面怎么开服联机？整合包服务端行不行？**
都行，见上文「本地开服」一节：`server-install` + `server-start` 两条命令，加载器服务端加 `--forge/--fabric/--neoforge`，搬实例 mod 加 `--from`。

## 许可与声明

本项目以 [GNU GPL v3](../LICENSE) 开源。本项目是独立开发的开源软件：与 Mojang / Microsoft 无关联（"Minecraft" 是 Mojang Synergies AB 的商标），与 [PCL](https://github.com/Hex-Dragon/PCL2) 官方亦无隶属或授权关系。按"原样"提供，不承担使用责任（见许可证第 15、16 条）。
