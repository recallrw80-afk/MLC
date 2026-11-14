#ifndef MLCCORE_MLC_H
#define MLCCORE_MLC_H

#include <QString>
#include <QList>
#include <functional>
#include "core/mlccore_export.h"

namespace mlc {

// ---- 回调类型（仅异步函数使用） ----

/// 日志回调：(line)
using LogCallback = std::function<void(const QString &line)>;
/// 退出回调：(exitCode)
using ExitCallback = std::function<void(int exitCode)>;

// ---- 结构化数据 ----

/// 当前配置快照
struct ConfigInfo {
    QString version;       // MLC 版本 (GIT_DESCRIBE)
    QString commit;        // Git commit hash
    QString gameFolder;    // 默认游戏目录
    bool    gameFolderSet; // 是否已设置
    QList<struct PlayerEntry> players;
    QString selectedPlayer; // 当前选中的玩家 UUID
};

/// 导入进度步骤
struct ImportProgress {
    QString step;    // 当前步骤描述
    int     percent; // 0~100
};

/// 玩家配置条目
struct PlayerEntry {
    QString uuid;
    QString name;
    QString avatar;   // 头像路径
    QString skinType; // "slim" / "wide"
};

// ---- SDK API ----

/// 列出已导入的整合包实例（从 INI 映射）
MLCCORE_EXPORT QStringList listVersions();

/// 列出已安装的原版 MC 版本（从 versions/ 目录）
MLCCORE_EXPORT QStringList listMcVersions();

/// 安装指定版本（当前为桩实现）
/// 下载原版 MC 版本（json + jar + libraries + assets + natives，同步等待完成）
/// 已存在的文件按 sha1 跳过——重复调用即"校验/补齐"
/// versionId 为空 = 最新正式版（解析官方版本清单 latest.release）
MLCCORE_EXPORT bool installVersion(const QString &versionId,
                                    std::function<void(const ImportProgress &)> onProgress = nullptr);

/// 自动下载并安装指定大版本的 Java（Adoptium JRE，解压到 {mcFolder}/javas/ 并注册）
/// errOut 为失败原因；majorVersion <= 0 时按 8 处理
MLCCORE_EXPORT bool installJavaRuntime(int majorVersion, QString *errOut);

/// 启动游戏（离线模式，异步）
MLCCORE_EXPORT bool launchVersion(const QString &versionId,
                                    LogCallback onLog = nullptr,
                                    ExitCallback onExit = nullptr);

/// 列出系统中可用的 Java 运行时
MLCCORE_EXPORT QStringList listJavas();

/// 导入完成回调：(ok, message, data)
/// data 在"mod 包缺 targetInstance"错误时为当前实例列表，其余情况为空
using ImportCompleteCallback = std::function<void(bool ok, const QString &msg, const QStringList &data)>;

/// 导入整合包（异步）
/// @param targetInstance Mod 包（jar-only zip）的目标实例显示名，其余类型传空
MLCCORE_EXPORT void importModpack(const QString &filePath,
                                    const QString &instanceName,
                                    const QString &targetInstance,
                                    std::function<void(const ImportProgress &)> onProgress,
                                    ImportCompleteCallback onComplete);

/// 删除实例（通过 INI 映射查找随机目录名并删除，移除映射记录）
MLCCORE_EXPORT bool removeInstance(const QString &name);

/// 实例信息
struct InstanceInfo {
    QString dirName;   // 随机目录名（空 = 实例不存在）
    QString path;      // 实例目录（带尾斜杠）
    QString version;   // Setup.ini 的 Version 键（启动用版本 json 名）
    int modCount = 0;  // mods/ 下 jar 数量（含禁用）
};

/// Mod 条目
struct ModEntry {
    QString fileName;  // 文件名（含 .jar / .jar.disabled 后缀）
    qint64 size = 0;
    bool enabled = false; // .jar = 启用，.jar.disabled = 禁用
};

/// 读取实例信息（dirName 为空 = 实例不存在）
MLCCORE_EXPORT InstanceInfo instanceInfo(const QString &displayName);

/// 列出实例的 Mod（按文件名排序）
MLCCORE_EXPORT QList<ModEntry> listMods(const QString &displayName);

/// 启用/禁用 Mod（重命名 .jar ↔ .jar.disabled）
MLCCORE_EXPORT bool setModEnabled(const QString &displayName, const QString &fileName, bool enabled);

/// 删除 Mod 文件（仅限实例 mods/ 目录内的 jar，防路径穿越）
MLCCORE_EXPORT bool deleteMod(const QString &displayName, const QString &fileName);

/// 读取当前配置快照（同步）
MLCCORE_EXPORT ConfigInfo getConfig();

/// 列出所有玩家配置
MLCCORE_EXPORT QList<PlayerEntry> listPlayers();

/// 添加玩家配置（返回生成的条目；skinType: "slim"|"wide"|"default"）
/// customUuid 非空时作为配置键（高级用法，需合法 UUID 格式），留空自动生成
MLCCORE_EXPORT PlayerEntry addPlayer(const QString &name, const QString &avatar = QString(),
                                      const QString &skinType = "slim",
                                      const QString &customUuid = QString());

/// 修改玩家配置（uuid 不存在返回 false；newUuid 非空且不同则迁移配置键）
MLCCORE_EXPORT bool updatePlayer(const QString &uuid, const QString &name,
                                  const QString &avatar, const QString &skinType,
                                  const QString &newUuid = QString());

/// 删除玩家配置（UUID 不存在时返回 false）
MLCCORE_EXPORT bool removePlayer(const QString &uuid);

/// 选择当前玩家（UUID 不存在时返回 false，不修改选中项）
MLCCORE_EXPORT bool selectPlayer(const QString &uuid);

// ---- 外置登录（authlib-injector，如 LittleSkin） ----

/// 外置登录状态快照
struct AuthlibLoginInfo {
    bool    loggedIn = false;
    QString server;  // 认证服务器（yggdrasil 根地址）
    QString name;
    QString uuid;
};

/// 外置登录完成回调：(ok, errorOrName, info) —— ok=false 时 errorOrName 为服务器返回的错误
using AuthlibLoginCallback = std::function<void(bool ok, const QString &errorOrName, const AuthlibLoginInfo &info)>;

/// 外置登录（异步）。成功后自动加密持久化 token：
/// 之后 launchVersion 自动用该账号启动（每次启动在线刷新，失败回退离线）。
MLCCORE_EXPORT void loginAuthlib(const QString &serverUrl, const QString &username,
                                  const QString &password, AuthlibLoginCallback onComplete);

/// 清除持久化的外置登录态（launch 回退为离线玩家）
MLCCORE_EXPORT void logoutAuthlib();

/// 当前持久化的外置登录信息（纯本地读取，无网络）
MLCCORE_EXPORT AuthlibLoginInfo currentAuthlibLogin();

/// 设置用户级 CurseForge API key（加密持久化；空串 = 清除，回退编译期内嵌）。
/// 解析优先级：指令设置 > 编译期内嵌 > 空（走 MCIM 镜像）。
MLCCORE_EXPORT void setCfApiKey(const QString &key);

/// CF key 当前来源："user"（指令设置）/ "embedded"（编译期内嵌）/ "none"（无，走镜像）。
/// 注意：内嵌 key 禁止回显——任何状态展示只能用本函数的来源标识，不得输出 key 本体
MLCCORE_EXPORT QString cfApiKeySource();

// ---- 服务端（本地开服） ----

/// 下载/安装服务端到 {mcFolder}/servers/<标识>/（标识 = 版本 或 版本-加载器-加载器版本）。
/// loaderType 为空 = 原版（sha1 校验，已存在跳过）；传 "forge"/"fabric"/"neoforge" 走对应安装器
/// （--installServer / fabric server 模式），loaderVersion 空 = 自动解析最新。
/// 返回 false 即失败（原因经 onProgress/日志）。
MLCCORE_EXPORT bool installServer(const QString &versionId,
                                   const QString &loaderType = QString(),
                                   const QString &loaderVersion = QString(),
                                   std::function<void(const ImportProgress &)> onProgress = nullptr);

/// 前台运行服务端（nogui，控制台输入直通；进程退出经 onExit 回调）。
/// 需先 installServer 且在服务端目录写好 eula.txt（eula=true）——EULA 确认由调用方负责。
/// 首次启动自动生成最小 server.properties（online-mode=false，离线/外置登录玩家进服的前提）。
MLCCORE_EXPORT bool startServer(const QString &versionId,
                                 LogCallback onLog = nullptr,
                                 ExitCallback onExit = nullptr);

} // namespace mlc

#endif // MLCCORE_MLC_H
