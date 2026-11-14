#ifndef MLC_DOWNLOADMANAGER_H
#define MLC_DOWNLOADMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMap>
#include <QQueue>
#include <functional>
#include <nlohmann/json.hpp>
#include "core/types.h"
#include "core/mlccore_export.h"

/**
 * HTTP download manager with retry logic and progress tracking.
 * Mirrors the original ModNet + ModDownload functionality.
 */
class MLCCORE_EXPORT DownloadManager : public QObject
{
    Q_OBJECT

public:
    static DownloadManager& instance();

    /// Progress callback: (bytesReceived, bytesTotal)
    using ProgressCallback = std::function<void(qint64, qint64)>;
    /// Completion callback: (success, localFilePath or error)
    using CompletionCallback = std::function<void(bool, QString)>;

    // ---- Simple download ----

    /// Download a file to a local path
    QNetworkReply* download(const QString &url, const QString &savePath,
                            int maxRetries = 3);

    /// Download with callbacks
    QNetworkReply* download(const QString &url, const QString &savePath,
                            ProgressCallback onProgress,
                            CompletionCallback onComplete,
                            int maxRetries = 3);

    /// Download to memory (small files like version JSON)
    QNetworkReply* downloadToString(const QString &url,
                                     std::function<void(bool, QString)> onComplete,
                                     int maxRetries = 3);

    /// Download JSON to memory with custom headers
    QNetworkReply* downloadJsonWithHeaders(const QString &url,
                                            const QMap<QByteArray, QByteArray> &headers,
                                            std::function<void(bool, QString, nlohmann::json)> onComplete,
                                            int maxRetries = 3);

    /// Download to memory with custom headers
    QNetworkReply* downloadToStringWithHeaders(const QString &url,
                                                const QMap<QByteArray, QByteArray> &headers,
                                                std::function<void(bool, QString)> onComplete,
                                                int maxRetries = 3);

    /// Download to memory with custom headers, reporting the HTTP status code.
    /// onComplete(success, httpStatusCode, data or errorString)——鉴权类失败（401/403/429）
    /// 调用方可据此做降级（如 CF key 失效回退镜像），无 HTTP 响应时 statusCode 为 0
    QNetworkReply* downloadToStringWithStatus(const QString &url,
                                               const QMap<QByteArray, QByteArray> &headers,
                                               std::function<void(bool, int, QString)> onComplete,
                                               int maxRetries = 3);

    /// Download and parse JSON with custom headers, reporting the HTTP status code
    QNetworkReply* downloadJsonWithStatus(const QString &url,
                                           const QMap<QByteArray, QByteArray> &headers,
                                           std::function<void(bool, int, QString, nlohmann::json)> onComplete,
                                           int maxRetries = 3);

    // ---- JSON download helpers ----

    /// Download and parse JSON
    QNetworkReply* downloadJson(const QString &url,
                                 std::function<void(bool, QString, nlohmann::json)> onComplete,
                                 int maxRetries = 3);

    /// Get the version manifest URL
    static QString versionManifestUrl();

    /// Get a version JSON URL
    static QString versionJsonUrl(const QString &versionId);

    /// Get assets index URL
    static QString assetsIndexUrl(const QString &assetsVersion);

signals:
    void downloadStarted(const QString &url);
    void downloadProgress(const QString &url, qint64 received, qint64 total);
    void downloadFinished(const QString &url, bool success, const QString &msg);

private:
    DownloadManager();
    QNetworkReply* downloadInternal(const QString &url, const QString &savePath,
                                     ProgressCallback onProgress,
                                     CompletionCallback onComplete,
                                     int retriesRemaining);

    // 全局限流（见 download()）：同时在传数量有限，其余进自有队列
    struct PendingDownload {
        QString url, savePath;
        ProgressCallback onProgress;
        CompletionCallback onComplete;
        int retries;
    };
    static constexpr int kMaxInFlight = 16;
    void dispatchPending();
    int m_inFlight = 0;
    QQueue<PendingDownload> m_pending;

    QNetworkAccessManager *m_nam = nullptr;
};

#endif // MLC_DOWNLOADMANAGER_H
