#ifndef MLC_OFFLINEAUTH_H
#define MLC_OFFLINEAUTH_H

#include "auth/authbase.h"
#include <QCryptographicHash>

/**
 * Offline / legacy login.
 * Generates a UUID (v3 offline) from the username.
 */
class MLCCORE_EXPORT OfflineAuth : public AuthBase
{
    Q_OBJECT

public:
    OfflineAuth(const QString &username = QString());

    void login(Callback onComplete) override;
    void cancel() override {}
    LoginType loginType() const override { return LoginType::Legacy; }

    void setUsername(const QString &username) { m_username = username; }
    QString username() const { return m_username; }

    /// Generate offline UUID from username (matching the Mojang algorithm)
    Q_INVOKABLE static QString generateOfflineUuid(const QString &username);

    /// Generate a random client token
    static QString generateClientToken();

    /// UUID 默认皮肤判定（同 PCL McSkinSex）：第 7/15/23/31 位 XOR 后 mod 2，奇=Alex 偶=Steve
    Q_INVOKABLE static QString skinSexFromUuid(const QString &uuid);

    /// Build a complete offline LoginResult for a username.
    /// skinType: "default"（随机）/ "slim"|"alex"（迭代到 Alex）/ "wide"|"steve"（迭代到 Steve）
    /// Used by the launch page to supply credentials to Launcher.launchVersion().
    Q_INVOKABLE static LoginResult createOfflineLogin(const QString &username,
                                                       const QString &skinType = "default");

private:
    QString m_username;
};

#endif // MLC_OFFLINEAUTH_H
