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
    QString url = FORGE_API + "/index.html";
    DownloadManager::instance().downloadToString(url, [onComplete](bool ok, QString html) {
        QStringList versions;
        if (!ok) { onComplete(false, versions); return; }
        // Parse HTML to extract version links
        QRegularExpression re(R"(forge-(\d+\.\d+(?:\.\d+)?-\d+\.\d+\.\d+)-installer)");
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            auto m = it.next();
            versions.append(m.captured(1));
        }
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
    // Best-effort: return the recommended Forge version for this MC version.
    // For production, this should call the Forge API. Known stable versions:
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
    if (onComplete) onComplete(knownStable.value(mcVersion));
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
