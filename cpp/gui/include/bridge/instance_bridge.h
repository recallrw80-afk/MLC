#ifndef MLC_INSTANCE_BRIDGE_H
#define MLC_INSTANCE_BRIDGE_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// 实例管理的 QML 桥接层：包装 mlc:: 实例信息 / Mod 管理 API（SDK 自由函数 QML 不可直接调用）
class InstanceBridge : public QObject
{
    Q_OBJECT

public:
    static InstanceBridge& instance();

    /// 实例信息：{dirName, path, version, modCount, exists}
    Q_INVOKABLE QVariantMap instanceInfo(const QString &displayName) const;

    /// Mod 列表：[{fileName, size, enabled}]（按文件名排序）
    Q_INVOKABLE QVariantList modList(const QString &displayName) const;

    /// 启用/禁用 Mod（重命名 .jar ↔ .jar.disabled）
    Q_INVOKABLE bool setModEnabled(const QString &displayName, const QString &fileName, bool enabled);

    /// 删除 Mod 文件
    Q_INVOKABLE bool deleteMod(const QString &displayName, const QString &fileName);

    /// 删除整个实例（危险操作，调用方负责二次确认）
    Q_INVOKABLE bool removeInstance(const QString &displayName);

signals:
    void modsChanged(const QString &displayName);
    void instancesChanged();
};

#endif // MLC_INSTANCE_BRIDGE_H
