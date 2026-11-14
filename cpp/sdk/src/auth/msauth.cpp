#include "auth/msauth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QLoggingCategory>
#include <QDateTime>

static Q_LOGGING_CATEGORY(logMs, "mlc.auth.ms")

// Microsoft OAuth constants
static const QString CLIENT_ID = "00000000402b5328"; // Official Minecraft launcher client ID
static const QString DEVICE_CODE_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
static const QString TOKEN_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
static const QString XBL_AUTH_URL = "https://user.auth.xboxlive.com/user/authenticate";
static const QString XSTS_AUTH_URL = "https://xsts.auth.xboxlive.com/xsts/authorize";
static const QString MC_AUTH_URL = "https://api.minecraftservices.com/authentication/login_with_xbox";
static const QString MC_PROFILE_URL = "https://api.minecraftservices.com/minecraft/profile";

MsAuth::MsAuth() {
    m_nam = new QNetworkAccessManager(this);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(true);
}

void MsAuth::login(Callback onComplete) {
    // 重入保护：先让旧会话走一次性出口收到"已取消"，再开新会话。
    // 旧链在途的 reply 回调靠 m_session 序号校验丢弃，不会污染新会话。
    if (m_active) {
        qCWarning(logMs) << "login() called while a session is active, cancelling it first";
        if (m_pollTimer) m_pollTimer->stop();
        finishLogin(false, LoginResult());   // 旧 callback 收到 false（回调契约：取消也要回调）
    }
    ++m_session;              // 新会话号；旧 lambda 捕获的序号不再匹配
    m_callback = onComplete;
    m_cancelled = false;
    m_active = true;
    m_pollRetries = 0;
    m_deadlineMs = 0;
    emit loginProgress("Requesting device code...");
    requestDeviceCode();
}

void MsAuth::cancel() {
    // 只有活动会话才能取消：空闲/已完成的 cancel 不得再回调一次
    if (!m_active) return;
    // 不要在这里提前清 m_active —— 交给 finishLogin 的统一出口处理，
    // 否则 finishLogin 的一次性守卫会被绕过
    m_cancelled = true;
    if (m_pollTimer) m_pollTimer->stop();
    finishLogin(false, LoginResult());
}

void MsAuth::finishLogin(bool success, const LoginResult &result) {
    // 恰好一次出口：会话已结束则静默丢弃（不能被重复取消/迟到回调触发第二次）
    if (!m_active) return;
    m_active = false;
    m_cancelled = true;                     // 让所有在途回调失效
    auto cb = m_callback;
    m_callback = nullptr;
    emit loginFinished(success, result);
    if (cb) cb(success, result);
}

void MsAuth::requestDeviceCode() {
    QNetworkRequest request(DEVICE_CODE_URL);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QByteArray data = QString("client_id=%1&scope=XboxLive.signin%20offline_access").arg(CLIENT_ID).toUtf8();

    const quint64 session = m_session;
    QNetworkReply *reply = m_nam->post(request, data);
    connect(reply, &QNetworkReply::finished, this, [this, reply, session]() {
        reply->deleteLater();
        if (session != m_session || !m_active) return;   // 旧会话/已取消，丢弃

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(logMs) << "Device code request failed:" << reply->errorString();
            finishLogin(false, LoginResult());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        QString userCode = root.value("user_code").toString();
        QString deviceCode = root.value("device_code").toString();
        QString verificationUrl = root.value("verification_uri").toString();
        int interval = root.value("interval").toInt(5);
        const int expiresIn = root.value("expires_in").toInt(900);

        if (userCode.isEmpty() || deviceCode.isEmpty()) {
            qCWarning(logMs) << "Invalid device code response";
            finishLogin(false, LoginResult());
            return;
        }

        qCInfo(logMs) << "Device code:" << userCode << "URL:" << verificationUrl;
        emit deviceCodeReady(userCode, verificationUrl);
        emit loginProgress(QString("Open %1 and enter code: %2").arg(verificationUrl, userCode));

        // 首次轮询也要等满服务器给的 interval（RFC 8628），否则会立刻
        // 撞 authorization_pending / 触发 slow_down
        const QString dc = deviceCode;
        m_pollTimer->stop();
        m_pollTimer->disconnect();
        m_deadlineMs = QDateTime::currentMSecsSinceEpoch() + qint64(qMax(60, expiresIn)) * 1000;
        connect(m_pollTimer, &QTimer::timeout, this, [this, dc, interval, session]() {
            if (session != m_session || !m_active) return;
            pollForToken(dc, interval);
        });
        m_pollTimer->start(qMax(1, interval) * 1000);
    });
}

void MsAuth::pollForToken(const QString &deviceCode, int intervalSeconds) {
    if (!m_active || m_cancelled) return;

    QNetworkRequest request(TOKEN_URL);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QByteArray data = QString("grant_type=urn:ietf:params:oauth:grant-type:device_code"
                              "&client_id=%1&device_code=%2")
                          .arg(CLIENT_ID, deviceCode).toUtf8();

    const quint64 session = m_session;
    QNetworkReply *reply = m_nam->post(request, data);
    connect(reply, &QNetworkReply::finished, this, [this, reply, deviceCode, intervalSeconds, session]() {
        reply->deleteLater();
        if (session != m_session || !m_active) return;   // 旧会话/已取消，丢弃

        // 用成员定时器调度下一轮（cancel() 可停止；裸 QTimer::singleShot 无法取消）
        auto scheduleNext = [this, deviceCode, session](int nextInterval) {
            m_pollTimer->stop();
            m_pollTimer->disconnect();
            connect(m_pollTimer, &QTimer::timeout, this, [this, deviceCode, nextInterval, session]() {
                if (session != m_session || !m_active) return;
                pollForToken(deviceCode, nextInterval);
            });
            // 不超过设备码剩余有效期
            qint64 delayMs = qint64(qMax(1, nextInterval)) * 1000;
            if (m_deadlineMs > 0) {
                const qint64 left = m_deadlineMs - QDateTime::currentMSecsSinceEpoch();
                if (left > 0) delayMs = qMin(delayMs, left);
            }
            m_pollTimer->start(int(qMax<qint64>(1, delayMs)));
        };

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        QString error = root.value("error").toString();
        if (error == "authorization_pending") {
            // 设备码有效期（expires_in）是绝对截止时间：slow_down 只拉长间隔，
            // 不应因此延长总期限。没给 expires_in 时按 15 分钟兜底。
            if (m_deadlineMs <= 0)
                m_deadlineMs = QDateTime::currentMSecsSinceEpoch() + 15 * 60 * 1000;
            if (QDateTime::currentMSecsSinceEpoch() >= m_deadlineMs) {
                qCWarning(logMs) << "Device code expired, polling timed out";
                finishLogin(false, LoginResult());
                return;
            }
            emit loginProgress("Waiting for approval...");
            scheduleNext(intervalSeconds);
            return;
        }

        if (error == "slow_down") {
            // RFC 8628：限流时轮询间隔 +5s 继续，不是致命错误
            if (m_deadlineMs > 0 && QDateTime::currentMSecsSinceEpoch() >= m_deadlineMs) {
                qCWarning(logMs) << "Device code expired during slow_down";
                finishLogin(false, LoginResult());
                return;
            }
            scheduleNext(intervalSeconds + 5);
            return;
        }

        if (!error.isEmpty()) {
            qCWarning(logMs) << "Token error:" << error;
            finishLogin(false, LoginResult());
            return;
        }

        QString accessToken = root.value("access_token").toString();
        if (accessToken.isEmpty()) {
            qCWarning(logMs) << "No access token in response";
            finishLogin(false, LoginResult());
            return;
        }

        qCInfo(logMs) << "Got Microsoft access token";

        emit loginProgress("Authenticating with Xbox Live...");
        authenticateWithXbl(accessToken);
    });
}

void MsAuth::authenticateWithXbl(const QString &accessToken) {
    QNetworkRequest request(XBL_AUTH_URL);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    QJsonObject props;
    props["AuthMethod"] = "RPS";
    props["SiteName"] = "user.auth.xboxlive.com";
    props["RpsTicket"] = QString("d=%1").arg(accessToken);
    body["Properties"] = props;
    body["RelyingParty"] = "http://auth.xboxlive.com";
    body["TokenType"] = "JWT";

    const quint64 session = m_session;
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, session]() {
        reply->deleteLater();
        if (session != m_session || !m_active) return;

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(logMs) << "XBL auth failed:" << reply->errorString();
            finishLogin(false, LoginResult());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QString xblToken = doc.object().value("Token").toString();

        if (xblToken.isEmpty()) {
            qCWarning(logMs) << "No XBL token";
            finishLogin(false, LoginResult());
            return;
        }

        qCInfo(logMs) << "Got XBL token";
        emit loginProgress("Authenticating with XSTS...");
        authenticateWithXsts(xblToken);
    });
}

void MsAuth::authenticateWithXsts(const QString &xblToken) {
    QNetworkRequest request(XSTS_AUTH_URL);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    QJsonObject props;
    props["SandboxId"] = "RETAIL";
    QJsonArray userTokens;
    userTokens.append(xblToken);
    props["UserTokens"] = userTokens;
    body["Properties"] = props;
    body["RelyingParty"] = "rp://api.minecraftservices.com/";
    body["TokenType"] = "JWT";

    const quint64 session = m_session;
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, session]() {
        reply->deleteLater();
        if (session != m_session || !m_active) return;

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(logMs) << "XSTS auth failed:" << reply->errorString();
            finishLogin(false, LoginResult());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        QString xstsToken = root.value("Token").toString();
        QJsonObject displayClaims = root.value("DisplayClaims").toObject();
        QJsonArray xui = displayClaims.value("xui").toArray();
        QString userHash;
        if (!xui.isEmpty()) {
            userHash = xui[0].toObject().value("uhs").toString();
        }

        if (xstsToken.isEmpty() || userHash.isEmpty()) {
            qCWarning(logMs) << "No XSTS token or user hash";
            finishLogin(false, LoginResult());
            return;
        }

        qCInfo(logMs) << "Got XSTS token";
        emit loginProgress("Authenticating with Minecraft...");
        authenticateWithMinecraft(xstsToken, userHash);
    });
}

void MsAuth::authenticateWithMinecraft(const QString &xstsToken, const QString &userHash) {
    QNetworkRequest request(MC_AUTH_URL);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["identityToken"] = QString("XBL3.0 x=%1;%2").arg(userHash, xstsToken);

    const quint64 session = m_session;
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, session]() {
        reply->deleteLater();
        if (session != m_session || !m_active) return;

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(logMs) << "MC auth failed:" << reply->errorString();
            finishLogin(false, LoginResult());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QString mcToken = doc.object().value("access_token").toString();

        if (mcToken.isEmpty()) {
            qCWarning(logMs) << "No Minecraft token";
            finishLogin(false, LoginResult());
            return;
        }

        qCInfo(logMs) << "Got Minecraft access token";
        emit loginProgress("Getting Minecraft profile...");
        getMinecraftProfile(mcToken);
    });
}

void MsAuth::getMinecraftProfile(const QString &mcAccessToken) {
    QNetworkRequest request(MC_PROFILE_URL);
    request.setRawHeader("Authorization", ("Bearer " + mcAccessToken).toUtf8());

    const quint64 session = m_session;
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mcAccessToken, session]() {
        reply->deleteLater();
        if (session != m_session || !m_active) return;

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(logMs) << "Profile request failed:" << reply->errorString();
            finishLogin(false, LoginResult());
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        LoginResult result;
        result.name = root.value("name").toString();
        result.uuid = root.value("id").toString();
        result.accessToken = mcAccessToken;
        result.type = "Ms";
        result.clientToken = ""; // Generated separately
        result.profileJson = QString::fromUtf8(doc.toJson());

        if (result.name.isEmpty() || result.uuid.isEmpty()) {
            qCWarning(logMs) << "Empty profile - need to buy Minecraft?";
            finishLogin(false, LoginResult());
            return;
        }

        qCInfo(logMs) << "Logged in as:" << result.name << "(UUID:" << result.uuid << ")";
        finishLogin(true, result);
    });
}
