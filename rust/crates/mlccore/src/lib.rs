//! mlccore — MLC 启动器核心库（Rust 重写版）
//!
//! 对应原 C++ `sdk/`（libmlccore）。模块按 [docs/rust-rewrite-plan.md](../../docs/rust-rewrite-plan.md)
//! 阶段 1 的垂直切片组织；迁移期硬约束（命令表面/磁盘格式/加密字节兼容）见同文档。
//!
//! 公有 API 表见 [docs/rust-inventory.md](../../docs/rust-inventory.md) 表 2。
//!
//! 已落地：util（crypto/file/zip/tar.gz/tar.xz/ini/platform/args）、settings、download、
//! version（含 install/remove）、java（探测/安装）、launch、auth、installer（forge/fabric/neoforge）、
//! modpack（detect/common/清单解析/Compressed/Mod/CF + loader 管线）、update（更新/卸载）、
//! server（本地开服）。Microsoft OAuth 单独 spike。
//! CLI 已覆盖 inventory 表 1 的 30 个命令；`mlccore-ffi` 覆盖表 2 同步 + 异步 API
//! （import/launch/server），待 GUI bridge 接线与 cbindgen 进 CMake。

pub mod auth;
pub mod download;
pub mod installer;
pub mod java;
pub mod launch;
pub mod modpack;
pub mod server;
pub mod settings;
pub mod update;
pub mod util;
pub mod version;

/// 构建期注入的版本号（git describe，如 v0.1.3）
pub const GIT_DESCRIBE: &str = env!("GIT_DESCRIBE");
/// 构建期注入的短 commit hash
pub const GIT_COMMIT_HASH: &str = env!("GIT_COMMIT_HASH");
