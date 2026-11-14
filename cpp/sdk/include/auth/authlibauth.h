#ifndef MLC_AUTHLIBAUTH_H
#define MLC_AUTHLIBAUTH_H

#include "auth/authbase.h"
#include <QNetworkAccessManager>

/**
 * Authlib-Injector / Nide8 (Unified Pass) login.
 * Both use a third-party authentication server with a custom Yggdrasil API.
 */
class MLCCORE_EXPORT AuthlibAuth : public AuthBase
{
    Q_OBJECT

public:
    enum class ServerType {
        AuthlibInjector,  // authlib-injector based
        Nide8             // Nide8 / Unified Pass
    };

    AuthlibAuth(ServerType type = ServerType::AuthlibInjector);

    void login(Callback onComplete) override;
    void cancel() override;
    LoginType loginType() const override;

    void setServerUrl(const QString &url) { m_serverUrl = url; }
    void setUsername(const QString &username) { m_username = username; }
    void setPassword(const QString &password) { m_password = password; }

    QString serverUrl() const { return m_serverUrl; }

    /// 已有 token 续期：POST {server}/authserver/refresh（accessToken 过期后免密换新）
    /// 成功后 LoginResult 带新 accessToken（clientToken 不变），调用方须重新持久化
    void setTokens(const QString &accessToken, const QString &clientToken) {
        m_accessToken = accessToken;
        m_clientToken = clientToken;
    }
    void refresh(Callback onComplete);

private:
    void doAuthlibLogin(Callback onComplete);
    void doNideLogin(Callback onComplete);

    /// Emit loginFinished and invoke the callback (if set) with the same result.
    void finishLogin(bool success, const LoginResult &result, const Callback &cb);

    ServerType m_type;
    QString m_serverUrl;
    QString m_username;
    QString m_password;
    QString m_accessToken;
    QString m_clientToken;

    QNetworkAccessManager *m_nam = nullptr;
    bool m_cancelled = false;
    Callback m_callback = nullptr;  // cancel() 时也需要回调（与 MsAuth 行为一致）
};

#endif // MLC_AUTHLIBAUTH_H
