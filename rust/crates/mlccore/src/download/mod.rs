//! 下载层，对应 C++ `sdk/src/download/`。三个子模块：
//! - [`manager`]：HTTP 引擎（并发限流、两阶段超时、重试、forgecdn→MCIM 回退）
//! - [`modplatform`]：CF/Modrinth API 与镜像链
//! - [`assets`]：原版版本下载管线（manifest→json→jar→libs→assets→natives）

pub mod assets;
pub mod manager;
pub mod modplatform;

pub use assets::{
    rules_allow_this_platform, AssetDownloader, Stage, StageCallback, VersionContext,
};
pub use manager::{
    assets_index_url, forgecdn_fallback, version_json_url, version_manifest_url, DownloadError,
    DownloadManager, ProgressCallback, TimeoutPhase, MAX_IN_FLIGHT,
};
pub use modplatform::{
    cf_class_id_for, mr_project_type_for, CfKeySource, ModFileInfo, ModPlatform, ModResource,
    Platform, ResourceType, CF_API, CF_MIRROR, EMBEDDED_CF_KEY, MR_API,
};
