// 整合包导入入口：类型检测 → 解压到 tmp/<pid>/pack/ → 派发到类型安装器
#include "modpack_common.h"
#include "core/versionmanager.h"
#include "download/downloadmanager.h"
#include <QDir>
#include <QFile>

// ---- internal dispatch (from already-extracted directory) ----

static void installModpackFromDir(const QString &filePath, const QString &packDir,
                                   PackType type, const QString &instanceName,
                                   const QString &targetInstance,
                                   PackProgressCallback onProgress,
                                   PackCompleteCallback onComplete) {
    // 处理一级目录包装（如 zip/蛊真人/ → 进入子目录）
    QString effectiveDir = packDir;
    QDir rootDir(effectiveDir);
    auto entries = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.size() == 1) {
        QString subDir = entries.first().absoluteFilePath() + "/";
        QDir sub(subDir);
        if (sub.exists("manifest.json") || sub.exists("modrinth.index.json") ||
            sub.exists("mmc-pack.json") || sub.exists("mcbbs.packmeta") ||
            sub.exists("modpack.json") || sub.exists("modpack.zip") ||
            sub.exists("modpack.mrpack")) {
            effectiveDir = subDir;
        }
    }

    // 分派到对应的安装器（nlohmann value() 遇 null 会抛 type_error，
    // 畸形清单统一在此兜底为干净错误而不是进程终止）
    try {
        switch (type) {
        case PackType::CurseForge:
            installCurseForge(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        case PackType::HMCL:
            installHMCL(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        case PackType::MultiMC:
            installMultiMC(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        case PackType::MCBBS:
            installMCBBS(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        case PackType::Modrinth:
            installModrinth(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        case PackType::LauncherPack:
            installLauncherPack(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        case PackType::Mod:
            installMod(effectiveDir, targetInstance, onProgress, onComplete);
            break;
        case PackType::Compressed:
            installCompressed(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        default:
            // 回退：当作压缩版 .minecraft 处理
            installCompressed(filePath, effectiveDir, instanceName, onProgress, onComplete);
            break;
        }
    } catch (const std::exception &e) {
        // 安装器可能已经 beginInstall()（建好实例目录 + .incomplete 标记）后才抛异常：
        // 这里拿不到 finalDir，按"本进程刚建的 .incomplete 目录"回滚，避免残留半成品
        rollbackIncompleteInstances();
        cleanupPackTmp();
        if (onComplete) onComplete(false, QString("清单解析失败: %1").arg(e.what()));
    }
}

// ---- main entry ----

void installModpack(const QString &filePath,
                     const QString &instanceName,
                     const QString &targetInstance,
                     PackProgressCallback onProgress,
                     PackCompleteCallback onComplete) {
    if (!QFile::exists(filePath)) {
        if (onComplete) onComplete(false, "File not found: " + filePath);
        return;
    }

    // 包装完成回调：所有失败路径（含异步链深处）统一兜底清理本进程 tmp/<pid>/
    PackCompleteCallback guardedComplete = [onComplete](bool ok, const QString &msg) {
        if (!ok) cleanupPackTmp();
        if (onComplete) onComplete(ok, msg);
    };

    if (onProgress) onProgress("Detecting modpack type...", 5);

    PackType type = detectPackType(filePath);
    if (onProgress) onProgress("Type: " + packTypeName(type), 8);

    // 在游戏目录下创建本进程专属 tmp/<pid>/，解压到其下 pack/
    QString mcFolder = VersionManager::instance().mcFolder();
    // 设置 mcFolder 供 AssetDownloader 兜底使用
    DownloadManager::instance().setProperty("mcFolder", mcFolder);
    QString tmpRoot = packTmpRoot();

    // 只清本进程子目录（上次崩溃残留），不动其他进程的 tmp/<pid>/
    cleanupPackTmp();
    QDir().mkpath(tmpRoot);

    QString packDir = tmpRoot + "pack/";
    QDir().mkpath(packDir);

    if (!extractZip(filePath, packDir, onProgress, 10)) {
        cleanupPackTmp();
        guardedComplete(false, "Extraction failed");
        return;
    }

    installModpackFromDir(filePath, packDir, type, instanceName, targetInstance, onProgress, guardedComplete);

    // 注意：tmp/<pid>/ 不在此清理——异步下载链末尾的 finalizeNow() 会统一清理；
    // 同步路径（Compressed Step 3 / Mod 包）已自行清理，失败路径由 guardedComplete 兜底。
}
