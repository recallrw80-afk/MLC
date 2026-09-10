//! 原版版本下载管线，对应 C++ `assetdownloader.cpp`，流程逐行为镜像：
//! manifest → 版本 JSON 落盘 → client JAR（SHA1 校验）→ libraries → assets → natives。
//!
//! 关键语义（对齐 C++）：
//! - 并行扇出（libraries/assets/natives 各自全量并行，受引擎的 16 路限流约束）
//! - libraries/natives 一个失败即整体失败；assets 部分失败不致命（缺贴图游戏可跑），
//!   全部失败才算失败（对齐 C++ 的 `f < total` 判定）
//! - 存在且 SHA1 匹配即跳过（重复执行 = 校验补齐）
//! - 写盘失败/磁盘满截断必须立即报错，不静默继续
//!
//! 与 C++ 的已知差异（有意为之）：
//! - C++ 的 downloadVersion 管线下载 JSON/JAR/libraries/assets 但**从未调用 downloadNatives**
//!   （versionmanager.cpp 单独调用）；Rust 版在 download_version 里串入 natives，使
//!   `mc-install` 一次到位，语义与 C++ 的 mc-install 完整路径一致。

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use serde_json::Value;

use super::manager::{assets_index_url, version_manifest_url, DownloadManager};
use crate::util::file;
use crate::util::platform;

/// 版本下载上下文（对齐 McVersion 的路径三元组）
pub struct VersionContext {
    /// 版本 ID（如 "1.20.1"）
    pub id: String,
    /// {mcRoot}/versions/{id}/
    pub version_dir: PathBuf,
    /// {mcRoot}/（全局 libraries/assets 所在）
    pub mc_root: PathBuf,
}

/// 下载阶段（供 CLI 显示）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stage {
    Manifest,
    VersionJson,
    ClientJar,
    Libraries,
    Assets,
    Natives,
    Done,
}

pub type StageCallback = Arc<dyn Fn(Stage) + Send + Sync>;

pub struct AssetDownloader {
    manager: DownloadManager,
    on_stage: Option<StageCallback>,
    /// 资产下载基址（C++ 写死 resources.download.minecraft.net；测试可注入本地服务器）
    resources_base: String,
}

impl AssetDownloader {
    pub fn new(manager: DownloadManager) -> Self {
        Self {
            manager,
            on_stage: None,
            resources_base: "https://resources.download.minecraft.net".to_string(),
        }
    }

    /// 测试用：注入资产基址（生产不要调）
    pub fn resources_base(mut self, base: &str) -> Self {
        self.resources_base = base.trim_end_matches('/').to_string();
        self
    }

    pub fn on_stage(mut self, cb: StageCallback) -> Self {
        self.on_stage = Some(cb);
        self
    }

    fn stage(&self, s: Stage) {
        if let Some(cb) = &self.on_stage {
            cb(s);
        }
    }

    /// 读官方清单的 latest.release（对齐 installVersion 空版本号分支）
    pub async fn latest_release_id(&self) -> Result<String, String> {
        self.latest_release_id_from_url(version_manifest_url())
            .await
    }

    /// 测试可注入清单 URL
    pub async fn latest_release_id_from_url(&self, manifest_url: &str) -> Result<String, String> {
        let (_, manifest) = self
            .manager
            .download_json(manifest_url, &[])
            .await
            .map_err(|e| e.to_string())?;
        let id = manifest
            .get("latest")
            .and_then(|l| l.get("release"))
            .and_then(|r| r.as_str())
            .unwrap_or("");
        if id.is_empty() {
            return Err("版本清单缺少 latest.release".into());
        }
        Ok(id.to_string())
    }

    /// 下载完整版本（manifest → JSON → JAR → libraries → assets → natives）
    pub async fn download_version(&self, version_id: &str, mc_root: &Path) -> Result<(), String> {
        // Step 1: 版本清单
        self.stage(Stage::Manifest);
        let (_, manifest) = self
            .manager
            .download_json(version_manifest_url(), &[])
            .await
            .map_err(|e| e.to_string())?;

        let versions = manifest
            .get("versions")
            .and_then(|v| v.as_array())
            .ok_or_else(|| "Malformed version manifest".to_string())?;

        let version_url = versions
            .iter()
            .find(|v| v.get("id").and_then(|i| i.as_str()) == Some(version_id))
            .and_then(|v| v.get("url"))
            .and_then(|u| u.as_str())
            .map(str::to_string)
            .ok_or_else(|| format!("Version not found: {version_id}"))?;

        // Step 2: 版本 JSON 落盘
        self.stage(Stage::VersionJson);
        let (_, ver_json) = self
            .manager
            .download_json(&version_url, &[])
            .await
            .map_err(|e| format!("下载版本 JSON 失败: {e}"))?;

        let version_dir = mc_root.join("versions").join(version_id);
        let json_path = version_dir.join(format!("{version_id}.json"));
        std::fs::create_dir_all(&version_dir).map_err(|e| format!("无法创建目录: {e}"))?;
        let json_bytes =
            serde_json::to_vec(&ver_json).map_err(|e| format!("序列化版本 JSON 失败: {e}"))?;
        std::fs::write(&json_path, &json_bytes)
            .map_err(|e| format!("无法写入版本 JSON {}: {e}", json_path.display()))?;

        let ctx = VersionContext {
            id: version_id.to_string(),
            version_dir,
            mc_root: mc_root.to_path_buf(),
        };

        // Step 3: client JAR
        self.stage(Stage::ClientJar);
        self.download_client_jar(&ctx, &json_path).await?;

        // Step 4: libraries
        self.stage(Stage::Libraries);
        self.download_libraries(&ctx, &ver_json).await?;

        // Step 5: assets
        self.stage(Stage::Assets);
        self.download_assets(&ctx, &ver_json).await?;

        // Step 6: natives
        self.stage(Stage::Natives);
        self.download_natives(&ctx, &ver_json).await?;

        self.stage(Stage::Done);
        Ok(())
    }

    /// client JAR（SHA1 校验 + 缓存跳过）
    pub async fn download_client_jar(
        &self,
        ctx: &VersionContext,
        json_path: &Path,
    ) -> Result<(), String> {
        let text =
            std::fs::read_to_string(json_path).map_err(|_| "无法读取版本 JSON".to_string())?;
        let ver_json: Value =
            serde_json::from_str(&text).map_err(|_| "版本 JSON 非法".to_string())?;

        let client = ver_json
            .get("downloads")
            .and_then(|d| d.get("client"))
            .filter(|c| c.is_object())
            .ok_or_else(|| "版本 JSON 缺少 client 下载项".to_string())?;
        let url = client.get("url").and_then(|u| u.as_str()).unwrap_or("");
        let sha1 = client.get("sha1").and_then(|s| s.as_str()).unwrap_or("");
        if url.is_empty() {
            return Err("无 client 下载 URL".to_string());
        }

        let jar_path = ctx.version_dir.join(format!("{}.jar", ctx.id));
        if jar_path.exists() && !sha1.is_empty() && file::verify_sha1(&jar_path, sha1) {
            tracing::info!("client JAR 已是最新: {}", ctx.id);
            return Ok(());
        }

        tracing::info!("下载 client JAR: {url}");
        self.manager
            .download_file(url, &jar_path, None)
            .await
            .map_err(|e| e.to_string())?;

        if !sha1.is_empty() && !file::verify_sha1(&jar_path, sha1) {
            let _ = std::fs::remove_file(&jar_path);
            return Err("SHA1 校验失败".to_string());
        }
        tracing::info!("client JAR 下载完成: {}", jar_path.display());
        Ok(())
    }

    /// libraries（并行扇出；任一失败即整体失败）
    pub async fn download_libraries(
        &self,
        ctx: &VersionContext,
        ver_json: &Value,
    ) -> Result<(), String> {
        let libs = match ver_json.get("libraries").and_then(|l| l.as_array()) {
            None => {
                tracing::info!("无 libraries 可下载");
                return Ok(());
            }
            Some(l) => l,
        };

        let mut to_download: Vec<(String, PathBuf)> = Vec::new();
        for lib in libs {
            if !lib.is_object() || !rules_allow_this_platform(lib) {
                continue;
            }
            let artifact = lib.get("downloads").and_then(|d| d.get("artifact"));
            match artifact {
                Some(a) if a.is_object() => {
                    let url = a.get("url").and_then(|u| u.as_str()).unwrap_or("");
                    let rel = a.get("path").and_then(|p| p.as_str()).unwrap_or("");
                    let sha1 = a.get("sha1").and_then(|s| s.as_str()).unwrap_or("");
                    if url.is_empty() || rel.is_empty() {
                        continue;
                    }
                    let save = ctx.mc_root.join("libraries").join(rel);
                    if save.exists() && !sha1.is_empty() && file::verify_sha1(&save, sha1) {
                        continue; // 已是最新
                    }
                    to_download.push((url.to_string(), save));
                }
                _ => {
                    // 无 artifact：natives 容器由 download_natives 处理；旧格式走 maven 坐标推导
                    if lib.get("natives").is_some() {
                        continue;
                    }
                    let name = lib.get("name").and_then(|n| n.as_str()).unwrap_or("");
                    let rel = file::maven_name_to_path(name);
                    if rel.is_empty() {
                        continue;
                    }
                    let base = lib
                        .get("url")
                        .and_then(|u| u.as_str())
                        .filter(|u| !u.is_empty())
                        .map(|u| {
                            if u.ends_with('/') {
                                u.to_string()
                            } else {
                                format!("{u}/")
                            }
                        })
                        .unwrap_or_else(|| "https://libraries.minecraft.net/".to_string());
                    let save = ctx.mc_root.join("libraries").join(&rel);
                    if save.exists() {
                        continue; // 无 sha1 可校验，存在即跳过
                    }
                    to_download.push((format!("{base}{rel}"), save));
                }
            }
        }

        if to_download.is_empty() {
            tracing::info!("libraries 全部已是最新");
            return Ok(());
        }

        let total = to_download.len();
        tracing::info!("下载 {total} 个 libraries…");
        let failed = Arc::new(AtomicUsize::new(0));
        let mut tasks = Vec::with_capacity(total);
        for (url, save) in to_download {
            let manager = &self.manager;
            let failed = failed.clone();
            tasks.push(async move {
                if let Some(dir) = save.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                if manager.download_file(&url, &save, None).await.is_err() {
                    failed.fetch_add(1, Ordering::SeqCst);
                }
            });
        }
        futures_util::future::join_all(tasks).await;

        let f = failed.load(Ordering::SeqCst);
        if f > 0 {
            return Err(format!("{f}/{total} 个 libraries 下载失败"));
        }
        tracing::info!("libraries 全部下载完成");
        Ok(())
    }

    /// assets（并行扇出；部分失败不致命，全部失败才算失败——对齐 C++ 的 f < total）
    pub async fn download_assets(
        &self,
        ctx: &VersionContext,
        ver_json: &Value,
    ) -> Result<(), String> {
        let asset_index_id = ver_json
            .get("assetIndex")
            .and_then(|a| a.get("id"))
            .and_then(|i| i.as_str())
            .map(str::to_string)
            .or_else(|| {
                ver_json
                    .get("assets")
                    .and_then(|a| a.as_str())
                    .map(str::to_string)
            })
            .unwrap_or_default();
        if asset_index_id.is_empty() {
            tracing::info!("无资产索引（legacy assets）");
            return Ok(());
        }

        let asset_index_url = ver_json
            .get("assetIndex")
            .and_then(|a| a.get("url"))
            .and_then(|u| u.as_str())
            .map(str::to_string)
            .unwrap_or_else(|| assets_index_url(&asset_index_id));

        tracing::info!("下载资产索引: {asset_index_id}");
        let (_, index_json) = self
            .manager
            .download_json(&asset_index_url, &[])
            .await
            .map_err(|e| format!("下载资产索引失败: {e}"))?;

        // 索引落盘（游戏启动需要 assets/indexes/<id>.json）
        let idx_path = ctx
            .mc_root
            .join("assets")
            .join("indexes")
            .join(format!("{asset_index_id}.json"));
        if let Some(dir) = idx_path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| format!("无法创建资产目录: {e}"))?;
        }
        let idx_bytes =
            serde_json::to_vec(&index_json).map_err(|e| format!("序列化资产索引失败: {e}"))?;
        std::fs::write(&idx_path, &idx_bytes)
            .map_err(|e| format!("无法写入资产索引 {}: {e}", idx_path.display()))?;

        let objects = match index_json.get("objects").and_then(|o| o.as_object()) {
            None => return Ok(()),
            Some(o) => o,
        };

        let mut to_download: Vec<(String, PathBuf)> = Vec::new();
        for (_name, entry) in objects {
            let hash = entry.get("hash").and_then(|h| h.as_str()).unwrap_or("");
            if hash.is_empty() {
                continue;
            }
            let sub = file::asset_path_from_hash(hash);
            let save = ctx.mc_root.join("assets").join("objects").join(&sub);
            if save.exists() && file::verify_sha1(&save, hash) {
                continue;
            }
            let url = format!("{}/{sub}", self.resources_base);
            to_download.push((url, save));
        }

        if to_download.is_empty() {
            tracing::info!("资产全部已是最新");
            return Ok(());
        }

        let total = to_download.len();
        tracing::info!("下载 {total} 个资产…");
        let failed = Arc::new(AtomicUsize::new(0));
        let mut tasks = Vec::with_capacity(total);
        for (url, save) in to_download {
            let manager = &self.manager;
            let failed = failed.clone();
            tasks.push(async move {
                if let Some(dir) = save.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                if manager.download_file(&url, &save, None).await.is_err() {
                    failed.fetch_add(1, Ordering::SeqCst);
                }
            });
        }
        futures_util::future::join_all(tasks).await;

        let f = failed.load(Ordering::SeqCst);
        if f == total && total > 0 {
            return Err(format!("全部 {total} 个资产下载失败"));
        }
        if f > 0 {
            tracing::warn!("{f}/{total} 个资产失败（部分失败不致命）");
        } else {
            tracing::info!("资产全部下载完成");
        }
        Ok(())
    }

    /// natives（并行扇出 + 解压拍平；任一失败即整体失败）
    pub async fn download_natives(
        &self,
        ctx: &VersionContext,
        ver_json: &Value,
    ) -> Result<(), String> {
        let libs = match ver_json.get("libraries").and_then(|l| l.as_array()) {
            None => return Ok(()),
            Some(l) => l,
        };

        let os = platform::platform_name();
        let arch64 = platform::is_64bit();
        let natives_suffix = format!(":natives-{os}");
        let classifier_key = match (os, arch64) {
            ("windows", true) => "natives-windows-64",
            ("linux", true) => "natives-linux-64",
            _ => "",
        };
        let native_classifier = format!("natives-{os}");

        let mut to_download: Vec<(String, PathBuf)> = Vec::new();
        let mut all_native_jars: Vec<PathBuf> = Vec::new();

        for lib in libs {
            if !lib.is_object() || !rules_allow_this_platform(lib) {
                continue;
            }
            let lib_name = lib.get("name").and_then(|n| n.as_str()).unwrap_or("");

            // 新格式（1.19+）：name 带 :natives-<os> 后缀，jar 已由 libraries 阶段下载
            if lib_name.len() > natives_suffix.len() && lib_name.ends_with(&natives_suffix) {
                if let Some(p) = lib
                    .get("downloads")
                    .and_then(|d| d.get("artifact"))
                    .and_then(|a| a.get("path"))
                    .and_then(|p| p.as_str())
                {
                    all_native_jars.push(ctx.mc_root.join("libraries").join(p));
                }
                continue;
            }

            // 旧格式（≤1.18）：downloads.classifiers 里的 natives-<os>[-64] 条目
            let Some(classifiers) = lib.get("downloads").and_then(|d| d.get("classifiers")) else {
                continue;
            };

            let native = classifiers
                .get(classifier_key)
                .filter(|_| !classifier_key.is_empty())
                .or_else(|| classifiers.get(&native_classifier));
            let Some(native) = native else { continue };

            let url = native.get("url").and_then(|u| u.as_str()).unwrap_or("");
            let rel = native.get("path").and_then(|p| p.as_str()).unwrap_or("");
            if url.is_empty() || rel.is_empty() {
                continue;
            }
            let save = ctx.mc_root.join("libraries").join(rel);
            all_native_jars.push(save.clone());
            if save.exists() {
                let sha1 = native.get("sha1").and_then(|s| s.as_str()).unwrap_or("");
                if !sha1.is_empty() && file::verify_sha1(&save, sha1) {
                    continue;
                }
            }
            to_download.push((url.to_string(), save));
        }

        let natives_dir = ctx.version_dir.join("natives");

        // 先并行下载缺失项，再统一解压（对齐 C++ extractAll 在全部完成后调用）
        if !to_download.is_empty() {
            let total = to_download.len();
            tracing::info!("下载 {total} 个 natives…");
            let failed = Arc::new(AtomicUsize::new(0));
            let mut tasks = Vec::with_capacity(total);
            for (url, save) in to_download {
                let manager = &self.manager;
                let failed = failed.clone();
                tasks.push(async move {
                    if let Some(dir) = save.parent() {
                        let _ = std::fs::create_dir_all(dir);
                    }
                    if manager.download_file(&url, &save, None).await.is_err() {
                        failed.fetch_add(1, Ordering::SeqCst);
                    }
                });
            }
            futures_util::future::join_all(tasks).await;
            let f = failed.load(Ordering::SeqCst);
            if f > 0 {
                return Err(format!("{f}/{total} 个 natives 下载失败"));
            }
        }

        // 解压全部本机平台 native jar（含已缓存的——jar 在 libraries/ 但 natives/ 可能从未解压）
        if all_native_jars.is_empty() {
            tracing::info!("natives 全部已是最新");
            return Ok(());
        }
        tracing::info!("解压 native libraries…");
        for jar in &all_native_jars {
            if !jar.exists() {
                continue;
            }
            let extracted = file::extract_natives_jar(jar, &natives_dir);
            for f in extracted {
                tracing::info!(
                    "  解压: {}",
                    Path::new(&f)
                        .file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                );
            }
        }
        tracing::info!("natives 解压到: {}", natives_dir.display());
        Ok(())
    }
}

/// 库规则判定（对齐 C++ rulesAllowOnThisPlatform）：
/// 无 rules → 允许；每条匹配的规则覆盖前面的结果；缺 action 按 allow。
/// os.name/os.arch 双条件都非空时需同时匹配（对齐 C++ 的 matches = matches && …）。
pub fn rules_allow_this_platform(lib: &Value) -> bool {
    let rules = match lib.get("rules").and_then(|r| r.as_array()) {
        None => return true,
        Some(r) => r,
    };
    let mut allowed = false;
    for rule in rules {
        let action = rule.get("action").and_then(|a| a.as_str()).unwrap_or("");
        let rule_allows = action.is_empty() || action == "allow";
        let mut matches = true;
        if let Some(os) = rule.get("os").filter(|o| o.is_object()) {
            let os_name = os.get("name").and_then(|n| n.as_str()).unwrap_or("");
            let os_arch = os.get("arch").and_then(|a| a.as_str()).unwrap_or("");
            if !os_name.is_empty() {
                matches = os_name == platform::platform_name();
            }
            if !os_arch.is_empty() {
                matches = matches
                    && os_arch
                        == if platform::is_64bit() {
                            "x86_64"
                        } else {
                            "x86"
                        };
            }
        }
        if matches {
            allowed = rule_allows;
        }
    }
    allowed
}
