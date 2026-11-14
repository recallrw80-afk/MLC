// mlc — Minecraft launcher CLI frontend
// Links only libmlccore (QtCore + QtNetwork, no GUI/QML)
//
// 维护指南：新增命令只需两步（行为由注册表驱动）：
//   1. 写一个 int handleXxx(QStringList &args)（放进对应 cmd_*.cpp 组，commands.h 声明）
//   2. 在 commands.cpp 的 COMMANDS[] 加一条（用法/一句话/参数详解，中英双语）
// 总表（help）、参数详解（<命令> -h）、派发、mcFolder 守卫全部由 COMMANDS[] 生成。

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QLoggingCategory>
#include <iostream>

#include "commands.h"
#include "i18n.h"
#include "core/settings.h"
#include "core/versionmanager.h"

void printHelpTable();
void printCommandHelp(const QString &cmd);

// 表驱动派发：返回 -1 表示未知命令
static int dispatchCommand(const QString &cmd, QStringList &args) {
    const Command *found = nullptr;
    for (int i = 0; i < COMMANDS_COUNT; ++i) {
        if (cmd == QLatin1String(COMMANDS[i].name)) { found = &COMMANDS[i]; break; }
    }
    if (!found) return -1;
    if (!found->needMcFolder)
        return found->handler(args);

    // 组 B：解析游戏目录（--folder 一次性覆盖优先，否则读 Settings）
    QString mcFolder;
    QString folderArg = extractFolder(args);
    if (!folderArg.isEmpty())
        mcFolder = folderArg;
    else
        mcFolder = Settings::instance().getString("LaunchFolderSelect");

    if (mcFolder.isEmpty()) {
        std::cerr << T("error: 未设置游戏目录，请先执行 set-folder\n",
                       "error: no game folder set, run set-folder first\n").toStdString();
        return 1;
    }
    // --folder 是一次性覆盖，不写回配置（持久化只走 set-folder 命令）
    VersionManager::instance().setMcFolder(mcFolder, folderArg.isEmpty());

    return found->handler(args);
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("mlc");
    app.setApplicationVersion(GIT_DESCRIBE);

    // 默认静默，关闭 SDK 调试日志
    QLoggingCategory::setFilterRules("mlc.*.info=false\nmlc.*.debug=false");
    // 语言只由 set-lang 持久设置控制（无命令行 flag）
    Settings::initialize();
    if (Settings::instance().getString("UiLanguage") == "zh")
        setLang(false);

    // ---- 选项解析 ----
    QCommandLineParser parser;
    parser.setApplicationDescription(
        _("MLC 命令行启动器", "MLC Command-Line Launcher"));

    parser.addPositionalArgument("command", "placeholder");
    parser.setOptionsAfterPositionalArgumentsMode(
        QCommandLineParser::ParseAsPositionalArguments);
    parser.process(app);

    // ---- 命令派发 ----
    QStringList args = parser.positionalArguments();
    if (args.isEmpty()) { printHelpTable(); return 1; }

    // 任何命令带 -h/-help/--help 都只打印帮助（防"想看帮助却执行了真实操作"，如 update -help）
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "-h" || args[i] == "-help" || args[i] == "--help") {
            printCommandHelp(args.at(0));
            return 0;
        }
    }

    int ret = dispatchCommand(args.at(0), args);
    if (ret < 0) {
        std::cerr << _(QString("未知命令: %1\n").arg(args.at(0)).toStdString(),
                       QString("error: unknown command: %1\n").arg(args.at(0)).toStdString());
        printHelpTable();
        return 1;
    }
    return ret;
}
