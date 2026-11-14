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

// ---- 玩家 Profile 命令 ----

// 玩家配置向导（create-vite 风格问答，仅 TTY；existing 非空 = 编辑模式，各问取现值为默认）
// 交互流程全在 CLI 层，SDK 只提供 add/update/list 数据接口
struct WizardResult { QString name, avatar, skin, customUuid; };
static bool playerWizard(const mlc::PlayerEntry *existing, WizardResult &out) {
    auto name = tuiInput(_("玩家名字", "Player name"),
                         existing ? existing->name : QString(),
                         _("必填", "required"));
    if (!name) return false;
    if (name->isEmpty()) {
        std::cerr << _("error: 玩家名字不能为空\n", "error: player name required\n");
        return false;
    }

    // 皮肤类型：上下键单选
    static const QStringList skins = {"slim", "wide", "default"};
    int skinIdx = tuiSelect(_("皮肤类型", "Skin type"), skins,
                            existing ? skins.indexOf(existing->skinType) : 0);
    if (skinIdx < 0) return false;

    auto avatar = tuiInput(_("头像路径", "Avatar path"),
                           existing ? existing->avatar : QString(),
                           _("可留空", "optional"));
    if (!avatar) return false;

    QString customUuid;
    auto adv = tuiConfirm(_("需要高级配置吗", "Advanced options"), false);
    if (!adv) return false;
    if (*adv) {
        auto u = tuiInput(_("自定义 UUID", "Custom UUID"),
                          existing ? existing->uuid : QString(),
                          _("留空自动生成", "empty = auto"));
        if (!u) return false;
        customUuid = *u;
    }

    out = {*name, *avatar, skins[skinIdx], customUuid};
    return true;
}

int handlePlayerAdd(QStringList &args) {
    if (args.size() < 2) {
        // 无参：TTY 进交互向导，非 TTY（脚本/管道）报用法
        if (!isatty(fileno(stdin))) {
            std::cerr << _("error:  mlc player-add <名称> [--avatar <路径>] [--skin <slim|wide|default>]\n",
                           "error:  mlc player-add <name> [--avatar <path>] [--skin <slim|wide|default>]\n");
            return 1;
        }
        WizardResult w;
        if (!playerWizard(nullptr, w)) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        auto entry = mlc::addPlayer(w.name, w.avatar, w.skin, w.customUuid);
        std::cout << _("已添加玩家:\n", "Player added:\n")
                  << "  UUID: " << entry.uuid.toStdString() << "\n"
                  << "  " << _("名称: ", "Name: ") << entry.name.toStdString() << "\n"
                  << "  Skin: " << entry.skinType.toStdString() << "\n";
        if (!entry.avatar.isEmpty())
            std::cout << "  " << _("头像: ", "Avatar: ") << entry.avatar.toStdString() << "\n";
        return 0;
    }
    QString avatar = extractFlag(args, "--avatar");
    QString skin = extractFlag(args, "--skin");
    if (args.size() < 2) {
        std::cerr << _("error: 缺少玩家名称\n", "error: missing player name\n");
        return 1;
    }
    if (!skin.isEmpty() && skin != "slim" && skin != "wide" && skin != "default") {
        std::cerr << _("error: 皮肤类型必须是 slim / wide / default\n",
                       "error: skin type must be slim / wide / default\n");
        return 1;
    }
    auto entry = mlc::addPlayer(args[1], avatar, skin.isEmpty() ? "slim" : skin);
    std::cout << _("已添加玩家:\n", "Player added:\n")
              << "  UUID: " << entry.uuid.toStdString() << "\n"
              << "  " << _("名称: ", "Name: ") << entry.name.toStdString() << "\n"
              << "  Skin: " << entry.skinType.toStdString() << "\n";
    if (!entry.avatar.isEmpty())
        std::cout << "  " << _("头像: ", "Avatar: ") << entry.avatar.toStdString() << "\n";
    return 0;
}

// 把"序号或 uuid"解析成玩家 uuid（序号 = player-list 中的 1 起始编号）
static QString resolvePlayerUuid(const QString &idOrIndex) {
    bool isNum = false;
    int n = idOrIndex.toInt(&isNum);
    if (isNum && n >= 1) {
        auto players = mlc::listPlayers();
        if (n <= players.size()) return players[n - 1].uuid;
    }
    return idOrIndex;  // 不是有效序号则按 uuid 处理
}

int handlePlayerRm(QStringList &args) {
    if (args.size() < 2) {
        // 无参：TTY 上下键选择要删的玩家（二次确认）；非 TTY 报用法
        if (!isatty(fileno(stdin))) {
            std::cerr << _("error:  mlc player-rm <uuid|序号>\n",
                           "error:  mlc player-rm <uuid|index>\n");
            return 1;
        }
        auto players = mlc::listPlayers();
        if (players.isEmpty()) {
            std::cerr << _("error:  没有玩家配置\n", "error:  no player profiles\n");
            return 1;
        }
        QStringList names;
        for (const auto &p : players) names << p.name;
        int pick = tuiSelect(_("选择要删除的玩家", "Select a player to remove"), names);
        if (pick < 0) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        auto yes = tuiConfirm(QString(_("确定删除 %1 吗", "Remove %1")).arg(players[pick].name), false);
        if (!yes || !*yes) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        if (mlc::removePlayer(players[pick].uuid)) {
            std::cout << "success" << std::endl;
            return 0;
        }
        std::cerr << _("error: UUID 或序号不存在\n", "error: UUID or index not found\n");
        return 1;
    }
    QString uuid = resolvePlayerUuid(args[1]);
    if (mlc::removePlayer(uuid)) {
        std::cout << "success" << std::endl;
        return 0;
    }
    std::cerr << _("error: UUID 或序号不存在\n", "error: UUID or index not found\n");
    return 1;
}

int handlePlayerEdit(QStringList &args) {
    auto players = mlc::listPlayers();
    if (players.isEmpty()) {
        std::cerr << _("error:  没有玩家配置，请先 player-add\n",
                       "error:  no player profiles, run player-add first\n");
        return 1;
    }
    // 定位目标：参数为 uuid|序号；无参时 TTY 上下键选择
    QString uuid;
    if (args.size() >= 2) {
        uuid = resolvePlayerUuid(args[1]);
    } else {
        if (!isatty(fileno(stdin))) {
            std::cerr << _("error:  mlc player-edit <uuid|序号>\n",
                           "error:  mlc player-edit <uuid|index>\n");
            return 1;
        }
        QStringList names;
        for (const auto &p : players) names << p.name;
        int pick = tuiSelect(_("选择要修改的玩家", "Select a player to edit"), names);
        if (pick < 0) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        uuid = players[pick].uuid;
    }
    const mlc::PlayerEntry *existing = nullptr;
    for (const auto &p : players)
        if (p.uuid == uuid) { existing = &p; break; }
    if (!existing) {
        std::cerr << _("error: UUID 或序号不存在\n", "error: UUID or index not found\n");
        return 1;
    }

    WizardResult w;
    if (!playerWizard(existing, w)) {
        std::cerr << _("已取消\n", "Cancelled\n");
        return 1;
    }
    if (!mlc::updatePlayer(uuid, w.name, w.avatar, w.skin, w.customUuid)) {
        std::cerr << _("error: 修改失败（UUID 冲突？）\n", "error: update failed (UUID conflict?)\n");
        return 1;
    }
    std::cout << "success" << std::endl;
    return 0;
}

int handlePlayerList(QStringList &args) { Q_UNUSED(args);
    auto players = mlc::listPlayers();
    if (players.isEmpty()) {
        std::cout << _("（无玩家配置）\n", "(No player profiles)\n");
        return 0;
    }
    QString selected = Settings::instance().selectedPlayer();
    int idx = 1;
    for (const auto &p : players) {
        bool isSel = (p.uuid == selected);
        std::cout << (isSel ? " * " : "   ") << idx++ << ") "
                  << p.uuid.toStdString() << "\n"
                  << "     " << _("名称: ", "Name: ") << p.name.toStdString() << "\n";
        if (!p.avatar.isEmpty())
            std::cout << "     " << _("头像: ", "Avatar: ") << p.avatar.toStdString() << "\n";
        std::cout << "     Skin: " << p.skinType.toStdString() << "\n";
    }
    return 0;
}

int handlePlayerSelect(QStringList &args) {
    QString uuid;
    if (args.size() < 2) {
        // 无参：TTY 弹上下键选择（非 TTY 提示用法）
        auto players = mlc::listPlayers();
        if (players.isEmpty()) {
            std::cerr << _("error:  没有玩家配置，请先 player-add\n",
                           "error:  no player profiles, run player-add first\n");
            return 1;
        }
        if (!isatty(fileno(stdin))) {
            std::cerr << _("error:  mlc player-select <uuid|序号>\n",
                           "error:  mlc player-select <uuid|index>\n");
            return 1;
        }
        QStringList names;
        for (const auto &p : players) names << p.name;
        int pick = tuiSelect("选择当前玩家", names);
        if (pick < 0) {
            std::cerr << _("已取消\n", "Cancelled\n");
            return 1;
        }
        uuid = players[pick].uuid;
    } else {
        uuid = resolvePlayerUuid(args[1]);
    }
    if (!mlc::selectPlayer(uuid)) {
        std::cerr << _("error:  玩家 UUID 或序号不存在\n", "error:  player UUID or index not found\n");
        return 1;
    }
    std::cout << "success" << std::endl;
    return 0;
}

// ---- 需要 mcFolder 的命令 ----

int handleLogin(QStringList &args) {
    auto cur = mlc::currentAuthlibLogin();
    if (cur.loggedIn) {
        std::cout << T("当前已登录: %1 @ %2\n", "Currently logged in: %1 @ %2\n")
                         .arg(cur.name, cur.server).toStdString();
        if (isatty(fileno(stdin))) {
            auto again = tuiConfirm(_("重新登录？", "Log in again?"), false);
            if (!again || !*again) return 0;
        } else {
            std::cout << _("切换账号请先 mlc logout\n", "Run mlc logout first to switch accounts\n");
            return 0;
        }
    }

    // 位置参数可预填服务器/邮箱；密码只走交互输入（不进命令行与 shell 历史）
    QString server = args.size() >= 2 ? args[1] : QString();
    QString email  = args.size() >= 3 ? args[2] : QString();
    if (!isatty(fileno(stdin))) {
        std::cerr << _("error:  login 需要交互终端（用法: mlc login [服务器] [邮箱]）\n",
                       "error:  login requires an interactive terminal (usage: mlc login [server] [email])\n");
        return 1;
    }
    if (server.isEmpty()) {
        auto s = tuiInput(_("外置登录服务器", "Auth server"),
                          "https://littleskin.cn/api/yggdrasil",
                          _("如 LittleSkin", "e.g. LittleSkin"));
        if (!s) { std::cerr << _("已取消\n", "Cancelled\n"); return 1; }
        server = *s;
    }
    while (server.endsWith('/')) server.chop(1);
    if (email.isEmpty()) {
        auto e = tuiInput(_("邮箱 / 用户名", "Email / username"), {}, _("必填", "required"));
        if (!e) { std::cerr << _("已取消\n", "Cancelled\n"); return 1; }
        email = *e;
    }
    auto p = tuiPassword(_("密码", "Password"));
    if (!p) { std::cerr << _("已取消\n", "Cancelled\n"); return 1; }
    if (email.isEmpty() || p->isEmpty()) {
        std::cerr << _("error:  邮箱和密码不能为空\n", "error:  email and password must not be empty\n");
        return 1;
    }

    std::cout << _("正在登录...\n", "Logging in...\n");
    bool done = false, ok = false;
    QString msg;
    QEventLoop loop;
    QPointer<QEventLoop> guard = &loop;
    mlc::loginAuthlib(server, email, *p,
        [&](bool o, const QString &m, const mlc::AuthlibLoginInfo &) {
            ok = o; msg = m; done = true;
            if (guard) guard->quit();
        });
    if (!done) loop.exec();  // done 标志防同步完成导致裸等

    if (ok) {
        std::cout << T("登录成功: %1 @ %2\n启动游戏将使用此外置账号（每次启动自动在线刷新）。\n",
                       "Logged in: %1 @ %2\nLaunches will use this account (auto-refreshed online).\n")
                         .arg(msg, server).toStdString();
        return 0;
    }
    std::cerr << T("error:  登录失败: %1\n", "error:  login failed: %1\n").arg(msg).toStdString();
    return 1;
}

int handleLogout(QStringList &args) {
    Q_UNUSED(args);
    auto cur = mlc::currentAuthlibLogin();
    if (!cur.loggedIn) {
        std::cout << _("当前没有外置登录态\n", "No external login to clear\n");
        return 0;
    }
    mlc::logoutAuthlib();
    std::cout << T("已退出外置账号 %1，启动将回退为离线玩家\n",
                   "Logged out %1; launches fall back to the offline player\n")
                     .arg(cur.name).toStdString();
    return 0;
}

// ---- 服务端（本地开服） ----

// 解析 --forge/--fabric/--neoforge：可带值（指定版本）也可裸写（自动最新）
