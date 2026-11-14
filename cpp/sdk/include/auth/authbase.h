#ifndef MLC_AUTHBASE_H
#define MLC_AUTHBASE_H

#include <QObject>
#include <functional>
#include "core/types.h"
#include "core/mlccore_export.h"

/**
 * Base class for authentication providers.
 * Each provider implements doLogin() to perform its specific auth flow.
 */
class MLCCORE_EXPORT AuthBase : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(bool, LoginResult)>;

    virtual ~AuthBase() = default;

    /// Start the login flow
    virtual void login(Callback onComplete) = 0;

    /// Cancel ongoing login
    virtual void cancel() = 0;

    /// Get the login type
    virtual LoginType loginType() const = 0;

    /// Create appropriate auth provider for the given type
    static AuthBase* create(LoginType type);

signals:
    void loginProgress(const QString &status);
    void loginFinished(bool success, const LoginResult &result);
};

#endif // MLC_AUTHBASE_H
