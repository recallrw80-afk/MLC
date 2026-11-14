//! 平台抽象，对应 C++ `platform_utils.h`：OS/架构识别、可执行文件旁目录、
//! 标准目录（dirs crate）、PATH 探测（which crate）。

use std::path::{Path, PathBuf};

/// 用户主目录（对应 QDir::homePath()）
pub fn home_dir() -> PathBuf {
    dirs::home_dir().unwrap_or_else(|| PathBuf::from("/"))
}

/// 游戏侧平台名，对齐 C++ platformName()（用于 Minecraft 版本 JSON 的 os.name 匹配）
pub fn platform_name() -> &'static str {
    #[cfg(target_os = "windows")]
    {
        "windows"
    }
    #[cfg(target_os = "linux")]
    {
        "linux"
    }
    #[cfg(target_os = "macos")]
    {
        "osx"
    }
    #[cfg(not(any(target_os = "windows", target_os = "linux", target_os = "macos")))]
    {
        "unknown"
    }
}

/// 是否 64 位（对齐 C++ is64BitSystem()，依据 CPU 架构而非进程位数）
pub fn is_64bit() -> bool {
    cfg!(target_pointer_width = "64")
}

/// 当前 CPU 架构（Adoptium 选包用；对齐 QSysInfo::currentCpuArchitecture 的判断点）
pub fn cpu_arch() -> &'static str {
    #[cfg(target_arch = "x86_64")]
    {
        "x86_64"
    }
    #[cfg(target_arch = "aarch64")]
    {
        "aarch64"
    }
    #[cfg(target_arch = "x86")]
    {
        "x86"
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64", target_arch = "x86")))]
    {
        "unknown"
    }
}

/// PATH 环境变量分隔符（Windows `;`，其余 `:`）
pub fn path_env_sep() -> char {
    if cfg!(target_os = "windows") {
        ';'
    } else {
        ':'
    }
}

/// java 可执行文件名（Windows 带 .exe）
pub fn java_bin_name() -> &'static str {
    if cfg!(target_os = "windows") {
        "java.exe"
    } else {
        "java"
    }
}

/// 可执行文件所在目录（对应 QCoreApplication::applicationDirPath()）
pub fn exe_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// 把路径规范化为正斜杠绝对路径字符串（尾斜杠可选），对齐 QDir::absolutePath() 的用法
pub fn normalize_path_string(path: &Path) -> String {
    let abs = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map(|c| c.join(path))
            .unwrap_or_else(|_| path.to_path_buf())
    };
    abs.to_string_lossy().replace('\\', "/")
}

/// 官方启动器 .minecraft 路径，对齐 VersionManager::loadFolderList
pub fn official_minecraft_folder() -> PathBuf {
    if cfg!(target_os = "windows") {
        std::env::var_os("APPDATA")
            .map(PathBuf::from)
            .unwrap_or_else(home_dir)
    } else if cfg!(target_os = "macos") {
        home_dir()
            .join("Library")
            .join("Application Support")
            .join("minecraft")
    } else {
        home_dir().join(".minecraft")
    }
}
