// 命令注册表数据：新增命令在此加一条 + 写对应 handleXxx，即完成注册（总表/详解/派发自动生效）
#include "commands.h"
#include "test.h"
#include "i18n.h"

#include <QCoreApplication>
#include <iostream>

// ---- 共享小工具实现 ----

QString extractFlag(QStringList &args, const QString &flag) {
    int idx = args.indexOf(flag);
    if (idx < 0) return {};
    // flag 存在但缺少值（位于末尾，或后面紧跟另一个 flag）：视为「写了但没给值」，
    // 必须把 flag 本身从 args 移除，否则它会残留并被后续 args[1] 之类的位置参数
    // 读取误当成用户输入（如 `mlc inpack x.zip --to` 会把 "--to" 当实例名）
    if (idx + 1 >= args.size() || args.at(idx + 1).startsWith("--")) {
        args.removeAt(idx);
        return {};
    }
    QString value = args.at(idx + 1);
    args.removeAt(idx + 1);
    args.removeAt(idx);
    return value;
}
QString extractFolder(QStringList &args) { return extractFlag(args, "--folder"); }
QString extractRename(QStringList &args) { return extractFlag(args, "--r"); }

// ---- help / version 两个内置命令的 handler ----

void printHelpTable();  // help.cpp
static int handleHelpCmd(QStringList &) { printHelpTable(); return 0; }
static int handleVersionCmd(QStringList &) {
    std::cout << QCoreApplication::applicationName().toStdString() << " "
              << QCoreApplication::applicationVersion().toStdString() << std::endl;
    return 0;
}

const Command COMMANDS[] = {
    // ---- 实例 ----
    {"list", "list", "list",
     "列出已导入的整合包实例", "List imported instances",
     "用法: mlc list\n列出所有已导入的整合包实例（INI 映射中的）。",
     "Usage: mlc list\nList all imported modpack instances (from the INI mapping).",
     handleList, true},
    {"mods", "mods <名称>", "mods <name>",
     "列出实例的 Mod 及启用状态", "List mods of an instance with enabled state",
     "用法: mlc mods <名称>\n  <名称>  实例显示名（mlc list 里看到的）。",
     "Usage: mlc mods <name>\n  <name>  instance display name (as shown by mlc list).",
     handleMods, true},
    {"mc-list", "mc-list", "mc-list",
     "列出原版 MC 版本", "List vanilla MC versions",
     "用法: mlc mc-list\n列出已下载的原版 MC 版本与加载器版本。",
     "Usage: mlc mc-list\nList downloaded vanilla MC and loader versions.",
     handleMcList, true},
    {"launch", "launch [名称]", "launch [name]",
     "启动整合包游戏（不填写 name 则列出实例选择）",
     "Launch a modpack (If name is not filled in, the instance selection will be listed.)",
     "用法: mlc launch [名称]\n"
     "  [名称]  实例显示名或原版版本号；省略则上下键选择（非 TTY 退回输序号）。\n"
     "有外置登录态时自动用该账号启动（在线刷新），失败回退离线玩家。",
     "Usage: mlc launch [name]\n"
     "  [name]  instance display name or vanilla version; omit for the picker.\n"
     "With a persisted external login, launches with that account (auto-refresh); falls back to offline on failure.",
     handleLaunch, true},
    {"list-rm", "list-rm [名称|*] [--yes]", "list-rm [name|*] [--yes]",
     "删除实例（* 清空全部；无参上下键选择）", "Remove instance (* for all, select with arrows without args)",
     "用法: mlc list-rm [名称|*] [--yes]\n"
     "  [名称]  实例显示名；* = 删除全部（注意加引号防 shell 展开）；省略则上下键选择 + 二次确认。\n"
     "  --yes   非交互环境（脚本/管道）删除全部实例时的显式确认；交互式终端不需要（会弹确认）。",
     "Usage: mlc list-rm [name|*] [--yes]\n"
     "  [name]  instance display name; * = remove all (quote it); omit for picker + confirmation.\n"
     "  --yes   explicit confirmation for removing ALL instances in a non-interactive shell; not needed on a TTY.",
     handleRm, true},

    // ---- 下载与导入 ----
    {"inpack", "inpack <文件> [--r <名称>]", "inpack <file> [--r <name>]",
     "导入整合包", "Import modpack",
     "用法: mlc inpack <文件> [--r <名称>] [--to <实例>] [--folder <路径>]\n"
     "  <文件>       整合包路径（CF/Modrinth/MultiMC/HMCL/外壳包/压缩 .minecraft/纯 Mod 包）\n"
     "  --r <名称>   重命名实例（默认用包内名称）\n"
     "  --to <实例>  纯 Mod 包必填：目标实例（Mod 装进它的 mods/）\n"
     "  --folder <路径>  临时改用别的游戏目录（一次性，不写回配置）",
     "Usage: mlc inpack <file> [--r <name>] [--to <instance>] [--folder <path>]\n"
     "  <file>       modpack file (CF/Modrinth/MultiMC/HMCL/launcher-shell/compressed .minecraft/plain mods)\n"
     "  --r <name>   rename the instance\n"
     "  --to <inst>  required for plain-mod zips: target instance to receive the mods\n"
     "  --folder <path>  one-off game folder override (not persisted)",
     handleInpack, true},
    {"mc-install", "mc-install [版本]", "mc-install [version]",
     "下载原版 MC 版本（不带参数为最新正式版）", "Download a vanilla MC version (latest release without args)",
     "用法: mlc mc-install [版本]\n  [版本]  如 1.20.1；省略 = 最新正式版。重复执行 = 校验补齐缺失文件。",
     "Usage: mlc mc-install [version]\n  [version]  e.g. 1.20.1; omit for latest release. Re-running verifies/repairs files.",
     handleInstall, true},
    {"java-install", "java-install <大版本>", "java-install <major>",
     "下载安装 Java（Adoptium JRE）", "Download & install Java (Adoptium JRE)",
     "用法: mlc java-install <大版本>\n  <大版本>  如 8 / 17 / 21（从 Adoptium 下载 JRE 并注册）。",
     "Usage: mlc java-install <major>\n  <major>  e.g. 8 / 17 / 21 (downloads a JRE from Adoptium and registers it).",
     handleInstallJava, true},

    // ---- 服务端 ----
    {"server-install", "server-install [版本] [--forge|--fabric|--neoforge [版本]] [--from 实例]",
     "server-install [version] [--forge|--fabric|--neoforge [ver]] [--from inst]",
     "下载/安装服务端（原版或加载器；--from 复制实例 mod 与配置）",
     "Install a server (vanilla or loader; --from copies instance mods/config)",
     "用法: mlc server-install [版本] [--forge|--fabric|--neoforge [加载器版本]] [--from <实例>]\n"
     "  [版本]                MC 版本，如 1.20.1；省略 = 最新正式版\n"
     "  --forge / --fabric / --neoforge   装加载器服务端；裸写 = 自动最新加载器版本，带值 = 指定版本\n"
     "  --from <实例>         把该实例的 mods/config/defaultconfigs 复制进服务端目录\n"
     "产物在 {游戏目录}/servers/<标识>/，标识如 1.20.1 或 1.20.1-forge-47.3.0。\n"
     "注意：客户端专属 mod（Sodium 等渲染/界面类）会让服务端启动崩溃。",
     "Usage: mlc server-install [version] [--forge|--fabric|--neoforge [loader-ver]] [--from <instance>]\n"
     "  [version]             MC version, e.g. 1.20.1; omit for latest release\n"
     "  --forge/--fabric/--neoforge   loader server; bare flag = latest loader version, value = pinned\n"
     "  --from <instance>     copy the instance's mods/config/defaultconfigs into the server dir\n"
     "Output goes to {game folder}/servers/<id>/, e.g. 1.20.1 or 1.20.1-forge-47.3.0.\n"
     "Note: client-only mods (rendering/UI like Sodium) crash dedicated servers.",
     handleServerInstall, true},
    {"server-start", "server-start <版本> [--eula]", "server-start <version> [--eula]",
     "前台启动服务端（控制台直通；--eula 书面同意协议）",
     "Run server in foreground (console attached; --eula accepts the EULA)",
     "用法: mlc server-start <标识> [--eula]\n"
     "  <标识>   servers/ 下的目录名（原版=版本号，加载器=版本-加载器-版本）\n"
     "  --eula   书面同意 Minecraft EULA（首次启动必须；TTY 下也可交互确认）\n"
     "前台运行，控制台直通（/stop 关服）。首启自动写 online-mode=false。",
     "Usage: mlc server-start <id> [--eula]\n"
     "  <id>     directory name under servers/ (vanilla = version, loader = version-loader-ver)\n"
     "  --eula   accept the Minecraft EULA in writing (required on first start; TTY can confirm interactively)\n"
     "Runs in the foreground with an attached console (/stop to halt). First start writes online-mode=false.",
     handleServerStart, true},

    // ---- 玩家与外置登录 ----
    {"player-add", "player-add <名称>", "player-add <name>",
     "添加玩家配置（无参进入交互向导）", "Add player profile (interactive wizard without args)",
     "用法: mlc player-add [名称] [--avatar <路径>] [--skin <slim|wide|default>]\n"
     "  无参进入交互向导（名字→皮肤→头像→高级自定义 UUID）。首个玩家自动选中。",
     "Usage: mlc player-add [name] [--avatar <path>] [--skin <slim|wide|default>]\n"
     "  No args = interactive wizard (name → skin → avatar → advanced custom UUID). First player auto-selected.",
     handlePlayerAdd, false},
    {"player-edit", "player-edit [uuid|序号]", "player-edit [uuid|index]",
     "修改玩家配置（交互向导）", "Edit player profile (interactive wizard)",
     "用法: mlc player-edit [uuid|序号]\n交互向导修改，回车保留原值；无参上下键选择玩家。",
     "Usage: mlc player-edit [uuid|index]\nInteractive wizard; Enter keeps current value. No args = picker.",
     handlePlayerEdit, false},
    {"player-rm", "player-rm [uuid|序号]", "player-rm [uuid|index]",
     "删除玩家配置（无参上下键选择）", "Remove player profile (select with arrows without args)",
     "用法: mlc player-rm [uuid|序号]\n无参上下键选择 + 二次确认。删除选中玩家后自动选中剩余首个。",
     "Usage: mlc player-rm [uuid|index]\nNo args = picker + confirmation. Removing the selected player selects the first remaining.",
     handlePlayerRm, false},
    {"player-list", "player-list", "player-list",
     "列出玩家配置", "List player profiles",
     "用法: mlc player-list\n列出玩家（带序号，* 为当前选中）。",
     "Usage: mlc player-list\nList players (numbered, * marks the selected one).",
     handlePlayerList, false},
    {"player-select", "player-select <uuid|序号>", "player-select <uuid|index>",
     "选择当前玩家（按列表序号或 uuid）", "Select current player (by index or uuid)",
     "用法: mlc player-select [uuid|序号]\n选择当前玩家（离线模式启动用）；无参上下键选择。",
     "Usage: mlc player-select [uuid|index]\nSelect the active player (used for offline launches); no args = picker.",
     handlePlayerSelect, false},
    {"login", "login [服务器] [邮箱]", "login [server] [email]",
     "外置登录（authlib-injector，如 LittleSkin；登录态持久保存）",
     "External authlib-injector login (e.g. LittleSkin; session persisted)",
     "用法: mlc login [服务器] [邮箱]\n"
     "外置登录（authlib-injector，如 LittleSkin）。无参向导：服务器→邮箱→密码（掩码输入）。\n"
     "登录态加密持久化，launch 自动在线刷新。需交互终端。",
     "Usage: mlc login [server] [email]\n"
     "External authlib-injector login (e.g. LittleSkin). No args = wizard: server → email → password (masked).\n"
     "Session encrypted & persisted; launch auto-refreshes. Requires a TTY.",
     handleLogin, false},
    {"logout", "logout", "logout",
     "退出外置登录", "Log out the external account",
     "用法: mlc logout\n清除外置登录态，launch 回退离线玩家。",
     "Usage: mlc logout\nClear the external login; launches fall back to the offline player.",
     handleLogout, false},

    // ---- 配置与其他 ----
    {"set-folder", "set-folder <路径>", "set-folder <path>",
     "设置默认游戏目录", "Set default Minecraft folder",
     "用法: mlc set-folder <路径>\n设置默认游戏目录（持久保存）。",
     "Usage: mlc set-folder <path>\nSet the default game folder (persisted).",
     handleSetFolder, false},
    {"set-lang", "set-lang <en|zh>", "set-lang <en|zh>",
     "设置界面语言（持久保存）", "Set UI language (persistent)",
     "用法: mlc set-lang <en|zh>\n设置界面语言（持久保存）。",
     "Usage: mlc set-lang <en|zh>\nSet the UI language (persisted).",
     handleSetLang, false},
    {"set-mem", "set-mem <MB|auto>", "set-mem <MB|auto>",
     "设置游戏最大内存（auto=自动分配）", "Set max game memory (auto = automatic)",
     "用法: mlc set-mem <MB|auto>\n设置游戏最大内存；auto（默认）按可用内存 50% 分配（上限 16G）。",
     "Usage: mlc set-mem <MB|auto>\nSet max game memory; auto (default) = 50% of available RAM (cap 16G).",
     handleSetMem, false},
    {"set-cf-key", "set-cf-key <key|--clear>", "set-cf-key <key|--clear>",
     "设置/清除自定义 CurseForge key（无参查看来源）", "Set/clear custom CurseForge key (no arg = show source)",
     "用法: mlc set-cf-key <key|--clear>\n"
     "  <key>     设置自定义 CurseForge API key（加密存储，优先于编译期内嵌）\n"
     "  --clear   清除自定义 key，回退编译期内嵌/镜像\n"
     "  无参      显示当前 key 来源（不回显 key 本体）",
     "Usage: mlc set-cf-key <key|--clear>\n"
     "  <key>     set a custom CurseForge API key (encrypted, overrides the embedded one)\n"
     "  --clear   remove the custom key, fall back to embedded/mirror\n"
     "  no args   show the current key source (never prints the key itself)",
     handleSetCfKey, false},
    {"list-javas", "list-javas", "list-javas",
     "列出可用 Java", "List available Java runtimes",
     "用法: mlc list-javas\n列出检测到的 Java 运行时。",
     "Usage: mlc list-javas\nList detected Java runtimes.",
     handleListJavas, false},
    {"config", "config", "config",
     "查看当前配置", "Show current configuration",
     "用法: mlc config\n查看当前配置（版本/目录/内存/玩家/外置登录态等）。",
     "Usage: mlc config\nShow current config (version/folder/memory/players/login state).",
     handleConfig, false},
    {"report", "report [描述]", "report [description]",
     "生成 GitHub Issue 预填链接（附环境+日志）", "Create a prefilled GitHub issue link (env + logs attached)",
     "用法: mlc report [描述]\n生成 GitHub Issue 预填链接（自动附环境信息+最近启动日志，已脱敏）。",
     "Usage: mlc report [description]\nGenerate a prefilled GitHub issue link (env + recent launch log attached, sanitized).",
     handleReport, false},
    {"update", "update [-beta]", "update [-beta]",
     "检查并更新到最新版本（自动选源：GitHub 优先，失败自动走 Gitee 镜像）",
     "Check for and apply updates (auto source: GitHub first, Gitee mirror fallback)",
     "用法: mlc update [-beta]\n"
     "检查新版本并原地更新（仅 install.sh 安装的副本）。源自动降级：GitHub 优先，不可用自动切 Gitee 镜像。\n"
     "  -beta   允许更新到预发布版（默认只跟正式版；SemVer 比较：同数字段 正式版>预发布、beta<rc）",
     "Usage: mlc update [-beta]\n"
     "Check for updates and update in place (install.sh-installed copies only). Source auto-fallback: GitHub first, Gitee mirror on failure.\n"
     "  -beta   allow pre-releases (default tracks stable; SemVer: same version stable>pre, beta<rc)",
     handleUpdate, false},
    {"uninstall", "uninstall [-r]", "uninstall [-r]",
     "卸载 mlc（-r 保留游戏目录）", "Uninstall mlc (-r keeps game folder)",
     "用法: mlc uninstall [-r]\n  -r  保留游戏目录内容（默认连游戏目录一起清空）。仅 install.sh 安装的副本可用。",
     "Usage: mlc uninstall [-r]\n  -r  keep game folder contents (default clears them). install.sh-installed copies only.",
     handleUninstall, false},
    {"test", "test", "test",
     "全系统自检", "Run system self-test",
     "用法: mlc test\n全系统自检（含真实下载冒烟）；存在 FAIL 时退出码为 1。",
     "Usage: mlc test\nFull system self-check (incl. real downloads); exit code 1 on any FAIL.",
     [](QStringList &) -> int { return handleTest(); }, false},
    {"help", "help", "help",
     "显示帮助信息", "Show help information",
     "用法: mlc help\n显示命令总表。mlc <命令> -h 显示该命令的参数详解。",
     "Usage: mlc help\nShow the command list. mlc <command> -h shows detailed parameter help.",
     handleHelpCmd, false},
    {"version", "version", "version",
     "显示版本号", "Show version number",
     "用法: mlc version\n显示版本号（git describe 注入，如 v0.1.3 / v0.1.4-beta）。",
     "Usage: mlc version\nShow the version (injected via git describe, e.g. v0.1.3 / v0.1.4-beta).",
     handleVersionCmd, false},
};

const int COMMANDS_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);
