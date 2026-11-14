#include "core/mlccore_export.h"
#ifndef MLC_FILE_UTILS_H
#define MLC_FILE_UTILS_H

#include <QString>
#include <QStringList>
#include <QByteArray>

namespace FileUtils {

/// Build asset path from SHA1 hash: first 2 chars / full hash
MLCCORE_EXPORT QString assetPathFromHash(const QString &hash);

/// Verify file SHA1 matches expected hash
MLCCORE_EXPORT bool verifySha1(const QString &filePath, const QString &expectedHash);

/// Extract native libraries from a JAR/ZIP file into destDir.
/// Only extracts .so, .dll, .dylib, .jnilib files (skipping META-INF/).
/// Returns list of extracted file paths, or empty on failure.
MLCCORE_EXPORT QStringList extractNativesJar(const QString &jarPath, const QString &destDir);

/// maven 坐标 → 仓库相对路径（旧格式 libraries 用，≤1.13 的 JSON 无 downloads.artifact）
/// "group:artifact:version[:classifier][@ext]" → "group/artifact/version/artifact-version[-classifier].ext"
/// 解析失败返回空串
MLCCORE_EXPORT QString mavenNameToPath(const QString &name);

// ---- 通用 ZIP 读取（替代 QProcess unzip 调用） ----

/// 列出 zip 内所有条目名（按 local file header 顺序遍历；失败返回空）
MLCCORE_EXPORT QStringList listZipEntries(const QString &zipPath);

/// 解压整个 zip 到 destDir（保留目录结构；全部条目成功才返回 true）
MLCCORE_EXPORT bool extractZip(const QString &zipPath, const QString &destDir,
                                QString *errorOut = nullptr);

/// 解出单个条目到 destPath（用于内层 zip 探测；未找到返回 false）
MLCCORE_EXPORT bool extractZipEntry(const QString &zipPath, const QString &entryName,
                                     const QString &destPath);

/// 读取单个条目内容到内存（未找到/失败返回空）
MLCCORE_EXPORT QByteArray readZipEntry(const QString &zipPath, const QString &entryName);

/// 递归复制目录内容（替代 QProcess cp -r；Qt 处理中文文件名无编码问题）
MLCCORE_EXPORT bool copyDir(const QString &src, const QString &dst);

/// 路径拼接（头文件内联，自动补分隔符——目录变量约定带尾斜杠，不确定时用这个）
inline QString pathJoin(const QString &a, const QString &b) {
    if (a.isEmpty() || a.endsWith('/')) return a + b;
    return a + '/' + b;
}

/// 解压 .tar.gz（gzip 解压 + tar 遍历；用于 Adoptium JRE 包，替代外部 tar 命令）
MLCCORE_EXPORT bool extractTarGz(const QString &tgzPath, const QString &destDir,
                                  QString *errorOut = nullptr);

/// 清空目录内容但不顺符号链接：链接/普通文件用 QFile::remove，只有真实目录才 removeRecursively
/// （QDir::removeRecursively 作用于指向目录的符号链接时会删掉链接目标的内容——越界删除）
MLCCORE_EXPORT bool removeDirContents(const QString &path);

/// 安全删除整棵目录树（含目录本身；不顺符号链接）。路径不存在返回 true
MLCCORE_EXPORT bool removeTree(const QString &path);

} // namespace FileUtils

#endif // MLC_FILE_UTILS_H
