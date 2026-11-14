#include "bridge/mod_platform_bridge.h"

#include <QDir>
#include <QTimer>

#include "mlc.h"
#include "core/versionmanager.h"
#include "download/modplatform.h"

ModPlatformBridge& ModPlatformBridge::instance() {
    static ModPlatformBridge b;
    return b;
}

static QVariantMap modResourceToMap(const ModResource &r) {
    return {
        {"id", r.id}, {"name", r.name}, {"summary", r.summary},
        {"author", r.author}, {"iconUrl", r.iconUrl}, {"websiteUrl", r.websiteUrl},
        {"downloadCount", r.downloadCount}, {"versions", r.versions},
    };
}

static ModPlatform::Platform toPlatform(int platform) {
    return platform == 1 ? ModPlatform::Modrinth : ModPlatform::CurseForge;
}

void ModPlatformBridge::search(int platform, int category, const QString &query, int page) {
    if (m_searching) return;
    m_searching = true;
    emit searchingChanged();

    auto type = static_cast<ModPlatform::ResourceType>(qBound(0, category, 4));
    ModPlatform::instance().searchResources(toPlatform(platform), type, query, page, 25,
        [this](bool ok, QList<ModResource> mods) {
            QTimer::singleShot(0, this, [this, ok, mods]() {
                QVariantList out;
                for (const auto &r : mods) out.append(modResourceToMap(r));
                m_searching = false;
                emit searchingChanged();
                emit searchFinished(ok, out);
            });
        });
}

void ModPlatformBridge::getModFiles(int platform, const QString &modId) {
    ModPlatform::instance().getModFiles(
        toPlatform(platform),
        modId,
        [this, modId](bool ok, QList<ModFileInfo> files) {
            QTimer::singleShot(0, this, [this, ok, modId, files]() {
                QVariantList out;
                for (const auto &f : files) {
                    out.append(QVariantMap{
                        {"id", f.id}, {"displayName", f.displayName},
                        {"fileName", f.fileName}, {"gameVersions", f.gameVersions},
                        {"loaders", f.loaders}, {"fileSize", f.fileSize},
                        {"sha1", f.sha1}, {"releaseDate", f.releaseDate},
                    });
                }
                emit modFilesFinished(ok, modId, out);
            });
        });
}

void ModPlatformBridge::downloadModToInstance(int platform, const QString &modId,
                                              const QString &fileId, const QString &fileName,
                                              const QString &instanceName,
                                              const QString &targetSubDir) {
    if (m_downloading) return;
    // 子目录只允许简单名字，防路径拼接出格
    if (targetSubDir.isEmpty() || targetSubDir.contains('/') || targetSubDir.contains("..")) {
        emit downloadFinished(false, QStringLiteral("非法目标子目录"));
        return;
    }
    auto info = mlc::instanceInfo(instanceName);
    if (info.dirName.isEmpty()) {
        emit downloadFinished(false, QStringLiteral("实例不存在: ") + instanceName);
        return;
    }
    m_downloading = true;
    m_downloadPercent = 0;
    emit downloadingChanged();
    emit downloadProgressChanged();

    QDir().mkpath(info.path + targetSubDir);
    QString savePath = info.path + targetSubDir + "/" + fileName;
    ModPlatform::instance().downloadMod(
        toPlatform(platform),
        modId, fileId, savePath,
        [this](bool ok, QString msg) {
            QTimer::singleShot(0, this, [this, ok, msg]() {
                m_downloading = false;
                emit downloadingChanged();
                emit downloadFinished(ok, msg);
            });
        },
        [this](qint64 received, qint64 total) {
            if (total <= 0) return;
            QTimer::singleShot(0, this, [this, received, total]() {
                m_downloadPercent = int(received * 100 / total);
                emit downloadProgressChanged();
            });
        });
}

void ModPlatformBridge::downloadModpackAndImport(int platform, const QString &modId,
                                                 const QString &fileId, const QString &fileName) {
    if (m_downloading) return;
    m_downloading = true;
    m_downloadPercent = 0;
    emit downloadingChanged();
    emit downloadProgressChanged();

    // 暂存到 cache/（不能放 tmp/——导入管线入口会清空 tmp/），导入完成无论成败都删暂存
    QString stageDir = VersionManager::instance().mcFolder() + "cache/";
    QDir().mkpath(stageDir);
    QString savePath = stageDir + fileName;

    ModPlatform::instance().downloadMod(
        toPlatform(platform),
        modId, fileId, savePath,
        [this, savePath](bool ok, QString msg) {
            if (!ok) {
                QTimer::singleShot(0, this, [this, ok, msg]() {
                    m_downloading = false;
                    emit downloadingChanged();
                    emit modpackImportFinished(false, msg, {});
                });
                return;
            }
            // 下载成功 → 走导入管线（进度与完成回调都投递回 UI 线程）
            mlc::importModpack(savePath, QString(), QString(),
                [this](const mlc::ImportProgress &p) {
                    QTimer::singleShot(0, this, [this, percent = p.percent]() {
                        m_downloadPercent = percent;
                        emit downloadProgressChanged();
                    });
                },
                [this, savePath](bool ok2, const QString &msg2, const QStringList &data) {
                    QTimer::singleShot(0, this, [this, ok2, msg2, data, savePath]() {
                        QFile::remove(savePath);
                        m_downloading = false;
                        emit downloadingChanged();
                        emit modpackImportFinished(ok2, msg2, data);
                    });
                });
        },
        [this](qint64 received, qint64 total) {
            if (total <= 0) return;
            QTimer::singleShot(0, this, [this, received, total]() {
                m_downloadPercent = int(received * 100 / total);
                emit downloadProgressChanged();
            });
        });
}
