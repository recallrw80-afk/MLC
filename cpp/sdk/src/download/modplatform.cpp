#include "download/modplatform.h"
#include "download/downloadmanager.h"
#include "core/settings.h"
#include "cf_key_embedded.h"  // CMake 生成：编译期嵌入的 CF key（发布构建为空串）

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QLoggingCategory>

static Q_LOGGING_CATEGORY(logMod, "mlc.mod")

// 安全读取字符串字段：键缺失、为 null 或类型不符时返回空串
// （nlohmann 的 value(key, "") 在键存在但为 null 时会抛 type_error）
static QString jsonStr(const json &j, const char *key) {
    if (!j.contains(key) || !j[key].is_string()) return {};
    return QString::fromStdString(j[key].get<std::string>());
}

const QString ModPlatform::CF_API = "https://api.curseforge.com/v1";
const QString ModPlatform::CF_MIRROR = "https://mod.mcimirror.top/curseforge/v1";
const QString ModPlatform::MR_API = "https://api.modrinth.com/v2";

ModPlatform& ModPlatform::instance() {
    static ModPlatform m;
    // CF key 解析链（2026-08-07 起）：指令设置（Settings 加密存储）→ 编译期嵌入 → 空走 MCIM 镜像。
    // 注意：任何展示路径都不得回显内嵌 key
    static bool keyResolved = false;
    if (!keyResolved) {
        keyResolved = true;
        m.m_cfApiKey = Settings::instance().getEncrypted("CfApiKey");
        if (m.m_cfApiKey.isEmpty())
            m.m_cfApiKey = QStringLiteral(MLC_CF_API_KEY_EMBEDDED);
        if (m.m_cfApiKey.isEmpty())
            qCInfo(logMod) << "未配置 CurseForge API key，使用 MCIM 镜像";
    }
    return m;
}

// CurseForge 请求辅助

// 无 API key 时把官方地址改写为 MCIM 镜像地址
QString ModPlatform::cfApiUrl(const QString &officialUrl) const {
    if (!m_cfApiKey.isEmpty()) return officialUrl;
    QString mirrored = officialUrl;
    return mirrored.replace(CF_API, CF_MIRROR);
}

// 发起 CF API GET 请求：有 key 走官方并附带 x-api-key，无 key 走 MCIM 镜像；
// 官方返回 401/403/429（key 失效/被吊销/配额超限）时自动回退镜像重试一次，功能降级而非硬挂
void ModPlatform::cfJsonGet(const QString &officialUrl,
                            std::function<void(bool, QString, json)> onComplete) {
    if (m_cfApiKey.isEmpty()) {
        DownloadManager::instance().downloadJson(cfApiUrl(officialUrl), onComplete);
        return;
    }
    QMap<QByteArray, QByteArray> headers;
    headers.insert("x-api-key", m_cfApiKey.toUtf8());
    headers.insert("Accept", "application/json");
    DownloadManager::instance().downloadJsonWithStatus(officialUrl, headers,
        [officialUrl, onComplete](bool ok, int statusCode, QString errOrData, json result) {
            if (!ok && (statusCode == 401 || statusCode == 403 || statusCode == 429)) {
                qCWarning(logMod) << "CF API 返回" << statusCode
                                  << "（key 失效或超限），回退 MCIM 镜像:" << officialUrl;
                QString mirrored = officialUrl;
                DownloadManager::instance().downloadJson(
                    mirrored.replace(CF_API, CF_MIRROR), onComplete);
                return;
            }
            if (onComplete) onComplete(ok, errOrData, result);
        });
}

// Public API — search

// CF classId（gameId=432 Minecraft 下的分类）
static int cfClassIdFor(ModPlatform::ResourceType type) {
    switch (type) {
    case ModPlatform::Mod:          return 6;
    case ModPlatform::ModPack:      return 4471;
    case ModPlatform::ResourcePack: return 12;
    case ModPlatform::Shader:       return 6552;
    case ModPlatform::DataPack:     return 6945;
    }
    return 6;
}

// Modrinth facets 的 project_type
static const char *mrProjectTypeFor(ModPlatform::ResourceType type) {
    switch (type) {
    case ModPlatform::Mod:          return "mod";
    case ModPlatform::ModPack:      return "modpack";
    case ModPlatform::ResourcePack: return "resourcepack";
    case ModPlatform::Shader:       return "shader";
    case ModPlatform::DataPack:     return "datapack";
    }
    return "mod";
}

void ModPlatform::searchMods(Platform platform, const QString &query, int page, int pageSize,
                               std::function<void(bool, QList<ModResource>)> onComplete) {
    searchResources(platform, Mod, query, page, pageSize, onComplete);
}

void ModPlatform::searchResources(Platform platform, ResourceType type, const QString &query,
                                    int page, int pageSize,
                                    std::function<void(bool, QList<ModResource>)> onComplete) {
    switch (platform) {
    case CurseForge: searchCurseForge(query, page, pageSize, cfClassIdFor(type), onComplete); break;
    case Modrinth:   searchModrinth(query, page, pageSize, mrProjectTypeFor(type), onComplete); break;
    }
}

void ModPlatform::getModDetails(Platform platform, const QString &modId,
                                  std::function<void(bool, ModResource)> onComplete) {
    switch (platform) {
    case CurseForge: getCurseForgeModDetails(modId, onComplete); break;
    case Modrinth:   getModrinthModDetails(modId, onComplete); break;
    }
}

void ModPlatform::getModFiles(Platform platform, const QString &modId,
                                std::function<void(bool, QList<ModFileInfo>)> onComplete) {
    switch (platform) {
    case CurseForge: getCurseForgeFiles(modId, onComplete); break;
    case Modrinth:   getModrinthFiles(modId, onComplete); break;
    }
}

QString ModPlatform::getFileDownloadUrl(Platform platform,
                                          const QString &modId, const QString &fileId) {
    switch (platform) {
    case CurseForge:
        return QString("%1/mods/%2/files/%3/download-url").arg(CF_API, modId, fileId);
    case Modrinth:
        // Modrinth 没有 /project/{id}/version/{vid} 路由；单版本走 GET /version/{id}
        // （fileId 在上层就是 version id，见 getModrinthFiles）
        return QString("%1/version/%2").arg(MR_API, fileId);
    }
    return {};
}

void ModPlatform::downloadMod(Platform platform, const QString &modId,
                                const QString &fileId, const QString &savePath,
                                std::function<void(bool, QString)> onComplete,
                                std::function<void(qint64, qint64)> onProgress) {
    if (platform == CurseForge) {
        // CurseForge：先解析下载 URL，再下载文件
        // 受限文件的 data 字段为 null（禁止第三方分发），jsonStr 安全处理
        QString url = getFileDownloadUrl(platform, modId, fileId);
        cfJsonGet(url, [savePath, onComplete, onProgress](bool ok, QString, json result) {
                if (!ok) { onComplete(false, "Failed to get download URL"); return; }
                QString dlUrl = jsonStr(result, "data");
                if (dlUrl.isEmpty()) { onComplete(false, "Empty download URL"); return; }

                DownloadManager::instance().download(dlUrl, savePath, onProgress, onComplete);
            });
    } else {
        // Modrinth: version endpoint returns file info with download URL
        QString url = getFileDownloadUrl(platform, modId, fileId);
        DownloadManager::instance().downloadJson(
            url, [savePath, onComplete, onProgress](bool ok, QString, json ver) {
                if (!ok) { onComplete(false, "Failed to get version info"); return; }
                if (!ver.contains("files") || ver["files"].empty()) {
                    onComplete(false, "No files in version"); return;
                }
                QString dlUrl = jsonStr(ver["files"][0], "url");
                if (dlUrl.isEmpty()) { onComplete(false, "Empty download URL"); return; }
                DownloadManager::instance().download(dlUrl, savePath, onProgress, onComplete);
            });
    }
}

// CurseForge API implementation

void ModPlatform::searchCurseForge(const QString &query, int page, int pageSize, int classId,
                                     std::function<void(bool, QList<ModResource>)> onComplete) {
    QUrl url(CF_API + "/mods/search");
    QUrlQuery q;
    q.addQueryItem("gameId", "432"); // Minecraft
    q.addQueryItem("classId", QString::number(classId));
    if (!query.isEmpty()) q.addQueryItem("searchFilter", query);
    q.addQueryItem("index", QString::number(page * pageSize));
    q.addQueryItem("pageSize", QString::number(pageSize));
    q.addQueryItem("sortField", "2"); // Popularity
    q.addQueryItem("sortOrder", "desc");
    url.setQuery(q);

    cfJsonGet(url.toString(), [onComplete](bool ok, QString, json result) {
            QList<ModResource> mods;
            if (!ok || !result.contains("data")) { onComplete(false, mods); return; }

            for (const auto &item : result["data"]) {
                ModResource r;
                r.id = QString::number(item.value("id", 0));
                r.name = jsonStr(item, "name");
                r.summary = jsonStr(item, "summary");
                r.author = (item.contains("authors") && item["authors"].is_array() && !item["authors"].empty())
                    ? jsonStr(item["authors"][0], "name") : QString();
                r.iconUrl = (item.contains("logo") && item["logo"].is_object())
                    ? jsonStr(item["logo"], "thumbnailUrl") : QString();
                r.downloadCount = item.value("downloadCount", 0);
                r.websiteUrl = (item.contains("links") && item["links"].is_object())
                    ? jsonStr(item["links"], "websiteUrl") : QString();
                r.lastUpdated = 0;
                if (item.contains("latestFiles") && !item["latestFiles"].empty()) {
                    for (const auto &gv : item["latestFiles"][0].value("gameVersions", json::array())) {
                        if (gv.is_string())
                            r.versions.append(QString::fromStdString(gv.get<std::string>()));
                    }
                }
                mods.append(r);
            }
            onComplete(true, mods);
        });
}

void ModPlatform::getCurseForgeModDetails(const QString &modId,
                                            std::function<void(bool, ModResource)> onComplete) {
    QString url = CF_API + "/mods/" + modId;

    cfJsonGet(url, [this, modId, onComplete](bool ok, QString, json result) {
            ModResource r;
            if (!ok || !result.contains("data")) { onComplete(false, r); return; }
            auto &item = result["data"];
            r.id = QString::number(item.value("id", 0));
            r.name = jsonStr(item, "name");
            r.summary = jsonStr(item, "summary");
            r.downloadCount = item.value("downloadCount", 0);
            // /mods/{id} 的响应不含正文：正文必须另请求 /mods/{id}/description
            // （CF 返回的是 HTML 片段字符串，这里原样带回由前端渲染）
            QString descUrl = CF_API + "/mods/" + modId + "/description";
            cfJsonGet(descUrl, [r, onComplete](bool ok2, QString, json desc) mutable {
                if (ok2 && desc.contains("data") && desc["data"].is_string())
                    r.description = QString::fromStdString(desc["data"].get<std::string>());
                onComplete(true, r);
            });
        });
}

void ModPlatform::getCurseForgeFiles(const QString &modId,
                                       std::function<void(bool, QList<ModFileInfo>)> onComplete) {
    QString url = CF_API + "/mods/" + modId + "/files";

    cfJsonGet(url, [onComplete](bool ok, QString, json result) {
            QList<ModFileInfo> files;
            if (!ok || !result.contains("data")) { onComplete(false, files); return; }
            for (const auto &item : result["data"]) {
                ModFileInfo f;
                f.id = QString::number(item.value("id", 0));
                f.displayName = jsonStr(item, "displayName");
                f.fileName = jsonStr(item, "fileName");
                f.downloadUrl = jsonStr(item, "downloadUrl");
                f.fileSize = item.value("fileLength", 0);
                // 哈希优先取 SHA1（algo=1），盲取 [0] 可能拿到 MD5
                if (item.contains("hashes") && item["hashes"].is_array()) {
                    for (const auto &h : item["hashes"]) {
                        QString v = jsonStr(h, "value");
                        if (v.isEmpty()) continue;
                        if (h.value("algo", 0) == 1) { f.sha1 = v; break; }
                        if (f.sha1.isEmpty()) f.sha1 = v;
                    }
                }
                for (const auto &gv : item.value("gameVersions", json::array())) {
                    if (gv.is_string())
                        f.gameVersions.append(QString::fromStdString(gv.get<std::string>()));
                }
                files.append(f);
            }
            onComplete(true, files);
        });
}

// Modrinth API implementation

void ModPlatform::searchModrinth(const QString &query, int page, int pageSize, const QString &projectType,
                                   std::function<void(bool, QList<ModResource>)> onComplete) {
    QUrl url(MR_API + "/search");
    QUrlQuery q;
    if (!query.isEmpty()) q.addQueryItem("query", query);
    q.addQueryItem("offset", QString::number(page * pageSize));
    q.addQueryItem("limit", QString::number(pageSize));
    q.addQueryItem("facets", QString("[[\"project_type:%1\"]]").arg(projectType));
    url.setQuery(q);

    DownloadManager::instance().downloadJson(
        url.toString(), [onComplete](bool ok, QString, json result) {
            QList<ModResource> mods;
            if (!ok || !result.contains("hits")) { onComplete(false, mods); return; }
            for (const auto &item : result["hits"]) {
                ModResource r;
                r.id = jsonStr(item, "project_id");
                r.name = jsonStr(item, "title");
                r.summary = jsonStr(item, "description");
                r.author = jsonStr(item, "author");
                r.iconUrl = jsonStr(item, "icon_url");
                r.downloadCount = item.value("downloads", 0);
                r.websiteUrl = QString("https://modrinth.com/mod/%1").arg(r.id);
                if (item.contains("versions")) {
                    for (const auto &v : item["versions"]) {
                        if (v.is_string())
                            r.versions.append(QString::fromStdString(v.get<std::string>()));
                    }
                }
                mods.append(r);
            }
            onComplete(true, mods);
        });
}

void ModPlatform::getModrinthModDetails(const QString &modId,
                                          std::function<void(bool, ModResource)> onComplete) {
    QString url = MR_API + "/project/" + modId;
    DownloadManager::instance().downloadJson(
        url, [onComplete](bool ok, QString, json item) {
            ModResource r;
            if (!ok) { onComplete(false, r); return; }
            r.id = jsonStr(item, "id");
            r.name = jsonStr(item, "title");
            r.summary = jsonStr(item, "description");
            r.description = jsonStr(item, "body");
            r.downloadCount = item.value("downloads", 0);
            r.websiteUrl = QString("https://modrinth.com/mod/%1").arg(r.id);
            onComplete(true, r);
        });
}

void ModPlatform::getModrinthFiles(const QString &modId,
                                     std::function<void(bool, QList<ModFileInfo>)> onComplete) {
    QString url = MR_API + "/project/" + modId + "/version";
    DownloadManager::instance().downloadJson(
        url, [onComplete](bool ok, QString, json result) {
            QList<ModFileInfo> files;
            if (!ok || !result.is_array()) { onComplete(false, files); return; }
            for (const auto &ver : result) {
                if (!ver.contains("files") || ver["files"].empty()) continue;
                ModFileInfo f;
                f.id = jsonStr(ver, "id");
                f.displayName = jsonStr(ver, "name");
                f.fileName = jsonStr(ver["files"][0], "filename");
                f.downloadUrl = jsonStr(ver["files"][0], "url");
                f.fileSize = ver["files"][0].value("size", 0);
                f.sha1 = (ver["files"][0].contains("hashes") && ver["files"][0]["hashes"].is_object())
                    ? jsonStr(ver["files"][0]["hashes"], "sha1") : QString();
                for (const auto &gv : ver.value("game_versions", json::array())) {
                    if (gv.is_string())
                        f.gameVersions.append(QString::fromStdString(gv.get<std::string>()));
                }
                for (const auto &ld : ver.value("loaders", json::array())) {
                    if (ld.is_string())
                        f.loaders.append(QString::fromStdString(ld.get<std::string>()));
                }
                files.append(f);
            }
            onComplete(true, files);
        });
}
