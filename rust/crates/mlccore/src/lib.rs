//! mlccore — MLC 启动器核心库（Rust 重写版）
//!
//! 对应原 C++ `sdk/`（libmlccore）。模块按 [docs/rust-rewrite-plan.md](../../docs/rust-rewrite-plan.md)
//! 阶段 1 的垂直切片组织；迁移期硬约束（命令表面/磁盘格式/加密字节兼容）见同文档。
//!
//! 公有 API 表见 [docs/rust-inventory.md](../../docs/rust-inventory.md) 表 2。

pub mod auth;
pub mod download;
pub mod java;
pub mod launch;
pub mod modpack;
pub mod settings;
pub mod update;
pub mod util;
pub mod version;

/// 构建期注入的版本号（git describe，如 v0.1.3）
pub const GIT_DESCRIBE: &str = env!("GIT_DESCRIBE");
/// 构建期注入的短 commit hash
pub const GIT_COMMIT_HASH: &str = env!("GIT_COMMIT_HASH");
