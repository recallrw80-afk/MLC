#include "core/launcher.h"
#include "core/launchbuilder.h"
#include "core/versionmanager.h"
#include "core/settings.h"
#include "core/javamanager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>

static Q_LOGGING_CATEGORY(logLaunch, "mlc.launcher")

Launcher& Launcher::instance() {
    static Launcher l;
    return l;
}

Launcher::Launcher() {
    m_gameProcess = new QProcess(this);
    m_gameProcess->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_gameProcess, &QProcess::readyReadStandardOutput,
            this, &Launcher::onGameStdout);
    connect(m_gameProcess, &QProcess::readyReadStandardError,
            this, &Launcher::onGameStderr);
    connect(m_gameProcess, &QProcess::started,
            this, &Launcher::onGameStarted);
    connect(m_gameProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &Launcher::onGameFinished);
    connect(m_gameProcess, &QProcess::errorOccurred,
            this, &Launcher::onGameError);
}

// Launch

bool Launcher::launch(const McVersion &version,
                       const JavaEntry &java,
                       const LoginResult &login,
                       const McLaunchOptions &options) {
    if (m_state == LaunchState::Running || m_state == LaunchState::Launching) {
        qCWarning(logLaunch) << "Game already running or launching";
        return false;
    }

    // interrupt() 的 kill 是异步的：旧进程未退出就复用 QProcess，start() 会被
    // 静默吞掉（不发 errorOccurred）——先等它真正退出。此刻 m_state 尚未切换，
    // 旧进程的 finished() 由 onGameFinished 按 Interrupted 等当前状态正常消化
    if (m_gameProcess->state() != QProcess::NotRunning) {
        m_gameProcess->kill();
        if (!m_gameProcess->waitForFinished(3000)) {
            // 旧进程杀不掉时 start() 必然被吞，必须主动报失败
            setState(LaunchState::Failed);
            emit launchFailed("Previous game process did not exit, cannot relaunch");
            return false;
        }
    }

    // Validate
    if (!QFileInfo::exists(java.pathJava)) {
        qCWarning(logLaunch) << "Java not found:" << java.pathJava;
        setState(LaunchState::Failed);
        emit launchFailed("Java not found: " + java.pathJava);
        return false;
    }

    m_version = version;
    m_java = java;
    m_login = login;
    m_options = options;

    // Load version JSON
    setState(LaunchState::Prechecking);
    setStatus("Loading version JSON...");

    // Resolve inheritance chain
    json versionJson = VersionManager::resolveInheritanceChain(
        m_version.pathVersion + m_version.id + ".json");

    LaunchBuilder::instance().setVersionJson(versionJson);
    LaunchBuilder::instance().setLaunchOptions(m_options);

    setState(LaunchState::Launching);
    setStatus("Building launch arguments...");

    if (!LaunchBuilder::instance().build(m_version, m_java, m_login)) {
        setState(LaunchState::Failed);
        emit launchFailed("Failed to build launch arguments");
        return false;
    }

    doLaunch();
    return true;
}

bool Launcher::launchVersion(const QString &versionId, const LoginResult &login) {
    if (m_state == LaunchState::Running || m_state == LaunchState::Launching) {
        qCWarning(logLaunch) << "Game already running or launching";
        return false;
    }

    // 1. Load version
    setState(LaunchState::Prechecking);
    setStatus("正在检查版本...");
    setProgress(5);

    auto &verMgr = VersionManager::instance();
    McVersion version = verMgr.loadVersion(versionId);
    if (!version.isValid) {
        setState(LaunchState::Failed);
        emit launchFailed("Cannot load version: " + versionId);
        return false;
    }

    // 2. Get Java
    setState(LaunchState::GettingJava);
    setStatus("正在查找 Java...");
    setProgress(10);

    auto &javaMgr = JavaManager::instance();
    auto javaList = javaMgr.javaList();
    if (javaList.isEmpty()) {
        javaMgr.scanSystemJava();
        javaMgr.waitForScanFinished();  // 扫描是异步的，读结果前必须等完成
        javaList = javaMgr.javaList();
    }
    if (javaList.isEmpty()) {
        setState(LaunchState::Failed);
        emit launchFailed("No Java runtime found. Please install Java.");
        return false;
    }

    // Pick best Java for this version
    JavaEntry bestJava;
    // 用户在 GUI Java 管理页指定的 Java 优先（LaunchJavaSelect = java 可执行文件路径，空 = 自动）
    QString userJava = Settings::instance().getString("LaunchJavaSelect");
    if (!userJava.isEmpty()) {
        for (const auto &j : javaList) {
            if (j.pathJava == userJava) { bestJava = j; break; }
        }
        if (bestJava.pathJava.isEmpty())
            qCWarning(logLaunch) << "用户指定的 Java 不存在，回退自动选择:" << userJava;
    }
    if (bestJava.pathJava.isEmpty()) {
        bestJava = javaList.first();
        // MC 1.x 的特性版本号在第二段（"1.20.1" → 20），majorVersion() 恒为 1
        int feature = version.vanillaVersion.majorVersion() == 1
            ? version.vanillaVersion.minorVersion()
            : version.vanillaVersion.majorVersion();
        int targetMajor = feature > 0 ? feature : 8;
        for (const auto &j : javaList) {
            if (j.majorVersion >= targetMajor && j.is64Bit == is64BitSystem()) {
                bestJava = j;
                break;
            }
        }
    }

    // 3. Login state is supplied by the caller — no hardcoded offline assumption.
    setState(LaunchState::LoggingIn);
    setStatus("正在设置登录...");
    setProgress(15);

    // 4. Build launch options from settings
    McLaunchOptions options;
    // LaunchMaxMemory：0（默认）= 自动（在 LaunchBuilder 统一收口）；>0 = 固定值
    options.maxMemoryMB = Settings::instance().getString("LaunchMaxMemory", "0").toInt();
    options.minMemoryMB = Settings::instance().getString("LaunchMinMemory", "512").toInt();
    QString fsValue = Settings::instance().getString("LaunchFullscreen", "false").toLower();
    options.fullscreen = (fsValue == "true" || fsValue == "1");
    options.windowWidth = Settings::instance().getString("LaunchWidth", "854").toInt();
    options.windowHeight = Settings::instance().getString("LaunchHeight", "480").toInt();

    // 5. Delegate to the full launch method
    return launch(version, bestJava, login, options);
}

void Launcher::doLaunch() {
    setState(LaunchState::Launching);
    setStatus("Starting game process...");

    openLaunchLog();

    QString javaExe = m_java.pathJava;

    // Build the full command
    QStringList allArgs;
    auto repl = LaunchBuilder::instance().buildReplacements(m_version, m_java, m_login);
    allArgs.append(LaunchBuilder::instance().applyReplacements(
        LaunchBuilder::instance().jvmArgs(), repl));
    allArgs.append(LaunchBuilder::instance().mainClass());
    allArgs.append(LaunchBuilder::instance().applyReplacements(
        LaunchBuilder::instance().gameArgs(), repl));

    // Log the command
    QString cmdLog = javaExe;
    for (const auto &arg : allArgs) {
        // 仅显示用：含空格的参数加引号（exec 参数本身保持原样）
        cmdLog += " " + (arg.contains(' ') ? "\"" + arg + "\"" : arg);
    }
    // 凭据打码：authlib 外置登录后 --accessToken 是真 token，进 Qt 日志/落盘前必须掩掉
    // （只处理这行命令行回显；游戏自身输出不经过这里，保持原样）
    static const QRegularExpression tokenRe("(--accessToken\\s+)\\S+");
    cmdLog.replace(tokenRe, "\\1***");
    qCInfo(logLaunch) << "Launch command:" << cmdLog.toUtf8().constData();
    appendLog("> " + cmdLog);
    appendLog("");

    // GLFW 3.4 中文输入修复：mlc::launchVersion 已把 lwjgl-glfw natives jar 换成
    // 3.3.6（标记 libglfw.so.glfw34-fixed），LWJGL 提取出来的就是 GLFW 3.4
    QString mcFolder = VersionManager::instance().mcFolder();
    QString vanillaId = m_version.vanillaVersion.toString();
    bool glfw34Fixed = !vanillaId.isEmpty()
        && QFileInfo::exists(mcFolder + "versions/" + vanillaId + "/natives/libglfw.so.glfw34-fixed");
    if (glfw34Fixed)
        appendLog("[MLC] 使用 GLFW 3.4（IME 修复），保留中文输入");

    // Set up environment
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // PATH: add Java bin
    QString pathVar = env.value("PATH");
    QString javaBinDir = QDir(m_java.pathFolder).absolutePath();
    if (currentPlatform() == Platform::Windows) {
        pathVar = javaBinDir + ";" + pathVar;
    } else {
        pathVar = javaBinDir + ":" + pathVar;
    }
    env.insert("PATH", pathVar);

    // APPDATA / game directory
    QString gameDir = m_version.pathIndie;
    // Remove trailing slash
    if (gameDir.endsWith('/') || gameDir.endsWith('\\')) {
        gameDir.chop(1);
    }
    // APPDATA 仅 Windows 下 MC 用于定位 .minecraft，其他平台无意义
    if (currentPlatform() == Platform::Windows)
        env.insert("APPDATA", gameDir);

    // Minecraft-specific env
    env.insert("MINECRAFT_LAUNCHER_NAME", "MLC");
    env.insert("MINECRAFT_LAUNCHER_VERSION", "0.1");

    // NVIDIA Linux 驱动的 threaded optimizations 会在渲染线程随机 SIGSEGV
    // （glfwWaitEventsTimeout 里跳到坏地址）。Embeddium 也会禁它，但 FML 的
    // 早期加载窗口在它之前 5 秒就建了 GL 上下文，进程内 setenv 已太晚——
    // 必须在启动器层随环境变量注入，进程一启动就生效。
    // 只有 NVIDIA 驱动读这个变量，其他显卡设置无副作用。
    if (!env.contains("__GL_THREADED_OPTIMIZATIONS"))
        env.insert("__GL_THREADED_OPTIMIZATIONS", "0");

#if defined(Q_OS_LINUX)
    // fcitx/ibus 的 XIM 模块会让 GLFW 3.3 在 glfwWaitEventsTimeout 里 SIGSEGV
    // （原版 MC 也会在窗口出现后几秒内崩，pc=0xc906 固定坏地址，已实测）。
    // 首选 GLFW 3.4 修复（上面 glfw34Fixed 为真时 XIM 无害，可打中文，跳过禁用）；
    // 否则回退：强制 XMODIFIERS=@im=none——副作用是游戏内不能用输入法，但换来不崩溃。
    if (!env.contains("MLC_KEEP_XIM") && !glfw34Fixed) {
        if (env.value("XMODIFIERS") != "@im=none") {
            env.insert("XMODIFIERS", "@im=none");
            appendLog("[MLC] 已禁用 XIM 输入法钩子（XMODIFIERS=@im=none），"
                      "规避 fcitx/ibus 导致的 GLFW 崩溃");
        }
    }
#endif

    // 无显示环境（无 GUI）：用 xvfb-run 虚拟显示包装启动
    QString program = javaExe;
    QStringList finalArgs = allArgs;
    bool headless = qEnvironmentVariableIsEmpty("DISPLAY") &&
                    qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    if (headless) {
        QString xvfb = QStandardPaths::findExecutable("xvfb-run");
        if (!xvfb.isEmpty()) {
            appendLog("[MLC] 无显示环境，使用 xvfb 虚拟显示启动");
            program = xvfb;
            finalArgs.prepend(javaExe);
            finalArgs.prepend("-a");  // 自动分配 display 号
        } else {
            qCWarning(logLaunch) << "无显示环境且未找到 xvfb-run——"
                    "游戏需要 X 显示才能创建窗口，请安装 xvfb 或配置 DISPLAY";
        }
    }

    m_gameProcess->setProcessEnvironment(env);
    m_gameProcess->setWorkingDirectory(gameDir);
    m_gameProcess->setProgram(program);
    m_gameProcess->setArguments(finalArgs);

    // Start asynchronously. The Running state and gameStarted() signal are
    // emitted from onGameStarted() (connected to QProcess::started) so we
    // never block the Qt event loop. Start failures arrive via errorOccurred.
    m_gameProcess->start();
}

void Launcher::onGameStarted() {
    // 中断竞态：start 已异步发出，started 信号可能在 interrupt() 之后到达——不得覆盖中断状态
    if (m_state == LaunchState::Interrupted) return;
    setState(LaunchState::Running);
    setStatus("Game running...");
    emit gameStarted();

    qCInfo(logLaunch) << "Game process started, PID:" << m_gameProcess->processId();
}

void Launcher::interrupt() {
    if (m_state == LaunchState::Running || m_state == LaunchState::Launching) {
        // Launching 状态下进程可能已异步 start——必须真的 kill，否则取消无效
        if (m_gameProcess->state() != QProcess::NotRunning)
            m_gameProcess->kill();
        setState(LaunchState::Interrupted);
        setStatus("Interrupted");
        appendLog("[MLC] Game process killed by user.");
    } else if (m_state == LaunchState::Downloading) {
        setState(LaunchState::Interrupted);
        setStatus("Interrupted");
    }
}

// Game output

void Launcher::processGameOutput(QByteArray &buffer, const QByteArray &data) {
    buffer += data;
    // 字节级缓冲：多字节 UTF-8 字符跨 readyRead 边界时不会切碎（'\n' 不会出现在多字节序列中）
    int idx;
    while ((idx = buffer.indexOf('\n')) != -1) {
        QString line = QString::fromUtf8(buffer.left(idx)).trimmed();
        buffer.remove(0, idx + 1);
        if (!line.isEmpty()) {
            emit gameLog(line);
            appendLog(line);
        }
    }
}

void Launcher::flushLogBuffer(QByteArray &buffer) {
    QString line = QString::fromUtf8(buffer).trimmed();
    buffer.clear();
    if (!line.isEmpty()) {
        emit gameLog(line);
        appendLog(line);
    }
}

void Launcher::onGameStdout() {
    processGameOutput(m_logBuffer, m_gameProcess->readAllStandardOutput());
}

void Launcher::onGameStderr() {
    processGameOutput(m_logBufferErr, m_gameProcess->readAllStandardError());
}

void Launcher::onGameFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    // 冲刷两通道中无换行结尾的残留日志
    flushLogBuffer(m_logBuffer);
    flushLogBuffer(m_logBufferErr);

    // 用户主动中断时不再覆盖状态
    if (m_state == LaunchState::Interrupted) {
        emit gameExited(exitCode, "Interrupted by user");
        if (m_logFile.isOpen()) m_logFile.close();
        return;
    }
    if (exitStatus == QProcess::CrashExit) {
        appendLog(QString("[MLC] Game crashed with exit code %1").arg(exitCode));
        setState(LaunchState::Failed);
        emit gameExited(exitCode, "Game crashed");
    } else if (exitCode != 0) {
        // JVM 正常启动但以非零码退出（参数错误、mainClass 缺失等）——不是"正常退出"
        appendLog(QString("[MLC] Game exited abnormally with code %1").arg(exitCode));
        setState(LaunchState::Failed);
        emit gameExited(exitCode, "Game exited abnormally");
    } else {
        appendLog(QString("[MLC] Game exited with code %1").arg(exitCode));
        setState(LaunchState::Finished);
        setStatus("Game exited");
        emit gameExited(exitCode, "Game exited normally");
    }
    if (m_logFile.isOpen()) m_logFile.close();
}

void Launcher::onGameError(QProcess::ProcessError error) {
    if (error == QProcess::Crashed) {
        // 崩溃时 QProcess 还会发 finished()——终态统一由 onGameFinished 上报，
        // 避免 launchFailed + gameExited 双触发（且运行中崩溃不是"启动失败"）
        return;
    }
    // 用户已取消：异步错误（kill 触发的 abort 等）不得把 Interrupted 覆盖成 Failed
    if (m_state == LaunchState::Interrupted) return;

    // 只有 FailedToStart 才是"启动失败"（进程从未起来，不会再有 finished）。
    // 运行期 I/O/超时错误时进程仍在跑：终态交给 finished() 统一上报，
    // 否则会先发 launchFailed、退出时再发 gameExited，双上报且污染运行中状态。
    if (error != QProcess::FailedToStart) {
        appendLog("[MLC] Game process I/O error: " + m_gameProcess->errorString());
        qCWarning(logLaunch) << "Game process error (non-fatal, waiting for exit):"
                             << m_gameProcess->errorString();
        return;
    }

    QString errMsg = "Failed to start game process. Check Java installation.";

    appendLog("[MLC] ERROR: " + errMsg);
    qCWarning(logLaunch) << errMsg;
    setState(LaunchState::Failed);
    if (m_logFile.isOpen()) m_logFile.close();
    emit launchFailed(errMsg);
}

// State management

void Launcher::setState(LaunchState newState) {
    if (m_state == newState) return;
    m_state = newState;
    emit stateChanged();

    if (newState == LaunchState::Failed) {
        setStatus("Failed");
        m_progress = 0;
        emit progressChanged();
    } else if (newState == LaunchState::Finished) {
        setStatus("Finished");
        m_progress = 100;
        emit progressChanged();
    }
}

void Launcher::setStatus(const QString &text) {
    m_statusText = text;
    emit statusTextChanged();
}

void Launcher::setProgress(int value) {
    if (m_progress == value) return;
    m_progress = value;
    emit progressChanged();
}

void Launcher::appendLog(const QString &line) {
    // Log to Qt logging system
    static QLoggingCategory logCat("mlc.game");
    qCInfo(logCat).noquote() << line;

    // 落盘：每次启动一份文件，攒 100 行刷一次（游戏日志量大，逐行 flush 太贵）
    if (m_logFile.isOpen()) {
        m_logFile.write(line.toUtf8());
        m_logFile.write("\n");
        if (++m_logLinesSinceFlush >= 100) {
            m_logFile.flush();
            m_logLinesSinceFlush = 0;
        }
    }

    // The UI can connect to gameLog() signal for display
}

void Launcher::openLaunchLog() {
    if (m_logFile.isOpen()) m_logFile.close();
    // 跟随本次启动实际生效的游戏目录：mcFolder() 含 CLI --folder 的一次性覆盖
    // （直接读 Settings 的 LaunchFolderSelect 会在覆盖时把日志写进旧目录）；
    // mcFolder() 自带默认值兜底、永不为空且带尾斜杠
    QString mcFolder = VersionManager::instance().mcFolder();
    QDir logDir(mcFolder + "logs");
    if (!logDir.mkpath(".")) return;

    // 滚动清理：只留最近 9 份，加上本次共 10 份
    const auto old = logDir.entryInfoList({"mlc-launch-*.log"}, QDir::Files, QDir::Time);
    for (int i = 9; i < old.size(); ++i)
        QFile::remove(old[i].absoluteFilePath());

    QString name = "mlc-launch-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".log";
    m_logFile.setFileName(logDir.filePath(name));
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(logLaunch) << "Cannot open launch log file:" << m_logFile.fileName();
        return;
    }
    m_logLinesSinceFlush = 0;
    appendLog("[MLC] Launch log: " + m_logFile.fileName());
}
