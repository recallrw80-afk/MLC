// 命令注册表：一个命令的全部元信息收在一条 Command 里，
// 帮助总表（help.cpp）、参数详解（help.cpp）、派发（main.cpp）都从 COMMANDS[] 生成
#ifndef MLC_CLI_COMMANDS_H
#define MLC_CLI_COMMANDS_H

#include <QStringList>

struct Command {
    const char *name;                 // 派发键（如 "launch"）
    const char *usageCn, *usageEn;    // 总表用法列（如 "launch [名称]" / "launch [name]"）
    const char *descCn, *descEn;      // 总表一句话描述
    const char *detailCn, *detailEn;  // -h 参数详解
    int (*handler)(QStringList &args);
    bool needMcFolder;                // 组 B：需要先解析并校验游戏目录
};

extern const Command COMMANDS[];
extern const int COMMANDS_COUNT;

// ---- 各命令处理函数（按组分布在 cmd_*.cpp） ----

int handleList(QStringList &args);
int handleMods(QStringList &args);
int handleMcList(QStringList &args);
int handleLaunch(QStringList &args);
int handleRm(QStringList &args);
int handleInstall(QStringList &args);
int handleInstallJava(QStringList &args);
int handleInpack(QStringList &args);

int handleServerInstall(QStringList &args);
int handleServerStart(QStringList &args);

int handlePlayerAdd(QStringList &args);
int handlePlayerEdit(QStringList &args);
int handlePlayerRm(QStringList &args);
int handlePlayerList(QStringList &args);
int handlePlayerSelect(QStringList &args);
int handleLogin(QStringList &args);
int handleLogout(QStringList &args);

int handleSetFolder(QStringList &args);
int handleSetLang(QStringList &args);
int handleSetMem(QStringList &args);
int handleSetCfKey(QStringList &args);
int handleListJavas(QStringList &args);
int handleConfig(QStringList &args);
int handleReport(QStringList &args);
int handleUpdate(QStringList &args);
int handleUninstall(QStringList &args);

// ---- 跨命令共享的小工具（实现在 commands.cpp） ----

// 从参数列表取出 --flag 的值并移除这对参数（无则返回空）
QString extractFlag(QStringList &args, const QString &flag);
QString extractFolder(QStringList &args);
QString extractRename(QStringList &args);

#endif // MLC_CLI_COMMANDS_H
