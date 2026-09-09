// 构建期注入版本信息，语义对齐原 CMake 的 git describe 注入（见根 CMakeLists.txt）
use std::process::Command;

fn git(args: &[&str]) -> Option<String> {
    let out = Command::new("git").args(args).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if s.is_empty() {
        None
    } else {
        Some(s)
    }
}

fn main() {
    // 仓库根在 rust/ 的上一级
    let describe = git(&["-C", "..", "describe", "--tags", "--always"])
        .unwrap_or_else(|| "v0.1-unknown".to_string());
    let commit =
        git(&["-C", "..", "rev-parse", "--short=7", "HEAD"]).unwrap_or_else(|| "0000000".into());

    println!("cargo:rustc-env=GIT_DESCRIBE={describe}");
    println!("cargo:rustc-env=GIT_COMMIT_HASH={commit}");
    // git HEAD 变化时重跑（tag 变化靠 describe 输出变化自然触发）
    println!("cargo:rerun-if-changed=../.git/HEAD");
    println!("cargo:rerun-if-changed=../.git/refs");
}
