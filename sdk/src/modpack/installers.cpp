// 类型安装器：解析各格式清单 → beginInstall 前置 → 复制内容 → downloadAndFinalize
#include "modpack_common.h"
#include "core/versionmanager.h"
#include "core/settings.h"
#include "util/file_utils.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

// ---- Type 0: CurseForge ----

void installCurseForge(const QString &filePath, const QString &packDir,
                       const QString &instanceName,
                       PackProgressCallback onProgress,
                       PackCompleteCallback onComplete) {
    Q_UNUSED(filePath)
    // 读取 manifest.json
    QString manifestPath = packDir + "/manifest.json";
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (onComplete) onComplete(false, "Cannot read manifest.json");
        return;
    }
    bool ok; json manifest = parseJsonSafe(f.readAll(), &ok); if (!ok) { if (onComplete) onComplete(false, "manifest.json parse failed"); return; }
    f.close();

    QString mcVersion = QString::fromStdString(manifest.value("minecraft", json::object()).value("version", ""));
    QString packName = QString::fromStdString(manifest.value("name", "Modpack"));

    // 读取 modloaders
    QString forgeVer, neoVer, fabricVer;
    if (manifest.contains("minecraft") && manifest["minecraft"].contains("modLoaders")) {
        for (const auto &loader : manifest["minecraft"]["modLoaders"]) {
            QString id = QString::fromStdString(loader.value("id", ""));
            if (id.startsWith("forge-")) forgeVer = id.mid(6);
            else if (id.startsWith("neoforge-")) neoVer = id.mid(9);
            else if (id.startsWith("fabric-")) fabricVer = id.mid(7);
        }
    }

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("CurseForge modpack detected: " + name, 15);

    // 复制 overrides（清单字段不可信：必须是 packDir 内的相对路径，防目录穿越）
    QString overridesDir = QString::fromStdString(manifest.value("overrides", "overrides"));
    QString srcOverride = resolvePackSubdir(packDir, overridesDir);
    if (srcOverride.isEmpty() && !overridesDir.isEmpty()) {
        cleanupOnError(finalDir);
        if (onComplete) onComplete(false, "清单中的 overrides 路径非法: " + overridesDir);
        return;
    }
    if (!srcOverride.isEmpty() && QDir(srcOverride).exists()) {
        if (onProgress) onProgress("Copying modpack files...", 20);
        if (!copyOrFail(srcOverride, finalDir, onComplete)) return;
    }

    // 提取 mod 列表
    QList<ModDownloadEntry> mods;
    if (manifest.contains("files")) {
        for (const auto &f : manifest["files"]) {
            ModDownloadEntry m;
            m.cfModId = QString::number(f.value("projectID", 0));
            m.cfFileId = QString::number(f.value("fileID", 0));
            m.savePath = "mods/" + m.cfModId + "_" + m.cfFileId + ".jar";
            mods.append(m);
        }
    }

    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, forgeVer, neoVer, fabricVer,
                        finalDir, name, mods, onProgress, onComplete);
}

// ---- Type 1: HMCL ----

void installHMCL(const QString &filePath, const QString &packDir,
                 const QString &instanceName,
                 PackProgressCallback onProgress,
                 PackCompleteCallback onComplete) {
    Q_UNUSED(filePath)
    QFile f(packDir + "/modpack.json");
    if (!f.open(QIODevice::ReadOnly)) {
        if (onComplete) onComplete(false, "Cannot read modpack.json");
        return;
    }
    bool ok; json modpack = parseJsonSafe(f.readAll(), &ok); if (!ok) { if (onComplete) onComplete(false, "modpack.json parse failed"); return; }
    f.close();

    QString packName = QString::fromStdString(modpack.value("name", "HMCL Modpack"));
    QString mcVersion = QString::fromStdString(modpack.value("gameVersion", ""));

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("HMCL modpack detected: " + name, 15);

    // 复制 minecraft/ 文件夹
    QString mcSrc = packDir + "/minecraft/";
    if (QDir(mcSrc).exists()) {
        if (onProgress) onProgress("Copying modpack files...", 20);
        if (!copyOrFail(mcSrc, finalDir, onComplete)) return;
    }

    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, {}, {}, {}, finalDir, name, {}, onProgress, onComplete);
}

// ---- Type 2: MultiMC ----

void installMultiMC(const QString &filePath, const QString &packDir,
                    const QString &instanceName,
                    PackProgressCallback onProgress,
                    PackCompleteCallback onComplete) {
    Q_UNUSED(filePath)
    QFile f(packDir + "/mmc-pack.json");
    if (!f.open(QIODevice::ReadOnly)) {
        if (onComplete) onComplete(false, "Cannot read mmc-pack.json");
        return;
    }
    bool ok; json mmc = parseJsonSafe(f.readAll(), &ok); if (!ok) { if (onComplete) onComplete(false, "mmc-pack.json parse failed"); return; }
    f.close();

    // 从 components 读取 MC 版本和 modloader
    QString mcVersion, forgeVer, neoVer, fabricVer;
    if (mmc.contains("components")) {
        for (const auto &comp : mmc["components"]) {
            QString uid = QString::fromStdString(comp.value("uid", ""));
            if (uid == "net.minecraft") mcVersion = QString::fromStdString(comp.value("version", ""));
            else if (uid == "net.minecraftforge") forgeVer = QString::fromStdString(comp.value("version", ""));
            else if (uid == "net.neoforged") neoVer = QString::fromStdString(comp.value("version", ""));
            else if (uid == "net.fabricmc.fabric-loader") fabricVer = QString::fromStdString(comp.value("version", ""));
        }
    }
    QString packName = QString::fromStdString(mmc.value("name", "MMC Modpack"));

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("MultiMC modpack detected: " + name, 15);

    // 复制 .minecraft/ 文件夹
    QString mcSrc = packDir + "/.minecraft/";
    if (QDir(mcSrc).exists()) {
        if (onProgress) onProgress("Copying modpack files...", 20);
        if (!copyOrFail(mcSrc, finalDir, onComplete)) return;
    }

    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, forgeVer, neoVer, fabricVer,
                        finalDir, name, {}, onProgress, onComplete);
}

// ---- Type 3: MCBBS ----

void installMCBBS(const QString &filePath, const QString &packDir,
                  const QString &instanceName,
                  PackProgressCallback onProgress,
                  PackCompleteCallback onComplete) {
    Q_UNUSED(filePath)
    // 尝试读取 mcbbs.packmeta，否则读 manifest.json
    json meta;
    QString mcVersion, forgeVer, neoVer, fabricVer;
    QString overridesDir = "overrides";

    if (QFile::exists(packDir + "/mcbbs.packmeta")) {
        QFile f(packDir + "/mcbbs.packmeta");
        if (f.open(QIODevice::ReadOnly)) {
            bool ok; meta = parseJsonSafe(f.readAll(), &ok); if (!ok) { if (onComplete) onComplete(false, "packmeta parse failed"); return; }
            f.close();
        }
    } else if (QFile::exists(packDir + "/manifest.json")) {
        QFile f(packDir + "/manifest.json");
        if (f.open(QIODevice::ReadOnly)) {
            bool ok; meta = parseJsonSafe(f.readAll(), &ok); if (!ok) { if (onComplete) onComplete(false, "packmeta parse failed"); return; }
            f.close();
        }
        overridesDir = QString::fromStdString(meta.value("overrides", "overrides"));
    }

    // 解析 addons
    if (meta.contains("addons")) {
        for (const auto &addon : meta["addons"]) {
            QString id = QString::fromStdString(addon.value("id", ""));
            QString ver = QString::fromStdString(addon.value("version", ""));
            if (id == "game") mcVersion = ver;
            else if (id == "forge") forgeVer = ver;
            else if (id == "neoforge") neoVer = ver;
            else if (id == "fabric") fabricVer = ver;
        }
    }
    QString packName = QString::fromStdString(meta.value("name", "MCBBS Modpack"));

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("MCBBS modpack detected: " + name, 15);

    // 复制 overrides（packmeta 字段不可信：必须是 packDir 内的相对路径）
    QString srcOverride = resolvePackSubdir(packDir, overridesDir);
    if (srcOverride.isEmpty() && !overridesDir.isEmpty()) {
        cleanupOnError(finalDir);
        if (onComplete) onComplete(false, "清单中的 overrides 路径非法: " + overridesDir);
        return;
    }
    if (!srcOverride.isEmpty() && QDir(srcOverride).exists()) {
        if (onProgress) onProgress("Copying modpack files...", 20);
        if (!copyOrFail(srcOverride, finalDir, onComplete)) return;
    }

    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, forgeVer, neoVer, fabricVer,
                        finalDir, name, {}, onProgress, onComplete);
}

// ---- Type 4: Modrinth ----

void installModrinth(const QString &filePath, const QString &packDir,
                     const QString &instanceName,
                     PackProgressCallback onProgress,
                     PackCompleteCallback onComplete) {
    Q_UNUSED(filePath)
    QString indexPath = packDir + "/modrinth.index.json";
    QFile f(indexPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (onComplete) onComplete(false, "Cannot read modrinth.index.json: " + f.errorString() + " (" + indexPath + ")");
        return;
    }
    bool ok; json index = parseJsonSafe(f.readAll(), &ok); if (!ok) { if (onComplete) onComplete(false, "modrinth.index.json parse failed"); return; }
    f.close();

    // 从 dependencies 读取版本
    QString mcVersion, forgeVer, neoVer, fabricVer;
    if (index.contains("dependencies")) {
        for (const auto &[key, val] : index["dependencies"].items()) {
            if (!val.is_string()) continue;
            QString ver = QString::fromStdString(val.get<std::string>());
            if (key == "minecraft") mcVersion = ver;
            else if (key == "forge") forgeVer = ver;
            else if (key == "neoforge" || key == "neo-forge") neoVer = ver;
            else if (key == "fabric-loader") fabricVer = ver;
        }
    }
    QString packName = QString::fromStdString(index.value("name", "Modrinth Modpack"));

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("Modrinth modpack detected: " + name, 15);

    // 复制 overrides 和 client-overrides
    for (const auto &sub : {"overrides", "client-overrides"}) {
        QString src = packDir + "/" + sub;
        if (QDir(src).exists()) {
            if (!copyOrFail(src, finalDir, onComplete)) return;
        }
    }

    // 提取 mod 列表（Modrinth 有直接下载 URL）
    QList<ModDownloadEntry> mods;
    if (index.contains("files")) {
        for (const auto &f : index["files"]) {
            ModDownloadEntry m;
            m.savePath = QString::fromStdString(f.value("path", "mods/unknown.jar"));
            // zip-slip 防护：path 必须是相对且不越界
            while (m.savePath.startsWith('/')) m.savePath.remove(0, 1);
            if (m.savePath.split('/').contains(".."))
                m.savePath = "mods/" + QFileInfo(m.savePath).fileName();
            if (f.contains("downloads") && f["downloads"].is_array() && !f["downloads"].empty()
                && f["downloads"][0].is_string()) {
                m.url = QString::fromStdString(f["downloads"][0].get<std::string>());
            }
            if (!m.url.isEmpty()) mods.append(m);
        }
    }

    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, forgeVer, neoVer, fabricVer,
                        finalDir, name, mods, onProgress, onComplete);
}

// ---- LauncherPack / Compressed 共用辅助 ----

// 从 LatestLaunch.bat 中提取版本名（title 行: "title 启动 - 1.21.1-NeoForge_21.1.226"）
static QString parseVersionFromBat(const QString &batPath) {
    QFile f(batPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    // 尝试匹配 title 行: "title 启动 - XXX"
    QRegularExpression re("title\\s+[^-]+-\\s*(\\S+)");
    auto match = re.match(content);
    if (match.hasMatch())
        return match.captured(1).trimmed();

    // 回退: 找 --version 参数
    QRegularExpression reVer("--version\\s+(\\S+)");
    auto matchVer = reVer.match(content);
    if (matchVer.hasMatch())
        return matchVer.captured(1).trimmed();

    return {};
}

// 在目录及其一级子目录中查找 .minecraft 或 PCL 标记
// 返回 {rootDir, isPclPack}
static QPair<QString, bool> findMcOrPclRoot(const QString &dir) {
    // 直接匹配 dir/.minecraft/
    if (QDir(dir + "/.minecraft").exists())
        return {dir, QFile::exists(dir + "/PCL/Setup.ini") &&
                     QFile::exists(dir + "/PCL/LatestLaunch.bat")};

    // 直接匹配 dir/PCL/ 标记（无 .minecraft 但有 PCL 配置）
    if (QFile::exists(dir + "/PCL/Setup.ini") &&
        QFile::exists(dir + "/PCL/LatestLaunch.bat"))
        return {dir, true};

    // 一级子目录
    QDir d(dir);
    for (const auto &entry : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString sub = entry.absoluteFilePath();
        if (QDir(sub + "/.minecraft").exists())
            return {sub, QFile::exists(sub + "/PCL/Setup.ini") &&
                         QFile::exists(sub + "/PCL/LatestLaunch.bat")};
        if (QFile::exists(sub + "/PCL/Setup.ini") &&
            QFile::exists(sub + "/PCL/LatestLaunch.bat"))
            return {sub, true};
    }
    return {{}, false};
}

// 从 .minecraft/versions/ 提取 MC 版本（读版本 json 判定，而非只看目录名——
// 目录名可能是 loader 命名，如 fabric-loader-0.16.10-1.21.1、neoforge-21.1.66）
static QString detectMcVersion(const QString &rootDir) {
    QString base = QDir(rootDir + "/.minecraft").exists() ? rootDir + "/.minecraft" : rootDir;
    QDir vd(base + "/versions");
    if (!vd.exists()) return {};
    for (const auto &entry : vd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString dirName = entry.fileName();
        QString jsonPath = entry.absoluteFilePath() + "/" + dirName + ".json";
        if (!QFile::exists(jsonPath)) continue;

        // NeoForge 目录命名：neoforge-{major}.{minor}[.patch] → MC 1.{major}.{minor}
        QRegularExpression neoRe(R"(^neoforge-(\d+)\.(\d+))");
        auto neoM = neoRe.match(dirName);
        if (neoM.hasMatch())
            return QString("1.%1.%2").arg(neoM.captured(1), neoM.captured(2));

        // fabric-loader-{loaderVer}-{mcVer}：取末尾的 MC 版本号
        QRegularExpression tailRe(R"((\d+\.\d+(?:\.\d+)?)$)");
        auto tailM = tailRe.match(dirName);
        if (dirName.startsWith("fabric-loader-") && tailM.hasMatch())
            return tailM.captured(1);

        // 读 json：inheritsFrom 优先，id 以数字版本开头次之
        QFile f(jsonPath);
        if (f.open(QIODevice::ReadOnly)) {
            json j = parseJsonSafe(f.readAll());
            f.close();
            QString inherit = QString::fromStdString(j.value("inheritsFrom", ""));
            if (!inherit.isEmpty()) return inherit;
            QString id = QString::fromStdString(j.value("id", ""));
            QRegularExpression idRe(R"(^\d+\.\d+(?:\.\d+)?)");
            auto idM = idRe.match(id);
            if (idM.hasMatch()) return idM.captured(0);
        }

        // 目录名本身像 MC 版本号（PCL 风格，如 1.21.1-NeoForge_21.1.226）
        QRegularExpression dirRe(R"(^\d+\.\d+(?:\.\d+)?)");
        auto dirM = dirRe.match(dirName);
        if (dirM.hasMatch()) return dirM.captured(0);
    }
    return {};
}

// 把目录里散落的 mod jar 复制到实例 mods/（返回是否有复制）
static bool copyLooseJars(const QString &dir, const QString &finalDir) {
    bool copied = false;
    QDir d(dir);
    for (const auto &entry : d.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (entry.fileName().endsWith(".jar", Qt::CaseInsensitive)) {
            QDir(finalDir + "mods/").mkpath(".");
            QFile::copy(entry.absoluteFilePath(), finalDir + "mods/" + entry.fileName());
            copied = true;
        }
    }
    return copied;
}

// ---- Type 9: Launcher pack —— 外层的 mod jar + 内层 zip 的 .minecraft 合并为一个标准实例 ----

void installLauncherPack(const QString &filePath, const QString &packDir,
                         const QString &instanceName,
                         PackProgressCallback onProgress,
                         PackCompleteCallback onComplete) {
    QString mcFolder = VersionManager::instance().mcFolder();
    QString tmpRoot = packTmpRoot();

    // 1. 找到内层 zip/mrpack
    QStringList candidates;
    QDir dir(packDir);
    for (const auto &entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.isFile()) {
            QString fn = entry.fileName().toLower();
            if (fn.endsWith(".zip") || fn.endsWith(".mrpack"))
                candidates.append(entry.absoluteFilePath());
        } else {
            QDir sub(entry.absoluteFilePath());
            for (const auto &subEntry : sub.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                QString fn = subEntry.fileName().toLower();
                if (fn.endsWith(".zip") || fn.endsWith(".mrpack"))
                    candidates.append(subEntry.absoluteFilePath());
            }
        }
    }
    if (candidates.isEmpty()) {
        if (onComplete) onComplete(false, "No modpack files found in archive");
        return;
    }
    // 优先 modpack.zip/mrpack，否则取第一个
    QString innerPack = candidates.first();
    for (const auto &c : candidates) {
        QString fn = QFileInfo(c).fileName().toLower();
        if (fn == "modpack.zip" || fn == "modpack.mrpack") { innerPack = c; break; }
    }

    // 2. 解压内层 zip 到临时目录
    QString mergeDir = tmpRoot + "launcher_merge/";
    FileUtils::removeTree(mergeDir);
    QDir().mkpath(mergeDir);
    if (onProgress) onProgress("Extracting inner modpack...", 10);
    if (!extractZip(innerPack, mergeDir, onProgress, 10)) {
        if (onComplete) onComplete(false, "Failed to extract inner modpack");
        return;
    }

    // 3. 找到 .minecraft 根目录
    auto [rootDir, isPcl] = findMcOrPclRoot(mergeDir);
    if (rootDir.isEmpty()) {
        if (onComplete) onComplete(false, "No game files found in inner modpack");
        return;
    }
    QString mcSource = rootDir;
    if (QDir(rootDir + "/.minecraft").exists())
        mcSource = rootDir + "/.minecraft/";

    // 4. 确定实例名和 MC 版本
    QString packName = QFileInfo(filePath).completeBaseName();

    QString mcVersion = detectMcVersion(rootDir);
    if (mcVersion.isEmpty())
        mcVersion = parseVersionFromBat(rootDir + "/PCL/LatestLaunch.bat");

    // 5. 前置 + 合并 .minecraft + 外层 mod
    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("Modpack: " + name, 15);

    if (onProgress) onProgress("Copying game files...", 18);
    if (!copyOrFail(mcSource, finalDir, onComplete)) return;

    // 包内 assets/libraries 合并进全局共享目录——资源不能丢
    // （尤其是 modloader 依赖库，downloadVersion 只提供原版库，删了启动必缺）
    for (const auto &shared : {"assets", "libraries"}) {
        QString src = finalDir + shared;
        if (QDir(src).exists()) {
            if (!copyOrFail(src, mcFolder + shared + "/", onComplete)) return;
            FileUtils::removeTree(src);
        }
    }

    // PCL 配置（在 root 级别，不在 .minecraft 内）
    if (QDir(rootDir + "/PCL").exists())
        copyDir(rootDir + "/PCL/", finalDir + "PCL/");

    // 复制外层及一级子目录的散落 mod jar
    bool hasLooseMods = copyLooseJars(packDir, finalDir);
    QDir outer(packDir);
    for (const auto &subdir : outer.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (copyLooseJars(subdir.absoluteFilePath(), finalDir))
            hasLooseMods = true;
    }
    if (hasLooseMods && onProgress)
        onProgress("Additional mods copied...", 22);

    // 6. 下载平台相关文件（MC 本体 + natives）
    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, {}, {}, {},
        finalDir, name, {}, onProgress, onComplete);
}

// ---- Fallback: Compressed .minecraft ----

// 处理已找到的 .minecraft / PCL 根目录
static void processCompressedRoot(const QString &rootDir, bool isPclPack,
                                   const QString &filePath, const QString &instanceName,
                                   PackProgressCallback onProgress,
                                   PackCompleteCallback onComplete) {
    // 确定实例名：优先用户指定 → zip 文件名 → 兜底 LatestLaunch.bat
    QString packName = QFileInfo(filePath).completeBaseName();
    if (packName.isEmpty() && isPclPack)
        packName = parseVersionFromBat(rootDir + "/PCL/LatestLaunch.bat");

    // 检测 MC 版本（用于下载正确的平台文件）
    QString mcVersion = detectMcVersion(rootDir);
    if (mcVersion.isEmpty() && isPclPack)
        mcVersion = parseVersionFromBat(rootDir + "/PCL/LatestLaunch.bat");

    // 定位 .minecraft 内容源
    QString mcSource = rootDir;
    bool hasMcFolder = QDir(rootDir + "/.minecraft").exists();
    if (hasMcFolder)
        mcSource = rootDir + "/.minecraft/";

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress(
        isPclPack ? "检测到 PCL 启动器Modpack: " + name
                  : "Compressed .minecraft detected: " + name, 15);

    if (onProgress) onProgress("Copying modpack files...", 20);
    if (!copyOrFail(mcSource, finalDir, onComplete)) return;

    // 包内 assets/libraries 合并进全局共享目录——资源不能丢
    QString mcFolder = VersionManager::instance().mcFolder();
    for (const auto &shared : {"assets", "libraries"}) {
        QString src = finalDir + shared;
        if (QDir(src).exists()) {
            if (!copyOrFail(src, mcFolder + shared + "/", onComplete)) return;
            FileUtils::removeTree(src);
        }
    }

    // 如果 PCL/ 在 .minecraft 同级，也复制到实例目录
    if (hasMcFolder && QDir(rootDir + "/PCL").exists())
        copyDir(rootDir + "/PCL/", finalDir + "PCL/");

    // 如果根目录有散落的 mod jar，复制到实例目录 mods/
    if (copyLooseJars(rootDir, finalDir) && onProgress)
        onProgress("Additional mods copied...", 22);

    // 下载 MC 版本文件（确保 libraries/natives/assets 匹配当前平台）
    if (onProgress) onProgress("Preparing download...", 30);
    downloadAndFinalize(mcVersion, {}, {}, {},
                        finalDir, name, {}, onProgress, onComplete);
}

void installCompressed(const QString &filePath, const QString &packDir,
                       const QString &instanceName,
                       PackProgressCallback onProgress,
                       PackCompleteCallback onComplete) {
    QString mcFolder = VersionManager::instance().mcFolder();

    // Step 1: 在 packDir 中直接查找 .minecraft 或 PCL 标记
    auto [rootDir, isPcl] = findMcOrPclRoot(packDir);
    if (!rootDir.isEmpty()) {
        processCompressedRoot(rootDir, isPcl, filePath, instanceName, onProgress, onComplete);
        return;
    }

    // Step 2: 递归解压内层 zip（最多二级），查找 .minecraft
    QString tmpRoot = packTmpRoot();
    QList<QPair<QString, int>> zipQueue;
    int innerSeq = 0;

    // 收集 packDir 中的 zip
    QDir pd(packDir);
    for (const auto &entry : pd.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        QString n = entry.fileName().toLower();
        if (n.endsWith(".zip") || n.endsWith(".mrpack"))
            zipQueue.append({entry.absoluteFilePath(), 0});
    }
    for (const auto &subdir : pd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sd(subdir.absoluteFilePath());
        for (const auto &entry : sd.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
            QString n = entry.fileName().toLower();
            if (n.endsWith(".zip") || n.endsWith(".mrpack"))
                zipQueue.append({entry.absoluteFilePath(), 0});
        }
    }

    while (!zipQueue.isEmpty()) {
        auto [zipPath, depth] = zipQueue.takeFirst();
        if (depth >= 2) continue;

        QString extractDir = tmpRoot + "inner_" + QString::number(innerSeq++) + "/";
        QDir().mkpath(extractDir);

        if (!extractZip(zipPath, extractDir, onProgress, 15 + depth * 5))
            continue;

        auto [innerRoot, innerPcl] = findMcOrPclRoot(extractDir);
        if (!innerRoot.isEmpty()) {
            if (onProgress) onProgress("Game files found in inner archive...", 15 + depth * 5);

            // 也复制外层散落的 mod jar
            QDir outerDir = QFileInfo(zipPath).absoluteDir();
            for (const auto &entry : outerDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                if (entry.fileName().endsWith(".jar", Qt::CaseInsensitive)) {
                    // 把 jar 临时复制到 rootDir 让 processCompressedRoot 处理
                    QFile::copy(entry.absoluteFilePath(), innerRoot + "/" + entry.fileName());
                }
            }

            processCompressedRoot(innerRoot, innerPcl, filePath, instanceName, onProgress, onComplete);
            return;
        }

        // 继续深入一级
        if (depth < 1) {
            QDir ed(extractDir);
            for (const auto &entry : ed.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                QString n = entry.fileName().toLower();
                if (n.endsWith(".zip") || n.endsWith(".mrpack"))
                    zipQueue.append({entry.absoluteFilePath(), depth + 1});
            }
        }
    }

    // Step 3: 回退 — 通过条目列表定位 versions 模式，直接解压（同步路径）
    QStringList names = FileUtils::listZipEntries(filePath);

    QString mcRoot = findMcRoot(names);
    QString packName = QFileInfo(filePath).completeBaseName();

    QString finalDir, name;
    if (!beginInstall(instanceName, packName, onComplete, finalDir, name)) return;
    if (onProgress) onProgress("Compressed .minecraft detected: " + name, 15);

    // Compressed 已含全部文件，直接在 tmp 解压后移入
    QString instanceDir = QDir(finalDir).dirName();
    QString workDir = packTmpRoot() + "extract_" + instanceDir + "/";
    FileUtils::removeTree(workDir);
    QDir().mkpath(workDir);

    if (onProgress) onProgress("Extracting...", 20);
    if (!extractZip(filePath, workDir, onProgress, 25)) {
        FileUtils::removeTree(workDir);
        cleanupOnError(finalDir);
        if (onComplete) onComplete(false, "Extraction failed");
        return;
    }

    // 如有 mcRoot 包装层，把内容提出来
    if (!mcRoot.isEmpty()) {
        QString innerDir = workDir + mcRoot;
        if (QDir(innerDir).exists() && innerDir != workDir) {
            // 复制失败不能删原件继续——按失败回滚
            if (!copyDir(innerDir, workDir)) {
                FileUtils::removeTree(workDir);
                cleanupOnError(finalDir);
                if (onComplete) onComplete(false, "Copy inner content failed");
                return;
            }
            FileUtils::removeTree(innerDir);
        }
    }

    QDir().mkpath(QFileInfo(finalDir).absolutePath());
    if (!copyDir(workDir, finalDir)) {
        FileUtils::removeTree(workDir);
        cleanupOnError(finalDir);
        if (onComplete) onComplete(false, "Copy to instance directory failed");
        return;
    }
    FileUtils::removeTree(workDir);

    // 包内 assets/libraries 合并进全局共享目录——资源不能丢
    for (const auto &shared : {"assets", "libraries"}) {
        QString src = finalDir + shared;
        if (QDir(src).exists()) {
            if (!copyOrFail(src, mcFolder + shared + "/", onComplete)) return;
            FileUtils::removeTree(src);
        }
    }

    QDir(finalDir + "PCL/").mkpath(".");
    QSettings ini(finalDir + "PCL/Setup.ini", QSettings::IniFormat);
    ini.beginGroup("Setup");
    ini.setValue("Name", name);
    ini.setValue("VersionArgumentIndie", 1);
    ini.setValue("VersionArgumentIndieV2", true);
    // 与下载管线的 finalizeNow 保持一致：launch 依赖 Version 键解析启动版本，
    // 缺了会导致这个路径装出来的实例启动时版本解析不到
    {
        const QString vn = resolveInstanceVersionName(finalDir, QString(), QString(), QString());
        if (!vn.isEmpty()) ini.setValue("Version", vn);
    }
    ini.endGroup();
    ini.sync();
    writeInstanceMapping(instanceDir, name);
    cleanupPackTmp();
    markComplete(finalDir);
    if (onComplete) onComplete(true, name);
}

// ---- Type 5: Mod 包 —— jar 复制到目标实例的 mods/ ----

void installMod(const QString &packDir, const QString &targetInstance,
                PackProgressCallback onProgress,
                PackCompleteCallback onComplete) {
    if (targetInstance.isEmpty()) {
        if (onComplete) onComplete(false, "此 zip 为 mod 包，需要加 --to <实例名>");
        return;
    }

    // 解析目标实例（显示名 → 随机目录名）
    QString dirName = Settings::instance().dirForDisplayName(targetInstance);
    QString instDir = VersionManager::instance().mcFolder() + "instances/" + dirName + "/";
    if (dirName.isEmpty() || !QDir(instDir).exists()) {
        if (onComplete) onComplete(false, "目标实例不存在: " + targetInstance);
        return;
    }

    // 递归收集包内所有 jar → 实例 mods/（拍平到文件名）
    if (onProgress) onProgress("Copying mods...", 50);
    QString modsDir = instDir + "mods/";
    if (!QDir().mkpath(modsDir)) {
        if (onComplete) onComplete(false, "无法创建 mods 目录: " + modsDir);
        return;
    }

    // 先收集全部 jar（算清总数，用于判定"全部成功"）
    QStringList jarPaths;
    QDirIterator it(packDir, {"*.jar"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        jarPaths.append(it.filePath());
    }

    // 第一阶段：全部复制成临时文件（不碰任何已有 mod，避免"先删后拷"在失败时丢数据）
    QStringList staged;   // 临时文件路径
    QStringList targets;  // 最终目标路径
    QString copyErr;
    for (const QString &src : jarPaths) {
        const QString fileName = QFileInfo(src).fileName();
        if (fileName.isEmpty()) {
            copyErr = "包内 jar 文件名为空: " + src;
            break;
        }
        QString dst = modsDir + fileName;
        QString tmp = dst + ".mlc-new";
        QFile::remove(tmp);  // 清上轮残留
        if (!QFile::copy(src, tmp)) {
            copyErr = QString("复制失败: %1（磁盘已满或无权限？）").arg(fileName);
            break;
        }
        staged.append(tmp);
        targets.append(dst);
    }

    // 任一 jar 复制失败 = 整体失败：删掉临时文件，实例 mods/ 保持原样
    if (!copyErr.isEmpty()) {
        for (const QString &tmp : staged) QFile::remove(tmp);
        if (onComplete) onComplete(false, copyErr);
        return;
    }

    // 第二阶段：提交（旧文件先改名备份，失败可完整回滚）
    QStringList committed;  // 已换入的目标路径
    QStringList backups;    // 对应的备份路径（空串 = 原本无同名文件）
    QString commitErr;
    for (int i = 0; i < staged.size() && commitErr.isEmpty(); ++i) {
        const QString dst = targets[i];
        const QString tmp = staged[i];
        QString bak;
        if (QFile::exists(dst)) {
            bak = dst + ".mlc-old";
            QFile::remove(bak);
            if (!QFile::rename(dst, bak)) {
                commitErr = QString("替换失败: %1（文件被占用？）").arg(QFileInfo(dst).fileName());
                break;
            }
        }
        if (!QFile::rename(tmp, dst)) {
            if (!bak.isEmpty()) QFile::rename(bak, dst);  // 还原这一项
            commitErr = QString("写入失败: %1").arg(QFileInfo(dst).fileName());
            break;
        }
        committed.append(dst);
        backups.append(bak);
    }

    if (!commitErr.isEmpty()) {
        // 回滚已提交项：删新文件、恢复备份
        for (int i = 0; i < committed.size(); ++i) {
            QFile::remove(committed[i]);
            if (!backups[i].isEmpty()) QFile::rename(backups[i], committed[i]);
        }
        // 清理未提交的临时文件
        for (int j = committed.size(); j < staged.size(); ++j) QFile::remove(staged[j]);
        if (onComplete) onComplete(false, commitErr);
        return;
    }

    // 全部提交成功：删除备份
    for (const QString &bak : backups) {
        if (!bak.isEmpty()) QFile::remove(bak);
    }

    if (onProgress) onProgress("Complete", 100);
    // mod 包不走下载管线（无 finalizeNow），成功时自行清理本进程 tmp/<pid>/
    cleanupPackTmp();
    if (onComplete) onComplete(true, QString("%1 mod(s) added to %2").arg(committed.size()).arg(targetInstance));
}
