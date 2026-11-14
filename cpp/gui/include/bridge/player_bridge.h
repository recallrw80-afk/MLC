#ifndef MLC_PLAYER_BRIDGE_H
#define MLC_PLAYER_BRIDGE_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "core/types.h"

// 玩家管理的 QML 桥接层：包装 mlc:: 玩家 API（SDK 自由函数 QML 不可直接调用）
// 数据真源在 MLC.ini（经 mlc:: 读写），本类只做转发 + 变更通知
class PlayerBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentUuid READ currentUuid NOTIFY playersChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY playersChanged)
    Q_PROPERTY(QString currentSkinType READ currentSkinType NOTIFY playersChanged)

public:
    static PlayerBridge& instance();

    QString currentUuid() const;
    QString currentName() const;
    QString currentSkinType() const;

    /// 玩家列表：[{uuid, name, avatar, skinType, selected}]
    Q_INVOKABLE QVariantList playerList() const;

    /// 选中玩家（uuid 不存在返回 false）
    Q_INVOKABLE bool selectPlayer(const QString &uuid);

    /// 添加玩家并自动选中（skinType: "slim"/"wide"），返回新条目 {uuid, name, skinType}
    Q_INVOKABLE QVariantMap addPlayer(const QString &name, const QString &skinType = "slim");

    /// 删除玩家；删除当前选中者后自动选中剩余首个
    Q_INVOKABLE bool removePlayer(const QString &uuid);

    /// 用当前选中玩家构造 LoginResult（slim→Alex / wide→Steve，同 CLI 启动语义）；
    /// 无玩家时回退默认 "Player"
    Q_INVOKABLE LoginResult createLogin() const;

signals:
    void playersChanged();

private:
    PlayerBridge() = default;
};

#endif // MLC_PLAYER_BRIDGE_H
