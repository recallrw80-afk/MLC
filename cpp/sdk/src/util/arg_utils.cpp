#include "util/arg_utils.h"

#include <QRegularExpression>
#include <QSet>

namespace ArgUtils {

QStringList splitJavaArgs(const QString &str) {
    QStringList args;
    bool inQuote = false;
    QString current;

    for (int i = 0; i < str.length(); ++i) {
        QChar c = str[i];
        if (c == '"') {
            inQuote = !inQuote;
        } else if (c == ' ' && !inQuote) {
            if (!current.isEmpty()) {
                args.append(current.trimmed());
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.trimmed().isEmpty())
        args.append(current.trimmed());

    return args;
}

QStringList deduplicateArgs(const QStringList &args) {
    // 可重复参数（成对出现的模块 flag）不参与去重：
    // Forge/NeoForge 的 JVM 参数含多个 --add-opens/--add-exports，
    // 按前缀去重会把它们删到只剩一个，值变成孤儿参数
    static const QSet<QString> repeatableFlags = {
        QStringLiteral("--add-opens"), QStringLiteral("--add-exports"),
        QStringLiteral("--add-reads"), QStringLiteral("--patch-module")
    };
    // 带值参数：flag 与紧随其后的值是一个整体，去重必须同去同留，
    // 否则值会残留成孤儿参数（如 -cp 去重后 classpath 变成裸参数 → 启动失败）
    static const QSet<QString> valueFlags = {
        // JVM
        QStringLiteral("-cp"), QStringLiteral("-classpath"), QStringLiteral("--class-path"),
        QStringLiteral("-p"), QStringLiteral("--module-path"),
        QStringLiteral("--add-modules"), QStringLiteral("--upgrade-module-path"),
        QStringLiteral("--limit-modules"),
        // 游戏参数（vanilla 与 Forge 常见项）
        QStringLiteral("--username"), QStringLiteral("--version"),
        QStringLiteral("--gameDir"), QStringLiteral("--assetsDir"),
        QStringLiteral("--assetIndex"), QStringLiteral("--uuid"),
        QStringLiteral("--accessToken"), QStringLiteral("--clientId"),
        QStringLiteral("--xuid"), QStringLiteral("--userType"),
        QStringLiteral("--versionType"), QStringLiteral("--width"),
        QStringLiteral("--height"), QStringLiteral("--server"),
        QStringLiteral("--port"), QStringLiteral("--session"),
        QStringLiteral("--userProperties"), QStringLiteral("--tweakClass"),
        QStringLiteral("--launchTarget"), QStringLiteral("--fml.forgeVersion"),
        QStringLiteral("--fml.mcVersion"), QStringLiteral("--fml.forgeGroup"),
        QStringLiteral("--fml.mcpVersion"), QStringLiteral("--quickPlaySingleplayer"),
        QStringLiteral("--quickPlayMultiplayer"), QStringLiteral("--quickPlayRealms")
    };
    // -Xmx2G / -Xms512m 这类无分隔符参数按 flag 名去重（后写的覆盖先写的）
    // 排除 -XX: 前缀（-XX:+UseG1GC、-XX:G1NewSizePercent=20 等各自独立）
    static const QRegularExpression xFlagRe(QStringLiteral("^(-X(?!X)[a-zA-Z]+)"));

    auto keyOf = [&](const QString &a) -> QString {
        if (repeatableFlags.contains(a)) return {};  // 空 key = 不去重
        auto m = xFlagRe.match(a);
        if (m.hasMatch()) return m.captured(1);      // -Xmx2G → -Xmx
        return a.section(QRegularExpression("[= ]"), 0, 0);
    };

    // 标记带值 flag 的值下标 → 所属 flag 下标（已被消费为值的项不再视为 flag，
    // 与 java 的参数解析一致；flag 是末项时无值，按普通参数处理）
    QMap<int, int> valueOwner;
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (valueFlags.contains(args[i]) && !valueOwner.contains(i))
            valueOwner.insert(i + 1, i);
    }

    // 保留每个 key 的最后一次出现（值不独立参与去重，随 flag 去留）
    QMap<QString, int> lastIndex;
    for (int i = 0; i < args.size(); ++i) {
        if (valueOwner.contains(i)) continue;
        QString key = keyOf(args[i]);
        if (!key.isEmpty()) lastIndex[key] = i;
    }
    QStringList result;
    for (int i = 0; i < args.size(); ++i) {
        if (valueOwner.contains(i)) {
            // 值：仅当所属 flag 被保留时保留（带值 flag 的 key 即 flag 本身）
            int owner = valueOwner.value(i);
            if (lastIndex.value(args[owner]) == owner)
                result.append(args[i]);
            continue;
        }
        QString key = keyOf(args[i]);
        if (key.isEmpty() || lastIndex.value(key) == i)
            result.append(args[i]);
    }
    return result;
}

} // namespace ArgUtils
