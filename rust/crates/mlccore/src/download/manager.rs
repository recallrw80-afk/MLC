//! HTTP 下载引擎，对应 C++ `downloadmanager.cpp`，逐行为镜像：
//! - UA `MLC/0.1`、仅 HTTP/1.1（对齐 Http2AllowedAttribute=false）、
//!   重定向跟随但禁止 https→http 降级（NoLessSafeRedirectPolicy）
//! - 并发限流 16（超出排队，语义等同 C++ 的 m_pending 队列）
//! - 两阶段超时：文件下载首字节 60s / 文本下载首字节 30s，此后每块数据 30s 停滞
//! - 失败重试（默认 3 次，间隔 500ms，HTTP ≥400 与网络错误同样重试——对齐 C++）
//! - 文件下载重试耗尽且主机为 edge.forgecdn.net 时，回退 MCIM 镜像（同路径换主机，带 1 次重试）
//! - 文件下载失败不留半成品（落盘中途失败删除文件，对齐 C++ 磁盘满删半成品的语义）
//!
//! 与 C++ 的差异（有意为之）：
//! - 流式落盘（C++ 全量缓冲后一次写入）——对上层语义相同，内存占用更好
//! - 断点续传与 C++ 相同的粒度：无 HTTP Range，靠"存在且 SHA1 匹配即跳过"（asset 层职责）

use std::path::Path;
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use reqwest::header::HeaderMap;
use serde_json::Value;
use tokio::io::AsyncWriteExt;
use tokio::sync::Semaphore;
use url::Url;

/// 并发上限，对齐 C++ `kMaxInFlight`
pub const MAX_IN_FLIGHT: usize = 16;
pub const USER_AGENT: &str = "MLC/0.1";

const FILE_FIRST_BYTE_TIMEOUT: Duration = Duration::from_secs(60);
const TEXT_FIRST_BYTE_TIMEOUT: Duration = Duration::from_secs(30);
const STALL_TIMEOUT: Duration = Duration::from_secs(30);
const MAX_RETRIES: u32 = 3;
const RETRY_DELAY: Duration = Duration::from_millis(500);

/// 进度回调：(received, total)；total 未知时为 0
pub type ProgressCallback = Arc<dyn Fn(u64, u64) + Send + Sync>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TimeoutPhase {
    /// 响应头阶段（连接/服务器装死兜底）
    FirstByte,
    /// 传输停滞
    Stall,
}

#[derive(Debug, Clone)]
pub enum DownloadError {
    /// HTTP ≥400（code 供上层做降级判定，如 CF key 401/403/429 回退镜像）
    HttpStatus {
        url: String,
        code: u16,
    },
    Timeout {
        url: String,
        phase: TimeoutPhase,
    },
    Network {
        url: String,
        message: String,
    },
    Io {
        path: String,
        message: String,
    },
    Json {
        url: String,
        message: String,
    },
}

impl std::fmt::Display for DownloadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DownloadError::HttpStatus { url, code } => write!(f, "HTTP {code}: {url}"),
            DownloadError::Timeout { url, phase } => {
                write!(f, "下载超时（{phase:?}）: {url}")
            }
            DownloadError::Network { url, message } => write!(f, "网络错误: {url}: {message}"),
            DownloadError::Io { path, message } => write!(f, "写盘失败: {path}: {message}"),
            DownloadError::Json { url, message } => write!(f, "JSON 解析错误: {url}: {message}"),
        }
    }
}

impl std::error::Error for DownloadError {}

pub struct DownloadManager {
    client: reqwest::Client,
    semaphore: Arc<Semaphore>,
    max_retries: u32,
    retry_delay: Duration,
    file_first_byte_timeout: Duration,
    text_first_byte_timeout: Duration,
    stall_timeout: Duration,
}

impl DownloadManager {
    /// 默认参数的全局实例（对齐 C++ 单例）。reqwest Client 内部连接池全局复用。
    pub fn shared() -> &'static DownloadManager {
        static SHARED: OnceLock<DownloadManager> = OnceLock::new();
        SHARED.get_or_init(DownloadManager::new)
    }

    pub fn new() -> Self {
        DownloadManager::builder().build()
    }

    pub fn builder() -> DownloadManagerBuilder {
        DownloadManagerBuilder::default()
    }
    /// 下载文件到本地路径（重试 + forgecdn 回退 + 失败清理半成品）。
    /// 返回写入的字节数。并发超限时在此排队（对齐 C++）。
    pub async fn download_file(
        &self,
        url: &str,
        save_path: &Path,
        on_progress: Option<ProgressCallback>,
    ) -> Result<u64, DownloadError> {
        let _permit =
            self.semaphore
                .clone()
                .acquire_owned()
                .await
                .map_err(|_| DownloadError::Network {
                    url: url.to_string(),
                    message: "下载管理器已关闭".into(),
                })?;

        let mut result = self
            .fetch_file_with_retries(url, save_path, self.max_retries, on_progress.clone())
            .await;
        if result.is_err() {
            // C++：forgecdn 文件 CDN 最终失败 → MCIM 代理，带 1 次重试
            if let Some(fallback) = forgecdn_fallback(url) {
                tracing::info!("forgecdn 失败，回退 MCIM 镜像: {fallback}");
                result = self
                    .fetch_file_with_retries(&fallback, save_path, 1, on_progress)
                    .await;
            }
        }
        if result.is_err() {
            // 不留半成品
            let _ = tokio::fs::remove_file(save_path).await;
        }
        result
    }

    /// 下载文本（API JSON 等），返回 (HTTP 状态码, 文本)。
    /// 失败时错误携带状态码（对齐 C++ downloadToStringWithStatus 的降级通道）。
    pub async fn download_text(
        &self,
        url: &str,
        headers: &[(String, String)],
    ) -> Result<(u16, String), DownloadError> {
        let _permit =
            self.semaphore
                .clone()
                .acquire_owned()
                .await
                .map_err(|_| DownloadError::Network {
                    url: url.to_string(),
                    message: "下载管理器已关闭".into(),
                })?;
        let mut retries = self.max_retries;
        loop {
            match self.attempt_text(url, headers).await {
                Ok(v) => return Ok(v),
                Err(e) if retries > 0 => {
                    retries -= 1;
                    tracing::warn!("请求失败，重试（剩 {retries} 次）: {url}: {e}");
                    tokio::time::sleep(self.retry_delay).await;
                }
                Err(e) => return Err(e),
            }
        }
    }

    /// 下载并解析 JSON。JSON 解析失败不重试（对齐 C++：解析发生在下载成功之后）。
    pub async fn download_json(
        &self,
        url: &str,
        headers: &[(String, String)],
    ) -> Result<(u16, Value), DownloadError> {
        let (code, text) = self.download_text(url, headers).await?;
        serde_json::from_str(&text)
            .map(|v| (code, v))
            .map_err(|e| DownloadError::Json {
                url: url.to_string(),
                message: e.to_string(),
            })
    }

    async fn fetch_file_with_retries(
        &self,
        url: &str,
        save_path: &Path,
        mut retries: u32,
        on_progress: Option<ProgressCallback>,
    ) -> Result<u64, DownloadError> {
        loop {
            match self.attempt_file(url, save_path, on_progress.clone()).await {
                Ok(n) => return Ok(n),
                Err(e) if retries > 0 => {
                    retries -= 1;
                    tracing::warn!("下载失败，重试（剩 {retries} 次）: {url}: {e}");
                    tokio::time::sleep(self.retry_delay).await;
                }
                Err(e) => return Err(e),
            }
        }
    }

    async fn attempt_file(
        &self,
        url: &str,
        save_path: &Path,
        on_progress: Option<ProgressCallback>,
    ) -> Result<u64, DownloadError> {
        let resp = self.send(url, &[], self.file_first_byte_timeout).await?;
        let code = resp.status().as_u16();
        if code >= 400 {
            return Err(DownloadError::HttpStatus {
                url: url.to_string(),
                code,
            });
        }
        let total = resp.content_length();

        if let Some(dir) = save_path.parent() {
            tokio::fs::create_dir_all(dir)
                .await
                .map_err(|e| DownloadError::Io {
                    path: dir.to_string_lossy().into_owned(),
                    message: e.to_string(),
                })?;
        }
        let mut file = tokio::fs::File::create(save_path)
            .await
            .map_err(|e| DownloadError::Io {
                path: save_path.to_string_lossy().into_owned(),
                message: e.to_string(),
            })?;

        use futures_util::StreamExt;
        let mut stream = resp.bytes_stream();
        let mut received: u64 = 0;
        loop {
            let chunk = match tokio::time::timeout(self.stall_timeout, stream.next()).await {
                Ok(Some(chunk)) => chunk.map_err(|e| DownloadError::Network {
                    url: url.to_string(),
                    message: e.to_string(),
                })?,
                Ok(None) => break,
                Err(_) => {
                    return Err(DownloadError::Timeout {
                        url: url.to_string(),
                        phase: TimeoutPhase::Stall,
                    });
                }
            };
            file.write_all(&chunk)
                .await
                .map_err(|e| DownloadError::Io {
                    path: save_path.to_string_lossy().into_owned(),
                    message: e.to_string(),
                })?;
            received += chunk.len() as u64;
            if let Some(cb) = &on_progress {
                cb(received, total.unwrap_or(0));
            }
        }
        file.flush().await.map_err(|e| DownloadError::Io {
            path: save_path.to_string_lossy().into_owned(),
            message: e.to_string(),
        })?;
        tracing::info!(
            "下载完成: {url} -> {} ({received} 字节)",
            save_path.display()
        );
        Ok(received)
    }

    async fn attempt_text(
        &self,
        url: &str,
        headers: &[(String, String)],
    ) -> Result<(u16, String), DownloadError> {
        let resp = self
            .send(url, headers, self.text_first_byte_timeout)
            .await?;
        let code = resp.status().as_u16();
        if code >= 400 {
            return Err(DownloadError::HttpStatus {
                url: url.to_string(),
                code,
            });
        }
        let body = match tokio::time::timeout(self.stall_timeout, resp.bytes()).await {
            Ok(bytes) => bytes.map_err(|e| DownloadError::Network {
                url: url.to_string(),
                message: e.to_string(),
            })?,
            Err(_) => {
                return Err(DownloadError::Timeout {
                    url: url.to_string(),
                    phase: TimeoutPhase::Stall,
                });
            }
        };
        Ok((code, String::from_utf8_lossy(&body).into_owned()))
    }

    async fn send(
        &self,
        url: &str,
        headers: &[(String, String)],
        first_byte_timeout: Duration,
    ) -> Result<reqwest::Response, DownloadError> {
        let mut request = self.client.get(url);
        let mut header_map = HeaderMap::new();
        for (k, v) in headers {
            header_map.insert(
                reqwest::header::HeaderName::from_bytes(k.as_bytes()).map_err(|e| {
                    DownloadError::Network {
                        url: url.to_string(),
                        message: format!("非法请求头 {k}: {e}"),
                    }
                })?,
                reqwest::header::HeaderValue::from_str(v).map_err(|e| DownloadError::Network {
                    url: url.to_string(),
                    message: format!("非法请求头值 {k}: {e}"),
                })?,
            );
        }
        request = request.headers(header_map);

        // 首字节超时：覆盖连接 + 响应头阶段（对齐 C++ 两阶段超时的第一段）
        match tokio::time::timeout(first_byte_timeout, request.send()).await {
            Ok(Ok(resp)) => Ok(resp),
            Ok(Err(e)) => Err(DownloadError::Network {
                url: url.to_string(),
                message: e.to_string(),
            }),
            Err(_) => Err(DownloadError::Timeout {
                url: url.to_string(),
                phase: TimeoutPhase::FirstByte,
            }),
        }
    }
}

impl Default for DownloadManager {
    fn default() -> Self {
        Self::new()
    }
}

/// forgecdn 文件 CDN 回退：主机为 edge.forgecdn.net 时，同路径换主机到 MCIM 代理
/// （对齐 C++ downloadInternal 的降级分支；其余 URL 不回退）。
pub fn forgecdn_fallback(url: &str) -> Option<String> {
    let parsed = Url::parse(url).ok()?;
    if parsed.host_str() == Some("edge.forgecdn.net") {
        Some(url.replacen("edge.forgecdn.net", "mod.mcimirror.top", 1))
    } else {
        None
    }
}

// ---- Mojang URL 辅助（对齐 C++ versionManifestUrl/versionJsonUrl/assetsIndexUrl） ----

/// 版本清单 URL
pub fn version_manifest_url() -> &'static str {
    "https://launchermeta.mojang.com/mc/game/version_manifest.json"
}

/// 版本 JSON 回退 URL：sha1(versionId) 十六进制前 2 位 + `/` + `{id}.json`
pub fn version_json_url(version_id: &str) -> String {
    use sha1::Digest;
    let mut hasher = sha1::Sha1::new();
    hasher.update(version_id.as_bytes());
    let hex: String = hasher
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    format!(
        "https://launchermeta.mojang.com/v1/packages/{}/{}.json",
        &hex[..2],
        version_id
    )
}

/// 资产索引回退 URL
pub fn assets_index_url(assets_version: &str) -> String {
    format!("https://launchermeta.mojang.com/v1/packages/{assets_version}.json")
}

// ---- 构建器（超时/重试可调，测试用短超时） ----

#[derive(Default)]
pub struct DownloadManagerBuilder {
    max_in_flight: Option<usize>,
    max_retries: Option<u32>,
    retry_delay: Option<Duration>,
    file_first_byte_timeout: Option<Duration>,
    text_first_byte_timeout: Option<Duration>,
    stall_timeout: Option<Duration>,
}

impl DownloadManagerBuilder {
    pub fn max_in_flight(mut self, n: usize) -> Self {
        self.max_in_flight = Some(n);
        self
    }
    pub fn max_retries(mut self, n: u32) -> Self {
        self.max_retries = Some(n);
        self
    }
    pub fn retry_delay(mut self, d: Duration) -> Self {
        self.retry_delay = Some(d);
        self
    }
    pub fn timeouts(
        mut self,
        file_first_byte: Duration,
        text_first_byte: Duration,
        stall: Duration,
    ) -> Self {
        self.file_first_byte_timeout = Some(file_first_byte);
        self.text_first_byte_timeout = Some(text_first_byte);
        self.stall_timeout = Some(stall);
        self
    }

    pub fn build(self) -> DownloadManager {
        // UA、HTTP/1.1、重定向策略在此定格（对齐 C++ 请求属性）
        let client = reqwest::Client::builder()
            .user_agent(USER_AGENT)
            .http1_only()
            .redirect({
                use reqwest::redirect::Policy;
                Policy::custom(|attempt| {
                    if attempt.previous().len() >= 10 {
                        attempt.stop()
                    } else if let Some(prev) = attempt.previous().last() {
                        if prev.scheme() == "https" && attempt.url().scheme() == "http" {
                            // https→http 降级禁止（NoLessSafe）
                            attempt.stop()
                        } else {
                            attempt.follow()
                        }
                    } else {
                        attempt.follow()
                    }
                })
            })
            .build()
            .expect("reqwest client 构建失败");
        DownloadManager {
            client,
            semaphore: Arc::new(Semaphore::new(self.max_in_flight.unwrap_or(MAX_IN_FLIGHT))),
            max_retries: self.max_retries.unwrap_or(MAX_RETRIES),
            retry_delay: self.retry_delay.unwrap_or(RETRY_DELAY),
            file_first_byte_timeout: self
                .file_first_byte_timeout
                .unwrap_or(FILE_FIRST_BYTE_TIMEOUT),
            text_first_byte_timeout: self
                .text_first_byte_timeout
                .unwrap_or(TEXT_FIRST_BYTE_TIMEOUT),
            stall_timeout: self.stall_timeout.unwrap_or(STALL_TIMEOUT),
        }
    }
}
