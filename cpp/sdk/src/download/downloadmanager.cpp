#include "download/downloadmanager.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <nlohmann/json.hpp>

static Q_LOGGING_CATEGORY(logDl, "mlc.download")

DownloadManager& DownloadManager::instance() {
    static DownloadManager m;
    return m;
}

DownloadManager::DownloadManager() {
    m_nam = new QNetworkAccessManager(this);
}

// ---- Simple download ----

QNetworkReply* DownloadManager::download(const QString &url, const QString &savePath,
                                          int maxRetries) {
    return download(url, savePath, nullptr, nullptr, maxRetries);
}

QNetworkReply* DownloadManager::download(const QString &url, const QString &savePath,
                                          ProgressCallback onProgress,
                                          CompletionCallback onComplete,
                                          int maxRetries) {
    // 全局限流：在传满 kMaxInFlight 时进自有队列（此刻还没有 socket，谈不上挂死），
    // 超时计时从真实派发起算——从根上消除 QNAM 内部排队造成的假超时
    if (m_inFlight >= kMaxInFlight) {
        m_pending.enqueue({url, savePath, onProgress, onComplete, maxRetries});
        return nullptr;  // 排队中，暂无 reply
    }
    m_inFlight++;
    auto tracked = [this, onComplete](bool ok, QString msg) {
        m_inFlight--;
        if (onComplete) onComplete(ok, msg);
        dispatchPending();
    };
    return downloadInternal(url, savePath, onProgress, tracked, maxRetries);
}

void DownloadManager::dispatchPending() {
    while (m_inFlight < kMaxInFlight && !m_pending.isEmpty()) {
        PendingDownload p = m_pending.dequeue();
        m_inFlight++;
        auto tracked = [this, cb = p.onComplete](bool ok, QString msg) {
            m_inFlight--;
            if (cb) cb(ok, msg);
            dispatchPending();
        };
        downloadInternal(p.url, p.savePath, p.onProgress, tracked, p.retries);
    }
}

QNetworkReply* DownloadManager::downloadInternal(const QString &url,
                                                  const QString &savePath,
                                                  ProgressCallback onProgress,
                                                  CompletionCallback onComplete,
                                                  int retriesRemaining) {
    emit downloadStarted(url);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "MLC/0.1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply *reply = m_nam->get(request);

    // 两阶段超时：首字节 60s（连接/响应头阶段兜底，服务器装死最多挂这么久就被 abort 进重试链）；
    // 首字节后每次传输活动重置为 30s 停滞计时。不能用 setTransferTimeout——它从请求创建即计时
    QTimer *stallTimer = new QTimer(reply);
    stallTimer->setSingleShot(true);
    // stallAbort=true 表示本次 abort 由超时器发起（应重试）；
    // 否则是调用方主动 abort（取消，不应重试也不应报"下载失败"）
    auto stallAbort = QSharedPointer<bool>::create(false);
    connect(stallTimer, &QTimer::timeout, reply, [reply, url, stallAbort]() {
        qCWarning(logDl) << "Transfer stalled, aborting:" << url;
        *stallAbort = true;
        reply->abort();
    });
    auto restartStallTimer = [stallTimer]() { stallTimer->start(30000); };
    connect(reply, &QNetworkReply::readyRead, this, restartStallTimer);
    connect(reply, &QNetworkReply::metaDataChanged, this, restartStallTimer);
    stallTimer->start(60000);

    // Progress
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, url, onProgress, restartStallTimer](qint64 received, qint64 total) {
                restartStallTimer();
                emit downloadProgress(url, received, total);
                if (onProgress) onProgress(received, total);
            });

    // Completion
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        stallTimer->stop();

        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool success = (reply->error() == QNetworkReply::NoError);

        // 用户主动取消：不是网络错误，直接按终态上报，绝不重试
        // （QNetworkReply::abort() 产生 OperationCanceledError）
        if (!success && reply->error() == QNetworkReply::OperationCanceledError
            && !*stallAbort) {
            reply->deleteLater();
            qCInfo(logDl) << "Download canceled by caller:" << url;
            emit downloadFinished(url, false, QStringLiteral("canceled"));
            if (onComplete) onComplete(false, QStringLiteral("canceled"));
            return;
        }
        reply->deleteLater();

        // Handle HTTP errors
        if (statusCode >= 400) success = false;

        // Retry on failure
        if (!success && retriesRemaining > 0) {
            qCWarning(logDl) << "Download failed, retrying:" << url
                             << "Error:" << reply->errorString()
                             << "Retries left:" << retriesRemaining - 1;
            // Delay retry slightly
            QTimer::singleShot(500, this, [this, url, savePath, onProgress, onComplete, retriesRemaining]() {
                downloadInternal(url, savePath, onProgress, onComplete, retriesRemaining - 1);
            });
            // 重试期间不发 downloadFinished——任务尚未终结，误报失败会误导上层
            return;
        }

        if (!success) {
            // CF 文件 CDN（edge.forgecdn.net）拒绝访问时，回退 MCIM 代理
            // （同 PCL-CE DlSourceModDownloadGet 方案，路径不变只换主机）
            if (QUrl(url).host() == "edge.forgecdn.net") {
                QString fallback = url;
                fallback.replace("edge.forgecdn.net", "mod.mcimirror.top");
                qCInfo(logDl) << "forgecdn failed, fallback to MCIM mirror:" << fallback;
                downloadInternal(fallback, savePath, onProgress, onComplete, 1);
                return;
            }
            QString err = reply->errorString();
            qCWarning(logDl) << "Download failed:" << url << err;
            emit downloadFinished(url, false, err);
            if (onComplete) onComplete(false, err);
            return;
        }

        // Save to file
        QByteArray data = reply->readAll();
        QDir().mkpath(QFileInfo(savePath).absolutePath());

        QFile file(savePath);
        if (!file.open(QIODevice::WriteOnly)) {
            QString err = "Cannot write to: " + savePath;
            qCWarning(logDl) << err;
            emit downloadFinished(url, false, err);
            if (onComplete) onComplete(false, err);
            return;
        }
        qint64 written = file.write(data);
        file.close();
        if (written != data.size()) {
            // 写入不完整（如磁盘满）：删掉半成品文件，避免留下损坏产物被误用
            file.remove();
            QString err = "Write failed: " + savePath + " (" + file.errorString() + ")";
            qCWarning(logDl) << "Download failed:" << url << err;
            emit downloadFinished(url, false, err);
            if (onComplete) onComplete(false, err);
            return;
        }
        qCInfo(logDl) << "Downloaded:" << url << "->" << savePath
                      << "(" << data.size() << "bytes)";
        emit downloadFinished(url, true, savePath);
        if (onComplete) onComplete(true, savePath);
    });

    return reply;
}

// ---- Memory download ----

QNetworkReply* DownloadManager::downloadToString(const QString &url,
                                                   std::function<void(bool, QString)> onComplete,
                                                   int maxRetries) {
    return downloadToStringWithHeaders(url, {}, onComplete, maxRetries);
}

QNetworkReply* DownloadManager::downloadJson(const QString &url,
                                               std::function<void(bool, QString, nlohmann::json)> onComplete,
                                               int maxRetries) {
    return downloadJsonWithHeaders(url, {}, onComplete, maxRetries);
}

// ---- Memory download with custom headers ----

QNetworkReply* DownloadManager::downloadToStringWithHeaders(const QString &url,
                                                              const QMap<QByteArray, QByteArray> &headers,
                                                              std::function<void(bool, QString)> onComplete,
                                                              int maxRetries) {
    return downloadToStringWithStatus(url, headers,
        [onComplete](bool success, int, QString data) {
            if (onComplete) onComplete(success, data);
        }, maxRetries);
}

QNetworkReply* DownloadManager::downloadToStringWithStatus(const QString &url,
                                                           const QMap<QByteArray, QByteArray> &headers,
                                                           std::function<void(bool, int, QString)> onComplete,
                                                           int maxRetries) {
    emit downloadStarted(url);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "MLC/0.1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }

    QNetworkReply *reply = m_nam->get(request);

    // 两阶段超时（同 downloadInternal）：首字节 30s（连接/响应头阶段兜底），
    // 首字节后每次传输活动重置 30s 停滞计时
    QTimer *stallTimer = new QTimer(reply);
    stallTimer->setSingleShot(true);
    connect(stallTimer, &QTimer::timeout, reply, [reply, url]() {
        qCWarning(logDl) << "Transfer stalled, aborting:" << url;
        reply->abort();
    });
    auto restartStallTimer = [stallTimer]() { stallTimer->start(30000); };
    connect(reply, &QNetworkReply::readyRead, this, restartStallTimer);
    connect(reply, &QNetworkReply::metaDataChanged, this, restartStallTimer);
    stallTimer->start(30000);

    // Capture headers by value for retry recursion.
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        stallTimer->stop();
        reply->deleteLater();
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool success = (reply->error() == QNetworkReply::NoError && statusCode < 400);

        if (!success && maxRetries > 0) {
            QTimer::singleShot(500, this, [=, this]() {
                downloadToStringWithStatus(url, headers, onComplete, maxRetries - 1);
            });
            return;
        }

        if (!success) {
            if (onComplete) onComplete(false, statusCode, reply->errorString());
            return;
        }

        QByteArray data = reply->readAll();
        if (onComplete) onComplete(true, statusCode, QString::fromUtf8(data));
    });

    return reply;
}

QNetworkReply* DownloadManager::downloadJsonWithHeaders(const QString &url,
                                                          const QMap<QByteArray, QByteArray> &headers,
                                                          std::function<void(bool, QString, nlohmann::json)> onComplete,
                                                          int maxRetries) {
    return downloadToStringWithHeaders(url, headers,
        [onComplete](bool success, QString data) {
            if (!success) {
                if (onComplete) onComplete(false, data, nlohmann::json());
                return;
            }
            try {
                nlohmann::json j = nlohmann::json::parse(data.toStdString());
                if (onComplete) onComplete(true, data, j);
            } catch (const std::exception &e) {
                if (onComplete) onComplete(false, QString("JSON parse error: ") + e.what(), nlohmann::json());
            }
        }, maxRetries);
}

QNetworkReply* DownloadManager::downloadJsonWithStatus(const QString &url,
                                                          const QMap<QByteArray, QByteArray> &headers,
                                                          std::function<void(bool, int, QString, nlohmann::json)> onComplete,
                                                          int maxRetries) {
    return downloadToStringWithStatus(url, headers,
        [onComplete](bool success, int statusCode, QString data) {
            if (!success) {
                if (onComplete) onComplete(false, statusCode, data, nlohmann::json());
                return;
            }
            try {
                nlohmann::json j = nlohmann::json::parse(data.toStdString());
                if (onComplete) onComplete(true, statusCode, data, j);
            } catch (const std::exception &e) {
                if (onComplete) onComplete(false, statusCode, QString("JSON parse error: ") + e.what(), nlohmann::json());
            }
        }, maxRetries);
}

// ---- URL helpers ----

QString DownloadManager::versionManifestUrl() {
    return "https://launchermeta.mojang.com/mc/game/version_manifest.json";
}

QString DownloadManager::versionJsonUrl(const QString &versionId) {
    // 版本 JSON 的真实地址来自 version_manifest 里该条目的 url 字段（路径是
    // JSON 内容的 sha1，无法由版本号推出）。这里按“先查清单”给出可工作的入口，
    // 不再构造必然 404 的伪路径。
    Q_UNUSED(versionId)
    return versionManifestUrl();
}

QString DownloadManager::assetsIndexUrl(const QString &assetsVersion) {
    // 同理：资源索引地址也来自版本 JSON 的 assetIndex.url（按内容哈希），
    // 不能用资源索引 id 拼。id 为空时返回空串，调用方应改读版本 JSON。
    Q_UNUSED(assetsVersion)
    return {};
}
