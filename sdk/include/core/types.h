#ifndef MLC_TYPES_H
#define MLC_TYPES_H

#include "core/mlccore_export.h"

#include <QString>
#include <QList>
#include <QDateTime>
#include <QVersionNumber>
#include <optional>
#include "util/platform_utils.h"

// Load state (mirrors original LoadState enum)

enum class LoadState {
    Waiting,
    Loading,
    Finished,
    Failed,
    Interrupted
};

// Java

struct JavaEntry {
    QString pathFolder;      // Path to java binary folder (trailing separator)
    QString pathJava;        // Full path to java executable
    QVersionNumber version;  // Full version (e.g., 1.8.0.321)
    int majorVersion = 0;    // Major version (e.g., 8, 17, 21)
    bool isJre = false;      // JRE vs JDK
    bool is64Bit = false;    // 64-bit vs 32-bit
    bool isUserImport = false;

    QString toString() const {
        // "%1 %2" 用 majorVersion，"%3" 只在它与 majorVersion 不一致时补全版本号，
        // 避免出现 "JRE 8 (1.8.0.321)" 这种重复/矛盾的显示
        const QString full = version.toString();
        const bool redundant = (majorVersion > 0
                                && (full == QString::number(majorVersion)
                                    || full.endsWith('.' + QString::number(majorVersion))));
        return QString("%1 %2%3%4: %5")
            .arg(isJre ? "JRE" : "JDK")
            .arg(majorVersion)
            .arg(redundant || full.isEmpty() ? QString() : QString(" (%1)").arg(full))
            .arg(is64Bit ? "" : ", 32-bit")
            .arg(pathFolder);
    }
};

// Minecraft Login

enum class LoginType {
    Legacy = 0,  // Offline
    Nide = 2,    // Nide8 / Unified Pass
    Auth = 3,    // Authlib-Injector
    Ms = 5       // Microsoft OAuth
};

struct MLCCORE_EXPORT LoginResult {
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString uuid MEMBER uuid)
    Q_PROPERTY(QString accessToken MEMBER accessToken)
    Q_PROPERTY(QString type MEMBER type)

public:
    QString name;
    QString uuid;
    QString accessToken;
    QString type;        // "Legacy", "Nide", "Auth", "Ms"
    QString clientToken;
    QString profileJson; // Microsoft profile JSON
    QString serverUrl;   // 外置登录（authlib/Nide）服务器地址；启动时注入 javaagent 用
    QString errorMessage; // 失败原因（仅失败时填充，内存传递不落盘）

    bool isValid() const {
        return !name.isEmpty() && !uuid.isEmpty();
    }
};

// Minecraft Version

struct McVersionInfo {
    QString id;              // Version ID (e.g., "1.20.1")
    QString type;            // "release", "snapshot", "old_beta", "old_alpha"
    QDateTime releaseTime;
    QString url;             // URL to version JSON
    bool isLocal = false;    // Already downloaded?
};

struct McModLoaderInfo {
    bool hasForge = false;
    bool hasFabric = false;
    bool hasNeoForge = false;
    bool hasOptiFine = false;
    bool hasLiteLoader = false;

    // Version strings for each loader
    QString forgeVersion;
    QString fabricVersion;
    QString neoForgeVersion;
    QString optiFineVersion;

    bool hasAny() const {
        return hasForge || hasFabric || hasNeoForge || hasOptiFine || hasLiteLoader;
    }
};

struct McVersion {
    QString id;                       // Version ID
    QString type;                     // "release", "snapshot", etc.
    QDateTime releaseTime;
    QString inheritName;              // Inherits from (empty if none)
    QVersionNumber vanillaVersion;    // Base Minecraft version
    McModLoaderInfo modLoader;
    bool isValid = false;             // Parsed successfully?
    QString info;                     // Error info if not valid

    // Paths
    QString pathVersion;              // Version folder path
    QString pathIndie;                // Instance folder path (may differ for isolation)
    QString pathJson;                 // Version JSON file path
    QString pathJar;                  // Version JAR file path

    QString versionDisplayName() const {
        QString name = id;
        if (modLoader.hasForge) name += QString(" (Forge %1)").arg(modLoader.forgeVersion);
        if (modLoader.hasFabric) name += QString(" (Fabric %1)").arg(modLoader.fabricVersion);
        if (modLoader.hasNeoForge) name += QString(" (NeoForge %1)").arg(modLoader.neoForgeVersion);
        if (modLoader.hasOptiFine) name += QString(" (OptiFine %1)").arg(modLoader.optiFineVersion);
        return name;
    }
};

// Minecraft Folder

struct McFolder {
    QString name;
    QString location;  // Path with trailing separator

    enum class Type {
        Vanilla,
        RenamedVanilla,
        Custom
    };
    Type type = Type::Vanilla;
};

// Download

struct DownloadProgress {
    qint64 bytesReceived = 0;
    qint64 bytesTotal = 0;
    double speed = 0.0;       // bytes/sec
    QString status;           // Current status text
};

// Launch options

struct McLaunchOptions {
    QString serverIp;
    QString saveBatch;        // Save launch script instead of launching
    QString targetVersionId;  // Force specific version
    QStringList extraGameArgs;
    int maxMemoryMB = 0;      // 0 = 按系统可用内存自动分配；>0 = 固定值
    int minMemoryMB = 512;
    bool fullscreen = false;
    int windowWidth = 854;
    int windowHeight = 480;
};

// Log entry (for UI log display)

struct LogEntry {
    QDateTime time;
    QString text;
    enum Level { Info, Warn, Error } level = Info;
};

#endif // MLC_TYPES_H
