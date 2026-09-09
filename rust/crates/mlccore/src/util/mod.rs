//! 工具层：对应 C++ `sdk/src/util/`（file_utils / platform_utils / crypto_utils）
//! 与 `arg_utils`（Rust 侧由 clap 覆盖）。

pub mod crypto;
pub mod file;
pub mod ini;
pub mod platform;
