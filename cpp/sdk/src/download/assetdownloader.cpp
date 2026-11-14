#include "download/assetdownloader.h"
#include "download/downloadmanager.h"
#include "core/versionmanager.h"
#include "util/file_utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSharedPointer>

static Q_LOGGING_CATEGORY(logAsset, "mlc.asset")

// 安全读取字符串成员：键缺失或类型不符时返回空串，避免 .value()/.get<>()
// 类型不符抛 nlohmann 异常、穿过 Qt 信号槽导致 std::terminate（P0-3）
static std::string jsonStrOr(const json &j, const char *key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

AssetDownloader& AssetDownloader::instance() {
    static AssetDownloader m;
    return m;
}

// Full pipeline

void AssetDownloader::downloadVersion(const QString &versionId,
                                       std::function<void(bool, QString)> onComplete) {
    emit downloadLog("Starting download for version: " + versionId);
    emit downloadProgress("Fetching version manifest...", 0, 5);

    // Step 1: Get version manifest
    DownloadManager::instance().downloadJson(
        "https://launchermeta.mojang.com/mc/game/version_manifest.json",
        [this, versionId, onComplete](bool ok, QString err, json manifest) {
            if (!ok) {
                emit downloadLog("Failed: " + err);
                if (onComplete) onComplete(false, err);
                return;
            }

            // N3：manifest 必须是含 versions 数组的对象，否则按畸形数据报错
            if (!manifest.is_object() ||
                !manifest.contains("versions") || !manifest["versions"].is_array()) {
                QString msg = "Malformed version manifest";
                emit downloadLog(msg);
                if (onComplete) onComplete(false, msg);
                return;
            }

            QString versionUrl;
            for (const auto &v : manifest["versions"]) {
                if (jsonStrOr(v, "id") == versionId.toStdString()) {
                    versionUrl = QString::fromStdString(jsonStrOr(v, "url"));
                    break;
                }
            }
            if (versionUrl.isEmpty()) {
                QString msg = "Version not found: " + versionId;
                emit downloadLog(msg);
                if (onComplete) onComplete(false, msg);
                return;
            }

            // Step 2: Download version JSON
            emit downloadProgress("Downloading version JSON...", 1, 5);
            DownloadManager::instance().downloadJson(
                versionUrl, [this, versionId, onComplete](bool ok2, QString err2, json verJson) {
                    if (!ok2) {
                        emit downloadLog("Failed to download version JSON: " + err2);
                        if (onComplete) onComplete(false, err2);
                        return;
                    }

                    // 从 VersionManager 读取游戏目录，version 文件放入标准的 versions/ 子目录
                    QString mcRoot = VersionManager::instance().mcFolder();
                    if (mcRoot.isEmpty()) {
                        mcRoot = DownloadManager::instance().property("mcFolder").toString();
                    }
                    QString jsonPath = mcRoot + "versions/" + versionId + "/" + versionId + ".json";
                    QDir().mkpath(QFileInfo(jsonPath).absolutePath());
                    QFile f(jsonPath);
                    const QByteArray jsonBytes = QJsonDocument(QJsonDocument::fromJson(
                        QString::fromStdString(verJson.dump()).toUtf8())).toJson();
                    // N5/P1-9：落盘失败（含磁盘满写截断）必须立即报错，
                    // 否则后续步骤会报误导性的 "Version JSON not found"
                    if (!f.open(QIODevice::WriteOnly) || f.write(jsonBytes) != jsonBytes.size()) {
                        QString msg = "Cannot write version JSON: " + jsonPath;
                        emit downloadLog(msg);
                        if (onComplete) onComplete(false, msg);
                        return;
                    }
                    f.close();

                    // Build a McVersion for path references
                    McVersion ver;
                    ver.id = versionId;
                    ver.pathVersion = QFileInfo(jsonPath).absolutePath() + "/";  // {mcFolder}/versions/{id}/
                    ver.pathJar = ver.pathVersion + versionId + ".jar";
                    ver.pathIndie = mcRoot;  // {mcFolder}/  全局 libraries/assets 放这里

                    // Parse asset index
                    QString assetIndex;
                    if (verJson.contains("assetIndex") && verJson["assetIndex"].is_object()) {
                        assetIndex = QString::fromStdString(jsonStrOr(verJson["assetIndex"], "id"));
                    } else if (verJson.contains("assets") && verJson["assets"].is_string()) {
                        assetIndex = QString::fromStdString(verJson["assets"].get<std::string>());
                    }

                    // Step 3: Download client JAR
                    emit downloadProgress("Downloading client JAR...", 2, 5);
                    downloadClientJar(ver, [this, ver, verJson, assetIndex, onComplete]
                        (bool ok3, QString err3) {
                        if (!ok3) {
                            emit downloadLog("JAR download failed: " + err3);
                            if (onComplete) onComplete(false, err3);
                            return;
                        }

                        // Step 4: Download libraries
                        emit downloadProgress("Downloading libraries...", 3, 5);
                        downloadLibraries(ver, verJson, [this, ver, assetIndex, onComplete]
                            (bool ok4, QString err4) {
                            if (!ok4) {
                                emit downloadLog("Library download failed: " + err4);
                                if (onComplete) onComplete(false, err4);
                                return;
                            }

                            // Step 5: Download assets
                            emit downloadProgress("Downloading assets...", 4, 5);
                            downloadAssets(ver, [this, onComplete](bool ok5, QString err5) {
                                emit downloadProgress("Complete!", 5, 5);
                                if (onComplete) onComplete(ok5, ok5 ? QString() : err5);
                            });
                        });
                    });
                });
        });
}

// Version JSON

void AssetDownloader::downloadVersionJson(const QString &versionId,
                                            std::function<void(bool, QString)> onComplete) {
    downloadVersion(versionId, onComplete);
}

// Client JAR

void AssetDownloader::downloadClientJar(const McVersion &version,
                                          std::function<void(bool, QString)> onComplete) {
    // The client JAR URL is derived from the version JSON's "downloads" -> "client" -> "url"
    // We need the version JSON. If it's already local, use it to get the download URL.
    QString jsonPath = version.pathVersion + version.id + ".json";

    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        // Try direct Mojang download (without knowing the hash)
        QString fallbackUrl = QString(
            "https://launcher.mojang.com/v1/objects/%1/client.jar")
            .arg(version.id); // This isn't correct but serves as placeholder
        emit downloadLog("No version JSON found, cannot determine JAR download URL");
        if (onComplete) onComplete(false, "Version JSON not found");
        return;
    }

    QByteArray raw = f.readAll();
    f.close();
    json verJson = json::parse(raw.toStdString(), nullptr, false);

    if (verJson.is_discarded()) {
        if (onComplete) onComplete(false, "Invalid version JSON");
        return;
    }

    // Get download URL
    QString downloadUrl;
    QString expectedHash;

    if (verJson.contains("downloads") && verJson["downloads"].is_object()
        && verJson["downloads"].contains("client")
        && verJson["downloads"]["client"].is_object()) {
        const auto &client = verJson["downloads"]["client"];
        downloadUrl = QString::fromStdString(jsonStrOr(client, "url"));
        expectedHash = QString::fromStdString(jsonStrOr(client, "sha1"));
    }

    if (downloadUrl.isEmpty()) {
        emit downloadLog("No client download URL in version JSON");
        if (onComplete) onComplete(false, "No download URL");
        return;
    }

    QString savePath = version.pathJar;
    QDir().mkpath(QFileInfo(savePath).absolutePath());

    // If file exists and hash matches, skip
    if (QFileInfo::exists(savePath) && !expectedHash.isEmpty()) {
        if (FileUtils::verifySha1(savePath, expectedHash)) {
            emit downloadLog("Client JAR already up-to-date: " + version.id);
            if (onComplete) onComplete(true, savePath);
            return;
        }
    }

    emit downloadLog("Downloading client JAR: " + downloadUrl);

    DownloadManager::instance().download(
        downloadUrl, savePath,
        [this](qint64 recv, qint64 total) {
            emit downloadProgress("Client JAR", (int)recv, (int)total);
        },
        [this, savePath, expectedHash, onComplete](bool success, QString msg) {
            if (!success) {
                if (onComplete) onComplete(false, msg);
                return;
            }
            // Verify SHA1
            if (!expectedHash.isEmpty() && !FileUtils::verifySha1(savePath, expectedHash)) {
                QFile::remove(savePath);
                emit downloadLog("Client JAR SHA1 mismatch");
                if (onComplete) onComplete(false, "SHA1 verification failed");
                return;
            }
            emit downloadLog("Client JAR downloaded: " + savePath);
            if (onComplete) onComplete(true, savePath);
        });
}

// Libraries

// 判断库的 rules 是否允许在当前平台使用（libraries 与 natives 共用）
static bool rulesAllowOnThisPlatform(const json &lib) {
    if (!lib.contains("rules") || !lib["rules"].is_array()) return true;
    bool allowed = false;
    for (const auto &rule : lib["rules"]) {
        // 缺 action 按 "allow" 处理；类型不符时 jsonStrOr 返回空串同样放行
        std::string action = jsonStrOr(rule, "action");
        bool ruleAllows = (action.empty() || action == "allow");
        bool matches = true;
        if (rule.contains("os") && rule["os"].is_object()) {
            std::string osName = jsonStrOr(rule["os"], "name");
            std::string osArch = jsonStrOr(rule["os"], "arch");
            if (!osName.empty()) {
                switch (currentPlatform()) {
                case Platform::Windows: matches = (osName == "windows"); break;
                case Platform::Linux:   matches = (osName == "linux"); break;
                case Platform::MacOS:   matches = (osName == "osx"); break;
                default: break;
                }
            }
            if (!osArch.empty()) {
                matches = matches && (osArch == (is64BitSystem() ? "x86_64" : "x86"));
            }
        }
        if (matches) allowed = ruleAllows;
    }
    return allowed;
}

void AssetDownloader::downloadLibraries(const McVersion &version,
                                          const json &versionJson,
                                          std::function<void(bool, QString)> onComplete) {
    if (!versionJson.contains("libraries") || !versionJson["libraries"].is_array()) {
        emit downloadLog("No libraries to download");
        if (onComplete) onComplete(true, QString());
        return;
    }

    // Count downloadable libraries (those with artifact downloads, passing rules)
    // 第三项是期望 sha1：下载后必须校验（200-坏字节会静默产出损坏库 → 运行期崩溃）
    struct LibTask { QString url; QString savePath; QString sha1; };
    QList<LibTask> toDownload;

    for (const auto &lib : versionJson["libraries"]) {
        if (!lib.is_object()) continue;
        if (!rulesAllowOnThisPlatform(lib)) continue;

        if (!lib.contains("downloads") || !lib["downloads"].contains("artifact")) {
            // 无 artifact 且带 natives 节 = natives 容器（jinput-platform 等），
            // 由 downloadNatives 走 classifiers 处理，没有主 jar 可下
            if (lib.contains("natives")) continue;
            // 旧格式（≤1.13 的 JSON）：只有 maven name，按标准仓库推导 URL
            QString rel = FileUtils::mavenNameToPath(
                QString::fromStdString(jsonStrOr(lib, "name")));
            if (rel.isEmpty()) continue;
            // 部分旧库自带仓库地址，默认 libraries.minecraft.net
            QString base = QString::fromStdString(jsonStrOr(lib, "url"));
            if (base.isEmpty()) base = "https://libraries.minecraft.net/";
            if (!base.endsWith('/')) base += '/';
            QString savePath = version.pathIndie + "libraries/" + rel;
            if (QFileInfo::exists(savePath)) continue;  // 无 sha1 可校验，存在即跳过
            QDir().mkpath(QFileInfo(savePath).absolutePath());
            toDownload.append({base + rel, savePath, QString()});
            continue;
        }

        auto &artifact = lib["downloads"]["artifact"];
        std::string url = jsonStrOr(artifact, "url");
        std::string path = jsonStrOr(artifact, "path");
        std::string sha1 = jsonStrOr(artifact, "sha1");

        if (url.empty() || path.empty()) continue;

        QString savePath = version.pathIndie + "libraries/" + QString::fromStdString(path);
        // Skip if already exists and hash matches
        if (QFileInfo::exists(savePath) && !sha1.empty()) {
            if (FileUtils::verifySha1(savePath, QString::fromStdString(sha1))) continue;
        }

        QDir().mkpath(QFileInfo(savePath).absolutePath());
        toDownload.append({QString::fromStdString(url), savePath,
                           QString::fromStdString(sha1)});
    }

    if (toDownload.isEmpty()) {
        emit downloadLog("All libraries up-to-date");
        if (onComplete) onComplete(true, QString());
        return;
    }

    int total = toDownload.size();
    auto completed = QSharedPointer<int>::create(0);
    auto failed = QSharedPointer<int>::create(0);
    auto totalPtr = QSharedPointer<int>::create(total);

    emit downloadLog(QString("Downloading %1 libraries...").arg(total));
    emit downloadProgress("Libraries", 0, total);

    for (const auto &item : toDownload) {
        DownloadManager::instance().download(
            item.url, item.savePath,
            nullptr,
            [this, completed, failed, totalPtr, onComplete,
             savePath = item.savePath, expectedSha1 = item.sha1](bool success, QString) {
                // 下载成功也要校验 sha1：CDN 出错/代理篡改/截断都会产出"成功"的坏文件
                if (success && !expectedSha1.isEmpty()
                    && !FileUtils::verifySha1(savePath, expectedSha1)) {
                    emit downloadLog("Library SHA1 mismatch, removing: " + savePath);
                    QFile::remove(savePath);
                    success = false;
                }
                (*completed)++;
                emit downloadProgress("Libraries", *completed, *totalPtr);
                if (!success) (*failed)++;

                if (*completed >= *totalPtr) {
                    int f = *failed;
                    int t = *totalPtr;
                    if (f > 0) {
                        QString msg = QString("%1/%2 libraries failed").arg(f).arg(t);
                        emit downloadLog(msg);
                        if (onComplete) onComplete(false, msg);
                    } else {
                        emit downloadLog("All libraries downloaded");
                        if (onComplete) onComplete(true, QString());
                    }
                }
            });
    }
}

// Assets

void AssetDownloader::downloadAssets(const McVersion &version,
                                       std::function<void(bool, QString)> onComplete) {
    // First download the asset index JSON
    QString jsonPath = version.pathVersion + version.id + ".json";
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (onComplete) onComplete(false, "Cannot read version JSON");
        return;
    }

    json verJson = json::parse(f.readAll().toStdString(), nullptr, false);
    f.close();

    // P1-10：解析失败（语法错误）必须报错，不能当成功继续
    if (verJson.is_discarded()) {
        if (onComplete) onComplete(false, "Invalid version JSON");
        return;
    }

    QString assetIndexId;
    if (verJson.contains("assetIndex") && verJson["assetIndex"].is_object()) {
        assetIndexId = QString::fromStdString(jsonStrOr(verJson["assetIndex"], "id"));
    }
    if (assetIndexId.isEmpty() && verJson.contains("assets") && verJson["assets"].is_string()) {
        assetIndexId = QString::fromStdString(verJson["assets"].get<std::string>());
    }

    QString assetIndexUrl;
    if (verJson.contains("assetIndex") && verJson["assetIndex"].is_object()
        && verJson["assetIndex"].contains("url")
        && verJson["assetIndex"]["url"].is_string()) {
        assetIndexUrl = QString::fromStdString(verJson["assetIndex"]["url"].get<std::string>());
    }

    if (assetIndexId.isEmpty()) {
        emit downloadLog("No asset index found, using legacy assets");
        if (onComplete) onComplete(true, QString());
        return;
    }

    emit downloadLog("Downloading asset index: " + assetIndexId);

    auto doAssetDownload = [this, version, assetIndexId, onComplete](json assetIndexJson) {
        // 资产索引落盘：游戏启动需要 {mc}/assets/indexes/{id}.json
        QString idxPath = version.pathIndie + "assets/indexes/" + assetIndexId + ".json";
        QDir().mkpath(QFileInfo(idxPath).absolutePath());
        QFile idxFile(idxPath);
        const QByteArray idxBytes = QString::fromStdString(assetIndexJson.dump()).toUtf8();
        // P1-9：写失败/磁盘满截断必须报错走失败回调，不能静默继续
        if (!idxFile.open(QIODevice::WriteOnly) || idxFile.write(idxBytes) != idxBytes.size()) {
            QString msg = "Cannot write asset index: " + idxPath;
            emit downloadLog(msg);
            if (onComplete) onComplete(false, msg);
            return;
        }
        idxFile.close();

        if (!assetIndexJson.contains("objects") || !assetIndexJson["objects"].is_object()) {
            if (onComplete) onComplete(true, QString());
            return;
        }

        auto &objects = assetIndexJson["objects"];
        // 第三项是期望 sha1：下载后必须校验，否则损坏资源会被当成有效资源静默接受
        struct AssetTask { QString url; QString savePath; QString sha1; };
        QList<AssetTask> toDownload;

        for (auto it = objects.begin(); it != objects.end(); ++it) {
            std::string hash = jsonStrOr(it.value(), "hash");
            if (hash.empty()) continue;

            QString hashStr = QString::fromStdString(hash);
            QString subPath = FileUtils::assetPathFromHash(hashStr);
            QString savePath = version.pathIndie + "assets/objects/" + subPath;

            if (QFileInfo::exists(savePath) && FileUtils::verifySha1(savePath, hashStr))
                continue;

            QString url = "https://resources.download.minecraft.net/" + subPath;
            QDir().mkpath(QFileInfo(savePath).absolutePath());
            toDownload.append({url, savePath, hashStr});
        }

        if (toDownload.isEmpty()) {
            emit downloadLog("All assets up-to-date");
            if (onComplete) onComplete(true, QString());
            return;
        }

        int total = toDownload.size();
        auto completed = QSharedPointer<int>::create(0);
        auto failed = QSharedPointer<int>::create(0);

        emit downloadLog(QString("Downloading %1 assets...").arg(total));
        emit downloadProgress("Assets", 0, total);

        for (const auto &item : toDownload) {
            DownloadManager::instance().download(
                item.url, item.savePath,
                nullptr,
                [this, completed, failed, total, onComplete,
                 savePath = item.savePath, expectedSha1 = item.sha1](bool success, QString) {
                    // 内容损坏的资源既非"成功"也非网络失败——必须删掉并计入 failed，
                    // 否则坏文件留在 assets/objects/ 里导致贴图/音效随机出错
                    if (success && !FileUtils::verifySha1(savePath, expectedSha1)) {
                        emit downloadLog("Asset SHA1 mismatch, removing: " + savePath);
                        QFile::remove(savePath);
                        success = false;
                    }
                    (*completed)++;
                    if (!success) (*failed)++;
                    if (*completed % 50 == 0 || *completed >= total) {
                        emit downloadProgress("Assets", *completed, total);
                    }
                    if (*completed >= total) {
                        int f = *failed;
                        if (f > 0) {
                            emit downloadLog(QString("%1/%2 assets failed").arg(f).arg(total));
                        } else {
                            emit downloadLog("All assets downloaded");
                        }
                        // 部分资产失败不致命（缺贴图/声音游戏仍可运行），全部失败才算失败
                        if (onComplete) onComplete(f < total, f >= total
                            ? QString("All %1 assets failed").arg(total) : QString());
                    }
                });
        }
    };

    // Download asset index JSON — 所有路径都必须触发 onComplete
    auto handleIndex = [this, doAssetDownload, onComplete](bool ok, QString err, json idxJson) {
        if (!ok) {
            emit downloadLog("Failed to download asset index: " + err);
            if (onComplete) onComplete(false, "Failed to download asset index: " + err);
            return;
        }
        doAssetDownload(idxJson);
    };

    if (!assetIndexUrl.isEmpty()) {
        DownloadManager::instance().downloadJson(assetIndexUrl, handleIndex);
    } else {
        // Fallback: construct URL from asset index ID
        QString fallbackUrl = QString("https://launchermeta.mojang.com/v1/packages/%1.json")
                                  .arg(assetIndexId);
        DownloadManager::instance().downloadJson(fallbackUrl, handleIndex);
    }
}

// Natives

void AssetDownloader::downloadNatives(const McVersion &version,
                                        std::function<void(bool, QString)> onComplete) {
    QString jsonPath = version.pathVersion + version.id + ".json";
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (onComplete) onComplete(false, "Cannot read version JSON");
        return;
    }

    json verJson = json::parse(f.readAll().toStdString(), nullptr, false);
    f.close();

    // P1-10：解析失败（语法错误）必须报错，不能当成功继续
    if (verJson.is_discarded()) {
        if (onComplete) onComplete(false, "Invalid version JSON");
        return;
    }

    if (!verJson.contains("libraries") || !verJson["libraries"].is_array()) {
        if (onComplete) onComplete(true, QString());
        return;
    }

    // Find native libraries matching current platform
    QList<std::pair<QString, QString>> nativesToDownload; // <url, savePath>
    QStringList allNativeJars;  // 所有本机平台 native jar（含已缓存的，解压用）

    QString nativeClassifier;
    QString osName;
    switch (currentPlatform()) {
    case Platform::Windows: nativeClassifier = "natives-windows"; osName = "windows"; break;
    case Platform::Linux:   nativeClassifier = "natives-linux";   osName = "linux"; break;
    case Platform::MacOS:   nativeClassifier = "natives-macos";   osName = "osx"; break;
    default: break;
    }
    // 新格式（1.19+）：natives 是独立库条目，name 带 ":natives-<os>" 后缀
    const std::string nativesSuffix = ":natives-" + osName.toStdString();

    for (const auto &lib : verJson["libraries"]) {
        if (!lib.is_object()) continue;
        if (!rulesAllowOnThisPlatform(lib)) continue;

        std::string libName = jsonStrOr(lib, "name");

        // 新格式：jar 已由 downloadLibraries 按普通 artifact 下载，此处只收集路径待解压
        if (libName.size() > nativesSuffix.size() && libName.ends_with(nativesSuffix)) {
            if (lib.contains("downloads") && lib["downloads"].contains("artifact")) {
                std::string p = jsonStrOr(lib["downloads"]["artifact"], "path");
                if (!p.empty())
                    allNativeJars.append(version.pathIndie + "libraries/" + QString::fromStdString(p));
            }
            continue;
        }

        // 旧格式（≤1.18）：downloads.classifiers 里的 natives-<os> 条目
        // downloads 键缺失时 const operator[] 是 UB，必须先 contains 判定
        if (!lib.contains("downloads")) continue;

        auto &downloads = lib["downloads"];
        if (!downloads.contains("classifiers")) continue;

        auto &classifiers = downloads["classifiers"];
        std::string classifierKey = nativeClassifier.toStdString();
        if (currentPlatform() == Platform::Windows && is64BitSystem())
            classifierKey = "natives-windows-64";
        if (currentPlatform() == Platform::Linux && is64BitSystem())
            classifierKey = "natives-linux-64";

        if (!classifiers.contains(classifierKey)) {
            // Try without arch suffix
            if (!classifiers.contains(nativeClassifier.toStdString())) continue;
            classifierKey = nativeClassifier.toStdString();
        }

        auto &native = classifiers[classifierKey];
        std::string url = jsonStrOr(native, "url");
        // 必须用 classifier 产物的 maven 路径（形如 org/lwjgl/.../x-natives-linux.jar），
        // 不能用 lib["name"]（maven 坐标，不是文件路径）
        std::string path = jsonStrOr(native, "path");

        if (url.empty() || path.empty()) continue;

        QString savePath = version.pathIndie + "libraries/" + QString::fromStdString(path);
        allNativeJars.append(savePath);
        QDir().mkpath(QFileInfo(savePath).absolutePath());

        // Check if the native JAR already exists
        if (QFileInfo::exists(savePath)) {
            // Check SHA1 if available
            std::string sha1 = jsonStrOr(native, "sha1");
            if (!sha1.empty() && FileUtils::verifySha1(savePath, QString::fromStdString(sha1)))
                continue;
        }

        nativesToDownload.append({QString::fromStdString(url), savePath});
    }

    QString nativesDir = version.pathVersion + "natives/";

    // 解压全部本机平台 native jar（含已缓存的——jar 在 libraries/ 但 natives/ 可能从未解压）
    auto extractAll = [this, allNativeJars, nativesDir]() {
        if (allNativeJars.isEmpty()) return;
        emit downloadLog("Extracting native libraries...");
        for (const QString &jarPath : allNativeJars) {
            if (!QFileInfo::exists(jarPath)) continue;
            QStringList extracted = FileUtils::extractNativesJar(jarPath, nativesDir);
            for (const QString &f : extracted) {
                emit downloadLog("  Extracted: " + QFileInfo(f).fileName());
            }
        }
        emit downloadLog("Native libraries extracted to: " + nativesDir);
    };

    if (nativesToDownload.isEmpty()) {
        emit downloadLog("All native libraries up-to-date");
        extractAll();
        if (onComplete) onComplete(true, QString());
        return;
    }

    int total = nativesToDownload.size();
    auto completed = QSharedPointer<int>::create(0);
    auto failed = QSharedPointer<int>::create(0);

    emit downloadLog(QString("Downloading %1 native libraries...").arg(total));
    emit downloadProgress("Natives", 0, total);

    for (const auto &item : nativesToDownload) {
        DownloadManager::instance().download(
            item.first, item.second,
            nullptr,
            [this, completed, failed, total, extractAll, onComplete](bool success, QString) {
                (*completed)++;
                emit downloadProgress("Natives", *completed, total);
                if (!success) (*failed)++;

                if (*completed >= total) {
                    extractAll();
                    int f = *failed;
                    if (onComplete) onComplete(f == 0, f > 0 ? QString("%1/%2 failed").arg(f).arg(total) : QString());
                }
            });
    }
}

