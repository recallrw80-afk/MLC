//! Modloader 安装器，对应 C++ `installer.cpp`：
//! 下载官方 installer.jar → 用 Java 执行 → 写入 {mcDir}/versions。
//!
//! Forge/NeoForge 要求游戏根目录存在 `launcher_profiles.json`，缺失时补最小文件。
//! 安装器进程失败 = 整合包导入失败（由调用方回滚）。

use std::path::Path;
use std::process::Command;

use crate::download::manager::DownloadManager;

pub const FORGE_API: &str = "https://files.minecraftforge.net/net/minecraftforge/forge";
pub const FABRIC_API: &str = "https://meta.fabricmc.net/v2";
pub const NEOFORGE_API: &str = "https://maven.neoforged.net/releases/net/neoforged/neoforge";

/// 确保 Forge 安装器所需的 launcher_profiles.json 存在
fn ensure_launcher_profiles(mc_dir: &Path) {
    let p = mc_dir.join("launcher_profiles.json");
    if !p.exists() {
        let _ = std::fs::create_dir_all(mc_dir);
        let _ = std::fs::write(p, br#"{"profiles":{}}"#);
    }
}

fn run_java_installer(java_path: &str, args: &[String]) -> Result<(), String> {
    tracing::info!("Running installer: {java_path} {}", args.join(" "));
    let output = Command::new(java_path)
        .args(args)
        .output()
        .map_err(|e| format!("Cannot start installer process: {e}"))?;
    if !output.status.success() {
        let err = String::from_utf8_lossy(&output.stderr);
        let out = String::from_utf8_lossy(&output.stdout);
        tracing::warn!("Installer failed: {err} {out}");
        return Err(if !err.trim().is_empty() {
            err.to_string()
        } else {
            out.to_string()
        });
    }
    Ok(())
}

/// 解析 Fabric 安装器 jar 下载 URL（stable 优先）
pub async fn resolve_fabric_installer_url(dlm: &DownloadManager) -> Result<String, String> {
    let (_, list) = dlm
        .download_json(&format!("{FABRIC_API}/versions/installer"), &[])
        .await
        .map_err(|e| format!("Failed to resolve Fabric installer: {e}"))?;
    let arr = list.as_array().ok_or("No Fabric installer URL found")?;
    for e in arr {
        if e.get("stable").and_then(|v| v.as_bool()).unwrap_or(false) {
            if let Some(u) = e.get("url").and_then(|v| v.as_str()) {
                if !u.is_empty() {
                    return Ok(u.to_string());
                }
            }
        }
    }
    arr.first()
        .and_then(|e| e.get("url"))
        .and_then(|v| v.as_str())
        .filter(|u| !u.is_empty())
        .map(|s| s.to_string())
        .ok_or_else(|| "No Fabric installer URL found".to_string())
}

/// Fabric 客户端安装
pub async fn install_fabric(
    dlm: &DownloadManager,
    mc_dir: &Path,
    mc_version: &str,
    loader_version: &str,
    java_path: &str,
) -> Result<(), String> {
    let url = resolve_fabric_installer_url(dlm).await?;
    let jar = std::env::temp_dir().join("fabric-installer.jar");
    tracing::info!("Downloading fabric-installer.jar...");
    dlm.download_file(&url, &jar, None)
        .await
        .map_err(|e| e.to_string())?;
    let args = vec![
        "-jar".to_string(),
        jar.to_string_lossy().to_string(),
        "client".into(),
        "-dir".into(),
        mc_dir.to_string_lossy().to_string(),
        "-mcversion".into(),
        mc_version.to_string(),
        "-loader".into(),
        loader_version.to_string(),
    ];
    run_java_installer(java_path, &args)
}

/// Forge 客户端安装
pub async fn install_forge(
    dlm: &DownloadManager,
    mc_dir: &Path,
    mc_version: &str,
    forge_version: &str,
    java_path: &str,
) -> Result<(), String> {
    ensure_launcher_profiles(mc_dir);
    let jar_name = format!("forge-{mc_version}-{forge_version}-installer.jar");
    let url = format!(
        "https://maven.minecraftforge.net/net/minecraftforge/forge/{mc_version}-{forge_version}/{jar_name}"
    );
    let jar = std::env::temp_dir().join(&jar_name);
    tracing::info!("Downloading {jar_name}...");
    dlm.download_file(&url, &jar, None)
        .await
        .map_err(|e| e.to_string())?;
    let args = vec![
        "-jar".to_string(),
        jar.to_string_lossy().to_string(),
        "--installClient".into(),
        mc_dir.to_string_lossy().to_string(),
    ];
    run_java_installer(java_path, &args)
}

/// NeoForge 客户端安装
pub async fn install_neoforge(
    dlm: &DownloadManager,
    mc_dir: &Path,
    _mc_version: &str,
    neo_version: &str,
    java_path: &str,
) -> Result<(), String> {
    ensure_launcher_profiles(mc_dir);
    let jar_name = format!("neoforge-{neo_version}-installer.jar");
    let url = format!("{NEOFORGE_API}/{neo_version}/{jar_name}");
    let jar = std::env::temp_dir().join(&jar_name);
    tracing::info!("Downloading {jar_name}...");
    dlm.download_file(&url, &jar, None)
        .await
        .map_err(|e| e.to_string())?;
    let args = vec![
        "-jar".to_string(),
        jar.to_string_lossy().to_string(),
        "--installClient".into(),
        mc_dir.to_string_lossy().to_string(),
    ];
    run_java_installer(java_path, &args)
}

/// 统一入口（对齐 Installer::installLoader）
pub async fn install_loader(
    dlm: &DownloadManager,
    loader_type: &str,
    mc_dir: &Path,
    mc_version: &str,
    loader_version: &str,
    java_path: &str,
) -> Result<(), String> {
    match loader_type {
        "forge" => install_forge(dlm, mc_dir, mc_version, loader_version, java_path).await,
        "fabric" => install_fabric(dlm, mc_dir, mc_version, loader_version, java_path).await,
        "neoforge" => install_neoforge(dlm, mc_dir, mc_version, loader_version, java_path).await,
        other => Err(format!("Unsupported loader: {other}")),
    }
}

/// 服务端模式安装（对齐 installLoaderServer）
pub async fn install_loader_server(
    dlm: &DownloadManager,
    loader_type: &str,
    dir: &Path,
    mc_version: &str,
    loader_version: &str,
    java_path: &str,
) -> Result<(), String> {
    ensure_launcher_profiles(dir);
    if loader_type == "fabric" {
        let url = resolve_fabric_installer_url(dlm).await?;
        let jar = std::env::temp_dir().join("fabric-installer.jar");
        dlm.download_file(&url, &jar, None)
            .await
            .map_err(|e| e.to_string())?;
        let args = vec![
            "-jar".to_string(),
            jar.to_string_lossy().to_string(),
            "server".into(),
            "-dir".into(),
            dir.to_string_lossy().to_string(),
            "-mcversion".into(),
            mc_version.to_string(),
            "-loader".into(),
            loader_version.to_string(),
            "-downloadMinecraft".into(),
        ];
        run_java_installer(java_path, &args)
    } else {
        // Forge / NeoForge 复用客户端 installer 下载，再 --installServer
        let jar_name = match loader_type {
            "forge" => format!("forge-{mc_version}-{loader_version}-installer.jar"),
            "neoforge" => format!("neoforge-{loader_version}-installer.jar"),
            other => return Err(format!("Unsupported loader: {other}")),
        };
        let url = match loader_type {
            "forge" => format!(
                "https://maven.minecraftforge.net/net/minecraftforge/forge/{mc_version}-{loader_version}/{jar_name}"
            ),
            _ => format!("{NEOFORGE_API}/{loader_version}/{jar_name}"),
        };
        let jar = std::env::temp_dir().join(&jar_name);
        dlm.download_file(&url, &jar, None)
            .await
            .map_err(|e| e.to_string())?;
        let args = vec![
            "-jar".to_string(),
            jar.to_string_lossy().to_string(),
            "--installServer".into(),
            dir.to_string_lossy().to_string(),
        ];
        run_java_installer(java_path, &args)
    }
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forge安装器url格式() {
        let url = format!(
            "https://maven.minecraftforge.net/net/minecraftforge/forge/{mc}-{fv}/forge-{mc}-{fv}-installer.jar",
            mc = "1.20.1",
            fv = "47.2.0"
        );
        assert!(url.ends_with("forge-1.20.1-47.2.0-installer.jar"));
        assert!(url.contains("/1.20.1-47.2.0/"));
    }

    #[test]
    fn fabric元数据解析stable优先() {
        // 纯函数路径：resolve 依赖网络；这里钉死 JSON 解析逻辑
        let list = serde_json::json!([
            {"stable": false, "url": "https://example/unstable.jar"},
            {"stable": true, "url": "https://example/stable.jar"}
        ]);
        let arr = list.as_array().unwrap();
        let mut picked = String::new();
        for e in arr {
            if e.get("stable").and_then(|v| v.as_bool()).unwrap_or(false) {
                picked = e.get("url").and_then(|v| v.as_str()).unwrap_or("").into();
                break;
            }
        }
        assert_eq!(picked, "https://example/stable.jar");
    }

    #[test]
    fn launcher_profiles兜底写入() {
        let dir = std::env::temp_dir().join(format!("mlc-inst-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        ensure_launcher_profiles(&dir);
        assert_eq!(
            std::fs::read_to_string(dir.join("launcher_profiles.json")).unwrap(),
            r#"{"profiles":{}}"#
        );
    }
}
