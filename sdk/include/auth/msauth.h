#ifndef MLC_MSAUTH_H
#define MLC_MSAUTH_H

#include "auth/authbase.h"
#include <QNetworkAccessManager>
#include <QTimer>

/**
 * Microsoft OAuth login (Device Code Flow).
 *
 * Flow:
 * 1. Request device code from Microsoft
 * 2. User opens URL and enters code in browser
 * 3. Poll for token
 * 4. Exchange Microsoft token for Xbox Live token
 * 5. Exchange XBL token for XSTS token
 * 6. Exchange XSTS token for Minecraft token
 * 7. Get Minecraft profile
 */
class MLCCORE_EXPORT MsAuth : public AuthBase
{
    Q_OBJECT

public:
    MsAuth();

    void login(Callback onComplete) override;
    void cancel() override;
    LoginType loginType() const override { return LoginType::Ms; }

    /// Start login, results via deviceCodeReady + loginFinished signals
    Q_INVOKABLE void startLogin() { login(nullptr); }

signals:
    /// Emitted when user needs to open a URL and enter a code
    void deviceCodeReady(const QString &userCode, const QString &verificationUrl);

private:
    void requestDeviceCode();
    void pollForToken(const QString &deviceCode, int intervalSeconds);
    void authenticateWithXbl(const QString &accessToken);
    void authenticateWithXsts(const QString &xblToken);
    void authenticateWithMinecraft(const QString &xstsToken, const QString &userHash);
    void getMinecraftProfile(const QString &mcAccessToken);

    /// Emit loginFinished and invoke the callback (if set) with the same result.
    /// 保证每个会话恰好回调一次：内部先取走 callback 并标记会话结束。
    void finishLogin(bool success, const LoginResult &result);

    QNetworkAccessManager *m_nam = nullptr;
    Callback m_callback;
    QTimer *m_pollTimer = nullptr;
    int m_pollRetries = 0;
    bool m_cancelled = false;
    /// 会话序号：每次 login() 自增。所有异步回调校验自己所属会话，
    /// 防止旧会话（已取消/被新登录顶掉）污染新会话状态。
    quint64 m_session = 0;
    /// 当前会话是否仍有效（false = 已终结或已被取消）
    bool m_active = false;
    /// 设备码绝对截止时间（毫秒时间戳，来自 devicecode 的 expires_in；0 = 未设置）
    qint64 m_deadlineMs = 0;
};

#endif // MLC_MSAUTH_H
