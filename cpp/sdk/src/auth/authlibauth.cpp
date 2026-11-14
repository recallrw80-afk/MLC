#include "auth/authlibauth.h"
#include "auth/msauth.h"
#include "auth/offlineauth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QUuid>
#include <QLoggingCategory>

static Q_LOGGING_CATEGORY(logAl, "mlc.auth.authlib")

AuthlibAuth::AuthlibAuth(ServerType type)
    : m_type(type) {
    m_nam = new QNetworkAccessManager(this);
}

LoginType AuthlibAuth::loginType() const
{
    return m_type == ServerType::AuthlibInjector ? LoginType::Auth : LoginType::Nide;
}

void AuthlibAuth::login(Callback onComplete) {
    m_cancelled = false;
    m_callback = onComplete;

    if (m_username.trimmed().isEmpty() || m_password.isEmpty()) {
        qCWarning(logAl) << "Username or password empty";
        finishLogin(false, LoginResult(), onComplete);
        return;
    }

    if (m_type == ServerType::Nide8) {
        doNideLogin(onComplete);
    } else {
        doAuthlibLogin(onComplete);
    }
}

void AuthlibAuth::cancel() {
    m_cancelled = true;
    // 取消也必须触发完成回调——回调驱动的调用方还在等（与 MsAuth::cancel 行为一致）
    finishLogin(false, LoginResult(), m_callback);
    m_callback = nullptr;
}

void AuthlibAuth::finishLogin(bool success, const LoginResult &result, const Callback &cb) {
    emit loginFinished(success, result);
    if (cb) cb(success, result);
}

void AuthlibAuth::doAuthlibLogin(Callback onComplete) {
    // Authlib-Injector uses Yggdrasil API
    // POST {server}/authserver/authenticate
    // Body: { agent: { name: "Minecraft", version: 1 }, username, password, clientToken, requestUser: true }

    QString clientToken = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QUrl url(m_serverUrl);
    QString authUrl = m_serverUrl;
    if (!authUrl.endsWith('/')) authUrl += '/';
    authUrl += "authserver/authenticate";

    QNetworkRequest request{QUrl(authUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    QJsonObject agent;
    agent["name"] = "Minecraft";
    agent["version"] = 1;
    body["agent"] = agent;
    body["username"] = m_username;
    body["password"] = m_password;
    body["clientToken"] = clientToken;
    body["requestUser"] = true;

    emit loginProgress("Authenticating with third-party server...");

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, clientToken, onComplete]() {
        reply->deleteLater();
        if (m_cancelled) return;

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        auto failWith = [&](const QString &why) {
            LoginResult err;
            err.errorMessage = why;
            qCWarning(logAl) << "Authlib login failed:" << why;
            finishLogin(false, err, onComplete);
        };

        // 服务器业务错误（如密码错误）：HTTP 4xx + {"errorMessage": "..."}
        if (reply->error() != QNetworkReply::NoError) {
            QString why = root.value("errorMessage").toString();
            if (why.isEmpty()) why = reply->errorString();
            failWith(why);
            return;
        }

        // 无 selectedProfile 时取 availableProfiles 首个（多角色账号的简化处理）
        QJsonObject profile = root.value("selectedProfile").toObject();
        if (profile.isEmpty()) {
            auto arr = root.value("availableProfiles").toArray();
            if (!arr.isEmpty()) profile = arr.first().toObject();
        }

        LoginResult result;
        result.name = profile.value("name").toString();
        result.uuid = profile.value("id").toString();
        result.accessToken = root.value("accessToken").toString();
        result.type = "Auth";
        result.clientToken = clientToken;
        result.serverUrl = m_serverUrl;

        if (result.name.isEmpty() || result.accessToken.isEmpty()) {
            failWith(root.value("errorMessage").toString("响应缺少角色或 accessToken"));
            return;
        }

        qCInfo(logAl) << "Authlib login success:" << result.name;
        finishLogin(true, result, onComplete);
    });
}

void AuthlibAuth::refresh(Callback onComplete) {
    m_cancelled = false;
    m_callback = onComplete;
    if (m_serverUrl.isEmpty() || m_accessToken.isEmpty()) {
        finishLogin(false, LoginResult(), onComplete);
        return;
    }

    QString url = m_serverUrl;
    if (!url.endsWith('/')) url += '/';
    url += "authserver/refresh";

    QJsonObject body;
    body["accessToken"] = m_accessToken;
    if (!m_clientToken.isEmpty()) body["clientToken"] = m_clientToken;
    body["requestUser"] = true;

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    emit loginProgress("Refreshing third-party token...");

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, onComplete]() {
        reply->deleteLater();
        if (m_cancelled) return;

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        auto failWith = [&](const QString &why) {
            LoginResult err;
            err.errorMessage = why;
            qCWarning(logAl) << "Authlib refresh failed:" << why;
            finishLogin(false, err, onComplete);
        };

        if (reply->error() != QNetworkReply::NoError) {
            QString why = root.value("errorMessage").toString();
            if (why.isEmpty()) why = reply->errorString();
            failWith(why);
            return;
        }

        QString newToken = root.value("accessToken").toString();
        if (newToken.isEmpty()) {
            failWith(root.value("errorMessage").toString("响应缺少 accessToken"));
            return;
        }

        LoginResult result;
        result.accessToken = newToken;
        result.clientToken = m_clientToken;  //  refresh 不换 clientToken
        // refresh 响应可带 selectedProfile；不带则由调用方沿用本地保存的 name/uuid
        QJsonObject profile = root.value("selectedProfile").toObject();
        result.name = profile.value("name").toString();
        result.uuid = profile.value("id").toString();
        result.type = m_type == ServerType::Nide8 ? "Nide" : "Auth";
        result.serverUrl = m_serverUrl;
        finishLogin(true, result, onComplete);
    });
}

void AuthlibAuth::doNideLogin(Callback onComplete) {
    // Nide8 uses a simpler API
    // POST {server}/api/yggdrasil/authserver/authenticate
    // Similar to Authlib but at a different endpoint

    QString clientToken = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QString authUrl = m_serverUrl;
    if (!authUrl.endsWith('/')) authUrl += '/';
    authUrl += "api/yggdrasil/authserver/authenticate";

    QNetworkRequest request{QUrl(authUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    QJsonObject agent;
    agent["name"] = "Minecraft";
    agent["version"] = 1;
    body["agent"] = agent;
    body["username"] = m_username;
    body["password"] = m_password;
    body["clientToken"] = clientToken;
    body["requestUser"] = true;

    emit loginProgress("Authenticating with Nide8 server...");

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, clientToken, onComplete]() {
        reply->deleteLater();
        if (m_cancelled) return;

        if (reply->error() != QNetworkReply::NoError) {
            QString errMsg = QString::fromUtf8(reply->readAll());
            qCWarning(logAl) << "Nide8 auth failed:" << reply->errorString() << errMsg;
            finishLogin(false, LoginResult(), onComplete);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject root = doc.object();

        LoginResult result;
        result.name = root.value("selectedProfile").toObject().value("name").toString();
        result.uuid = root.value("selectedProfile").toObject().value("id").toString();
        result.accessToken = root.value("accessToken").toString();
        result.type = "Nide";
        result.clientToken = clientToken;

        if (result.name.isEmpty() || result.accessToken.isEmpty()) {
            QString err = root.value("errorMessage").toString("Unknown error");
            qCWarning(logAl) << "Nide8 login failed:" << err;
            finishLogin(false, LoginResult(), onComplete);
            return;
        }

        qCInfo(logAl) << "Nide8 login success:" << result.name;
        finishLogin(true, result, onComplete);
    });
}

// AuthBase factory

AuthBase* AuthBase::create(LoginType type) {
    switch (type) {
    case LoginType::Legacy:
        return new OfflineAuth();
    case LoginType::Ms:
        return new MsAuth();
    case LoginType::Auth:
        return new AuthlibAuth(AuthlibAuth::ServerType::AuthlibInjector);
    case LoginType::Nide:
        return new AuthlibAuth(AuthlibAuth::ServerType::Nide8);
    default:
        return nullptr;
    }
}
