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

static std::optional<QString> extractLoaderFlag(QStringList &args, const QString &flag,
                                                QString &loaderType) {
    int i = args.indexOf(flag);
    if (i < 0) return std::nullopt;
    args.removeAt(i);
    loaderType = flag.mid(2);
    if (i < args.size() && !args[i].startsWith('-'))
        return args.takeAt(i);  // 显式版本
    return QString();           // 裸写：空串 = 自动最新
}

int handleServerInstall(QStringList &args) {
    QString loaderType, loaderVer, fromInstance;
    bool hasLoader = false;
    for (const auto &f : {"--forge", "--fabric", "--neoforge"}) {
        if (auto v = extractLoaderFlag(args, f, loaderType)) {
            hasLoader = true;
            loaderVer = *v;
            break;
        }
    }
    fromInstance = extractFlag(args, "--from");

    QString ver = args.size() >= 2 ? args[1] : QString();
    std::cout << (ver.isEmpty()
        ? _("正在下载最新版 MC 服务端 ...\n", "Downloading latest MC server ...\n")
        : _(QString("正在下载 MC %1 服务端 ...\n").arg(ver).toStdString(),
            QString("Downloading MC %1 server ...\n").arg(ver).toStdString()));
    bool ok = mlc::installServer(ver, hasLoader ? loaderType : QString(), loaderVer,
        [](const mlc::ImportProgress &p) {
            std::cout << "  " << p.step.toStdString() << "\n";
        });
    if (!ok) {
        std::cerr << _("error:  服务端安装失败\n", "error:  server install failed\n");
        return 1;
    }

    // --from <实例>：把该实例的 mods/config/defaultconfigs 复制进服务端目录
    if (!fromInstance.isEmpty()) {
        QString dirName = Settings::instance().dirForDisplayName(fromInstance);
        if (dirName.isEmpty()) {
            std::cerr << T("error:  实例 %1 不存在（服务端已装好，未复制 mod）\n",
                           "error:  instance %1 not found (server installed, mods not copied)\n")
                             .arg(fromInstance).toStdString();
            return 1;
        }
        QString instDir = VersionManager::instance().mcFolder() + "instances/" + dirName + "/";
        // 服务端标识 = 版本[-加载器-版本]：与 installServer 内部一致；
        // --from 需要显式版本号（ver 为空时刚装的是最新正式版，无法可靠拼目录名）
        if (ver.isEmpty()) {
            std::cerr << _("error:  --from 需要显式版本号（如 mlc server-install 1.20.1 --forge --from xxx）\n",
                           "error:  --from requires an explicit version\n");
            return 1;
        }
        QString serverId = ver;
        if (hasLoader) {
            // 重新解析一次加载器版本以拼目录名（installServer 内部解析的）
            // 简化：读取 servers/ 下以 ver-loaderType- 开头的目录
            QString prefix = ver + "-" + loaderType + "-";
            QDir sd(VersionManager::instance().mcFolder() + "servers");
            const auto entries = sd.entryList({prefix + "*"}, QDir::Dirs | QDir::NoDotAndDotDot);
            if (entries.isEmpty()) {
                std::cerr << _("error:  找不到服务端目录\n", "error:  server dir not found\n");
                return 1;
            }
            serverId = entries.first();
        }
        QString dst = VersionManager::instance().mcFolder() + "servers/" + serverId + "/";
        for (const auto &sub : {"mods", "config", "defaultconfigs"}) {
            if (QDir(instDir + sub).exists()) {
                std::cout << _("复制 ", "Copying ") << sub << " ..." << std::endl;
                FileUtils::copyDir(instDir + sub, dst + sub);
            }
        }
        std::cout << _("注意：客户端专属 mod（如 Sodium 等渲染类）会让服务端崩溃，启动失败请先删 mods/ 里的渲染/界面类 mod\n",
                       "Note: client-only mods (rendering/UI like Sodium) crash dedicated servers — remove them from mods/ if startup fails\n");
    }
    std::cout << "success" << std::endl;
    return 0;
}

int handleServerStart(QStringList &args) {
    if (args.size() < 2) {
        std::cerr << _("error:  mlc server-start <版本>\n", "error:  mlc server-start <version>\n");
        return 1;
    }
    QString ver = args[1];
    QString dir = VersionManager::instance().mcFolder() + "servers/" + ver + "/";
    if (!QDir(dir).exists()) {
        std::cerr << T("error:  服务端未安装，请先 mlc server-install %1\n",
                       "error:  server not installed, run mlc server-install %1 first\n")
                         .arg(ver).toStdString();
        return 1;
    }

    // EULA 闸门：--eula 直接书面同意；TTY 交互确认；否则拒绝
    bool eulaFlag = args.contains("--eula");
    // 只认真正的 eula=true 键：注释行 "#eula=true" 不算同意
    auto readEula = [&dir]() {
        QFile f(dir + "eula.txt");
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString all = QString::fromUtf8(f.readAll());
        f.close();
        for (const QString &raw : all.split('\n')) {
            QString line = raw;
            const int hash = line.indexOf('#');
            if (hash >= 0) line = line.left(hash);
            line = line.trimmed();
            if (line.isEmpty()) continue;
            const int eq = line.indexOf('=');
            if (eq < 0) continue;
            if (line.left(eq).trimmed().compare("eula", Qt::CaseInsensitive) == 0
                && line.mid(eq + 1).trimmed().compare("true", Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    bool accepted = readEula();
    if (!accepted && eulaFlag) {
        QFile f(dir + "eula.txt");
        if (f.open(QIODevice::WriteOnly)) f.write("eula=true\n");
        accepted = true;
    }
    if (!accepted) {
        std::cout << _("Minecraft 最终用户许可协议: https://aka.ms/MinecraftEULA\n",
                       "Minecraft EULA: https://aka.ms/MinecraftEULA\n");
        if (isatty(fileno(stdin))) {
            auto yes = tuiConfirm(_("我已阅读并同意 EULA", "I have read and agree to the EULA"), false);
            if (yes && *yes) {
                QFile f(dir + "eula.txt");
                if (f.open(QIODevice::WriteOnly)) f.write("eula=true\n");
                accepted = true;
            }
        } else {
            std::cerr << _("error:  请先阅读 EULA，并以 --eula 参数表示同意\n",
                           "error:  read the EULA first, then pass --eula to accept\n");
        }
        if (!accepted) return 1;
    }

    std::cout << T("正在启动服务端 %1（控制台直通，/stop 关服）...\n",
                   "Starting server %1 (console attached, /stop to halt)...\n").arg(ver).toStdString();
    if (!mlc::startServer(ver,
            [](const QString &line) { std::cout << line.toStdString() << "\n"; },
            [](int code) {
                std::cout << _("服务端已退出: ", "Server exited: ") << code << "\n";
                QCoreApplication::quit();
            })) {
        std::cerr << _("error:  服务端启动失败\n", "error:  failed to start server\n");
        return 1;
    }
    QCoreApplication::instance()->exec();  // 等服务端进程结束
    return 0;
}

// ---- 自身安装管理（uninstall / update） ----

// 安装根目录（install.sh 的落位）；不是该布局时拒绝卸载/更新（防止误动开发/分发副本）
