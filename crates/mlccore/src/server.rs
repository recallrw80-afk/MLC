//! 本地开服，对应 C++ `mlc::installServer` / `startServer`：
//! 原版 server.jar 或 Forge/NeoForge/Fabric 服务端；EULA 闸门；前台控制台直通。

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::download::manager::{version_manifest_url, DownloadManager};
use crate::settings::Settings;
use crate::util::file;
use crate::util::platform;
use crate::version::{McVersion, McVersionNumber};

/// 服务端目录 `{mc}/servers/<id>/`
pub fn server_dir(mc_folder: &Path, server_id: &str) -> PathBuf {
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    PathBuf::from(format!("{mc}/servers/{server_id}"))
}

/// 服务端标识：版本 或 版本-加载器-加载器版本
pub fn server_id(mc_version: &str, loader_type: &str, loader_ver: &str) -> String {
    if loader_type.is_empty() {
        mc_version.to_string()
    } else {
        format!("{mc_version}-{loader_type}-{loader_ver}")
    }
}

async fn resolve_version_json_url(
    dlm: &DownloadManager,
    version_id: &str,
) -> Result<(String, String), String> {
    let (_, manifest) = dlm
        .download_json(version_manifest_url(), &[])
        .await
        .map_err(|e| e.to_string())?;
    let mut id = version_id.to_string();
    if id.is_empty() {
        id = manifest
            .pointer("/latest/release")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
    }
    if id.is_empty() {
        return Err("无法解析最新正式版".into());
    }
    let url = manifest
        .get("versions")
        .and_then(|v| v.as_array())
        .and_then(|arr| {
            arr.iter()
                .find(|v| v.get("id").and_then(|x| x.as_str()) == Some(id.as_str()))
                .and_then(|v| v.get("url"))
                .and_then(|v| v.as_str())
                .map(|s| s.to_string())
        })
        .ok_or_else(|| format!("版本不在清单中: {id}"))?;
    Ok((id, url))
}

/// Fabric 最新 loader（meta API，stable 优先）
pub async fn detect_best_fabric_version(
    dlm: &DownloadManager,
    mc_version: &str,
) -> Result<String, String> {
    let url = format!(
        "https://meta.fabricmc.net/v2/versions/loader/{}",
        mc_version
    );
    let (_, list) = dlm
        .download_json(&url, &[])
        .await
        .map_err(|e| e.to_string())?;
    let arr = list.as_array().ok_or("Fabric loader 列表非法")?;
    for e in arr {
        if e.get("stable").and_then(|v| v.as_bool()).unwrap_or(false) {
            if let Some(v) = e.pointer("/loader/version").and_then(|v| v.as_str()) {
                return Ok(v.to_string());
            }
        }
    }
    arr.first()
        .and_then(|e| e.pointer("/loader/version"))
        .and_then(|v| v.as_str())
        .map(|s| s.to_string())
        .ok_or_else(|| "无法解析 Fabric loader 版本".to_string())
}

fn pick_java(mc_folder: &Path, mc_version: &str) -> Result<String, String> {
    let javas = crate::java::scan_system_java(mc_folder);
    let mut probe = McVersion {
        id: mc_version.to_string(),
        is_valid: true,
        vanilla_version: McVersionNumber::parse(&crate::modpack::extract_vanilla_version(
            mc_version,
        )),
        ..Default::default()
    };
    let _ = &mut probe;
    crate::java::select_java_for_version(&javas, &probe)
        .or_else(|| javas.first().cloned())
        .map(|j| j.path_java)
        .filter(|p| !p.is_empty())
        .ok_or_else(|| "未检测到 Java 运行时".to_string())
}

/// 安装服务端（对齐 installServer）
pub async fn install_server(
    dlm: &DownloadManager,
    mc_folder: &Path,
    version_id: &str,
    loader_type: &str,
    loader_version: &str,
) -> Result<String, String> {
    let (id, _json_url) = resolve_version_json_url(dlm, version_id).await?;

    if !loader_type.is_empty() {
        let mut lv = loader_version.to_string();
        if lv.is_empty() {
            lv = if loader_type == "fabric" {
                detect_best_fabric_version(dlm, &id).await?
            } else {
                // Forge/NeoForge 空版本：暂不自动探测最新，要求显式版本
                return Err(format!("{loader_type} 请显式指定版本，如 --forge 47.2.0"));
            };
        }
        let sid = server_id(&id, loader_type, &lv);
        let dir = server_dir(mc_folder, &sid);
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let java = pick_java(mc_folder, &id)?;
        crate::installer::install_loader_server(dlm, loader_type, &dir, &id, &lv, &java).await?;
        return Ok(sid);
    }

    // 原版：版本 json → downloads.server
    let (_, vj) = dlm
        .download_json(&_json_url, &[])
        .await
        .map_err(|e| e.to_string())?;
    let srv = vj
        .pointer("/downloads/server")
        .ok_or("该版本没有官方服务端 jar")?;
    let url = srv.get("url").and_then(|v| v.as_str()).unwrap_or("");
    let sha1 = srv.get("sha1").and_then(|v| v.as_str()).unwrap_or("");
    if url.is_empty() {
        return Err("server 下载 URL 为空".into());
    }
    let dir = server_dir(mc_folder, &id);
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let jar = dir.join("server.jar");
    if jar.exists() && !sha1.is_empty() && file::verify_sha1(&jar, sha1) {
        return Ok(id);
    }
    dlm.download_file(url, &jar, None).await.map_err(|e| {
        let _ = std::fs::remove_file(&jar);
        e.to_string()
    })?;
    if !sha1.is_empty() && !file::verify_sha1(&jar, sha1) {
        let _ = std::fs::remove_file(&jar);
        return Err("server.jar SHA1 校验失败".into());
    }
    Ok(id)
}

/// EULA 已接受？
pub fn eula_accepted(dir: &Path) -> bool {
    std::fs::read_to_string(dir.join("eula.txt"))
        .map(|t| t.contains("eula=true"))
        .unwrap_or(false)
}

/// 写入 eula=true
pub fn accept_eula(dir: &Path) -> Result<(), String> {
    std::fs::write(dir.join("eula.txt"), "eula=true\n").map_err(|e| e.to_string())
}

/// 把实例的 mods/config/defaultconfigs 复制进服务端目录
pub fn copy_instance_to_server(
    mc_folder: &Path,
    instance_name: &str,
    server_id: &str,
    settings: &Settings,
) -> Result<(), String> {
    let dir_name = settings
        .dir_for_display_name(instance_name)
        .filter(|d| !d.is_empty())
        .ok_or_else(|| format!("实例 {instance_name} 不存在"))?;
    let mc = platform::normalize_path_string(mc_folder);
    let mc = mc.trim_end_matches('/');
    let inst = PathBuf::from(format!("{mc}/instances/{dir_name}"));
    let dst = server_dir(mc_folder, server_id);
    for sub in ["mods", "config", "defaultconfigs"] {
        let src = inst.join(sub);
        if src.is_dir() {
            println!("复制 {sub} ...");
            if !file::copy_dir(&src, &dst.join(sub)) {
                return Err(format!("复制 {sub} 失败"));
            }
        }
    }
    Ok(())
}

fn find_unix_args(dir: &Path) -> Option<PathBuf> {
    let libs = dir.join("libraries");
    let mut stack = vec![libs];
    while let Some(d) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&d) else {
            continue;
        };
        for e in entries.flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.file_name().map(|n| n == "unix_args.txt").unwrap_or(false) {
                return Some(p);
            }
        }
    }
    None
}

fn find_server_jar(dir: &Path) -> Option<PathBuf> {
    if let Ok(entries) = std::fs::read_dir(dir) {
        for e in entries.flatten() {
            let n = e.file_name().to_string_lossy().to_string();
            if n.ends_with("-universal.jar") && n.starts_with("forge-") {
                return Some(e.path());
            }
            if n == "fabric-server-launch.jar" {
                return Some(e.path());
            }
            if n == "server.jar" {
                return Some(e.path());
            }
        }
    }
    None
}

/// 启动服务端（对齐 startServer）。EULA 须已接受。
/// 返回子进程 exit code。
pub fn start_server(mc_folder: &Path, server_id: &str, settings: &Settings) -> Result<i32, String> {
    let dir = server_dir(mc_folder, server_id);
    if !dir.is_dir() {
        return Err(format!("服务端未安装，请先 mlc server-install {server_id}"));
    }
    if !eula_accepted(&dir) {
        return Err("EULA 未同意。请先阅读 https://aka.ms/MinecraftEULA，然后传 --eula".into());
    }

    // 首启写 online-mode=false（不覆盖已有）
    let props = dir.join("server.properties");
    if !props.exists() {
        let _ = std::fs::write(&props, "online-mode=false\n");
    }

    // Java：标识可能带 loader 后缀，取第一段 MC 版本
    let mc_part = server_id.split('-').next().unwrap_or(server_id);
    let java = pick_java(mc_folder, mc_part)?;

    let max_mem = settings
        .get_string("LaunchMaxMemory")
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    let max_mem = if max_mem <= 0 { 2048 } else { max_mem };
    let mem_args = [format!("-Xmx{max_mem}M"), "-Xms512M".to_string()];

    let mut args: Vec<String> = Vec::new();
    if let Some(unix_args) = find_unix_args(&dir) {
        args.extend(mem_args.iter().cloned());
        let user_args = dir.join("user_jvm_args.txt");
        if user_args.exists() {
            args.push(format!("@{}", user_args.display()));
        }
        args.push(format!("@{}", unix_args.display()));
        args.push("nogui".into());
    } else {
        let Some(jar) = find_server_jar(&dir) else {
            return Err("找不到可启动的 server jar".into());
        };
        args.extend(mem_args.iter().cloned());
        args.push("-jar".into());
        args.push(jar.to_string_lossy().to_string());
        args.push("nogui".into());
    }

    tracing::info!("Starting server: {java} {}", args.join(" "));
    let mut child = Command::new(&java)
        .args(&args)
        .current_dir(&dir)
        .stdin(Stdio::inherit())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|e| format!("Failed to start server: {e}"))?;
    let status = child.wait().map_err(|e| e.to_string())?;
    Ok(status.code().unwrap_or(-1))
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 服务端标识与目录() {
        assert_eq!(server_id("1.20.1", "", ""), "1.20.1");
        assert_eq!(
            server_id("1.20.1", "forge", "47.2.0"),
            "1.20.1-forge-47.2.0"
        );
        let d = server_dir(Path::new("/mc"), "1.20.1");
        let s = d.to_string_lossy().replace('\\', "/");
        assert!(s.ends_with("/mc/servers/1.20.1"));
    }

    #[test]
    fn eula检查() {
        let dir = std::env::temp_dir().join(format!("mlc-eula-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        assert!(!eula_accepted(&dir));
        accept_eula(&dir).unwrap();
        assert!(eula_accepted(&dir));
    }

    #[test]
    fn 识别server_jar布局() {
        let dir = std::env::temp_dir().join(format!("mlc-srvjar-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("server.jar"), b"x").unwrap();
        assert!(find_server_jar(&dir).is_some());
        let _ = std::fs::remove_file(dir.join("server.jar"));
        std::fs::write(dir.join("forge-1.20.1-47.2.0-universal.jar"), b"x").unwrap();
        let j = find_server_jar(&dir).unwrap();
        assert!(j.to_string_lossy().contains("universal"));
    }
}
