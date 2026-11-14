#ifndef MLC_ASSETDOWNLOADER_H
#define MLC_ASSETDOWNLOADER_H

#include <QObject>
#include <functional>
#include <nlohmann/json.hpp>
#include "core/types.h"
#include "core/mlccore_export.h"

using json = nlohmann::json;

/**
 * Minecraft asset and library downloader.
 * Handles version JSON, JAR, assets, and native library download/extraction.
 */
class MLCCORE_EXPORT AssetDownloader : public QObject
{
    Q_OBJECT

public:
    static AssetDownloader& instance();

    /// Download a full Minecraft version (JSON + JAR + assets + libraries)
    void downloadVersion(const QString &versionId,
                         std::function<void(bool, QString)> onComplete);

    /// Download version JSON from Mojang
    void downloadVersionJson(const QString &versionId,
                              std::function<void(bool, QString)> onComplete);

    /// Download the client JAR (including verification)
    void downloadClientJar(const McVersion &version,
                            std::function<void(bool, QString)> onComplete);

    /// Download all libraries for a version
    void downloadLibraries(const McVersion &version,
                            const json &versionJson,
                            std::function<void(bool, QString)> onComplete);

    /// Download asset index and all assets
    void downloadAssets(const McVersion &version,
                         std::function<void(bool, QString)> onComplete);

    /// Download and extract native libraries
    void downloadNatives(const McVersion &version,
                          std::function<void(bool, QString)> onComplete);

signals:
    void downloadProgress(const QString &task, int current, int total);
    void downloadLog(const QString &message);

private:
    AssetDownloader() = default;

    /// Build asset index path from hash
    static QString assetPathFromHash(const QString &hash);

    /// Verify file SHA1 matches expected hash
    static bool verifySha1(const QString &filePath, const QString &expectedHash);

    int m_totalTasks = 0;
    int m_completedTasks = 0;
};

#endif // MLC_ASSETDOWNLOADER_H
