#include "core/installer.h"
#include "download/downloadmanager.h"
#include "util/platform_utils.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static Q_LOGGING_CATEGORY(logInstall, "mlc.installer")

const QString Installer::FORGE_API = "https://files.minecraftforge.net/net/minecraftforge/forge";
const QString Installer::FABRIC_API = "https://meta.fabricmc.net/v2";
const QString Installer::NEOFORGE_API = "https://maven.neoforged.net/releases/net/neoforged/neoforge";

Installer& Installer::instance() {
    static Installer i;
    return i;
}

// URL helpers

QString Installer::getInstallerUrl(const QString &loaderType, const QString &mcVersion) {
    if (loaderType == "forge") {
        QString forgeVer = m_forgeCache.value(mcVersion);
        if (forgeVer.isEmpty()) return {};
        return QString("https://maven.minecraftforge.net/net/minecraftforge/forge/%1-%2/forge-%1-%2-installer.jar")
            .arg(mcVersion, forgeVer);
    }
    if (loaderType == "fabric") {
        return FABRIC_API + "/versions/loader/" + mcVersion;
    }
    if (loaderType == "neoforge") {
        QString neoVer = m_neoCache.value(mcVersion);
        if (neoVer.isEmpty()) return {};
        return NEOFORGE_API + "/" + neoVer + "/neoforge-" + neoVer + "-installer.jar";
    }
    return {};
}

void Installer::downloadInstaller(const QString &loaderType, const QString &mcVersion,
                                    const QString &loaderVersion,
                                    std::function<void(bool, QString)> onComplete) {
    QString jarName;
    QString url;

    if (loaderType == "forge") {
        jarName = QString("forge-%1-%2-installer.jar").arg(mcVersion, loaderVersion);
        url = QString("https://maven.minecraftforge.net/net/minecraftforge/forge/%1-%2/%3")
                  .arg(mcVersion, loaderVersion, jarName);
    } else if (loaderType == "fabric") {
        // Fabric 官方安装器地址需从 meta API 解析
        // （/server/jar 端点返回的是服务端启动器，不能用于客户端安装）
        DownloadManager::instance().downloadJson(FABRIC_API + "/versions/installer",
            [this, onComplete](bool ok, QString err, json list) {
                if (!ok) {
                    if (onComplete) onComplete(false, "Failed to resolve Fabric installer: " + err);
                    return;
                }
                QString installerUrl;
                if (list.is_array()) {
                    for (const auto &e : list) {
                        if (e.value("stable", false)) {
                            installerUrl = QString::fromStdString(e.value("url", ""));
                            break;
                        }
                    }
                    if (installerUrl.isEmpty() && !list.empty())
                        installerUrl = QString::fromStdString(list[0].value("url", ""));
                }
                if (installerUrl.isEmpty()) {
                    if (onComplete) onComplete(false, "No Fabric installer URL found");
                    return;
                }
                QString savePath = QDir::temp().filePath("fabric-installer.jar");
                emit installLog("Downloading fabric-installer.jar...");
                DownloadManager::instance().download(installerUrl, savePath,
                    nullptr, [onComplete, savePath](bool ok2, QString err2) {
                        if (onComplete) onComplete(ok2, ok2 ? savePath : err2);
                    });
            });
        return;
    } else if (loaderType == "neoforge") {
        jarName = QString("neoforge-%1-installer.jar").arg(loaderVersion);
        url = NEOFORGE_API + "/" + loaderVersion + "/" + jarName;
    } else {
        if (onComplete) onComplete(false, "Unknown loader: " + loaderType);
        return;
    }

    QString savePath = QDir::temp().filePath(jarName);
    emit installLog("Downloading " + jarName + "...");
    DownloadManager::instance().download(url, savePath,
        nullptr, [onComplete, savePath](bool ok, QString err) {
            if (onComplete) onComplete(ok, ok ? savePath : err);
        });
}

// Installer runner

void Installer::runInstallerJar(const QString &jarPath, const QString &javaPath,
                                  const QStringList &args,
                                  std::function<void(bool, QString)> onComplete) {
    m_isRunning = true;
    emit runningChanged();

    QProcess *proc = new QProcess(this);
    proc->setProgram(javaPath);
    proc->setArguments(args);
    // 安装器（尤其 Forge）会在工作目录写下 <jar>-installer.jar.log 等文件。
    // 不设工作目录时会落到启动器的 CWD（仓库根/构建目录），污染工作区。
    // 统一指到系统临时目录（jar 本身就下载在 QDir::temp()）。
    proc->setWorkingDirectory(QDir::tempPath());

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, onComplete](int exitCode, QProcess::ExitStatus) {
        proc->deleteLater();
        m_isRunning = false;
        emit runningChanged();

        if (exitCode == 0) {
            m_statusText = "Installation complete";
            emit statusTextChanged();
            if (onComplete) onComplete(true, QString());
        } else {
            QString err = QString::fromUtf8(proc->readAllStandardError());
            qCWarning(logInstall) << "Installer failed:" << err;
            m_statusText = "Installation failed";
            emit statusTextChanged();
            if (onComplete) onComplete(false, err);
        }
    });

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, onComplete](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;  // 崩溃等其他错误由 finished 处理
        proc->deleteLater();
        m_isRunning = false;
        emit runningChanged();
        QString err = "Cannot start installer process: " + proc->errorString();
        qCWarning(logInstall) << err;
        m_statusText = "Installation failed";
        emit statusTextChanged();
        if (onComplete) onComplete(false, err);
    });

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        QString line = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        if (!line.isEmpty()) emit installLog(line);
    });

    // Log the command
    QString cmd = javaPath;
    for (const auto &a : args) cmd += " " + a;
    emit installLog("Running: " + cmd);
    qCInfo(logInstall) << "Running installer:" << cmd;

    proc->start();
    m_statusText = "Installing...";
    emit statusTextChanged();
    emit installProgress("Installing...", 50);
}

// Forge

void Installer::installForge(const QString &mcDir, const QString &mcVersion,
                               const QString &forgeVersion, const QString &javaPath,
                               std::function<void(bool, QString)> onComplete) {
    downloadInstaller("forge", mcVersion, forgeVersion,
        [this, mcDir, javaPath, onComplete](bool ok, QString jarPath) {
            if (!ok) { if (onComplete) onComplete(false, jarPath); return; }

            runInstallerJar(jarPath, javaPath, {"-jar", jarPath, "--installClient", mcDir},
                            onComplete);
        });
}

void Installer::installFabric(const QString &mcDir, const QString &mcVersion,
                                const QString &loaderVersion, const QString &javaPath,
                                std::function<void(bool, QString)> onComplete) {
    downloadInstaller("fabric", mcVersion, loaderVersion,
        [this, mcDir, mcVersion, loaderVersion, javaPath, onComplete](bool ok, QString jarPath) {
            if (!ok) { if (onComplete) onComplete(false, jarPath); return; }
            QStringList args;
            args << "-jar" << jarPath << "client" << "-dir" << mcDir
                 << "-mcversion" << mcVersion << "-loader" << loaderVersion;
            runInstallerJar(jarPath, javaPath, args, onComplete);
        });
}

// 服务端模式：Forge/NeoForge 用 --installServer，Fabric 用 server 子命令 + 下载原版服务端
void Installer::installLoaderServer(const QString &loaderType, const QString &dir,
                                      const QString &mcVersion, const QString &loaderVersion,
                                      const QString &javaPath,
                                      std::function<void(bool, QString)> onComplete) {
    downloadInstaller(loaderType, mcVersion, loaderVersion,
        [this, loaderType, dir, mcVersion, loaderVersion, javaPath, onComplete](bool ok, QString jarPath) {
            if (!ok) { if (onComplete) onComplete(false, jarPath); return; }
            QStringList args;
            if (loaderType == "fabric") {
                args << "-jar" << jarPath << "server" << "-dir" << dir
                     << "-mcversion" << mcVersion << "-loader" << loaderVersion
                     << "-downloadMinecraft";
            } else {
                args << "-jar" << jarPath << "--installServer" << dir;
            }
            runInstallerJar(jarPath, javaPath, args, onComplete);
        });
}

void Installer::installNeoForge(const QString &mcDir, const QString &mcVersion,
                                  const QString &neoVersion, const QString &javaPath,
                                  std::function<void(bool, QString)> onComplete) {
    downloadInstaller("neoforge", mcVersion, neoVersion,
        [this, mcDir, javaPath, onComplete](bool ok, QString jarPath) {
            if (!ok) { if (onComplete) onComplete(false, jarPath); return; }
            runInstallerJar(jarPath, javaPath, {"-jar", jarPath, "--installClient", mcDir},
                            onComplete);
        });
}

void Installer::installLoader(const QString &loaderType, const QString &mcDir,
                                const QString &mcVersion,
                                const QString &loaderVersion, const QString &javaPath,
                                std::function<void(bool, QString)> onComplete) {
    // Forge/NeoForge 安装器要求游戏根目录存在 launcher_profiles.json（原版启动器痕迹），
    // 否则报 "no minecraft launcher profile" 拒绝安装——缺失时补一个最小文件
    if (loaderType == "forge" || loaderType == "neoforge") {
        QString profilesPath = QDir(mcDir).filePath("launcher_profiles.json");
        if (!QFile::exists(profilesPath)) {
            QFile f(profilesPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("{\"profiles\":{}}");
                f.close();
            }
        }
    }

    if (loaderType == "forge") installForge(mcDir, mcVersion, loaderVersion, javaPath, onComplete);
    else if (loaderType == "fabric") installFabric(mcDir, mcVersion, loaderVersion, javaPath, onComplete);
    else if (loaderType == "neoforge") installNeoForge(mcDir, mcVersion, loaderVersion, javaPath, onComplete);
    else if (onComplete) onComplete(false, "Unsupported loader: " + loaderType);
}

// Version detection — fetch available versions from APIs

void Installer::fetchForgeVersions(std::function<void(bool, QStringList)> onComplete) {
    // Forge 的 maven 站已不提供 index.html（Reposilite 返回 404），旧的 HTML 解析
    // 必然失败、永远拿不到版本列表。改用官方仍在维护的 promotions_slim.json：
    //   { "promos": { "1.20.1-latest": "47.4.10", "1.20.1-recommended": "47.3.0", ... } }
    QString url = FORGE_API + "/promotions_slim.json";
    DownloadManager::instance().downloadJson(url, [onComplete](bool ok, QString, json doc) {
        QStringList versions;
        if (!ok || !doc.is_object() || !doc.contains("promos") || !doc["promos"].is_object()) {
            onComplete(false, versions);
            return;
        }
        // key 形如 "<mc>-latest" / "<mc>-recommended"，value 即 Forge 版本号。
        // 统一转成 "<mc>-<forge>"，与 installForge 拼 URL 的约定一致。
        for (auto it = doc["promos"].begin(); it != doc["promos"].end(); ++it) {
            if (!it.value().is_string()) continue;
            const QString key = QString::fromStdString(it.key());
            const int dash = key.lastIndexOf('-');
            if (dash <= 0) continue;
            const QString mcVer = key.left(dash);
            const QString forgeVer = QString::fromStdString(it.value().get<std::string>());
            if (forgeVer.isEmpty()) continue;
            versions.append(mcVer + "-" + forgeVer);
        }
        versions.removeDuplicates();
        onComplete(true, versions);
    });
}

void Installer::fetchFabricVersions(std::function<void(bool, QStringList)> onComplete) {
    QString url = FABRIC_API + "/versions/loader";
    DownloadManager::instance().downloadJson(url, [this, onComplete](bool ok, QString, json arr) {
        QStringList versions;
        if (!ok || !arr.is_array()) { onComplete(false, versions); return; }
        for (const auto &v : arr) {
            versions.append(QString::fromStdString(
                v.value("version", v.value("loader", json::object()).value("version", ""))));
        }
        versions.removeAll({});
        versions.removeDuplicates();
        // Cache for detectBestFabricVersion
        m_fabricVersions = versions;
        onComplete(true, versions);
    });
}

void Installer::fetchNeoForgeVersions(std::function<void(bool, QStringList)> onComplete) {
    // NeoForge uses Maven metadata
    QString url = NEOFORGE_API + "/maven-metadata.xml";
    DownloadManager::instance().downloadToString(url, [onComplete](bool ok, QString xml) {
        QStringList versions;
        if (!ok) { onComplete(false, versions); return; }
        QRegularExpression re(R"(<version>([\d.]+)</version>)");
        auto it = re.globalMatch(xml);
        while (it.hasNext()) {
            auto m = it.next();
            versions.append(m.captured(1));
        }
        onComplete(true, versions);
    });
}

void Installer::detectBestForgeVersion(const QString &mcVersion,
                                         std::function<void(QString)> onComplete) {
    // 先查内置稳定版本表（快、无需网络），命中即返回。
    // 表只覆盖常见版本；未收录的 MC 版本回退到 Forge 官方 promotions 接口查询，
    // 否则会返回空串导致安装直接失败。
    QMap<QString, QString> knownStable = {
        {"1.20.1", "47.3.0"},
        {"1.20.4", "49.0.47"},
        {"1.19.4", "45.2.9"},
        {"1.19.2", "43.4.2"},
        {"1.18.2", "40.2.14"},
        {"1.16.5", "36.2.39"},
        {"1.12.2", "14.23.5.2859"},
        {"1.8.9",  "11.15.1.2318"},
    };
    if (knownStable.contains(mcVersion)) {
        if (onComplete) onComplete(knownStable.value(mcVersion));
        return;
    }

    // 回退：优先 recommended，其次 latest
    fetchForgeVersions([mcVersion, onComplete](bool ok, QStringList versions) {
        QString best;
        if (ok) {
            for (const auto &v : versions) {
                if (!v.startsWith(mcVersion + "-")) continue;
                // versions 里同时含 <mc>-latest 与 <mc>-recommended 两条，
                // fetchForgeVersions 已合并去重，这里取版本号最大的即可
                const QString forgeVer = v.mid(mcVersion.size() + 1);
                if (best.isEmpty()) { best = forgeVer; continue; }
                if (QVersionNumber::fromString(forgeVer) > QVersionNumber::fromString(best))
                    best = forgeVer;
            }
        }
        if (onComplete) onComplete(best);  // 查不到给空串，由上层报错
    });
}

QString Installer::detectBestFabricVersion(const QString &) {
    return "0.16.10"; // Latest stable Fabric loader
}

void Installer::detectBestNeoForgeVersion(const QString &mcVersion,
                                            std::function<void(QString)> onComplete) {
    QMap<QString, QString> knownStable = {
        {"1.20.1", "47.1.109"},
        {"1.20.4", "68.1.66"},
        {"1.21",   "21.1.66"},
    };
    if (onComplete) onComplete(knownStable.value(mcVersion));
}
