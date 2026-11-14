#include "auth/offlineauth.h"

#include <QUuid>
#include <QLoggingCategory>

static Q_LOGGING_CATEGORY(logAuth, "mlc.auth.offline")

OfflineAuth::OfflineAuth(const QString &username)
    : m_username(username) {
}

void OfflineAuth::login(Callback onComplete) {
    if (m_username.trimmed().isEmpty()) {
        emit loginFinished(false, LoginResult());
        if (onComplete) onComplete(false, LoginResult());
        return;
    }

    LoginResult result = createOfflineLogin(m_username);

    qCInfo(logAuth) << "Offline login for:" << result.name << "uuid:" << result.uuid;

    emit loginFinished(true, result);
    if (onComplete) onComplete(true, result);
}

QString OfflineAuth::generateOfflineUuid(const QString &username) {
    // Mojang offline UUID = MD5("OfflinePlayer:" + username)
    // Formatted as UUID with version 3 variant
    QString input = "OfflinePlayer:" + username;
    QByteArray hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Md5);

    // Set UUID version to 3 and variant to 8/9/a/b
    hash[6] = (hash[6] & 0x0f) | 0x30; // version 3
    hash[8] = (hash[8] & 0x3f) | 0x80; // variant

    // Format as UUID: 8-4-4-4-12
    QString uuid;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) uuid += '-';
        uuid += QString("%1").arg((unsigned char)hash[i], 2, 16, QChar('0'));
    }

    return uuid;
}

QString OfflineAuth::generateClientToken() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString OfflineAuth::skinSexFromUuid(const QString &uuid) {
    // McSkinSex：UUID 第 7/15/23/31 位十六进制 XOR 后 mod 2（同 PCL ModMinecraft.vb:1765）
    QString u = uuid;
    u.remove('-');
    if (u.length() != 32) return "Steve";
    auto nib = [&](int i) { return QString(u[i]).toInt(nullptr, 16); };
    return ((nib(7) ^ nib(15) ^ nib(23) ^ nib(31)) % 2) ? "Alex" : "Steve";
}

// 递增 UUID 末 5 位直到默认皮肤为目标性别（同 PCL McLoginLegacyUuidWithCustomSkin）
static QString uuidForSkinSex(const QString &uuid, const QString &targetSex) {
    QString u = uuid;
    u.remove('-');
    if (u.length() != 32) return uuid;
    while (OfflineAuth::skinSexFromUuid(u) != targetSex) {
        bool ok = false;
        ulong tail = u.mid(27).toULong(&ok, 16);
        if (!ok) tail = 0;
        tail = (tail + 1) & 0xFFFFF;
        u = u.left(27) + QString("%1").arg(tail, 5, 16, QChar('0'));
    }
    return u.left(8) + '-' + u.mid(8, 4) + '-' + u.mid(12, 4) + '-' + u.mid(16, 4) + '-' + u.mid(20);
}

LoginResult OfflineAuth::createOfflineLogin(const QString &username, const QString &skinType) {
    LoginResult result;
    result.name = username.trimmed();
    result.uuid = generateOfflineUuid(result.name);
    // 离线皮肤：按类型迭代 UUID 到目标默认皮肤（PCL SkinType 1=Steve 2=Alex 4=按 slim 设置）
    QString target;
    if (skinType.compare("slim", Qt::CaseInsensitive) == 0 ||
        skinType.compare("alex", Qt::CaseInsensitive) == 0)
        target = "Alex";
    else if (skinType.compare("wide", Qt::CaseInsensitive) == 0 ||
             skinType.compare("steve", Qt::CaseInsensitive) == 0)
        target = "Steve";
    if (!target.isEmpty())
        result.uuid = uuidForSkinSex(result.uuid, target);
    result.accessToken = "0"; // Offline mode doesn't need a real token
    result.type = "Legacy";
    result.clientToken = generateClientToken();
    return result;
}
