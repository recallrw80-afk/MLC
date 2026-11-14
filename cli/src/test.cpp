// test 自检子系统：纯函数单测 + 合成包冒烟（从 main.cpp 拆出）
#include "test.h"
#include "i18n.h"

#include "mlc.h"
#include "modpack.h"
#include "core/settings.h"
#include "core/versionmanager.h"
#include "util/arg_utils.h"
#include "util/file_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <iostream>

// ---- test 自检（CLI 层，逐条验证命令） ----

// ---- 冒烟测试辅助（合成包，几 KB，不依赖本机大文件） ----

struct ImportResult { bool success = false; bool done = false; QString message; QStringList data; };

// 同步等待一次导入完成（带超时；回调捕获 QSharedPointer/QPointer，超时后回调不再访问栈对象）
static ImportResult importAndWait(const QString &filePath, const QString &name,
                                   const QString &to = {}, int timeoutMs = 600000) {
    auto state = QSharedPointer<ImportResult>::create();
    QEventLoop loop;
    QPointer<QEventLoop> loopGuard = &loop;
    mlc::importModpack(filePath, name, to,
        [](const mlc::ImportProgress &) {},
        [state, loopGuard](bool ok, const QString &msg, const QStringList &data) {
            state->success = ok; state->message = msg; state->data = data; state->done = true;
            if (loopGuard) loopGuard->quit();
        });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    if (!state->done) loop.exec();  // 同步失败路径下 quit 先于 exec，不能裸等
    if (!state->done) { state->message = "import timeout"; }
    return *state;
}

// 生成最小 CurseForge 整合包（1 个真实小 mod JEI；badMod=true 时附加一个必失败的 mod；
// forgeVer 可用于构造必失败的 modloader 版本）
static QString makeSyntheticCfPack(const QString &workDir, bool badMod,
                                   const QString &forgeVer = "47.4.10") {
    QDir().mkpath(workDir + "/overrides");
    QFile mf(workDir + "/manifest.json");
    if (!mf.open(QIODevice::WriteOnly)) return {};
    QByteArray files = R"({"projectID": 238222, "fileID": 8419086, "required": true})";
    if (badMod)
        files += R"(, {"projectID": 999999999, "fileID": 1, "required": true})";
    mf.write(QByteArray(R"({
  "minecraft": {"version": "1.20.1", "modLoaders": [{"id": "forge-)" + forgeVer.toUtf8() + R"(", "primary": true}]},
  "manifestType": "minecraftModpack",
  "manifestVersion": 1,
  "name": "MLCSmokeCF",
  "version": "1.0",
  "author": "mlc-test",
  "overrides": "overrides",
  "files": [)" + files + "]\n}"));
    mf.close();
    QFile f(workDir + "/overrides/keepme.txt");
    if (f.open(QIODevice::WriteOnly)) { f.write("test"); f.close(); }

    QString zipPath = workDir + "/pack.zip";
    QProcess zip;
    zip.setWorkingDirectory(workDir);
    zip.start("zip", {"-qr", zipPath, "."});
    zip.waitForFinished(15000);
    return zip.exitCode() == 0 ? zipPath : QString();
}

// 生成最小 HMCL 整合包（1.12.2，验证旧格式下载链）
static QString makeSyntheticHmclPack(const QString &workDir) {
    QDir().mkpath(workDir);
    QFile mf(workDir + "/modpack.json");
    if (!mf.open(QIODevice::WriteOnly)) return {};
    mf.write(R"({"name": "MLCSmoke112", "gameVersion": "1.12.2", "version": "1.0"})");
    mf.close();

    QString zipPath = workDir + "/pack.zip";
    QProcess zip;
    zip.setWorkingDirectory(workDir);
    zip.start("zip", {"-qr", zipPath, "modpack.json"});
    zip.waitForFinished(15000);
    return zip.exitCode() == 0 ? zipPath : QString();
}

// 生成 jar-only 的 mod 包（无 .minecraft/清单 → PackType::Mod）
static QString makeSyntheticModPack(const QString &workDir) {
    QDir().mkpath(workDir + "/mods");
    QFile f(workDir + "/mods/testmod.jar");
    if (f.open(QIODevice::WriteOnly)) { f.write("fake jar for smoke test"); f.close(); }

    QString zipPath = workDir + "/pack.zip";
    QProcess zip;
    zip.setWorkingDirectory(workDir);
    zip.start("zip", {"-qr", zipPath, "."});
    zip.waitForFinished(15000);
    return zip.exitCode() == 0 ? zipPath : QString();
}

// 探测 zip 命令是否可用（合成包冒烟的前置条件，不可用时相关用例标 SKIP 而非 FAIL）
static bool zipAvailable() {
    QProcess zip;
    zip.start("zip", {"-v"});
    if (!zip.waitForFinished(5000)) {
        zip.kill();
        zip.waitForFinished(1000);
        return false;
    }
    return zip.exitCode() == 0;
}

struct TestItem {
    QString cmd;
    QString status;  // OK / WARN / FAIL / SKIP
    QString detail;
};

static QList<TestItem> runCommandTests() {
    QList<TestItem> results;
    auto ok  = [&](const QString &cmd, const QString &detail)
        { results.append({cmd, "OK", detail}); };
    auto warn= [&](const QString &cmd, const QString &detail)
        { results.append({cmd, "WARN", detail}); };
    auto fail= [&](const QString &cmd, const QString &detail)
        { results.append({cmd, "FAIL", detail}); };
    auto skip= [&](const QString &cmd, const QString &detail)
        { results.append({cmd, "SKIP", detail}); };

    // config
    {
        auto cfg = mlc::getConfig();
        if (!cfg.version.isEmpty() && !cfg.commit.isEmpty())
            ok("config", QString("%1 (%2)").arg(cfg.version, cfg.commit));
        else
            fail("config", "版本信息缺失");
    }

    // set-folder（写入 → 读回 → 还原）
    {
        QString orig = Settings::instance().getString("LaunchFolderSelect");
        const QString testPath = "/tmp/_mlc_test_mc_/";
        Settings::instance().setString("LaunchFolderSelect", testPath);
        QString readBack = Settings::instance().getString("LaunchFolderSelect");
        Settings::instance().setString("LaunchFolderSelect", orig);
        if (readBack == testPath)
            ok("set-folder", "写入/读取/还原 正常");
        else
            fail("set-folder", QString("写入 '%1' 读回 '%2'").arg(testPath, readBack));
    }

    // set-lang（写入 → 读回 → 还原）
    {
        QString orig = Settings::instance().getString("UiLanguage");
        Settings::instance().setString("UiLanguage", "zh");
        QString readBack = Settings::instance().getString("UiLanguage");
        Settings::instance().setString("UiLanguage", orig);
        if (readBack == "zh")
            ok("set-lang", "写入/读取/还原 正常");
        else
            fail("set-lang", QString("写入 'zh' 读回 '%1'").arg(readBack));
    }

    // player-add → list → select → rm 全链路
    QString testUuid;
    {
        auto entry = mlc::addPlayer("_mlc_test_");
        if (!entry.uuid.isEmpty() && entry.name == "_mlc_test_") {
            ok("player-add", QString("创建 (uuid: %1)").arg(entry.uuid));
            testUuid = entry.uuid;
        } else {
            fail("player-add", "创建失败");
        }
    }

    if (!testUuid.isEmpty()) {
        auto players = mlc::listPlayers();
        bool found = false;
        for (const auto &p : players)
            if (p.uuid == testUuid) { found = true; break; }
        if (found)
            ok("player-list", QString("共 %1 个玩家，测试玩家已找到").arg(players.size()));
        else
            fail("player-list", "测试玩家未出现在列表中");

        QString before = Settings::instance().selectedPlayer();
        mlc::selectPlayer(testUuid);
        QString after = Settings::instance().selectedPlayer();
        mlc::selectPlayer(before);
        if (after == testUuid)
            ok("player-select", "选中/还原 正常");
        else
            fail("player-select", "选中失败");

        mlc::removePlayer(testUuid);
        players = mlc::listPlayers();
        bool gone = true;
        for (const auto &p : players)
            if (p.uuid == testUuid) { gone = false; break; }
        if (gone)
            ok("player-rm", "删除成功");
        else
            fail("player-rm", "删除后仍在列表中");
    } else {
        skip("player-list",   "player-add 失败，跳过");
        skip("player-select", "player-add 失败，跳过");
        skip("player-rm",     "player-add 失败，跳过");
    }

    // 外置登录态持久化往返（加密写入 → currentAuthlibLogin 读回 → logoutAuthlib 清除）
    {
        auto &s = Settings::instance();
        s.setString("Authlib/Server", "https://example.com/api/yggdrasil");
        s.setEncrypted("Authlib/AccessToken", "_mlc_test_token_");
        s.setEncrypted("Authlib/ClientToken", "_mlc_test_client_");
        s.setString("Authlib/Name", "_mlc_test_");
        s.setString("Authlib/Uuid", "00000000-0000-0000-0000-000000000000");
        auto info = mlc::currentAuthlibLogin();
        bool readOk = info.loggedIn && info.name == "_mlc_test_"
                      && info.server == "https://example.com/api/yggdrasil"
                      && s.getEncrypted("Authlib/AccessToken") == "_mlc_test_token_";
        mlc::logoutAuthlib();
        bool cleared = !mlc::currentAuthlibLogin().loggedIn;
        if (readOk && cleared)
            ok("login-persist", "加密持久化/读取/注销 往返正常");
        else
            fail("login-persist", QString("readOk=%1 cleared=%2").arg(readOk).arg(cleared));
    }

    // CF key 保密：用户 key 只以密文存在；清除后 ini 无 CfApiKey 行；内嵌 key 永不落盘
    {
        QString iniPath = QCoreApplication::applicationDirPath() + "/MLC.ini";
        mlc::setCfApiKey("_mlc_test_cfkey_");
        bool plainLeak = false;
        QFile f(iniPath);
        if (f.open(QIODevice::ReadOnly)) {
            plainLeak = QString::fromUtf8(f.readAll()).contains("_mlc_test_cfkey_");
            f.close();
        }
        mlc::setCfApiKey("");  // 清除
        bool leftover = false;
        QFile f2(iniPath);
        if (f2.open(QIODevice::ReadOnly)) {
            leftover = QString::fromUtf8(f2.readAll()).contains("CfApiKey");
            f2.close();
        }
        if (!plainLeak && !leftover)
            ok("cf-key-privacy", "用户 key 密文存储、清除无残留、内嵌 key 不落盘");
        else
            fail("cf-key-privacy", QString("plainLeak=%1 leftover=%2").arg(plainLeak).arg(leftover));
    }

    // list-javas
    {
        auto names = mlc::listJavas();
        if (!names.isEmpty())
            ok("list-javas", QString("检测到 %1 个 Java: %2").arg(names.size()).arg(names.join(", ")));
        else
            warn("list-javas", "未检测到 Java 运行时");
    }

    // list（实例 — INI 映射）
    {
        QString folder = Settings::instance().getString("LaunchFolderSelect");
        if (!folder.isEmpty()) {
            VersionManager::instance().setMcFolder(folder);
            auto ids = mlc::listVersions();
            if (ids.isEmpty())
                ok("list", "目录已设置，无导入实例");
            else
                ok("list", QString("检测到 %1 个实例: %2").arg(ids.size()).arg(ids.join(", ")));
        } else {
            warn("list", "游戏目录未设置，跳过（请先 set-folder）");
        }
    }

    // mc-list（原版 MC — versions/ 目录）
    {
        QString folder = Settings::instance().getString("LaunchFolderSelect");
        if (!folder.isEmpty()) {
            VersionManager::instance().setMcFolder(folder);
            auto ids = mlc::listMcVersions();
            if (ids.isEmpty())
                ok("mc-list", "无原版 MC 版本");
            else
                ok("mc-list", QString("检测到 %1 个版本: %2").arg(ids.size()).arg(ids.join(", ")));
        } else {
            warn("mc-list", "游戏目录未设置，跳过");
        }
    }

    // launch
    {
        QString folder = Settings::instance().getString("LaunchFolderSelect");
        if (!folder.isEmpty()) {
            VersionManager::instance().setMcFolder(folder);
            VersionManager::instance().loadLocalVersions();
            auto ids = VersionManager::instance().versionIds();
            if (!ids.isEmpty())
                skip("launch", QString("实例 '%1' 存在但跳过（需 GUI 环境）").arg(ids.first()));
            else
                skip("launch", "无可用实例，跳过");
        } else {
            skip("launch", "游戏目录未设置，跳过");
        }
    }

    // ---- 纯函数单测（无网络） ----

    // mavenNameToPath：maven 坐标 → 仓库路径
    {
        struct Case { const char *in; const char *expect; };
        const Case cases[] = {
            {"org.lwjgl:lwjgl:3.2.2", "org/lwjgl/lwjgl/3.2.2/lwjgl-3.2.2.jar"},
            {"org.lwjgl:lwjgl-glfw:3.3.1:natives-linux",
             "org/lwjgl/lwjgl-glfw/3.3.1/lwjgl-glfw-3.3.1-natives-linux.jar"},
            {"net.minecraftforge:forge:1.12.2-14.23.5.2860:universal@zip",
             "net/minecraftforge/forge/1.12.2-14.23.5.2860/forge-1.12.2-14.23.5.2860-universal.zip"},
        };
        int bad = 0;
        for (const auto &c : cases)
            if (FileUtils::mavenNameToPath(c.in) != c.expect) bad++;
        if (bad == 0) ok("mavenNameToPath", "3 例通过");
        else fail("mavenNameToPath", QString("%1/3 例失败").arg(bad));
    }

    // deduplicateArgs：--add-opens 可重复保留、-Xmx 后者覆盖
    {
        QStringList in = {"--add-opens", "java.base/java.util=ALL-UNNAMED",
                          "--add-opens", "java.base/java.lang=ALL-UNNAMED",
                          "-Xmx1G", "-Xmx2G"};
        auto out = ArgUtils::deduplicateArgs(in);
        int opens = out.count("--add-opens");
        int xmxCount = 0;
        for (const auto &a : out) if (a.startsWith("-Xmx")) xmxCount++;
        if (opens == 2 && xmxCount == 1 && out.contains("-Xmx2G"))
            ok("deduplicateArgs", "可重复 flag 保留 + -Xmx 去重 正常");
        else
            fail("deduplicateArgs", "输出异常: " + out.join(" "));
    }

    // deduplicateArgs 配对场景：-cp / --server / --port 带值参数同去同留，值不得成孤儿
    {
        QStringList in = {"-cp", "libs/a.jar", "-Xmx2G",
                          "-cp", "libs/b.jar",
                          "--server", "a.com", "--port", "25566",
                          "--server", "b.com", "--port", "25565"};
        auto out = ArgUtils::deduplicateArgs(in);
        QStringList expect = {"-Xmx2G", "-cp", "libs/b.jar",
                              "--server", "b.com", "--port", "25565"};
        if (out == expect)
            ok("deduplicateArgs-pair", "带值参数配对去重 正常");
        else
            fail("deduplicateArgs-pair", "输出异常: " + out.join(" "));
    }

    // ---- 合成包冒烟（inpack 端到端，需要网络与游戏目录） ----
    {
        // 统一在这里补尾斜杠：后续所有 folder + "xxx" 拼接都依赖它。
        // （原先只在 472/503 行零散补，导致 421 行在补之前就拼接，拼出 "…mcinstances/…" 的错路径）
        QString folder = Settings::instance().getString("LaunchFolderSelect");
        if (!folder.isEmpty() && !folder.endsWith('/')) folder += '/';
        if (folder.isEmpty()) {
            warn("inpack-smoke", "游戏目录未设置，跳过合成包冒烟");
        } else if (!zipAvailable()) {
            // 无 zip 命令无法合成测试包：相关用例标 SKIP 而非 FAIL（环境缺失，不算失败）
            // mc-install 依赖 inpack-112 留下的 1.12.2 缓存，一并跳过
            skip("detectPackType", "zip 命令不可用，跳过");
            skip("inpack-cf", "zip 命令不可用，跳过");
            skip("inpack-rollback", "zip 命令不可用，跳过");
            skip("inpack-loader-rollback", "zip 命令不可用，跳过");
            skip("inpack-112", "zip 命令不可用，跳过");
            skip("mc-install", "zip 命令不可用，跳过");
        } else {
            VersionManager::instance().setMcFolder(folder);
            QString base = QDir::temp().filePath("_mlc_smoke_"
                + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));

            QString cfOkZip  = makeSyntheticCfPack(base + "/cf_ok", false);
            QString cfBadZip = makeSyntheticCfPack(base + "/cf_bad", true);
            QString hmclZip  = makeSyntheticHmclPack(base + "/hmcl");

            // detectPackType：合成包类型识别
            if (!cfOkZip.isEmpty() && !hmclZip.isEmpty()) {
                bool typeOk = detectPackType(cfOkZip) == PackType::CurseForge
                           && detectPackType(hmclZip) == PackType::HMCL;
                if (typeOk) ok("detectPackType", "CF/HMCL 合成包识别正确");
                else fail("detectPackType", "类型识别错误");
            } else {
                fail("detectPackType", "合成测试包失败（zip 可用但打包失败）");
            }

            // CF 成功路：1 个真实 mod（JEI）→ 导入成功 + 实例可见
            const QString cfName = "__mlc_smoke_cf__";
            mlc::removeInstance(cfName);
            if (!cfOkZip.isEmpty()) {
                auto r = importAndWait(cfOkZip, cfName);
                if (r.success && mlc::listVersions().contains(cfName))
                    ok("inpack-cf", "CF 合成包导入成功（mod 下载链路正常）");
                else
                    fail("inpack-cf", "导入失败: " + r.message);

                // mod 包流程（jar-only zip）：趁着 cfName 实例还在
                if (r.success) {
                    QString modZip = makeSyntheticModPack(base + "/mod_only");
                    // 无 --to：报错且实例列表应包含 cfName
                    auto rNoTo = importAndWait(modZip, {});
                    if (!rNoTo.success && rNoTo.data.contains(cfName))
                        ok("inpack-mod-need-to", "mod 包缺 --to → 报错 + 实例列表");
                    else
                        fail("inpack-mod-need-to",
                             QString("success=%1 data=[%2]").arg(rNoTo.success).arg(rNoTo.data.join(",")));
                    // 有 --to：jar 应进入实例 mods/
                    auto rTo = importAndWait(modZip, {}, cfName);
                    QString instDirName = Settings::instance().dirForDisplayName(cfName);
                    bool jarOk = rTo.success
                        && QFile::exists(folder + "instances/" + instDirName + "/mods/testmod.jar");
                    if (jarOk)
                        ok("inpack-mod-to", "mod 包 --to → jar 已进入实例 mods/");
                    else
                        fail("inpack-mod-to", QString("success=%1 jar 未到位 (%2)").arg(rTo.success).arg(rTo.message));
                }
                mlc::removeInstance(cfName);
            } else {
                skip("inpack-cf", "合成包失败");
            }

            // CF 回滚路：假 mod → 必须导入失败且实例无残留
            const QString badName = "__mlc_smoke_bad__";
            mlc::removeInstance(badName);
            if (!cfBadZip.isEmpty()) {
                auto r = importAndWait(cfBadZip, badName);
                bool listed = mlc::listVersions().contains(badName);
                if (!r.success && !listed)
                    ok("inpack-rollback", "mod 失败 → 导入失败且无残留");
                else
                    fail("inpack-rollback",
                         QString("预期失败且无残留，实际 success=%1 listed=%2")
                             .arg(r.success).arg(listed));
                mlc::removeInstance(badName);
            } else {
                skip("inpack-rollback", "合成包失败");
            }

            // modloader 回滚路：假 Forge 版本 → 安装必须失败且无残留
            const QString loaderName = "__mlc_smoke_loader__";
            mlc::removeInstance(loaderName);
            QString cfLoaderZip = makeSyntheticCfPack(base + "/cf_loader", false, "99.99.99");
            if (!cfLoaderZip.isEmpty()) {
                auto r = importAndWait(cfLoaderZip, loaderName);
                bool listed = mlc::listVersions().contains(loaderName);
                if (!r.success && !listed)
                    ok("inpack-loader-rollback", "modloader 失败 → 导入失败且无残留");
                else
                    fail("inpack-loader-rollback",
                         QString("预期失败且无残留，实际 success=%1 listed=%2")
                             .arg(r.success).arg(listed));
                mlc::removeInstance(loaderName);
            } else {
                skip("inpack-loader-rollback", "合成包失败");
            }

            // HMCL 1.12.2：旧格式库下载 + 老格式 natives 解压
            const QString hmclName = "__mlc_smoke_112__";
            mlc::removeInstance(hmclName);
            if (!hmclZip.isEmpty()) {
                auto r = importAndWait(hmclZip, hmclName);
                bool verOk = QFile::exists(folder + "versions/1.12.2/1.12.2.json")
                          && QFile::exists(folder + "versions/1.12.2/1.12.2.jar");
                bool nativesOk = !QDir(folder + "versions/1.12.2/natives")
                                      .entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty();
                if (r.success && verOk && nativesOk)
                    ok("inpack-112", "1.12.2 旧格式下载 + natives 解压正常");
                else
                    fail("inpack-112", QString("success=%1 verOk=%2 nativesOk=%3 (%4)")
                                         .arg(r.success).arg(verOk).arg(nativesOk).arg(r.message));
                mlc::removeInstance(hmclName);
            } else {
                skip("inpack-112", "合成包失败");
            }

            // install：原版 MC 下载（1.12.2 经 inpack-112 已缓存，重复调用即校验补齐）
            {
                if (mlc::installVersion("1.12.2"))
                    ok("mc-install", "install 1.12.2 校验补齐正常");
                else
                    fail("mc-install", "install 1.12.2 失败");
            }

            QDir(base).removeRecursively();
        }
    }

    // rm（创建随机目录 + INI 映射 → removeInstance → 验证删除）
    {
        QString folder = Settings::instance().getString("LaunchFolderSelect");
        if (!folder.isEmpty()) {
            if (!folder.endsWith('/')) folder += '/';
            const QString testName = "_mlc_test_rm_";
            // 用随机目录名模拟新存储模型（instances/ 子目录）
            QString randomDir = "test_" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            QString testDir = folder + "instances/" + randomDir + "/";
            QDir().mkpath(testDir);
            QDir(testDir + "PCL/").mkpath(".");
            // 写入 INI 映射
            Settings::instance().setInstanceDir(randomDir, testName);
            if (QDir(testDir).exists()) {
                bool removed = mlc::removeInstance(testName);
                bool dirGone = !QDir(testDir).exists();
                bool mappingGone = Settings::instance().dirForDisplayName(testName).isEmpty();
                if (removed && dirGone && mappingGone)
                    ok("list-rm", "映射删除/目录清理 正常");
                else if (!mappingGone)
                    fail("list-rm", "映射残留");
                else
                    fail("list-rm", "删除失败或目录残留");
                // 兜底清理
                if (!dirGone) QDir(testDir).removeRecursively();
            } else {
                fail("list-rm", "无法创建测试目录");
            }
        } else {
            skip("list-rm", "游戏目录未设置，跳过");
        }
    }

    return results;
}

int handleTest() {
    std::cout << _("=== MLC 全系统自检 ===\n", "=== MLC System Self-Test ===\n") << std::endl;

    auto results = runCommandTests();
    bool hasFail = false;
    for (const auto &r : results) {
        QString tag;
        if (r.status == "OK")      tag = "\033[32m[ OK ]\033[0m";
        else if (r.status == "WARN")   tag = "\033[33m[WARN]\033[0m";
        else if (r.status == "FAIL")   tag = "\033[31m[FAIL]\033[0m";
        else if (r.status == "SKIP")   tag = "\033[90m[SKIP]\033[0m";
        else                      tag = "[INFO]";

        if (r.status == "FAIL") hasFail = true;
        std::cout << "  " << tag.toStdString() << " "
                  << QString("%1").arg(r.cmd, -22).toStdString()
                  << r.detail.toStdString() << std::endl;
    }
    // 存在 FAIL 项时以非零退出码返回，便于脚本/CI 判定
    return hasFail ? 1 : 0;
}
