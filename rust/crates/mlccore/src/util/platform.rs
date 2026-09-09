//! 平台抽象，对应 C++ `platform_utils.h`：OS/架构识别、可执行文件旁目录、
//! 标准目录（dirs crate）、PATH 探测（which crate）。

use std::path::PathBuf;

/// 用户主目录（对应 QDir::homePath()）
pub fn home_dir() -> PathBuf {
    dirs::home_dir().unwrap_or_else(|| PathBuf::from("/"))
}
