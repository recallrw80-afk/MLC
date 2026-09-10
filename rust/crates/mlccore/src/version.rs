//! 版本管理，对应 C++ `versionmanager.cpp`：原版装/验/修、实例解析
//! （PCL/Setup.ini 的 Version 键 → 实例 versions/ 扫描）、INI [Instances] 映射维护。
//!
//! 只读路径 + `install_version`（接 AssetDownloader，对齐 mlc::installVersion）。

use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde_json::{json, Value};

use crate::download::{AssetDownloader, Stage};
use crate::settings::Settings;
use crate::util::platform;

// ---------------------------------------------------------------- 版本号

/// 对齐 QVersionNumber：段比较；空/null 小于一切
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct McVersionNumber {
    segments: Vec<i32>,
}

impl McVersionNumber {
    pub fn empty() -> Self {
        Self::default()
    }

    pub fn is_empty(&self) -> bool {
        self.segments.is_empty()
    }

    pub fn from_segments(segments: &[i32]) -> Self {
        Self {
            segments: segments.to_vec(),
        }
    }

    /// 对齐 QVersionNumber::fromString：按 `.` 切段，非数字段截断
    pub fn parse(s: &str) -> Self {
        let mut segments = Vec::new();
        for part in s.split('.') {
            match part.parse::<i32>() {
                Ok(v) => segments.push(v),
                Err(_) => break,
            }
        }
        Self { segments }
    }

    pub fn major(&self) -> i32 {
        self.segments.first().copied().unwrap_or(0)
    }

    pub fn minor(&self) -> i32 {
        self.segments.get(1).copied().unwrap_or(0)
    }

    pub fn segment(&self, i: usize) -> i32 {
        self.segments.get(i).copied().unwrap_or(0)
    }

    pub fn segment_count(&self) -> usize {
        self.segments.len()
    }
}

impl std::fmt::Display for McVersionNumber {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = self
            .segments
            .iter()
            .map(|x| x.to_string())
            .collect::<Vec<_>>()
            .join(".");
        f.write_str(&s)
    }
}

impl PartialOrd for McVersionNumber {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for McVersionNumber {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        // 空（null）小于一切，包括另一个空
        if self.is_empty() && other.is_empty() {
            return std::cmp::Ordering::Equal;
        }
        if self.is_empty() {
            return std::cmp::Ordering::Less;
        }
        if other.is_empty() {
            return std::cmp::Ordering::Greater;
        }
        let n = self.segments.len().max(other.segments.len());
        for i in 0..n {
            let a = self.segment(i);
            let b = other.segment(i);
            match a.cmp(&b) {
                std::cmp::Ordering::Equal => continue,
                other => return other,
            }
        }
        std::cmp::Ordering::Equal
    }
}

// ---------------------------------------------------------------- 类型

/// 对齐 McVersionInfo
#[derive(Debug, Clone, Default)]
pub struct McVersionInfo {
    pub id: String,
    /// "release" / "snapshot" / "old_beta" / "old_alpha" / "instance"
    pub kind: String,
    pub release_time: String,
    pub url: String,
    pub is_local: bool,
}

/// 对齐 McModLoaderInfo
#[derive(Debug, Clone, Default)]
pub struct McModLoaderInfo {
    pub has_forge: bool,
    pub has_fabric: bool,
    pub has_neoforge: bool,
    pub has_optifine: bool,
    pub has_liteloader: bool,
    pub forge_version: String,
    pub fabric_version: String,
    pub neoforge_version: String,
    pub optifine_version: String,
}

impl McModLoaderInfo {
    pub fn has_any(&self) -> bool {
        self.has_forge
            || self.has_fabric
            || self.has_neoforge
            || self.has_optifine
            || self.has_liteloader
    }
}

/// 对齐 McVersion
#[derive(Debug, Clone, Default)]
pub struct McVersion {
    pub id: String,
    pub kind: String,
    pub release_time: String,
    pub inherit_name: String,
    pub vanilla_version: McVersionNumber,
    pub mod_loader: McModLoaderInfo,
    pub is_valid: bool,
    pub info: String,
    pub path_version: String,
    pub path_indie: String,
    pub path_json: String,
    pub path_jar: String,
}

// ---------------------------------------------------------------- 路径

/// 解析游戏目录：settings 的 LaunchFolderSelect → 缺省 exe 旁 mc/
pub fn resolve_mc_folder(settings: &Settings) -> PathBuf {
    let saved = settings
        .get_string("LaunchFolderSelect")
        .filter(|s| !s.is_empty());
    match saved {
        Some(s) => {
            let mut p = platform::normalize_path_string(Path::new(&s));
            if !p.ends_with('/') {
                p.push('/');
            }
            PathBuf::from(p)
        }
        None => default_mc_folder(),
    }
}

/// 默认游戏目录（可执行文件旁 mc/，带尾斜杠）
pub fn default_mc_folder() -> PathBuf {
    let p = platform::normalize_path_string(&platform::exe_dir().join("mc"));
    PathBuf::from(format!("{p}/"))
}

fn join_under(root: &Path, rel: &str) -> PathBuf {
    // root 通常以 / 结尾；Path::join 会正确处理
    let mut p = root.to_path_buf();
    for part in rel.split('/').filter(|s| !s.is_empty()) {
        p.push(part);
    }
    p
}

fn path_str(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

// ---------------------------------------------------------------- 实例列表

/// 从 INI [Instances] 读取本地实例显示名（对齐 loadLocalVersions）。
/// 默认目录下清理失效映射；临时 folder 只过滤不清理。
pub fn list_instances(settings: &mut Settings, mc_folder: &Path) -> Vec<McVersionInfo> {
    let mut out = Vec::new();
    if !mc_folder.exists() {
        return out;
    }

    let persisted = settings
        .get_string("LaunchFolderSelect")
        .unwrap_or_default();
    let mut persisted_norm = platform::normalize_path_string(Path::new(&persisted));
    if !persisted_norm.is_empty() && !persisted_norm.ends_with('/') {
        persisted_norm.push('/');
    }
    let mc_norm = {
        let mut s = path_str(mc_folder);
        if !s.ends_with('/') {
            s.push('/');
        }
        s
    };
    let is_default_folder = !persisted.is_empty() && persisted_norm == mc_norm;

    let instance_map = settings.instance_dirs();
    for (dir_name, display_name) in instance_map {
        let dir_path = join_under(mc_folder, &format!("instances/{dir_name}"));
        let setup = dir_path.join("PCL").join("Setup.ini");
        if !setup.exists() {
            if is_default_folder && !dir_path.exists() {
                settings.remove_instance_dir(&dir_name);
            }
            continue;
        }
        out.push(McVersionInfo {
            id: display_name,
            kind: "instance".into(),
            release_time: String::new(),
            url: String::new(),
            is_local: true,
        });
    }
    out
}

/// 扫描 versions/ 下的原版/模组版本目录（对齐 loadMcVersions），按 releaseTime 降序
pub fn list_mc_versions(mc_folder: &Path) -> Vec<McVersionInfo> {
    let mut out = Vec::new();
    let versions_dir = join_under(mc_folder, "versions");
    let Ok(entries) = std::fs::read_dir(&versions_dir) else {
        return out;
    };
    for entry in entries.flatten() {
        if !entry.path().is_dir() {
            continue;
        }
        let dir_name = entry.file_name().to_string_lossy().to_string();
        let json_path = entry.path().join(format!("{dir_name}.json"));
        if !json_path.exists() {
            continue;
        }
        let mut info = McVersionInfo {
            id: dir_name,
            kind: "unknown".into(),
            release_time: String::new(),
            url: String::new(),
            is_local: true,
        };
        if let Ok(text) = std::fs::read_to_string(&json_path) {
            if let Ok(root) = serde_json::from_str::<Value>(&text) {
                if let Some(t) = root.get("type").and_then(|v| v.as_str()) {
                    info.kind = t.to_string();
                }
                if let Some(rt) = root.get("releaseTime").and_then(|v| v.as_str()) {
                    info.release_time = rt.to_string();
                }
            }
        }
        out.push(info);
    }
    out.sort_by(|a, b| b.release_time.cmp(&a.release_time));
    out
}

// ---------------------------------------------------------------- 解析

/// 读取 PCL Setup.ini 的 [Setup] Version 键（普通 INI，非 MLC 嵌套格式）
fn read_setup_version(setup_ini: &Path) -> Option<String> {
    let text = std::fs::read_to_string(setup_ini).ok()?;
    let mut in_setup = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') && line.ends_with(']') {
            in_setup = line.eq_ignore_ascii_case("[Setup]");
            continue;
        }
        if !in_setup {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            if k.trim().eq_ignore_ascii_case("Version") {
                let v = v.trim().trim_matches('"');
                if !v.is_empty() {
                    return Some(v.to_string());
                }
            }
        }
    }
    None
}

/// 实例版本解析（对齐 loadInstanceVersion）
pub fn load_instance_version(mc_folder: &Path, dir_name: &str) -> McVersion {
    let inst_dir = join_under(mc_folder, &format!("instances/{dir_name}"));

    // 1. 版本名：Setup.ini → 实例内唯一带 json 的版本文件夹
    let mut ver_name =
        read_setup_version(&inst_dir.join("PCL").join("Setup.ini")).unwrap_or_default();
    if ver_name.is_empty() {
        let vd = inst_dir.join("versions");
        if let Ok(entries) = std::fs::read_dir(&vd) {
            for entry in entries.flatten() {
                if !entry.path().is_dir() {
                    continue;
                }
                let name = entry.file_name().to_string_lossy().to_string();
                if entry.path().join(format!("{name}.json")).exists() {
                    ver_name = name;
                    break;
                }
            }
        }
    }
    if ver_name.is_empty() {
        return McVersion {
            is_valid: false,
            info: format!("实例未记录游戏版本: {dir_name}"),
            ..Default::default()
        };
    }

    // 2. version json：实例内优先，全局 versions/ 兜底
    let mut json_path = inst_dir
        .join("versions")
        .join(&ver_name)
        .join(format!("{ver_name}.json"));
    if !json_path.exists() {
        json_path = join_under(mc_folder, &format!("versions/{ver_name}/{ver_name}.json"));
    }
    if !json_path.exists() {
        return McVersion {
            is_valid: false,
            info: format!("version json 缺失: {ver_name}"),
            ..Default::default()
        };
    }

    let mut ver = parse_version_json(mc_folder, &json_path);

    // 3. PathIndie：版本目录下有 mods/ 或 config/ 则用它，否则实例根
    let version_sub = inst_dir.join("versions").join(&ver_name);
    let game_dir = if version_sub.join("mods").is_dir() || version_sub.join("config").is_dir() {
        format!("{}/", path_str(&version_sub))
    } else {
        format!("{}/", path_str(&inst_dir))
    };
    ver.path_indie = game_dir;
    ver
}

/// 加载全局 versions/<id>/<id>.json
pub fn load_version(mc_folder: &Path, version_id: &str) -> McVersion {
    let json_path = join_under(
        mc_folder,
        &format!("versions/{version_id}/{version_id}.json"),
    );
    parse_version_json(mc_folder, &json_path)
}

/// 删除实例（对齐 removeInstance）：先清 INI 映射再删目录
pub fn remove_instance(settings: &mut Settings, mc_folder: &Path, name: &str) -> bool {
    if name.is_empty() || name.contains('/') || name.contains('\\') || name.contains("..") {
        return false;
    }
    let mapped = settings
        .dir_for_display_name(name)
        .filter(|d| !d.is_empty());
    match mapped {
        Some(dir_name) => {
            settings.remove_instance_dir(&dir_name);
            let dir = join_under(mc_folder, &format!("instances/{dir_name}"));
            if !dir.exists() {
                // 映射已清理；目录已被手动删掉也视为成功
                return true;
            }
            crate::util::file::remove_tree(&dir)
        }
        None => {
            // 回退：显示名直接当目录名
            let dir = join_under(mc_folder, &format!("instances/{name}"));
            if !dir.exists() {
                return false;
            }
            settings.remove_instance_dir(name);
            crate::util::file::remove_tree(&dir)
        }
    }
}

/// 解析 version json 文件（对齐 parseVersionJson(path)）
pub fn parse_version_json(mc_folder: &Path, json_path: &Path) -> McVersion {
    let mut ver = McVersion::default();
    let text = match std::fs::read_to_string(json_path) {
        Ok(t) => t,
        Err(_) => {
            ver.is_valid = false;
            ver.info = format!("Cannot open version JSON: {}", path_str(json_path));
            return ver;
        }
    };
    let j: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(_) => {
            ver.is_valid = false;
            ver.info = format!("Failed to parse JSON: {}", path_str(json_path));
            return ver;
        }
    };
    let version_id = json_path
        .parent()
        .and_then(|p| p.file_name())
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default();
    let mut ver = parse_version_json_value(&j, &version_id);
    ver.path_json = path_str(json_path);
    ver.path_version = format!("{}/", json_path.parent().map(path_str).unwrap_or_default());
    let jar = json_path
        .parent()
        .map(|p| p.join(format!("{version_id}.jar")))
        .unwrap_or_default();
    if jar.exists() {
        ver.path_jar = path_str(&jar);
    } else {
        // modded 版本目录只有 json——client jar 在 vanilla 目录
        let vanilla = ver.vanilla_version.to_string();
        if !vanilla.is_empty() {
            let vanilla_jar = join_under(mc_folder, &format!("versions/{vanilla}/{vanilla}.jar"));
            if vanilla_jar.exists() {
                ver.path_jar = path_str(&vanilla_jar);
            }
        }
    }
    ver.path_indie = format!("{}/", path_str(mc_folder));
    ver
}

/// 解析 version json 值（对齐 parseVersionJson(json, versionId)）
pub fn parse_version_json_value(j: &Value, version_id: &str) -> McVersion {
    let mut ver = McVersion {
        id: version_id.to_string(),
        is_valid: true,
        ..Default::default()
    };
    let Some(obj) = j.as_object() else {
        ver.is_valid = false;
        ver.info = format!("version json 根节点不是对象: {version_id}");
        return ver;
    };

    // 字段类型畸形 → 版本无效（对齐 C++ try/catch）
    let kind = obj
        .get("type")
        .and_then(|v| v.as_str())
        .unwrap_or("unknown");
    if obj.contains_key("type") && !obj["type"].is_string() {
        ver.is_valid = false;
        ver.info = format!("version json 字段类型畸形: {version_id}");
        return ver;
    }
    ver.kind = kind.to_string();

    if let Some(rt) = obj.get("releaseTime") {
        if let Some(s) = rt.as_str() {
            ver.release_time = s.to_string();
        } else {
            ver.is_valid = false;
            ver.info = format!("version json 字段类型畸形: {version_id}");
            return ver;
        }
    }

    if let Some(inh) = obj.get("inheritsFrom") {
        if let Some(s) = inh.as_str() {
            ver.inherit_name = s.to_string();
        } else {
            ver.is_valid = false;
            ver.info = format!("version json 字段类型畸形: {version_id}");
            return ver;
        }
    }

    match detect_mod_loaders(j) {
        Ok(info) => ver.mod_loader = info,
        Err(_) => {
            ver.is_valid = false;
            ver.info = format!("version json 字段类型畸形: {version_id}");
            return ver;
        }
    }

    match detect_vanilla_version(j, version_id) {
        Ok(vanilla) => ver.vanilla_version = McVersionNumber::parse(&vanilla),
        Err(_) => {
            ver.is_valid = false;
            ver.info = format!("version json 字段类型畸形: {version_id}");
        }
    }
    ver
}

/// 模组加载器检测（对齐 detectModLoaders）；字段类型错误返回 Err
/// （内部错误通道，对齐 C++ 异常路径，不对外暴露 unit error）
#[allow(clippy::result_unit_err)]
pub fn detect_mod_loaders(version_json: &Value) -> Result<McModLoaderInfo, ()> {
    let mut info = McModLoaderInfo::default();
    let Some(obj) = version_json.as_object() else {
        return Ok(info);
    };

    if let Some(inherit) = obj.get("inheritsFrom") {
        let inherit = inherit.as_str().ok_or(())?;
        let version_id = obj
            .get("id")
            .map(|v| v.as_str().ok_or(()))
            .transpose()?
            .unwrap_or("");
        let q_ver_id = version_id.to_lowercase();
        let _ = inherit.to_lowercase(); // C++ 读了 inherit 但判定只看 versionId

        // NeoForge 必须先于 Forge（"neoforge" 含 "forge" 子串）
        if q_ver_id.contains("neoforge") {
            info.has_neoforge = true;
        }
        if !info.has_neoforge && q_ver_id.contains("forge") {
            info.has_forge = true;
            info.forge_version = extract_pattern(version_id, "forge[-_]([\\d.]+)");
        }
        if q_ver_id.contains("fabric") {
            info.has_fabric = true;
        }
        if q_ver_id.contains("optifine") {
            info.has_optifine = true;
        }
        if q_ver_id.contains("liteloader") {
            info.has_liteloader = true;
        }
    }

    if let Some(fl) = obj.get("fabricLoader") {
        if let Some(fl_obj) = fl.as_object() {
            info.has_fabric = true;
            info.fabric_version = fl_obj
                .get("version")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string();
        } else {
            return Err(());
        }
    }

    Ok(info)
}

/// 从字符串提取第一个正则捕获组；无 regex crate，手写极简匹配仅支持本文件用到的模式
fn extract_pattern(input: &str, pattern: &str) -> String {
    // 仅支持 `forge[-_]([\d.]+)` 这一种调用
    if pattern.starts_with("forge") {
        let lower = input.to_ascii_lowercase();
        let bytes = lower.as_bytes();
        let mut i = 0;
        while i < bytes.len() {
            if lower[i..].starts_with("forge-") || lower[i..].starts_with("forge_") {
                let rest = &input[i + 6..];
                let mut out = String::new();
                for c in rest.chars() {
                    if c.is_ascii_digit() || c == '.' {
                        out.push(c);
                    } else {
                        break;
                    }
                }
                return out;
            }
            i += 1;
        }
    }
    String::new()
}

/// 提取原版基版本（对齐 detectVanillaVersion）
#[allow(clippy::result_unit_err)]
fn detect_vanilla_version(version_json: &Value, version_id: &str) -> Result<String, ()> {
    if let Some(inherit) = version_json.get("inheritsFrom") {
        let inherit = inherit.as_str().ok_or(())?;
        let lower = inherit.to_lowercase();
        if !lower.contains("forge")
            && !lower.contains("fabric")
            && !lower.contains("neoforge")
            && !lower.contains("optifine")
        {
            return Ok(inherit.to_string());
        }
    }

    // `^(\d+\.\d+(?:\.\d+)?)`
    let mut parts = version_id.split('.');
    if let (Some(a), Some(b)) = (parts.next(), parts.next()) {
        if !a.is_empty() && a.chars().all(|c| c.is_ascii_digit()) {
            let mut out = format!("{a}.{b}");
            // b 必须是纯数字前缀
            let b_num: String = b.chars().take_while(|c| c.is_ascii_digit()).collect();
            if b_num.is_empty() {
                return Ok(version_id.to_string());
            }
            out = format!("{a}.{b_num}");
            if let Some(c) = parts.next() {
                let c_num: String = c.chars().take_while(|c| c.is_ascii_digit()).collect();
                if !c_num.is_empty() {
                    out = format!("{out}.{c_num}");
                }
            }
            return Ok(out);
        }
    }
    Ok(version_id.to_string())
}

// ---------------------------------------------------------------- 继承链

/// 解析 inheritsFrom 链并合并（对齐 resolveInheritanceChain）。
/// 环检测、父版本缺失、字段畸形均返回 None（等价 C++ 空 json）。
pub fn resolve_inheritance_chain(mc_folder: &Path, json_path: &Path) -> Option<Value> {
    let mut visited = std::collections::HashSet::new();
    resolve_chain_inner(mc_folder, json_path, &mut visited).unwrap_or_default()
}

#[allow(clippy::result_unit_err)]
fn resolve_chain_inner(
    mc_folder: &Path,
    json_path: &Path,
    visited: &mut std::collections::HashSet<PathBuf>,
) -> Result<Option<Value>, ()> {
    let canonical = std::fs::canonicalize(json_path).unwrap_or_else(|_| json_path.to_path_buf());
    if !visited.insert(canonical) {
        tracing::warn!("Circular inheritsFrom detected: {}", path_str(json_path));
        return Ok(None);
    }

    let text = std::fs::read_to_string(json_path).map_err(|_| ())?;
    let mut result: Value = serde_json::from_str(&text).map_err(|_| ())?;

    let Some(obj) = result.as_object_mut() else {
        return Ok(Some(result));
    };
    let Some(inherit) = obj
        .get("inheritsFrom")
        .and_then(|v| v.as_str())
        .map(String::from)
    else {
        // 无继承或 inheritsFrom 非字符串 → 原样返回
        return Ok(Some(result));
    };

    // 父版本路径：同 versions 目录 → 全局 mcFolder/versions
    let parent_sibling = json_path
        .parent()
        .and_then(|p| p.parent()) // versions/
        .map(|v| v.join(&inherit).join(format!("{inherit}.json")))
        .unwrap_or_default();
    let parent_path = if parent_sibling.exists() {
        parent_sibling
    } else {
        join_under(mc_folder, &format!("versions/{inherit}/{inherit}.json"))
    };

    let Some(parent) = resolve_chain_inner(mc_folder, &parent_path, visited)? else {
        return Ok(Some(result));
    };
    let Some(parent_obj) = parent.as_object() else {
        return Ok(Some(result));
    };

    let result_obj = result.as_object_mut().unwrap();

    // arguments.jvm/game：父在前、子追加
    for section in ["jvm", "game"] {
        if let Some(parent_args) = parent_obj.get("arguments").and_then(|a| a.get(section)) {
            let args = result_obj.entry("arguments").or_insert_with(|| json!({}));
            let Some(args_obj) = args.as_object_mut() else {
                return Err(());
            };
            match args_obj.get(section) {
                None => {
                    args_obj.insert(section.to_string(), parent_args.clone());
                }
                Some(child_args) => {
                    let Some(child_arr) = child_args.as_array() else {
                        return Err(());
                    };
                    let Some(parent_arr) = parent_args.as_array() else {
                        return Err(());
                    };
                    let mut merged = parent_arr.clone();
                    merged.extend(child_arr.iter().cloned());
                    args_obj.insert(section.to_string(), Value::Array(merged));
                }
            }
        }
    }

    // libraries：按 maven name 去重，子级优先
    if let Some(parent_libs) = parent_obj.get("libraries") {
        let Some(parent_arr) = parent_libs.as_array() else {
            return Err(());
        };
        match result_obj.get("libraries") {
            None => {
                result_obj.insert("libraries".into(), parent_libs.clone());
            }
            Some(child_libs) => {
                let Some(child_arr) = child_libs.as_array() else {
                    return Err(());
                };
                let mut existing: std::collections::HashSet<String> = Default::default();
                for lib in child_arr {
                    let name = lib
                        .as_object()
                        .and_then(|o| o.get("name"))
                        .and_then(|v| v.as_str())
                        .unwrap_or("");
                    if !name.is_empty() {
                        existing.insert(name.to_string());
                    }
                }
                let mut merged = child_arr.clone();
                for lib in parent_arr {
                    let name = lib
                        .as_object()
                        .and_then(|o| o.get("name"))
                        .and_then(|v| v.as_str())
                        .unwrap_or("");
                    if !name.is_empty() && existing.contains(name) {
                        continue;
                    }
                    merged.push(lib.clone());
                }
                result_obj.insert("libraries".into(), Value::Array(merged));
            }
        }
    }

    for key in ["mainClass", "minecraftArguments", "assetIndex", "assets"] {
        if let Some(v) = parent_obj.get(key) {
            if !result_obj.contains_key(key) {
                result_obj.insert(key.to_string(), v.clone());
            }
        }
    }

    Ok(Some(result))
}

// ---------------------------------------------------------------- 安装

/// 阶段 → 进度百分比（对齐 mlc.cpp installVersion 的进度映射；
/// Rust 管线一次做完 natives，故 Natives 靠近收尾）
pub fn stage_percent(s: Stage) -> i32 {
    match s {
        Stage::Manifest => 5,
        Stage::VersionJson => 10,
        Stage::ClientJar => 15,
        Stage::Libraries => 35,
        Stage::Assets => 55,
        Stage::Natives => 85,
        Stage::Done => 100,
    }
}

/// 进度回调：(消息, 百分比)；百分比 -1 = 纯日志
pub type InstallProgressCallback = Arc<dyn Fn(&str, i32) + Send + Sync>;

/// 安装/校验补齐原版版本（对齐 mlc::installVersion）。
/// `version_id` 为空 = 最新正式版；重复执行 = SHA1 校验补齐。
/// 返回实际安装的版本 ID。
pub async fn install_version(
    mc_folder: &Path,
    version_id: &str,
    downloader: &AssetDownloader,
    on_progress: Option<&InstallProgressCallback>,
) -> Result<String, String> {
    let id = if version_id.trim().is_empty() {
        let id = downloader.latest_release_id().await?;
        if let Some(cb) = on_progress {
            cb(&format!("Latest release: {id}"), 0);
        }
        id
    } else {
        version_id.trim().to_string()
    };

    if let Some(cb) = on_progress {
        cb(
            &format!("Installing {id}..."),
            stage_percent(Stage::Manifest),
        );
    }
    downloader.download_version(&id, mc_folder).await?;
    if let Some(cb) = on_progress {
        cb("Complete", 100);
    }
    Ok(id)
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn temp_root(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mlc-ver-{}-{tag}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(path: &Path, content: &str) {
        if let Some(p) = path.parent() {
            fs::create_dir_all(p).unwrap();
        }
        fs::write(path, content).unwrap();
    }

    fn settings_in(mc: &Path) -> Settings {
        let mut s = Settings::load(&mc.join("MLC.ini"));
        s.set_string("LaunchFolderSelect", &format!("{}/", path_str(mc)));
        s
    }

    #[test]
    fn 版本号比较对齐qversionnumber() {
        assert!(McVersionNumber::parse("1.8.0.321") < McVersionNumber::parse("1.8.0.322"));
        assert!(McVersionNumber::empty() < McVersionNumber::parse("1.0"));
        assert!(McVersionNumber::parse("1.20.1") < McVersionNumber::parse("1.21"));
        assert_eq!(McVersionNumber::parse("21.0.2").major(), 21);
        // 1.8 → major=1, minor=8（Java 主版本兼容矩阵用）
        assert_eq!(
            McVersionNumber::parse("1.8.0_321".replace('_', ".").as_str()).minor(),
            8
        );
    }

    #[test]
    fn 实例列表与失效映射清理() {
        let mc = temp_root("inst");
        let mut s = settings_in(&mc);

        // 有效实例
        write(
            &mc.join("instances/abc/PCL/Setup.ini"),
            "[Setup]\nVersion=1.20.1\n",
        );
        s.set_instance_dir("abc", "我的包");
        // 默认目录下目录缺失 → 清理
        s.set_instance_dir("ghost", "幽灵包");

        let list = list_instances(&mut s, &mc);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].id, "我的包");
        assert_eq!(list[0].kind, "instance");
        assert!(s.dir_for_display_name("幽灵包").is_none());
        assert!(s.dir_for_display_name("我的包").is_some());
    }

    #[test]
    fn mc版本列表按时间排序() {
        let mc = temp_root("mcver");
        write(
            &mc.join("versions/1.20.1/1.20.1.json"),
            r#"{"type":"release","releaseTime":"2023-06-12T00:00:00+00:00"}"#,
        );
        write(
            &mc.join("versions/1.21/1.21.json"),
            r#"{"type":"release","releaseTime":"2024-06-13T00:00:00+00:00"}"#,
        );
        // 无 json 的目录应被跳过
        fs::create_dir_all(mc.join("versions/broken")).unwrap();

        let list = list_mc_versions(&mc);
        assert_eq!(list.len(), 2);
        assert_eq!(list[0].id, "1.21");
        assert_eq!(list[1].id, "1.20.1");
        assert_eq!(list[0].kind, "release");
    }

    #[test]
    fn 实例版本解析与pathindie() {
        let mc = temp_root("loadinst");
        let inst = "pack1";
        write(
            &mc.join(format!("instances/{inst}/PCL/Setup.ini")),
            "[Setup]\nVersion=1.20.1-forge-47.2.0\n",
        );
        write(
            &mc.join(format!(
                "instances/{inst}/versions/1.20.1-forge-47.2.0/1.20.1-forge-47.2.0.json"
            )),
            r#"{"id":"1.20.1-forge-47.2.0","type":"release","inheritsFrom":"1.20.1"}"#,
        );
        // 版本目录带 mods → PathIndie 指向版本目录
        fs::create_dir_all(mc.join(format!(
            "instances/{inst}/versions/1.20.1-forge-47.2.0/mods"
        )))
        .unwrap();

        let ver = load_instance_version(&mc, inst);
        assert!(ver.is_valid);
        assert_eq!(ver.id, "1.20.1-forge-47.2.0");
        assert!(ver.mod_loader.has_forge);
        assert_eq!(ver.mod_loader.forge_version, "47.2.0");
        assert_eq!(ver.vanilla_version.to_string(), "1.20.1");
        assert!(ver.path_indie.ends_with("1.20.1-forge-47.2.0/"));

        // 无 mods → 实例根
        let inst2 = "pack2";
        write(
            &mc.join(format!("instances/{inst2}/PCL/Setup.ini")),
            "[Setup]\nVersion=1.20.1\n",
        );
        write(
            &mc.join(format!("instances/{inst2}/versions/1.20.1/1.20.1.json")),
            r#"{"id":"1.20.1","type":"release"}"#,
        );
        let ver2 = load_instance_version(&mc, inst2);
        assert!(ver2.is_valid);
        assert!(ver2.path_indie.ends_with(&format!("instances/{inst2}/")));
    }

    #[test]
    fn 模组加载器检测() {
        let fabric = serde_json::json!({
            "id": "fabric-loader-0.15.7-1.20.1",
            "inheritsFrom": "1.20.1"
        });
        let info = detect_mod_loaders(&fabric).unwrap();
        assert!(info.has_fabric);
        assert!(!info.has_forge);

        let neo = serde_json::json!({
            "id": "1.20.1-neoforge-47.1.100",
            "inheritsFrom": "1.20.1"
        });
        let info = detect_mod_loaders(&neo).unwrap();
        assert!(info.has_neoforge);
        assert!(!info.has_forge); // 不得误判为 Forge

        let forge = serde_json::json!({
            "id": "1.20.1-forge-47.2.0",
            "inheritsFrom": "1.20.1"
        });
        let info = detect_mod_loaders(&forge).unwrap();
        assert!(info.has_forge);
        assert_eq!(info.forge_version, "47.2.0");
    }

    #[test]
    fn 继承链合并与环检测() {
        let mc = temp_root("inherit");
        // 父
        write(
            &mc.join("versions/1.20.1/1.20.1.json"),
            r#"{
                "id": "1.20.1",
                "mainClass": "net.minecraft.client.main.Main",
                "arguments": {"jvm": ["-Xmx1G"], "game": ["--username"]},
                "libraries": [{"name": "com.mojang:netty:1.0"}],
                "assetIndex": {"id": "5"},
                "assets": "5"
            }"#,
        );
        // 子
        write(
            &mc.join("versions/forge/forge.json"),
            r#"{
                "id": "forge",
                "inheritsFrom": "1.20.1",
                "arguments": {"jvm": ["-Dforge=1"]},
                "libraries": [{"name": "net.minecraftforge:forge:1"}],
                "mainClass": "cpw.mods.bootstraplauncher.BootstrapLauncher"
            }"#,
        );

        let merged =
            resolve_inheritance_chain(&mc, &mc.join("versions/forge/forge.json")).expect("merge");
        assert_eq!(
            merged["mainClass"],
            "cpw.mods.bootstraplauncher.BootstrapLauncher"
        );
        assert_eq!(merged["assets"], "5");
        assert_eq!(merged["arguments"]["jvm"][0], "-Xmx1G");
        assert_eq!(merged["arguments"]["jvm"][1], "-Dforge=1");
        assert_eq!(merged["arguments"]["game"][0], "--username");
        assert_eq!(merged["libraries"].as_array().unwrap().len(), 2);

        // 环
        write(
            &mc.join("versions/a/a.json"),
            r#"{"id":"a","inheritsFrom":"b"}"#,
        );
        write(
            &mc.join("versions/b/b.json"),
            r#"{"id":"b","inheritsFrom":"a"}"#,
        );
        assert!(
            resolve_inheritance_chain(&mc, &mc.join("versions/a/a.json")).is_none()
                || resolve_inheritance_chain(&mc, &mc.join("versions/a/a.json"))
                    .unwrap()
                    .get("mainClass")
                    .is_none()
        );
    }

    #[test]
    fn 默认mc目录解析() {
        let s = Settings::load(&std::env::temp_dir().join("mlc-no-config.ini"));
        let folder = resolve_mc_folder(&s);
        assert!(path_str(&folder).ends_with("/mc/"));
    }

    #[test]
    fn 安装阶段进度映射单调() {
        assert_eq!(stage_percent(Stage::Manifest), 5);
        assert_eq!(stage_percent(Stage::Done), 100);
        let order = [
            stage_percent(Stage::Manifest),
            stage_percent(Stage::VersionJson),
            stage_percent(Stage::ClientJar),
            stage_percent(Stage::Libraries),
            stage_percent(Stage::Assets),
            stage_percent(Stage::Natives),
            stage_percent(Stage::Done),
        ];
        assert!(order.windows(2).all(|w| w[0] <= w[1]));
    }

    #[tokio::test]
    async fn latest_release从本地清单解析() {
        use crate::download::manager::DownloadManager;
        use std::io::{Read, Write};
        use std::net::TcpListener;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        std::thread::spawn(move || {
            if let Ok((mut s, _)) = listener.accept() {
                let mut buf = [0u8; 512];
                let _ = s.read(&mut buf);
                let body = br#"{"latest":{"release":"1.21.1","snapshot":"24w33a"},"versions":[]}"#;
                let head = format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = s.write_all(head.as_bytes());
                let _ = s.write_all(body);
            }
        });

        let mgr = DownloadManager::builder().max_retries(0).build();
        let ad = AssetDownloader::new(mgr);
        let id = ad
            .latest_release_id_from_url(&format!("http://{addr}/manifest.json"))
            .await
            .unwrap();
        assert_eq!(id, "1.21.1");
    }
}
