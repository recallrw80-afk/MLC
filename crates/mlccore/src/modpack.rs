//! 整合包管线，对应 C++ `sdk/src/modpack/`：detect → common → installers → pipeline。
//!
//! 已覆盖：类型检测、`.incomplete` 回滚、清单解析、Compressed/Mod 本地安装、
//! 以及经 `crate::installer` 的 Forge/NeoForge/Fabric 安装。

use std::path::{Path, PathBuf};

use crate::settings::Settings;
use crate::util::file;
use crate::util::platform;

// ---------------------------------------------------------------- 类型

/// 对齐 PackType
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PackType {
    Unknown,
    CurseForge,
    Hmcl,
    MultiMc,
    Mcbbs,
    Modrinth,
    Mod,
    LauncherPack,
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

/// 检测整合包类型（对齐 detectPackType）
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

    if has_root(&roots, "mcbbs.packmeta") || has_first(&first_level, "mcbbs.packmeta") {
        return PackType::Mcbbs;
    }
    if has_root(&roots, "mmc-pack.json") || has_first(&first_level, "mmc-pack.json") {
        return PackType::MultiMc;
    }
    if has_root(&roots, "modrinth.index.json") || has_first(&first_level, "modrinth.index.json") {
        return PackType::Modrinth;
    }
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
    if has_root(&roots, "modpack.json") || has_first(&first_level, "modpack.json") {
        return PackType::Hmcl;
    }
    if has_root(&roots, "modpack.zip")
        || has_root(&roots, "modpack.mrpack")
        || has_first(&first_level, "modpack.zip")
        || has_first(&first_level, "modpack.mrpack")
    {
        return PackType::LauncherPack;
    }
    // 单个内层 zip/mrpack 递归探测
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

    for e in &entries {
        if e.contains("versions/") && e.ends_with(".json") {
            return PackType::Compressed;
        }
    }
    for e in &entries {
        if e.to_ascii_lowercase().ends_with(".jar") {
            return PackType::Mod;
        }
    }
    PackType::Unknown
}

// ---------------------------------------------------------------- common

/// 随机 8 位实例目录名
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

pub fn validate_instance_name(name: &str) -> bool {
    if name.is_empty() {
        return false;
    }
    if name.contains('/') || name.contains('\\') || name.contains("..") || name == "." {
        return false;
    }
    true
}

/// 在 archive 条目里找 .minecraft 根前缀
pub fn find_mc_root(entries: &[String]) -> String {
    for e in entries {
        if let Some(pos) = e.find("/versions/") {
            let rest = &e[pos + "/versions/".len()..];
            let mut parts = rest.splitn(2, '/');
            if let (Some(_id), Some(file)) = (parts.next(), parts.next()) {
                if file.ends_with(".json") && !file.contains('/') {
                    return e[..pos + 1].to_string();
                }
            }
        } else if let Some(rest) = e.strip_prefix("versions/") {
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

/// 提取纯净 MC 版本号
pub fn extract_vanilla_version(v: &str) -> String {
    let mut out = String::new();
    let mut parts = 0;
    for (i, c) in v.char_indices() {
        if c.is_ascii_digit() {
            out.push(c);
        } else if c == '.' && !out.is_empty() && parts < 2 && i + 1 < v.len() {
            let next_is_digit = v[i + 1..].starts_with(|c: char| c.is_ascii_digit());
            if next_is_digit {
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

fn incomplete_marker_path(final_dir: &Path) -> PathBuf {
    let mut s = final_dir.to_string_lossy().replace('\\', "/");
    if s.ends_with('/') {
        s.pop();
    }
    s.push_str(".incomplete");
    PathBuf::from(s)
}

pub fn mark_incomplete(final_dir: &Path) {
    let _ = std::fs::create_dir_all(final_dir);
    let _ = std::fs::write(incomplete_marker_path(final_dir), b"");
}

pub fn mark_complete(final_dir: &Path) {
    let _ = std::fs::remove_file(incomplete_marker_path(final_dir));
}

pub fn is_incomplete(final_dir: &Path) -> bool {
    incomplete_marker_path(final_dir).exists()
}

pub fn pack_tmp_root(mc_folder: &Path) -> PathBuf {
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    PathBuf::from(format!("{mc}/tmp/{}", std::process::id()))
}

pub fn cleanup_pack_tmp(mc_folder: &Path) {
    file::remove_tree(&pack_tmp_root(mc_folder));
}

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

// ---------------------------------------------------------------- 安装器

/// Mod 下载条目
#[derive(Debug, Clone, Default)]
pub struct ModDownloadEntry {
    pub url: String,
    pub save_path: String,
    pub cf_mod_id: String,
    pub cf_file_id: String,
}

/// 写 PCL Setup.ini
pub fn write_setup_ini(
    final_dir: &Path,
    name: &str,
    version_json_name: Option<&str>,
) -> std::io::Result<()> {
    let dir = final_dir.join("PCL");
    std::fs::create_dir_all(&dir)?;
    let mut body = String::from("[Setup]\n");
    body.push_str(&format!("Name={name}\n"));
    body.push_str("VersionArgumentIndie=1\n");
    body.push_str("VersionArgumentIndieV2=true\n");
    if let Some(v) = version_json_name.filter(|s| !s.is_empty()) {
        body.push_str(&format!("Version={v}\n"));
    }
    std::fs::write(dir.join("Setup.ini"), body)
}

/// 实例应记录的版本 json 名（对齐 resolveInstanceVersionName）
pub fn resolve_instance_version_name(
    final_dir: &Path,
    mc_version: &str,
    loader_type: &str,
    loader_ver: &str,
) -> String {
    // 实例内版本文件夹优先
    let vd = final_dir.join("versions");
    if let Ok(entries) = std::fs::read_dir(&vd) {
        for entry in entries.flatten() {
            if !entry.path().is_dir() {
                continue;
            }
            let dn = entry.file_name().to_string_lossy().to_string();
            if entry.path().join(format!("{dn}.json")).exists() {
                return dn;
            }
        }
    }
    if mc_version.is_empty() {
        return String::new();
    }
    let vanilla = extract_vanilla_version(mc_version);
    // 在 mc 根 versions/ 里按 loader 前缀找（final_dir = mc/instances/<id>，根是其祖父）
    // 更稳妥：从 final_dir 向上找 mc 根
    let mc_root = final_dir
        .parent() // instances
        .and_then(|p| p.parent()) // mc root
        .map(|p| p.to_path_buf());
    if let Some(root) = mc_root {
        let prefix = match loader_type {
            "forge" => format!("{vanilla}-forge-"),
            "neoforge" => "neoforge-".to_string(),
            "fabric" => "fabric-loader-".to_string(),
            _ => format!("{vanilla}-"),
        };
        if let Ok(entries) = std::fs::read_dir(root.join("versions")) {
            let mut fallback = String::new();
            for entry in entries.flatten() {
                if !entry.path().is_dir() {
                    continue;
                }
                let dn = entry.file_name().to_string_lossy().to_string();
                if !dn.starts_with(&prefix) {
                    continue;
                }
                if !entry.path().join(format!("{dn}.json")).exists() {
                    continue;
                }
                if !loader_ver.is_empty() && dn.contains(loader_ver) {
                    return dn;
                }
                if fallback.is_empty() {
                    fallback = dn;
                }
            }
            if !fallback.is_empty() {
                return fallback;
            }
        }
    }
    vanilla
}

/// 解析 CurseForge manifest.json → (mc, name, forge, neo, fabric, mods)
pub type CfManifestParts = (
    String,
    String,
    String,
    String,
    String,
    Vec<ModDownloadEntry>,
);

pub fn parse_curseforge_manifest(pack_dir: &Path) -> Result<CfManifestParts, String> {
    let path = pack_dir.join("manifest.json");
    let text = std::fs::read_to_string(&path).map_err(|_| "Cannot read manifest.json")?;
    let m: serde_json::Value =
        serde_json::from_str(&text).map_err(|_| "manifest.json parse failed")?;
    let mc = m
        .pointer("/minecraft/version")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let name = m
        .get("name")
        .and_then(|v| v.as_str())
        .unwrap_or("Modpack")
        .to_string();
    let mut forge = String::new();
    let mut neo = String::new();
    let mut fabric = String::new();
    if let Some(loaders) = m
        .pointer("/minecraft/modLoaders")
        .and_then(|v| v.as_array())
    {
        for loader in loaders {
            let id = loader.get("id").and_then(|v| v.as_str()).unwrap_or("");
            if let Some(v) = id.strip_prefix("forge-") {
                forge = v.to_string();
            } else if let Some(v) = id.strip_prefix("neoforge-") {
                neo = v.to_string();
            } else if let Some(v) = id.strip_prefix("fabric-") {
                fabric = v.to_string();
            }
        }
    }
    let mut mods = Vec::new();
    if let Some(files) = m.get("files").and_then(|v| v.as_array()) {
        for f in files {
            let project = f
                .get("projectID")
                .and_then(|v| v.as_i64())
                .unwrap_or(0)
                .to_string();
            let file = f
                .get("fileID")
                .and_then(|v| v.as_i64())
                .unwrap_or(0)
                .to_string();
            mods.push(ModDownloadEntry {
                save_path: format!("mods/{project}_{file}.jar"),
                cf_mod_id: project,
                cf_file_id: file,
                ..Default::default()
            });
        }
    }
    Ok((mc, name, forge, neo, fabric, mods))
}

pub fn curseforge_overrides_dir(manifest_json: &serde_json::Value) -> String {
    manifest_json
        .get("overrides")
        .and_then(|v| v.as_str())
        .unwrap_or("overrides")
        .to_string()
}

pub fn parse_hmcl_manifest(pack_dir: &Path) -> Result<(String, String), String> {
    let path = pack_dir.join("modpack.json");
    let text = std::fs::read_to_string(&path).map_err(|_| "Cannot read modpack.json")?;
    let m: serde_json::Value =
        serde_json::from_str(&text).map_err(|_| "modpack.json parse failed")?;
    Ok((
        m.get("gameVersion")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string(),
        m.get("name")
            .and_then(|v| v.as_str())
            .unwrap_or("HMCL Modpack")
            .to_string(),
    ))
}

pub fn parse_multimc_manifest(
    pack_dir: &Path,
) -> Result<(String, String, String, String, String), String> {
    let path = pack_dir.join("mmc-pack.json");
    let text = std::fs::read_to_string(&path).map_err(|_| "Cannot read mmc-pack.json")?;
    let m: serde_json::Value =
        serde_json::from_str(&text).map_err(|_| "mmc-pack.json parse failed")?;
    let mut mc = String::new();
    let mut forge = String::new();
    let mut neo = String::new();
    let mut fabric = String::new();
    if let Some(comps) = m.get("components").and_then(|v| v.as_array()) {
        for c in comps {
            let uid = c.get("uid").and_then(|v| v.as_str()).unwrap_or("");
            let ver = c.get("version").and_then(|v| v.as_str()).unwrap_or("");
            match uid {
                "net.minecraft" => mc = ver.to_string(),
                "net.minecraftforge" => forge = ver.to_string(),
                "net.neoforged" => neo = ver.to_string(),
                "net.fabricmc.fabric-loader" => fabric = ver.to_string(),
                _ => {}
            }
        }
    }
    let name = m
        .get("name")
        .and_then(|v| v.as_str())
        .unwrap_or("MMC Modpack")
        .to_string();
    Ok((mc, name, forge, neo, fabric))
}

pub fn parse_mcbbs_manifest(
    pack_dir: &Path,
) -> Result<(String, String, String, String, String, String), String> {
    let (path, is_manifest) = if pack_dir.join("mcbbs.packmeta").exists() {
        (pack_dir.join("mcbbs.packmeta"), false)
    } else {
        (pack_dir.join("manifest.json"), true)
    };
    let text = std::fs::read_to_string(&path).map_err(|_| "packmeta parse failed")?;
    let m: serde_json::Value = serde_json::from_str(&text).map_err(|_| "packmeta parse failed")?;
    let mut mc = String::new();
    let mut forge = String::new();
    let mut neo = String::new();
    let mut fabric = String::new();
    if let Some(addons) = m.get("addons").and_then(|v| v.as_array()) {
        for a in addons {
            let id = a.get("id").and_then(|v| v.as_str()).unwrap_or("");
            let ver = a.get("version").and_then(|v| v.as_str()).unwrap_or("");
            match id {
                "game" => mc = ver.to_string(),
                "forge" => forge = ver.to_string(),
                "neoforge" => neo = ver.to_string(),
                "fabric" => fabric = ver.to_string(),
                _ => {}
            }
        }
    }
    let name = m
        .get("name")
        .and_then(|v| v.as_str())
        .unwrap_or("MCBBS Modpack")
        .to_string();
    let overrides = if is_manifest {
        m.get("overrides")
            .and_then(|v| v.as_str())
            .unwrap_or("overrides")
            .to_string()
    } else {
        "overrides".to_string()
    };
    Ok((mc, name, forge, neo, fabric, overrides))
}

/// 递归收集 jar 拍平到实例 mods/
pub fn install_mod(
    settings: &Settings,
    mc_folder: &Path,
    pack_dir: &Path,
    target_instance: &str,
) -> Result<usize, String> {
    if target_instance.is_empty() {
        return Err("此 zip 为 mod 包，需要加 --to <实例名>".into());
    }
    let dir_name = settings
        .dir_for_display_name(target_instance)
        .unwrap_or_default();
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    let inst_dir = PathBuf::from(format!("{mc}/instances/{dir_name}"));
    if dir_name.is_empty() || !inst_dir.is_dir() {
        return Err(format!("目标实例不存在: {target_instance}"));
    }
    let mods_dir = inst_dir.join("mods");
    let _ = std::fs::create_dir_all(&mods_dir);
    let mut copied = 0;
    copy_jars_recursive(pack_dir, &mods_dir, &mut copied);
    Ok(copied)
}

fn copy_jars_recursive(src: &Path, dest_mods: &Path, copied: &mut usize) {
    let Ok(entries) = std::fs::read_dir(src) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            copy_jars_recursive(&path, dest_mods, copied);
        } else {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.to_ascii_lowercase().ends_with(".jar") {
                let dest = dest_mods.join(&name);
                let _ = std::fs::remove_file(&dest);
                if std::fs::copy(&path, &dest).is_ok() {
                    *copied += 1;
                }
            }
        }
    }
}

fn merge_shared_dirs(final_dir: &Path, mc_folder: &Path) -> Result<(), String> {
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    for shared in ["assets", "libraries"] {
        let src = final_dir.join(shared);
        if src.is_dir() {
            let dst = PathBuf::from(format!("{mc}/{shared}"));
            if !file::copy_dir(&src, &dst) {
                return Err(format!("Copy failed: {}", src.display()));
            }
            file::remove_tree(&src);
        }
    }
    Ok(())
}

/// Compressed 安装（本地：解压 → 共享目录合并 → Setup.ini）
pub fn install_compressed(
    settings: &mut Settings,
    mc_folder: &Path,
    file_path: &Path,
    instance_name: &str,
) -> Result<String, String> {
    let entries = file::list_zip_entries(file_path);
    let mc_root = find_mc_root(&entries);
    let pack_name = file_path
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "Compressed".into());

    let (final_dir, name) = begin_install(settings, mc_folder, instance_name, &pack_name)?;

    let tmp = pack_tmp_root(mc_folder);
    let instance_dir = final_dir
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    let work_dir = tmp.join(format!("extract_{instance_dir}"));
    file::remove_tree(&work_dir);
    let _ = std::fs::create_dir_all(&work_dir);

    if let Err(e) = file::extract_zip(file_path, &work_dir) {
        file::remove_tree(&work_dir);
        cleanup_on_error(settings, mc_folder, &final_dir);
        return Err(e);
    }

    if !mc_root.is_empty() {
        let inner = work_dir.join(mc_root.trim_end_matches('/'));
        if inner.is_dir() && inner != work_dir {
            let Ok(rd) = std::fs::read_dir(&inner) else {
                cleanup_on_error(settings, mc_folder, &final_dir);
                return Err("Copy inner content failed".into());
            };
            for e in rd.flatten() {
                let to = work_dir.join(e.file_name());
                let from = e.path();
                let ok = if from.is_dir() {
                    file::copy_dir(&from, &to)
                } else {
                    std::fs::copy(&from, &to).is_ok()
                };
                if !ok {
                    file::remove_tree(&work_dir);
                    cleanup_on_error(settings, mc_folder, &final_dir);
                    return Err("Copy inner content failed".into());
                }
            }
            file::remove_tree(&inner);
        }
    }

    if !file::copy_dir(&work_dir, &final_dir) {
        file::remove_tree(&work_dir);
        cleanup_on_error(settings, mc_folder, &final_dir);
        return Err("Copy to instance directory failed".into());
    }
    file::remove_tree(&work_dir);

    if let Err(e) = merge_shared_dirs(&final_dir, mc_folder) {
        cleanup_on_error(settings, mc_folder, &final_dir);
        return Err(e);
    }

    write_setup_ini(&final_dir, &name, None).map_err(|e| e.to_string())?;
    finalize_install(settings, mc_folder, &final_dir, &name);
    Ok(name)
}

/// CurseForge 本地准备结果
pub struct CfPrepared {
    pub final_dir: PathBuf,
    pub name: String,
    pub mc_version: String,
    pub forge: String,
    pub neo: String,
    pub fabric: String,
    pub mods: Vec<ModDownloadEntry>,
}

/// CurseForge 本地准备：begin + overrides + mod 列表
pub fn prepare_curseforge(
    settings: &mut Settings,
    mc_folder: &Path,
    pack_dir: &Path,
    instance_name: &str,
) -> Result<CfPrepared, String> {
    let (mc, pack_name, forge, neo, fabric, mods) = parse_curseforge_manifest(pack_dir)?;
    let (final_dir, name) = begin_install(settings, mc_folder, instance_name, &pack_name)?;

    let manifest_path = pack_dir.join("manifest.json");
    let overrides = std::fs::read_to_string(&manifest_path)
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .map(|j| curseforge_overrides_dir(&j))
        .unwrap_or_else(|| "overrides".into());
    let src_override = pack_dir.join(&overrides);
    if src_override.is_dir() && !file::copy_dir(&src_override, &final_dir) {
        cleanup_on_error(settings, mc_folder, &final_dir);
        return Err("Copy failed: overrides".into());
    }
    Ok(CfPrepared {
        final_dir,
        name,
        mc_version: mc,
        forge,
        neo,
        fabric,
        mods,
    })
}

/// 下载管线入参
pub struct FinalizeRequest<'a> {
    pub final_dir: &'a Path,
    pub name: &'a str,
    pub mc_version: &'a str,
    pub forge_ver: &'a str,
    pub neo_ver: &'a str,
    pub fabric_ver: &'a str,
    pub mods: &'a [ModDownloadEntry],
}

/// 下载管线。顺序：MC 本体 → modloader → mods → finalize。任一步失败整体回滚。
pub async fn download_and_finalize(
    settings: &mut Settings,
    mc_folder: &Path,
    downloader: &crate::download::AssetDownloader,
    platforms: &crate::download::ModPlatform,
    req: FinalizeRequest<'_>,
) -> Result<(), String> {
    let FinalizeRequest {
        final_dir,
        name,
        mc_version,
        forge_ver,
        neo_ver,
        fabric_ver,
        mods,
    } = req;
    let loader_type = if !forge_ver.is_empty() {
        "forge"
    } else if !neo_ver.is_empty() {
        "neoforge"
    } else if !fabric_ver.is_empty() {
        "fabric"
    } else {
        ""
    };
    let loader_ver = match loader_type {
        "forge" => forge_ver,
        "neoforge" => neo_ver,
        "fabric" => fabric_ver,
        _ => "",
    };

    if !mc_version.is_empty() {
        let vanilla = extract_vanilla_version(mc_version);
        if let Err(e) = crate::version::install_version(mc_folder, &vanilla, downloader, None).await
        {
            cleanup_on_error(settings, mc_folder, final_dir);
            return Err(format!("Download Minecraft failed: {e}"));
        }
    }

    // modloader
    if !loader_type.is_empty() {
        let vanilla = extract_vanilla_version(mc_version);
        if vanilla.is_empty() {
            cleanup_on_error(settings, mc_folder, final_dir);
            return Err("缺少 MC 版本，无法安装 modloader".into());
        }
        let javas = crate::java::scan_system_java(mc_folder);
        let probe = crate::version::McVersion {
            is_valid: true,
            vanilla_version: crate::version::McVersionNumber::parse(&vanilla),
            ..Default::default()
        };
        let java = crate::java::select_java_for_version(&javas, &probe)
            .or_else(|| javas.first().cloned())
            .ok_or_else(|| {
                cleanup_on_error(settings, mc_folder, final_dir);
                "No Java runtime found for modloader install".to_string()
            });
        let java = match java {
            Ok(j) => j,
            Err(e) => return Err(e),
        };
        if java.path_java.is_empty() {
            cleanup_on_error(settings, mc_folder, final_dir);
            return Err("No Java runtime found for modloader install".into());
        }
        tracing::info!("Installing {loader_type} {loader_ver} on MC {vanilla}...");
        if let Err(e) = crate::installer::install_loader(
            downloader.manager(),
            loader_type,
            mc_folder,
            &vanilla,
            loader_ver,
            &java.path_java,
        )
        .await
        {
            cleanup_on_error(settings, mc_folder, final_dir);
            return Err(format!("Install modloader failed: {e}"));
        }
    }

    for m in mods {
        let save = final_dir.join(&m.save_path);
        if let Some(p) = save.parent() {
            let _ = std::fs::create_dir_all(p);
        }
        if !m.url.is_empty() {
            if downloader
                .manager()
                .download_file(&m.url, &save, None)
                .await
                .is_err()
            {
                cleanup_on_error(settings, mc_folder, final_dir);
                return Err(format!("Mod download failed: {}", m.url));
            }
        } else if !m.cf_mod_id.is_empty() {
            use crate::download::Platform;
            if let Err(e) = platforms
                .download_mod(
                    Platform::CurseForge,
                    &m.cf_mod_id,
                    &m.cf_file_id,
                    &save,
                    None,
                )
                .await
            {
                cleanup_on_error(settings, mc_folder, final_dir);
                return Err(format!("Mod download failed: CF {} ({e})", m.cf_mod_id));
            }
        }
    }

    let version_name =
        resolve_instance_version_name(final_dir, mc_version, loader_type, loader_ver);
    write_setup_ini(final_dir, name, Some(&version_name)).map_err(|e| e.to_string())?;
    finalize_install(settings, mc_folder, final_dir, name);
    Ok(())
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
            zip.start_file(*name, opts).unwrap();
            zip.write_all(data).unwrap();
        }
        zip.finish().unwrap();
        zip_path
    }

    fn temp_settings(tag: &str) -> Settings {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("mlc-pk-{}-{tag}-{n}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut s = Settings::load(&dir.join("MLC.ini"));
        s.set_string("LaunchFolderSelect", &format!("{}/", dir.display()));
        s
    }

    fn mc_of(s: &Settings) -> PathBuf {
        PathBuf::from(
            s.get_string("LaunchFolderSelect")
                .unwrap()
                .trim_end_matches('/')
                .to_string(),
        )
    }

    #[test]
    fn 检测主要类型() {
        assert_eq!(
            detect_pack_type(&temp_zip(
                "cf",
                &[("manifest.json", br#"{"minecraft":{"version":"1.20.1"}}"#)]
            )),
            PackType::CurseForge
        );
        assert_eq!(
            detect_pack_type(&temp_zip(
                "mcbbs",
                &[("manifest.json", br#"{"addons":{}}"#)]
            )),
            PackType::Mcbbs
        );
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
    fn incomplete标记与回滚() {
        let mut s = temp_settings("rollback");
        let mc = mc_of(&s);
        let (final_dir, name) = begin_install(&mut s, &mc, "", "Pack").unwrap();
        assert!(final_dir.is_dir());
        assert!(is_incomplete(&final_dir));
        assert_eq!(name, "Pack");
        fs::write(final_dir.join("half.txt"), b"x").unwrap();
        cleanup_on_error(&mut s, &mc, &final_dir);
        assert!(!final_dir.exists());
        assert!(s.dir_for_display_name("Pack").is_none());
    }

    #[test]
    fn 解析curseforge清单() {
        let dir = std::env::temp_dir().join(format!("mlc-cf-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("manifest.json"),
            r#"{
                "name": "测试包",
                "minecraft": {
                    "version": "1.20.1",
                    "modLoaders": [{"id": "forge-47.2.0", "primary": true}]
                },
                "files": [
                    {"projectID": 238222, "fileID": 4712868, "required": true}
                ],
                "overrides": "overrides"
            }"#,
        )
        .unwrap();
        let (mc, name, forge, neo, fabric, mods) = parse_curseforge_manifest(&dir).unwrap();
        assert_eq!(mc, "1.20.1");
        assert_eq!(name, "测试包");
        assert_eq!(forge, "47.2.0");
        assert!(neo.is_empty() && fabric.is_empty());
        assert_eq!(mods.len(), 1);
        assert_eq!(mods[0].save_path, "mods/238222_4712868.jar");
    }

    #[test]
    fn 解析hmcl与mmc清单() {
        let dir = std::env::temp_dir().join(format!("mlc-hm-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("modpack.json"),
            r#"{"name":"HMCL包","gameVersion":"1.19.2"}"#,
        )
        .unwrap();
        let (mc, name) = parse_hmcl_manifest(&dir).unwrap();
        assert_eq!((mc.as_str(), name.as_str()), ("1.19.2", "HMCL包"));

        fs::write(
            dir.join("mmc-pack.json"),
            r#"{"name":"MMC","components":[
                {"uid":"net.minecraft","version":"1.20.1"},
                {"uid":"net.minecraftforge","version":"47.2.0"}
            ]}"#,
        )
        .unwrap();
        let (mc, _name, forge, _neo, fabric) = parse_multimc_manifest(&dir).unwrap();
        assert_eq!(mc, "1.20.1");
        assert_eq!(forge, "47.2.0");
        assert!(fabric.is_empty());
    }

    #[test]
    fn compressed压缩安装() {
        let mut s = temp_settings("comp-inst");
        let mc = mc_of(&s);
        let z = temp_zip(
            "compinst",
            &[
                ("versions/1.20.1/1.20.1.json", br#"{"id":"1.20.1"}"#),
                ("mods/a.jar", b"jar"),
            ],
        );
        let name = install_compressed(&mut s, &mc, &z, "我的压缩包").unwrap();
        assert_eq!(name, "我的压缩包");
        let dir_name = s.dir_for_display_name(&name).unwrap();
        let inst = mc.join("instances").join(&dir_name);
        assert!(inst.join("mods/a.jar").exists());
        assert!(inst.join("PCL/Setup.ini").exists());
        assert!(!is_incomplete(&inst));
    }

    #[test]
    fn mod包复制到目标实例() {
        let mut s = temp_settings("modinst");
        let mc = mc_of(&s);
        let (dir, name) = begin_install(&mut s, &mc, "目标", "").unwrap();
        finalize_install(&mut s, &mc, &dir, &name);

        let z = temp_zip(
            "modpk",
            &[("sub/a.jar", b"a"), ("b.jar", b"b"), ("readme.txt", b"no")],
        );
        let extract = pack_tmp_root(&mc).join("modpk");
        file::extract_zip(&z, &extract).unwrap();
        let copied = install_mod(&s, &mc, &extract, "目标").unwrap();
        assert_eq!(copied, 2);
        assert!(dir.join("mods/a.jar").exists());
        assert!(install_mod(&s, &mc, &extract, "").is_err());
        assert!(install_mod(&s, &mc, &extract, "不存在").is_err());
    }

    #[test]
    fn 有loader但缺mc版本时回滚() {
        let mut s = temp_settings("loader-fail");
        let mc = mc_of(&s);
        let (final_dir, name) = begin_install(&mut s, &mc, "带Forge的包", "").unwrap();
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let dl =
            crate::download::AssetDownloader::new(crate::download::manager::DownloadManager::new());
        let mp = crate::download::ModPlatform::for_test("", "", "", "");
        let r = rt.block_on(download_and_finalize(
            &mut s,
            &mc,
            &dl,
            &mp,
            FinalizeRequest {
                final_dir: &final_dir,
                name: &name,
                mc_version: "",
                forge_ver: "47.2.0",
                neo_ver: "",
                fabric_ver: "",
                mods: &[],
            },
        ));
        assert!(r.is_err());
        assert!(r.unwrap_err().contains("缺少 MC 版本"));
        assert!(!final_dir.exists());
        assert!(s.dir_for_display_name("带Forge的包").is_none());
    }

    #[test]
    fn 版本号与目录名工具() {
        assert_eq!(extract_vanilla_version("1.21.1-NeoForge_x"), "1.21.1");
        assert!(validate_instance_name("我的包"));
        assert!(!validate_instance_name("a/b"));
        let a = generate_instance_dir();
        assert_eq!(a.len(), 8);
    }
}
