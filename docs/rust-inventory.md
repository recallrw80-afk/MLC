# Rust 重写盘点表（阶段 0 产出）

> 迁移的唯一事实来源。重写期间发现与 C++ 实现不符时，以代码为准并回改本表。
> 硬约束见 [rust-rewrite-plan.md](rust-rewrite-plan.md)：命令表面与磁盘/加密格式不可变。

## 表 1 — CLI 命令（30 个，`cli/src/commands.cpp` 的 `COMMANDS[]`）

handler 列为 `handleXxx` 所在文件；guard 列为注册表中的 mcFolder 守卫标志（true = 需游戏目录已就绪才允许执行）。

| 命令 | 用法 | handler | 文件 | guard |
| --- | --- | --- | --- | --- |
| list | `list` | handleList | cmd_instance.cpp | ✓ |
| mods | `mods <名称>` | handleMods | cmd_instance.cpp | ✓ |
| mc-list | `mc-list` | handleMcList | cmd_instance.cpp | ✓ |
| launch | `launch [名称]`（无参=选择器；有外置登录态自动在线刷新，失败回退离线） | handleLaunch | cmd_instance.cpp | ✓ |
| list-rm | `list-rm [名称\|*]`（无参=选择器+二次确认） | handleRm | cmd_instance.cpp | ✓ |
| inpack | `inpack <文件> [--r 名称] [--to 实例] [--folder 路径]` | handleInpack | cmd_instance.cpp | ✓ |
| mc-install | `mc-install [版本]`（空=最新正式版；重复执行=校验补齐） | handleInstall | cmd_instance.cpp | ✓ |
| java-install | `java-install <大版本>`（Adoptium JRE） | handleInstallJava | cmd_instance.cpp | ✓ |
| server-install | `server-install [版本] [--forge\|--fabric\|--neoforge [版本]] [--from 实例]` | handleServerInstall | cmd_server.cpp | ✓ |
| server-start | `server-start <标识> [--eula]`（前台控制台直通） | handleServerStart | cmd_server.cpp | ✓ |
| player-add | `player-add [名称] [--avatar 路径] [--skin slim\|wide\|default]`（无参=向导） | handlePlayerAdd | cmd_player.cpp | ✗ |
| player-edit | `player-edit [uuid\|序号]`（交互向导，回车保留原值） | handlePlayerEdit | cmd_player.cpp | ✗ |
| player-rm | `player-rm [uuid\|序号]`（无参=选择器+确认） | handlePlayerRm | cmd_player.cpp | ✗ |
| player-list | `player-list`（`*` 标记当前选中） | handlePlayerList | cmd_player.cpp | ✗ |
| player-select | `player-select [uuid\|序号]` | handlePlayerSelect | cmd_player.cpp | ✗ |
| login | `login [服务器] [邮箱]`（authlib-injector；无参=向导含密码掩码；需 TTY） | handleLogin | cmd_player.cpp | ✗ |
| logout | `logout` | handleLogout | cmd_player.cpp | ✗ |
| set-folder | `set-folder <路径>` | handleSetFolder | cmd_misc.cpp | ✗ |
| set-lang | `set-lang <en\|zh>` | handleSetLang | cmd_misc.cpp | ✗ |
| set-mem | `set-mem <MB\|auto>`（auto=可用内存 50%，上限 16G） | handleSetMem | cmd_misc.cpp | ✗ |
| set-cf-key | `set-cf-key <key\|--clear>`（无参=显示来源，不回显 key） | handleSetCfKey | cmd_misc.cpp | ✗ |
| list-javas | `list-javas` | handleListJavas | cmd_misc.cpp | ✗ |
| config | `config` | handleConfig | cmd_misc.cpp | ✗ |
| report | `report [描述]`（GitHub Issue 预填链接，环境+日志已脱敏） | handleReport | cmd_misc.cpp | ✗ |
| update | `update [-beta]`（仅 install.sh 安装副本；GitHub→Gitee 自动降级；SemVer：同数字段 正式版>预发布、beta<rc） | handleUpdate | cmd_misc.cpp | ✗ |
| uninstall | `uninstall [-r]`（-r 保留游戏目录；仅 install.sh 安装副本） | handleUninstall | cmd_misc.cpp | ✗ |
| test | `test`（含真实下载冒烟；任一 FAIL 退出码 1） | handleTest | test.cpp | ✗ |
| help | `help`；`mlc <命令> -h` 出详解 | handleHelpCmd | help.cpp | ✗ |
| version | `version`（git describe 注入） | handleVersionCmd | commands.cpp | ✗ |

共享参数解析：`extractFlag`（`--folder`/`--r` 为通用旗标提取器）。每个命令的中英双语一句话简介与 `-h` 详解全文在 `COMMANDS[]` 内，**Rust 侧逐条照抄**。

## 表 2 — SDK 公共 API（`sdk/include/mlc.h`，25 函数 + 6 类型）

类型：`ConfigInfo`、`ImportProgress{step,percent}`、`PlayerEntry{uuid,name,avatar,skinType}`、`InstanceInfo{dirName,path,version,modCount}`、`ModEntry{fileName,size,enabled}`、`AuthlibLoginInfo{loggedIn,server,name,uuid}`。
回调：`LogCallback(line)`、`ExitCallback(exitCode)`、`ImportCompleteCallback(ok,msg,data)`、`AuthlibLoginCallback(ok,errorOrName,info)`。

| 分组 | 函数 | 备注 |
| --- | --- | --- |
| 实例 | `listVersions()` | 读 INI [Instances] 映射，校验目录+PCL/Setup.ini |
| 实例 | `listMcVersions()` | 扫 versions/ 目录 |
| 实例 | `installVersion(versionId, onProgress)` | 同步；空串=最新 release；sha1 跳过=校验补齐 |
| 实例 | `launchVersion(versionId, onLog, onExit)` | 异步；离线模式 |
| 实例 | `removeInstance(name)` | 经 INI 映射找随机目录名后删除 |
| 实例 | `instanceInfo(displayName)` | dirName 空=不存在 |
| 实例 | `listMods(displayName)` | 按文件名排序；`.jar.disabled`=禁用 |
| 实例 | `setModEnabled(displayName, fileName, enabled)` | 重命名 `.jar`↔`.jar.disabled` |
| 实例 | `deleteMod(displayName, fileName)` | 限实例 mods/ 内，防路径穿越 |
| 导入 | `importModpack(filePath, instanceName, targetInstance, onProgress, onComplete)` | 异步；targetInstance 仅纯 Mod 包用 |
| Java | `installJavaRuntime(majorVersion, errOut)` | ≤0 按 8 处理；解压到 {mcFolder}/javas/ 并注册 |
| Java | `listJavas()` | 系统探测结果 |
| 配置 | `getConfig()` | 同步快照 |
| 玩家 | `listPlayers()` / `addPlayer(name,avatar,skinType,customUuid)` / `updatePlayer(...)` / `removePlayer(uuid)` / `selectPlayer(uuid)` | skinType: slim/wide/default；updatePlayer 支持 newUuid 迁移配置键 |
| 外置登录 | `loginAuthlib(server,username,password,cb)` / `logoutAuthlib()` / `currentAuthlibLogin()` | 成功后加密持久化；launch 时在线刷新、失败回退离线 |
| CF key | `setCfApiKey(key)` / `cfApiKeySource()` | 优先级：用户设置 > 编译期内嵌 > 无（MCIM 镜像）；**内嵌 key 禁止回显** |
| 服务端 | `installServer(versionId, loaderType, loaderVersion, onProgress)` / `startServer(versionId, onLog, onExit)` | 标识=版本 或 版本-加载器-加载器版本；首启自动写 online-mode=false |

FFI 子集：GUI 经 4 个 bridge 使用上表大部分函数（instance/player/install/mod_platform bridge + 进度回调）。`mlccore-ffi` 按此表出 C ABI。

## 表 3 — 磁盘格式

### 游戏目录（默认 `<可执行文件旁>/mc/`，`set-folder` 可改）

```
mc/
├── instances/    # 每个整合包实例一个随机目录名（隔离）
│   └── <随机名>/
│       ├── PCL/Setup.ini     # Version 键 = 启动用版本 json 名（实例解析入口）
│       ├── mods/             # .jar=启用，.jar.disabled=禁用
│       └── logs/latest.log   # 游戏自身日志
├── versions/       # 下载的 MC 版本（json/jar/natives）
├── libraries/      # 游戏库（实例间共享）
├── assets/         # 资源文件（共享）
├── javas/          # 自动下载的 Java 运行时
├── servers/<标识>/ # server-install 产物（标识=版本 或 版本-加载器-版本）
└── logs/           # mlc-launch-<时间戳>.log（全量启动日志，保留最近 10 份）
```

### 配置文件 `MLC.ini`（可执行文件旁，QSettings IniFormat）

**布局要点（2026-09 源码核实）**：QSettings 把完整键路径按第一个 `/` 分组，节 = 首段。因为 initialize 时 `beginGroup("MLC")`，**MLC 的全部键都在 `[MLC]` 一节内**，子路径（`Profile/<uuid>/Name`、`Instances/<dir>`、`Authlib/AccessToken`）以 `\` 嵌套键写入（如 `Profile\<uuid>\Name`）——不是独立节。键/节名按 UTF-16 码元转义（`/`→`\`、空格→`%20`、CJK→`%UXXXX` 大写、emoji→代理对各一个 `%U`）；**值不做 %U 转义**，原样 UTF-8（含 `;` `,` `=` 或首尾空格时加引号，`\n` 等控制字符用字母转义）。Rust 侧编解码已逐规则镜像（`rust/crates/mlccore/src/util/ini.rs`，规则源自 Qt 6.11.1 qsettings.cpp）。

| 完整键路径（文件内均落 `[MLC]` 节） | 含义 |
| --- | --- |
| `MLC/LaunchFolderSelect` | 默认游戏目录（缺省自动写 `<appdir>/mc/`） |
| `MLC/LoginType` | 登录类型（0=离线 Legacy） |
| `MLC/LaunchArgumentWindowType` / `Priority` / `Ram` | 启动默认值（1 / 1 / true） |
| `MLC/VersionRamOptimize` / `LaunchAdvanceGC` / `SystemLaunchCount` | 默认值 0 |
| `MLC/SelectedPlayer` | 当前选中玩家 UUID |
| `MLC/Profile/<uuid>/Name` `\|` `SkinType` `\|` `Avatar` | 玩家档案字段（**不加密**） |
| `MLC/Instances/<随机目录名>` | 随机目录名 → 显示名 映射（实例列表数据源） |
| `MLC/Instance_<id>/*` | 每实例覆盖设置（getInstance/setInstance） |
| `MLC/CfApiKey` | **DES+base64 加密**，用户自设 CF key |
| `MLC/Authlib/AccessToken`、`MLC/Authlib/ClientToken` | **DES+base64 加密**，外置登录会话 |

加密：DES-ECB + PKCS7 + base64；实际密钥 = MD5(UTF-8 "MLCLiunx") 的前 8 个原始字节（`"Liunx"` 拼写错误为有意兼容负载，见计划硬约束 3）。注意 C++ 的 DES 是手写实现（IP/FP 走 LSB 位序，自洽但非标准）——Rust 侧逐行镜像 + golden 测试（`rust/crates/mlccore/tests/golden/`）保证字节级兼容。QSettings INI 的转义规则（特殊字符 `%XX` 编码等）Rust 侧读写时必须复刻，建议 golden 测试覆盖。

### `.incomplete` 标记与回滚

- 导入开始：创建实例目录 + 写 `<实例目录>.incomplete` 标记文件（`modpack/common.cpp`）。
- 任一步失败：整体回滚（删目录与标记），不留半成品。
- 成功完成：删除标记。启动/列表逻辑应视带标记目录为未完成实例。
- 临时文件在 `tmp/<pid>/`，进程结束自清理（`pipeline.cpp`）。

### 其他约定

- 实例显示名 ↔ 随机目录名的映射只存在于 MLC.ini `[Instances]`；目录被手动删除时映射要容忍失效（当前目录=持久化默认目录时才清理失效映射，`--folder` 临时覆盖只过滤不清理）。
- 版本注入：`git describe --tags --always` + 7 位 commit，构建期写入（Rust 侧用 build.rs 复刻）。
