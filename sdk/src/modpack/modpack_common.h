#ifndef MLC_MODPACK_COMMON_H
#define MLC_MODPACK_COMMON_H

// 整合包安装内部共享：各安装器与下载管线共用的工具和流程
// 不属于公开 API（公开接口见 sdk/include/modpack.h）

#include <QString>
#include <QStringList>
#include <QList>
#include <nlohmann/json.hpp>
#include "modpack.h"

using json = nlohmann::json;

// Mod 下载条目
struct ModDownloadEntry {
    QString url;       // 直接下载 URL（Modrinth）
    QString savePath;  // 保存到实例目录 + savePath
    QString cfModId;   // CurseForge project ID
    QString cfFileId;  // CurseForge file ID
};

// ---- 通用辅助（common.cpp） ----

QString generateInstanceDir();  // 随机 8 位实例目录名
void writeInstanceMapping(const QString &dirName, const QString &displayName);
json parseJsonSafe(const QByteArray &data, bool *ok = nullptr);
bool copyDir(const QString &src, const QString &dst);
bool extractZip(const QString &zipPath, const QString &destDir,
                PackProgressCallback onProgress, int baseProgress = 10);
QString findMcRoot(const QStringList &entries);
bool validateInstanceName(const QString &name);

// 校验清单里来的相对子目录（如 manifest 的 overrides / packmeta 的 overrides）：
// 必须是 packDir 内的简单相对路径，拒绝 ..、绝对路径、盘符、反斜杠穿越。
// 合法时返回规范化后的绝对路径，非法返回空串。
QString resolvePackSubdir(const QString &packDir, const QString &rel);

// 导入临时目录（common.cpp）：本进程专属 {mcFolder}/tmp/<pid>/，
// 多进程并发 inpack 各自隔离，只建/删自己的子目录，不再整目录清 tmp/
QString packTmpRoot();
// 清理本进程的 tmp/<pid>/（成功/失败路径都调；FileUtils::removeTree 不顺符号链接）
void cleanupPackTmp();

// ---- .incomplete 标记与回滚（common.cpp） ----

void markIncomplete(const QString &finalDir);
void markComplete(const QString &finalDir);
void cleanupOnError(const QString &finalDir);  // 删实例目录 + INI 映射 + 本进程 tmp/<pid>/
// 兜底回滚：扫描 instances/ 下带 .incomplete 标记的目录并整体删除
// （用于拿不到 finalDir 的异常路径，如清单解析抛错）
void rollbackIncompleteInstances();
bool checkNameConflict(const QString &targetDir, const QString &name,
                       bool explicitName, PackCompleteCallback onComplete);

// 各安装器共用的前置流程：解析实例名 → 冲突检查 → 建目录 + .incomplete 标记
// 失败时已调 onComplete(false)，返回 false
bool beginInstall(const QString &instanceName, const QString &packName,
                  PackCompleteCallback onComplete,
                  QString &finalDirOut, QString &nameOut);

// 复制失败 = 导入失败（cleanupOnError + onComplete(false)，返回 false）
bool copyOrFail(const QString &src, const QString &finalDir, PackCompleteCallback onComplete);

// 提取纯净 MC 版本号（"1.21.1-NeoForge_21.1.226" → "1.21.1"）
QString extractVanillaVersion(const QString &v);

// 确定实例应记录的版本 json 名（写进 Setup.ini 的 Version 键）：
// 实例内版本文件夹（带 json）→ 按 loader 类型精确匹配全局 versions/ → vanilla
QString resolveInstanceVersionName(const QString &finalDir, const QString &mcVersion,
                                   const QString &loaderType = {}, const QString &loaderVer = {});

// ---- 下载管线（pipeline.cpp） ----

void downloadAndFinalize(const QString &mcVersion,
                         const QString &forgeVer, const QString &neoVer, const QString &fabricVer,
                         const QString &finalDir, const QString &name,
                         const QList<ModDownloadEntry> &mods,
                         PackProgressCallback onProgress,
                         PackCompleteCallback onComplete);

// ---- 类型安装器（installers.cpp） ----

void installCurseForge(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installHMCL(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installMultiMC(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installMCBBS(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installModrinth(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installLauncherPack(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installCompressed(const QString &filePath, const QString &packDir, const QString &instanceName, PackProgressCallback, PackCompleteCallback);
void installMod(const QString &packDir, const QString &targetInstance, PackProgressCallback, PackCompleteCallback);

#endif // MLC_MODPACK_COMMON_H
