//! Java 管理，对应 C++ `javamanager.cpp`。无现成 crate（全是领域逻辑）：
//! 候选路径枚举（JAVA_HOME/PATH/注册表//usr/lib/jvm/java_home）→ `java -version` 解析
//! → MC 版本兼容矩阵 → Adoptium API 下载。基于 std::process / std::fs。
//!
//! 本切片覆盖：探测、校验、版本解析、兼容矩阵、选择、Adoptium URL。
//! 真正下载/解压 JRE 在 installJava 切片接入 DownloadManager。

use std::path::{Path, PathBuf};
use std::process::Command;

use crate::util::platform;
use crate::version::{McVersion, McVersionNumber};

// ---------------------------------------------------------------- 类型

/// 对齐 JavaEntry
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct JavaEntry {
    /// java 所在目录（尾斜杠）
    pub path_folder: String,
    /// java 可执行文件完整路径
    pub path_java: String,
    /// 完整版本（段比较用）
    pub version: McVersionNumber,
    /// 主版本：1.8.x → 8；21.0.x → 21
    pub major_version: i32,
    pub is_jre: bool,
    pub is_64bit: bool,
    pub is_user_import: bool,
}

impl JavaEntry {
    /// 对齐 C++ toString()（C++ 里 versionStr 计算后未使用，只用 version.toString()）
    pub fn display(&self) -> String {
        format!(
            "{} {} ({}){}: {}",
            if self.is_jre { "JRE" } else { "JDK" },
            self.major_version,
            self.version,
            if self.is_64bit { "" } else { ", 32-bit" },
            self.path_folder
        )
    }
}

// ---------------------------------------------------------------- 解析

/// 解析 `java -version` 输出（对齐 parseJavaVersionOutput）
pub fn parse_java_version_output(output: &str) -> Option<McVersionNumber> {
    let version_str = extract_quoted_version(output).or_else(|| extract_loose_version(output))?;
    let mut version_str = version_str.replace('_', ".");
    if let Some(dash) = version_str.find('-') {
        if dash > 0 {
            version_str.truncate(dash);
        }
    }

    let mut segments = Vec::new();
    for part in version_str.split('.') {
        match part.parse::<i32>() {
            Ok(v) => segments.push(v),
            Err(_) => break,
        }
    }
    if segments.is_empty() {
        return None;
    }
    while segments.len() < 4 {
        segments.push(0);
    }
    Some(McVersionNumber::from_segments(&segments))
}

/// `version "…"`
fn extract_quoted_version(output: &str) -> Option<String> {
    let lower = output; // 保持原文大小写；关键字 version 大小写不敏感
    let bytes = lower.as_bytes();
    let mut i = 0;
    while i + 7 <= bytes.len() {
        if output[i..i + 7].eq_ignore_ascii_case("version") {
            let rest = output[i + 7..].trim_start();
            if let Some(stripped) = rest.strip_prefix('"') {
                if let Some(end) = stripped.find('"') {
                    return Some(stripped[..end].to_string());
                }
            }
        }
        i += 1;
    }
    None
}

/// 备用：`(\d+[\._]\d+[\._]\d+[\._]?\d*)`
fn extract_loose_version(output: &str) -> Option<String> {
    let chars: Vec<char> = output.chars().collect();
    let n = chars.len();
    let mut i = 0;
    while i < n {
        if !chars[i].is_ascii_digit() {
            i += 1;
            continue;
        }
        // 尝试匹配 数字._ 数字._ 数字 (._数字)?
        let mut j = i;
        let mut parts = 0;
        let mut last_sep = false;
        while j < n {
            let c = chars[j];
            if c.is_ascii_digit() {
                last_sep = false;
                j += 1;
                continue;
            }
            if (c == '.' || c == '_') && !last_sep && j + 1 < n && chars[j + 1].is_ascii_digit() {
                last_sep = true;
                parts += 1;
                j += 1;
                continue;
            }
            break;
        }
        if parts >= 2 {
            return Some(chars[i..j].iter().collect());
        }
        i += 1;
    }
    None
}

/// 主版本：1.8.x → 8；21.x → 21
fn major_from_version(v: &McVersionNumber) -> i32 {
    if v.major() == 1 {
        v.minor()
    } else {
        v.major()
    }
}

// ---------------------------------------------------------------- 校验与探测

fn java_binary_exists(bin_dir: &Path) -> PathBuf {
    bin_dir.join(platform::java_bin_name())
}

fn is_java_binary(path: &Path) -> bool {
    path.file_name()
        .map(|n| {
            let n = n.to_string_lossy().to_lowercase();
            n == "java" || n == "java.exe"
        })
        .unwrap_or(false)
}

/// 对齐 checkJava：跑 `java -version`，填充版本/位数/JRE
pub fn check_java(mut entry: JavaEntry) -> Option<JavaEntry> {
    let java_path = Path::new(&entry.path_java);
    if !java_path.exists() {
        return None;
    }

    let javac = Path::new(&entry.path_folder).join(if cfg!(target_os = "windows") {
        "javac.exe"
    } else {
        "javac"
    });
    entry.is_jre = !javac.exists();

    let output = Command::new(java_path).arg("-version").output().ok()?;
    // 合并 stdout/stderr（java -version 通常写 stderr）
    let mut text = String::from_utf8_lossy(&output.stdout).to_string();
    text.push_str(&String::from_utf8_lossy(&output.stderr));
    let text = text.trim();
    if text.is_empty() {
        return None;
    }

    let version = parse_java_version_output(text)?;
    entry.version = version.clone();
    entry.major_version = major_from_version(&version);

    let lower = text.to_lowercase();
    entry.is_64bit = lower.contains("64-bit") || lower.contains("64 bit");

    if entry.major_version <= 4 || entry.major_version >= 100 {
        return None;
    }
    Some(entry)
}

fn add_unique(list: &mut Vec<JavaEntry>, entry: JavaEntry) {
    if list.iter().any(|e| e.path_folder == entry.path_folder) {
        return;
    }
    list.push(entry);
}

fn scan_path_env(list: &mut Vec<JavaEntry>) {
    let path_env = std::env::var_os("PATH")
        .or_else(|| std::env::var_os("Path"))
        .unwrap_or_default();
    let path_env = path_env.to_string_lossy().to_string();
    for part in path_env.split(platform::path_env_sep()) {
        let trimmed = part.trim();
        if trimmed.is_empty() {
            continue;
        }
        let dir = PathBuf::from(trimmed);
        let java_path = java_binary_exists(&dir);
        if java_path.exists() && is_java_binary(&java_path) {
            let entry = JavaEntry {
                path_folder: format!("{}/", platform::normalize_path_string(&dir)),
                path_java: platform::normalize_path_string(&java_path),
                is_user_import: false,
                ..Default::default()
            };
            if let Some(ok) = check_java(entry) {
                add_unique(list, ok);
            }
        }
    }
}

fn scan_java_home(list: &mut Vec<JavaEntry>) {
    let Ok(java_home) = std::env::var("JAVA_HOME") else {
        return;
    };
    if java_home.is_empty() {
        return;
    }
    let bin = Path::new(&java_home).join("bin");
    let java_path = java_binary_exists(&bin);
    if java_path.exists() {
        let entry = JavaEntry {
            path_folder: format!("{}/", platform::normalize_path_string(&bin)),
            path_java: platform::normalize_path_string(&java_path),
            is_user_import: false,
            ..Default::default()
        };
        if let Some(ok) = check_java(entry) {
            add_unique(list, ok);
        }
    }
}

fn scan_folder(list: &mut Vec<JavaEntry>, folder: &Path, is_user_import: bool) {
    let Ok(dir) = std::fs::read_dir(folder) else {
        return;
    };
    for entry in dir.flatten() {
        let path = entry.path();
        if path.is_dir() {
            // 标准 JDK 布局
            let java_path = java_binary_exists(&path.join("bin"));
            if java_path.exists() {
                let je = JavaEntry {
                    path_folder: format!("{}/", platform::normalize_path_string(&path.join("bin"))),
                    path_java: platform::normalize_path_string(&java_path),
                    is_user_import,
                    ..Default::default()
                };
                if let Some(ok) = check_java(je) {
                    add_unique(list, ok);
                }
            }
            // macOS JVM bundle
            let mac_path = java_binary_exists(&path.join("Contents/Home/bin"));
            if mac_path.exists() {
                let je = JavaEntry {
                    path_folder: format!(
                        "{}/",
                        platform::normalize_path_string(&path.join("Contents/Home/bin"))
                    ),
                    path_java: platform::normalize_path_string(&mac_path),
                    is_user_import,
                    ..Default::default()
                };
                if let Some(ok) = check_java(je) {
                    add_unique(list, ok);
                }
            }
        } else if is_java_binary(&path) {
            let parent = path.parent().unwrap_or(folder);
            let je = JavaEntry {
                path_folder: format!("{}/", platform::normalize_path_string(parent)),
                path_java: platform::normalize_path_string(&path),
                is_user_import,
                ..Default::default()
            };
            if let Some(ok) = check_java(je) {
                add_unique(list, ok);
            }
        }
    }
}

/// 对齐 javaSearchPaths
pub fn java_search_paths(mc_folder: &Path) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let mut add = |p: PathBuf| {
        if p.is_dir() && !paths.contains(&p) {
            paths.push(p);
        }
    };

    // MLC 自动下载的 Java
    if !mc_folder.as_os_str().is_empty() {
        add(mc_folder.join("javas"));
    }

    if cfg!(target_os = "windows") {
        for base in [
            "C:/Program Files/Java",
            "C:/Program Files (x86)/Java",
            "C:/Program Files/Eclipse Adoptium",
            "C:/Program Files/AdoptOpenJDK",
            "C:/Program Files/Zulu",
            "C:/Program Files/Semeru",
            "C:/Program Files/Microsoft",
        ] {
            add(PathBuf::from(base));
        }
        for drive in 'C'..='Z' {
            let root = format!("{drive}:/");
            if Path::new(&root).is_dir() {
                add(PathBuf::from(format!("{root}Program Files/Java")));
                add(PathBuf::from(format!(
                    "{root}Program Files/Eclipse Adoptium"
                )));
            }
        }
    } else if cfg!(target_os = "macos") {
        for p in [
            "/Library/Java/JavaVirtualMachines",
            "/opt/homebrew/opt/openjdk",
            "/opt/homebrew/opt/openjdk@17",
            "/opt/homebrew/opt/openjdk@11",
            "/opt/homebrew/opt/openjdk@8",
            "/usr/local/opt/openjdk",
            "/usr/local/opt/openjdk@17",
        ] {
            add(PathBuf::from(p));
        }
    } else {
        for p in [
            "/usr/lib/jvm",
            "/usr/lib64/jvm",
            "/usr/local/lib/jvm",
            "/opt/jdk",
            "/opt/java",
        ] {
            add(PathBuf::from(p));
        }
        // /usr/lib/jvm 子目录的 bin/java
        if let Ok(entries) = std::fs::read_dir("/usr/lib/jvm") {
            for e in entries.flatten() {
                if e.path().join("bin").join("java").exists() {
                    add(e.path().join("bin"));
                }
            }
        }
    }

    paths
}

/// 系统扫描：PATH → JAVA_HOME → 搜索路径（对齐 scanSystemJava 的完成语义）
pub fn scan_system_java(mc_folder: &Path) -> Vec<JavaEntry> {
    let mut list = Vec::new();
    scan_path_env(&mut list);
    scan_java_home(&mut list);
    for dir in java_search_paths(mc_folder) {
        scan_folder(&mut list, &dir, false);
    }
    list
}

// ---------------------------------------------------------------- 兼容矩阵

/// 对齐 getJavaCompatibilityRange。返回 (min, max)；None = 无限制
pub fn get_java_compatibility_range(
    version: &McVersion,
) -> (Option<McVersionNumber>, Option<McVersionNumber>) {
    let mut out_min: Option<McVersionNumber> = None;
    let mut out_max: Option<McVersionNumber> = None;

    let feature = if version.vanilla_version.major() == 1 {
        version.vanilla_version.minor()
    } else {
        version.vanilla_version.major()
    };
    let patch = if version.vanilla_version.segment_count() > 2 {
        version.vanilla_version.segment(2)
    } else {
        0
    };

    fn apply_max(out_max: &mut Option<McVersionNumber>, v: McVersionNumber) {
        match out_max {
            None => *out_max = Some(v),
            Some(cur) => {
                if v < *cur {
                    *out_max = Some(v);
                }
            }
        }
    }
    fn apply_min(out_min: &mut Option<McVersionNumber>, v: McVersionNumber) {
        match out_min {
            None => *out_min = Some(v),
            Some(cur) => {
                if v > *cur {
                    *out_min = Some(v);
                }
            }
        }
    }

    let j = |segs: &[i32]| McVersionNumber::from_segments(segs);

    // 原版基线
    if feature < 8 {
        apply_max(&mut out_max, j(&[1, 8, 999, 999]));
    } else if (8..=12).contains(&feature) {
        apply_min(&mut out_min, j(&[1, 8, 0, 0]));
        apply_max(&mut out_max, j(&[1, 8, 999, 999]));
    } else if (13..=16).contains(&feature) {
        apply_min(&mut out_min, j(&[1, 8, 0, 0]));
    } else if feature == 17 {
        apply_min(&mut out_min, j(&[16, 0, 0, 0]));
    } else if feature >= 18 {
        apply_min(&mut out_min, j(&[17, 0, 0, 0]));
    }
    if feature >= 21 || (feature == 20 && patch >= 5) {
        apply_min(&mut out_min, j(&[21, 0, 0, 0]));
    }

    // Forge
    if version.mod_loader.has_forge && version.is_valid {
        let forge_ver = McVersionNumber::parse(&version.mod_loader.forge_version);
        if (6..=7).contains(&feature) && patch <= 2 {
            apply_min(&mut out_min, j(&[1, 7, 0, 0]));
            apply_max(&mut out_max, j(&[1, 7, 999, 999]));
        } else if feature <= 12 {
            apply_max(&mut out_max, j(&[1, 8, 999, 999]));
        } else if feature <= 14 {
            apply_min(&mut out_min, j(&[1, 8, 0, 0]));
            apply_max(&mut out_max, j(&[10, 999, 999, 999]));
        } else if feature == 15 {
            apply_min(&mut out_min, j(&[1, 8, 0, 0]));
            apply_max(&mut out_max, j(&[15, 999, 999, 999]));
        } else if feature == 16 && !forge_ver.is_empty() {
            if forge_ver >= j(&[34, 0, 0]) && forge_ver <= j(&[36, 2, 25]) {
                apply_max(&mut out_max, j(&[1, 8, 0, 320]));
            } else if forge_ver >= j(&[36, 2, 26]) && forge_ver < j(&[37, 0, 0]) {
                apply_max(&mut out_max, j(&[23, 999, 999, 999]));
            }
        } else if feature == 17 && !forge_ver.is_empty() {
            if forge_ver >= j(&[37, 0, 0]) && forge_ver <= j(&[37, 0, 79]) {
                apply_max(&mut out_max, j(&[16, 999, 999, 999]));
            }
        } else if feature == 18 && version.mod_loader.has_optifine {
            apply_max(&mut out_max, j(&[18, 999, 999, 999]));
        } else if feature == 19
            && !forge_ver.is_empty()
            && forge_ver >= j(&[45, 0, 21])
            && forge_ver <= j(&[45, 0, 65])
        {
            apply_max(&mut out_max, j(&[19, 999, 999, 999]));
        }
        if !forge_ver.is_empty() && forge_ver >= j(&[45, 0, 66]) && forge_ver <= j(&[47, 4, 8]) {
            apply_max(&mut out_max, j(&[21, 999, 999, 999]));
        }
    }

    // NeoForge
    if version.mod_loader.has_neoforge && feature == 20 {
        apply_max(&mut out_max, j(&[21, 999, 999, 999]));
    }

    // Fabric
    if version.mod_loader.has_fabric && version.is_valid {
        if (15..=16).contains(&feature) {
            apply_min(&mut out_min, j(&[1, 8, 0, 0]));
        } else if feature >= 18 {
            apply_min(&mut out_min, j(&[17, 0, 0, 0]));
        }
    }

    // LiteLoader
    if version.mod_loader.has_liteloader {
        apply_max(&mut out_max, j(&[1, 8, 999, 999]));
    }

    (out_min, out_max)
}

// ---------------------------------------------------------------- 选择

/// 对齐 selectJava：区间过滤后优先 64 位 → 最低版本 → JDK
pub fn select_java(
    list: &[JavaEntry],
    min: Option<&McVersionNumber>,
    max: Option<&McVersionNumber>,
) -> Option<JavaEntry> {
    let mut candidates: Vec<&JavaEntry> = list
        .iter()
        .filter(|java| {
            if let Some(min) = min {
                if !min.is_empty() && java.version < *min {
                    return false;
                }
            }
            if let Some(max) = max {
                if !max.is_empty() && java.version > *max {
                    return false;
                }
            }
            if java.is_64bit && !platform::is_64bit() {
                return false;
            }
            true
        })
        .collect();

    if candidates.is_empty() {
        return None;
    }

    candidates.sort_by(|a, b| {
        b.is_64bit
            .cmp(&a.is_64bit)
            .then(a.version.cmp(&b.version))
            .then(b.is_jre.cmp(&a.is_jre))
    });
    candidates.first().map(|e| (*e).clone())
}

/// 对齐 selectJavaForVersion
pub fn select_java_for_version(list: &[JavaEntry], version: &McVersion) -> Option<JavaEntry> {
    let (min, max) = get_java_compatibility_range(version);
    select_java(list, min.as_ref(), max.as_ref())
}

/// Adoptium 下载 URL（对齐 getJavaDownloadUrl / installJavaRuntime）
pub fn adoptium_download_url(major_version: i32) -> Option<String> {
    let os = if cfg!(target_os = "windows") {
        "windows"
    } else if cfg!(target_os = "macos") {
        "mac"
    } else {
        "linux"
    };
    let arch = if platform::cpu_arch() == "aarch64" {
        "aarch64"
    } else if platform::is_64bit() {
        "x64"
    } else {
        "x86"
    };
    Some(format!(
        "https://api.adoptium.net/v3/binary/latest/{major_version}/ga/{os}/{arch}/jre/hotspot/normal/eclipse"
    ))
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    fn mc(s: &str) -> McVersionNumber {
        McVersionNumber::parse(s)
    }

    fn entry(version: &str, major: i32, is64: bool, is_jre: bool) -> JavaEntry {
        JavaEntry {
            path_folder: format!("/java/{version}/bin/"),
            path_java: format!("/java/{version}/bin/java"),
            version: mc(version),
            major_version: major,
            is_jre,
            is_64bit: is64,
            is_user_import: false,
        }
    }

    #[test]
    fn 解析java版本输出() {
        assert_eq!(
            parse_java_version_output(r#"openjdk version "1.8.0_321""#)
                .unwrap()
                .to_string(),
            "1.8.0.321"
        );
        assert_eq!(
            parse_java_version_output(r#"openjdk version "21.0.2" 2024-01-16"#)
                .unwrap()
                .major(),
            21
        );
        assert_eq!(
            parse_java_version_output(r#"openjdk version "17.0.9" 2023-10-17"#)
                .unwrap()
                .major(),
            17
        );
        // 备用模式
        assert_eq!(
            parse_java_version_output("something 21.0.2-beta")
                .unwrap()
                .major(),
            21
        );
        assert!(parse_java_version_output("no version here").is_none());
    }

    #[test]
    fn 兼容矩阵关键节点() {
        let mut v = McVersion {
            is_valid: true,
            vanilla_version: mc("1.20.1"),
            ..Default::default()
        };
        let (min, max) = get_java_compatibility_range(&v);
        assert_eq!(min.unwrap(), mc("17.0.0.0"));
        assert!(max.is_none());

        v.vanilla_version = mc("1.20.5");
        let (min, _) = get_java_compatibility_range(&v);
        assert_eq!(min.unwrap(), mc("21.0.0.0"));

        v.vanilla_version = mc("1.12.2");
        let (min, max) = get_java_compatibility_range(&v);
        assert_eq!(min.unwrap(), mc("1.8.0.0"));
        assert_eq!(max.unwrap(), mc("1.8.999.999"));

        // Forge 47.2.0 on 1.20.1 → max Java 21
        v.vanilla_version = mc("1.20.1");
        v.mod_loader.has_forge = true;
        v.mod_loader.forge_version = "47.2.0".into();
        let (_, max) = get_java_compatibility_range(&v);
        assert_eq!(max.unwrap(), mc("21.999.999.999"));

        // LiteLoader → max Java 8
        v.mod_loader = Default::default();
        v.mod_loader.has_liteloader = true;
        let (_, max) = get_java_compatibility_range(&v);
        assert_eq!(max.unwrap(), mc("1.8.999.999"));
    }

    #[test]
    fn 选择java优先64位与最低版本() {
        let list = vec![
            entry("1.8.0.321", 8, true, true),
            entry("17.0.9", 17, true, true),
            entry("21.0.2", 21, true, true),
            entry("21.0.2", 21, false, false), // 32-bit JDK
        ];
        let (min, max) = (Some(mc("17.0.0.0")), Some(mc("21.999.999.999")));
        let picked = select_java(&list, min.as_ref(), max.as_ref()).unwrap();
        assert_eq!(picked.major_version, 17); // 区间内最低
        assert!(picked.is_64bit);

        let min21 = Some(mc("21.0.0.0"));
        let picked = select_java(&list, min21.as_ref(), None).unwrap();
        assert_eq!(picked.major_version, 21);
        assert!(picked.is_64bit); // 优先 64 位
    }

    #[test]
    fn adoptium地址格式() {
        let url = adoptium_download_url(17).unwrap();
        assert!(url.contains("https://api.adoptium.net/v3/binary/latest/17/ga/"));
        assert!(url.contains("/jre/hotspot/normal/eclipse"));
    }

    #[test]
    fn display字符串格式() {
        let mut e = entry("1.8.0.321", 8, true, true);
        e.path_folder = "/opt/jdk8/bin/".into();
        assert_eq!(e.display(), "JRE 8 (1.8.0.321): /opt/jdk8/bin/");
        let mut e = entry("21.0.2", 21, false, false);
        e.path_folder = "/opt/jdk21/bin/".into();
        // entry 直接 parse 不填充段；真实 check_java 会 pad 到 4 段
        assert_eq!(e.display(), "JDK 21 (21.0.2), 32-bit: /opt/jdk21/bin/");
    }
}
