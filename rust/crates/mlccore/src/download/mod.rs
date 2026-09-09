//! 下载层，对应 C++ `sdk/src/download/`。三个子模块：
//! - [`manager`]：HTTP 引擎（并发限流、两阶段超时、重试、forgecdn→MCIM 回退）
//! - `modplatform`：CF/Modrinth API 与镜像链（后续切片）
//! - `assets`：原版版本下载管线（后续切片）

pub mod manager;

pub use manager::{
    assets_index_url, forgecdn_fallback, version_json_url, version_manifest_url, DownloadError,
    DownloadManager, ProgressCallback, TimeoutPhase, MAX_IN_FLIGHT,
};
