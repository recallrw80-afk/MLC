// 命令处理函数组（从 main.cpp 拆分；逻辑未动）
#include "commands.h"
#include "i18n.h"
#include "tui_select.h"
#include "tui_prompt.h"
#include "mlc.h"
#include "core/settings.h"
#include "core/versionmanager.h"
#include "download/downloadmanager.h"
#include "util/file_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUrl>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <unistd.h>

int handleList(QStringList &args) { Q_UNUSED(args);
    auto ids = mlc::listVersions();
    if (ids.isEmpty()) {
        std::cout << T("(No instances)\n", "(No instances)\n").toStdString();
    } else {
        std::cout << T("Instances:\n", "Instances:\n").toStdString();
        for (const auto &id : ids)
            std::cout << "  " << id.toStdString() << "\n";
    }
    return 0;
}

int handleMcList(QStringList &args) { Q_UNUSED(args);
    auto ids = mlc::listMcVersions();
    if (ids.isEmpty()) {
        std::cout << T("(No vanilla MC versions)\n",
                       "(No vanilla MC versions)\n").toStdString();
    } else {
        std::cout << T("Vanilla MC versions:\n",
                       "Vanilla MC versions:\n").toStdString();
        for (const auto &id : ids)
            std::cout << "  " << id.toStdString() << "\n";
    }
    return 0;
}

int handleMods(QStringList &args) {
    QString name = args.size() >= 2 ? args.at(1) : QString();
    if (name.isEmpty()) {
        auto ids = mlc::listVersions();
        std::cerr << T("用法: mlc mods <实例名>\n", "usage: mlc mods <instance>\n").toStdString();
        for (const auto &id : ids)
            std::cerr << "  " << id.toStdString() << "\n";
        return ids.isEmpty() ? 0 : 1;
    }
    auto info = mlc::instanceInfo(name);
    if (info.dirName.isEmpty()) {
        std::cerr << T("error: 实例不存在: ", "error: instance not found: ").toStdString()
                  << name.toStdString() << "\n";
        return 1;
    }
    auto mods = mlc::listMods(name);
    std::cout << name.toStdString() << T(" 的 Mod（共 ", " mods (").toStdString()
              << mods.size() << T(" 个）:\n", "):\n").toStdString();
    if (mods.isEmpty())
        std::cout << T("  (无 Mod)\n", "  (no mods)\n").toStdString();
    for (const auto &m : mods) {
        std::cout << "  " << (m.enabled ? "[on]  " : "[off] ")
                  << m.fileName.toStdString() << "  ("
                  << QString::number(m.size / 1048576.0, 'f', 2).toStdString() << " MB)\n";
    }
    return 0;
}

int handleLaunch(QStringList &args) {
    QString target;
    if (args.size() < 2) {
        // 未指定实例：TTY 用上下键 TUI 选择，非 TTY（管道）退回输序号。
        // 列表 = 实例 + 原版/加载器版本（launchVersion 解析时实例名优先，原版走 loadVersion）
        auto ids = mlc::listVersions();
        const auto mcIds = mlc::listMcVersions();
        for (const auto &v : mcIds)
            if (!ids.contains(v)) ids << v;
        if (ids.isEmpty()) {
            std::cerr << _("error:  没有实例，请先导入整合包\n",
                           "error:  no instances, import a modpack first\n");
            return 1;
        }
        int choice = -1;
        if (isatty(fileno(stdin))) {
            choice = tuiSelect("选择要启动的实例", ids);
            if (choice < 0) {
                std::cerr << _("已取消\n", "Cancelled\n");
                return 1;
            }
        } else {
            std::cout << _("选择要启动的实例:\n", "Select an instance to launch:\n");
            for (int i = 0; i < ids.size(); ++i)
                std::cout << "  " << (i + 1) << ") " << ids[i].toStdString() << "\n";
            std::cout << _("请输入序号: ", "Enter number: ");
            std::cout.flush();
            std::string line;
            std::getline(std::cin, line);
            bool okNum = false;
            int n = QString::fromStdString(line).toInt(&okNum);
            if (!okNum || n < 1 || n > ids.size()) {
                std::cerr << _("error:  无效的选择\n", "error:  invalid choice\n");
                return 1;
            }
            choice = n - 1;
        }
        target = ids[choice];
    } else {
        target = args[1];
    }
    std::cout << _(QString("正在启动 %1 ...\n").arg(target).toStdString(),
                   QString("Launching %1 ...\n").arg(target).toStdString());
    if (!mlc::launchVersion(target,
            [](const QString &line) { std::cout << "[MC] " << line.toStdString() << std::endl; },
            [](int code) {
                std::cout << _("exit: ", "exit: ") << code << "\n";
                QCoreApplication::quit();
            })) {
        std::cerr << T("error: launch failed\n", "error: launch failed\n").toStdString();
        return 1;
    }
    std::cout << "success" << std::endl;
    QCoreApplication::instance()->exec(); // 等待游戏进程结束
    return 0;
}

int handleInpack(QStringList &args) {
    if (args.size() < 2) {
        std::cerr << _("error:  mlc inpack <文件> [--r <名称>] [--to <实例>] [--folder <路径>]\n",
                       "error:  mlc inpack <file> [--r <name>] [--to <instance>] [--folder <path>]\n");
        return 1;
    }
    QString rename = extractRename(args);
    QString to = extractFlag(args, "--to");
    if (args.size() < 2) {  // --r/--to/--folder 移除后可能没有文件参数
        std::cerr << _("error:  mlc inpack <文件> [--r <名称>] [--to <实例>] [--folder <路径>]\n",
                       "error:  mlc inpack <file> [--r <name>] [--to <instance>] [--folder <path>]\n");
        return 1;
    }
    std::cout << _("正在导入整合包...\n", "Importing modpack...\n");

    for (;;) {
        bool done = false;
        int  result = 1;
        QString retryTo;  // mod 包场景：用户在 TUI 里选中的目标实例
        mlc::importModpack(args[1], rename, to,
            [](const mlc::ImportProgress &p) {
                int bars = p.percent / 5;
                std::cout << "\r  [";
                for (int i = 0; i < 20; ++i)
                    std::cout << (i < bars ? "=" : i == bars ? ">" : " ");
                std::cout << "] " << p.percent << "% " << p.step.toStdString();
                if (p.percent >= 100) std::cout << std::endl;
                std::cout.flush();
            },
            [&](bool ok, const QString &msg, const QStringList &data) {
                if (ok) {
                    std::cout << std::endl << _("success: ", "success: ")
                              << msg.toStdString() << std::endl;
                    result = 0;
                } else {
                    // mod 包缺 --to：TTY 弹上下键选择实例后重试；否则按原样报错
                    if (msg.contains("--to") && !data.isEmpty() && isatty(fileno(stdin))) {
                        int pick = tuiSelect(msg, data);
                        if (pick >= 0) retryTo = data[pick];
                        done = true;
                        QCoreApplication::quit();
                        return;
                    }
                    std::cerr << std::endl << _("error: ", "error: ")
                              << msg.toStdString() << std::endl;
                    // mod 包缺 --to 且无 TTY：输出当前实例列表（先判断有没有实例）
                    if (msg.contains("--to")) {
                        if (data.isEmpty()) {
                            std::cout << _("（当前没有实例，请先导入整合包）\n",
                                           "(no instances yet, import a modpack first)\n");
                        } else {
                            std::cout << _("当前实例:\n", "Current instances:\n");
                            for (const auto &d : data)
                                std::cout << "  " << d.toStdString() << "\n";
                        }
                    }
                }
                done = true;
                QCoreApplication::quit();
            });

        if (!done) QCoreApplication::instance()->exec(); // 等待异步下载完成

        if (retryTo.isEmpty()) return result;
        // 用户已选择目标实例，重试导入
        to = retryTo;
        std::cout << _("以实例 ", "Retrying with target instance ")
                  << to.toStdString() << _(" 为目标重新导入...\n", " ...\n");
    }
}

int handleRm(QStringList &args) {
    // --yes：非交互环境显式确认批量删除。先摘除，避免它被后续 args[1] 当成实例名
    const bool assumeYes = args.removeAll("--yes") > 0;
    if (args.size() < 2) {
        // 无参：TTY 上下键选择要删的实例（二次确认）；非 TTY 报用法
        if (!isatty(fileno(stdin))) {
            std::cerr << _("error:  mlc list-rm <名称|*>\n",
                           "error:  mlc list-rm <name|*>\n");
            return 1;
        }
        auto ids = mlc::listVersions();
        if (ids.isEmpty()) {
            std::cout << _("没有可删除的实例\n", "No instances to remove\n");
            return 0;
        }
        int pick = tuiSelect(_("选择要删除的实例", "Select an instance to remove"), ids);
        if (pick < 0) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        auto yes = tuiConfirm(QString(_("确定删除 %1 吗", "Remove %1")).arg(ids[pick]), false);
        if (!yes || !*yes) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        if (mlc::removeInstance(ids[pick])) {
            std::cout << "success" << std::endl;
            return 0;
        }
        std::cerr << _("error: 实例不存在或删除失败\n",
                       "error: instance not found or removal failed\n");
        return 1;
    }
    // shell 会把不带引号的 * 展开成当前目录文件列表（多个参数）——检测并提示加引号
    if (args.size() > 2) {
        std::cerr << _("error:  参数过多（shell 会展开 *）。删除全部实例请加引号：mlc list-rm \"*\"\n",
                       "error:  too many arguments (shell expands *). To remove all instances, quote it: mlc list-rm \"*\"\n");
        return 1;
    }
    if (args[1] == "*") {
        auto ids = mlc::listVersions();
        if (ids.isEmpty()) {
            std::cout << _("没有可删除的实例\n", "No instances to remove\n");
            return 0;
        }
        // 通配删全部是不可逆的批量操作——必须先确认。
        // （单条删除在 TTY 分支有二次确认，这里原先却直接删，误输入即全丢）
        std::cout << _("即将删除以下全部实例（不可恢复）:\n",
                       "About to delete ALL of the following instances (irreversible):\n");
        for (const auto &id : ids)
            std::cout << "  - " << id.toStdString() << "\n";
        std::cout.flush();
        if (isatty(fileno(stdin))) {
            auto yes = tuiConfirm(QString(_("确认删除全部 %1 个实例", "Delete all %1 instances"))
                                      .arg(ids.size()), false);
            if (!yes || !*yes) {
                std::cerr << _("已取消\n", "Cancelled\n");
                return 1;
            }
        } else if (!assumeYes) {
            // 非 TTY（脚本/管道）：要求显式 --yes，避免管道里误触发全删
            std::cerr << _("error:  非交互环境删除全部实例需加 --yes 显式确认\n",
                           "error:  removing all instances non-interactively requires --yes\n");
            return 1;
        }
        int removed = 0;
        for (const auto &id : ids) {
            if (mlc::removeInstance(id)) removed++;
        }
        std::cout << _(QString("已删除 %1 个实例\n").arg(removed).toStdString(),
                       QString("Removed %1 instance(s)\n").arg(removed).toStdString());
        return 0;
    }
    if (mlc::removeInstance(args[1])) {
        std::cout << "success" << std::endl;
        return 0;
    }
    std::cerr << _("error: 实例不存在或删除失败\n",
                   "error: instance not found or removal failed\n");
    return 1;
}

int handleInstall(QStringList &args) {
    // 不带参数 = 最新正式版（SDK 解析 latest.release）
    QString ver = args.size() >= 2 ? args[1] : QString();
    std::cout << (ver.isEmpty()
        ? _("正在下载最新版 MC ...\n", "Downloading latest MC ...\n")
        : _(QString("正在下载 MC %1 ...\n").arg(ver).toStdString(),
            QString("Downloading MC %1 ...\n").arg(ver).toStdString()));
    bool ok = mlc::installVersion(ver,
        [](const mlc::ImportProgress &p) {
            if (p.percent >= 0) {
                int bars = p.percent / 5;
                std::cout << "\r  [";
                for (int i = 0; i < 20; ++i)
                    std::cout << (i < bars ? "=" : i == bars ? ">" : " ");
                std::cout << "] " << p.percent << "% " << p.step.toStdString();
                std::cout.flush();
            } else {
                std::cout << "\r  " << p.step.toStdString() << "                    ";
                std::cout.flush();
            }
        });
    std::cout << std::endl;
    if (ok) {
        std::cout << "success" << std::endl;
        return 0;
    }
    std::cerr << _("error:  下载失败（版本不存在或网络错误）\n",
                   "error:  download failed (version not found or network error)\n");
    return 1;
}

int handleInstallJava(QStringList &args) {
    if (args.size() < 2) {
        std::cerr << _("error:  mlc java-install <大版本>\n",
                       "error:  mlc java-install <major>\n");
        return 1;
    }
    bool okNum = false;
    int major = args[1].toInt(&okNum);
    if (!okNum || major <= 0) {
        std::cerr << _("error:  无效的 Java 大版本\n", "error:  invalid Java major version\n");
        return 1;
    }
    std::cout << _(QString("正在下载 JRE %1 ...\n").arg(major).toStdString(),
                   QString("Downloading JRE %1 ...\n").arg(major).toStdString());
    QString err;
    if (!mlc::installJavaRuntime(major, &err)) {
        std::cerr << _("error:  ", "error: ") << err.toStdString() << std::endl;
        return 1;
    }
    std::cout << "success" << std::endl;
    return 0;
}

// ---- 命令派发 ----

/// 解析 mcFolder 并派发到对应处理函数。
/// 返回值 >= 0 表示已处理（退出码），-1 表示未知命令。
