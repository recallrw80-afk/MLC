#include "core/javamanager.h"
#include "core/settings.h"
#include "core/versionmanager.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QLoggingCategory>
#include <QThreadPool>

static Q_LOGGING_CATEGORY(logJava, "mlc.java")

JavaManager& JavaManager::instance() {
    static JavaManager m;
    return m;
}

// Scanning

QStringList JavaManager::javaSearchPaths() {
    QStringList paths;

    auto addIfExists = [&](const QString &path) {
        if (QDir(path).exists()) {
            paths.append(QDir(path).absolutePath());
        }
    };

    // MLC 自动下载的 Java 存放目录（{mcFolder}/javas/）——确保重装/重启后仍被发现
    QString mcFolder = VersionManager::instance().mcFolder();
    if (!mcFolder.isEmpty())
        addIfExists(mcFolder + "javas/");

    switch (currentPlatform()) {
    case Platform::Windows: {
        // Common Windows Java paths
        addIfExists("C:/Program Files/Java");
        addIfExists("C:/Program Files (x86)/Java");
        addIfExists("C:/Program Files/Eclipse Adoptium");
        addIfExists("C:/Program Files/AdoptOpenJDK");
        addIfExists("C:/Program Files/Zulu");
        addIfExists("C:/Program Files/Semeru");
        addIfExists("C:/Program Files/Microsoft");

        // Check all drives for Java
        for (char drive = 'C'; drive <= 'Z'; ++drive) {
            QString root = QString("%1:/").arg(drive);
            if (QDir(root).exists()) {
                addIfExists(root + "Program Files/Java");
                addIfExists(root + "Program Files/Eclipse Adoptium");
            }
        }
        break;
    }
    case Platform::Linux: {
        addIfExists("/usr/lib/jvm");
        addIfExists("/usr/lib64/jvm");
        addIfExists("/usr/local/lib/jvm");
        addIfExists("/opt/jdk");
        addIfExists("/opt/java");

        // Also check subdirectories of /usr/lib/jvm which is the most common
        QDir jvmDir("/usr/lib/jvm");
        if (jvmDir.exists()) {
            for (const auto &entry : jvmDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString javaPath = entry.absoluteFilePath() + "/bin/java";
                if (QFileInfo::exists(javaPath)) {
                    paths.append(entry.absoluteFilePath() + "/bin");
                }
            }
        }
        break;
    }
    case Platform::MacOS: {
        addIfExists("/Library/Java/JavaVirtualMachines");
        // Homebrew
        addIfExists("/opt/homebrew/opt/openjdk");
        addIfExists("/opt/homebrew/opt/openjdk@17");
        addIfExists("/opt/homebrew/opt/openjdk@11");
        addIfExists("/opt/homebrew/opt/openjdk@8");
        addIfExists("/usr/local/opt/openjdk");
        addIfExists("/usr/local/opt/openjdk@17");
        break;
    }
    default:
        break;
    }

    return paths;
}

bool JavaManager::isJavaBinary(const QString &path) const
{
    QString name = QFileInfo(path).baseName().toLower();
    return name == "java" || name == "java.exe";
}

void JavaManager::scanSystemJava() {
    // Check-and-set under the mutex so two near-simultaneous calls can't
    // both pass the guard and spawn duplicate pool work.
    {
        QMutexLocker lock(&m_mutex);
        if (m_isScanning) return;
        m_isScanning = true;
    }
    emit scanningChanged();
    emit javaScanProgress("Scanning for Java...");

    QThreadPool::globalInstance()->start([this]() {
        QList<JavaEntry> found;

        // 1. Scan PATH environment variable
        scanPathVariable();

        // 2. Scan JAVA_HOME
        scanJavaHome();

        // 3. Scan system paths
        QStringList searchPaths = javaSearchPaths();
        for (const auto &dir : searchPaths) {
            scanFolder(dir, false);
        }

        emit javaScanProgress("Java scan complete");
        {
            QMutexLocker lock(&m_mutex);
            m_isScanning = false;
        }
        emit scanningChanged();
    });
}

void JavaManager::waitForScanFinished() {
    while (m_isScanning.load())
        QThread::msleep(20);
}

void JavaManager::scanPathVariable() {
    QString pathEnv = qEnvironmentVariable("PATH");
    if (pathEnv.isEmpty()) {
        pathEnv = qEnvironmentVariable("Path"); // Windows
    }

    // Windows 的 PATH 分隔符是 ';'（盘符含 ':' 会切碎），Unix 是 ':'
    const QChar sep = (currentPlatform() == Platform::Windows) ? ';' : ':';
    const auto entries = pathEnv.split(sep, Qt::SkipEmptyParts);
    for (const auto &path : entries) {
        QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) continue;

        QString javaPath = trimmed + "/java";
        if (currentPlatform() == Platform::Windows) javaPath += ".exe";

        if (QFileInfo::exists(javaPath) && isJavaBinary(javaPath)) {
            JavaEntry entry;
            entry.pathFolder = QDir(trimmed).absolutePath() + "/";
            entry.pathJava = QDir(javaPath).absolutePath();
            entry.isUserImport = false;

            if (checkJava(entry)) {
                addJavaEntry(entry);
            }
        }
    }
}

void JavaManager::scanJavaHome() {
    QString javaHome = qEnvironmentVariable("JAVA_HOME");
    if (javaHome.isEmpty()) return;

    QString javaBin = QDir(javaHome).filePath("bin");
    QString javaPath = javaBin + "/java";
    if (currentPlatform() == Platform::Windows) javaPath += ".exe";

    if (QFileInfo::exists(javaPath)) {
        JavaEntry entry;
        entry.pathFolder = QDir(javaBin).absolutePath() + "/";
        entry.pathJava = QDir(javaPath).absolutePath();
        entry.isUserImport = false;

        if (checkJava(entry)) {
                addJavaEntry(entry);
        }
    }
}

void JavaManager::scanFolder(const QString &folder, bool isUserImport) {
    QDir dir(folder);
    if (!dir.exists()) return;

    // Look for java binaries recursively (up to 3 levels deep)
    QStringList nameFilters;
    if (currentPlatform() == Platform::Windows) {
        nameFilters << "java.exe";
    } else {
        nameFilters << "java";
    }

    const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        if (entry.isDir()) {
            // Check if this directory has a bin/java
            QString javaPath = entry.absoluteFilePath() + "/bin/java";
            if (currentPlatform() == Platform::Windows) javaPath += ".exe";

            if (QFileInfo::exists(javaPath)) {
                JavaEntry je;
                je.pathFolder = QDir(entry.absoluteFilePath() + "/bin").absolutePath() + "/";
                je.pathJava = QDir(javaPath).absolutePath();
                je.isUserImport = isUserImport;

                if (checkJava(je)) {
                addJavaEntry(je);
                }
            }
            // On macOS, also check Contents/Home/bin/java inside JavaVirtualMachines
            QString macJavaPath = entry.absoluteFilePath() + "/Contents/Home/bin/java";
            if (QFileInfo::exists(macJavaPath)) {
                JavaEntry je;
                je.pathFolder = QDir(entry.absoluteFilePath() + "/Contents/Home/bin").absolutePath() + "/";
                je.pathJava = QDir(macJavaPath).absolutePath();
                je.isUserImport = isUserImport;

                if (checkJava(je)) {
                addJavaEntry(je);
                }
            }
        } else {
            // Direct java binary
            if (isJavaBinary(entry.absoluteFilePath())) {
                JavaEntry je;
                je.pathFolder = QDir(entry.absolutePath()).absolutePath() + "/";
                je.pathJava = entry.absoluteFilePath();
                je.isUserImport = isUserImport;

                if (checkJava(je)) {
                addJavaEntry(je);
                }
            }
        }
    }
}

// ---- Deduplicating append (thread-safe) ----

bool JavaManager::addJavaEntry(const JavaEntry &entry) {
    {
        QMutexLocker lock(&m_mutex);
        for (const auto &e : m_javaList) {
            if (e.pathFolder == entry.pathFolder) return false;
        }
        m_javaList.append(entry);
    }
    emit javaListChanged();  // 锁外发送，避免直连槽回调造成死锁
    return true;
}

// Java runtime checking

bool JavaManager::checkJava(JavaEntry &entry) {
    if (!QFileInfo::exists(entry.pathJava)) {
        qCDebug(logJava) << "Java binary not found:" << entry.pathJava;
        return false;
    }

    // Check if javac exists (JRE vs JDK)
    QString javacPath = entry.pathFolder + "javac";
    if (currentPlatform() == Platform::Windows) javacPath += ".exe";
    entry.isJre = !QFileInfo::exists(javacPath);

    // Run java -version
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(entry.pathJava, {"-version"});

    if (!process.waitForFinished(15000)) {
        qCDebug(logJava) << "Java -version timed out:" << entry.pathJava;
        emit javaCheckFailed(entry.pathJava, "Timed out");
        return false;
    }

    QString output = QString::fromUtf8(process.readAll()).trimmed();
    if (output.isEmpty()) {
        output = QString::fromUtf8(process.readAllStandardError()).trimmed();
    }

    if (output.isEmpty()) {
        qCDebug(logJava) << "Java -version produced no output:" << entry.pathJava;
        emit javaCheckFailed(entry.pathJava, "No output from java -version");
        return false;
    }

    // Parse version
    entry.version = parseJavaVersionOutput(output);
    if (entry.version.isNull()) {
        qCDebug(logJava) << "Failed to parse Java version from:" << output;
        emit javaCheckFailed(entry.pathJava, "Failed to parse version: " + output.left(200));
        return false;
    }

    // Determine major version
    if (entry.version.majorVersion() == 1) {
        entry.majorVersion = entry.version.minorVersion(); // 1.8 -> 8
    } else {
        entry.majorVersion = entry.version.majorVersion(); // 21.0.2 -> 21
    }

    // 64-bit detection
    entry.is64Bit = output.contains("64-Bit", Qt::CaseInsensitive) ||
                    output.contains("64 Bit", Qt::CaseInsensitive);

    // On 64-bit systems, 32-bit Java is not ideal but still usable
    // Don't auto-reject it, let the caller decide

    // Basic sanity check
    if (entry.majorVersion <= 4 || entry.majorVersion >= 100) {
        qCDebug(logJava) << "Java version out of range:" << entry.version;
        emit javaCheckFailed(entry.pathJava, "Version out of range: " + entry.version.toString());
        return false;
    }

    qCDebug(logJava) << "Found Java:" << entry.toString();
    return true;
}

QList<JavaEntry> JavaManager::checkJavaList(const QList<JavaEntry> &list) {
    QList<JavaEntry> valid;
    for (auto entry : list) {
        if (checkJava(entry)) {
            valid.append(entry);
        }
    }
    return valid;
}

// Version parsing

QVersionNumber JavaManager::parseJavaVersionOutput(const QString &output) {
    // Try to find version string in output
    // Formats:
    //   java version "1.8.0_321"
    //   openjdk version "21.0.2" 2024-01-16
    //   openjdk version "17.0.9" 2023-10-17

    QRegularExpression re(R"(version\s+\"([^\"]+)\")");
    auto match = re.match(output);
    if (!match.hasMatch()) {
        // Try alternative: just look for a version-like pattern
        QRegularExpression re2(R"((\d+[\._]\d+[\._]\d+[\._]?\d*))");
        match = re2.match(output);
        if (!match.hasMatch()) return QVersionNumber();
    }

    QString versionStr = match.captured(1);
    // Replace underscores with dots: 1.8.0_321 -> 1.8.0.321
    versionStr.replace('_', '.');
    // Remove leading non-digit characters (e.g., "1.8.0" from "1.8.0.321")
    // Take only version parts: split by non-digit then by dot

    // Remove any trailing part after dash: "21.0.2-beta" -> "21.0.2"
    int dashIdx = versionStr.indexOf('-');
    if (dashIdx > 0) versionStr = versionStr.left(dashIdx);

    QStringList parts = versionStr.split('.');
    if (parts.isEmpty()) return QVersionNumber();

    // Build QVersionNumber
    QList<int> segments;
    for (const auto &part : parts) {
        bool ok;
        int val = part.toInt(&ok);
        if (ok) segments.append(val);
        else break;
    }

    if (segments.isEmpty()) return QVersionNumber();

    // Pad to at least 4 segments (like original VB code)
    while (segments.size() < 4) {
        segments.append(0);
    }

    return QVersionNumber(segments);
}

// Java selection

JavaEntry JavaManager::selectJava(const QVersionNumber &minVersion,
                                    const QVersionNumber &maxVersion) {
    QMutexLocker lock(&m_mutex);
    QList<JavaEntry*> candidates;

    for (auto &java : m_javaList) {
        if (!minVersion.isNull() && java.version < minVersion) continue;
        if (!maxVersion.isNull() && java.version > maxVersion) continue;
        if (java.is64Bit && !is64BitSystem()) continue;
        candidates.append(&java);
    }

    if (candidates.isEmpty()) return {};

    // Prefer 64-bit, then LOWEST satisfying version, then JDK over JRE：
    // modded MC（NeoForge/Fabric/Iris 等）对高于官方推荐版本的 Java 兼容性差，
    // 区间内选最低版本最接近官方推荐（1.21.x → 21 而非 25）
    std::sort(candidates.begin(), candidates.end(), [](JavaEntry *a, JavaEntry *b) {
        if (a->is64Bit != b->is64Bit) return a->is64Bit > b->is64Bit;
        if (a->version != b->version) return a->version < b->version;
        return !a->isJre && b->isJre;
    });

    return *candidates.first();
}

JavaEntry JavaManager::selectJavaForVersion(const McVersion &version) {
    QVersionNumber minVer, maxVer;
    getJavaCompatibilityRange(version, minVer, maxVer);
    return selectJava(minVer, maxVer);
}

// Java version compatibility matrix

void JavaManager::getJavaCompatibilityRange(const McVersion &version,
                                             QVersionNumber &outMin,
                                             QVersionNumber &outMax) {
    // Default: no restrictions
    outMin = QVersionNumber();
    outMax = QVersionNumber();

    // MC 1.x 的特性版本号在第二段（"1.20.1" → 20）
    int feature = version.vanillaVersion.majorVersion() == 1
        ? version.vanillaVersion.minorVersion()
        : version.vanillaVersion.majorVersion();
    int patch = version.vanillaVersion.segmentCount() > 2
        ? version.vanillaVersion.segmentAt(2) : 0;

    // 上限约束：null 表示无限制，直接赋值；否则取更严格者
    // （qMin(null, X) 会返回 null——null 比任何版本都小——上限永远写不进去）
    auto applyMax = [&outMax](const QVersionNumber &v) {
        if (outMax.isNull() || v < outMax) outMax = v;
    };

    // 原版基线约束（对 modloader 版本同样适用——modded 也跑在同一个 MC 上）
    // 注意 Java 9+ 的版本号首段即主版本（17.0.x），只有 Java 8 及以前是 1.x 格式
    if (feature < 8) {
        applyMax(QVersionNumber({1, 8, 999, 999})); // 1.7.x 及更早：Java 8 封顶
    } else if (feature >= 8 && feature <= 12) {
        outMin = qMax(outMin, QVersionNumber({1, 8, 0, 0}));
        applyMax(QVersionNumber({1, 8, 999, 999})); // Java 8
    } else if (feature >= 13 && feature <= 16) {
        outMin = qMax(outMin, QVersionNumber({1, 8, 0, 0})); // Java 8+
    } else if (feature == 17) {
        outMin = qMax(outMin, QVersionNumber({16, 0, 0, 0})); // 1.17 → Java 16+
    } else if (feature >= 18) {
        outMin = qMax(outMin, QVersionNumber({17, 0, 0, 0})); // 1.18–1.20.4 → Java 17+
    }
    if (feature >= 21 || (feature == 20 && patch >= 5)) {
        outMin = qMax(outMin, QVersionNumber({21, 0, 0, 0})); // 1.20.5+/1.21+ → Java 21+
    }

    // Forge checks
    if (version.modLoader.hasForge && version.isValid) {
        auto forge = version.modLoader.forgeVersion;
        auto forgeVer = QVersionNumber::fromString(forge);

        if (feature >= 6 && feature <= 7 && patch <= 2) {
            outMin = qMax(outMin, QVersionNumber({1, 7, 0, 0}));
            applyMax(QVersionNumber({1, 7, 999, 999})); // Java 7
        } else if (feature <= 12) {
            applyMax(QVersionNumber({1, 8, 999, 999})); // <= Java 8
        } else if (feature <= 14) {
            outMin = qMax(outMin, QVersionNumber({1, 8, 0, 0}));
            applyMax(QVersionNumber({10, 999, 999, 999})); // Java 8-10
        } else if (feature == 15) {
            outMin = qMax(outMin, QVersionNumber({1, 8, 0, 0}));
            applyMax(QVersionNumber({15, 999, 999, 999})); // Java 8-15
        } else if (feature == 16 && !forgeVer.isNull()) {
            // Forge 34.0.0 ~ 36.2.25: max Java 8u320
            if (forgeVer >= QVersionNumber({34, 0, 0}) && forgeVer <= QVersionNumber({36, 2, 25})) {
                applyMax(QVersionNumber({1, 8, 0, 320}));
            }
            // Forge 36.2.26+ and < 37: max Java 23
            else if (forgeVer >= QVersionNumber({36, 2, 26}) && forgeVer < QVersionNumber({37, 0, 0})) {
                applyMax(QVersionNumber({23, 999, 999, 999}));
            }
        } else if (feature == 17 && !forgeVer.isNull()) {
            // Forge 37.0.0 ~ 37.0.79: max Java 16
            if (forgeVer >= QVersionNumber({37, 0, 0}) && forgeVer <= QVersionNumber({37, 0, 79})) {
                applyMax(QVersionNumber({16, 999, 999, 999}));
            }
        } else if (feature == 18 && version.modLoader.hasOptiFine) {
            applyMax(QVersionNumber({18, 999, 999, 999})); // Java 18 max with OptiFine
        } else if (feature == 19 && !forgeVer.isNull()) {
            // Forge 45.0.21 ~ 45.0.65: max Java 19
            if (forgeVer >= QVersionNumber({45, 0, 21}) && forgeVer <= QVersionNumber({45, 0, 65})) {
                applyMax(QVersionNumber({19, 999, 999, 999}));
            }
        }
        // Forge 45.0.66 ~ 47.4.8: max Java 21
        if (!forgeVer.isNull() && forgeVer >= QVersionNumber({45, 0, 66}) && forgeVer <= QVersionNumber({47, 4, 8})) {
            applyMax(QVersionNumber({21, 999, 999, 999}));
        }
    }

    // NeoForge checks（1.20.x: max Java 21）
    if (version.modLoader.hasNeoForge && feature == 20) {
        applyMax(QVersionNumber({21, 999, 999, 999}));
    }

    // Fabric checks
    if (version.modLoader.hasFabric && version.isValid) {
        if (feature >= 15 && feature <= 16) {
            outMin = qMax(outMin, QVersionNumber({1, 8, 0, 0})); // Java 8+
        } else if (feature >= 18) {
            outMin = qMax(outMin, QVersionNumber({17, 0, 0, 0})); // Java 17+
        }
    }

    // LiteLoader: always max Java 8
    if (version.modLoader.hasLiteLoader) {
        applyMax(QVersionNumber({1, 8, 999, 999}));
    }
}

// Java selection (getter)

QString JavaManager::selectedJavaName() const
{
    if (m_selectedJava) return m_selectedJava->toString();
    return "None";
}

QStringList JavaManager::javaNames() const
{
    QMutexLocker lock(&m_mutex);
    QStringList names;
    for (const auto &j : m_javaList) {
        names.append(j.toString());
    }
    return names;
}

QVariantList JavaManager::javaVariantList() const
{
    QMutexLocker lock(&m_mutex);
    QVariantList out;
    for (const auto &j : m_javaList) {
        QVariantMap m;
        m.insert("pathJava", j.pathJava);
        m.insert("pathFolder", j.pathFolder);
        m.insert("display", j.toString());
        m.insert("majorVersion", j.majorVersion);
        m.insert("is64Bit", j.is64Bit);
        m.insert("isUserImport", j.isUserImport);
        out.append(m);
    }
    return out;
}

int JavaManager::javaCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_javaList.size();
}

// Java download

QString JavaManager::getJavaDownloadUrl(int majorVersion) const
{
    // Use Adoptium API for cross-platform Java downloads
    // Format: https://api.adoptium.net/v3/assets/latest/{version}/hotspot
    // For now return a static URL pattern
    // In production this should query the Adoptium API

    QString os, arch;
    switch (currentPlatform()) {
    case Platform::Windows: os = "windows"; break;
    case Platform::Linux:   os = "linux"; break;
    case Platform::MacOS:   os = "mac"; break;
    default: return QString();
    }

    // ARM64 不能用 x64 包（Adoptium 的 ARM64 标识是 aarch64）
    QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == "arm64" || cpu == "aarch64")
        arch = "aarch64";
    else
        arch = is64BitSystem() ? "x64" : "x86";

    return QString("https://api.adoptium.net/v3/binary/latest/%1/ga/%2/%3/jre/hotspot/normal/eclipse")
        .arg(majorVersion).arg(os).arg(arch);
}
