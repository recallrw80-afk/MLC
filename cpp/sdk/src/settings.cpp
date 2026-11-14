#include "core/settings.h"
#include "util/crypto_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QLoggingCategory>
#include <QUuid>

static Q_LOGGING_CATEGORY(logSettings, "mlc.settings")

Settings& Settings::instance() {
    static Settings s;
    return s;
}

Settings::Settings() {
}

Settings::~Settings() {
    if (m_settings) {
        m_settings->sync();
        delete m_settings;
    }
}

void Settings::initialize(const QString &configPath) {
    auto &s = instance();
    if (s.m_settings) return; // Already initialized

    QString path = configPath;
    if (path.isEmpty()) {
        // Use app data directory
        path = QCoreApplication::applicationDirPath() + "/MLC.ini";
    }

    // Ensure directory exists
    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    s.m_settings = new QSettings(path, QSettings::IniFormat);
    // 设置组名
    s.m_settings->beginGroup("MLC");
    s.initDefaults();

    qCDebug(logSettings) << "Settings initialized at" << path;
}

void Settings::initDefaults() {
    QMutexLocker locker(&m_mutex);
    // 默认游戏目录：可执行文件旁边的 mc/ 目录
    if (!m_settings->contains("LaunchFolderSelect")) {
        QString defaultMc = QCoreApplication::applicationDirPath() + "/mc/";
        m_settings->setValue("LaunchFolderSelect", defaultMc);
    }
    if (!m_settings->contains("LoginType")) {
        m_settings->setValue("LoginType", static_cast<int>(0)); // Default: Legacy (offline)
    }
    if (!m_settings->contains("LaunchArgumentWindowType")) {
        m_settings->setValue("LaunchArgumentWindowType", 1); // Windowed
    }
    if (!m_settings->contains("LaunchArgumentPriority")) {
        m_settings->setValue("LaunchArgumentPriority", 1); // Normal
    }
    if (!m_settings->contains("LaunchArgumentRam")) {
        m_settings->setValue("LaunchArgumentRam", true);
    }
    if (!m_settings->contains("VersionRamOptimize")) {
        m_settings->setValue("VersionRamOptimize", 0); // Global setting
    }
    if (!m_settings->contains("LaunchAdvanceGC")) {
        m_settings->setValue("LaunchAdvanceGC", 0); // Auto
    }
    if (!m_settings->contains("SystemLaunchCount")) {
        m_settings->setValue("SystemLaunchCount", 0);
    }

    // 默认玩家配置（首次运行时无任何 Profile 则自动创建）
    m_settings->beginGroup("Profile");
    bool hasProfiles = !m_settings->childGroups().isEmpty();
    m_settings->endGroup();
    if (!hasProfiles) {
        QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_settings->setValue("Profile/" + uuid + "/Name", "Player");
        m_settings->setValue("Profile/" + uuid + "/SkinType", "slim");
        m_settings->setValue("SelectedPlayer", uuid);
    }
}

// ---- Convenience methods ----

bool Settings::getBool(const QString &key, bool defaultValue) const
{
    return get<bool>(key, defaultValue);
}

void Settings::setBool(const QString &key, bool value) {
    set<bool>(key, value);
}

int Settings::getInt(const QString &key, int defaultValue) const
{
    return get<int>(key, defaultValue);
}

void Settings::setInt(const QString &key, int value) {
    set<int>(key, value);
}

QString Settings::getString(const QString &key, const QString &defaultValue) const
{
    return get<QString>(key, defaultValue);
}

void Settings::setString(const QString &key, const QString &value) {
    set<QString>(key, value);
}

void Settings::removeKey(const QString &key) {
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return;
    m_settings->remove(key);
    m_settings->sync();
}

QVariant Settings::value(const QString &key, const QVariant &defaultValue) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return defaultValue;
    return m_settings->value(key, defaultValue);
}

void Settings::setValue(const QString &key, const QVariant &value) {
    m_mutex.lock();
    if (m_settings) {
        m_settings->setValue(key, value);
        m_settings->sync();
    }
    m_mutex.unlock();
    // 信号在锁外发：槽函数可能回读 Settings（递归锁可重入，但锁外更不易死锁）
    emit settingChanged(key);
}

// ---- Encrypted settings ----

QString Settings::getEncrypted(const QString &key, const QString &defaultValue) const
{
    QString raw = get<QString>(key);
    if (raw.isEmpty()) return defaultValue;
    return CryptoUtils::pclDecrypt(raw);
}

void Settings::setEncrypted(const QString &key, const QString &value) {
    set<QString>(key, CryptoUtils::pclEncrypt(value));
}

// ---- Instance isolation ----

QString Settings::getInstance(const QString &instanceId, const QString &key,
                                const QString &defaultValue) const
{
    if (instanceId.isEmpty()) return getString(key, defaultValue);
    QString instanceKey = QString("Instance_%1/%2").arg(instanceId, key);
    QString val = getString(instanceKey);
    return val.isEmpty() ? defaultValue : val;
}

void Settings::setInstance(const QString &instanceId, const QString &key, const QString &value) {
    if (instanceId.isEmpty()) {
        setString(key, value);
    } else {
        QString instanceKey = QString("Instance_%1/%2").arg(instanceId, key);
        setString(instanceKey, value);
    }
}

QString Settings::instancePath(const QString &instanceId) const
{
    QString base = getString("LaunchFolderSelect");
    if (base.isEmpty()) base = QDir::homePath() + "/.minecraft/";
    // LaunchFolderSelect 存的是用户原始输入，不保证尾斜杠——拼接前必须规范化
    if (!base.endsWith('/')) base += '/';
    if (instanceId.isEmpty()) return base;
    // Per-instance path: base .minecraft / versions / instanceId
    return base + "versions/" + instanceId + "/";
}

// ---- Player profiles ----

QStringList Settings::playerProfiles() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return {};
    m_settings->beginGroup("Profile");
    QStringList uuids = m_settings->childGroups();
    m_settings->endGroup();
    return uuids;
}

QString Settings::getProfile(const QString &uuid, const QString &key,
                             const QString &defaultValue) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings || uuid.isEmpty()) return defaultValue;
    QString fullKey = QString("Profile/%1/%2").arg(uuid, key);
    return m_settings->value(fullKey, defaultValue).toString();
}

void Settings::setProfile(const QString &uuid, const QString &key, const QString &value)
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings || uuid.isEmpty()) return;
    QString fullKey = QString("Profile/%1/%2").arg(uuid, key);
    m_settings->setValue(fullKey, value);
    m_settings->sync();
}

void Settings::removeProfile(const QString &uuid)
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings || uuid.isEmpty()) return;
    m_settings->beginGroup("Profile");
    m_settings->remove(uuid);
    m_settings->endGroup();
    m_settings->sync();
}

QString Settings::selectedPlayer() const
{
    return getString("SelectedPlayer");
}

void Settings::selectPlayer(const QString &uuid)
{
    setString("SelectedPlayer", uuid);
}

// ---- Instance directory mappings ----

void Settings::setInstanceDir(const QString &dirName, const QString &displayName)
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return;
    m_settings->beginGroup("Instances");
    m_settings->setValue(dirName, displayName);
    m_settings->endGroup();
    m_settings->sync();
}

QMap<QString, QString> Settings::instanceDirs() const
{
    QMutexLocker locker(&m_mutex);
    QMap<QString, QString> result;
    if (!m_settings) return result;
    m_settings->beginGroup("Instances");
    const auto keys = m_settings->childKeys();
    for (const auto &key : keys)
        result[key] = m_settings->value(key).toString();
    m_settings->endGroup();
    return result;
}

void Settings::removeInstanceDir(const QString &dirName)
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) return;
    m_settings->beginGroup("Instances");
    m_settings->remove(dirName);
    m_settings->endGroup();
    m_settings->sync();
}

QString Settings::dirForDisplayName(const QString &displayName) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings || displayName.isEmpty()) return {};
    m_settings->beginGroup("Instances");
    const auto keys = m_settings->childKeys();
    for (const auto &key : keys) {
        if (m_settings->value(key).toString() == displayName) {
            m_settings->endGroup();
            return key;
        }
    }
    m_settings->endGroup();
    return {};
}
