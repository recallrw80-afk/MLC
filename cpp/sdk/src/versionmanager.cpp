#include "core/versionmanager.h"
#include "core/settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QSet>

static Q_LOGGING_CATEGORY(logVer, "mlc.version")

VersionManager& VersionManager::instance() {
    static VersionManager m;
    return m;
}

// Minecraft folder

QString VersionManager::mcFolder() {
    QMutexLocker lock(&m_mutex);
    if (m_mcFolder.isEmpty()) {
        // 惰性解析：Settings 的 LaunchFolderSelect → 默认可执行文件旁 ./mc/
        // （组 A 命令（如 list-javas）不走 setMcFolder，也依赖此默认值）
        QString saved = Settings::instance().getString("LaunchFolderSelect");
        if (saved.isEmpty())
            saved = QCoreApplication::applicationDirPath() + "/mc/";
        m_mcFolder = QDir(saved).absolutePath() + "/";
    }
    return m_mcFolder;
}

void VersionManager::setMcFolder(const QString &path, bool persist) {
    QString normalized = QDir(path).absolutePath() + "/";
    {
        QMutexLocker lock(&m_mutex);
        if (m_mcFolder == normalized) return;
        m_mcFolder = normalized;
    }
    // persist=false 用于一次性覆盖（如 CLI --folder），不写回配置
    if (persist)
        Settings::instance().setString("LaunchFolderSelect", path);
    // 用局部副本记日志/发信号：锁外重读成员可能与并发写入错开
    qCInfo(logVer) << "Minecraft folder set to:" << normalized;
    emit mcFolderChanged(normalized);

    // Reload versions
    loadLocalVersions();
}

QList<McFolder> VersionManager::loadFolderList() {
    QList<McFolder> folders;

    // 1. Check exe folder (current launcher directory)
    QString exeFolder = QCoreApplication::applicationDirPath() + "/";
    if (QDir(exeFolder + "versions").exists()) {
        folders.append({.name = "Current Folder", .location = exeFolder, .type = McFolder::Type::Vanilla});
    }

    // Scan subdirectories of exe folder for MC installations（含 .minecraft 隐藏目录）
    QDir exeDir(exeFolder);
    for (const auto &entry : exeDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        QString subPath = entry.absoluteFilePath() + "/";
        if (QDir(subPath + "versions").exists() || entry.fileName() == ".minecraft") {
            bool alreadyExists = false;
            for (const auto &f : folders) {
                if (f.location == subPath) { alreadyExists = true; break; }
            }
            if (!alreadyExists) {
                folders.append({.name = "Current Folder", .location = subPath, .type = McFolder::Type::Vanilla});
            }
        }
    }

    // 2. Official launcher folder
    // On Windows: %APPDATA%/.minecraft
    // On Linux: ~/.minecraft
    // On Mac: ~/Library/Application Support/minecraft
    QString officialPath;
    switch (currentPlatform()) {
    case Platform::Windows:
        officialPath = qEnvironmentVariable("APPDATA") + "/.minecraft/";
        break;
    case Platform::Linux:
        officialPath = QDir::homePath() + "/.minecraft/";
        break;
    case Platform::MacOS:
        officialPath = QDir::homePath() + "/Library/Application Support/minecraft/";
        break;
    default:
        officialPath = QDir::homePath() + "/.minecraft/";
        break;
    }

    if (QDir(officialPath + "versions").exists()) {
        bool alreadyExists = false;
        for (const auto &f : folders) {
            if (f.location == officialPath) { alreadyExists = true; break; }
        }
        if (!alreadyExists) {
            folders.append({.name = "Official Launcher", .location = officialPath, .type = McFolder::Type::Vanilla});
        }
    }

    // 3. Custom folders from settings
    QString customStr = Settings::instance().getString("LaunchFolders");
    if (!customStr.isEmpty()) {
        const auto entries = customStr.split('|', Qt::SkipEmptyParts);
        for (const auto &entry : entries) {
            if (!entry.contains('>')) continue;
            QString name = entry.section('>', 0, 0);
            QString loc = entry.section('>', 1);
            if (!loc.endsWith('/')) loc += '/';

            bool exists = false;
            for (auto &f : folders) {
                if (f.location == loc) {
                    f.name = name;
                    f.type = McFolder::Type::RenamedVanilla;
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                folders.append({.name = name, .location = loc, .type = McFolder::Type::Custom});
            }
        }
    }

    return folders;
}

// Version listing

void VersionManager::loadLocalVersions() {
    // 必须走 mcFolder()：它负责把 Settings/默认值惰性解析进 m_mcFolder。
    // 裸读 m_mcFolder 会让"只 Settings::initialize() 后直接调 listVersions()"的
    // 纯 SDK 调用方拿到空列表（CLI 因 main() 先 setMcFolder 才没暴露）。
    const QString folder = mcFolder();
    if (folder.isEmpty()) return;

    m_isLoading = true;
    emit loadingChanged();
    emit versionLoadProgress("Loading local versions...");

    m_versionList.clear();

    QDir mcDir(folder);
    if (!mcDir.exists()) {
        m_isLoading = false;
        emit loadingChanged();
        emit versionListChanged();  // 列表已清空，UI 需要同步刷新
        return;
    }

    // 从 INI [Instances] 映射中读取所有实例
    QMap<QString, QString> instanceMap = Settings::instance().instanceDirs();

    // 当前目录是否是持久化的默认目录（--folder 临时覆盖时只过滤不清理，防止误删默认目录的映射）
    QString persisted = Settings::instance().getString("LaunchFolderSelect");
    bool isDefaultFolder = (QDir(persisted).absolutePath() + "/" == folder);

    for (auto it = instanceMap.begin(); it != instanceMap.end(); ++it) {
        const QString &dirName = it.key();
        const QString &displayName = it.value();
        QString dirPath = folder + "instances/" + dirName + "/";

        if (!QFileInfo::exists(dirPath + "PCL/Setup.ini")) {
            // INI 映射存在但实例目录已丢失（如手动删除目录）：
            // 默认目录下清理失效映射（自修复）；--folder 看别的目录时只跳过不清理
            if (isDefaultFolder && !QDir(dirPath).exists())
                Settings::instance().removeInstanceDir(dirName);
            continue;
        }

        McVersionInfo info;
        info.id = displayName;
        info.isLocal = true;
        info.type = "instance";
        m_versionList.append(info);
    }

    emit versionListChanged();
    m_isLoading = false;
    emit loadingChanged();
}

// 只加载原版 MC 版本（versions/ 子目录）
void VersionManager::loadMcVersions() {
    // 同 loadLocalVersions：走 mcFolder() 触发惰性解析
    const QString folder = mcFolder();
    if (folder.isEmpty()) return;

    m_isLoading = true;
    emit loadingChanged();

    m_versionList.clear();

    QDir versionsDir(folder + "versions/");
    if (versionsDir.exists()) {
        for (const auto &entry : versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString dirName = entry.fileName();
            QString jsonPath = entry.absoluteFilePath() + "/" + dirName + ".json";
            if (!QFileInfo::exists(jsonPath)) continue;

            McVersionInfo info;
            info.id = dirName;
            info.isLocal = true;

            try {
                QFile file(jsonPath);
                if (file.open(QIODevice::ReadOnly)) {
                    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                    QJsonObject root = doc.object();
                    info.type = root.value("type").toString("unknown");
                    QString timeStr = root.value("releaseTime").toString();
                    if (!timeStr.isEmpty())
                        info.releaseTime = QDateTime::fromString(timeStr, Qt::ISODate);
                }
            } catch (...) {
                info.type = "unknown";
            }

            m_versionList.append(info);
        }
    }

    std::sort(m_versionList.begin(), m_versionList.end(),
              [](const McVersionInfo &a, const McVersionInfo &b) {
        return a.releaseTime > b.releaseTime;
    });

    emit versionListChanged();
    m_isLoading = false;
    emit loadingChanged();
}

void VersionManager::fetchVersionManifest() {
    m_isLoading = true;
    emit loadingChanged();
    emit versionLoadProgress("Fetching remote version list...");

    QNetworkRequest request(QUrl("https://launchermeta.mojang.com/mc/game/version_manifest.json"));
    request.setRawHeader("User-Agent", "MLC/0.1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(logVer) << "Failed to fetch version manifest:" << reply->errorString();
            m_isLoading = false;
            emit loadingChanged();
            emit versionLoadError("", "Network error: " + reply->errorString());
            return;
        }

        QByteArray data = reply->readAll();
        json manifest = json::parse(data.toStdString(), nullptr, false);
        // versions 必须是数组：语法合法但结构畸形（对象/字符串等）同样按无效处理
        if (manifest.is_discarded() || !manifest.contains("versions") ||
            !manifest["versions"].is_array()) {
            qCWarning(logVer) << "Invalid version manifest JSON";
            m_isLoading = false;
            emit loadingChanged();
            return;
        }

        // Cache manifest to disk
        QString cacheDir = mcFolder() + "versions/";
        QDir().mkpath(cacheDir);
        QFile cacheFile(cacheDir + "version_manifest.json");
        if (cacheFile.open(QIODevice::WriteOnly)) {
            cacheFile.write(data);
            cacheFile.close();
        }

        // Parse remote versions
        QList<McVersionInfo> remoteVersions;
        for (const auto &v : manifest["versions"]) {
            // 单条目字段类型畸形时 .value() 抛 type_error：跳过该条，不影响其余条目
            try {
                McVersionInfo info;
                info.id = QString::fromStdString(v.value("id", ""));
                info.type = QString::fromStdString(v.value("type", ""));
                info.url = QString::fromStdString(v.value("url", ""));
                QString timeStr = QString::fromStdString(v.value("releaseTime", ""));
                if (!timeStr.isEmpty())
                    info.releaseTime = QDateTime::fromString(timeStr, Qt::ISODate);
                info.isLocal = false;
                remoteVersions.append(info);
            } catch (const std::exception &e) {
                qCWarning(logVer) << "Skipping malformed manifest entry:" << e.what();
            }
        }

        // Merge: add remote versions not already in local list
        QSet<QString> localIds;
        for (const auto &v : m_versionList) localIds.insert(v.id);
        for (const auto &rv : remoteVersions) {
            if (!localIds.contains(rv.id))
                m_versionList.append(rv);
        }

        // Sort: releases first, then by releaseTime desc
        std::sort(m_versionList.begin(), m_versionList.end(),
                  [](const McVersionInfo &a, const McVersionInfo &b) {
            if (a.type != b.type) {
                if (a.type == "release") return true;
                if (b.type == "release") return false;
            }
            return a.releaseTime > b.releaseTime;
        });

        emit versionListChanged();
        m_isLoading = false;
        emit loadingChanged();
        qCInfo(logVer) << "Fetched" << remoteVersions.size()
                        << "remote versions, total:" << m_versionList.size();
    });
}

QVariantList VersionManager::remoteVersionList() const {
    QVariantList out;
    for (const auto &v : m_versionList) {
        if (v.isLocal) continue;
        QVariantMap m;
        m.insert("id", v.id);
        m.insert("type", v.type);
        m.insert("releaseTime", v.releaseTime.toString(Qt::ISODate));
        out.append(m);
    }
    return out;
}

QStringList VersionManager::versionIds() const
{
    QStringList ids;
    for (const auto &v : m_versionList) {
        ids.append(v.id);
    }
    return ids;
}

// Version parsing

// 实例版本解析（整合包实例，instances/{随机名}/）
McVersion VersionManager::loadInstanceVersion(const QString &dirName) {
    McVersion ver;
    QString instDir = m_mcFolder + "instances/" + dirName + "/";

    // 1. 版本名：Setup.ini 的 Version 键 → 实例内唯一带 json 的版本文件夹
    QString verName;
    QSettings ini(instDir + "PCL/Setup.ini", QSettings::IniFormat);
    ini.beginGroup("Setup");
    verName = ini.value("Version").toString();
    ini.endGroup();
    if (verName.isEmpty()) {
        QDir vd(instDir + "versions/");
        for (const auto &entry : vd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (QFile::exists(entry.absoluteFilePath() + "/" + entry.fileName() + ".json")) {
                verName = entry.fileName();
                break;
            }
        }
    }
    if (verName.isEmpty()) {
        ver.isValid = false;
        ver.info = "实例未记录游戏版本: " + dirName;
        return ver;
    }

    // 2. version json：实例内优先，全局 versions/ 兜底
    QString jsonPath = instDir + "versions/" + verName + "/" + verName + ".json";
    if (!QFile::exists(jsonPath))
        jsonPath = m_mcFolder + "versions/" + verName + "/" + verName + ".json";
    if (!QFile::exists(jsonPath)) {
        ver.isValid = false;
        ver.info = "version json 缺失: " + verName;
        return ver;
    }

    ver = parseVersionJson(jsonPath);

    // 3. 游戏目录（PathIndie，PCL 版本隔离语义）：
    // 实例 versions/{ver}/ 下有 mods/ 或 config/ → 游戏目录用它，否则用实例根
    QString gameDir = instDir;
    if (QDir(instDir + "versions/" + verName + "/mods").exists() ||
        QDir(instDir + "versions/" + verName + "/config").exists())
        gameDir = instDir + "versions/" + verName + "/";
    ver.pathIndie = gameDir;
    return ver;
}

McVersion VersionManager::loadVersion(const QString &versionId) {
    // QDir::filePath keeps the path clean (no "//" from the trailing slash in m_mcFolder).
    QString jsonPath = QDir(m_mcFolder).filePath("versions/" + versionId + "/" + versionId + ".json");
    return parseVersionJson(jsonPath);
}

McVersion VersionManager::parseVersionJson(const QString &jsonPath) {
    McVersion ver;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        ver.isValid = false;
        ver.info = "Cannot open version JSON: " + jsonPath;
        return ver;
    }

    // 直接用 nlohmann::json 解析，避免 QJsonDocument 中转
    json j = json::parse(file.readAll().toStdString(), nullptr, false);
    if (j.is_discarded()) {
        ver.isValid = false;
        ver.info = "Failed to parse JSON: " + jsonPath;
        return ver;
    }

    ver = parseVersionJson(j, QFileInfo(jsonPath).dir().dirName());
    ver.pathJson = jsonPath;
    ver.pathVersion = QFileInfo(jsonPath).absolutePath() + "/";
    ver.pathJar = ver.pathVersion + ver.id + ".jar";
    if (!QFile::exists(ver.pathJar)) {
        // modded 版本目录只有 json——client jar 在 vanilla 目录
        QString vanillaStr = ver.vanillaVersion.toString();
        QString vanillaJar = m_mcFolder + "versions/" + vanillaStr + "/" + vanillaStr + ".jar";
        if (!vanillaStr.isEmpty() && QFile::exists(vanillaJar))
            ver.pathJar = vanillaJar;
    }
    // pathIndie may differ for version isolation — same as pathVersion for now
    ver.pathIndie = m_mcFolder;

    return ver;
}

McVersion VersionManager::parseVersionJson(const json &j, const QString &versionId) {
    McVersion ver;
    ver.id = versionId;
    ver.isValid = true;

    // 根节点不是对象（语法合法但结构畸形）→ 版本无效
    if (!j.is_object()) {
        ver.isValid = false;
        ver.info = "version json 根节点不是对象: " + versionId;
        return ver;
    }

    // 字段类型畸形时 .value()/.get() 抛 type_error，兜底为版本无效
    // （同时覆盖 detectModLoaders/detectVanillaVersion 内的 .get<std::string>()）
    try {
        // Basic fields
        ver.type = QString::fromStdString(j.value("type", "unknown"));
        QString timeStr = QString::fromStdString(j.value("releaseTime", ""));
        if (!timeStr.isEmpty()) {
            ver.releaseTime = QDateTime::fromString(timeStr, Qt::ISODate);
        }

        // Inheritance
        ver.inheritName = QString::fromStdString(j.value("inheritsFrom", ""));

        // Detect mod loaders
        ver.modLoader = detectModLoaders(j);

        // Detect vanilla version
        QString vanillaStr = detectVanillaVersion(j, versionId);
        ver.vanillaVersion = QVersionNumber::fromString(vanillaStr);
    } catch (const std::exception &e) {
        qCWarning(logVer) << "Malformed version JSON:" << versionId << e.what();
        ver.isValid = false;
        ver.info = "version json 字段类型畸形: " + versionId;
    }

    return ver;
}

// Mod loader detection

McModLoaderInfo VersionManager::detectModLoaders(const json &versionJson) {
    McModLoaderInfo info;

    // Forge: has "inheritsFrom" and has "forge" in the version ID
    // Or has "arguments.jvm" referencing forge client
    if (versionJson.contains("inheritsFrom")) {
        std::string inherit = versionJson["inheritsFrom"].get<std::string>();
        std::string versionId;

        if (versionJson.contains("id")) {
            versionId = versionJson["id"].get<std::string>();
        }

        QString qVerId = QString::fromStdString(versionId).toLower();
        QString qInherit = QString::fromStdString(inherit).toLower();

        // NeoForge detection（必须先于 Forge："neoforge" 包含 "forge" 子串）
        if (qVerId.contains("neoforge")) {
            info.hasNeoForge = true;
        }

        // Forge detection（排除 neoforge，否则版本号会被误提取）
        if (!info.hasNeoForge && qVerId.contains("forge")) {
            info.hasForge = true;
            // 从版本目录名提取 Forge 版本。真实存在三种格式（大小写不敏感）：
            //   1.20.1-forge-47.2.0                  → 47.2.0
            //   1.12.2-forge1.12.2-14.23.5.2860      → 14.23.5.2860（旧版，无分隔符且带 MC 版本前缀）
            //   1.7.10-Forge10.13.4.1614             → 10.13.4.1614（大写 F）
            // 旧实现 "forge[-_]([\d.]+)" 只覆盖第一种：旧版格式匹配不到（forgeVersion 为空，
            // 兼容矩阵整段跳过），而 "forge-1.12.2-14.23.5.2860" 会错取 MC 版本 1.12.2。
            static const QRegularExpression forgeRe(
                QStringLiteral("forge[-_]?(?:\\d+(?:\\.\\d+)*-)?(\\d+\\.\\d+(?:\\.\\d+)*(?:\\.\\d+)?)"),
                QRegularExpression::CaseInsensitiveOption);
            auto match = forgeRe.match(QString::fromStdString(versionId));
            if (match.hasMatch()) {
                info.forgeVersion = match.captured(1);
            }
        }

        // Fabric detection
        if (qVerId.contains("fabric")) {
            info.hasFabric = true;
        }

        // OptiFine detection
        if (qVerId.contains("optifine")) {
            info.hasOptiFine = true;
        }

        // LiteLoader detection
        if (qVerId.contains("liteloader")) {
            info.hasLiteLoader = true;
        }
    }

    // Check for mod loader metadata in the JSON
    // Fabric puts its version in a custom field
    if (versionJson.contains("fabricLoader")) {
        info.hasFabric = true;
        info.fabricVersion = QString::fromStdString(
            versionJson["fabricLoader"].value("version", ""));
    }

    return info;
}

QString VersionManager::detectVanillaVersion(const json &versionJson, const QString &versionId) {
    // For modded versions, the vanilla version is in the inheritsFrom or the ID
    if (versionJson.contains("inheritsFrom")) {
        QString inherit = QString::fromStdString(versionJson["inheritsFrom"].get<std::string>());
        // If inheritsFrom is something like "1.20.1", that's the vanilla version
        if (!inherit.contains("forge") && !inherit.contains("fabric") &&
            !inherit.contains("neoforge") && !inherit.contains("optifine")) {
            return inherit;
        }
    }

    // Try to extract from version ID
    // "1.20.1-forge-47.2.0" -> "1.20.1"
    // "fabric-loader-0.15.7-1.20.1" -> "1.20.1"
    QRegularExpression vanillaRe(R"(^(\d+\.\d+(?:\.\d+)?))");
    auto match = vanillaRe.match(versionId);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    return versionId;
}

// Inheritance resolution

// 带环检测的递归实现（A 继承 B、B 继承 A 的损坏 JSON 会无限递归爆栈）
static json resolveChainWithVisited(const QString &jsonPath, QSet<QString> &visited) {
    QString canonical = QFileInfo(jsonPath).canonicalFilePath();
    if (canonical.isEmpty()) return json();
    if (visited.contains(canonical)) {
        qWarning() << "Circular inheritsFrom detected:" << jsonPath;
        return json();
    }
    visited.insert(canonical);

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) return json();

    QString jsonStr = QString::fromUtf8(file.readAll());
    json result = json::parse(jsonStr.toStdString(), nullptr, false);
    if (result.is_discarded()) return json();

    // If no inheritance, return as-is
    if (!result.contains("inheritsFrom")) return result;

    // inheritsFrom 非字符串（结构畸形）：无法定位父版本，按无继承处理
    if (!result["inheritsFrom"].is_string()) return result;

    std::string inheritId = result["inheritsFrom"].get<std::string>();
    QDir versionDir = QFileInfo(jsonPath).absoluteDir();
    versionDir.cdUp(); // Go to versions dir
    QString parentPath = versionDir.absolutePath() + "/" + QString::fromStdString(inheritId) + "/" +
                         QString::fromStdString(inheritId) + ".json";
    if (!QFile::exists(parentPath)) {
        // 实例内版本的父版本在全局 versions/（实例目录只存游戏文件）
        parentPath = VersionManager::instance().mcFolder() + "versions/" +
                     QString::fromStdString(inheritId) + "/" +
                     QString::fromStdString(inheritId) + ".json";
    }

    json parent = resolveChainWithVisited(parentPath, visited);
    if (parent.is_null()) return result;

    // Merge: child overrides parent
    // Merge arguments.jvm / arguments.game：数组按父版本在前、子版本追加拼接
    // （PCL 用 Newtonsoft JObject.Merge 的 Concat 语义；若子版本独占，
    //  vanilla 的标准启动参数（--username/--uuid/--gameDir 等）会全部丢失）
    for (const char *section : {"jvm", "game"}) {
        if (parent.contains("arguments") && parent["arguments"].contains(section)) {
            if (!result.contains("arguments")) result["arguments"] = json::object();
            if (!result["arguments"].contains(section)) {
                result["arguments"][section] = parent["arguments"][section];
            } else {
                json merged = parent["arguments"][section];
                for (const auto &a : result["arguments"][section]) merged.push_back(a);
                result["arguments"][section] = merged;
            }
        }
    }

    // Merge libraries（按 maven name 去重：子级覆盖父级同名库）
    if (parent.contains("libraries")) {
        if (!result.contains("libraries")) {
            result["libraries"] = parent["libraries"];
        } else {
            QSet<QString> existing;
            for (const auto &lib : result["libraries"])
                existing.insert(QString::fromStdString(lib.value("name", "")));
            for (const auto &lib : parent["libraries"]) {
                QString libName = QString::fromStdString(lib.value("name", ""));
                if (!libName.isEmpty() && existing.contains(libName)) continue;
                result["libraries"].push_back(lib);
            }
        }
    }

    // Merge mainClass
    if (parent.contains("mainClass") && !result.contains("mainClass")) {
        result["mainClass"] = parent["mainClass"];
    }

    // Merge minecraftArguments (old format)
    if (parent.contains("minecraftArguments") && !result.contains("minecraftArguments")) {
        result["minecraftArguments"] = parent["minecraftArguments"];
    }

    // Merge assetIndex
    if (parent.contains("assetIndex") && !result.contains("assetIndex")) {
        result["assetIndex"] = parent["assetIndex"];
    }

    // Merge assets (old format)
    if (parent.contains("assets") && !result.contains("assets")) {
        result["assets"] = parent["assets"];
    }

    return result;
}

json VersionManager::resolveInheritanceChain(const QString &jsonPath) {
    QSet<QString> visited;
    // 兜底：合并过程字段类型畸形（libraries 元素非对象、arguments 非对象/非数组等）
    // 抛 type_error，不能让它穿过 Qt 信号槽，按解析失败返回空 json
    try {
        return resolveChainWithVisited(jsonPath, visited);
    } catch (const std::exception &e) {
        qWarning() << "Failed to resolve version inheritance:" << jsonPath << e.what();
        return json();
    }
}
