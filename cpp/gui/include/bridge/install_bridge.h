#ifndef MLC_INSTALL_BRIDGE_H
#define MLC_INSTALL_BRIDGE_H

#include <QObject>
#include <QString>
#include <QStringList>

// 下载/安装任务的 QML 桥接层：包装 mlc:: 的导入与安装 API
// importModpack 本身异步；installVersion/installJavaRuntime 是同步阻塞，
// 统一放 QtConcurrent 工作线程执行，进度/结果经 QTimer::singleShot 回到 UI 线程
class InstallBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)

public:
    static InstallBridge& instance();

    bool busy() const { return m_busy; }
    int progressPercent() const { return m_percent; }
    QString progressText() const { return m_text; }

    /// 导入整合包（异步）。targetInstance 仅 Mod 包需要（目标实例显示名），其余传空
    Q_INVOKABLE void importModpack(const QString &filePath, const QString &targetInstance = QString());

    /// 下载/补齐原版 MC（空串 = 最新正式版），工作线程执行
    Q_INVOKABLE void installMcVersion(const QString &versionId);

    /// 下载安装指定大版本 JRE（Adoptium），工作线程执行
    Q_INVOKABLE void installJava(int majorVersion);

signals:
    void busyChanged();
    void progressChanged();
    /// 导入完成：!ok 时 msg 为原因；mod 包缺 targetInstance 时 data 为实例列表
    void importFinished(bool ok, const QString &msg, const QStringList &data);
    /// MC 安装完成
    void mcInstallFinished(bool ok, const QString &msg);
    /// JRE 安装完成
    void javaInstallFinished(bool ok, const QString &msg);

private:
    InstallBridge() = default;

    /// 单任务守卫：busy 时拒绝新任务（返回 false）
    bool tryStart(const QString &firstStep);
    void setProgress(const QString &text, int percent);
    void finish();

    bool m_busy = false;
    int m_percent = 0;
    QString m_text;
};

#endif // MLC_INSTALL_BRIDGE_H
