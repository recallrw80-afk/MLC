#ifndef MLC_LAUNCHER_H
#define MLC_LAUNCHER_H

#include <QObject>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include "core/types.h"
#include "core/mlccore_export.h"

/**
 * Minecraft game launcher using QProcess.
 * Handles process setup, environment variables, stdout/stderr capture,
 * game window detection, and process lifecycle management.
 */
class MLCCORE_EXPORT Launcher : public QObject
{
    Q_OBJECT
    Q_PROPERTY(LaunchState launchState READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool isRunning READ isRunning NOTIFY stateChanged)

public:
    enum LaunchState {
        Idle = 0,
        Prechecking,
        GettingJava,
        LoggingIn,
        Downloading,
        Launching,
        Running,
        Finished,
        Failed,
        Interrupted
    };
    Q_ENUM(LaunchState)

    static Launcher& instance();

    // ---- Launch ----

    /// Start the Minecraft launch sequence (C++ API)
    Q_INVOKABLE bool launch(const McVersion &version,
                             const JavaEntry &java,
                             const LoginResult &login,
                             const McLaunchOptions &options = McLaunchOptions());

    /// QML-friendly: launch by version ID. The caller supplies the login
    /// credentials (e.g. from OfflineAuth.createOfflineLogin or an MS login
    /// result) — the launcher no longer assumes offline "Player".
    Q_INVOKABLE bool launchVersion(const QString &versionId, const LoginResult &login);

    /// Interrupt/cancel the launch
    Q_INVOKABLE void interrupt();

    // ---- State ----

    LaunchState state() const { return m_state; }
    QString statusText() const { return m_statusText; }
    int progress() const { return m_progress; }
    bool isRunning() const { return m_state == LaunchState::Running; }
    QProcess* gameProcess() { return m_gameProcess; }

signals:
    void stateChanged();
    void statusTextChanged();
    void progressChanged();
    void gameLog(const QString &line);
    void gameStarted();
    void gameExited(int exitCode, const QString &reason);
    void launchFailed(const QString &error);

private slots:
    void onGameStdout();
    void onGameStderr();
    void onGameStarted();
    void onGameFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onGameError(QProcess::ProcessError error);

private:
    Launcher();
    void setState(LaunchState newState);
    void setStatus(const QString &text);
    void setProgress(int value);
    void appendLog(const QString &line);
    void doLaunch();
    /// 打开本次启动的落盘日志（<mc>/logs/mlc-launch-<时间戳>.log，滚动保留 10 份）
    void openLaunchLog();
    /// 按行处理游戏输出（buffer 保留不完整行；字节级缓冲避免 UTF-8 跨界乱码）
    void processGameOutput(QByteArray &buffer, const QByteArray &data);
    /// 冲刷缓冲中无换行结尾的残留行（进程退出时调用）
    void flushLogBuffer(QByteArray &buffer);

    // Data for current launch
    McVersion m_version;
    JavaEntry m_java;
    LoginResult m_login;
    McLaunchOptions m_options;

    QProcess *m_gameProcess = nullptr;
    LaunchState m_state = LaunchState::Idle;
    QString m_statusText;
    QByteArray m_logBuffer;     // stdout 不完整行缓冲
    QByteArray m_logBufferErr;  // stderr 不完整行缓冲（与 stdout 分开，防止两通道粘行）
    QFile m_logFile;            // 本次启动的落盘日志（mc/logs/mlc-launch-*.log）
    int m_logLinesSinceFlush = 0;
    int m_progress = 0;
};

#endif // MLC_LAUNCHER_H
