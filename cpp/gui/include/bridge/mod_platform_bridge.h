#ifndef MLC_MOD_PLATFORM_BRIDGE_H
#define MLC_MOD_PLATFORM_BRIDGE_H

#include <QObject>
#include <QString>
#include <QVariantList>

// Mod 平台（CurseForge/Modrinth）的 QML 桥接层：ModPlatform 接口全是 std::function
// 回调，QML 不可直接调用，本类包装为 Q_INVOKABLE + Qt 信号
class ModPlatformBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)

public:
    static ModPlatformBridge& instance();

    bool searching() const { return m_searching; }
    bool downloading() const { return m_downloading; }

    /// 搜索资源（platform: 0=CurseForge 1=Modrinth；
    /// category: 0=Mod 1=整合包 2=材质包 3=光影包 4=数据包；page 从 0 起）
    /// 结果经 searchFinished 返回 [{id, name, summary, author, iconUrl, websiteUrl, downloadCount, versions}]
    Q_INVOKABLE void search(int platform, int category, const QString &query, int page);

    /// 拉取 Mod 的文件列表，结果经 modFilesFinished 返回
    /// [{id, displayName, fileName, gameVersions, loaders, fileSize, sha1, releaseDate}]
    Q_INVOKABLE void getModFiles(int platform, const QString &modId);

    /// 下载指定文件到实例子目录（fileName 取自文件列表条目；
    /// targetSubDir：mods / resourcepacks / shaderpacks）
    /// 结果经 downloadFinished(ok, msg) 返回；进度经 downloadProgressChanged 属性
    Q_INVOKABLE void downloadModToInstance(int platform, const QString &modId,
                                           const QString &fileId, const QString &fileName,
                                           const QString &instanceName,
                                           const QString &targetSubDir = "mods");

    /// 下载整合包并导入（下载到 {mcFolder}/cache/ 暂存，完成后自动走导入管线并清理暂存文件）
    /// 结果经 modpackImportFinished(ok, msg, data) 返回；下载/导入进度均经 downloadPercent
    Q_INVOKABLE void downloadModpackAndImport(int platform, const QString &modId,
                                              const QString &fileId, const QString &fileName);

    Q_PROPERTY(int downloadPercent READ downloadPercent NOTIFY downloadProgressChanged)
    int downloadPercent() const { return m_downloadPercent; }

signals:
    void searchingChanged();
    void downloadingChanged();
    void downloadProgressChanged();
    void searchFinished(bool ok, const QVariantList &mods);
    void modFilesFinished(bool ok, const QString &modId, const QVariantList &files);
    void downloadFinished(bool ok, const QString &msg);
    /// 整合包下载+导入完成：!ok 时 msg 为原因；mod 包缺 targetInstance 时 data 为实例列表
    void modpackImportFinished(bool ok, const QString &msg, const QStringList &data);

private:
    ModPlatformBridge() = default;

    bool m_searching = false;
    bool m_downloading = false;
    int m_downloadPercent = 0;
};

#endif // MLC_MOD_PLATFORM_BRIDGE_H
