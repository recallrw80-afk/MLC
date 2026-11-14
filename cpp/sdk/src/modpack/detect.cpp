// 整合包类型检测（detectPackType / packTypeName）
#include "modpack_common.h"
#include "util/file_utils.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

PackType detectPackType(const QString &filePath) {
    // 列出 zip 条目（Qt 实现，无外部进程依赖）
    QStringList entries = FileUtils::listZipEntries(filePath);
    if (entries.isEmpty()) return PackType::Unknown;

    // 收集根目录和一级子目录的文件名
    QStringList roots, firstLevel;
    for (const auto &e : entries) {
        if (!e.contains('/')) {
            roots.append(e);
        } else {
            int slash = e.indexOf('/');
            if (e.lastIndexOf('/') == slash) {  // exactly one slash
                firstLevel.append(e);
            }
        }
    }

    auto hasRoot = [&](const QString &name) { return roots.contains(name); };
    // firstLevel entries are "DirName/filename", match on the filename part
    auto hasFirst = [&](const QString &name) {
        for (const auto &e : firstLevel) {
            int slash = e.indexOf('/');
            if (slash != -1 && e.mid(slash + 1) == name) return true;
        }
        return false;
    };

    // 按优先级检测
    // Type 3: MCBBS — mcbbs.packmeta (root or first-level)
    if (hasRoot("mcbbs.packmeta") || hasFirst("mcbbs.packmeta"))
        return PackType::MCBBS;

    // Type 2: MultiMC — mmc-pack.json
    if (hasRoot("mmc-pack.json") || hasFirst("mmc-pack.json"))
        return PackType::MultiMC;

    // Type 4: Modrinth
    if (hasRoot("modrinth.index.json") || hasFirst("modrinth.index.json"))
        return PackType::Modrinth;

    // Type 0/3: manifest.json
    if (hasRoot("manifest.json") || hasFirst("manifest.json")) {
        // 需要读取内容判断是否有 addons 键
        QString manifestPath;
        if (hasRoot("manifest.json")) manifestPath = "manifest.json";
        else {
            for (const auto &e : firstLevel) {
                if (e.endsWith("/manifest.json")) { manifestPath = e; break; }
            }
        }
        // 提取并解析 manifest.json
        QByteArray data = FileUtils::readZipEntry(filePath, manifestPath);
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isNull()) {
            QJsonObject obj = doc.object();
            if (obj.contains("addons"))
                return PackType::MCBBS;  // has addons → MCBBS
            else
                return PackType::CurseForge;  // no addons → CurseForge
        }
        return PackType::CurseForge;  // fallback
    }

    // Type 1: HMCL — modpack.json
    if (hasRoot("modpack.json") || hasFirst("modpack.json"))
        return PackType::HMCL;

    // Type 9: Launcher pack — contains modpack.zip or modpack.mrpack
    if (hasRoot("modpack.zip") || hasRoot("modpack.mrpack") ||
        hasFirst("modpack.zip") || hasFirst("modpack.mrpack"))
        return PackType::LauncherPack;

    // Type 9b: Launcher pack — 含单个内层 zip/mrpack 时先递归探测（内层是整合包结构即外壳包）。
    // 必须在 Type 5 Mod 判定之前：外壳包也带 jar，先判 Mod 会把套娃包误判成纯 Mod 包
    {
        QString innerZipPath;  // 内层 zip 在压缩包内的完整路径
        int  zipCount = 0;
        for (const auto &e : entries) {
            int slashes = e.count('/');
            if (slashes > 1) continue;
            QString fn = slashes == 0 ? e : e.mid(e.indexOf('/') + 1);
            if (fn.endsWith(".zip", Qt::CaseInsensitive) || fn.endsWith(".mrpack", Qt::CaseInsensitive)) {
                innerZipPath = e;  // 保留完整路径用于解出检测
                zipCount++;
            }
        }

        if (zipCount == 1) {
            // 把内层 zip 解出到唯一临时文件再检测（避免固定路径并发冲突）
            QString tmpInner = QDir::temp().filePath(
                QString("_mlc_detect_%1.zip").arg(QCoreApplication::applicationPid()));
            QFile::remove(tmpInner);
            QStringList innerEntries;
            if (FileUtils::extractZipEntry(filePath, innerZipPath, tmpInner))
                innerEntries = FileUtils::listZipEntries(tmpInner);
            QFile::remove(tmpInner);
            for (const auto &e : innerEntries) {
                if (e.startsWith(".minecraft/") || e.contains("/.minecraft/") || e.contains("versions/"))
                    return PackType::LauncherPack;
            }
        }
    }

    // Fallback: check if it looks like a .minecraft folder
    for (const auto &e : entries) {
        // pattern: */versions/*/X.json
        if (e.contains("versions/") && e.endsWith(".json"))
            return PackType::Compressed;
    }

    // Type 5: Mod 包 —— 找不到 .minecraft/清单，只有 jar（mod 导入，非整合包）
    for (const auto &e : entries) {
        if (e.endsWith(".jar", Qt::CaseInsensitive))
            return PackType::Mod;
    }

    return PackType::Unknown;
}

QString packTypeName(PackType type) {
    switch (type) {
    case PackType::CurseForge:   return "CurseForge";
    case PackType::HMCL:         return "HMCL";
    case PackType::MultiMC:      return "MultiMC (MMC)";
    case PackType::MCBBS:        return "MCBBS";
    case PackType::Modrinth:     return "Modrinth";
    case PackType::Mod:          return "Mod 包 (Mod Pack)";
    case PackType::LauncherPack: return "Launcher Pack";
    case PackType::Compressed:   return "Compressed .minecraft";
    default:                     return "Unknown";
    }
}
