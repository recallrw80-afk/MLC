//! 启动管线，对应 C++ `launchbuilder.cpp` + `launcher.cpp`：
//! JVM 参数构建、内存自动 sizing（sysinfo：可用内存 50%、上限 16G）、GC 档位、
//! fcitx/ibus XIM 崩溃规避（GLFW 3.4 替换）、启动日志落盘（mc/logs/mlc-launch-*，留 10 份）。
//!
//! 本切片覆盖：参数构建、替换表、规则判定、环境变量、命令组装。
//! 进程异步生命周期（QProcess 信号）由 CLI/GUI 层在后续切片接入。

use std::collections::BTreeMap;
use std::path::Path;

use serde_json::Value;

use crate::java::JavaEntry;
use crate::settings::Settings;
use crate::util::args::{deduplicate_args, split_java_args};
use crate::util::file::maven_name_to_path;
use crate::util::platform;
use crate::version::{McVersion, McVersionNumber};

// ---------------------------------------------------------------- 类型

/// 对齐 McLaunchOptions
#[derive(Debug, Clone)]
pub struct LaunchOptions {
    pub server_ip: String,
    pub extra_game_args: Vec<String>,
    /// 0 = 按系统可用内存自动
    pub max_memory_mb: i32,
    pub min_memory_mb: i32,
    pub fullscreen: bool,
    pub window_width: i32,
    pub window_height: i32,
}

impl Default for LaunchOptions {
    fn default() -> Self {
        Self {
            server_ip: String::new(),
            extra_game_args: Vec::new(),
            max_memory_mb: 0,
            min_memory_mb: 512,
            fullscreen: false,
            window_width: 854,
            window_height: 480,
        }
    }
}

/// 对齐 LoginResult（启动参数替换所需字段）
#[derive(Debug, Clone, Default)]
pub struct LoginResult {
    pub name: String,
    pub uuid: String,
    pub access_token: String,
    /// "Legacy" / "Auth" / "Ms" …
    pub login_type: String,
    pub client_token: String,
    pub profile_json: String,
    /// authlib-injector 服务器地址
    pub server_url: String,
}

/// 构建结果
#[derive(Debug, Clone, Default)]
pub struct LaunchCommand {
    pub main_class: String,
    pub jvm_args: Vec<String>,
    pub game_args: Vec<String>,
    /// 完全替换后的 java + jvm + main + game
    pub argv: Vec<String>,
    pub java_path: String,
    pub working_dir: String,
    pub env: BTreeMap<String, String>,
}

// ---------------------------------------------------------------- 内存

/// 自动最大内存（对齐 autoMaxMemoryMB）：可用 50%、512MB 对齐、夹在 [2048, 16384]
pub fn auto_max_memory_mb() -> i32 {
    let mem = available_memory_mb().unwrap_or(8192);
    let mut mb = mem / 2;
    mb = (mb / 512) * 512;
    mb.clamp(2048, 16384) as i32
}

/// 可用物理内存 MB（sysinfo；0.30 的 available_memory 为 KiB）
fn available_memory_mb() -> Option<i64> {
    use sysinfo::System;
    let mut sys = System::new();
    sys.refresh_memory();
    Some(sys.available_memory() as i64 / 1024)
}

// ---------------------------------------------------------------- 规则

/// 对齐 checkRules / rulesAllow
pub fn check_rules(rules: &Value) -> bool {
    let Some(arr) = rules.as_array() else {
        return true; // 无 rules = 允许（C++ 对非数组也 true？非数组时 is_array false → true）
    };
    if arr.is_empty() {
        return true;
    }
    let mut allowed = false;
    for rule in arr {
        let Some(rule) = rule.as_object() else {
            continue;
        };
        let rule_allows = rule
            .get("action")
            .and_then(|v| v.as_str())
            .unwrap_or("allow")
            == "allow";
        let mut matches = true;
        if let Some(os) = rule.get("os") {
            matches = false;
            if let Some(os) = os.as_object() {
                let os_name = os.get("name").and_then(|v| v.as_str()).unwrap_or("");
                let os_arch = os.get("arch").and_then(|v| v.as_str()).unwrap_or("");
                let mut os_match = true;
                if !os_name.is_empty() {
                    os_match = os_name == platform::platform_name();
                }
                if !os_arch.is_empty() {
                    let arch = if platform::is_64bit() {
                        "x86_64"
                    } else {
                        "x86"
                    };
                    os_match = os_match && os_arch == arch;
                }
                matches = os_match;
            }
        }
        if let Some(features) = rule.get("features") {
            matches = matches && check_features(features);
        }
        if matches {
            allowed = rule_allows;
        }
    }
    allowed
}

/// 对齐 checkFeatures：demo 拒绝；quick_play 键一律拒绝
pub fn check_features(features: &Value) -> bool {
    let Some(obj) = features.as_object() else {
        return true;
    };
    if let Some(demo) = obj.get("is_demo_user") {
        if demo.as_bool() == Some(true) {
            return false;
        }
    }
    for key in obj.keys() {
        if key.to_lowercase().contains("quick_play") {
            return false;
        }
    }
    true
}

// ---------------------------------------------------------------- 参数构建

fn json_str<'a>(j: &'a Value, key: &str) -> Option<&'a str> {
    j.get(key).and_then(|v| v.as_str())
}

/// 从 arguments 数组收集通过规则的字符串参数
fn collect_arg_list(section: &Value) -> Vec<String> {
    let mut out = Vec::new();
    let Some(arr) = section.as_array() else {
        return out;
    };
    for arg in arr {
        if let Some(s) = arg.as_str() {
            out.push(s.to_string());
        } else if let Some(obj) = arg.as_object() {
            if let Some(rules) = obj.get("rules") {
                if !check_rules(rules) {
                    continue;
                }
            }
            match obj.get("value") {
                Some(Value::String(s)) => out.push(s.clone()),
                Some(Value::Array(arr)) => {
                    for v in arr {
                        if let Some(s) = v.as_str() {
                            out.push(s.to_string());
                        }
                    }
                }
                _ => {}
            }
        }
    }
    out
}

/// GC 参数前缀（对齐 launchbuilder 的 removeIf 正则）
fn is_gc_arg(a: &str) -> bool {
    let Some(rest) = a.strip_prefix("-XX:") else {
        return false;
    };
    let rest = rest
        .strip_prefix('+')
        .or_else(|| rest.strip_prefix('-'))
        .unwrap_or(rest);
    const NAMES: &[&str] = &[
        "UseParallelGC",
        "UseG1GC",
        "UseZGC",
        "UseSerialGC",
        "UseConcMarkSweepGC",
        "UseParNewGC",
        "UseShenandoahGC",
        "ZGenerational",
        "UseCompactObjectHeaders",
        "MaxGCPauseMillis",
        "MinHeapFreeRatio",
    ];
    if NAMES.contains(&rest) {
        return true;
    }
    // G1*Percent / G1*Size
    rest.starts_with("G1")
        && (rest.ends_with("Percent")
            || rest.ends_with("Size")
            || rest.contains("Percent")
            || rest.contains("Size"))
}

/// 构建 JVM 参数（对齐 buildJvmArgs）
pub fn build_jvm_args(
    version_json: &Value,
    version: &McVersion,
    java: &JavaEntry,
    settings: &Settings,
    options: &LaunchOptions,
    mc_folder: &Path,
) -> Vec<String> {
    let mut args = Vec::new();

    if let Some(jvm) = version_json.get("arguments").and_then(|a| a.get("jvm")) {
        args.extend(collect_arg_list(jvm));
    } else {
        args.push("-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump".into());
        args.push("-Djava.library.path=${natives_directory}".into());
        args.push("-cp".into());
        args.push("${classpath}".into());
    }

    // 自定义 JVM 参数
    let mut custom = settings
        .get_instance(&version.id, "VersionAdvanceJvm")
        .unwrap_or_default();
    if custom.is_empty() {
        custom = settings.get_string("LaunchAdvanceJvm").unwrap_or_default();
    }
    if !custom.is_empty() {
        args.extend(split_java_args(&custom));
    }

    // 内存
    let mut max_mem = options.max_memory_mb;
    if max_mem <= 0 {
        max_mem = settings
            .get_string("LaunchMaxMemory")
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
    }
    if max_mem <= 0 {
        max_mem = auto_max_memory_mb();
        tracing::info!("Auto memory allocation: {max_mem} MB");
    }
    args.push(format!("-Xmx{max_mem}m"));
    if options.min_memory_mb > 0 {
        args.push(format!("-Xms{}m", options.min_memory_mb));
    }

    // GC
    let mut gc_type: i32 = settings
        .get_instance(&version.id, "VersionAdvanceGC")
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    if gc_type <= 0 {
        gc_type = settings.get_int("LaunchAdvanceGC", 0);
    }
    if gc_type != 3 {
        let use_g1 = gc_type == 1 || gc_type == 4 || (gc_type == 0 && java.major_version < 21);
        let use_zgc = !use_g1 && (gc_type == 2 || gc_type == 0);
        args.retain(|a| !is_gc_arg(a));
        if use_g1 {
            args.push("-XX:+UseG1GC".into());
            if gc_type == 4 {
                for a in [
                    "-XX:+ParallelRefProcEnabled",
                    "-XX:MaxGCPauseMillis=200",
                    "-XX:+UnlockExperimentalVMOptions",
                    "-XX:+DisableExplicitGC",
                    "-XX:+AlwaysPreTouch",
                    "-XX:G1NewSizePercent=30",
                    "-XX:G1MaxNewSizePercent=40",
                    "-XX:G1HeapRegionSize=8M",
                    "-XX:G1ReservePercent=20",
                    "-XX:G1HeapWastePercent=5",
                    "-XX:G1MixedGCCountTarget=4",
                    "-XX:InitiatingHeapOccupancyPercent=15",
                    "-XX:G1MixedGCLiveThresholdPercent=90",
                    "-XX:G1RSetUpdatingPauseTimePercent=5",
                    "-XX:SurvivorRatio=32",
                    "-XX:MaxTenuringThreshold=1",
                ] {
                    args.push(a.into());
                }
            }
        } else if use_zgc {
            args.push("-XX:+UseZGC".into());
            if (21..24).contains(&java.major_version) {
                if java.major_version < 23 {
                    args.push("-XX:+UnlockExperimentalVMOptions".into());
                }
                args.push("-XX:+ZGenerational".into());
            }
        }
    }

    if java.major_version >= 24 && java.is_64bit {
        args.push("-XX:+UseCompactObjectHeaders".into());
    }

    let _ = mc_folder;
    deduplicate_args(&args)
}

/// 构建游戏参数（对齐 buildGameArgs）
pub fn build_game_args(
    version_json: &Value,
    version: &McVersion,
    settings: &Settings,
    options: &LaunchOptions,
) -> Vec<String> {
    let mut args = Vec::new();

    if let Some(game) = version_json.get("arguments").and_then(|a| a.get("game")) {
        args.extend(collect_arg_list(game));
    } else if let Some(old) = json_str(version_json, "minecraftArguments") {
        args.extend(split_java_args(old));
        args.push("--height".into());
        args.push("${resolution_height}".into());
        args.push("--width".into());
        args.push("${resolution_width}".into());
    }

    let mut custom = settings
        .get_instance(&version.id, "VersionAdvanceGame")
        .unwrap_or_default();
    if custom.is_empty() {
        custom = settings.get_string("LaunchAdvanceGame").unwrap_or_default();
    }
    if !custom.is_empty() {
        args.extend(split_java_args(&custom));
    }

    args.extend(options.extra_game_args.iter().cloned());
    if options.fullscreen {
        args.push("--fullscreen".into());
    }

    if !options.server_ip.is_empty() {
        // releaseTime > 2023-04-04 → quickPlayMultiplayer
        let new_api = version.release_time.as_str() >= "2023-04-04";
        if new_api {
            args.push("--quickPlayMultiplayer".into());
            args.push(options.server_ip.clone());
        } else {
            args.push("--server".into());
            if let Some((host, port)) = options.server_ip.split_once(':') {
                args.push(host.to_string());
                args.push("--port".into());
                args.push(port.to_string());
            } else {
                args.push(options.server_ip.clone());
                args.push("--port".into());
                args.push("25565".into());
            }
        }
    }

    deduplicate_args(&args)
}

// ---------------------------------------------------------------- 替换表

/// 对齐 buildReplacements
pub fn build_replacements(
    version_json: &Value,
    version: &McVersion,
    _java: &JavaEntry,
    login: &LoginResult,
    options: &LaunchOptions,
    mc_folder: &Path,
) -> BTreeMap<String, String> {
    let mut r: BTreeMap<String, String> = BTreeMap::new();
    let mc = platform::normalize_path_string(mc_folder);
    let mc = if mc.ends_with('/') {
        mc
    } else {
        format!("{mc}/")
    };

    r.insert("auth_player_name".into(), login.name.clone());
    r.insert("auth_uuid".into(), login.uuid.clone());
    r.insert("auth_access_token".into(), login.access_token.clone());
    r.insert("auth_session".into(), login.access_token.clone());
    r.insert("auth_xuid".into(), String::new());
    let user_type = if login.login_type == "Ms" {
        "msa"
    } else {
        "legacy"
    };
    r.insert("user_type".into(), user_type.into());
    let props = if login.profile_json.is_empty() {
        "{}".into()
    } else {
        login.profile_json.clone()
    };
    r.insert("user_properties".into(), props);

    r.insert("version_name".into(), version.id.clone());
    r.insert("version_type".into(), version.kind.clone());
    r.insert("launcher_name".into(), "MLC".into());
    r.insert("launcher_version".into(), "0.1".into());
    r.insert(
        "clientid".into(),
        if login.client_token.is_empty() {
            "0".into()
        } else {
            login.client_token.clone()
        },
    );

    let path_indie = if version.path_indie.is_empty() {
        mc.clone()
    } else {
        let p = version.path_indie.replace('\\', "/");
        if p.ends_with('/') {
            p
        } else {
            format!("{p}/")
        }
    };
    r.insert("game_directory".into(), path_indie);
    r.insert("game_assets".into(), format!("{mc}assets/virtual/legacy/"));

    let mut assets_index = "legacy".to_string();
    if let Some(a) = json_str(version_json, "assets") {
        assets_index = a.to_string();
    }
    if let Some(id) = version_json
        .get("assetIndex")
        .and_then(|a| json_str(a, "id"))
    {
        assets_index = id.to_string();
    }
    r.insert("assets_index_name".into(), assets_index);
    r.insert("assets_root".into(), format!("{mc}assets/"));

    // Classpath
    let skip_main_jar = (version.mod_loader.has_forge || version.mod_loader.has_neoforge)
        && version.vanilla_version.major() == 1
        && version.vanilla_version.minor() >= 17;

    struct LibEntry {
        key: String,
        path: String,
        ver: McVersionNumber,
    }
    let mut lib_entries = Vec::new();
    if let Some(libs) = version_json.get("libraries").and_then(|l| l.as_array()) {
        for lib in libs {
            let Some(lib) = lib.as_object() else {
                continue;
            };
            let lib_v = Value::Object(lib.clone());
            if let Some(rules) = lib_v.get("rules") {
                if !check_rules(rules) {
                    continue;
                }
            }
            let maven_name = json_str(&lib_v, "name").unwrap_or("").to_string();
            let path = if let Some(p) = lib_v
                .get("downloads")
                .and_then(|d| d.get("artifact"))
                .and_then(|a| json_str(a, "path"))
            {
                format!("{mc}libraries/{p}")
            } else {
                if lib_v.get("natives").is_some() {
                    continue;
                }
                let rel = maven_name_to_path(&maven_name);
                if rel.is_empty() {
                    continue;
                }
                format!("{mc}libraries/{rel}")
            };
            let parts: Vec<&str> = maven_name.split(':').collect();
            let key = if parts.len() >= 3 {
                let mut k = format!("{}:{}", parts[0], parts[1]);
                if parts.len() > 3 {
                    k.push(':');
                    k.push_str(parts[3]);
                }
                k
            } else {
                path.clone()
            };
            let ver = if parts.len() >= 3 {
                McVersionNumber::parse(parts[2])
            } else {
                McVersionNumber::empty()
            };
            lib_entries.push(LibEntry { key, path, ver });
        }
    }

    let mut classpath_parts = Vec::new();
    let mut emitted: std::collections::HashSet<String> = Default::default();
    for e in &lib_entries {
        if emitted.contains(&e.key) {
            continue;
        }
        let has_higher = lib_entries.iter().any(|o| o.key == e.key && o.ver > e.ver);
        if has_higher {
            continue;
        }
        emitted.insert(e.key.clone());
        classpath_parts.push(e.path.clone());
    }
    if !skip_main_jar && !version.path_jar.is_empty() {
        classpath_parts.push(version.path_jar.clone());
    }
    let sep = if cfg!(target_os = "windows") {
        ";"
    } else {
        ":"
    };
    r.insert("classpath".into(), classpath_parts.join(sep));
    r.insert("classpath_separator".into(), sep.into());

    let vanilla = version.vanilla_version.to_string();
    r.insert(
        "natives_directory".into(),
        format!("{mc}versions/{vanilla}/natives/"),
    );
    r.insert("resolution_width".into(), options.window_width.to_string());
    r.insert(
        "resolution_height".into(),
        options.window_height.to_string(),
    );
    r.insert("library_directory".into(), format!("{mc}libraries/"));
    r
}

/// 对齐 applyReplacements（不做引号）
pub fn apply_replacements(args: &[String], repl: &BTreeMap<String, String>) -> Vec<String> {
    args.iter()
        .map(|arg| {
            let mut a = arg.clone();
            for (k, v) in repl {
                a = a.replace(&format!("${{{k}}}"), v);
            }
            a
        })
        .collect()
}

// ---------------------------------------------------------------- 完整构建

/// 从 version json + 登录态组装完整启动命令（对齐 LaunchBuilder::build + doLaunch 前半）
pub fn build_launch_command(
    version_json: &Value,
    version: &McVersion,
    java: &JavaEntry,
    login: &LoginResult,
    options: &LaunchOptions,
    settings: &Settings,
    mc_folder: &Path,
) -> Result<LaunchCommand, String> {
    if version_json.is_null() {
        return Err("No version JSON set".into());
    }
    let main_class = json_str(version_json, "mainClass")
        .unwrap_or("")
        .to_string();

    let mut jvm_args = build_jvm_args(version_json, version, java, settings, options, mc_folder);
    let game_args = build_game_args(version_json, version, settings, options);

    // authlib-injector javaagent
    if login.login_type == "Auth" && !login.server_url.is_empty() {
        let mc = platform::normalize_path_string(mc_folder);
        let jar = format!("{mc}/authlib-injector.jar");
        // normalize 可能无尾斜杠
        let jar = jar.replace("//", "/");
        if !Path::new(&jar).exists() {
            return Err(format!("authlib-injector.jar missing at {jar}"));
        }
        jvm_args.insert(0, format!("-javaagent:{}={}", jar, login.server_url));
    }

    let repl = build_replacements(version_json, version, java, login, options, mc_folder);
    let mut argv = vec![java.path_java.clone()];
    argv.extend(apply_replacements(&jvm_args, &repl));
    if !main_class.is_empty() {
        argv.push(main_class.clone());
    }
    argv.extend(apply_replacements(&game_args, &repl));

    // 环境变量与工作目录
    let mut game_dir = version.path_indie.replace('\\', "/");
    if game_dir.ends_with('/') {
        game_dir.pop();
    }
    if game_dir.is_empty() {
        let mc = platform::normalize_path_string(mc_folder);
        game_dir = mc.trim_end_matches('/').to_string();
    }

    let mut env: BTreeMap<String, String> = BTreeMap::new();
    // PATH 前置 java bin
    let path_var = std::env::var("PATH")
        .or_else(|_| std::env::var("Path"))
        .unwrap_or_default();
    let java_bin = platform::normalize_path_string(Path::new(&java.path_folder));
    let java_bin = java_bin.trim_end_matches('/').to_string();
    let sep = platform::path_env_sep();
    env.insert("PATH".into(), format!("{java_bin}{sep}{path_var}"));
    if cfg!(target_os = "windows") {
        env.insert("APPDATA".into(), game_dir.clone());
    }
    env.insert("MINECRAFT_LAUNCHER_NAME".into(), "MLC".into());
    env.insert("MINECRAFT_LAUNCHER_VERSION".into(), "0.1".into());
    env.insert("__GL_THREADED_OPTIMIZATIONS".into(), "0".into());

    // Linux：无 GLFW34 修复时禁用 XIM
    if cfg!(target_os = "linux") {
        let vanilla = version.vanilla_version.to_string();
        let marker = format!(
            "{}/versions/{vanilla}/natives/libglfw.so.glfw34-fixed",
            platform::normalize_path_string(mc_folder).trim_end_matches('/')
        )
        .replace("//", "/");
        let glfw34 = Path::new(&marker).exists();
        let keep_xim = std::env::var_os("MLC_KEEP_XIM").is_some();
        if !keep_xim && !glfw34 {
            env.insert("XMODIFIERS".into(), "@im=none".into());
        }
    }

    Ok(LaunchCommand {
        main_class,
        jvm_args: apply_replacements(&jvm_args, &repl),
        game_args: apply_replacements(&game_args, &repl),
        argv,
        java_path: java.path_java.clone(),
        working_dir: game_dir,
        env,
    })
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::path::PathBuf;

    fn java21() -> JavaEntry {
        JavaEntry {
            path_folder: "/j/bin/".into(),
            path_java: "/j/bin/java".into(),
            version: McVersionNumber::parse("21.0.2.0"),
            major_version: 21,
            is_jre: true,
            is_64bit: true,
            is_user_import: false,
        }
    }

    fn ver(vanilla: &str) -> McVersion {
        McVersion {
            id: "1.20.1".into(),
            kind: "release".into(),
            is_valid: true,
            vanilla_version: McVersionNumber::parse(vanilla),
            path_indie: "/mc/".into(),
            path_jar: "/mc/versions/1.20.1/1.20.1.jar".into(),
            ..Default::default()
        }
    }

    fn temp_settings() -> Settings {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("mlc-launch-{}-{n}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        Settings::load(&dir.join("MLC.ini"))
    }

    #[test]
    fn 自动内存夹紧() {
        let mb = auto_max_memory_mb();
        assert!((2048..=16384).contains(&mb));
        assert_eq!(mb % 512, 0);
    }

    #[test]
    fn 规则与quickplay特征() {
        assert!(check_rules(&json!([])));
        assert!(check_rules(&json!([
            {"action": "allow", "os": {"name": platform::platform_name()}}
        ])));
        assert!(!check_rules(&json!([
            {"action": "disallow", "os": {"name": platform::platform_name()}}
        ])));
        assert!(!check_features(&json!({"is_demo_user": true})));
        assert!(!check_features(&json!({"has_quick_plays_support": true})));
        assert!(check_features(&json!({"has_custom_resolution": true})));
    }

    #[test]
    fn 内存与gc注入() {
        let s = temp_settings();
        let v = ver("1.20.1");
        let j = java21();
        let opts = LaunchOptions {
            max_memory_mb: 4096,
            min_memory_mb: 512,
            ..Default::default()
        };
        let vj = json!({"mainClass": "net.minecraft.client.main.Main"});
        let args = build_jvm_args(&vj, &v, &j, &s, &opts, Path::new("/mc"));
        assert!(args.contains(&"-Xmx4096m".to_string()));
        assert!(args.contains(&"-Xms512m".to_string()));
        // Java 21 + Auto → ZGC
        assert!(args.contains(&"-XX:+UseZGC".to_string()));
        assert!(args.iter().any(|a| a.contains("HeapDumpPath"))); // legacy 固定 JVM
    }

    #[test]
    fn 替换与classpath() {
        let v = ver("1.20.1");
        let j = java21();
        let login = LoginResult {
            name: "Steve".into(),
            uuid: "u1".into(),
            access_token: "tok".into(),
            login_type: "Legacy".into(),
            ..Default::default()
        };
        let opts = LaunchOptions::default();
        let vj = json!({
            "assets": "17",
            "libraries": [
                {"name": "com.example:lib:1.0.0"},
                {"name": "com.example:lib:1.2.0"},
                {"name": "org.example:other:1.0", "natives": {"linux": "n"}},
                {"name": "com.example:lib:0.9.0"}
            ]
        });
        let r = build_replacements(&vj, &v, &j, &login, &opts, Path::new("/mc"));
        assert_eq!(r["auth_player_name"], "Steve");
        assert_eq!(r["assets_index_name"], "17");
        // 同名库保留最高版本
        let cp = &r["classpath"];
        assert!(cp.contains("lib-1.2.0.jar") || cp.contains("lib-1.2.0"));
        assert!(!cp.contains("lib-1.0.0"));
        assert!(!cp.contains("lib-0.9.0"));
        // natives 容器不进 classpath
        assert!(!cp.contains("org/example/other"));
        assert!(cp.ends_with("/mc/versions/1.20.1/1.20.1.jar") || cp.contains("1.20.1.jar"));

        let args = vec![
            "--username".to_string(),
            "${auth_player_name}".to_string(),
            "--gameDir".to_string(),
            "${game_directory}".to_string(),
        ];
        let out = apply_replacements(&args, &r);
        assert_eq!(out[1], "Steve");
        assert!(out[3].contains("/mc") || out[3].ends_with("mc/"));
    }

    #[test]
    fn 旧格式游戏参数与服务器端口拆分() {
        let s = temp_settings();
        let mut v = ver("1.12.2");
        v.release_time = "2017-09-18T00:00:00+00:00".into();
        let opts = LaunchOptions {
            server_ip: "play.example.com:25566".into(),
            ..Default::default()
        };
        let vj = json!({"minecraftArguments": "--username ${auth_player_name}"});
        let args = build_game_args(&vj, &v, &s, &opts);
        assert!(args.contains(&"--server".to_string()));
        assert!(args.contains(&"play.example.com".to_string()));
        assert!(args.contains(&"--port".to_string()));
        assert!(args.contains(&"25566".to_string()));
        assert!(args.contains(&"--height".to_string()));
    }

    #[test]
    fn 新格式quickplay服务器() {
        let s = temp_settings();
        let mut v = ver("1.20.1");
        v.release_time = "2023-06-12T00:00:00+00:00".into();
        let opts = LaunchOptions {
            server_ip: "mc.example.com".into(),
            ..Default::default()
        };
        let vj = json!({"arguments": {"game": ["--username", "${auth_player_name}"]}});
        let args = build_game_args(&vj, &v, &s, &opts);
        assert!(args.contains(&"--quickPlayMultiplayer".to_string()));
        assert!(args.contains(&"mc.example.com".to_string()));
    }

    #[test]
    fn 完整命令组装() {
        let s = temp_settings();
        let mut v = ver("1.20.1");
        v.path_indie = "/mc/".into();
        let j = java21();
        let login = LoginResult {
            name: "Alex".into(),
            uuid: "u2".into(),
            access_token: "t".into(),
            login_type: "Legacy".into(),
            ..Default::default()
        };
        let vj = json!({
            "mainClass": "net.minecraft.client.main.Main",
            "arguments": {
                "jvm": ["-Djava.library.path=${natives_directory}", "-cp", "${classpath}"],
                "game": ["--username", "${auth_player_name}", "--version", "${version_name}"]
            }
        });
        let cmd = build_launch_command(
            &vj,
            &v,
            &j,
            &login,
            &LaunchOptions::default(),
            &s,
            Path::new("/mc"),
        )
        .unwrap();
        assert_eq!(cmd.main_class, "net.minecraft.client.main.Main");
        assert_eq!(cmd.argv[0], "/j/bin/java");
        assert!(cmd.argv.contains(&"Alex".to_string()));
        assert!(cmd.argv.contains(&"1.20.1".to_string()));
        assert!(cmd.env.contains_key("MINECRAFT_LAUNCHER_NAME"));
        assert_eq!(cmd.env["MINECRAFT_LAUNCHER_NAME"], "MLC");
        let _: PathBuf = PathBuf::from(&cmd.working_dir);
    }
}
