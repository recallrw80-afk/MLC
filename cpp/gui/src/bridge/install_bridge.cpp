#include "bridge/install_bridge.h"

#include <QTimer>
#include <QtConcurrent>

#include "mlc.h"

InstallBridge& InstallBridge::instance() {
    static InstallBridge b;
    return b;
}

bool InstallBridge::tryStart(const QString &firstStep) {
    if (m_busy) return false;
    m_busy = true;
    emit busyChanged();
    setProgress(firstStep, 0);
    return true;
}

void InstallBridge::setProgress(const QString &text, int percent) {
    m_text = text;
    m_percent = percent;
    emit progressChanged();
}

void InstallBridge::finish() {
    m_busy = false;
    emit busyChanged();
}

void InstallBridge::importModpack(const QString &filePath, const QString &targetInstance) {
    if (!tryStart(QStringLiteral("正在解析整合包..."))) return;

    mlc::importModpack(filePath, QString(), targetInstance,
        [this](const mlc::ImportProgress &p) {
            // 回调可能来自 SDK 内部任意线程，统一投递到 UI 线程
            QTimer::singleShot(0, this, [this, step = p.step, percent = p.percent]() {
                setProgress(step, percent);
            });
        },
        [this](bool ok, const QString &msg, const QStringList &data) {
            QTimer::singleShot(0, this, [this, ok, msg, data]() {
                finish();
                emit importFinished(ok, msg, data);
            });
        });
}

void InstallBridge::installMcVersion(const QString &versionId) {
    if (!tryStart(QStringLiteral("正在下载游戏文件..."))) return;

    QtConcurrent::run([this, versionId]() {
        bool ok = mlc::installVersion(versionId,
            [this](const mlc::ImportProgress &p) {
                QTimer::singleShot(0, this, [this, step = p.step, percent = p.percent]() {
                    setProgress(step, percent);
                });
            });
        QTimer::singleShot(0, this, [this, ok]() {
            finish();
            emit mcInstallFinished(ok, ok ? QString() : QStringLiteral("游戏文件下载失败"));
        });
    });
}

void InstallBridge::installJava(int majorVersion) {
    if (!tryStart(QStringLiteral("正在下载 Java..."))) return;

    QtConcurrent::run([this, majorVersion]() {
        QString err;
        bool ok = mlc::installJavaRuntime(majorVersion, &err);
        QTimer::singleShot(0, this, [this, ok, err]() {
            finish();
            emit javaInstallFinished(ok, err);
        });
    });
}
