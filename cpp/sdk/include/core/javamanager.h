#ifndef MLC_JAVAMANAGER_H
#define MLC_JAVAMANAGER_H

#include <QObject>
#include <QList>
#include <QVariantList>
#include <QVersionNumber>
#include <QMutex>
#include <atomic>
#include "core/types.h"
#include "core/mlccore_export.h"

/**
 * Cross-platform Java detection, selection, and compatibility matching.
 * Mirrors the original ModJava + Java selection logic from ModLaunch.
 */
class MLCCORE_EXPORT JavaManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList javaNames READ javaNames NOTIFY javaListChanged)
    Q_PROPERTY(int javaCount READ javaCount NOTIFY javaListChanged)
    Q_PROPERTY(bool isScanning READ isScanning NOTIFY scanningChanged)
    Q_PROPERTY(QString selectedJava READ selectedJavaName NOTIFY selectedJavaChanged)

public:
    static JavaManager& instance();

    // ---- Scanning ----

    /// Scan system for all Java installations (async)
    Q_INVOKABLE void scanSystemJava();

    /// 同步等待进行中的扫描结束（CLI 等需要立即读取结果的场景）
    void waitForScanFinished();

    /// Scan a specific folder for java
    Q_INVOKABLE void scanFolder(const QString &folder, bool isUserImport = false);

    /// Check a specific Java installation (runs java -version)
    /// Returns true on success, sets error string on failure
    bool checkJava(JavaEntry &entry);

    /// Check a list of Java entries, removing invalid ones
    QList<JavaEntry> checkJavaList(const QList<JavaEntry> &list);

    // ---- Selection ----

    /// Find the best Java matching version constraints
    /// 返回拷贝而非内部指针（扫描线程并发重分配安全）；未找到返回空 entry（pathJava 为空）
    JavaEntry selectJava(const QVersionNumber &minVersion = QVersionNumber(),
                         const QVersionNumber &maxVersion = QVersionNumber());

    /// Auto-detect and select Java for a Minecraft version
    JavaEntry selectJavaForVersion(const McVersion &version);

    // ---- Accessors ----

    /// 锁内按值拷贝返回（扫描线程并发写 m_javaList，返回引用会 data race）
    QList<JavaEntry> javaList() const { QMutexLocker lock(&m_mutex); return m_javaList; }
    QStringList javaNames() const;
    /// QML 用：锁内按值拷贝的 Java 列表
    /// [{pathJava, pathFolder, display, majorVersion, is64Bit, isUserImport}]
    Q_INVOKABLE QVariantList javaVariantList() const;
    int javaCount() const;
    bool isScanning() const { return m_isScanning; }
    QString selectedJavaName() const;

    // ---- Download ----

    /// Get download URL for a Java major version
    QString getJavaDownloadUrl(int majorVersion) const;

    // ---- Static utilities ----

    /// Parse Java version from -version output string
    static QVersionNumber parseJavaVersionOutput(const QString &output);

    /// Get Java version compatibility range for a Minecraft version
    static void getJavaCompatibilityRange(const McVersion &version,
                                          QVersionNumber &outMin,
                                          QVersionNumber &outMax);

signals:
    void javaListChanged();
    void scanningChanged();
    void selectedJavaChanged();
    void javaScanProgress(const QString &status);
    void javaCheckFailed(const QString &path, const QString &error);

private:
    JavaManager() = default;

    static QStringList javaSearchPaths();
    void scanPathVariable();
    void scanJavaHome();
    bool isJavaBinary(const QString &path) const;

    /// Append a Java entry if its pathFolder isn't already in m_javaList.
    /// Thread-safe (locks m_mutex). Returns true if added.
    bool addJavaEntry(const JavaEntry &entry);

    QList<JavaEntry> m_javaList;
    JavaEntry *m_selectedJava = nullptr;
    std::atomic<bool> m_isScanning{false};
    mutable QMutex m_mutex;
};

#endif // MLC_JAVAMANAGER_H
