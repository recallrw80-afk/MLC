# Rust 重写计划：SDK + CLI（GUI 保持 Qt/QML）

> 2026-09 决定：`sdk/`（libmlccore）与 `cli/`（mlc）用 Rust 重写；`gui/`（QML）保持 Qt 不变，仅桥接层改走 C ABI。本文是迁移的唯一权威计划。

## 目标与动机

- 补平台缺口：Linux aarch64（Qt 官方无 6.11 ARM64 包，Rust 一等支持）、Windows 转正
- 发布物从"二进制 + 收编 Qt 库 + rpath 黑科技"简化为近似单文件静态二进制（Linux 用 musl）
- 摆脱对 QtCore/QtNetwork 的核心依赖（Qt 只留 GUI 壳）

## 目标架构

仓库根新增 `rust/` Cargo workspace，与现有 C++ 代码并存直至切换：

```
rust/
  Cargo.toml            # workspace
  crates/
    mlccore/            # lib：全部启动器逻辑（对应现有 sdk/）
    mlc/                # bin：clap CLI（对应现有 cli/）
    mlccore-ffi/        # cdylib：C ABI，供 Qt GUI 链接；cbindgen 生成 mlc_ffi.h
```

- GUI 侧：现有 4 个 bridge（instance/player/install/mod_platform）从直接调 C++ SDK 改为调 C ABI，QVariantMap 组装留在 Qt 侧，**QML 零改动**；进度/日志经回调函数指针转 Qt signal。
- CMake 用 corrosion（或直接 find_library）链接 Rust 产物。

## 硬约束（全程不可妥协）

1. **命令表面不变**：现有 30 个命令的名称、参数、中英 help、退出码逐条一致（清单在 `cli/src/commands.cpp` 的 `COMMANDS[]`）。
2. **磁盘格式兼容**：settings、玩家档案、实例目录布局、`.incomplete` 语义必须无缝读写现有数据。
3. **加密字节级兼容**：现有档案是 DES + base64，密钥为 `"MLC" + "Liunx"`（**`Liunx` 拼写错误是有意的兼容负载，禁止"修正"**——2026-09 已讨论定论）。受影响数据仅三项设置：`Authlib/AccessToken`、`Authlib/ClientToken`、`CfApiKey`（玩家档案不加密）。改密钥 = 全体用户重新登录 + CF key 丢失，而安全性零提升（硬编码密钥本就只是混淆）。若日后要真正提升存储安全，正确路线是换方案（随机密钥/OS keyring + 透明迁移），不是修拼写；且迁移仍需旧密钥解旧数据。Rust 实现必须先过与 C++ 输出逐字节对照的 golden 测试，再谈任何别的。
4. **回归门**：每个阶段结束，Rust `mlc test` 全绿 + 与旧 C++ 二进制对同一 mc 文件夹跑只读命令（`list`/`mods`/`mc-list`/`player-list`/`config` 等）输出 diff 干净。
5. 项目规矩不变：中文注释与提交信息、一提交一事、整合包失败整体回滚、驱动红线、GPL-3.0。

## 阶段 0 — 盘点与地基

- 盘点产出三张表：命令表（参数/输出/退出码）、`mlc.h` API 表、磁盘格式表（含加密格式与 `.incomplete` 流转）。
- 搭 workspace、rustfmt/clippy、CI 交叉编译矩阵：**linux x86_64 / aarch64（musl 静态）、windows x86_64（msvc）、macos universal2（lipo）**。
- 依赖清单（GPL-3.0 兼容，优先 MIT/Apache-2.0；延续"默认不引入"精神，每个依赖一句存在理由）：

| 依赖 | 存在理由 |
| --- | --- |
| clap | CLI 解析，复刻 30 个命令的参数与双语 help |
| serde + serde_json | 版本清单 / 整合包格式 / settings，全是 JSON |
| reqwest（rustls/ring）+ tokio + futures-util | 下载管理器需要并发 + 取消 + 进度回调；reqwest 0.12 的 rustls-tls 走 ring 提供者（windows-gnu 用 GNU as 汇编、无需 nasm，避免 aws-lc-rs 的 CMake 依赖），musl 全静态可达；async 染色核心库是可接受代价，FFI 边界用 runtime 句柄模式 |
| url | forgecdn→MCIM 回退的主机解析 |
| sha1 + base64 | 每个下载文件 SHA1 校验；DES 输出的 base64 编码 |
| zip + encoding_rs | 整合包解压 + GBK 中文文件名解码——**全平台单一代码路径，消灭 iconv 三分支** |
| xz2 + tar | 自更新 tar.xz 解包 |
| md-5 | 旧加密档案兼容的密钥派生（MD5 前 8 原始字节）；DES 块算法为 C++ 手写实现（自洽的非标准位序变体，标准 DES crate 不兼容），按原代码逐行移植、不引 crate（golden 向量在 `rust/crates/mlccore/tests/golden/`） |
| sysinfo | **仅**用于内存自动 sizing（可用内存 50% / 16G 上限需读物理内存）与游戏进程存活检测 |
| tracing | 结构化日志（替代 Qt 日志分类） |
| inquire / dialoguer + indicatif | 行式交互（复刻 create-vite 风格）+ 下载进度条 |
| dirs、which | 标准目录定位；PATH 中探测 java |

- **明确不引入**：tokio-tungstenite（全项目无 WebSocket 场景：服务器控制台是进程管道、OAuth 走 HTTP 设备码、启动器不讲 MC 协议）；Ratatui（全屏 TUI 框架，超出"行式交互 + 进度条"需求，且会改变 TTY 降级语义）。
- **文件/进程不引 crate**：`std::fs` 与 `std::process::Command` 本身跨平台；游戏启动、服务器控制台附加、`java -version` 探测均基于 std。
- **Java 管理器无现成插件**，也不需要：候选路径枚举（`JAVA_HOME`/PATH/注册表/`/usr/lib/jvm`/`java_home`）、`java -version` 解析、MC 版本兼容矩阵、Adoptium API 全是领域逻辑，基于 std 实现为独立可测模块。

## 阶段 1 — SDK 垂直切片（按依赖序，每片带单元测试）

1. **util**：file_utils / platform_utils / crypto_utils —— crypto 先行，golden 测试对照 C++ 输出。
2. **settings + types**：读写现有 settings 格式。
3. **下载层**：downloadmanager（SHA1 校验由 asset 层做"存在即跳过"——**C++ 无 HTTP Range 断点续传**；两阶段超时；**镜像回退仅 MCIM**：CF API 401/403/429 及 forgecdn 文件 CDN 最终失败均回退 `mod.mcimirror.top`，无 BMCLAPI）→ assetdownloader → modplatform。
4. **版本与 Java**：versionmanager（装/验/修）、javamanager（系统探测、版本兼容矩阵、Adoptium 自动下载）。
5. **启动**：launchbuilder + launcher —— 内存自动 sizing（可用内存 50%、上限 16G）、GC 档位、fcitx/ibus XIM 崩溃规避（GLFW 3.4 替换）、启动日志落盘，逐项对照移植。
6. **认证**：offline → authlib-injector（会话加密持久化 + 启动时在线刷新）→ Microsoft OAuth（**最大单点风险，最早做 spike 验证设备码与 token 刷新全流程**）。
7. **整合包管线**：detect → common → installers → pipeline。Forge/NeoForge/Fabric 安装器 processor 流程边缘 case 最多，放最后；`.incomplete` + 整体回滚语义原样保留。
8. **自更新**：update/uninstall。tar.xz 解包走 xz2；GitHub→Gitee 双源回退保留；**处理新旧包布局过渡**（旧 Qt 捆绑目录 → 新单文件的就地替换）。

## 阶段 2 — CLI

- clap 复刻 30 个命令；`-h` 详解逐条中英对照。
- TUI 用 inquire/dialoguer 复刻现有 create-vite 风格选择器与向导；无 TTY / 管道输入的降级行为对照旧实现。
- **尽早移植 `mlc test` 全系统自检**，它是阶段 1 后半程开始的回归门。
- launch 类命令加（或复刻）dry-run 能力，用于新旧二进制 JVM 参数 diff。

## 阶段 3 — GUI 对接（FFI）

- `mlccore-ffi`：为 GUI 用到的 API 子集（约 `mlc.h` 中 25 个函数）做 C ABI 包装；cbindgen 出头文件进构建。
- 改写 4 个 bridge + file_drop_handler；GUI 全页面手工回归（启动、下载进度、实例、Mod 平台）。
- 此阶段 C++ `sdk/` 只剩 GUI 这一条使用者，CLI 应已完全切到 Rust。

## 阶段 4 — 切换、发布与清理

- 打包：删除 `bundle-dist*.sh`、rpath `--disable-new-dtags`、Qt 收编逻辑；发布物 = 单文件 tar.xz（linux musl 全静态）。
- `install.sh`：识别 linux-aarch64 与 windows 包；Gitee 镜像逻辑保留。
- release CI：矩阵扩到 5 目标；CF key 用 `option_env!`/build.rs 编译期嵌入，语义与现状等价（无 key 回退 MCIM）。
- 文档：README（双语）改构建/安装节；CONTRIBUTING 依赖策略节改写；本计划标记完成。
- parity 达标后删除 C++ `sdk/` 与 `cli/`（含根 CMakeLists 中对应段），打 tag 发版。

## 迁移期规则（写进 AGENTS.md）

- C++ `sdk/`、`cli/` **功能冻结**：只修 bug，新功能一律进 Rust 核心。
- 每个 PR 自述影响：命令表面 / 磁盘格式 / 加密格式 三项有无触碰；触碰即需专项评审。
- 阶段门禁按"硬约束"第 4 条执行，不达标不进下一阶段。

## 主要风险与对策

| 风险 | 对策 |
| --- | --- |
| 加密格式不兼容（含 `Liunx` 密钥） | 阶段 1 第 1 片，golden 测试先行，不过不测后面 |
| Forge/NeoForge processor 管线边缘 case | 放最后做；用真实热门整合包做端到端验收集 |
| Microsoft OAuth 全流程 | 阶段 1 早期 spike，不过则重排后续计划 |
| TTY 交互行为差异 | 逐命令对照；无 TTY 降级列为测试项 |
| 自更新新旧布局过渡 | 老包→新包的就地升级列为专项测试 |
| "启动器能跑 ≠ 游戏能跑"（Linux ARM64 老版本无官方 LWJGL natives） | 文档注明；与语言无关，不在本期范围 |
