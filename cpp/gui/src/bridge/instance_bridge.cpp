#include "bridge/instance_bridge.h"

#include "mlc.h"

InstanceBridge& InstanceBridge::instance() {
    static InstanceBridge b;
    return b;
}

QVariantMap InstanceBridge::instanceInfo(const QString &displayName) const {
    auto info = mlc::instanceInfo(displayName);
    return {
        {"dirName", info.dirName},
        {"path", info.path},
        {"version", info.version},
        {"modCount", info.modCount},
        {"exists", !info.dirName.isEmpty()},
    };
}

QVariantList InstanceBridge::modList(const QString &displayName) const {
    QVariantList out;
    for (const auto &m : mlc::listMods(displayName)) {
        QVariantMap v;
        v.insert("fileName", m.fileName);
        v.insert("size", m.size);
        v.insert("enabled", m.enabled);
        out.append(v);
    }
    return out;
}

bool InstanceBridge::setModEnabled(const QString &displayName, const QString &fileName, bool enabled) {
    bool ok = mlc::setModEnabled(displayName, fileName, enabled);
    if (ok) emit modsChanged(displayName);
    return ok;
}

bool InstanceBridge::deleteMod(const QString &displayName, const QString &fileName) {
    bool ok = mlc::deleteMod(displayName, fileName);
    if (ok) emit modsChanged(displayName);
    return ok;
}

bool InstanceBridge::removeInstance(const QString &displayName) {
    bool ok = mlc::removeInstance(displayName);
    if (ok) emit instancesChanged();
    return ok;
}
