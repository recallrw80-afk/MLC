//! 整合包管线，对应 C++ `sdk/src/modpack/`：detect → common → installers → pipeline。
//!
//! 本切片覆盖：类型检测、实例目录名、`.incomplete` 标记与整体回滚、名称校验。
//! Forge/NeoForge/Fabric 安装器 processor 流程在后续切片。

use std::path::{Path, PathBuf};

use crate::settings::Settings;
use crate::util::file;
use crate::util::platform;

// ---------------------------------------------------------------- 类型

/// 对齐 PackType
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PackType {
    Unknown,
    /// manifest.json（无 addons）
    CurseForge,
    /// modpack.json
    Hmcl,
    /// mmc-pack.json
    MultiMc,
    /// mcbbs.packmeta 或 manifest.json（有 addons）
    Mcbbs,
    /// modrinth.index.json
    Modrinth,
    /// 只有 jar
    Mod,
    /// 内含 modpack.zip / modpack.mrpack
    LauncherPack,
    /// 压缩版 .minecraft
    Compressed,
}

impl PackType {
    pub fn name(&self) -> &'static str {
        match self {
            PackType::CurseForge => "CurseForge",
            PackType::Hmcl => "HMCL",
            PackType::MultiMc => "MultiMC (MMC)",
            PackType::Mcbbs => "MCBBS",
            PackType::Modrinth => "Modrinth",
            PackType::Mod => "Mod 包 (Mod Pack)",
            PackType::LauncherPack => "Launcher Pack",
            PackType::Compressed => "Compressed .minecraft",
            PackType::Unknown => "Unknown",
        }
    }
}

// ---------------------------------------------------------------- 检测

fn has_root(roots: &[String], name: &str) -> bool {
    roots.iter().any(|r| r == name)
}

fn has_first(first_level: &[String], name: &str) -> bool {
    first_level.iter().any(|e| {
        e.split_once('/')
            .map(|(_, fn_)| fn_ == name)
            .unwrap_or(false)
    })
}

/// 检测整合包类型（打开 zip 扫描标记文件；对齐 detectPackType）
pub fn detect_pack_type(file_path: &Path) -> PackType {
    let entries = file::list_zip_entries(file_path);
    if entries.is_empty() {
        return PackType::Unknown;
    }

    let mut roots = Vec::new();
    let mut first_level = Vec::new();
    for e in &entries {
        if !e.contains('/') {
            roots.push(e.clone());
        } else {
            let slash = e.find('/').unwrap();
            if e.rfind('/') == Some(slash) {
                first_level.push(e.clone());
            }
        }
    }

    // Type 3: MCBBS
    if has_root(&roots, "mcbbs.packmeta") || has_first(&first_level, "mcbbs.packmeta") {
        return PackType::Mcbbs;
    }
    // Type 2: MultiMC
    if has_root(&roots, "mmc-pack.json") || has_first(&first_level, "mmc-pack.json") {
        return PackType::MultiMc;
    }
    // Type 4: Modrinth
    if has_root(&roots, "modrinth.index.json") || has_first(&first_level, "modrinth.index.json") {
        return PackType::Modrinth;
    }
    // Type 0/3: manifest.json
    if has_root(&roots, "manifest.json") || has_first(&first_level, "manifest.json") {
        let manifest_path = if has_root(&roots, "manifest.json") {
            Some("manifest.json".to_string())
        } else {
            first_level
                .iter()
                .find(|e| e.ends_with("/manifest.json"))
                .cloned()
        };
        if let Some(mp) = manifest_path {
            if let Some(data) = file::read_zip_entry(file_path, &mp) {
                if let Ok(obj) = serde_json::from_slice::<serde_json::Value>(&data) {
                    if obj.get("addons").is_some() {
                        return PackType::Mcbbs;
                    }
                    return PackType::CurseForge;
                }
            }
        }
        return PackType::CurseForge;
    }
    // Type 1: HMCL
    if has_root(&roots, "modpack.json") || has_first(&first_level, "modpack.json") {
        return PackType::Hmcl;
    }
    // Type 9: Launcher pack
    if has_root(&roots, "modpack.zip")
        || has_root(&roots, "modpack.mrpack")
        || has_first(&first_level, "modpack.zip")
        || has_first(&first_level, "modpack.mrpack")
    {
        return PackType::LauncherPack;
    }
    // Type 9b: 单个内层 zip/mrpack 递归探测
    {
        let mut inner_zip: Option<String> = None;
        let mut zip_count = 0;
        for e in &entries {
            let slashes = e.matches('/').count();
            if slashes > 1 {
                continue;
            }
            let fn_ = if slashes == 0 {
                e.as_str()
            } else {
                e.split_once('/').map(|(_, f)| f).unwrap_or(e.as_str())
            };
            let lower = fn_.to_ascii_lowercase();
            if lower.ends_with(".zip") || lower.ends_with(".mrpack") {
                inner_zip = Some(e.clone());
                zip_count += 1;
            }
        }
        if zip_count == 1 {
            if let Some(inner_path) = inner_zip {
                let tmp =
                    std::env::temp_dir().join(format!("_mlc_detect_{}.zip", std::process::id()));
                let _ = std::fs::remove_file(&tmp);
                if file::extract_zip_entry(file_path, &inner_path, &tmp) {
                    let inner_entries = file::list_zip_entries(&tmp);
                    let _ = std::fs::remove_file(&tmp);
                    for e in inner_entries {
                        if e.starts_with(".minecraft/")
                            || e.contains("/.minecraft/")
                            || e.contains("versions/")
                        {
                            return PackType::LauncherPack;
                        }
                    }
                }
            }
        }
    }

    // Compressed .minecraft
    for e in &entries {
        if e.contains("versions/") && e.ends_with(".json") {
            return PackType::Compressed;
        }
    }
    // Mod 包
    for e in &entries {
        if e.to_ascii_lowercase().ends_with(".jar") {
            return PackType::Mod;
        }
    }
    PackType::Unknown
}

// ---------------------------------------------------------------- common

/// 随机 8 位实例目录名（对齐 generateInstanceDir）
pub fn generate_instance_dir() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    const CHARS: &[u8] = b"abcdefghijklmnopqrstuvwxyz0123456789";
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0);
    let mut state = nanos ^ (std::process::id() as u64) << 32;
    let mut out = String::with_capacity(8);
    for _ in 0..8 {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        let idx = (state % CHARS.len() as u64) as usize;
        out.push(CHARS[idx] as char);
    }
    out
}

/// 校验实例名：拒绝空、路径分隔符、`..`
pub fn validate_instance_name(name: &str) -> bool {
    if name.is_empty() {
        return false;
    }
    if name.contains('/') || name.contains('\\') || name.contains("..") || name == "." {
        return false;
    }
    true
}

/// 在 archive 条目里找 .minecraft 根前缀（对齐 findMcRoot）
pub fn find_mc_root(entries: &[String]) -> String {
    // (.+/)versions/[^/]+/[^/]+\.json
    for e in entries {
        if let Some(pos) = e.find("/versions/") {
            let rest = &e[pos + "/versions/".len()..];
            // versions/<id>/<id>.json
            let mut parts = rest.splitn(2, '/');
            if let (Some(_id), Some(file)) = (parts.next(), parts.next()) {
                if file.ends_with(".json") && !file.contains('/') {
                    return e[..pos + 1].to_string();
                }
            }
        } else if let Some(rest) = e.strip_prefix("versions/") {
            // 无前缀：versions/X/X.json → 根为 ""
            let mut parts = rest.splitn(2, '/');
            if let (Some(_id), Some(file)) = (parts.next(), parts.next()) {
                if file.ends_with(".json") && !file.contains('/') {
                    return String::new();
                }
            }
        }
    }
    String::new()
}

/// 提取纯净 MC 版本号（对齐 extractVanillaVersion）
pub fn extract_vanilla_version(v: &str) -> String {
    let mut out = String::new();
    let mut parts = 0;
    for (i, c) in v.char_indices() {
        if c.is_ascii_digit() {
            out.push(c);
        } else if c == '.' && !out.is_empty() && parts < 2 && i + 1 < v.len() {
            // 允许最多两段点：1.20.1
            let next_is_digit = v[i + 1..].starts_with(|c: char| c.is_ascii_digit());
            if next_is_digit && parts < 2 {
                out.push('.');
                parts += 1;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if out.is_empty() {
        v.to_string()
    } else {
        out
    }
}

/// 标记导入中（.incomplete 文件放在实例目录旁）
pub fn mark_incomplete(final_dir: &Path) {
    let _ = std::fs::create_dir_all(final_dir);
    let marker = final_dir.with_extension("incomplete");
    // with_extension 会去掉最后一段：instances/abc/ → instances/abc.incomplete 不对
    // C++ 是 finalDir + ".incomplete"（finalDir 带尾斜杠时等价于同级标记）
    let _ = marker;
    let mut s = final_dir.to_string_lossy().replace('\\', "/");
    if s.ends_with('/') {
        s.pop();
    }
    s.push_str(".incomplete");
    let _ = std::fs::write(s, b"");
}

/// 移除导入完成标记
pub fn mark_complete(final_dir: &Path) {
    let mut s = final_dir.to_string_lossy().replace('\\', "/");
    if s.ends_with('/') {
        s.pop();
    }
    s.push_str(".incomplete");
    let _ = std::fs::remove_file(s);
}

/// 是否带导入中标记
pub fn is_incomplete(final_dir: &Path) -> bool {
    let mut s = final_dir.to_string_lossy().replace('\\', "/");
    if s.ends_with('/') {
        s.pop();
    }
    s.push_str(".incomplete");
    Path::new(&s).exists()
}

fn incomplete_marker_path(final_dir: &Path) -> PathBuf {
    let mut s = final_dir.to_string_lossy().replace('\\', "/");
    if s.ends_with('/') {
        s.pop();
    }
    s.push_str(".incomplete");
    PathBuf::from(s)
}

/// 本进程导入临时目录 {mcFolder}/tmp/<pid>/
pub fn pack_tmp_root(mc_folder: &Path) -> PathBuf {
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    PathBuf::from(format!("{mc}/tmp/{}", std::process::id()))
}

/// 清理本进程 tmp/<pid>/
pub fn cleanup_pack_tmp(mc_folder: &Path) {
    file::remove_tree(&pack_tmp_root(mc_folder));
}

/// 导入失败回滚：删实例目录 + INI 映射 + 本进程 tmp
pub fn cleanup_on_error(settings: &mut Settings, mc_folder: &Path, final_dir: &Path) {
    let dir_name = final_dir
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    file::remove_tree(final_dir);
    let _ = std::fs::remove_file(incomplete_marker_path(final_dir));
    if !dir_name.is_empty() {
        settings.remove_instance_dir(&dir_name);
    }
    cleanup_pack_tmp(mc_folder);
}

/// 名称冲突检查（对齐 checkNameConflict）
pub fn check_name_conflict(
    settings: &Settings,
    mc_folder: &Path,
    target_dir: &Path,
    name: &str,
    explicit_name: bool,
) -> Result<(), String> {
    if !validate_instance_name(name) {
        return Err(format!("Invalid instance name: \"{name}\""));
    }
    let existing = settings.dir_for_display_name(name).unwrap_or_default();
    if !existing.is_empty() {
        let mc = platform::normalize_path_string(mc_folder);
        let mc = mc.trim_end_matches('/');
        let path = PathBuf::from(format!("{mc}/instances/{existing}"));
        if path.is_dir() {
            if explicit_name {
                return Err(format!(
                    "Instance \"{name}\" already exists, use a different name"
                ));
            }
            return Err(format!(
                "Instance \"{name}\" already exists, use --r <name>"
            ));
        }
    }
    if target_dir.exists() {
        return Err("Internal error: directory name collision, please retry".into());
    }
    Ok(())
}

/// 各安装器共用前置：解析实例名 → 冲突检查 → 建目录 + .incomplete
/// 返回 (final_dir, name)
pub fn begin_install(
    settings: &mut Settings,
    mc_folder: &Path,
    instance_name: &str,
    pack_name: &str,
) -> Result<(PathBuf, String), String> {
    let name = if instance_name.is_empty() {
        pack_name.to_string()
    } else {
        instance_name.to_string()
    };
    let instance_dir = generate_instance_dir();
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    let final_dir = PathBuf::from(format!("{mc}/instances/{instance_dir}"));
    check_name_conflict(
        settings,
        mc_folder,
        &final_dir,
        &name,
        !instance_name.is_empty(),
    )?;
    mark_incomplete(&final_dir);
    Ok((final_dir, name))
}

/// 成功收尾：写 INI 映射 + 删标记 + 清 tmp
pub fn finalize_install(
    settings: &mut Settings,
    mc_folder: &Path,
    final_dir: &Path,
    display_name: &str,
) {
    if let Some(dir_name) = final_dir
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
    {
        settings.set_instance_dir(&dir_name, display_name);
    }
    mark_complete(final_dir);
    cleanup_pack_tmp(mc_folder);
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::io::Write;

    fn temp_zip(tag: &str, files: &[(&str, &[u8])]) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mlc-modpack-{}-{tag}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let zip_path = dir.join("pack.zip");
        let file = fs::File::create(&zip_path).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        let opts = zip::write::SimpleFileOptions::default();
        for (name, data) in files {
            if name.ends_with('/') {
                zip.add_directory(*name, opts).unwrap();
            } else {
                zip.start_file(*name, opts).unwrap();
                zip.write_all(data).unwrap();
            }
        }
        zip.finish().unwrap();
        zip_path
    }

    fn temp_settings(tag: &str) -> Settings {
        let dir = std::env::temp_dir().join(format!("mlc-pk-{}-{tag}", std::process::id()));
        let _ = fs::create_dir_all(&dir);
        let mut s = Settings::load(&dir.join("MLC.ini"));
        s.set_string("LaunchFolderSelect", &format!("{}/", dir.display()));
        s
    }

    #[test]
    fn 检测curseforge与mcbbs() {
        let cf = temp_zip(
            "cf",
            &[
                ("manifest.json", br#"{"minecraft":{"version":"1.20.1"}}"#),
                ("mods/a.jar", b"jar"),
            ],
        );
        assert_eq!(detect_pack_type(&cf), PackType::CurseForge);

        let mcbbs = temp_zip(
            "mcbbs",
            &[(
                "manifest.json",
                br#"{"addons":{},"minecraft":{"version":"1.20.1"}}"#,
            )],
        );
        assert_eq!(detect_pack_type(&mcbbs), PackType::Mcbbs);

        let meta = temp_zip("meta", &[("mcbbs.packmeta", b"{}")]);
        assert_eq!(detect_pack_type(&meta), PackType::Mcbbs);
    }

    #[test]
    fn 检测multimc_modrinth_hmcl() {
        assert_eq!(
            detect_pack_type(&temp_zip("mmc", &[("mmc-pack.json", b"{}")])),
            PackType::MultiMc
        );
        assert_eq!(
            detect_pack_type(&temp_zip("mr", &[("modrinth.index.json", b"{}")])),
            PackType::Modrinth
        );
        assert_eq!(
            detect_pack_type(&temp_zip("hmcl", &[("modpack.json", b"{}")])),
            PackType::Hmcl
        );
    }

    #[test]
    fn 检测mod_compressed_launcherpack() {
        assert_eq!(
            detect_pack_type(&temp_zip("mod", &[("foo.jar", b"jar")])),
            PackType::Mod
        );
        assert_eq!(
            detect_pack_type(&temp_zip(
                "comp",
                &[("versions/1.20.1/1.20.1.json", br#"{"id":"1.20.1"}"#)]
            )),
            PackType::Compressed
        );
        assert_eq!(
            detect_pack_type(&temp_zip("lp", &[("modpack.zip", b"PK")])),
            PackType::LauncherPack
        );
    }

    #[test]
    fn 一级子目录标记也能识别() {
        let z = temp_zip(
            "first",
            &[("overrides/manifest.json", br#"{"minecraft":{}}"#)],
        );
        assert_eq!(detect_pack_type(&z), PackType::CurseForge);
    }

    #[test]
    fn 实例名校验与目录生成() {
        assert!(validate_instance_name("我的包"));
        assert!(!validate_instance_name(""));
        assert!(!validate_instance_name("a/b"));
        assert!(!validate_instance_name(".."));
        assert!(!validate_instance_name("."));
        let a = generate_instance_dir();
        let b = generate_instance_dir();
        assert_eq!(a.len(), 8);
        assert!(a
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit()));
        assert_ne!(a, b);
    }

    #[test]
    fn incomplete标记与回滚() {
        let mut s = temp_settings("rollback");
        let mc = PathBuf::from(
            s.get_string("LaunchFolderSelect")
                .unwrap()
                .trim_end_matches('/')
                .to_string(),
        );
        let (final_dir, name) = begin_install(&mut s, &mc, "", "Pack").unwrap();
        assert!(final_dir.is_dir());
        assert!(is_incomplete(&final_dir));
        assert_eq!(name, "Pack");

        // 模拟安装半成品
        fs::write(final_dir.join("half.txt"), b"x").unwrap();
        cleanup_on_error(&mut s, &mc, &final_dir);
        assert!(!final_dir.exists());
        assert!(!is_incomplete(&final_dir));
        assert!(s.dir_for_display_name("Pack").is_none());

        // 成功路径
        let (final_dir, name) = begin_install(&mut s, &mc, "正式包", "").unwrap();
        fs::write(final_dir.join("ok.txt"), b"x").unwrap();
        finalize_install(&mut s, &mc, &final_dir, &name);
        assert!(!is_incomplete(&final_dir));
        assert!(s.dir_for_display_name("正式包").is_some());
    }

    #[test]
    fn 名称冲突与非法名() {
        let mut s = temp_settings("conflict");
        let mc = PathBuf::from(
            s.get_string("LaunchFolderSelect")
                .unwrap()
                .trim_end_matches('/')
                .to_string(),
        );
        let (dir, name) = begin_install(&mut s, &mc, "已存在", "").unwrap();
        finalize_install(&mut s, &mc, &dir, &name);

        let target = mc.join("instances").join("zzzzzzzz");
        assert!(check_name_conflict(&s, &mc, &target, "已存在", true).is_err());
        assert!(check_name_conflict(&s, &mc, &target, "已存在", false).is_err());
        assert!(check_name_conflict(&s, &mc, &target, "新名字", true).is_ok());
        assert!(check_name_conflict(&s, &mc, &target, "bad/name", true).is_err());
    }

    #[test]
    fn 原版版本号提取() {
        assert_eq!(
            extract_vanilla_version("1.21.1-NeoForge_21.1.226"),
            "1.21.1"
        );
        assert_eq!(extract_vanilla_version("1.20.1"), "1.20.1");
        // C++ 正则 ^\d+\.\d+ 要求开头即数字，非数字开头原样返回
        assert_eq!(
            extract_vanilla_version("fabric-loader-0.15.7-1.20.1"),
            "fabric-loader-0.15.7-1.20.1"
        );
    }

    #[test]
    fn find_mc_root前缀识别() {
        let entries = vec![
            "foo/versions/1.20.1/1.20.1.json".to_string(),
            "foo/mods/a.jar".to_string(),
        ];
        assert_eq!(find_mc_root(&entries), "foo/");
        let entries = vec!["versions/1.20.1/1.20.1.json".to_string()];
        assert_eq!(find_mc_root(&entries), "");
        assert_eq!(find_mc_root(&["mods/a.jar".into()]), "");
    }

    #[test]
    fn zip提取与列表() {
        let z = temp_zip(
            "ex",
            &[
                ("a.txt", b"hello"),
                ("dir/b.txt", b"world"),
                ("版本/中文.jar", b"gbk"),
            ],
        );
        let names = file::list_zip_entries(&z);
        assert!(names.iter().any(|n| n == "a.txt"));
        assert!(names.iter().any(|n| n == "dir/b.txt"));
        assert!(names.iter().any(|n| n.contains("中文")));

        let data = file::read_zip_entry(&z, "a.txt").unwrap();
        assert_eq!(data, b"hello");

        let dest = std::env::temp_dir().join(format!("mlc-zx-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dest);
        assert!(file::extract_zip_entry(&z, "a.txt", &dest.join("a.txt")));
        assert_eq!(fs::read(dest.join("a.txt")).unwrap(), b"hello");

        let (ok, failed) = file::extract_zip(&z, &dest).unwrap();
        assert!(ok >= 3);
        assert_eq!(failed, 0);
        assert!(dest.join("dir/b.txt").exists());
    }
}
