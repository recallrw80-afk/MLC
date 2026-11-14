#include "core/launchbuilder.h"
#include "core/settings.h"
#include "core/versionmanager.h"
#include "util/arg_utils.h"
#include "util/file_utils.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>

static Q_LOGGING_CATEGORY(logBuild, "mlc.launchbuilder")

// 自动内存：取系统可用内存的 50%，按 512MB 对齐，限制在 [2048, 16384] MB。
// 读取失败时回退 4096MB。Linux 读 /proc/meminfo（注意其 st_size 恒为 0，
// 不能用 atEnd() 判断，必须以 readLine() 返回空作为结束条件）；macOS 走 sysctl hw.memsize。
#if defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#endif
static int autoMaxMemoryMB() {
#if defined(Q_OS_MACOS)
    int64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 && mem > 0) {
        long mb = long(mem / 1024 / 1024) / 2;
        mb = (mb / 512) * 512;
        return int(qBound(2048L, mb, 16384L));
    }
    return 4096;
#else
    QFile f("/proc/meminfo");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (QByteArray line = f.readLine(); !line.isEmpty(); line = f.readLine()) {
            if (line.startsWith("MemAvailable:")) {
                const auto parts = line.split(' ');
                for (const auto &p : parts) {
                    bool ok = false;
                    long kb = p.trimmed().toLong(&ok);
                    if (ok) {
                        long mb = (kb / 1024) / 2;
                        mb = (mb / 512) * 512;
                        return int(qBound(2048L, mb, 16384L));
                    }
                }
                break;
            }
        }
    }
    return 4096;
#endif
}

// 安全读取字符串字段：仅当 j 为 object、键存在且值为字符串时返回，否则返回默认值。
// version JSON 语法合法但结构畸形（类型不符）时 value()/get<>() 会抛 type_error，
// 穿过 Qt 信号槽即 std::terminate，统一用本函数兜底。
static std::string jsonStr(const json &j, const char *key, const std::string &def = "") {
    if (j.is_object() && j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return def;
}

LaunchBuilder& LaunchBuilder::instance() {
    static LaunchBuilder b;
    return b;
}

void LaunchBuilder::setVersionJson(const json &versionJson) {
    m_versionJson = versionJson;
}

void LaunchBuilder::setLaunchOptions(const McLaunchOptions &options) {
    m_options = options;
}

// Main build method

bool LaunchBuilder::build(const McVersion &version,
                           const JavaEntry &java,
                           const LoginResult &login) {
    m_jvmArgs.clear();
    m_gameArgs.clear();
    m_mainClass.clear();
    m_commandLine.clear();

    if (m_versionJson.is_discarded() || m_versionJson.is_null()) {
        qCWarning(logBuild) << "No version JSON set";
        return false;
    }

    // 兜底：version JSON 结构畸形（类型不符）时 nlohmann 抛 type_error，
    // 穿过 Qt 信号槽会 std::terminate——统一捕获，按构建失败处理
    try {
        // Get main class
        if (m_versionJson.contains("mainClass") && m_versionJson["mainClass"].is_string()) {
            m_mainClass = QString::fromStdString(m_versionJson["mainClass"].get<std::string>());
        }

        // Build args
        m_jvmArgs = buildJvmArgs(version, java);
        m_gameArgs = buildGameArgs(version, login);

        // 外置登录（authlib-injector）：注入 javaagent（jar 由启动流程预下载到约定位置；
        // 缺失时启动必然无法通过服务器验证，直接判失败）
        if (login.type == "Auth" && !login.serverUrl.isEmpty()) {
            QString jar = VersionManager::instance().mcFolder() + "authlib-injector.jar";
            if (!QFile::exists(jar)) {
                qCWarning(logBuild) << "authlib-injector.jar missing at" << jar;
                return false;
            }
            m_jvmArgs.prepend(QString("-javaagent:%1=%2").arg(jar, login.serverUrl));
        }

        // Build replacements and apply
        QMap<QString, QString> repl = buildReplacements(version, java, login);

        // Cache the fully substituted command line for display (commandLine()).
        m_commandLine = applyReplacements(m_jvmArgs, repl);
        m_commandLine.append(m_mainClass);
        m_commandLine.append(applyReplacements(m_gameArgs, repl));

        qCInfo(logBuild) << "=== Launch Arguments Begin ===";
        for (const auto &arg : applyReplacements(m_jvmArgs, repl)) {
            qCInfo(logBuild).noquote() << "[JVM]" << arg;
        }
        qCInfo(logBuild).noquote() << "[MAIN]" << m_mainClass;
        for (const auto &arg : applyReplacements(m_gameArgs, repl)) {
            qCInfo(logBuild).noquote() << "[GAME]" << arg;
        }
        qCInfo(logBuild) << "=== Launch Arguments End ===";

        return true;
    } catch (const std::exception &e) {
        qCWarning(logBuild) << "Malformed version JSON, build failed:" << e.what();
        return false;
    }
}

QString LaunchBuilder::commandLine() const
{
    // Returns the fully substituted argument list cached by build().
    // Previously this used an empty replacement map, which left ${classpath},
    // ${auth_player_name}, etc. as literal tokens in the display string.
    return m_commandLine.join(' ');
}

// JVM arguments

QStringList LaunchBuilder::buildJvmArgs(const McVersion &version, const JavaEntry &java) {
    QStringList args;

    // Read JVM args from version JSON (new format)
    if (m_versionJson.contains("arguments") && m_versionJson["arguments"].contains("jvm")) {
        for (const auto &arg : m_versionJson["arguments"]["jvm"]) {
            if (arg.is_string()) {
                args.append(QString::fromStdString(arg.get<std::string>()));
            } else if (arg.is_object()) {
                // Check rules
                if (arg.contains("rules") && !checkRules(arg["rules"])) continue;
                // Get value(s)
                if (arg.contains("value")) {
                    if (arg["value"].is_string()) {
                        args.append(QString::fromStdString(arg["value"].get<std::string>()));
                    } else if (arg["value"].is_array()) {
                        for (const auto &v : arg["value"]) {
                            if (v.is_string())
                                args.append(QString::fromStdString(v.get<std::string>()));
                        }
                    }
                }
            }
        }
    } else {
        // Old format: fixed JVM args
        qCInfo(logBuild) << "Using legacy JVM args";
        args.append("-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump");
        args.append("-Djava.library.path=${natives_directory}");
        args.append("-cp");
        args.append("${classpath}");
    }

    // Custom JVM args from settings
    QString customArg = Settings::instance().getInstance(version.id, "VersionAdvanceJvm");
    if (customArg.isEmpty()) {
        customArg = Settings::instance().getString("LaunchAdvanceJvm");
    }
    if (!customArg.isEmpty()) {
        args.append(ArgUtils::splitJavaArgs(customArg));
    }

    // Memory allocation（所有启动路径在此收口）
    // 优先级：调用方显式传入 > LaunchMaxMemory 设置（0=自动）> 按可用内存自动分配
    int maxMemMB = m_options.maxMemoryMB;
    if (maxMemMB <= 0)
        maxMemMB = Settings::instance().getString("LaunchMaxMemory", "0").toInt();
    if (maxMemMB <= 0) {
        maxMemMB = autoMaxMemoryMB();
        qCInfo(logBuild) << "Auto memory allocation:" << maxMemMB << "MB";
    }
    args.append(QString("-Xmx%1m").arg(maxMemMB));

    int minMemMB = m_options.minMemoryMB;
    if (minMemMB > 0) {
        args.append(QString("-Xms%1m").arg(minMemMB));
    }

    // GC selection: 0=Auto（<21 用 G1，21+ 用 ZGC）、1=强制 G1GC、2=强制 ZGC、3=自定义、4=优化 G1GC
    int gcType = Settings::instance().getInstance(version.id, "VersionAdvanceGC", "0").toInt();
    if (gcType <= 0) gcType = Settings::instance().getInt("LaunchAdvanceGC");
    if (gcType != 3) { // 3 = custom/user-defined
        bool useG1GC = (gcType == 1 || gcType == 4) ||
                       (gcType == 0 && java.majorVersion < 21);
        bool useZGC  = !useG1GC && (gcType == 2 || gcType == 0);

        // Remove existing GC args
        QRegularExpression gcArgRe(R"(^-XX:[+-]?(Use\w+GC|ZGenerational|UseCompactObjectHeaders|G1\w+Percent|G1\w+Size|MaxGCPauseMillis|MinHeapFreeRatio))");
        args.removeIf([&](const QString &a) { return gcArgRe.match(a).hasMatch(); });

        if (useG1GC) {
            args.append("-XX:+UseG1GC");
            if (gcType == 4) {
                // 优化 G1（MC 社区通用调优集，对齐 PCL 优化档）
                args.append("-XX:+ParallelRefProcEnabled");
                args.append("-XX:MaxGCPauseMillis=200");
                args.append("-XX:+UnlockExperimentalVMOptions");
                args.append("-XX:+DisableExplicitGC");
                args.append("-XX:+AlwaysPreTouch");
                args.append("-XX:G1NewSizePercent=30");
                args.append("-XX:G1MaxNewSizePercent=40");
                args.append("-XX:G1HeapRegionSize=8M");
                args.append("-XX:G1ReservePercent=20");
                args.append("-XX:G1HeapWastePercent=5");
                args.append("-XX:G1MixedGCCountTarget=4");
                args.append("-XX:InitiatingHeapOccupancyPercent=15");
                args.append("-XX:G1MixedGCLiveThresholdPercent=90");
                args.append("-XX:G1RSetUpdatingPauseTimePercent=5");
                args.append("-XX:SurvivorRatio=32");
                args.append("-XX:MaxTenuringThreshold=1");
            }
        } else if (useZGC) {
            args.append("-XX:+UseZGC");
            if (java.majorVersion >= 21 && java.majorVersion < 24) {
                // ZGenerational 在 21-22 是 preview（需解锁实验参数），23 转正式
                if (java.majorVersion < 23)
                    args.append("-XX:+UnlockExperimentalVMOptions");
                args.append("-XX:+ZGenerational");
            }
        }
    }

    // Compact object headers (Java 24+, 64-bit)
    if (java.majorVersion >= 24 && java.is64Bit) {
        args.append("-XX:+UseCompactObjectHeaders");
    }

    return ArgUtils::deduplicateArgs(args);
}

// Game arguments

QStringList LaunchBuilder::buildGameArgs(const McVersion &version, const LoginResult &login) {
    Q_UNUSED(login)
    QStringList args;

    // New format: arguments.game
    if (m_versionJson.contains("arguments") && m_versionJson["arguments"].contains("game")) {
        for (const auto &arg : m_versionJson["arguments"]["game"]) {
            if (arg.is_string()) {
                args.append(QString::fromStdString(arg.get<std::string>()));
            } else if (arg.is_object()) {
                if (arg.contains("rules") && !checkRules(arg["rules"])) continue;
                if (arg.contains("value")) {
                    if (arg["value"].is_string()) {
                        args.append(QString::fromStdString(arg["value"].get<std::string>()));
                    } else if (arg["value"].is_array()) {
                        for (const auto &v : arg["value"]) {
                            if (v.is_string())
                                args.append(QString::fromStdString(v.get<std::string>()));
                        }
                    }
                }
            }
        }
    } else if (m_versionJson.contains("minecraftArguments")
               && m_versionJson["minecraftArguments"].is_string()) {
        // Old format: minecraftArguments string
        QString oldArgs = QString::fromStdString(m_versionJson["minecraftArguments"].get<std::string>());
        args.append(ArgUtils::splitJavaArgs(oldArgs));
        // Always add resolution args
        args.append("--height");
        args.append("${resolution_height}");
        args.append("--width");
        args.append("${resolution_width}");
    }

    // Custom game args from settings
    QString customArg = Settings::instance().getInstance(version.id, "VersionAdvanceGame");
    if (customArg.isEmpty()) {
        customArg = Settings::instance().getString("LaunchAdvanceGame");
    }
    if (!customArg.isEmpty()) {
        args.append(ArgUtils::splitJavaArgs(customArg));
    }

    // Extra args from launch options
    args.append(m_options.extraGameArgs);

    // Fullscreen
    if (m_options.fullscreen) {
        args.append("--fullscreen");
    }

    // Quick join server
    if (!m_options.serverIp.isEmpty()) {
        if (version.releaseTime > QDateTime(QDate(2023, 4, 4), QTime(0, 0))) {
            args.append("--quickPlayMultiplayer");
            args.append(m_options.serverIp);
        } else {
            args.append("--server");
            if (m_options.serverIp.contains(':')) {
                args.append(m_options.serverIp.section(':', 0, 0));
                args.append("--port");
                args.append(m_options.serverIp.section(':', 1, 1));
            } else {
                args.append(m_options.serverIp);
                args.append("--port");
                args.append("25565");
            }
        }
    }

    return ArgUtils::deduplicateArgs(args);
}

// Replacements

QMap<QString, QString> LaunchBuilder::buildReplacements(const McVersion &version,
                                                         const JavaEntry &java,
                                                         const LoginResult &login) {
    QMap<QString, QString> r;

    // Auth
    r["auth_player_name"] = login.name;
    r["auth_uuid"] = login.uuid;
    r["auth_access_token"] = login.accessToken;
    r["auth_session"] = login.accessToken; // Legacy alias
    r["auth_xuid"] = ""; // Xbox User ID (for Minecraft Bedrock)
    r["user_type"] = login.type == "Ms" ? "msa" : "legacy";
    r["user_properties"] = "{}";
    if (!login.profileJson.isEmpty()) {
        r["user_properties"] = login.profileJson;
    }

    // Version
    r["version_name"] = version.id;
    r["version_type"] = version.type;

    // Launcher
    r["launcher_name"] = "MLC";
    r["launcher_version"] = "0.1";
    r["clientid"] = login.clientToken.isEmpty() ? "0" : login.clientToken;

    // Paths
    // libraries/assets/natives 始终用全局共享目录（PCL 规则：版本隔离只影响 game_directory）
    QString mcFolder = VersionManager::instance().mcFolder();
    r["game_directory"] = version.pathIndie;
    r["game_assets"] = mcFolder + "assets/virtual/legacy/";

    // Assets
    QString assetsIndex = "legacy";
    if (m_versionJson.contains("assets") && m_versionJson["assets"].is_string()) {
        assetsIndex = QString::fromStdString(m_versionJson["assets"].get<std::string>());
    }
    if (m_versionJson.contains("assetIndex") && m_versionJson["assetIndex"].contains("id")
            && m_versionJson["assetIndex"]["id"].is_string()) {
        assetsIndex = QString::fromStdString(m_versionJson["assetIndex"]["id"].get<std::string>());
    }
    r["assets_index_name"] = assetsIndex;
    r["assets_root"] = mcFolder + "assets/";

    // Classpath：libraries 在前（loader 先于 vanilla 由继承合并保证），
    // 主 jar 在最后；1.17+ 的 Forge/NeoForge 不加主 jar（fmlloader 从 libraries 加载）
    bool skipMainJar = (version.modLoader.hasForge || version.modLoader.hasNeoForge)
        && version.vanillaVersion.majorVersion() == 1
        && version.vanillaVersion.minorVersion() >= 17;
    // 收集库条目（同名库去重保留高版本——NeoForge 的 json 会重复列出 vanilla 库，
    // 重复 jar 会让 fmlloader 的 UnionFileSystem 崩溃，PCL 同样按此规则去重）
    struct LibEntry { QString key; QString path; QVersionNumber ver; };
    QList<LibEntry> libEntries;
    if (m_versionJson.contains("libraries")) {
        for (const auto &lib : m_versionJson["libraries"]) {
            if (!lib.is_object()) continue; // 畸形条目跳过
            if (lib.contains("rules") && !checkRules(lib["rules"])) continue;
            QString path;
            QString mavenName = QString::fromStdString(jsonStr(lib, "name"));
            if (lib.contains("downloads") && lib["downloads"].contains("artifact")) {
                // const json 上用 operator[] 取缺失键是 UB，且字段类型可能畸形，用 jsonStr 兜底
                std::string libPath = jsonStr(lib["downloads"]["artifact"], "path");
                if (libPath.empty()) continue;
                path = mcFolder + "libraries/" + QString::fromStdString(libPath);
            } else {
                // natives 容器条目不进 classpath（无主 jar）
                if (lib.contains("natives")) continue;
                // 旧格式（≤1.13）：只有 maven name，推导仓库相对路径
                QString rel = FileUtils::mavenNameToPath(mavenName);
                if (rel.isEmpty()) continue;
                path = mcFolder + "libraries/" + rel;
            }
            // key = group:artifact[:classifier]，按它比版本
            QString key = path;
            auto parts = mavenName.split(':');
            if (parts.size() >= 3)
                key = parts[0] + ':' + parts[1] + (parts.size() > 3 ? ':' + parts[3] : QString());
            libEntries.append({key, path,
                parts.size() >= 3 ? QVersionNumber::fromString(parts[2]) : QVersionNumber()});
        }
    }
    QStringList classpathParts;
    QSet<QString> emitted;
    for (const auto &e : libEntries) {
        if (emitted.contains(e.key)) continue;
        bool hasHigher = false;
        for (const auto &o : libEntries)
            if (o.key == e.key && o.ver > e.ver) { hasHigher = true; break; }
        if (hasHigher) continue;
        emitted.insert(e.key);
        classpathParts.append(e.path);
    }
    if (!skipMainJar)
        classpathParts.append(version.pathJar);
    QString classpathSep = (currentPlatform() == Platform::Windows) ? ";" : ":";
    r["classpath"] = classpathParts.join(classpathSep);
    r["classpath_separator"] = classpathSep;

    // Natives：全局 vanilla 版本的 natives 目录（实例版本共用 vanilla natives）
    r["natives_directory"] = mcFolder + "versions/" + version.vanillaVersion.toString() + "/natives/";

    // Resolution
    r["resolution_width"] = QString::number(m_options.windowWidth);
    r["resolution_height"] = QString::number(m_options.windowHeight);

    // Java
    r["library_directory"] = mcFolder + "libraries/";

    return r;
}

QStringList LaunchBuilder::applyReplacements(const QStringList &args,
                                              const QMap<QString, QString> &repl) const
{
    QStringList result;
    for (const auto &arg : args) {
        QString a = arg;
        for (auto it = repl.begin(); it != repl.end(); ++it) {
            a.replace("${" + it.key() + "}", it.value());
        }
        // 注意：不要在此加引号——参数直接传给 QProcess::setArguments()，
        // 它不做 shell 解析，字面引号会成为参数本身的一部分
        result.append(a);
    }
    return result;
}

// Rule checking

bool LaunchBuilder::checkRules(const json &rules) {
    if (!rules.is_array() || rules.empty()) return true; // No rules = allow

    bool allowed = false;

    for (const auto &rule : rules) {
        if (!rule.is_object()) continue; // 畸形规则跳过
        bool ruleAllows = (jsonStr(rule, "action", "allow") == "allow");

        // If no conditions, the rule matches
        bool matches = true;

        if (rule.contains("os")) {
            matches = false;
            const auto &os = rule["os"];
            if (os.is_object()) {
                std::string osName = jsonStr(os, "name");
                std::string osArch = jsonStr(os, "arch");

                bool osMatch = true;
                if (!osName.empty()) {
                    osMatch = (osName == platformName().toStdString());
                }
                if (!osArch.empty()) {
                    osMatch = osMatch && (osArch == (is64BitSystem() ? "x86_64" : "x86"));
                }
                matches = osMatch;
            }
        }

        if (rule.contains("features")) {
            matches = matches && checkFeatures(rule["features"]);
        }

        if (matches) {
            allowed = ruleAllows;
        }
    }

    return allowed;
}

bool LaunchBuilder::checkFeatures(const json &features) {
    // Check feature flags
    // For now, common features:
    // - "is_demo_user": false (we're not a demo user)
    // - "has_custom_resolution": true (if windowed mode)
    // - "has_quick_plays_support": true for 1.20+
    if (!features.is_object()) return true;

    if (features.contains("is_demo_user") && features["is_demo_user"].is_boolean()) {
        if (features["is_demo_user"].get<bool>()) return false;
    }

    if (features.contains("has_custom_resolution")) {
        // We always support custom resolution
    }

    // 未实现 QuickPlay——剔除 --quickPlay* 参数（has_quick_plays_support /
    // is_quick_play_singleplayer/multiplayer/realms 四类键）：
    // 未替换的 ${quickPlay*} 占位符会让游戏把占位符当世界名，自动创建并进入世界
    for (auto it = features.begin(); it != features.end(); ++it) {
        if (QString::fromStdString(it.key()).contains("quick_play", Qt::CaseInsensitive))
            return false;
    }

    return true;
}
