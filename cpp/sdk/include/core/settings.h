#ifndef MLC_SETTINGS_H
#define MLC_SETTINGS_H

#include "core/mlccore_export.h"
#include <QObject>
#include <QSettings>
#include <QVariant>
#include <QString>
#include <QMutex>
#include <QRecursiveMutex>

/**
 * Cross-platform settings manager.
 * Mirrors the original VB Settings.Get(Of T)(key) / Settings.Set(key, value) API.
 *
 * Uses QSettings (INI file on Linux/Mac, registry on Windows).
 * Stored in <app data>/MLC.ini on all platforms for consistency.
 *
 * 线程安全：所有直接访问 m_settings 的公开方法持有 m_mutex（递归锁——
 * 便捷方法会嵌套调用模板方法，如 getEncrypted → get<QString>）。
 */
class MLCCORE_EXPORT Settings : public QObject
{
    Q_OBJECT

public:
    static Settings& instance();
    static void initialize(const QString &configPath = QString());

    // Typed getters
    template<typename T>
    T get(const QString &key, const T &defaultValue = T()) const {
        QMutexLocker locker(&m_mutex);
        if (!m_settings) return defaultValue;
        QVariant v = m_settings->value(key);
        if (!v.isValid() || v.isNull()) return defaultValue;
        return v.value<T>();
    }

    // Typed setter
    template<typename T>
    void set(const QString &key, const T &value) {
        QMutexLocker locker(&m_mutex);
        if (!m_settings) return;
        m_settings->setValue(key, QVariant::fromValue(value));
        m_settings->sync();
    }

    // Bool convenience
    bool getBool(const QString &key, bool defaultValue = false) const;
    void setBool(const QString &key, bool value);

    // Int convenience
    int getInt(const QString &key, int defaultValue = 0) const;
    void setInt(const QString &key, int value);

    // String convenience
    QString getString(const QString &key, const QString &defaultValue = QString()) const;
    void setString(const QString &key, const QString &value);

    /// 删除键（整个条目从 ini 移除，区别于 setString(key,"") 留空行）
    void removeKey(const QString &key);

    // Direct QVariant access（QML 可调用：Settings.value(key, default) 读、Settings.setValue(key, value) 写并触发 settingChanged）
    Q_INVOKABLE QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);

    // Encrypted settings (DES via CryptoUtils::pclEncrypt/pclDecrypt)
    QString getEncrypted(const QString &key, const QString &defaultValue = QString()) const;
    void setEncrypted(const QString &key, const QString &value);

    // Instance isolation (per-version .minecraft)
    QString getInstance(const QString &instanceId, const QString &key,
                        const QString &defaultValue = QString()) const;
    void setInstance(const QString &instanceId, const QString &key, const QString &value);
    QString instancePath(const QString &instanceId) const;

    // ---- Player profiles (multi-user, UUID-based) ----

    /// 列出所有玩家配置的 UUID
    QStringList playerProfiles() const;

    /// 读取玩家配置字段
    QString getProfile(const QString &uuid, const QString &key,
                       const QString &defaultValue = QString()) const;

    /// 写入玩家配置字段
    void setProfile(const QString &uuid, const QString &key, const QString &value);

    /// 删除一个玩家配置
    void removeProfile(const QString &uuid);

    /// 当前选中的玩家 UUID
    QString selectedPlayer() const;
    void selectPlayer(const QString &uuid);

    // ---- Instance directory mappings (随机名 → 实例名) ----

    /// 写入实例目录映射：dirName → displayName（存储到 MLC.ini 的 [MLC] 节）
    void setInstanceDir(const QString &dirName, const QString &displayName);

    /// 读取全部实例映射：dirName → displayName
    QMap<QString, QString> instanceDirs() const;

    /// 删除一个实例映射
    void removeInstanceDir(const QString &dirName);

    /// 通过显示名反向查找目录名（找不到返回空字符串）
    QString dirForDisplayName(const QString &displayName) const;

    // Initialize defaults
    void initDefaults();

signals:
    void settingChanged(const QString &key);

private:
    Settings();
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    QSettings *m_settings = nullptr;
    mutable QRecursiveMutex m_mutex;  // 保护 m_settings 的一切访问（含头文件模板方法）
};

// Template specializations for common types
template<>
inline QString Settings::get<QString>(const QString &key, const QString &defaultValue) const {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return defaultValue;
    return m_settings->value(key, defaultValue).toString();
}

template<>
inline int Settings::get<int>(const QString &key, const int &defaultValue) const {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return defaultValue;
    return m_settings->value(key, defaultValue).toInt();
}

template<>
inline bool Settings::get<bool>(const QString &key, const bool &defaultValue) const {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return defaultValue;
    return m_settings->value(key, defaultValue).toBool();
}

template<>
inline void Settings::set<QString>(const QString &key, const QString &value) {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return;
    m_settings->setValue(key, value);
    m_settings->sync();
}

template<>
inline void Settings::set<int>(const QString &key, const int &value) {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return;
    m_settings->setValue(key, value);
    m_settings->sync();
}

template<>
inline void Settings::set<bool>(const QString &key, const bool &value) {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return;
    m_settings->setValue(key, value);
    m_settings->sync();
}

#endif // MLC_SETTINGS_H
