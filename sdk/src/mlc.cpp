#include "mlc.h"
#include "modpack.h"
#include "cf_key_embedded.h"  // CMake 生成：MLC_CF_API_KEY_EMBEDDED（key 来源判定/回退用）
#include "core/settings.h"
#include "core/versionmanager.h"
#include "core/javamanager.h"
#include "core/launcher.h"
#include "core/launchbuilder.h"
#include "core/installer.h"
#include "auth/offlineauth.h"
#include "auth/authlibauth.h"
#include "download/assetdownloader.h"
#include "download/downloadmanager.h"
#include "download/modplatform.h"
#include "util/file_utils.h"
#include "util/platform_utils.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QSysInfo>
#include <QUuid>
#include <QSharedPointer>
#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

// 等待一个异步操作完成（CLI 同步等待）
static bool waitForAsync(const std::function<void(std::function<void(bool, QString)>)> &starter) {
    QEventLoop loop;
    QPointer<QEventLoop> guard = &loop;
    auto state = QSharedPointer<bool>::create(false);
    auto done = QSharedPointer<bool>::create(false);
    starter([state, done, guard](bool ok, QString) {
        *state = ok;
        *done = true;
        if (guard) guard->quit();
    });
    if (!*done) loop.exec();  // 同步完成路径下 quit 先于 exec，不能裸等
    return *state;
}

// 确保 authlib-injector jar 就位（{mcFolder}/authlib-injector.jar）：
// 缺失则从官方源下载（失败回退 BMCLAPI 镜像），并按 latest.json 的 sha256 校验（javaagent 是代码注入，必须验完整性）
static bool ensureAuthlibInjector() {
    QString jarPath = VersionManager::instance().mcFolder() + "authlib-injector.jar";
    const bool existed = QFile::exists(jarPath);
    if (existed) {
        // 已存在也要校验：中断的下载或手工放错的文件会留下损坏 jar，
        // 而这是 -javaagent 代码注入，损坏会直接导致 JVM 起不来。
        // 校验需要 latest.json 的 sha256 —— 下面统一拉取元数据后再判定。
        qWarning() << "authlib-injector 已存在，校验完整性 ...";
    }
    json meta;
    bool gotMeta = false;
    for (const char *metaUrl : {"https://authlib-injector.yushi.moe/artifact/latest.json",
                                "https://bmclapi2.bangbang93.com/mirrors/authlib-injector/artifact/latest.json"}) {
        gotMeta = waitForAsync([&](std::function<void(bool, QString)> cb) {
            DownloadManager::instance().downloadJson(QString::fromLatin1(metaUrl),
                [&](bool ok, QString, json j) { if (ok && j.is_object()) meta = std::move(j); cb(ok, {}); });
        });
        if (gotMeta) break;
    }
    if (!gotMeta) {
        // 元数据拿不到：已有 jar 允许沿用（离线可玩），但没有则失败
        return existed;
    }

    QString url = QString::fromStdString(meta.value("download_url", ""));
    QString expectedSha;
    if (meta.contains("checksums") && meta["checksums"].is_object())
        expectedSha = QString::fromStdString(meta["checksums"].value("sha256", ""));
    if (url.isEmpty()) return false;

    bool ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        DownloadManager::instance().download(url, jarPath, nullptr,
            [cb](bool dlOk, QString) { cb(dlOk, {}); });
    });
    if (!ok) { QFile::remove(jarPath); return false; }

    if (existed) {
        // 已有文件：sha256 匹配就直接用，不匹配则删除并重新下载
        if (!expectedSha.isEmpty()) {
            QFile ef(jarPath);
            if (ef.open(QIODevice::ReadOnly)) {
                QCryptographicHash h(QCryptographicHash::Sha256);
                h.addData(&ef);
                ef.close();
                if (h.result().toHex().toLower() == expectedSha.toLower()) {
                    qWarning() << "authlib-injector 校验通过（复用已有文件）";
                    return true;
                }
            }
            qWarning() << "authlib-injector 校验失败，删除后重新下载";
            QFile::remove(jarPath);
        } else {
            return true;  // 无期望哈希，沿用已有文件
        }
    }

    if (!expectedSha.isEmpty()) {
        QFile f(jarPath);
        if (!f.open(QIODevice::ReadOnly)) { QFile::remove(jarPath); return false; }
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(&f);
        if (h.result().toHex().toLower() != expectedSha.toLower()) {
            qWarning() << "authlib-injector sha256 校验失败";
            QFile::remove(jarPath);
            return false;
        }
    }
    qWarning() << "authlib-injector 已下载:" << jarPath;
    return true;
}

// 启动预检：补齐缺失的游戏文件（对照原版 PCL 的 DlClientFix 补全下载）
// vanilla 文件缺失 → 自动重下（downloadVersion 按 sha1 跳过已有文件）；
// loader/pack 的库缺失 → 报错（无来源可自动补齐）
static bool ensureLaunchReadyImpl(const McVersion &version) {
    auto &vm = VersionManager::instance();
    QString mcFolder = vm.mcFolder();
    QString vanillaId = version.vanillaVersion.toString();

    json resolved = VersionManager::resolveInheritanceChain(version.pathJson);
    if (resolved.is_null()) {
        qWarning() << "启动预检: 无法解析 version json:" << version.pathJson;
        return false;
    }

    bool needVanilla = !QFile::exists(version.pathJar);
    QSet<QString> vanillaLibs;
    QSet<QString> loaderMissing;

    // 收集 vanilla json 的库名集合（判断缺失库能否自动补齐）
    if (!vanillaId.isEmpty()) {
        json vanillaJson = VersionManager::resolveInheritanceChain(
            mcFolder + "versions/" + vanillaId + "/" + vanillaId + ".json");
        if (!vanillaJson.is_null() && vanillaJson.contains("libraries")) {
            for (const auto &lib : vanillaJson["libraries"])
                vanillaLibs.insert(QString::fromStdString(lib.value("name", "")));
        }
    }

    // libraries 存在性检查
    if (resolved.contains("libraries")) {
        for (const auto &lib : resolved["libraries"]) {
            // 按平台 rules 过滤（osx-only 等库在本平台本就不该存在，跳过而非判缺失）
            if (lib.contains("rules") && !LaunchBuilder::checkRules(lib["rules"])) continue;
            QString rel;
            QString name = QString::fromStdString(lib.value("name", ""));
            if (lib.contains("downloads") && lib["downloads"].contains("artifact")) {
                rel = QString::fromStdString(lib["downloads"]["artifact"].value("path", ""));
            } else {
                if (lib.contains("natives")) continue;
                rel = FileUtils::mavenNameToPath(name);
            }
            if (rel.isEmpty()) continue;
            if (QFile::exists(mcFolder + "libraries/" + rel)) continue;
            if (vanillaLibs.contains(name))
                needVanilla = true;
            else
                loaderMissing.insert(name);
        }
    }

    // assets 索引 + 对象存在性检查
    QString assetsIndexId = "legacy";
    if (resolved.contains("assetIndex"))
        assetsIndexId = QString::fromStdString(resolved["assetIndex"].value("id", ""));
    else if (resolved.contains("assets"))
        assetsIndexId = QString::fromStdString(resolved["assets"].get<std::string>());
    QString idxPath = mcFolder + "assets/indexes/" + assetsIndexId + ".json";
    if (!QFile::exists(idxPath)) {
        needVanilla = true;
    } else {
        QFile f(idxPath);
        if (f.open(QIODevice::ReadOnly)) {
            json idx = json::parse(f.readAll().toStdString(), nullptr, false);
            if (!idx.is_discarded() && idx.contains("objects")) {
                for (auto it = idx["objects"].begin(); it != idx["objects"].end(); ++it) {
                    QString hash = QString::fromStdString(it.value().value("hash", ""));
                    if (hash.isEmpty()) continue;
                    if (!QFile::exists(mcFolder + "assets/objects/" +
                                       FileUtils::assetPathFromHash(hash))) {
                        needVanilla = true;
                        break;
                    }
                }
            }
            f.close();
        }
    }

    if (needVanilla && vanillaId.isEmpty()) {
        qWarning() << "启动预检: 主 jar/资产缺失但无法确定 MC 版本";
        return false;
    }

    // 补齐 vanilla 缺失（downloadVersion 内部按 sha1 跳过已有文件）
    if (needVanilla) {
        qWarning() << "启动预检: 补齐缺失的游戏文件（MC" << vanillaId << "）...";
        if (!waitForAsync([&](std::function<void(bool, QString)> cb) {
                AssetDownloader::instance().downloadVersion(vanillaId, cb);
            })) {
            qWarning() << "启动预检: 补齐游戏文件失败";
            return false;
        }
    }

    if (!loaderMissing.isEmpty()) {
        qWarning() << "启动预检: modloader 库缺失（无法自动补齐，请重新导入整合包）:"
                   << loaderMissing.values().join(", ");
        return false;
    }

    // natives：目录为空则补
    QString nativesDir = mcFolder + "versions/" + vanillaId + "/natives/";
    if (!vanillaId.isEmpty() &&
        QDir(nativesDir).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()) {
        McVersion ver;
        ver.id = vanillaId;
        ver.pathVersion = mcFolder + "versions/" + vanillaId + "/";
        ver.pathJar = ver.pathVersion + vanillaId + ".jar";
        ver.pathIndie = mcFolder;
        qWarning() << "启动预检: 补齐 natives...";
        if (!waitForAsync([&ver](std::function<void(bool, QString)> cb) {
                AssetDownloader::instance().downloadNatives(ver, cb);
            })) {
            qWarning() << "启动预检: 补齐 natives 失败";
            return false;
        }
    }
    return true;
}

// 清单解析异常兜底：畸形 version json/清单的 nlohmann 异常统一转为干净失败
static bool ensureLaunchReady(const McVersion &version) {
    try {
        return ensureLaunchReadyImpl(version);
    } catch (const std::exception &e) {
        qWarning() << "启动预检: 清单解析失败:" << e.what();
        return false;
    }
}

// 中文输入修复：fcitx/ibus 的 XIM 会让 GLFW 3.3 在 glfwWaitEventsTimeout 里 SIGSEGV。
// LWJGL ≥ 3.3.3 自带的 GLFW 3.4 已重构 XIM 路径不会崩。
// 注意 LWJGL 的 natives 加载机制：它从 classpath 里的 *-natives-linux.jar 提取
// libglfw.so 到 SharedLibraryExtractPath，文件不匹配就重新提取覆盖——所以直接
// 替换 natives 目录或前置 java.library.path 都无效（均已实测）。
// 唯一可靠做法：把 libraries/ 里的 lwjgl-glfw-<ver>-natives-linux.jar 内容换成
// 3.3.6 版（文件名不变），LWJGL 提取出来的就是 GLFW 3.4。
// 任何失败都静默返回：doLaunch 会回退到 XMODIFIERS=@im=none（禁输入法式修复）。
static void maybeInstallGlfw34(const McVersion &version) {
#if !defined(Q_OS_LINUX)
    // XIM 崩溃是 Linux/X11 特有问题（fcitx/ibus 的 XIM 钩子），macOS/Windows 无需处理
    Q_UNUSED(version);
    return;
#else
    QString xim = qEnvironmentVariable("XMODIFIERS");
    if (!xim.contains("@im=") || xim == "@im=none") return;  // 无 IME 钩子，无需处理

    auto &vm = VersionManager::instance();
    QString mcFolder = vm.mcFolder();
    QString vanillaId = version.vanillaVersion.toString();
    if (vanillaId.isEmpty()) return;
    QString nativesDir = mcFolder + "versions/" + vanillaId + "/natives/";
    QString marker = nativesDir + "libglfw.so.glfw34-fixed";
    if (QFile::exists(marker)) return;

    // 找 LWJGL 版本；≥ 3.3.3 自带 GLFW 3.4，无需替换
    QString lwjglVer;
    json resolved = VersionManager::resolveInheritanceChain(version.pathJson);
    if (!resolved.is_null() && resolved.contains("libraries")) {
        for (const auto &lib : resolved["libraries"]) {
            QString name = QString::fromStdString(lib.value("name", ""));
            if (name.startsWith("org.lwjgl:lwjgl:") || name.startsWith("org.lwjgl:lwjgl-glfw:")) {
                QString ver = name.section(':', -1);
                QStringList p = ver.split('.');
                if (p.size() >= 3) {
                    int minor = p[1].toInt(), patch = p[2].toInt();
                    if (minor > 3 || (minor == 3 && patch >= 3)) return;
                }
                lwjglVer = ver;
                break;
            }
        }
    }
    if (lwjglVer.isEmpty()) return;

    QString jarPath = mcFolder + "libraries/org/lwjgl/lwjgl-glfw/" + lwjglVer +
                      "/lwjgl-glfw-" + lwjglVer + "-natives-linux.jar";
    QString bak = jarPath + ".bak-vanilla";
    if (!QFile::exists(jarPath)) {
        // 上次替换中途失败可能留下 jar 缺失：有备份先恢复再重试，避免 jar 永久缺失
        if (QFile::exists(bak)) QFile::copy(bak, jarPath);
        if (!QFile::exists(jarPath)) return;
    }

    QString tmpJar = mcFolder + "tmp/lwjgl-glfw-3.3.6-natives-linux.jar";
    bool ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        DownloadManager::instance().download(
            "https://repo1.maven.org/maven2/org/lwjgl/lwjgl-glfw/3.3.6/lwjgl-glfw-3.3.6-natives-linux.jar",
            tmpJar, nullptr, cb);
    });
    // 替换前校验产物是合法 zip（防截断/错误页写进 libraries 后游戏起不来）
    if (ok && FileUtils::listZipEntries(tmpJar).isEmpty()) {
        qWarning() << "IME 修复: 下载的 lwjgl-glfw jar 不是合法 zip，放弃替换";
        ok = false;
    }
    if (ok && !QFile::exists(bak)) {
        // 原 jar 备份只做一次；备份失败则中止，否则替换失败无源可恢复
        ok = QFile::copy(jarPath, bak);
        if (!ok) qWarning() << "IME 修复: 备份原 jar 失败，放弃替换";
    }
    if (ok) {
        QFile::remove(jarPath);
        ok = QFile::copy(tmpJar, jarPath);
        if (ok) {
            QFile m(marker);
            if (m.open(QIODevice::WriteOnly)) m.write("lwjgl-glfw 3.3.6 (GLFW 3.4)\n");
            qWarning() << "IME 修复: MC" << vanillaId << "将使用 GLFW 3.4（保留中文输入）";
        } else {
            // 替换失败：清掉可能的半成品（QFile::copy 遇已存在目标会失败），从备份恢复原 jar
            QFile::remove(jarPath);
            if (QFile::copy(bak, jarPath))
                qWarning() << "IME 修复: 替换失败，已从备份恢复原 jar";
            else
                qWarning() << "IME 修复: 替换失败且备份恢复失败:" << jarPath;
        }
    }
    QFile::remove(tmpJar);
    if (!ok) qWarning() << "IME 修复: GLFW 3.4 准备失败，将回退到禁用 XIM 方案";
#endif
}

} // namespace


// 解析 eula.txt：逐行去掉注释与空白，只有真正的 eula=true 才算同意。
// （直接 contains("eula=true") 会把 "#eula=true" 这类注释误判为已同意）
static bool isEulaAccepted(const QString &dir) {
    QFile f(dir + "eula.txt");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString all = QString::fromUtf8(f.readAll());
    f.close();
    for (const QString &raw : all.split('\n')) {
        QString line = raw;
        const int hash = line.indexOf('#');
        if (hash >= 0) line = line.left(hash);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        if (line.left(eq).trimmed().compare("eula", Qt::CaseInsensitive) == 0
            && line.mid(eq + 1).trimmed().compare("true", Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

namespace mlc {

// 外置登录持久化（定义见文件尾部 "外置登录" 一节）
static void persistAuthlibLogin(const LoginResult &r);

QStringList listVersions() {
    auto &vm = VersionManager::instance();
    vm.loadLocalVersions();
    return vm.versionIds();
}

QStringList listMcVersions() {
    auto &vm = VersionManager::instance();
    vm.loadMcVersions();
    return vm.versionIds();
}

bool installJavaRuntime(int majorVersion, QString *errOut) {
    if (majorVersion <= 0) majorVersion = 8;
    QString osName;
    switch (currentPlatform()) {
    case Platform::Windows: osName = "windows"; break;
    case Platform::MacOS:   osName = "mac"; break;
    default:                osName = "linux"; break;
    }
    QString cpu = QSysInfo::currentCpuArchitecture();
    QString arch = (cpu == "arm64" || cpu == "aarch64") ? "aarch64"
                 : is64BitSystem() ? "x64" : "x86";
    QString url = QString("https://api.adoptium.net/v3/binary/latest/%1/ga/%2/%3/jre/hotspot/normal/eclipse")
                      .arg(majorVersion).arg(osName, arch);

    QString mcFolder = VersionManager::instance().mcFolder();
    QString tmpFile = mcFolder + "tmp/jre-" + QString::number(majorVersion)
                      + (osName == "windows" ? ".zip" : ".tar.gz");

    // 下载 JRE 包（Adoptium API 会 302 到实际地址，DownloadManager 跟随重定向）
    if (!waitForAsync([&](std::function<void(bool, QString)> cb) {
            DownloadManager::instance().download(url, tmpFile, nullptr, cb);
        })) {
        if (errOut) *errOut = "JRE 下载失败: " + url;
        return false;
    }

    // 解压到 {mcFolder}/javas/
    QString javasDir = mcFolder + "javas/";
    QString err;
    bool ok = (osName == "windows")
        ? FileUtils::extractZip(tmpFile, javasDir, &err)
        : FileUtils::extractTarGz(tmpFile, javasDir, &err);
    QFile::remove(tmpFile);
    if (!ok) {
        if (errOut) *errOut = "JRE 解压失败: " + err;
        return false;
    }

    // 扫描注册（scanFolder 递归查找 java 可执行文件并 checkJava 验证）
    JavaManager::instance().scanFolder(javasDir, false);
    return true;
}

bool installVersion(const QString &versionId,
                    std::function<void(const ImportProgress &)> onProgress) {
    // 空版本号 = 最新正式版（读官方版本清单 latest.release）
    QString id = versionId;
    if (id.isEmpty()) {
        json manifest;
        bool ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
            DownloadManager::instance().downloadJson(
                "https://launchermeta.mojang.com/mc/game/version_manifest.json",
                [&manifest, cb](bool ok2, QString err, json m) {
                    manifest = std::move(m);
                    cb(ok2, err);
                });
        });
        if (!ok || !manifest.contains("latest")) return false;
        id = QString::fromStdString(manifest["latest"].value("release", ""));
        if (id.isEmpty()) return false;
        if (onProgress) onProgress({"Latest release: " + id, 0});
    }

    // 连接 AssetDownloader 的信号转发进度
    auto &ad = AssetDownloader::instance();
    auto conn1 = QObject::connect(&ad, &AssetDownloader::downloadLog,
        [onProgress](const QString &msg) {
            if (onProgress) onProgress({msg, -1});
        });
    auto conn2 = QObject::connect(&ad, &AssetDownloader::downloadProgress,
        [onProgress](const QString &msg, int cur, int total) {
            if (onProgress && total > 0) onProgress({msg, 5 + 70 * cur / total});
        });

    // Step 1: 下载 MC 版本（json + jar + libraries + assets）
    bool ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        ad.downloadVersion(id, cb);
    });
    if (!ok) {
        QObject::disconnect(conn1);
        QObject::disconnect(conn2);
        return false;
    }

    // Step 2: natives
    QString mcFolder = VersionManager::instance().mcFolder();
    McVersion ver;
    ver.id = id;
    ver.pathVersion = mcFolder + "versions/" + id + "/";
    ver.pathJar = ver.pathVersion + id + ".jar";
    ver.pathIndie = mcFolder;
    if (onProgress) onProgress({"Downloading native libraries...", 85});
    ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        ad.downloadNatives(ver, cb);
    });

    QObject::disconnect(conn1);
    QObject::disconnect(conn2);
    if (onProgress) onProgress({ok ? "Complete" : "Natives failed", 100});
    return ok;
}

bool launchVersion(const QString &versionId,
                   LogCallback onLog,
                   ExitCallback onExit) {
    auto &launcher = Launcher::instance();
    auto &jm = JavaManager::instance();

    // 解析：INI 映射命中 → 实例版本（loadInstanceVersion）；否则全局版本
    QString dirName = Settings::instance().dirForDisplayName(versionId);
    McVersion version;
    if (!dirName.isEmpty())
        version = VersionManager::instance().loadInstanceVersion(dirName);
    else
        version = VersionManager::instance().loadVersion(versionId);
    if (!version.isValid) return false;

    // 登录：优先持久化的外置登录（authlib-injector，在线刷新换新 token）；
    // 无登录态或刷新/jar 准备失败回退离线玩家
    LoginResult login;
    auto savedLogin = currentAuthlibLogin();
    if (savedLogin.loggedIn) {
        AuthlibAuth auth;
        auth.setServerUrl(savedLogin.server);
        auth.setTokens(Settings::instance().getEncrypted("Authlib/AccessToken"),
                       Settings::instance().getEncrypted("Authlib/ClientToken"));
        bool done = false, refreshed = false;
        LoginResult r;
        QEventLoop loop;
        QPointer<QEventLoop> guard = &loop;
        auth.refresh([&](bool ok, LoginResult res) {
            refreshed = ok; r = res; done = true; if (guard) guard->quit(); });
        if (!done) loop.exec();  // done 标志防同步完成导致裸等
        if (refreshed) {
            if (r.name.isEmpty()) r.name = savedLogin.name;  // refresh 未回角色则沿用本地
            if (r.uuid.isEmpty()) r.uuid = savedLogin.uuid;
            persistAuthlibLogin(r);  // 新 accessToken 回写
            if (ensureAuthlibInjector()) {
                login = r;
            } else {
                qWarning() << "authlib-injector 下载失败，回退离线模式";
            }
        } else {
            qWarning() << "外置登录刷新失败（token 过期或网络问题），回退离线模式。重新登录: mlc login";
        }
    }
    if (!login.isValid()) {
        // 离线：使用选中的玩家 Profile（名字 + 皮肤类型，不再硬编码 "Player"）
        QString playerUuid = Settings::instance().selectedPlayer();
        QString playerName = Settings::instance().getProfile(playerUuid, "Name", "Player");
        if (playerName.isEmpty()) playerName = "Player";
        QString skinType = Settings::instance().getProfile(playerUuid, "SkinType", "slim");
        login = OfflineAuth::createOfflineLogin(playerName, skinType);
    }

    if (jm.javaList().isEmpty()) {
        jm.scanSystemJava();
        jm.waitForScanFinished();
    }
    // 优先按版本兼容矩阵选 Java；无严格匹配时回退到最优可用
    // （如 1.12.2 机器上只有 Java 21/25，严格匹配 Java 8 会落空，但新版本也能跑）
    JavaEntry java = jm.selectJavaForVersion(version);
    if (java.pathJava.isEmpty()) java = jm.selectJava();
    if (java.pathJava.isEmpty()) {
        // 一个 Java 都没有：自动下载矩阵推荐的最低大版本 JRE（同原版 PCL 的自动 Java 下载）
        QVersionNumber minVer, maxVer;
        JavaManager::getJavaCompatibilityRange(version, minVer, maxVer);
        int major = minVer.isNull() ? 8
            : (minVer.majorVersion() == 1 ? minVer.minorVersion() : minVer.majorVersion());
        qWarning() << "未检测到 Java 运行时，自动下载 JRE" << major << "...";
        QString err;
        if (!installJavaRuntime(major, &err)) {
            qWarning() << "Java 自动下载失败:" << err;
            return false;
        }
        java = jm.selectJavaForVersion(version);
        if (java.pathJava.isEmpty()) java = jm.selectJava();
        if (java.pathJava.isEmpty()) return false;
    }

    // 启动预检：补齐缺失的游戏文件（对照 PCL DlClientFix）
    if (!ensureLaunchReady(version)) return false;
    // 中文输入修复：XIM 激活且 LWJGL 老（GLFW 3.3）时替换 GLFW 3.4
    maybeInstallGlfw34(version);

    // 单例 Launcher 上挂的回调连接：每次启动前必须断开上一次的，
    // 否则同进程多次启动会累积连接，旧回调被新游戏的事件再次触发（双上报/污染）。
    // 文件级静态持有，避免改动公开头文件（保持 SDK 接口稳定）。
    static QList<QMetaObject::Connection> s_launchConns;
    auto releaseLaunchConns = []() {
        for (const auto &c : s_launchConns) QObject::disconnect(c);
        s_launchConns.clear();
    };
    releaseLaunchConns();

    if (onLog) {
        s_launchConns.append(QObject::connect(&launcher, &Launcher::gameLog,
                         [onLog](const QString &line) { onLog(line); }));
    }
    if (onExit) {
        s_launchConns.append(QObject::connect(&launcher, &Launcher::gameExited,
                         [onExit](int code, const QString &) { onExit(code); }));
        // FailedToStart 只发 launchFailed 不发 gameExited——必须转发，否则 CLI 挂死
        s_launchConns.append(QObject::connect(&launcher, &Launcher::launchFailed,
                         [onExit](const QString &) { onExit(-1); }));
    }

    return launcher.launch(version, java, login);
}

QStringList listJavas() {
    auto &jm = JavaManager::instance();
    jm.scanSystemJava();
    jm.waitForScanFinished();  // 扫描是异步的，读结果前必须等完成
    return jm.javaNames();
}

void importModpack(const QString &filePath,
                    const QString &instanceName,
                    const QString &targetInstance,
                    std::function<void(const ImportProgress &)> onProgress,
                    ImportCompleteCallback onComplete) {
    // mod 包（jar-only zip）缺目标实例：直接返回错误 + 当前实例列表
    if (targetInstance.isEmpty() && detectPackType(filePath) == PackType::Mod) {
        if (onComplete) onComplete(false, "此 zip 为 mod 包，需要加 --to <实例名>", listVersions());
        return;
    }
    // 适配：将 ImportProgress 结构体拆成 (status, percent) 传给内部实现
    auto adaptedProgress = [onProgress](const QString &status, int percent) {
        if (onProgress) onProgress({status, percent});
    };
    installModpack(filePath, instanceName, targetInstance, adaptedProgress,
        [onComplete](bool ok, const QString &msg) {
            if (onComplete) onComplete(ok, msg, {});
        });
}

bool removeInstance(const QString &name) {
    if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.contains(".."))
        return false;
    // 用 VersionManager 的运行时目录（尊重 --folder 一次性覆盖），不直接读 Settings
    QString folder = VersionManager::instance().mcFolder();

    // 优先从 INI 映射查找随机目录名
    QString dirName = Settings::instance().dirForDisplayName(name);
    if (!dirName.isEmpty()) {
        QString instanceDir = folder + "instances/" + dirName;
        // 先删 INI 映射，再删目录（即使目录删除失败，映射也已清理）
        Settings::instance().removeInstanceDir(dirName);
        // removeTree 不顺符号链接：实例目录里指向外部的链接只删链接本身
        return FileUtils::removeTree(instanceDir);
    }

    // 回退：直接用显示名作为目录名（兼容旧格式或测试）
    QString instanceDir = folder + "instances/" + name;
    if (!QDir(instanceDir).exists()) return false;
    bool ok = FileUtils::removeTree(instanceDir);
    // 也尝试清理可能残留的 INI 映射
    Settings::instance().removeInstanceDir(name);
    return ok;
}

InstanceInfo instanceInfo(const QString &displayName) {
    InstanceInfo info;
    QString dirName = Settings::instance().dirForDisplayName(displayName);
    if (dirName.isEmpty()) return info;
    QString path = VersionManager::instance().mcFolder() + "instances/" + dirName + "/";
    if (!QDir(path).exists()) return info;

    info.dirName = dirName;
    info.path = path;
    // 启动版本：PCL/Setup.ini 的 Version 键
    QSettings setupIni(path + "PCL/Setup.ini", QSettings::IniFormat);
    info.version = setupIni.value("Setup/Version").toString();
    QDir modsDir(path + "mods");
    info.modCount = modsDir.entryList({"*.jar", "*.jar.disabled"}, QDir::Files).size();
    return info;
}

/// 解析实例 mods/ 目录；fileName 只允许纯文件名（防路径穿越），失败返回空串
static QString resolveModsDir(const QString &displayName, QString *dirNameOut = nullptr) {
    QString dirName = Settings::instance().dirForDisplayName(displayName);
    if (dirName.isEmpty()) return {};
    if (dirNameOut) *dirNameOut = dirName;
    return VersionManager::instance().mcFolder() + "instances/" + dirName + "/mods/";
}

static bool isModFileName(const QString &fileName) {
    if (fileName.isEmpty() || fileName.contains('/') || fileName.contains('\\') || fileName.contains(".."))
        return false;
    return fileName.endsWith(".jar") || fileName.endsWith(".jar.disabled");
}

QList<ModEntry> listMods(const QString &displayName) {
    QList<ModEntry> out;
    QString modsPath = resolveModsDir(displayName);
    if (modsPath.isEmpty()) return out;
    QDir modsDir(modsPath);
    for (const auto &fi : modsDir.entryInfoList({"*.jar", "*.jar.disabled"}, QDir::Files, QDir::Name)) {
        ModEntry e;
        e.fileName = fi.fileName();
        e.size = fi.size();
        e.enabled = fi.fileName().endsWith(".jar");
        out.append(e);
    }
    return out;
}

bool setModEnabled(const QString &displayName, const QString &fileName, bool enabled) {
    if (!isModFileName(fileName)) return false;
    QString modsPath = resolveModsDir(displayName);
    if (modsPath.isEmpty()) return false;

    bool currentlyEnabled = fileName.endsWith(".jar");
    // 已是目标状态：文件存在即视为成功（幂等，本就达成调用方意图）
    if (currentlyEnabled == enabled) return QFile::exists(modsPath + fileName);
    // ".disabled" 共 9 字符：启用 = 去尾，禁用 = 追加
    QString toName = enabled ? fileName.left(fileName.size() - 9)
                             : fileName + ".disabled";
    if (QFile::exists(modsPath + toName)) return false;  // 目标已存在，避免覆盖
    return QFile::rename(modsPath + fileName, modsPath + toName);
}

bool deleteMod(const QString &displayName, const QString &fileName) {
    if (!isModFileName(fileName)) return false;
    QString modsPath = resolveModsDir(displayName);
    if (modsPath.isEmpty()) return false;
    return QFile::remove(modsPath + fileName);
}

ConfigInfo getConfig() {
    ConfigInfo info;
    info.version       = GIT_DESCRIBE;
    info.commit        = GIT_COMMIT_HASH;
    info.gameFolder    = Settings::instance().getString("LaunchFolderSelect");
    info.gameFolderSet = !info.gameFolder.isEmpty();
    info.players       = listPlayers();
    info.selectedPlayer = Settings::instance().selectedPlayer();
    return info;
}

QList<PlayerEntry> listPlayers() {
    QList<PlayerEntry> result;
    QStringList uuids = Settings::instance().playerProfiles();
    for (const auto &uuid : uuids) {
        PlayerEntry e;
        e.uuid     = uuid;
        e.name     = Settings::instance().getProfile(uuid, "Name");
        e.avatar   = Settings::instance().getProfile(uuid, "Avatar");
        e.skinType = Settings::instance().getProfile(uuid, "SkinType", "slim");
        result.append(e);
    }
    return result;
}

// 名称清洗：去掉控制字符（C0/C1，含 ESC 0x1b），避免玩家名/实例名里夹带
// 终端控制序列（会被 TUI 选择器原样写进终端，造成转义注入）
static QString sanitizeDisplayName(const QString &name) {
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        const ushort u = ch.unicode();
        if (u < 0x20 || (u >= 0x7F && u <= 0x9F)) continue;  // 控制字符丢弃
        out.append(ch);
    }
    return out.trimmed();
}

PlayerEntry addPlayer(const QString &name, const QString &avatar, const QString &skinType,
                      const QString &customUuid) {
    const QString safeName = sanitizeDisplayName(name);
    QString uuid = customUuid;
    uuid.remove('{'); uuid.remove('}');
    if (uuid.isEmpty()) uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Settings::instance().setProfile(uuid, "Name", safeName);
    Settings::instance().setProfile(uuid, "Avatar", avatar);
    Settings::instance().setProfile(uuid, "SkinType", skinType);

    // 当前没有选中玩家时自动选中（initDefaults 会预置一个默认 Profile，
    // 因此不能靠 playerProfiles().size()==1 判断"首个"）
    if (Settings::instance().selectedPlayer().isEmpty())
        Settings::instance().selectPlayer(uuid);

    return {uuid, safeName, avatar, skinType};
}

bool updatePlayer(const QString &uuid, const QString &name, const QString &avatar,
                  const QString &skinType, const QString &newUuid) {
    auto &s = Settings::instance();
    if (!s.playerProfiles().contains(uuid)) return false;

    QString target = newUuid;
    target.remove('{'); target.remove('}');
    if (!target.isEmpty() && target != uuid && s.playerProfiles().contains(target))
        return false;  // 目标键已被占用，拒绝覆盖

    const QString safeName = sanitizeDisplayName(name);
    s.setProfile(uuid, "Name", safeName);
    s.setProfile(uuid, "Avatar", avatar);
    s.setProfile(uuid, "SkinType", skinType);

    // 改了配置键：整体迁移，并跟随选中态
    if (!target.isEmpty() && target != uuid) {
        s.setProfile(target, "Name", name);
        s.setProfile(target, "Avatar", avatar);
        s.setProfile(target, "SkinType", skinType);
        bool wasSelected = (s.selectedPlayer() == uuid);
        s.removeProfile(uuid);
        if (wasSelected) s.selectPlayer(target);
    }
    return true;
}

bool removePlayer(const QString &uuid) {
    if (uuid.isEmpty()) return false;
    if (!Settings::instance().playerProfiles().contains(uuid)) return false;
    Settings::instance().removeProfile(uuid);
    if (Settings::instance().selectedPlayer() == uuid) {
        // 删掉的是选中玩家：回退选中剩余首个玩家，没有才置空
        QStringList rest = Settings::instance().playerProfiles();
        Settings::instance().selectPlayer(rest.isEmpty() ? QString() : rest.first());
    }
    return true;
}

bool selectPlayer(const QString &uuid) {
    if (!Settings::instance().playerProfiles().contains(uuid)) return false;
    Settings::instance().selectPlayer(uuid);
    return true;
}

// ---- 外置登录（authlib-injector，如 LittleSkin） ----

// accessToken/clientToken 加密落盘（CryptoUtils DES），服务器/名称/UUID 明文仅用于展示
static void persistAuthlibLogin(const LoginResult &r) {
    auto &s = Settings::instance();
    QString server = r.serverUrl;
    while (server.endsWith('/')) server.chop(1);  // 统一无尾斜杠落盘
    s.setString("Authlib/Server", server);
    s.setEncrypted("Authlib/AccessToken", r.accessToken);
    s.setEncrypted("Authlib/ClientToken", r.clientToken);
    s.setString("Authlib/Name", r.name);
    s.setString("Authlib/Uuid", r.uuid);
}

void loginAuthlib(const QString &serverUrl, const QString &username, const QString &password,
                  AuthlibLoginCallback onComplete) {
    // 尾斜杠归一化下沉到 SDK：AuthlibAuth 只补缺失的 / 不去多余的，
    // 带尾斜杠会拼出 //authserver/...，且原样回显进 r.serverUrl
    QString server = serverUrl;
    while (server.endsWith('/')) server.chop(1);
    auto *auth = new AuthlibAuth();  // 流程结束即 deleteLater 自毁
    auth->setServerUrl(server);
    auth->setUsername(username);
    auth->setPassword(password);
    auth->login([auth, onComplete](bool ok, LoginResult r) {
        auth->deleteLater();
        AuthlibLoginInfo info;
        if (ok) {
            persistAuthlibLogin(r);
            info.loggedIn = true;
            info.server = r.serverUrl;
            info.name = r.name;
            info.uuid = r.uuid;
            if (onComplete) onComplete(true, r.name, info);
        } else {
            if (onComplete) onComplete(false, r.errorMessage, info);
        }
    });
}

void logoutAuthlib() {
    auto &s = Settings::instance();
    s.setString("Authlib/Server", "");
    s.setEncrypted("Authlib/AccessToken", "");
    s.setEncrypted("Authlib/ClientToken", "");
    s.setString("Authlib/Name", "");
    s.setString("Authlib/Uuid", "");
}

AuthlibLoginInfo currentAuthlibLogin() {
    AuthlibLoginInfo info;
    auto &s = Settings::instance();
    QString server = s.getString("Authlib/Server");
    if (!server.isEmpty() && !s.getEncrypted("Authlib/AccessToken").isEmpty()) {
        info.loggedIn = true;
        info.server = server;
        info.name = s.getString("Authlib/Name");
        info.uuid = s.getString("Authlib/Uuid");
    }
    return info;
}

void setCfApiKey(const QString &key) {
    // 空串 = 清除：整行从 ini 移除（内嵌 key 永不得以任何形式落盘，ini 里只许有用户指令设置的密文）
    if (key.isEmpty())
        Settings::instance().removeKey("CfApiKey");
    else
        Settings::instance().setEncrypted("CfApiKey", key);
    // 同步单例（本进程后续 CF 请求立即生效）；清除后回退编译期内嵌
    ModPlatform::instance().setCurseForgeApiKey(
        key.isEmpty() ? QStringLiteral(MLC_CF_API_KEY_EMBEDDED) : key);
}

QString cfApiKeySource() {
    if (!Settings::instance().getEncrypted("CfApiKey").isEmpty()) return "user";
    if (!QStringLiteral(MLC_CF_API_KEY_EMBEDDED).isEmpty()) return "embedded";
    return "none";
}

// ---- 服务端（本地开服） ----

bool installServer(const QString &versionId, const QString &loaderType,
                   const QString &loaderVersion,
                   std::function<void(const ImportProgress &)> onProgress) {
    // 版本清单：拿版本 json 的 URL（空版本号 = 最新正式版）
    json manifest;
    bool ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        DownloadManager::instance().downloadJson(
            "https://launchermeta.mojang.com/mc/game/version_manifest.json",
            [&manifest, cb](bool ok2, QString err, json m) { manifest = std::move(m); cb(ok2, err); });
    });
    if (!ok || !manifest.is_object() || !manifest.contains("versions") || !manifest["versions"].is_array())
        return false;

    QString id = versionId;
    if (id.isEmpty() && manifest.contains("latest") && manifest["latest"].is_object())
        id = QString::fromStdString(manifest["latest"].value("release", ""));
    QString jsonUrl;
    for (const auto &v : manifest["versions"]) {
        if (!v.is_object()) continue;
        QString vid = v.contains("id") && v["id"].is_string()
                          ? QString::fromStdString(v["id"].get<std::string>()) : QString();
        if (vid == id) {
            if (v.contains("url") && v["url"].is_string())
                jsonUrl = QString::fromStdString(v["url"].get<std::string>());
            break;
        }
    }
    if (id.isEmpty() || jsonUrl.isEmpty()) return false;

    // ---- 加载器服务端（Forge / Fabric / NeoForge） ----
    if (!loaderType.isEmpty()) {
        // 解析加载器版本（空 = 最新）
        QString lv = loaderVersion;
        if (lv.isEmpty()) {
            auto &inst = Installer::instance();
            if (loaderType == "fabric") {
                lv = inst.detectBestFabricVersion(id);
            } else {
                ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
                    if (loaderType == "forge")
                        inst.detectBestForgeVersion(id, [&lv, cb](QString v) { lv = v; cb(true, {}); });
                    else if (loaderType == "neoforge")
                        inst.detectBestNeoForgeVersion(id, [&lv, cb](QString v) { lv = v; cb(true, {}); });
                    else
                        cb(false, {});
                });
                if (!ok) return false;
            }
            if (lv.isEmpty()) return false;
        }
        QString serverId = id + "-" + loaderType + "-" + lv;
        QString dir = VersionManager::instance().mcFolder() + "servers/" + serverId + "/";
        QDir().mkpath(dir);

        // 安装器需要 Java：按版本矩阵选
        auto &jm = JavaManager::instance();
        if (jm.javaList().isEmpty()) {
            jm.scanSystemJava();
            jm.waitForScanFinished();
        }
        McVersion ver;
        ver.id = id;
        ver.vanillaVersion = QVersionNumber::fromString(id);
        JavaEntry java = jm.selectJavaForVersion(ver);
        if (java.pathJava.isEmpty()) java = jm.selectJava();
        if (java.pathJava.isEmpty()) return false;

        if (onProgress) onProgress({"Installing " + loaderType + " " + lv + " server...", 30});
        ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
            Installer::instance().installLoaderServer(loaderType, dir, id, lv, java.pathJava,
                [cb](bool ok2, QString err) { cb(ok2, err); });
        });
        if (onProgress) onProgress({ok ? "Complete" : "Installer failed", 100});
        return ok;
    }

    // ---- 原版服务端 ----
    // 版本 json → downloads.server
    json vj;
    ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        DownloadManager::instance().downloadJson(jsonUrl,
            [&vj, cb](bool ok2, QString err, json j) { vj = std::move(j); cb(ok2, err); });
    });
    if (!ok || !vj.is_object()) return false;
    if (!vj.contains("downloads") || !vj["downloads"].is_object()
        || !vj["downloads"].contains("server") || !vj["downloads"]["server"].is_object())
        return false;  // 该版本没有官方服务端 jar（加载器版本走不通这条路）
    const json &srv = vj["downloads"]["server"];
    QString url = srv.contains("url") && srv["url"].is_string()
                      ? QString::fromStdString(srv["url"].get<std::string>()) : QString();
    QString sha1 = srv.contains("sha1") && srv["sha1"].is_string()
                       ? QString::fromStdString(srv["sha1"].get<std::string>()) : QString();
    if (url.isEmpty()) return false;

    QString dir = VersionManager::instance().mcFolder() + "servers/" + id + "/";
    QDir().mkpath(dir);
    QString jar = dir + "server.jar";
    if (QFile::exists(jar) && !sha1.isEmpty() && FileUtils::verifySha1(jar, sha1)) {
        if (onProgress) onProgress({"Server jar up-to-date", 100});
        return true;
    }
    if (onProgress) onProgress({"Downloading server " + id + "...", 30});
    ok = waitForAsync([&](std::function<void(bool, QString)> cb) {
        DownloadManager::instance().download(url, jar, nullptr,
            [cb](bool dlOk, QString) { cb(dlOk, {}); });
    });
    if (!ok) { QFile::remove(jar); return false; }
    if (!sha1.isEmpty() && !FileUtils::verifySha1(jar, sha1)) {
        qWarning() << "server.jar sha1 校验失败:" << id;
        QFile::remove(jar);
        return false;
    }
    if (onProgress) onProgress({"Complete", 100});
    return true;
}

bool startServer(const QString &versionId, LogCallback onLog, ExitCallback onExit) {
    QString dir = VersionManager::instance().mcFolder() + "servers/" + versionId + "/";
    if (!QDir(dir).exists()) return false;

    // EULA 闸门：只检查不接受（接受动作在 CLI 交互里）
    if (!isEulaAccepted(dir)) return false;

    // 最小 server.properties：只在缺失时写（不覆盖用户手改）；
    // online-mode=false 是离线/authlib 外置登录玩家能进服的前提
    if (!QFile::exists(dir + "server.properties")) {
        QFile f(dir + "server.properties");
        if (f.open(QIODevice::WriteOnly))
            f.write("online-mode=false\n");
    }

    auto &jm = JavaManager::instance();
    if (jm.javaList().isEmpty()) {
        jm.scanSystemJava();
        jm.waitForScanFinished();
    }
    // versionId 可能带加载器后缀（1.20.1-forge-47.4.10），矩阵取 MC 版本号部分
    McVersion ver;
    ver.id = versionId;
    ver.vanillaVersion = QVersionNumber::fromString(versionId.section('-', 0, 0));
    JavaEntry java = jm.selectJavaForVersion(ver);
    if (java.pathJava.isEmpty()) java = jm.selectJava();
    if (java.pathJava.isEmpty()) return false;

    int maxMem = Settings::instance().getString("LaunchMaxMemory", "0").toInt();
    if (maxMem <= 0) maxMem = 2048;

    // 布局识别：现代 Forge/NeoForge（unix_args.txt）> 旧版 Forge（universal jar）
    //          > Fabric（fabric-server-launch.jar）> 原版（server.jar）
    QStringList args;
    QStringList memArgs = {"-Xmx" + QString::number(maxMem) + "M", "-Xms512M"};
    QString unixArgs;
    QDirIterator it(dir + "libraries", {"unix_args.txt"}, QDir::Files,
                    QDirIterator::Subdirectories);
    if (it.hasNext()) unixArgs = it.next();
    if (!unixArgs.isEmpty()) {
        args = memArgs;
        if (QFile::exists(dir + "user_jvm_args.txt"))
            args << "@" + dir + "user_jvm_args.txt";
        args << "@" + unixArgs << "nogui";
    } else {
        QString jarToRun;
        for (const auto &pat : {"forge-*-universal.jar", "fabric-server-launch.jar", "server.jar"}) {
            const auto hits = QDir(dir).entryList({pat}, QDir::Files);
            if (!hits.isEmpty()) { jarToRun = dir + hits.first(); break; }
        }
        if (jarToRun.isEmpty()) return false;
        args = memArgs << "-jar" << jarToRun << "nogui";
    }

    auto *proc = new QProcess();
    proc->setWorkingDirectory(dir);
    proc->setProgram(java.pathJava);
    proc->setArguments(args);
    proc->setInputChannelMode(QProcess::ForwardedInputChannel);  // 控制台命令直通（/stop 等）
    if (onLog) {
        QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [proc, onLog]() {
            for (const auto &line : proc->readAllStandardOutput().split('\n'))
                if (!line.isEmpty()) onLog(QString::fromUtf8(line));
        });
        QObject::connect(proc, &QProcess::readyReadStandardError, proc, [proc, onLog]() {
            for (const auto &line : proc->readAllStandardError().split('\n'))
                if (!line.isEmpty()) onLog(QString::fromUtf8(line));
        });
    }
    // 错误处理约定：FailedToStart 不发 finished 由 errorOccurred 上报；
    // Crashed 的终态由 finished 统一上报，errorOccurred 里忽略避免双触发
    QObject::connect(proc, &QProcess::errorOccurred, proc,
        [proc, onExit](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            if (onExit) onExit(-1);
            proc->deleteLater();
        });
    QObject::connect(proc, &QProcess::finished, proc,
        [proc, onExit](int code, QProcess::ExitStatus) {
            if (onExit) onExit(code);
            proc->deleteLater();
        });
    proc->start();
    return true;
}

} // namespace mlc
