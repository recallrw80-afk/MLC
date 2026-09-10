//! mlc — 命令行前端（Rust 重写版）
//!
//! 只做参数解析与派发，不写业务逻辑（对应原 C++ cli/ 的分工）。
//! 命令清单见 docs/rust-inventory.md 表 1；名称/参数/退出码须逐条复刻。

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use clap::{Parser, Subcommand};

use mlccore::settings::{self, Settings};
use mlccore::util::platform;

// ---------------------------------------------------------------- CLI

#[derive(Parser)]
#[command(
    name = "mlc",
    version = mlccore::GIT_DESCRIBE,
    about = "MLC — MinecraftLauncherCLI (Rust rewrite WIP)",
    disable_help_subcommand = false
)]
struct Cli {
    /// 游戏目录覆盖（仅本次命令，不写配置） / Override mc folder for this run
    #[arg(long = "folder", global = true, value_name = "路径")]
    folder: Option<PathBuf>,

    #[command(subcommand)]
    command: Option<CliCommand>,
}

#[derive(Subcommand)]
enum CliCommand {
    /// 列出整合包实例 / List instances
    List,
    /// 列出实例 Mods / List mods of an instance
    Mods { name: String },
    /// 列出已安装的 MC 版本 / List installed MC versions
    McList,
    /// 删除整合包实例 / Remove an instance
    ListRm {
        /// 实例显示名
        name: String,
        /// 删除全部实例
        #[arg(long)]
        all: bool,
    },
    /// 安装原版 MC（空=最新正式版） / Install vanilla MC
    McInstall {
        /// 版本号，空 = 最新正式版
        version: Option<String>,
    },
    /// 安装 Adoptium JRE / Install Adoptium JRE
    JavaInstall {
        /// Java 大版本（如 17、21）；≤0 按 8
        major: i32,
    },
    /// 列出检测到的 Java / List detected Javas
    ListJavas,
    /// 显示配置 / Show config
    Config,
    /// 设置游戏目录 / Set game folder
    SetFolder { path: PathBuf },
    /// 设置界面语言 / Set UI language
    SetLang { lang: String },
    /// 设置最大内存（MB 或 auto） / Set max memory
    SetMem { mem: String },
    /// 设置/清除 CF API key / Set or clear CurseForge key
    SetCfKey {
        /// key 正文；与 --clear 二选一
        key: Option<String>,
        /// 清除用户 key
        #[arg(long)]
        clear: bool,
    },
    /// 列出玩家档案 / List players
    PlayerList,
    /// 添加离线玩家 / Add offline player
    PlayerAdd {
        name: String,
        #[arg(long, default_value = "")]
        avatar: String,
        #[arg(long, default_value = "default")]
        skin: String,
    },
    /// 删除玩家 / Remove player
    PlayerRm { uuid: String },
    /// 选择当前玩家 / Select player
    PlayerSelect { uuid: String },
    /// 编辑玩家档案 / Edit player profile
    PlayerEdit {
        /// 原 UUID
        uuid: String,
        #[arg(long, default_value = "")]
        name: String,
        #[arg(long, default_value = "")]
        avatar: String,
        #[arg(long, default_value = "")]
        skin: String,
        /// 迁移到新 UUID
        #[arg(long, default_value = "")]
        new_uuid: String,
    },
    /// Authlib 外置登录 / Authlib-injector login
    Login {
        server: String,
        username: String,
        /// 密码（不传则从 stdin 读一行）
        password: Option<String>,
    },
    /// 退出外置登录 / Logout authlib
    Logout,
    /// 导入整合包 / Import modpack
    Inpack {
        file: PathBuf,
        /// 实例显示名
        #[arg(long = "r")]
        rename: Option<String>,
        /// Mod 包目标实例
        #[arg(long = "to")]
        target: Option<String>,
    },
    /// 启动游戏 / Launch the game
    Launch {
        /// 实例显示名或版本号；空 = 若无实例则报错
        name: Option<String>,
    },
    /// 全系统自检 / Full system self-check
    Test,
    /// 安装本地服务端 / Install local MC server
    ServerInstall {
        /// MC 版本；空 = 最新正式版
        version: Option<String>,
        /// Forge 加载器（可接版本号）
        #[arg(long, num_args = 0..=1, default_missing_value = "")]
        forge: Option<String>,
        /// Fabric 加载器（可接版本号）
        #[arg(long, num_args = 0..=1, default_missing_value = "")]
        fabric: Option<String>,
        /// NeoForge 加载器（可接版本号）
        #[arg(long, num_args = 0..=1, default_missing_value = "")]
        neoforge: Option<String>,
        /// 从实例复制 mods/config
        #[arg(long)]
        from: Option<String>,
    },
    /// 启动本地服务端 / Start local MC server
    ServerStart {
        /// 服务端标识（版本 或 版本-加载器-版本）
        id: String,
        /// 同意 Minecraft EULA
        #[arg(long)]
        eula: bool,
    },
    /// 检查并更新 / Check and update
    Update {
        /// 包含预发布
        #[arg(short, long)]
        beta: bool,
    },
    /// 卸载（仅 install.sh 安装副本） / Uninstall
    Uninstall {
        /// 保留游戏目录内容
        #[arg(short, long)]
        keep: bool,
    },
    /// 生成 GitHub Issue 预填链接 / Prefilled GitHub issue link
    Report {
        /// 问题描述；空则用默认标题
        description: Vec<String>,
    },
    /// 显示版本号 / Show version
    Version,
}

// ---------------------------------------------------------------- 入口

fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .init();

    let cli = Cli::parse();
    settings::initialize(None);

    let code = match run(cli) {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("{e}");
            1
        }
    };
    std::process::exit(code);
}

fn run(cli: Cli) -> Result<(), String> {
    // --folder 一次性覆盖（不写配置）
    if let Some(folder) = &cli.folder {
        let p = platform::normalize_path_string(folder);
        let p = if p.ends_with('/') { p } else { format!("{p}/") };
        settings::with_global(|s| s.set_string("LaunchFolderSelect", &p));
    }

    let Some(cmd) = cli.command else {
        println!("mlc {}", mlccore::GIT_DESCRIBE);
        return Ok(());
    };

    match cmd {
        CliCommand::Version => {
            println!("mlc {}", mlccore::GIT_DESCRIBE);
            Ok(())
        }
        CliCommand::List => with_settings(list_instances),
        CliCommand::McList => with_settings(list_mc_versions),
        CliCommand::ListRm { name, all } => with_settings(|s| list_rm(s, &name, all)),
        CliCommand::Mods { name } => with_settings(|s| list_mods(s, &name)),
        CliCommand::McInstall { version } => block_on(install_mc(version.as_deref().unwrap_or(""))),
        CliCommand::JavaInstall { major } => block_on(install_java(major)),
        CliCommand::ListJavas => with_settings(list_javas),
        CliCommand::Config => with_settings(show_config),
        CliCommand::SetFolder { path } => with_settings(|s| set_folder(s, &path)),
        CliCommand::SetLang { lang } => with_settings(|s| set_lang(s, &lang)),
        CliCommand::SetMem { mem } => with_settings(|s| set_mem(s, &mem)),
        CliCommand::SetCfKey { key, clear } => {
            with_settings(|s| set_cf_key(s, key.as_deref(), clear))
        }
        CliCommand::PlayerList => with_settings(player_list),
        CliCommand::PlayerAdd { name, avatar, skin } => {
            with_settings(|s| player_add(s, &name, &avatar, &skin))
        }
        CliCommand::PlayerRm { uuid } => with_settings(|s| player_rm(s, &uuid)),
        CliCommand::PlayerSelect { uuid } => with_settings(|s| player_select(s, &uuid)),
        CliCommand::PlayerEdit {
            uuid,
            name,
            avatar,
            skin,
            new_uuid,
        } => with_settings(|s| player_edit(s, &uuid, &name, &avatar, &skin, &new_uuid)),
        CliCommand::Login {
            server,
            username,
            password,
        } => block_on(login(server, username, password)),
        CliCommand::Logout => with_settings(logout),
        CliCommand::Inpack {
            file,
            rename,
            target,
        } => with_settings(|s| inpack(s, &file, rename.as_deref(), target.as_deref())),
        CliCommand::Launch { name } => block_on(launch(name.as_deref().unwrap_or(""))),
        CliCommand::Test => run_self_test(),
        CliCommand::ServerInstall {
            version,
            forge,
            fabric,
            neoforge,
            from,
        } => block_on(server_install(
            version.as_deref().unwrap_or(""),
            forge.as_deref(),
            fabric.as_deref(),
            neoforge.as_deref(),
            from.as_deref(),
        )),
        CliCommand::ServerStart { id, eula } => with_settings(|s| server_start(s, &id, eula)),
        CliCommand::Update { beta } => block_on(do_update(beta)),
        CliCommand::Uninstall { keep } => mlccore::update::uninstall(keep),
        CliCommand::Report { description } => with_settings(|s| report(s, &description)),
    }
}

fn with_settings<T>(f: impl FnOnce(&mut Settings) -> Result<T, String>) -> Result<T, String> {
    match settings::with_global(f) {
        Some(r) => r,
        None => Err("Settings not initialized".to_string()),
    }
}

fn block_on<F, T>(f: F) -> Result<T, String>
where
    F: std::future::Future<Output = Result<T, String>>,
{
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| e.to_string())?
        .block_on(f)
}

fn mc_folder(s: &Settings) -> PathBuf {
    mlccore::version::resolve_mc_folder(s)
}

// ---------------------------------------------------------------- 实例/版本

fn list_instances(s: &mut Settings) -> Result<(), String> {
    let mc = mc_folder(s);
    let list = mlccore::version::list_instances(s, &mc);
    if list.is_empty() {
        println!("（无实例） / (no instances)");
        return Ok(());
    }
    for info in list {
        println!("{}", info.id);
    }
    Ok(())
}

fn list_mc_versions(s: &mut Settings) -> Result<(), String> {
    let mc = mc_folder(s);
    let list = mlccore::version::list_mc_versions(&mc);
    if list.is_empty() {
        println!("（无已安装版本） / (no installed versions)");
        return Ok(());
    }
    for info in list {
        println!("{}  {}", info.id, info.kind);
    }
    Ok(())
}

fn list_rm(s: &mut Settings, name: &str, all: bool) -> Result<(), String> {
    let mc = mc_folder(s);
    if all || name == "*" {
        let list = mlccore::version::list_instances(s, &mc);
        let n = list.len();
        for info in list {
            if mlccore::version::remove_instance(s, &mc, &info.id) {
                println!("已删除: {}", info.id);
            }
        }
        println!("共删除 {n} 个实例");
        return Ok(());
    }
    if name.is_empty() {
        return Err("用法: mlc list-rm <名称>  或  mlc list-rm --all".into());
    }
    if mlccore::version::remove_instance(s, &mc, name) {
        println!("已删除: {name}");
        Ok(())
    } else {
        Err(format!("实例不存在: {name}"))
    }
}

fn list_mods(s: &mut Settings, display_name: &str) -> Result<(), String> {
    let mc = mc_folder(s);
    let dir_name = s
        .dir_for_display_name(display_name)
        .ok_or_else(|| format!("实例不存在: {display_name}"))?;
    let mods_dir = mc.join("instances").join(&dir_name).join("mods");
    let entries = std::fs::read_dir(&mods_dir).map_err(|e| e.to_string())?;
    let mut names: Vec<(String, bool)> = Vec::new();
    for e in entries.flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        if n.ends_with(".jar") {
            names.push((n, true));
        } else if n.ends_with(".jar.disabled") {
            names.push((n, false));
        }
    }
    names.sort();
    if names.is_empty() {
        println!("（无 mods） / (no mods)");
        return Ok(());
    }
    for (n, enabled) in names {
        println!("{}{}", n, if enabled { "" } else { "  [disabled]" });
    }
    Ok(())
}

// ---------------------------------------------------------------- Java / config

fn list_javas(s: &mut Settings) -> Result<(), String> {
    let mc = mc_folder(s);
    let list = mlccore::java::scan_system_java(&mc);
    if list.is_empty() {
        println!("未检测到 Java / No Java found");
        return Ok(());
    }
    for j in list {
        println!("{}", j.display());
    }
    Ok(())
}

fn show_config(s: &mut Settings) -> Result<(), String> {
    let folder = s.get_string("LaunchFolderSelect").unwrap_or_default();
    println!("version:      {}", mlccore::GIT_DESCRIBE);
    println!("commit:       {}", mlccore::GIT_COMMIT_HASH);
    println!("gameFolder:   {folder}");
    println!("folderSet:    {}", !folder.is_empty());
    let players = mlccore::auth::list_players(s);
    let selected = s.selected_player().unwrap_or_default();
    println!("selected:     {selected}");
    println!("players:      {}", players.len());
    for p in players {
        let mark = if p.uuid == selected { "*" } else { " " };
        println!("  {mark} {}  {}", p.uuid, p.name);
    }
    let auth = mlccore::auth::current_authlib_login(s);
    if auth.logged_in {
        println!("authlib:      {}  {}", auth.server, auth.name);
    }
    Ok(())
}

// ---------------------------------------------------------------- 设置

fn set_folder(s: &mut Settings, path: &Path) -> Result<(), String> {
    if !path.exists() && !path.is_dir() {
        // 允许设置尚不存在的目录？C++ 用 QDir::absolutePath，不要求存在
    }
    let mut p = platform::normalize_path_string(path);
    if !p.ends_with('/') {
        p.push('/');
    }
    s.set_string("LaunchFolderSelect", &p);
    println!("游戏目录已设置: {p}");
    Ok(())
}

fn set_lang(s: &mut Settings, lang: &str) -> Result<(), String> {
    if lang != "en" && lang != "zh" {
        return Err("语言仅支持 en / zh".into());
    }
    s.set_string("UiLanguage", lang);
    println!("UiLanguage = {lang}");
    Ok(())
}

fn set_mem(s: &mut Settings, mem: &str) -> Result<(), String> {
    if mem.eq_ignore_ascii_case("auto") {
        s.set_string("LaunchMaxMemory", "0");
        println!("LaunchMaxMemory = auto");
        return Ok(());
    }
    let mb: i32 = mem.parse().map_err(|_| "请填数字 MB 或 auto")?;
    if mb < 0 {
        return Err("内存必须为非负整数".into());
    }
    s.set_string("LaunchMaxMemory", &mb.to_string());
    println!("LaunchMaxMemory = {mb}MB");
    Ok(())
}

fn set_cf_key(s: &mut Settings, key: Option<&str>, clear: bool) -> Result<(), String> {
    if clear {
        s.remove_key("CfApiKey");
        println!("已清除自定义 CurseForge key");
        return Ok(());
    }
    match key {
        None => {
            let user = s.get_encrypted("CfApiKey");
            if !user.is_empty() {
                println!("CurseForge key: 指令设置的自定义 key（加密保存中）");
            } else if !mlccore::download::EMBEDDED_CF_KEY.is_empty() {
                println!("CurseForge key: 编译期内嵌（发布版完整体验）");
            } else {
                println!("CurseForge key: 未设置，走 MCIM 镜像");
            }
            Ok(())
        }
        Some(k) => {
            if k.is_empty() {
                return Err("key 不能为空；清除请用 --clear".into());
            }
            s.set_encrypted("CfApiKey", k);
            println!("已保存自定义 CurseForge key（加密存储，立即生效）");
            Ok(())
        }
    }
}

// ---------------------------------------------------------------- 玩家

fn player_list(s: &mut Settings) -> Result<(), String> {
    let selected = s.selected_player().unwrap_or_default();
    let players = mlccore::auth::list_players(s);
    if players.is_empty() {
        println!("（无玩家） / (no players)");
        return Ok(());
    }
    for (i, p) in players.iter().enumerate() {
        let mark = if p.uuid == selected { "*" } else { " " };
        println!(
            "{mark} {}. {}  {}  skin={}",
            i + 1,
            p.uuid,
            p.name,
            p.skin_type
        );
    }
    Ok(())
}

fn player_add(s: &mut Settings, name: &str, avatar: &str, skin: &str) -> Result<(), String> {
    if name.trim().is_empty() {
        return Err("玩家名不能为空".into());
    }
    let p = mlccore::auth::add_player(s, name.trim(), avatar, skin, "");
    println!("已添加玩家: {}  {}", p.uuid, p.name);
    Ok(())
}

fn player_rm(s: &mut Settings, uuid: &str) -> Result<(), String> {
    if mlccore::auth::remove_player(s, uuid) {
        println!("已删除玩家 {uuid}");
        Ok(())
    } else {
        Err(format!("玩家不存在: {uuid}"))
    }
}

fn player_select(s: &mut Settings, uuid: &str) -> Result<(), String> {
    if mlccore::auth::select_player(s, uuid) {
        println!("当前玩家: {uuid}");
        Ok(())
    } else {
        Err(format!("玩家不存在: {uuid}"))
    }
}

fn player_edit(
    s: &mut Settings,
    uuid: &str,
    name: &str,
    avatar: &str,
    skin: &str,
    new_uuid: &str,
) -> Result<(), String> {
    let cur_name = s.get_profile(uuid, "Name").unwrap_or_default();
    let cur_avatar = s.get_profile(uuid, "Avatar").unwrap_or_default();
    let cur_skin = s
        .get_profile(uuid, "SkinType")
        .unwrap_or_else(|| "slim".into());
    let n = if name.is_empty() { &cur_name } else { name };
    let a = if avatar.is_empty() {
        &cur_avatar
    } else {
        avatar
    };
    let sk = if skin.is_empty() { &cur_skin } else { skin };
    let target = if new_uuid.is_empty() {
        uuid.to_string()
    } else {
        new_uuid.to_string()
    };
    if mlccore::auth::update_player(s, uuid, n, a, sk, &target) {
        println!("已更新玩家 {uuid} → {n}");
        Ok(())
    } else {
        Err(format!("更新失败（玩家不存在或目标 UUID 已占用）: {uuid}"))
    }
}

// ---------------------------------------------------------------- 登录

async fn login(server: String, username: String, password: Option<String>) -> Result<(), String> {
    let password = match password {
        Some(p) => p,
        None => {
            eprint!("Password: ");
            use std::io::Write;
            std::io::stderr().flush().ok();
            let mut line = String::new();
            std::io::stdin()
                .read_line(&mut line)
                .map_err(|e| e.to_string())?;
            line.trim_end_matches(['\n', '\r']).to_string()
        }
    };
    // async 期间不能持有 Settings 锁：先完成网络，再短锁写盘
    let login = mlccore::auth::login_authlib(&server, &username, &password).await?;
    settings::with_global(|s| {
        mlccore::auth::persist_authlib_login(s, &login);
    });
    println!("登录成功: {}  {}", login.name, login.uuid);
    Ok(())
}

fn logout(s: &mut Settings) -> Result<(), String> {
    mlccore::auth::logout_authlib(s);
    println!("已退出外置登录");
    Ok(())
}

// ---------------------------------------------------------------- 安装

async fn install_mc(version_id: &str) -> Result<(), String> {
    let mc = settings::with_global(|s| mc_folder(s))
        .ok_or_else(|| "Settings not initialized".to_string())?;
    let dl =
        mlccore::download::AssetDownloader::new(mlccore::download::manager::DownloadManager::new());
    let id = mlccore::version::install_version(&mc, version_id, &dl, None).await?;
    println!("已安装 Minecraft {id}");
    Ok(())
}

async fn install_java(major: i32) -> Result<(), String> {
    let mc = settings::with_global(|s| mc_folder(s))
        .ok_or_else(|| "Settings not initialized".to_string())?;
    let mgr = mlccore::download::manager::DownloadManager::new();
    let n = mlccore::java::install_java_runtime(&mc, major, &mgr).await?;
    println!("JRE {major} 已安装；当前共检测到 {n} 个 Java");
    Ok(())
}

async fn do_update(beta: bool) -> Result<(), String> {
    mlccore::update::check_and_update(beta).await?;
    Ok(())
}

async fn server_install(
    version: &str,
    forge: Option<&str>,
    fabric: Option<&str>,
    neoforge: Option<&str>,
    from: Option<&str>,
) -> Result<(), String> {
    let mc = settings::with_global(|s| mc_folder(s))
        .ok_or_else(|| "Settings not initialized".to_string())?;
    if version.is_empty() && from.is_some() {
        return Err(
            "--from 需要显式版本号（如 mlc server-install 1.20.1 --forge --from xxx）".into(),
        );
    }
    if version.is_empty() {
        println!("正在下载最新版 MC 服务端 ...");
    } else {
        println!("正在下载 MC {version} 服务端 ...");
    }

    let (loader_type, loader_ver) = if let Some(v) = forge {
        ("forge", v)
    } else if let Some(v) = neoforge {
        ("neoforge", v)
    } else if let Some(v) = fabric {
        ("fabric", v)
    } else {
        ("", "")
    };

    let dlm = mlccore::download::manager::DownloadManager::new();
    let sid = mlccore::server::install_server(&dlm, &mc, version, loader_type, loader_ver).await?;

    if let Some(inst) = from {
        // 有 loader 时目录名可能带具体 loader 版本；sid 即实际目录名
        if let Some(s) =
            settings::with_global(|s| mlccore::server::copy_instance_to_server(&mc, inst, &sid, s))
        {
            s?;
            println!("注意：客户端专属 mod（如 Sodium 等渲染类）会让服务端崩溃，启动失败请先删 mods/ 里的渲染/界面类 mod");
        }
    }
    println!("success");
    Ok(())
}

fn server_start(s: &mut Settings, id: &str, eula: bool) -> Result<(), String> {
    let mc = mc_folder(s);
    let dir = mlccore::server::server_dir(&mc, id);
    if !dir.is_dir() {
        return Err(format!("服务端未安装，请先 mlc server-install {id}"));
    }
    if !mlccore::server::eula_accepted(&dir) {
        println!("Minecraft 最终用户许可协议: https://aka.ms/MinecraftEULA");
        if eula {
            mlccore::server::accept_eula(&dir)?;
        } else {
            return Err("请先阅读 EULA，并以 --eula 参数表示同意".into());
        }
    }
    println!("正在启动服务端 {id}（控制台直通，/stop 关服）...");
    let code = mlccore::server::start_server(&mc, id, s)?;
    if code == 0 {
        Ok(())
    } else {
        Err(format!("服务端退出码 {code}"))
    }
}

// ---------------------------------------------------------------- report

/// RFC 3986 百分号编码（GitHub issue query 用）
fn percent_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char)
            }
            b' ' => out.push_str("%20"),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

fn sanitize_log_line(l: &str, home_s: &str) -> String {
    let mut l = l.to_string();
    if let Some(pos) = l.find("--accessToken") {
        let head = &l[..pos];
        let after_flag = &l[pos + "--accessToken".len()..];
        let ws_end = after_flag
            .char_indices()
            .find(|(_, c)| !c.is_whitespace())
            .map(|(i, _)| i)
            .unwrap_or(after_flag.len());
        let (ws, rest2) = after_flag.split_at(ws_end);
        let rest2 = rest2
            .split_once(|c: char| c.is_whitespace())
            .map(|(_, r)| r)
            .unwrap_or("");
        l = format!("{head}--accessToken{ws}***{rest2}");
    }
    if !home_s.is_empty() {
        l = l.replace(home_s, "~");
    }
    l
}

fn latest_launch_log_tail(mc: &Path, max_lines: usize, max_chars: usize) -> String {
    let logs = mc.join("logs");
    let Ok(entries) = std::fs::read_dir(&logs) else {
        return String::new();
    };
    let mut files: Vec<PathBuf> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .map(|n| {
                    let n = n.to_string_lossy();
                    n.starts_with("mlc-launch-") && n.ends_with(".log")
                })
                .unwrap_or(false)
        })
        .collect();
    files.sort();
    let Some(path) = files.last() else {
        return String::new();
    };
    let Ok(text) = std::fs::read_to_string(path) else {
        return String::new();
    };
    let home = mlccore::util::platform::home_dir();
    let home_s = mlccore::util::platform::normalize_path_string(&home);
    let mut lines: Vec<String> = text
        .lines()
        .map(|l| sanitize_log_line(l, &home_s))
        .collect();
    if lines.len() > max_lines {
        lines = lines[lines.len() - max_lines..].to_vec();
    }
    let mut tail = lines.join("\n");
    if tail.len() > max_chars {
        let cut = tail.len() - max_chars;
        let mut start = cut;
        while start < tail.len() && !tail.is_char_boundary(start) {
            start += 1;
        }
        tail = format!("（截断）\n{}", &tail[start..]);
    }
    tail
}

fn build_issue_url(repo: &str, desc: &str, tail: &str) -> String {
    let mut body = format!(
        "{desc}\n\n**环境 / Environment**\n- MLC: {} ({})\n- OS: {}\n",
        mlccore::GIT_DESCRIBE,
        mlccore::GIT_COMMIT_HASH,
        std::env::consts::OS
    );
    if !tail.is_empty() {
        body.push_str(&format!(
            "\n**最近启动日志 / Launch log (tail)**\n```\n{tail}\n```\n"
        ));
    }
    let title: String = desc.chars().take(60).collect();
    let title = if desc.chars().count() > 60 {
        format!("{title}...")
    } else {
        title
    };
    format!(
        "https://github.com/{repo}/issues/new?title={}&body={}",
        percent_encode(&title),
        percent_encode(&body)
    )
}

fn report(s: &mut Settings, description: &[String]) -> Result<(), String> {
    let desc = if description.is_empty() {
        "MLC 问题反馈".to_string()
    } else {
        description.join(" ")
    };
    let repo = std::env::var("MLC_REPO").unwrap_or_else(|_| "recallrw80-afk/MLC".into());
    let mc = mc_folder(s);
    let mut url = build_issue_url(&repo, &desc, &latest_launch_log_tail(&mc, 40, 4000));
    if url.len() > 7500 {
        url = build_issue_url(&repo, &desc, &latest_launch_log_tail(&mc, 15, 1200));
    }
    println!("Issue 预填链接（内容已生成，提交前可再编辑）:\n{url}");
    Ok(())
}

// ---------------------------------------------------------------- inpack

fn inpack(
    s: &mut Settings,
    file: &Path,
    rename: Option<&str>,
    target: Option<&str>,
) -> Result<(), String> {
    if !file.exists() {
        return Err(format!("File not found: {}", file.display()));
    }
    let mc = mc_folder(s);
    let pack_type = mlccore::modpack::detect_pack_type(file);
    println!("检测到: {}", pack_type.name());

    // 解压到本进程 tmp
    let extract = mlccore::modpack::pack_tmp_root(&mc).join("pack");
    mlccore::util::file::remove_tree(&extract);
    std::fs::create_dir_all(&extract).map_err(|e| e.to_string())?;
    let (_ok, failed) = mlccore::util::file::extract_zip(file, &extract)?;
    if failed > 0 {
        mlccore::modpack::cleanup_on_error(s, &mc, &extract);
        return Err("Extraction failed".into());
    }
    // 一级目录包装
    let mut effective = extract.clone();
    if let Ok(rd) = std::fs::read_dir(&extract) {
        let dirs: Vec<PathBuf> = rd
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.is_dir())
            .collect();
        if dirs.len() == 1 {
            let sub = &dirs[0];
            let markers = [
                "manifest.json",
                "modrinth.index.json",
                "mmc-pack.json",
                "mcbbs.packmeta",
                "modpack.json",
                "modpack.zip",
                "modpack.mrpack",
            ];
            if markers.iter().any(|m| sub.join(m).exists()) {
                effective = sub.clone();
            }
        }
    }

    let name = rename.unwrap_or("");
    match pack_type {
        mlccore::modpack::PackType::Mod => {
            let target = target.ok_or("此 zip 为 mod 包，需要加 --to <实例名>")?;
            let n = mlccore::modpack::install_mod(s, &mc, &effective, target)?;
            println!("{n} mod(s) added to {target}");
            Ok(())
        }
        mlccore::modpack::PackType::Compressed | mlccore::modpack::PackType::Unknown => {
            let n = mlccore::modpack::install_compressed(s, &mc, file, name)?;
            println!("Complete: {n}");
            Ok(())
        }
        mlccore::modpack::PackType::CurseForge => {
            let prep = mlccore::modpack::prepare_curseforge(s, &mc, &effective, name)?;
            println!("CurseForge modpack: {}", prep.name);
            let dl = mlccore::download::AssetDownloader::new(
                mlccore::download::manager::DownloadManager::new(),
            );
            let mp = mlccore::download::ModPlatform::shared();
            let r = block_on(mlccore::modpack::download_and_finalize(
                s,
                &mc,
                &dl,
                mp,
                mlccore::modpack::FinalizeRequest {
                    final_dir: &prep.final_dir,
                    name: &prep.name,
                    mc_version: &prep.mc_version,
                    forge_ver: &prep.forge,
                    neo_ver: &prep.neo,
                    fabric_ver: &prep.fabric,
                    mods: &prep.mods,
                },
            ));
            match r {
                Ok(()) => {
                    println!("Complete: {}", prep.name);
                    Ok(())
                }
                Err(e) => {
                    mlccore::modpack::cleanup_on_error(s, &mc, &prep.final_dir);
                    Err(e)
                }
            }
        }
        other => Err(format!(
            "{} 安装器尚未移植 / installer not yet ported",
            other.name()
        )),
    }
}

// ---------------------------------------------------------------- launch

async fn launch(name: &str) -> Result<(), String> {
    let (mc, player, skin) = settings::with_global(|s| {
        let mc = mc_folder(s);
        let players = mlccore::auth::list_players(s);
        let selected = s.selected_player().unwrap_or_default();
        let p = players
            .iter()
            .find(|x| x.uuid == selected)
            .cloned()
            .or_else(|| players.first().cloned());
        let (n, sk) = match p {
            Some(p) => (p.name, p.skin_type),
            None => ("Player".to_string(), "default".to_string()),
        };
        (mc, n, sk)
    })
    .ok_or_else(|| "Settings not initialized".to_string())?;

    // 解析版本：实例显示名 → 实例；否则全局版本
    let version = settings::with_global(|s| -> Result<_, String> {
        if let Some(dir) = s.dir_for_display_name(name) {
            Ok(mlccore::version::load_instance_version(&mc, &dir))
        } else {
            let v = mlccore::version::load_version(&mc, name);
            if !v.is_valid {
                // 无参数且有实例：列出候选
                let list = mlccore::version::list_instances(s, &mc);
                if name.is_empty() && !list.is_empty() {
                    let names: Vec<_> = list.iter().map(|i| i.id.clone()).collect();
                    return Err(format!("请指定实例或版本，例如: mlc launch {}", names[0]));
                }
                return Err(format!("无法加载版本: {name}"));
            }
            Ok(v)
        }
    })
    .ok_or_else(|| "Settings not initialized".to_string())??;

    if !version.is_valid {
        return Err(format!("版本无效: {}", version.info));
    }

    // 登录
    let login = settings::with_global(|s| {
        // 先拿缓存，网络在 await 里做
        mlccore::auth::current_authlib_login(s)
    })
    .unwrap_or_default();
    let auth_login = if login.logged_in {
        let access =
            settings::with_global(|s| s.get_encrypted("Authlib/AccessToken")).unwrap_or_default();
        let client =
            settings::with_global(|s| s.get_encrypted("Authlib/ClientToken")).unwrap_or_default();
        match mlccore::auth::refresh_authlib(&login.server, &access, &client).await {
            Ok(mut r) => {
                if r.name.is_empty() {
                    r.name = login.name.clone();
                }
                if r.uuid.is_empty() {
                    r.uuid = login.uuid.clone();
                }
                settings::with_global(|s| mlccore::auth::persist_authlib_login(s, &r));
                r
            }
            Err(e) => {
                eprintln!("authlib 刷新失败，回退离线: {e}");
                mlccore::auth::create_offline_login(&player, &skin)
            }
        }
    } else {
        mlccore::auth::create_offline_login(&player, &skin)
    };

    // Java
    let javas = mlccore::java::scan_system_java(&mc);
    let java = mlccore::java::select_java_for_version(&javas, &version)
        .or_else(|| javas.first().cloned())
        .ok_or("No Java runtime found. Please install Java.")?;

    // version json 继承链
    let json_path = PathBuf::from(&version.path_json);
    let vj = mlccore::version::resolve_inheritance_chain(&mc, &json_path)
        .ok_or("Failed to resolve version inheritance")?;

    let settings_snap =
        settings::with_global(|_s| Settings::load(&mlccore::settings::default_config_path()))
            .unwrap();

    let opts = settings::with_global(|s| mlccore::launch::LaunchOptions {
        max_memory_mb: s
            .get_string("LaunchMaxMemory")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0),
        min_memory_mb: s
            .get_string("LaunchMinMemory")
            .and_then(|v| v.parse().ok())
            .unwrap_or(512),
        fullscreen: s
            .get_string("LaunchFullscreen")
            .map(|v| v == "true" || v == "1")
            .unwrap_or(false),
        window_width: s
            .get_string("LaunchWidth")
            .and_then(|v| v.parse().ok())
            .unwrap_or(854),
        window_height: s
            .get_string("LaunchHeight")
            .and_then(|v| v.parse().ok())
            .unwrap_or(480),
        ..Default::default()
    })
    .unwrap_or_default();

    let login_res = auth_login.to_launch_login();
    let cmd = mlccore::launch::build_launch_command(
        &vj,
        &version,
        &java,
        &login_res,
        &opts,
        &settings_snap,
        &mc,
    )?;

    // 启动日志
    let log_dir = mc.join("logs");
    let _ = std::fs::create_dir_all(&log_dir);
    // 滚动清理
    if let Ok(entries) = std::fs::read_dir(&log_dir) {
        let mut logs: Vec<PathBuf> = entries
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.file_name()
                    .map(|n| {
                        let n = n.to_string_lossy();
                        n.starts_with("mlc-launch-") && n.ends_with(".log")
                    })
                    .unwrap_or(false)
            })
            .collect();
        logs.sort();
        while logs.len() >= 9 {
            let old = logs.remove(0);
            let _ = std::fs::remove_file(old);
        }
    }
    let ts = chrono_like_timestamp();
    let log_path = log_dir.join(format!("mlc-launch-{ts}.log"));
    let log_file = std::fs::File::create(&log_path).ok();
    if let Some(mut f) = log_file {
        use std::io::Write as _;
        let _ = writeln!(f, "> {}", cmd.java_path);
    }

    println!("启动: {} ({})", version.id, version.path_indie);
    let mut child = Command::new(&cmd.java_path)
        .args(&cmd.argv[1..])
        .current_dir(&cmd.working_dir)
        .envs(cmd.env.iter().map(|(k, v)| (k.as_str(), v.as_str())))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("Failed to start game process: {e}"))?;

    let stdout = child.stdout.take();
    let stderr = child.stderr.take();
    let log_path2 = log_path.clone();
    let h1 = std::thread::spawn(move || pipe_to(stdout, log_path2.clone(), false));
    let h2 = std::thread::spawn(move || pipe_to(stderr, log_path, true));
    let status = child.wait().map_err(|e| e.to_string())?;
    let _ = h1.join();
    let _ = h2.join();
    let code = status.code().unwrap_or(-1);
    if code == 0 {
        println!("[MLC] Game exited with code 0");
        Ok(())
    } else {
        Err(format!("[MLC] Game exited abnormally with code {code}"))
    }
}

fn chrono_like_timestamp() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // yyyyMMdd-HHmmss 近似（UTC）
    let days = secs / 86400;
    let rem = secs % 86400;
    let (h, m, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    // 1970-01-01 + days（简化：用 unix 日数转近似日期，足够日志文件名）
    let y = 1970 + days / 365;
    let doy = days % 365;
    format!("{y}{:03}-{h:02}{m:02}{s:02}", doy)
}

fn pipe_to(pipe: Option<impl std::io::Read>, log_path: PathBuf, is_err: bool) {
    use std::io::{BufRead, BufReader, Write as _};
    let Some(pipe) = pipe else { return };
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .ok();
    let reader = BufReader::new(pipe);
    for line in reader.lines().map_while(Result::ok) {
        if is_err {
            eprintln!("{line}");
        } else {
            println!("{line}");
        }
        if let Some(f) = file.as_mut() {
            let _ = writeln!(f, "{line}");
        }
    }
}

// ---------------------------------------------------------------- 自检

struct CheckResult {
    name: &'static str,
    ok: bool,
    detail: String,
}

/// 全系统自检（离线）：核心库纯函数 + 临时目录集成；任一 FAIL 退出码 1
fn run_self_test() -> Result<(), String> {
    println!("=== MLC 全系统自检 ===");
    let mut results: Vec<CheckResult> = Vec::new();
    let mut push = |name: &'static str, r: Result<(), String>| {
        results.push(match r {
            Ok(()) => CheckResult {
                name,
                ok: true,
                detail: "ok".into(),
            },
            Err(detail) => CheckResult {
                name,
                ok: false,
                detail,
            },
        });
    };

    // 1. 加密往返（兼容密钥）
    push("crypto", {
        let c = mlccore::util::crypto::pcl_encrypt("secret-key-123");
        if mlccore::util::crypto::pcl_decrypt(&c) == "secret-key-123" {
            Ok(())
        } else {
            Err("encrypt/decrypt 不一致".into())
        }
    });

    // 2. 离线 UUID
    push("offline-uuid", {
        let u = mlccore::auth::generate_offline_uuid("Notch");
        if u.len() == 36 && &u[14..15] == "3" {
            Ok(())
        } else {
            Err(format!("格式异常: {u}"))
        }
    });

    // 3. Java 版本解析
    push("java-version-parse", {
        let v = mlccore::java::parse_java_version_output(r#"openjdk version "1.8.0_321""#);
        match v {
            Some(n) if n.major() == 1 && n.minor() == 8 => Ok(()),
            other => Err(format!("{other:?}")),
        }
    });

    // 4. 版本号比较
    push("version-number", {
        use mlccore::version::McVersionNumber as V;
        if V::parse("1.20.1") < V::parse("1.21") && V::empty() < V::parse("1.0") {
            Ok(())
        } else {
            Err("比较语义错误".into())
        }
    });

    // 5. 参数切分与去重
    push("args", {
        use mlccore::util::args::{deduplicate_args, split_java_args};
        let parts = split_java_args(r#"-Dfoo="a b" -Xmx1G"#);
        let deduped = deduplicate_args(&[
            "-Xmx1G".into(),
            "-Xmx2G".into(),
            "-cp".into(),
            "old".into(),
            "-cp".into(),
            "new".into(),
        ]);
        if parts == vec!["-Dfoo=a b".to_string(), "-Xmx1G".to_string()]
            && deduped == vec!["-Xmx2G".to_string(), "-cp".to_string(), "new".to_string()]
        {
            Ok(())
        } else {
            Err(format!("parts={parts:?} dedup={deduped:?}"))
        }
    });

    // 6. maven 路径
    push("maven-path", {
        let p = mlccore::util::file::maven_name_to_path("com.example:lib:1.0.0");
        if p == "com/example/lib/1.0.0/lib-1.0.0.jar" {
            Ok(())
        } else {
            Err(p)
        }
    });

    // 7. Settings 往返 + 玩家
    push("settings-players", {
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "mlc-selftest-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("MLC.ini");
        let mut s = Settings::load(&path);
        s.set_string("LaunchFolderSelect", &format!("{}/", dir.display()));
        let p = mlccore::auth::add_player(&mut s, "自检", "", "slim", "");
        let s2 = Settings::load(&path);
        if s2.get_profile(&p.uuid, "Name").as_deref() == Some("自检")
            && s2.get_encrypted("Authlib/AccessToken").is_empty()
        {
            Ok(())
        } else {
            Err("玩家档案未持久化".into())
        }
    });

    // 8. 实例 begin/rollback
    push("instance-rollback", {
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "mlc-selftest-{}-rb-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::create_dir_all(&dir);
        let mut s = Settings::load(&dir.join("MLC.ini"));
        s.set_string("LaunchFolderSelect", &format!("{}/", dir.display()));
        let (final_dir, name) = mlccore::modpack::begin_install(&mut s, &dir, "", "自检包")?;
        if !mlccore::modpack::is_incomplete(&final_dir) {
            Err("缺少 .incomplete 标记".into())
        } else {
            mlccore::modpack::cleanup_on_error(&mut s, &dir, &final_dir);
            if final_dir.exists() || s.dir_for_display_name(&name).is_some() {
                Err("回滚不完整".into())
            } else {
                Ok(())
            }
        }
    });

    // 9. 整合包类型检测
    push("modpack-detect", {
        use std::io::Write;
        let dir = std::env::temp_dir().join(format!("mlc-selftest-{}-dz", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let zip_path = dir.join("pack.zip");
        let f = std::fs::File::create(&zip_path).map_err(|e| e.to_string())?;
        let mut z = zip::ZipWriter::new(f);
        let opts = zip::write::SimpleFileOptions::default();
        z.start_file("manifest.json", opts)
            .map_err(|e| e.to_string())?;
        z.write_all(br#"{"minecraft":{"version":"1.20.1"}}"#)
            .map_err(|e| e.to_string())?;
        z.finish().map_err(|e| e.to_string())?;
        let t = mlccore::modpack::detect_pack_type(&zip_path);
        if t == mlccore::modpack::PackType::CurseForge {
            Ok(())
        } else {
            Err(format!("检测为 {}", t.name()))
        }
    });

    // 10. 启动参数构建
    push("launch-build", {
        use mlccore::java::JavaEntry;
        use mlccore::launch::{build_replacements, LaunchOptions, LoginResult};
        use mlccore::version::{McVersion, McVersionNumber};
        use serde_json::json;
        let v = McVersion {
            id: "1.20.1".into(),
            kind: "release".into(),
            is_valid: true,
            vanilla_version: McVersionNumber::parse("1.20.1"),
            path_indie: "/mc/".into(),
            path_jar: "/mc/versions/1.20.1/1.20.1.jar".into(),
            ..Default::default()
        };
        let j = JavaEntry {
            path_java: "/j/bin/java".into(),
            path_folder: "/j/bin/".into(),
            version: McVersionNumber::parse("21.0.2.0"),
            major_version: 21,
            is_64bit: true,
            ..Default::default()
        };
        let login = LoginResult {
            name: "Steve".into(),
            uuid: "u1".into(),
            access_token: "t".into(),
            login_type: "Legacy".into(),
            ..Default::default()
        };
        let vj = json!({"assets": "17", "libraries": []});
        let r = build_replacements(
            &vj,
            &v,
            &j,
            &login,
            &LaunchOptions::default(),
            Path::new("/mc"),
        );
        if r.get("auth_player_name").map(|s| s.as_str()) == Some("Steve")
            && r.get("assets_index_name").map(|s| s.as_str()) == Some("17")
        {
            Ok(())
        } else {
            Err("替换表异常".into())
        }
    });

    // 11. QSettings INI 往返
    push("ini-roundtrip", {
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "mlc-selftest-{}-ini-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::create_dir_all(&dir);
        let mut s = Settings::load(&dir.join("MLC.ini"));
        s.set_instance_dir("abc123", "我的整合包");
        s.set_encrypted("CfApiKey", "k-123");
        let s2 = Settings::load(&dir.join("MLC.ini"));
        if s2.dir_for_display_name("我的整合包").as_deref() == Some("abc123")
            && s2.get_encrypted("CfApiKey") == "k-123"
        {
            Ok(())
        } else {
            Err("INI 往返失败".into())
        }
    });

    // 打印
    let mut failed = 0;
    for r in &results {
        let tag = if r.ok {
            "\x1b[32m[ OK ]\x1b[0m"
        } else {
            failed += 1;
            "\x1b[31m[FAIL]\x1b[0m"
        };
        println!("  {tag} {:<22} {}", r.name, r.detail);
    }
    println!("\n{} 项通过, {failed} 项失败", results.len() - failed);
    if failed > 0 {
        Err(format!("{failed} 项自检失败"))
    } else {
        Ok(())
    }
}
