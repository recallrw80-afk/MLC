#ifndef MLC_VERSIONMANAGER_H
#define MLC_VERSIONMANAGER_H

#include <QObject>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QVariantList>
#include <nlohmann/json.hpp>
#include "core/types.h"
#include "core/mlccore_export.h"

class QNetworkAccessManager;

using json = nlohmann::json;

/**
 * Minecraft version manager.
 * Handles version list parsing, JSON reading, inheritance chains,
 * mod loader detection, and Minecraft folder management.
 */
class MLCCORE_EXPORT VersionManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mcFolder READ mcFolder WRITE setMcFolder NOTIFY mcFolderChanged)
    Q_PROPERTY(QStringList versionIds READ versionIds NOTIFY versionListChanged)
    Q_PROPERTY(int versionCount READ versionCount NOTIFY versionListChanged)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY loadingChanged)

public:
    static VersionManager& instance();

    // ---- Minecraft folder ----

    QString mcFolder();
    void setMcFolder(const QString &path, bool persist = true);

    QList<McFolder> loadFolderList();

    // ---- Version listing ----

    /// Load local instances from INI [Instances] mappings
    Q_INVOKABLE void loadLocalVersions();

    /// Load vanilla MC versions from versions/ directory
    Q_INVOKABLE void loadMcVersions();

    /// Fetch version manifest from Mojang API
    Q_INVOKABLE void fetchVersionManifest();

    /// QML 用：远程可下载版本列表（[{id, type, releaseTime}]，isLocal=false 的条目）
    Q_INVOKABLE QVariantList remoteVersionList() const;

    /// Get all known version IDs (local + remote)
    QStringList versionIds() const;
    int versionCount() const { return m_versionList.size(); }
    bool isLoading() const { return m_isLoading; }

    // ---- Version parsing ----

    /// Load and parse a specific version's JSON
    McVersion loadVersion(const QString &versionId);

    /// 解析实例版本：实例 Setup.ini 的 Version 键 → 实例 versions/ 扫描；
    /// version json 实例内优先、全局 versions/ 兜底；PathIndie 按 PCL 版本隔离语义判定
    McVersion loadInstanceVersion(const QString &dirName);

    /// Parse version JSON from file path
    McVersion parseVersionJson(const QString &jsonPath);

    /// Parse version JSON from json object
    McVersion parseVersionJson(const json &j, const QString &versionId);

    // ---- Accessors ----

    const QList<McVersionInfo>& versionList() const { return m_versionList; }
    const McVersion* selectedVersion() const { return m_selectedVersion; }
    void setSelectedVersion(const McVersion *ver) { m_selectedVersion = ver; }

    // ---- Static utilities ----

    /// Detect mod loaders from version JSON
    static McModLoaderInfo detectModLoaders(const json &versionJson);

    /// Get the vanilla base version from a modded version
    static QString detectVanillaVersion(const json &versionJson, const QString &versionId);

    /// Resolve inheritance chain: follow "inheritsFrom" to the top, merging args
    static json resolveInheritanceChain(const QString &jsonPath);

signals:
    void mcFolderChanged(const QString &path);
    void versionListChanged();
    void loadingChanged();
    void versionLoadProgress(const QString &status);
    void versionLoadError(const QString &versionId, const QString &error);

private:
    VersionManager() = default;

    // m_mutex 保护 m_mcFolder：Java 扫描线程会经 mcFolder() 跨线程读取
    mutable QMutex m_mutex;
    QString m_mcFolder;
    QList<McVersionInfo> m_versionList;
    QNetworkAccessManager *m_nam = nullptr;
    const McVersion *m_selectedVersion = nullptr;
    bool m_isLoading = false;
};

#endif // MLC_VERSIONMANAGER_H
