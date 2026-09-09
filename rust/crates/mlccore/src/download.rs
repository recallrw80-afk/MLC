//! 下载层，对应 C++ `sdk/src/download/`：downloadmanager（SHA1 校验、断点续传、
//! BMCLAPI/MCIM 镜像回退、CF key 401/403/429 回退 MCIM）、assetdownloader、modplatform。
//! reqwest(rustls) + tokio；进度经 tokio::sync 回调通道上报。
