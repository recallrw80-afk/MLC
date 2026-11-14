//! 自更新与卸载，对应 C++ `cli/src/cmd_misc.cpp` 的 update/uninstall：
//! GitHub→Gitee 双源、SemVer 预发布比较、tar.xz 解包、交换式替换与回滚。

use std::path::{Path, PathBuf};

use crate::download::manager::DownloadManager;
use crate::util::file;
use crate::util::platform;

/// install.sh 安装根（`~/.local/lib/mlc`）；非安装副本返回 None
pub fn installed_root() -> Option<PathBuf> {
    let root = platform::home_dir().join(".local").join("lib").join("mlc");
    let app = platform::exe_dir();
    let app_s = platform::normalize_path_string(&app);
    let root_s = platform::normalize_path_string(&root);
    if app_s.starts_with(&root_s) {
        Some(root)
    } else {
        None
    }
}

/// 从 tag 中取数字段 `v?(\d+\.\d+(?:\.\d+)?)`
pub fn numeric_version(tag: &str) -> Vec<i32> {
    let t = tag.strip_prefix('v').unwrap_or(tag);
    let mut out = Vec::new();
    for part in t.split('.') {
        let digits: String = part.chars().take_while(|c| c.is_ascii_digit()).collect();
        if digits.is_empty() {
            break;
        }
        out.push(digits.parse().unwrap_or(0));
        // 若本段后还有非数字（如 1-beta），停止
        let rest = &part[digits.len()..];
        if !rest.is_empty() {
            break;
        }
    }
    out
}

/// tag 的预发布后缀（`-` 之后）；无则空串
pub fn prerelease_suffix(tag: &str) -> &str {
    match tag.find('-') {
        Some(i) => &tag[i + 1..],
        None => "",
    }
}

/// 对齐 C++ 版本比较：数字段更大 → 更新；数字段相同：正式 > 预发布；双预发布按后缀字典序
pub fn has_update(local_tag: &str, remote_tag: &str) -> bool {
    let lv = numeric_version(local_tag);
    let rv = numeric_version(remote_tag);
    let cmp_n = |a: &[i32], b: &[i32]| {
        let n = a.len().max(b.len());
        for i in 0..n {
            let x = a.get(i).copied().unwrap_or(0);
            let y = b.get(i).copied().unwrap_or(0);
            if x != y {
                return x.cmp(&y);
            }
        }
        std::cmp::Ordering::Equal
    };
    match cmp_n(&rv, &lv) {
        std::cmp::Ordering::Greater => true,
        std::cmp::Ordering::Less => false,
        std::cmp::Ordering::Equal => {
            let rs = prerelease_suffix(remote_tag);
            let ls = prerelease_suffix(local_tag);
            (rs.is_empty() && !ls.is_empty()) || (!rs.is_empty() && !ls.is_empty() && rs > ls)
        }
    }
}

/// 从 release JSON 数组挑一项：beta 取最新；否则跳过 prerelease 取首个正式版
pub fn pick_release(list: &[serde_json::Value], beta: bool) -> Option<serde_json::Value> {
    for r in list {
        let Some(obj) = r.as_object() else {
            continue;
        };
        let pre = obj
            .get("prerelease")
            .and_then(|v| v.as_bool())
            .unwrap_or(false);
        if beta || !pre {
            return Some(r.clone());
        }
    }
    None
}

/// 包名：`mlc-linux-<arch>.tar.xz`
pub fn package_name() -> String {
    let arch = if platform::cpu_arch() == "aarch64" {
        "aarch64"
    } else {
        "x86_64"
    };
    format!("mlc-linux-{arch}.tar.xz")
}

/// 从 GitHub release assets 找下载 URL
pub fn asset_url(rel: &serde_json::Value, pkg: &str) -> Option<String> {
    let assets = rel.get("assets")?.as_array()?;
    for a in assets {
        let name = a.get("name").and_then(|v| v.as_str()).unwrap_or("");
        if name == pkg {
            return a
                .get("browser_download_url")
                .and_then(|v| v.as_str())
                .map(|s| s.to_string());
        }
    }
    None
}

/// Gitee 固定路径规则拼下载 URL
pub fn gitee_download_url(gitee_repo: &str, tag: &str, pkg: &str) -> String {
    format!("https://gitee.com/{gitee_repo}/releases/download/{tag}/{pkg}")
}

fn remove_any(path: &Path) {
    let meta = std::fs::symlink_metadata(path);
    if meta.map(|m| m.file_type().is_symlink()).unwrap_or(false) {
        let _ = std::fs::remove_file(path);
    } else if path.is_dir() {
        file::remove_tree(path);
    } else {
        let _ = std::fs::remove_file(path);
    }
}

/// 交换式替换：旧项 → backup，新项 → app；任一步失败回滚已换入项
pub fn swap_replace(
    app_dir: &Path,
    new_dir: &Path,
    backup_dir: &Path,
    items: &[&str],
) -> Result<(), String> {
    let _ = std::fs::create_dir_all(backup_dir);
    let mut swapped: Vec<&str> = Vec::new();
    for &item in items {
        let src = new_dir.join(item);
        if !src.exists() {
            continue;
        }
        let dst = app_dir.join(item);
        let bak = backup_dir.join(item);
        if dst.exists() {
            std::fs::rename(&dst, &bak).map_err(|e| format!("backup {item}: {e}"))?;
        }
        if let Err(e) = std::fs::rename(&src, &dst) {
            if bak.exists() {
                let _ = std::fs::rename(&bak, &dst);
            }
            // 回滚已换入
            for &it in &swapped {
                remove_any(&app_dir.join(it));
                let _ = std::fs::rename(backup_dir.join(it), app_dir.join(it));
            }
            return Err(format!("replace {item}: {e}"));
        }
        swapped.push(item);
    }
    Ok(())
}

/// 检查并执行更新。返回 Ok(tag) 表示已更新到 tag；Ok(当前 tag) 表示已是最新。
/// Err 表示失败（含非 install.sh 副本拒绝更新）。
pub async fn check_and_update(beta: bool) -> Result<String, String> {
    let Some(root) = installed_root() else {
        return Err("当前不是 install.sh 安装副本（开发/分发路径），拒绝更新".into());
    };
    let repo = std::env::var("MLC_REPO").unwrap_or_else(|_| "recallrw80-afk/MLC".into());
    let gitee_repo = std::env::var("MLC_GITEE_REPO").unwrap_or_else(|_| "recall80/mlc".into());
    let local_tag = crate::GIT_DESCRIBE;

    println!("正在检查更新...");
    let dlm = DownloadManager::new();

    let mut use_cn = false;
    let rel = loop {
        let api = if use_cn {
            format!(
                "https://gitee.com/api/v5/repos/{gitee_repo}/releases?per_page=10&direction=desc"
            )
        } else if beta {
            format!("https://api.github.com/repos/{repo}/releases?per_page=1")
        } else {
            format!("https://api.github.com/repos/{repo}/releases/latest")
        };
        match dlm.download_json(&api, &[]).await {
            Ok((_, val)) => {
                // 数组 → 挑一项；对象 → 直接用
                let picked = if let Some(arr) = val.as_array() {
                    pick_release(arr, beta)
                } else if val.is_object() && val.get("tag_name").is_some() {
                    Some(val)
                } else {
                    None
                };
                match picked {
                    Some(r) => break r,
                    None => {
                        if !use_cn {
                            println!("GitHub 上没有可更新的版本，尝试 Gitee 镜像...");
                            use_cn = true;
                            continue;
                        }
                        if beta {
                            return Err("当前没有预发布版本".into());
                        }
                        return Err(
                            "当前只有预发布版本（没有正式版）。需要测试版请用 mlc update -beta"
                                .into(),
                        );
                    }
                }
            }
            Err(e) => {
                if !use_cn {
                    println!("GitHub 不可用，自动切换 Gitee 镜像...");
                    use_cn = true;
                    continue;
                }
                return Err(format!("检查更新失败（{e}）"));
            }
        }
    };

    let remote_tag = rel
        .get("tag_name")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    if remote_tag.is_empty() {
        return Err("release 缺少 tag_name".into());
    }
    if !has_update(local_tag, &remote_tag) {
        println!("已是最新版本: {local_tag}");
        return Ok(local_tag.to_string());
    }

    let pkg = package_name();
    let dl_url = if use_cn {
        gitee_download_url(&gitee_repo, &remote_tag, &pkg)
    } else {
        asset_url(&rel, &pkg)
            .ok_or_else(|| format!("新版本 {remote_tag} 没有 {} 架构的包", platform::cpu_arch()))?
    };

    println!("发现新版本 {remote_tag}（当前 {local_tag}），正在下载...");
    let tmp = root.join(".update-tmp");
    file::remove_tree(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    let pkg_path = tmp.join(&pkg);
    dlm.download_file(&dl_url, &pkg_path, None)
        .await
        .map_err(|e| {
            file::remove_tree(&tmp);
            format!("下载失败: {e}")
        })?;
    if let Err(e) = file::extract_tar_xz(&pkg_path, &tmp) {
        file::remove_tree(&tmp);
        return Err(format!("解压失败: {e}"));
    }

    let backup = tmp.join("old");
    let items = ["mlc", "lib", "plugins", "THIRD-PARTY-NOTICES.md"];
    if let Err(e) = swap_replace(&root, &tmp, &backup, &items) {
        file::remove_tree(&tmp);
        return Err(format!("替换文件失败，已回滚（目录不可写？）: {e}"));
    }
    file::remove_tree(&tmp);
    // 旧布局遗留
    for legacy in ["liblpclcore.so", "lpcl", "lpcl-cli", "lpcl-gui"] {
        let _ = std::fs::remove_file(root.join(legacy));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let p = root.join("mlc");
        if let Ok(meta) = std::fs::metadata(&p) {
            let mut perm = meta.permissions();
            perm.set_mode(perm.mode() | 0o755);
            let _ = std::fs::set_permissions(&p, perm);
        }
    }
    println!("更新完成: {remote_tag}（重启 mlc 生效）");
    Ok(remote_tag)
}

/// 清空目录内容（不删目录本身）
fn remove_dir_contents(dir: &Path) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for e in entries.flatten() {
        remove_any(&e.path());
    }
}

/// 卸载（对齐 handleUninstall）。非安装副本拒绝。
pub fn uninstall(keep_game: bool) -> Result<(), String> {
    let Some(root) = installed_root() else {
        return Err("当前不是 install.sh 安装副本（开发/分发路径），拒绝卸载".into());
    };

    if !keep_game {
        let game_dir = crate::version::default_mc_folder();
        // 安装副本的游戏目录在 root/mc/ 时才清；这里用根下的 mc
        let game = root.join("mc");
        let game = if game.is_dir() { game } else { game_dir };
        if game.is_dir() {
            println!("正在清空游戏目录: {}", game.display());
            remove_dir_contents(&game);
        }
    }

    // PATH 符号链接
    let bin = platform::home_dir().join(".local").join("bin");
    for name in ["mlc", "mlc-gui", "lpcl", "lpcl-cli", "lpcl-gui"] {
        let link = bin.join(name);
        if link
            .symlink_metadata()
            .map(|m| m.file_type().is_symlink())
            .unwrap_or(false)
        {
            if let Ok(target) = std::fs::read_link(&link) {
                let t = platform::normalize_path_string(&target);
                let r = platform::normalize_path_string(&root);
                if t.starts_with(&r) {
                    println!("删除命令链接: {}", link.display());
                    let _ = std::fs::remove_file(&link);
                }
            }
        }
    }

    if keep_game {
        println!("删除程序本体和配置（保留游戏内容）");
        for f in [
            "mlc",
            "mlc-gui",
            "THIRD-PARTY-NOTICES.md",
            "MLC.ini",
            "lpcl",
            "lpcl-cli",
            "lpcl-gui",
            "liblpclcore.so",
            "LPCL.ini",
        ] {
            let _ = std::fs::remove_file(root.join(f));
        }
        file::remove_tree(&root.join("lib"));
        file::remove_tree(&root.join("plugins"));
    } else {
        println!("删除安装目录: {}", root.display());
        file::remove_tree(&root);
    }

    println!("卸载完成。");
    if keep_game {
        println!("（已按 -r 保留游戏目录内容）");
    }
    // 自毁退出：跳过析构，避免 Settings 把缓存写回
    std::process::exit(0);
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 数字段与后缀解析() {
        assert_eq!(numeric_version("v0.1.4"), vec![0, 1, 4]);
        assert_eq!(numeric_version("0.1.4-beta"), vec![0, 1, 4]);
        assert_eq!(prerelease_suffix("v0.1.4-beta"), "beta");
        assert_eq!(prerelease_suffix("v0.1.4"), "");
    }

    #[test]
    fn 版本比较对齐cpp() {
        assert!(has_update("v0.1.3", "v0.1.4"));
        assert!(!has_update("v0.1.4", "v0.1.3"));
        // 数字段相同：本地预发布 → 远端正式
        assert!(has_update("v0.1.4-beta", "v0.1.4"));
        // beta < rc
        assert!(has_update("v0.1.4-beta", "v0.1.4-rc"));
        assert!(!has_update("v0.1.4-rc", "v0.1.4-beta"));
        // 相同
        assert!(!has_update("v0.1.4", "v0.1.4"));
        assert!(!has_update("v0.1.4-beta", "v0.1.4-beta"));
    }

    #[test]
    fn 挑选release() {
        use serde_json::json;
        let list = vec![
            json!({"tag_name": "v1-rc", "prerelease": true}),
            json!({"tag_name": "v1", "prerelease": false}),
        ];
        assert_eq!(pick_release(&list, false).unwrap()["tag_name"], "v1");
        assert_eq!(pick_release(&list, true).unwrap()["tag_name"], "v1-rc");
    }

    #[test]
    fn 包名与gitee路径() {
        let pkg = package_name();
        assert!(pkg.starts_with("mlc-linux-"));
        assert!(pkg.ends_with(".tar.xz"));
        let u = gitee_download_url("recall80/mlc", "v0.1.4", "mlc-linux-x86_64.tar.xz");
        assert_eq!(
            u,
            "https://gitee.com/recall80/mlc/releases/download/v0.1.4/mlc-linux-x86_64.tar.xz"
        );
    }

    #[test]
    fn 交换替换与回滚() {
        use std::fs;
        let dir = std::env::temp_dir().join(format!("mlc-upd-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let app = dir.join("app");
        let new = dir.join("new");
        let bak = dir.join("old");
        fs::create_dir_all(&app).unwrap();
        fs::create_dir_all(&new).unwrap();
        fs::write(app.join("mlc"), b"old-bin").unwrap();
        fs::write(app.join("keep.txt"), b"keep").unwrap();
        fs::write(new.join("mlc"), b"new-bin").unwrap();
        swap_replace(&app, &new, &bak, &["mlc", "lib"]).unwrap();
        assert_eq!(fs::read(app.join("mlc")).unwrap(), b"new-bin");
        assert_eq!(fs::read(app.join("keep.txt")).unwrap(), b"keep");
        assert!(!new.join("mlc").exists());
    }
}
