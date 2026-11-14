// 下载管线：MC 本体 → natives → modloader → mods → finalize
#include "modpack_common.h"
#include "core/settings.h"
#include "core/versionmanager.h"
#include "core/javamanager.h"
#include "core/installer.h"
#include "download/downloadmanager.h"
#include "download/assetdownloader.h"
#include "download/modplatform.h"
#include <QDir>
#include <QRegularExpression>
#include <QSettings>

void downloadModsAsync(const QList<ModDownloadEntry> &mods, int index,
                               const QString &finalDir, const QString &name, const QString &mcVersion,
                               const QString &loaderType, const QString &loaderVer,
                               PackProgressCallback onProgress,
                               PackCompleteCallback onComplete) {
    // 所有 mod 下载完成后的 finalize
    auto finalizeNow = [=]() {
        QDir(finalDir + "PCL/").mkpath(".");
        QSettings ini(finalDir + "PCL/Setup.ini", QSettings::IniFormat);
        ini.beginGroup("Setup");
        ini.setValue("Name", name);
        ini.setValue("VersionArgumentIndie", 1);
        ini.setValue("VersionArgumentIndieV2", true);
        // 记录版本 json 名——launch 时据此解析（实例内版本文件夹/全局 loader 目录/vanilla）
        QString versionName = resolveInstanceVersionName(finalDir, mcVersion, loaderType, loaderVer);
        if (!versionName.isEmpty()) ini.setValue("Version", versionName);
        ini.endGroup();
        ini.sync();
        // 写入 INI 实例映射（随机目录名 → 显示名）
        writeInstanceMapping(QDir(finalDir).dirName(), name);
        // 清理本进程 tmp/<pid>/ + 移除 .incomplete 标记
        cleanupPackTmp();
        markComplete(finalDir);
        if (onProgress) onProgress("Complete", 100);
        if (onComplete) onComplete(true, name);
    };

    if (index >= mods.size()) {
        if (onProgress) onProgress("Finalizing...", 95);
        finalizeNow();
        return;
    }

    const auto &mod = mods[index];
    if (onProgress) onProgress(QString("Downloading mod (%1/%2)...").arg(index + 1).arg(mods.size()),
                                70 + (30 * index / mods.size()));

    // 任一 mod 下载失败 = 整合包导入失败：回滚删除实例 + 清本进程 tmp（cleanupOnError），不再继续
    auto failNow = [=](const QString &what) {
        qWarning() << "Mod download failed, rolling back:" << what;
        cleanupOnError(finalDir);
        if (onComplete) onComplete(false, "Mod download failed: " + what);
    };

    if (!mod.url.isEmpty()) {
        // 直接 URL（Modrinth）
        DownloadManager::instance().download(mod.url, finalDir + mod.savePath,
            nullptr,
            [=](bool ok, QString) {
                if (!ok) { failNow(mod.url); return; }
                downloadModsAsync(mods, index + 1, finalDir, name, mcVersion, loaderType, loaderVer, onProgress, onComplete);
            });
    } else if (!mod.cfModId.isEmpty()) {
        // CurseForge — 通过 ModPlatform 解析下载 URL
        ModPlatform::instance().downloadMod(ModPlatform::CurseForge,
            mod.cfModId, mod.cfFileId, finalDir + mod.savePath,
            [=](bool ok, QString) {
                if (!ok) { failNow("CF mod " + mod.cfModId); return; }
                downloadModsAsync(mods, index + 1, finalDir, name, mcVersion, loaderType, loaderVer, onProgress, onComplete);
            });
    } else {
        downloadModsAsync(mods, index + 1, finalDir, name, mcVersion, loaderType, loaderVer, onProgress, onComplete);
    }
}

void downloadAndFinalize(const QString &mcVersion,
                                 const QString &forgeVer, const QString &neoVer, const QString &fabricVer,
                                 const QString &finalDir, const QString &name,
                                 const QList<ModDownloadEntry> &mods,
                                 PackProgressCallback onProgress,
                                 PackCompleteCallback onComplete) {
    // 确定 modloader（forge > neoforge > fabric）
    QString loaderType, loaderVer;
    if (!forgeVer.isEmpty())       { loaderType = "forge"; loaderVer = forgeVer; }
    else if (!neoVer.isEmpty())    { loaderType = "neoforge"; loaderVer = neoVer; }
    else if (!fabricVer.isEmpty()) { loaderType = "fabric"; loaderVer = fabricVer; }

    auto startModDownloads = [=]() {
        downloadModsAsync(mods, 0, finalDir, name, mcVersion, loaderType, loaderVer, onProgress, onComplete);
    };

    if (mcVersion.isEmpty()) {
        startModDownloads();
        return;
    }

    // Step 1: 下载 MC 版本（JSON + JAR + libraries + assets）
    QString vanilla = extractVanillaVersion(mcVersion);
    if (onProgress) onProgress("Downloading Minecraft " + vanilla + "...", 35);
    AssetDownloader::instance().downloadVersion(vanilla,
        [=](bool ok, QString err) {
            if (!ok) {
                cleanupOnError(finalDir);
                if (onComplete) onComplete(false, "Download Minecraft failed: " + err);
                return;
            }

            // Step 2: 下载 natives（平台相关原生库）
            if (onProgress) onProgress("Downloading native libraries...", 55);
            McVersion ver;
            ver.id = vanilla;
            ver.pathVersion = VersionManager::instance().mcFolder() + "versions/" + vanilla + "/";
            ver.pathJar = ver.pathVersion + vanilla + ".jar";
            ver.pathIndie = VersionManager::instance().mcFolder();
            AssetDownloader::instance().downloadNatives(ver,
                [=](bool ok2, QString err2) {
                    // natives 失败 = 导入失败（缺 .so 启动必崩，不存在"部分成功"）
                    if (!ok2) {
                        cleanupOnError(finalDir);
                        if (onComplete) onComplete(false, "Download natives failed: " + err2);
                        return;
                    }

                    // Step 3: Installing modloader
                    auto installLoader = [=]() {
                        if (loaderType.isEmpty()) {
                            startModDownloads();
                            return;
                        }

                        if (onProgress) onProgress("Installing " + loaderType + "...", 65);
                        auto &jm = JavaManager::instance();
                        if (jm.javaList().isEmpty()) {
                            // inpack 流程此前不会触发 Java 扫描，这里补扫并等待
                            jm.scanSystemJava();
                            jm.waitForScanFinished();
                        }
                        // 按 MC 版本兼容矩阵选 Java（老 Forge 安装器需要 Java 8，
                        // 不能拿扫描列表的第一个）
                        McVersion jver;
                        jver.vanillaVersion = QVersionNumber::fromString(vanilla);
                        JavaEntry je = jm.selectJavaForVersion(jver);
                        if (je.pathJava.isEmpty()) je = jm.selectJava();
                        QString javaPath = je.pathJava;
                        if (javaPath.isEmpty()) {
                            // 没有 Java 装不了 modloader，按失败处理
                            cleanupOnError(finalDir);
                            if (onComplete) onComplete(false, "No Java runtime found for modloader install");
                            return;
                        }

                        Installer::instance().installLoader(loaderType,
                            VersionManager::instance().mcFolder(), vanilla, loaderVer, javaPath,
                            [=](bool ok3, QString err3) {
                                // modloader 失败 = 导入失败（mod 全不生效）
                                if (!ok3) {
                                    cleanupOnError(finalDir);
                                    if (onComplete) onComplete(false, "Install modloader failed: " + err3);
                                    return;
                                }
                                startModDownloads();
                            });
                    };

                    installLoader();
                });
        });
}

