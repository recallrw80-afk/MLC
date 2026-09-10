//! Mod 平台 API（CurseForge / Modrinth），对应 C++ `modplatform.cpp`。
//!
//! CF key 解析链（对齐 C++）：用户设置（Settings 加密存储）→ 编译期内嵌 →
//! 无 key 走 MCIM 镜像。请求链：有 key 走官方 API 并附带 `x-api-key`；
//! 官方返回 401/403/429（key 失效/被吊销/配额超限）时自动回退镜像重试一次。
//!
//! ⚠️ 内嵌 key 禁止回显——任何状态展示只能用 [`CfKeySource`]，不得输出 key 本体。

use std::sync::OnceLock;

use serde_json::Value;

use super::manager::{DownloadError, DownloadManager, ProgressCallback};

// ---- API 基址（对齐 C++ 常量） ----

pub const CF_API: &str = "https://api.curseforge.com/v1";
pub const CF_MIRROR: &str = "https://mod.mcimirror.top/curseforge/v1";
pub const MR_API: &str = "https://api.modrinth.com/v2";

/// 编译期内嵌的 CF key（build.rs 从 MLC/.env 读入；未嵌入为空串）
pub const EMBEDDED_CF_KEY: &str = match option_env!("MLC_CF_API_KEY_EMBEDDED") {
    Some(k) => k,
    None => "",
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Platform {
    CurseForge,
    Modrinth,
}

/// 资源类型（CF classId / Modrinth project_type 映射见下方常量表）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResourceType {
    Mod,
    ModPack,
    ResourcePack,
    Shader,
    DataPack,
}

/// CF key 当前来源（对齐 C++ cfApiKeySource；展示专用，永不含 key 本体）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CfKeySource {
    User,
    Embedded,
    None,
}

/// Mod 资源条目（搜索/详情），对应 C++ ModResource
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ModResource {
    pub id: String,
    pub name: String,
    pub summary: String,
    pub description: String,
    pub author: String,
    pub icon_url: String,
    pub website_url: String,
    pub download_count: i64,
    pub last_updated: i64,
    pub versions: Vec<String>,
}

/// 文件条目，对应 C++ ModFileInfo
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ModFileInfo {
    pub id: String,
    pub display_name: String,
    pub file_name: String,
    pub download_url: String,
    pub game_versions: Vec<String>,
    pub loaders: Vec<String>,
    pub file_size: i64,
    pub release_date: i64,
    pub sha1: String,
    pub is_release: bool,
}

// ---- 解析辅助（对齐 C++ jsonStr：键缺失/null/类型不符 → 空串） ----

fn s<'a>(j: &'a Value, key: &str) -> &'a str {
    j.get(key).and_then(|v| v.as_str()).unwrap_or("")
}

fn n(j: &Value, key: &str) -> i64 {
    j.get(key).and_then(|v| v.as_i64()).unwrap_or(0)
}

fn strs(j: &Value, key: &str) -> Vec<String> {
    j.get(key)
        .and_then(|v| v.as_array())
        .map(|arr| {
            arr.iter()
                .filter_map(|v| v.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

// ---- 类型映射 ----

/// CF classId（gameId=432 Minecraft 下的分类）
pub fn cf_class_id_for(t: ResourceType) -> i64 {
    match t {
        ResourceType::Mod => 6,
        ResourceType::ModPack => 4471,
        ResourceType::ResourcePack => 12,
        ResourceType::Shader => 6552,
        ResourceType::DataPack => 6945,
    }
}

/// Modrinth facets 的 project_type
pub fn mr_project_type_for(t: ResourceType) -> &'static str {
    match t {
        ResourceType::Mod => "mod",
        ResourceType::ModPack => "modpack",
        ResourceType::ResourcePack => "resourcepack",
        ResourceType::Shader => "shader",
        ResourceType::DataPack => "datapack",
    }
}

// ---- 解析（纯函数，逐字段对齐 C++ 回调里的组装逻辑） ----

pub fn parse_cf_search_item(item: &Value) -> ModResource {
    let mut r = ModResource {
        id: n(item, "id").to_string(),
        name: s(item, "name").to_string(),
        summary: s(item, "summary").to_string(),
        download_count: n(item, "downloadCount"),
        ..Default::default()
    };
    if let Some(author) = item
        .get("authors")
        .and_then(|a| a.as_array())
        .and_then(|a| a.first())
    {
        r.author = s(author, "name").to_string();
    }
    if let Some(logo) = item.get("logo") {
        r.icon_url = s(logo, "thumbnailUrl").to_string();
    }
    if let Some(links) = item.get("links") {
        r.website_url = s(links, "websiteUrl").to_string();
    }
    if let Some(latest) = item.get("latestFiles").and_then(|f| f.get(0)) {
        r.versions = strs(latest, "gameVersions");
    }
    r
}

pub fn parse_cf_details(item: &Value) -> ModResource {
    ModResource {
        id: n(item, "id").to_string(),
        name: s(item, "name").to_string(),
        summary: s(item, "summary").to_string(),
        description: s(item, "description").to_string(),
        download_count: n(item, "downloadCount"),
        ..Default::default()
    }
}

/// 文件条目解析。哈希优先取 SHA1（algo=1），盲取 [0] 可能拿到 MD5（对齐 C++ 注释）
pub fn parse_cf_file(item: &Value) -> ModFileInfo {
    let mut f = ModFileInfo {
        id: n(item, "id").to_string(),
        display_name: s(item, "displayName").to_string(),
        file_name: s(item, "fileName").to_string(),
        download_url: s(item, "downloadUrl").to_string(),
        file_size: n(item, "fileLength"),
        game_versions: strs(item, "gameVersions"),
        ..Default::default()
    };
    if let Some(hashes) = item.get("hashes").and_then(|h| h.as_array()) {
        for h in hashes {
            let v = s(h, "value");
            if v.is_empty() {
                continue;
            }
            if n(h, "algo") == 1 {
                f.sha1 = v.to_string();
                break;
            }
            if f.sha1.is_empty() {
                f.sha1 = v.to_string();
            }
        }
    }
    f
}

pub fn parse_mr_search_item(item: &Value) -> ModResource {
    let id = s(item, "project_id").to_string();
    ModResource {
        id: id.clone(),
        name: s(item, "title").to_string(),
        summary: s(item, "description").to_string(),
        author: s(item, "author").to_string(),
        icon_url: s(item, "icon_url").to_string(),
        download_count: n(item, "downloads"),
        website_url: format!("https://modrinth.com/mod/{id}"),
        versions: strs(item, "versions"),
        ..Default::default()
    }
}

pub fn parse_mr_details(item: &Value) -> ModResource {
    let id = s(item, "id").to_string();
    ModResource {
        id: id.clone(),
        name: s(item, "title").to_string(),
        summary: s(item, "description").to_string(),
        description: s(item, "body").to_string(),
        download_count: n(item, "downloads"),
        website_url: format!("https://modrinth.com/mod/{id}"),
        ..Default::default()
    }
}

/// MR 版本条目 → 文件信息（files[0]）
pub fn parse_mr_version(ver: &Value) -> Option<ModFileInfo> {
    let file = ver.get("files")?.get(0)?;
    Some(ModFileInfo {
        id: s(ver, "id").to_string(),
        display_name: s(ver, "name").to_string(),
        file_name: s(file, "filename").to_string(),
        download_url: s(file, "url").to_string(),
        file_size: n(file, "size"),
        sha1: ver
            .get("files")
            .and_then(|f| f.get(0))
            .and_then(|f| f.get("hashes"))
            .map(|h| s(h, "sha1").to_string())
            .unwrap_or_default(),
        game_versions: strs(ver, "game_versions"),
        loaders: strs(ver, "loaders"),
        ..Default::default()
    })
}

// ---- URL 构建（对齐 C++ QUrlQuery 参数集） ----

fn cf_search_url(base: &str, class_id: i64, query: &str, page: u32, page_size: u32) -> String {
    use url::form_urlencoded::Serializer;
    let mut q = Serializer::new(String::new());
    q.append_pair("gameId", "432"); // Minecraft
    q.append_pair("classId", &class_id.to_string());
    if !query.is_empty() {
        q.append_pair("searchFilter", query);
    }
    q.append_pair("index", &(page * page_size).to_string());
    q.append_pair("pageSize", &page_size.to_string());
    q.append_pair("sortField", "2"); // Popularity
    q.append_pair("sortOrder", "desc");
    format!("{base}/mods/search?{}", q.finish())
}

fn mr_search_url(base: &str, project_type: &str, query: &str, page: u32, page_size: u32) -> String {
    use url::form_urlencoded::Serializer;
    let mut q = Serializer::new(String::new());
    if !query.is_empty() {
        q.append_pair("query", query);
    }
    q.append_pair("offset", &(page * page_size).to_string());
    q.append_pair("limit", &page_size.to_string());
    q.append_pair("facets", &format!("[[\"project_type:{project_type}\"]]"));
    format!("{base}/search?{}", q.finish())
}

// ---- ModPlatform ----

pub struct ModPlatform {
    manager: DownloadManager,
    cf_api_base: String,
    cf_mirror_base: String,
    mr_api_base: String,
    cf_api_key: String,
    cf_key_source: CfKeySource,
}

impl ModPlatform {
    /// 全局实例（对齐 C++ 单例；key 链只解析一次）
    pub fn shared() -> &'static ModPlatform {
        static SHARED: OnceLock<ModPlatform> = OnceLock::new();
        SHARED.get_or_init(|| {
            let user_key = crate::settings::with_global(|s| s.get_encrypted("CfApiKey"));
            let (key, source) = resolve_cf_key(user_key.as_deref(), EMBEDDED_CF_KEY);
            if source == CfKeySource::None {
                tracing::info!("未配置 CurseForge API key，使用 MCIM 镜像");
            }
            ModPlatform::with_key(key, source)
        })
    }

    fn with_key(cf_api_key: String, cf_key_source: CfKeySource) -> Self {
        Self {
            manager: DownloadManager::new(),
            cf_api_base: CF_API.to_string(),
            cf_mirror_base: CF_MIRROR.to_string(),
            mr_api_base: MR_API.to_string(),
            cf_api_key,
            cf_key_source,
        }
    }

    /// 测试构建：注入基址与 key（生产走 shared()）
    pub fn for_test(cf_api: &str, cf_mirror: &str, mr: &str, cf_api_key: &str) -> Self {
        let source = if cf_api_key.is_empty() {
            CfKeySource::None
        } else {
            CfKeySource::User
        };
        let mut p = Self::with_key(cf_api_key.to_string(), source);
        p.cf_api_base = cf_api.to_string();
        p.cf_mirror_base = cf_mirror.to_string();
        p.mr_api_base = mr.to_string();
        p
    }

    /// 注入下载引擎（测试用短超时）
    pub fn set_manager(&mut self, manager: DownloadManager) {
        self.manager = manager;
    }

    pub fn cf_key_source(&self) -> CfKeySource {
        self.cf_key_source
    }

    /// 无 key 时把官方地址改写为 MCIM 镜像（对齐 cfApiUrl）
    fn cf_api_url(&self, official_url: &str) -> String {
        if !self.cf_api_key.is_empty() {
            return official_url.to_string();
        }
        self.mirror_url_for(official_url)
    }

    /// 官方地址强制转镜像（降级用；不查 key，401/403/429 时 key 已失效）
    fn mirror_url_for(&self, official_url: &str) -> String {
        official_url.replacen(&self.cf_api_base, &self.cf_mirror_base, 1)
    }

    /// CF API GET：有 key 走官方并附带 x-api-key，无 key 走 MCIM 镜像；
    /// 官方 401/403/429（key 失效/超限）时自动回退镜像重试一次（对齐 cfJsonGet）
    async fn cf_json_get(&self, official_url: &str) -> Result<Value, String> {
        if self.cf_api_key.is_empty() {
            let url = self.cf_api_url(official_url);
            return self
                .manager
                .download_json(&url, &[])
                .await
                .map(|(_, v)| v)
                .map_err(|e| e.to_string());
        }
        let headers = vec![
            ("x-api-key".to_string(), self.cf_api_key.clone()),
            ("Accept".to_string(), "application/json".to_string()),
        ];
        match self.manager.download_json(official_url, &headers).await {
            Ok((_, v)) => Ok(v),
            Err(e @ DownloadError::HttpStatus { .. }) => {
                // 401/403/429（key 失效/超限）→ 回退镜像重试一次
                let code = match &e {
                    DownloadError::HttpStatus { code, .. } => *code,
                    _ => unreachable!(),
                };
                if code == 401 || code == 403 || code == 429 {
                    tracing::warn!(
                        "CF API 返回 {code}（key 失效或超限），回退 MCIM 镜像: {official_url}"
                    );
                    let mirrored = self.mirror_url_for(official_url);
                    return self
                        .manager
                        .download_json(&mirrored, &[])
                        .await
                        .map(|(_, v)| v)
                        .map_err(|e| e.to_string());
                }
                Err(e.to_string())
            }
            Err(e) => Err(e.to_string()),
        }
    }

    // ---- 搜索 / 详情 / 文件 ----

    pub async fn search_resources(
        &self,
        platform: Platform,
        r#type: ResourceType,
        query: &str,
        page: u32,
        page_size: u32,
    ) -> Result<Vec<ModResource>, String> {
        match platform {
            Platform::CurseForge => {
                let url = cf_search_url(
                    &self.cf_api_base,
                    cf_class_id_for(r#type),
                    query,
                    page,
                    page_size,
                );
                let result = self.cf_json_get(&url).await?;
                Ok(result
                    .get("data")
                    .and_then(|d| d.as_array())
                    .map(|arr| arr.iter().map(parse_cf_search_item).collect())
                    .unwrap_or_default())
            }
            Platform::Modrinth => {
                let url = mr_search_url(
                    &self.mr_api_base,
                    mr_project_type_for(r#type),
                    query,
                    page,
                    page_size,
                );
                let (_, result) = self
                    .manager
                    .download_json(&url, &[])
                    .await
                    .map_err(|e| e.to_string())?;
                Ok(result
                    .get("hits")
                    .and_then(|d| d.as_array())
                    .map(|arr| arr.iter().map(parse_mr_search_item).collect())
                    .unwrap_or_default())
            }
        }
    }

    pub async fn get_mod_details(
        &self,
        platform: Platform,
        mod_id: &str,
    ) -> Result<ModResource, String> {
        match platform {
            Platform::CurseForge => {
                let url = format!("{}/mods/{mod_id}", self.cf_api_base);
                let result = self.cf_json_get(&url).await?;
                result
                    .get("data")
                    .map(parse_cf_details)
                    .ok_or_else(|| "响应缺少 data".to_string())
            }
            Platform::Modrinth => {
                let url = format!("{}/project/{mod_id}", self.mr_api_base);
                let (_, item) = self
                    .manager
                    .download_json(&url, &[])
                    .await
                    .map_err(|e| e.to_string())?;
                Ok(parse_mr_details(&item))
            }
        }
    }

    pub async fn get_mod_files(
        &self,
        platform: Platform,
        mod_id: &str,
    ) -> Result<Vec<ModFileInfo>, String> {
        match platform {
            Platform::CurseForge => {
                let url = format!("{}/mods/{mod_id}/files", self.cf_api_base);
                let result = self.cf_json_get(&url).await?;
                Ok(result
                    .get("data")
                    .and_then(|d| d.as_array())
                    .map(|arr| arr.iter().map(parse_cf_file).collect())
                    .unwrap_or_default())
            }
            Platform::Modrinth => {
                let url = format!("{}/project/{mod_id}/version", self.mr_api_base);
                let (_, result) = self
                    .manager
                    .download_json(&url, &[])
                    .await
                    .map_err(|e| e.to_string())?;
                Ok(result
                    .as_array()
                    .map(|arr| arr.iter().filter_map(parse_mr_version).collect())
                    .unwrap_or_default())
            }
        }
    }

    /// 特定文件的解析端点（CF：先解析下载 URL 再下载；MR：version 端点即含文件 URL）
    pub fn file_download_url(&self, platform: Platform, mod_id: &str, file_id: &str) -> String {
        match platform {
            Platform::CurseForge => {
                format!(
                    "{}/mods/{mod_id}/files/{file_id}/download-url",
                    self.cf_api_base
                )
            }
            Platform::Modrinth => {
                format!("{}/project/{mod_id}/version/{file_id}", self.mr_api_base)
            }
        }
    }

    /// 下载 mod 文件（对应 C++ downloadMod）
    pub async fn download_mod(
        &self,
        platform: Platform,
        mod_id: &str,
        file_id: &str,
        save_path: &std::path::Path,
        on_progress: Option<ProgressCallback>,
    ) -> Result<u64, String> {
        let dl_url = match platform {
            Platform::CurseForge => {
                // 受限文件的 data 字段为 null（禁止第三方分发），空串即失败
                let url = self.file_download_url(platform, mod_id, file_id);
                let result = self.cf_json_get(&url).await?;
                let dl = result.get("data").and_then(|v| v.as_str()).unwrap_or("");
                if dl.is_empty() {
                    return Err("Empty download URL".to_string());
                }
                dl.to_string()
            }
            Platform::Modrinth => {
                let url = self.file_download_url(platform, mod_id, file_id);
                let (_, ver) = self
                    .manager
                    .download_json(&url, &[])
                    .await
                    .map_err(|e| e.to_string())?;
                let first_file = ver
                    .get("files")
                    .and_then(|f| f.as_array())
                    .and_then(|list| list.first());
                match first_file {
                    None => return Err("No files in version".to_string()),
                    Some(file) => file
                        .get("url")
                        .and_then(|v| v.as_str())
                        .unwrap_or("")
                        .to_string(),
                }
            }
        };
        if dl_url.is_empty() {
            return Err("Empty download URL".to_string());
        }
        self.manager
            .download_file(&dl_url, save_path, on_progress)
            .await
            .map_err(|e| e.to_string())
    }
}

/// key 解析链（纯函数）：用户设置 → 编译期内嵌 → 无（走镜像）
fn resolve_cf_key(user_key: Option<&str>, embedded: &str) -> (String, CfKeySource) {
    match user_key.filter(|k| !k.is_empty()) {
        Some(k) => (k.to_string(), CfKeySource::User),
        None if !embedded.is_empty() => (embedded.to_string(), CfKeySource::Embedded),
        None => (String::new(), CfKeySource::None),
    }
}

// ---------------------------------------------------------------- 测试（解析/映射/key 链纯函数）

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn 类型映射钉死() {
        assert_eq!(cf_class_id_for(ResourceType::Mod), 6);
        assert_eq!(cf_class_id_for(ResourceType::ModPack), 4471);
        assert_eq!(cf_class_id_for(ResourceType::ResourcePack), 12);
        assert_eq!(cf_class_id_for(ResourceType::Shader), 6552);
        assert_eq!(cf_class_id_for(ResourceType::DataPack), 6945);
        assert_eq!(mr_project_type_for(ResourceType::ModPack), "modpack");
        assert_eq!(mr_project_type_for(ResourceType::DataPack), "datapack");
    }

    #[test]
    fn key解析链() {
        // 用户设置优先
        assert_eq!(
            resolve_cf_key(Some("user-key"), "embedded"),
            ("user-key".to_string(), CfKeySource::User)
        );
        // 空用户 key 视为未设置 → 内嵌
        assert_eq!(
            resolve_cf_key(Some(""), "embedded"),
            ("embedded".to_string(), CfKeySource::Embedded)
        );
        // 无用户 key → 内嵌
        assert_eq!(
            resolve_cf_key(None, "embedded"),
            ("embedded".to_string(), CfKeySource::Embedded)
        );
        // 都没有 → 走镜像
        assert_eq!(resolve_cf_key(None, ""), (String::new(), CfKeySource::None));
    }

    #[test]
    fn cf搜索解析钉死() {
        let item = json!({
            "id": 455289,
            "name": "JEI",
            "summary": "Just Enough Items",
            "downloadCount": 300000000i64,
            "authors": [{"name": "mezz"}],
            "logo": {"thumbnailUrl": "https://thumb.png"},
            "links": {"websiteUrl": "https://curseforge.com/jei"},
            "latestFiles": [{"gameVersions": ["1.20.1", "Forge"]}]
        });
        let r = parse_cf_search_item(&item);
        assert_eq!(r.id, "455289");
        assert_eq!(r.name, "JEI");
        assert_eq!(r.summary, "Just Enough Items");
        assert_eq!(r.author, "mezz");
        assert_eq!(r.icon_url, "https://thumb.png");
        assert_eq!(r.website_url, "https://curseforge.com/jei");
        assert_eq!(r.download_count, 300000000);
        assert_eq!(r.versions, vec!["1.20.1", "Forge"]);
        // 缺失字段安全兜底（对齐 jsonStr）
        let empty = parse_cf_search_item(&json!({}));
        assert!(empty.name.is_empty() && empty.id == "0");
    }

    #[test]
    fn cf文件哈希优先sha1() {
        // MD5 在前、SHA1 在后 → 取 SHA1
        let f = parse_cf_file(&json!({
            "id": 1, "displayName": "d", "fileName": "f.jar", "fileLength": 123,
            "hashes": [
                {"algo": 2, "value": "md5hash"},
                {"algo": 1, "value": "sha1hash"}
            ],
            "gameVersions": ["1.20.1"]
        }));
        assert_eq!(f.sha1, "sha1hash");
        // SHA1 在前 → 直接取并停止
        let f = parse_cf_file(&json!({
            "hashes": [{"algo": 1, "value": "sha1first"}, {"algo": 2, "value": "md5"}]
        }));
        assert_eq!(f.sha1, "sha1first");
        // 只有 MD5 → 兜底取首个非空
        let f = parse_cf_file(&json!({
            "hashes": [{"algo": 2, "value": "onlymd5"}]
        }));
        assert_eq!(f.sha1, "onlymd5");
    }

    #[test]
    fn cf详情解析钉死() {
        let r = parse_cf_details(&json!({
            "id": 238222, "name": "Just Enough Items",
            "summary": "s", "description": "<p>body</p>", "downloadCount": 1
        }));
        assert_eq!(r.description, "<p>body</p>");
        assert_eq!(r.id, "238222");
    }

    #[test]
    fn modrinth解析钉死() {
        let hit = json!({
            "project_id": "u6dRJSh7", "title": "Fabric API",
            "description": "d", "author": "modmuss50",
            "icon_url": "https://i.png", "downloads": 900000000i64,
            "versions": ["1.20.1"]
        });
        let r = parse_mr_search_item(&hit);
        assert_eq!(r.id, "u6dRJSh7");
        assert_eq!(r.website_url, "https://modrinth.com/mod/u6dRJSh7");
        assert_eq!(r.download_count, 900000000);

        let ver = json!({
            "id": "ver1", "name": "v1.0", "game_versions": ["1.20.1"], "loaders": ["fabric"],
            "files": [{"filename": "f.jar", "url": "https://cdn/f.jar", "size": 2048,
                        "hashes": {"sha1": "abc"}}]
        });
        let f = parse_mr_version(&ver).expect("应有文件");
        assert_eq!(f.file_name, "f.jar");
        assert_eq!(f.download_url, "https://cdn/f.jar");
        assert_eq!(f.file_size, 2048);
        assert_eq!(f.sha1, "abc");
        assert_eq!(f.loaders, vec!["fabric"]);
        // files 缺失 → None（对齐 C++ continue）
        assert!(parse_mr_version(&json!({})).is_none());
    }

    #[test]
    fn 搜索url参数构建() {
        let u = cf_search_url("https://x/v1", 6, "jei mods", 2, 20);
        assert!(u.starts_with("https://x/v1/mods/search?"));
        assert!(u.contains("gameId=432"));
        assert!(u.contains("classId=6"));
        assert!(u.contains("searchFilter=jei+mods"));
        assert!(u.contains("index=40"));
        assert!(u.contains("pageSize=20"));
        assert!(u.contains("sortField=2"));
        assert!(u.contains("sortOrder=desc"));
        // 空查询不带 searchFilter
        let u = cf_search_url("https://x/v1", 6, "", 0, 20);
        assert!(!u.contains("searchFilter"));

        let u = mr_search_url("https://m/v2", "mod", "", 0, 25);
        assert!(u.starts_with("https://m/v2/search?"));
        assert!(u.contains("offset=0"));
        assert!(u.contains("limit=25"));
        assert!(u.contains(rlt("project_type:mod").as_str()));
        assert!(!u.contains("query="));
    }

    // facets 参数里的引号编码形式
    fn rlt(s: &str) -> String {
        url::form_urlencoded::Serializer::new(String::new())
            .append_pair("facets", &format!("[[\"{s}\"]]"))
            .finish()
            .trim_start_matches("facets=")
            .to_string()
    }

    #[test]
    fn cf地址改写按key有无() {
        let p = ModPlatform::for_test(CF_API, CF_MIRROR, MR_API, "some-key");
        assert_eq!(
            p.cf_api_url("https://api.curseforge.com/v1/mods/1"),
            "https://api.curseforge.com/v1/mods/1"
        );
        let p = ModPlatform::for_test(CF_API, CF_MIRROR, MR_API, "");
        assert_eq!(
            p.cf_api_url("https://api.curseforge.com/v1/mods/1"),
            "https://mod.mcimirror.top/curseforge/v1/mods/1"
        );
    }
}
