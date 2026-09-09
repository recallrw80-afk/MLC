//! 配置读写，对应 C++ `settings.cpp` / `settings.h`。
//!
//! 磁盘格式见 docs/rust-inventory.md 表 3：可执行文件旁 MLC.ini，
//! QSettings IniFormat。键路径以 `MLC/` 为根（对应 C++ initialize 里的
//! `beginGroup("MLC")`），`Profile/<uuid>/<字段>`、`Instances/<随机目录名>`、
//! `Authlib/AccessToken` 等子路径在文件里都落在 [MLC] 节内（以 `\` 嵌套键），
//! 编解码由 [`crate::util::ini`] 负责（逐规则镜像 Qt 6.11.1）。
//!
//! 兼容性约定：
//! - 完整键在内存中保持唯一、更新原位、新增追加（对齐 QSettings 的
//!   originalKeys 位置语义），序列化输出因此与 C++ 版结构一致
//! - 键不存在/值为 null（@Invalid()）→ get 返回默认值；存在但不可解析 →
//!   int 返回 0、bool 返回 false（对齐 QVariant::toInt/toBool）
//! - 加密键经 [`crate::util::crypto`]（DES + `"MLCLiunx"`，兼容负载勿改）

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use crate::util::crypto;
use crate::util::ini;

/// 配置存储。键为完整路径（`MLC/...`），值为未转义字符串；保持插入序。
pub struct Settings {
    path: PathBuf,
    entries: Vec<(String, String)>,
}

impl Settings {
    /// 从 INI 文件加载。文件缺失或解析失败 → 空表（QSettings 状态异常时
    /// get 全部落默认值，行为等价）。
    pub fn load(path: &Path) -> Self {
        let entries = std::fs::read_to_string(path)
            .map(|text| {
                let (entries, _format_error) = ini::parse(&text);
                // @Invalid()（null）视为不存在，随写回从文件中消失——
                // MLC 的 C++ 版从不写 @Invalid()，此分支仅兼容手工文件
                entries
                    .into_iter()
                    .filter_map(|(k, v)| v.map(|v| (k, v)))
                    .collect()
            })
            .unwrap_or_default();
        Self {
            path: path.to_path_buf(),
            entries,
        }
    }

    /// 写回文件（对应每次 set 后的 `sync()`）
    pub fn save(&self) {
        let text = ini::serialize(&self.entries);
        if let Some(dir) = self.path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let _ = std::fs::write(&self.path, text);
    }

    fn index_of(&self, full_key: &str) -> Option<usize> {
        self.entries.iter().position(|(k, _)| k == full_key)
    }

    fn set_full(&mut self, full_key: &str, value: String) {
        match self.index_of(full_key) {
            Some(i) => self.entries[i].1 = value,
            None => self.entries.push((full_key.to_string(), value)),
        }
        self.save();
    }

    fn get_full(&self, full_key: &str) -> Option<&String> {
        self.index_of(full_key).map(|i| &self.entries[i].1)
    }

    /// 删除键或子树（对应 QSettings::remove：`Profile/<uuid>` 形式删整组）
    pub fn remove_key(&mut self, key: &str) {
        let full = format!("MLC/{key}");
        let prefix = format!("{full}/");
        self.entries
            .retain(|(k, _)| k != &full && !k.starts_with(&prefix));
        self.save();
    }

    // ---- 基础键值（key 相对 MLC 组，对齐 C++ 调用方视角） ----

    pub fn get_string(&self, key: &str) -> Option<String> {
        self.get_full(&format!("MLC/{key}")).cloned()
    }

    pub fn set_string(&mut self, key: &str, value: &str) {
        self.set_full(&format!("MLC/{key}"), value.to_string());
    }

    /// 缺失 → default；存在但非数字 → 0（对齐 QVariant::toInt）
    pub fn get_int(&self, key: &str, default: i32) -> i32 {
        match self.get_string(key) {
            None => default,
            Some(s) => s.trim().parse::<i32>().unwrap_or(0),
        }
    }

    pub fn set_int(&mut self, key: &str, value: i32) {
        self.set_full(&format!("MLC/{key}"), value.to_string());
    }

    /// "true"/"1"/非零数字 → true，其余 → false（对齐 QVariant::toBool）
    pub fn get_bool(&self, key: &str, default: bool) -> bool {
        match self.get_string(key) {
            None => default,
            Some(s) => {
                let s = s.trim();
                s.eq_ignore_ascii_case("true")
                    || s == "1"
                    || s.parse::<i32>().map(|v| v != 0).unwrap_or(false)
            }
        }
    }

    pub fn set_bool(&mut self, key: &str, value: bool) {
        self.set_full(
            &format!("MLC/{key}"),
            if value { "true" } else { "false" }.into(),
        );
    }

    // ---- 加密键值 ----

    /// 缺失或空 → 空串（对齐 getEncrypted(key, "")）
    pub fn get_encrypted(&self, key: &str) -> String {
        match self.get_string(key) {
            Some(raw) if !raw.is_empty() => crypto::pcl_decrypt(&raw),
            _ => String::new(),
        }
    }

    pub fn set_encrypted(&mut self, key: &str, value: &str) {
        let cipher = crypto::pcl_encrypt(value);
        self.set_string(key, &cipher);
    }

    // ---- 玩家档案（Profile/<uuid>/<字段>） ----

    /// 全部玩家 UUID（对应 childGroups，QMap 有序 → 按字典序）
    pub fn player_profiles(&self) -> Vec<String> {
        let mut uuids = std::collections::BTreeSet::new();
        for (k, _) in &self.entries {
            if let Some(rest) = k.strip_prefix("MLC/Profile/") {
                if let Some((uuid, _)) = rest.split_once('/') {
                    if !uuid.is_empty() {
                        uuids.insert(uuid.to_string());
                    }
                }
            }
        }
        uuids.into_iter().collect()
    }

    pub fn get_profile(&self, uuid: &str, key: &str) -> Option<String> {
        if uuid.is_empty() {
            return None;
        }
        self.get_string(&format!("Profile/{uuid}/{key}"))
    }

    pub fn set_profile(&mut self, uuid: &str, key: &str, value: &str) {
        if uuid.is_empty() {
            return;
        }
        self.set_string(&format!("Profile/{uuid}/{key}"), value);
    }

    pub fn remove_profile(&mut self, uuid: &str) {
        if uuid.is_empty() {
            return;
        }
        self.remove_key(&format!("Profile/{uuid}"));
    }

    pub fn selected_player(&self) -> Option<String> {
        self.get_string("SelectedPlayer").filter(|s| !s.is_empty())
    }

    pub fn select_player(&mut self, uuid: &str) {
        self.set_string("SelectedPlayer", uuid);
    }

    // ---- 实例目录映射（Instances/<随机目录名> = 显示名） ----

    pub fn set_instance_dir(&mut self, dir_name: &str, display_name: &str) {
        self.set_string(&format!("Instances/{dir_name}"), display_name);
    }

    /// dirName → displayName（QMap 字典序）
    pub fn instance_dirs(&self) -> BTreeMap<String, String> {
        let mut map = BTreeMap::new();
        for (k, v) in &self.entries {
            if let Some(dir) = k.strip_prefix("MLC/Instances/") {
                // 键路径含 '/' 的历史数据不存在（dirName 为随机目录名），
                // 取余下整段以容忍未来扩展
                map.insert(dir.to_string(), v.clone());
            }
        }
        map
    }

    pub fn remove_instance_dir(&mut self, dir_name: &str) {
        self.remove_key(&format!("Instances/{dir_name}"));
    }

    /// 显示名反查目录名（找不到 → None；对齐 C++ 空显示名直接返回空）
    pub fn dir_for_display_name(&self, display_name: &str) -> Option<String> {
        if display_name.is_empty() {
            return None;
        }
        self.instance_dirs()
            .into_iter()
            .find(|(_, name)| name == display_name)
            .map(|(dir, _)| dir)
    }

    // ---- 实例隔离设置（Instance_<id>/<key>） ----

    pub fn get_instance(&self, instance_id: &str, key: &str) -> Option<String> {
        if instance_id.is_empty() {
            return self.get_string(key);
        }
        self.get_string(&format!("Instance_{instance_id}/{key}"))
    }

    pub fn set_instance(&mut self, instance_id: &str, key: &str, value: &str) {
        if instance_id.is_empty() {
            self.set_string(key, value);
            return;
        }
        self.set_string(&format!("Instance_{instance_id}/{key}"), value);
    }

    // ---- 其他 ----

    /// 默认游戏目录（可执行文件旁 mc/，缺省自动写入），对应 initDefaults
    pub fn init_defaults(&mut self) {
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|d| d.to_path_buf()))
            .unwrap_or_else(|| PathBuf::from("."));
        let default_mc = format!("{}/mc/", exe_dir.to_string_lossy().replace('\\', "/"));
        if self.get_string("LaunchFolderSelect").is_none() {
            self.set_string("LaunchFolderSelect", &default_mc);
        }
        if self.get_string("LoginType").is_none() {
            self.set_int("LoginType", 0); // Legacy（离线）
        }
        if self.get_string("LaunchArgumentWindowType").is_none() {
            self.set_int("LaunchArgumentWindowType", 1); // Windowed
        }
        if self.get_string("LaunchArgumentPriority").is_none() {
            self.set_int("LaunchArgumentPriority", 1); // Normal
        }
        if self.get_string("LaunchArgumentRam").is_none() {
            self.set_bool("LaunchArgumentRam", true);
        }
        if self.get_string("VersionRamOptimize").is_none() {
            self.set_int("VersionRamOptimize", 0); // 全局设置
        }
        if self.get_string("LaunchAdvanceGC").is_none() {
            self.set_int("LaunchAdvanceGC", 0); // Auto
        }
        if self.get_string("SystemLaunchCount").is_none() {
            self.set_int("SystemLaunchCount", 0);
        }
    }

    /// 实例路径（对齐 instancePath：基目录规范化尾斜杠 + versions/<id>/；
    /// 基目录未设置时回退 ~/.minecraft/）
    pub fn instance_path(&self, instance_id: &str) -> String {
        let mut base = self
            .get_string("LaunchFolderSelect")
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| {
                format!(
                    "{}/.minecraft/",
                    crate::util::platform::home_dir()
                        .to_string_lossy()
                        .replace('\\', "/")
                )
            });
        if !base.ends_with('/') {
            base.push('/');
        }
        if instance_id.is_empty() {
            return base;
        }
        format!("{base}versions/{instance_id}/")
    }
}

// ---- 全局单例（对齐 Settings::instance()/initialize） ----

static GLOBAL: OnceLock<Mutex<Option<Settings>>> = OnceLock::new();

fn global() -> &'static Mutex<Option<Settings>> {
    GLOBAL.get_or_init(|| Mutex::new(None))
}

/// 默认配置路径：可执行文件旁 MLC.ini（对齐 initialize 的缺省分支）
pub fn default_config_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
        .join("MLC.ini")
}

/// 初始化全局配置（幂等：已初始化则忽略，对齐 "Already initialized"）。
/// `config_path` 为 None 时用可执行文件旁 MLC.ini；加载后写入默认值。
pub fn initialize(config_path: Option<&Path>) {
    let mut guard = global().lock().unwrap();
    if guard.is_some() {
        return;
    }
    let path = config_path
        .map(|p| p.to_path_buf())
        .unwrap_or_else(default_config_path);
    let mut settings = Settings::load(&path);
    settings.init_defaults();
    *guard = Some(settings);
}

/// 访问全局配置；未初始化时闭包不执行、返回 None（对齐 !m_settings 分支）。
pub fn with_global<T>(f: impl FnOnce(&mut Settings) -> T) -> Option<T> {
    let mut guard = global().lock().unwrap();
    guard.as_mut().map(f)
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_settings(tag: &str) -> Settings {
        let dir = std::env::temp_dir().join(format!("mlc-test-{}-{tag}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let mut s = Settings::load(&dir.join("MLC.ini"));
        s.init_defaults();
        s
    }

    #[test]
    fn 基础键值与持久化() {
        let dir = std::env::temp_dir().join(format!("mlc-test-{}-persist", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("MLC.ini");

        let mut s = Settings::load(&path);
        s.set_string("LaunchFolderSelect", "/home/u/my mc/");
        s.set_int("LoginType", 0);
        s.set_bool("LaunchArgumentRam", true);
        s.set_string("Instances/d1", "我的整合包");
        s.set_encrypted("CfApiKey", "secret-key-123");

        // 重新加载：文件内容必须无损恢复（含加密与中文）
        let s2 = Settings::load(&path);
        assert_eq!(
            s2.get_string("LaunchFolderSelect").as_deref(),
            Some("/home/u/my mc/")
        );
        assert_eq!(s2.get_int("LoginType", -1), 0);
        assert!(s2.get_bool("LaunchArgumentRam", false));
        assert_eq!(s2.dir_for_display_name("我的整合包").as_deref(), Some("d1"));
        assert_eq!(s2.get_encrypted("CfApiKey"), "secret-key-123");
    }

    #[test]
    fn 默认值只写一次且缺省可读() {
        let mut s = temp_settings("defaults");
        // init_defaults 已在 temp_settings 内执行
        assert!(s
            .get_string("LaunchFolderSelect")
            .unwrap()
            .ends_with("/mc/"));
        assert_eq!(s.get_int("LoginType", 99), 0);
        assert_eq!(s.get_int("LaunchArgumentWindowType", 99), 1);
        assert!(s.get_bool("LaunchArgumentRam", false));
        assert_eq!(s.get_int("SystemLaunchCount", 99), 0);
        // 已存在的不覆盖
        s.set_int("LoginType", 2);
        s.init_defaults();
        assert_eq!(s.get_int("LoginType", 99), 2);
    }

    #[test]
    fn 类型解析对齐qvariant() {
        let mut s = temp_settings("types");
        assert_eq!(s.get_int("nope", 7), 7); // 缺失 → default
        s.set_string("BadInt", "abc");
        assert_eq!(s.get_int("BadInt", 7), 0); // 存在但非数字 → 0
        s.set_string("B1", "true");
        s.set_string("B2", "1");
        s.set_string("B3", "false");
        s.set_string("B4", "0");
        s.set_string("B5", "whatever");
        assert!(s.get_bool("B1", false));
        assert!(s.get_bool("B2", false));
        assert!(!s.get_bool("B3", true));
        assert!(!s.get_bool("B4", true));
        assert!(!s.get_bool("B5", true));
    }

    #[test]
    fn 玩家档案增删查与排序() {
        let mut s = temp_settings("players");
        s.set_profile("uuid-b", "Name", "乙");
        s.set_profile("uuid-a", "Name", "甲");
        s.set_profile("uuid-a", "SkinType", "slim");
        s.select_player("uuid-b");

        assert_eq!(s.player_profiles(), vec!["uuid-a", "uuid-b"]); // 字典序
        assert_eq!(s.selected_player().as_deref(), Some("uuid-b"));
        assert_eq!(s.get_profile("uuid-a", "Name").as_deref(), Some("甲"));
        assert_eq!(s.get_profile("uuid-a", "SkinType").as_deref(), Some("slim"));

        s.remove_profile("uuid-a");
        assert_eq!(s.player_profiles(), vec!["uuid-b"]);
        // remove_profile 删整组：子键一并消失
        assert_eq!(s.get_profile("uuid-a", "SkinType"), None);
        // 空 uuid 操作被忽略（对齐 C++ 判空）
        s.set_profile("", "Name", "x");
        s.remove_profile("");
        assert!(s.player_profiles().iter().all(|u| !u.is_empty()));
    }

    #[test]
    fn 实例映射与显示名反查() {
        let mut s = temp_settings("instances");
        s.set_instance_dir("abc123", "Pack B");
        s.set_instance_dir("xyz789", "Pack A");
        let dirs = s.instance_dirs();
        let keys: Vec<_> = dirs.keys().map(String::as_str).collect();
        assert_eq!(keys, vec!["abc123", "xyz789"]); // QMap 字典序
        assert_eq!(s.dir_for_display_name("Pack A").as_deref(), Some("xyz789"));
        assert_eq!(s.dir_for_display_name(""), None); // 空显示名 → 无
        assert_eq!(s.dir_for_display_name("不存在"), None);
        s.remove_instance_dir("abc123");
        assert!(s.dir_for_display_name("Pack B").is_none());
    }

    #[test]
    fn 删除键支持子树与实例隔离键() {
        let mut s = temp_settings("remove");
        s.set_instance("inst-1", "Memory", "4096");
        assert_eq!(s.get_instance("inst-1", "Memory").as_deref(), Some("4096"));
        assert_eq!(
            s.get_instance("", "LaunchFolderSelect").is_some(),
            s.get_string("LaunchFolderSelect").is_some()
        ); // 空 id 退回全局键
        s.set_profile("u1", "Name", "n");
        s.remove_key("Profile/u1"); // 子树删除
        assert!(s.get_profile("u1", "Name").is_none());
    }

    #[test]
    fn 实例路径规范化() {
        let s = temp_settings("path");
        let base = s.instance_path("");
        assert!(base.ends_with('/'));
        let v = s.instance_path("1.20.1");
        assert!(v.ends_with("/versions/1.20.1/"));
        assert!(v.starts_with(base.as_str()));
    }
}
