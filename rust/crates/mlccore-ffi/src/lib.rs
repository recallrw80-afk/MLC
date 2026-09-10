//! mlccore 的 C ABI 包装，供 Qt GUI（mlc-gui 的 4 个 bridge）链接。
//!
//! 约定：
//! - 字符串出参所有权归调用方，统一用 `mlc_string_free` 释放；入参为 NUL 结尾 UTF-8
//! - 列表/结构体出参为 JSON 字符串（UTF-8），同样用 `mlc_string_free`
//! - 异步 API 在内部 tokio runtime 上阻塞至完成，再经回调返回
//! - 头文件由 cbindgen 生成（见 cbindgen.toml），API 子集见 docs/rust-inventory.md 表 2

#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::sync::OnceLock;

use serde_json::json;

/// 返回版本字符串（静态存储，调用方禁止释放）
#[no_mangle]
pub extern "C" fn mlc_version() -> *const c_char {
    static VERSION: OnceLock<CString> = OnceLock::new();
    VERSION
        .get_or_init(|| CString::new(mlccore::GIT_DESCRIBE).expect("版本号不含 NUL"))
        .as_ptr()
}

/// 释放由本库返回的堆字符串
///
/// # Safety
/// `s` 必须来自本库返回的 CString::into_raw，且只能释放一次。
#[no_mangle]
pub unsafe extern "C" fn mlc_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

unsafe fn cstr_to_str<'a>(s: *const c_char) -> Option<&'a str> {
    if s.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(s) }.to_str().ok()
}

fn leak_cstring(s: String) -> *mut c_char {
    CString::new(s.replace('\0', ""))
        .map(|c| c.into_raw())
        .unwrap_or(std::ptr::null_mut())
}

fn runtime() -> &'static tokio::runtime::Runtime {
    static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("tokio runtime")
    })
}

fn ensure_settings() {
    mlccore::settings::initialize(None);
}

fn mc_folder() -> std::path::PathBuf {
    mlccore::settings::with_global(|s| mlccore::version::resolve_mc_folder(s))
        .unwrap_or_else(mlccore::version::default_mc_folder)
}

// ---------------------------------------------------------------- 实例

/// 实例显示名列表（JSON 数组字符串）
#[no_mangle]
pub extern "C" fn mlc_list_versions() -> *mut c_char {
    ensure_settings();
    let mc = mc_folder();
    let list = mlccore::settings::with_global(|s| {
        mlccore::version::list_instances(s, &mc)
            .into_iter()
            .map(|i| i.id)
            .collect::<Vec<_>>()
    })
    .unwrap_or_default();
    leak_cstring(serde_json::to_string(&list).unwrap_or_else(|_| "[]".into()))
}

/// 已安装 MC 版本列表（JSON 数组字符串）
#[no_mangle]
pub extern "C" fn mlc_list_mc_versions() -> *mut c_char {
    ensure_settings();
    let mc = mc_folder();
    let list: Vec<String> = mlccore::version::list_mc_versions(&mc)
        .into_iter()
        .map(|i| i.id)
        .collect();
    leak_cstring(serde_json::to_string(&list).unwrap_or_else(|_| "[]".into()))
}

/// 删除实例
#[no_mangle]
pub extern "C" fn mlc_remove_instance(name: *const c_char) -> bool {
    ensure_settings();
    let Some(name) = (unsafe { cstr_to_str(name) }) else {
        return false;
    };
    let mc = mc_folder();
    mlccore::settings::with_global(|s| mlccore::version::remove_instance(s, &mc, name))
        .unwrap_or(false)
}

/// 实例信息（JSON 对象；dirName 空 = 不存在）
#[no_mangle]
pub extern "C" fn mlc_instance_info(display_name: *const c_char) -> *mut c_char {
    ensure_settings();
    let Some(name) = (unsafe { cstr_to_str(display_name) }) else {
        return leak_cstring("{}".into());
    };
    let mc = mc_folder();
    let info = mlccore::settings::with_global(|s| {
        let dir_name = s.dir_for_display_name(name).unwrap_or_default();
        if dir_name.is_empty() {
            return json!({"dirName": ""});
        }
        let path = mc.join("instances").join(&dir_name);
        if !path.is_dir() {
            return json!({"dirName": ""});
        }
        // Setup.ini Version
        let setup = path.join("PCL").join("Setup.ini");
        let mut version = String::new();
        if let Ok(text) = std::fs::read_to_string(&setup) {
            let mut in_setup = false;
            for line in text.lines() {
                let line = line.trim();
                if line.starts_with('[') && line.ends_with(']') {
                    in_setup = line.eq_ignore_ascii_case("[Setup]");
                    continue;
                }
                if in_setup {
                    if let Some((k, v)) = line.split_once('=') {
                        if k.trim().eq_ignore_ascii_case("Version") {
                            version = v.trim().to_string();
                            break;
                        }
                    }
                }
            }
        }
        let mods_dir = path.join("mods");
        let mut mod_count = 0;
        if let Ok(rd) = std::fs::read_dir(&mods_dir) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                if n.ends_with(".jar") || n.ends_with(".jar.disabled") {
                    mod_count += 1;
                }
            }
        }
        json!({
            "dirName": dir_name,
            "path": format!("{}/", path.to_string_lossy().replace('\\', "/")),
            "version": version,
            "modCount": mod_count,
        })
    })
    .unwrap_or_else(|| json!({"dirName": ""}));
    leak_cstring(info.to_string())
}

/// 实例 mods 列表（JSON 数组：fileName/size/enabled）
#[no_mangle]
pub extern "C" fn mlc_list_mods(display_name: *const c_char) -> *mut c_char {
    ensure_settings();
    let empty = leak_cstring("[]".into());
    let Some(name) = (unsafe { cstr_to_str(display_name) }) else {
        return empty;
    };
    let mc = mc_folder();
    let list = mlccore::settings::with_global(|s| {
        let Some(dir_name) = s.dir_for_display_name(name).filter(|d| !d.is_empty()) else {
            return Vec::new();
        };
        let mods_dir = mc.join("instances").join(dir_name).join("mods");
        let mut out: Vec<(String, u64, bool)> = Vec::new();
        if let Ok(rd) = std::fs::read_dir(&mods_dir) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                let enabled = n.ends_with(".jar");
                if !enabled && !n.ends_with(".jar.disabled") {
                    continue;
                }
                let size = e.metadata().map(|m| m.len()).unwrap_or(0);
                out.push((n, size, enabled));
            }
        }
        out.sort_by(|a, b| a.0.cmp(&b.0));
        out
    })
    .unwrap_or_default();
    let arr: Vec<_> = list
        .into_iter()
        .map(|(file_name, size, enabled)| {
            json!({"fileName": file_name, "size": size, "enabled": enabled})
        })
        .collect();
    leak_cstring(serde_json::to_string(&arr).unwrap_or_else(|_| "[]".into()))
}

fn is_mod_file_name(n: &str) -> bool {
    !n.is_empty()
        && !n.contains('/')
        && !n.contains('\\')
        && !n.contains("..")
        && (n.ends_with(".jar") || n.ends_with(".jar.disabled"))
}

fn resolve_mods_dir(display_name: &str) -> Option<std::path::PathBuf> {
    let mc = mc_folder();
    let dir_name = mlccore::settings::with_global(|s| s.dir_for_display_name(display_name))??;
    if dir_name.is_empty() {
        return None;
    }
    Some(mc.join("instances").join(dir_name).join("mods"))
}

/// 启用/禁用 Mod
#[no_mangle]
pub extern "C" fn mlc_set_mod_enabled(
    display_name: *const c_char,
    file_name: *const c_char,
    enabled: bool,
) -> bool {
    ensure_settings();
    let (Some(dn), Some(fn_)) = (unsafe { cstr_to_str(display_name) }, unsafe {
        cstr_to_str(file_name)
    }) else {
        return false;
    };
    if !is_mod_file_name(fn_) {
        return false;
    }
    let Some(mods_dir) = resolve_mods_dir(dn) else {
        return false;
    };
    let currently = fn_.ends_with(".jar");
    if currently == enabled {
        return mods_dir.join(fn_).exists();
    }
    let to = if enabled {
        fn_.strip_suffix(".disabled").unwrap_or(fn_).to_string()
    } else {
        format!("{fn_}.disabled")
    };
    let src = mods_dir.join(fn_);
    let dst = mods_dir.join(&to);
    if dst.exists() {
        return false;
    }
    std::fs::rename(src, dst).is_ok()
}

/// 删除 Mod 文件
#[no_mangle]
pub extern "C" fn mlc_delete_mod(display_name: *const c_char, file_name: *const c_char) -> bool {
    ensure_settings();
    let (Some(dn), Some(fn_)) = (unsafe { cstr_to_str(display_name) }, unsafe {
        cstr_to_str(file_name)
    }) else {
        return false;
    };
    if !is_mod_file_name(fn_) {
        return false;
    }
    let Some(mods_dir) = resolve_mods_dir(dn) else {
        return false;
    };
    std::fs::remove_file(mods_dir.join(fn_)).is_ok()
}

// ---------------------------------------------------------------- 配置 / Java

/// 配置快照（JSON）
#[no_mangle]
pub extern "C" fn mlc_get_config() -> *mut c_char {
    ensure_settings();
    let cfg = mlccore::settings::with_global(|s| {
        let folder = s.get_string("LaunchFolderSelect").unwrap_or_default();
        let selected = s.selected_player().unwrap_or_default();
        let players: Vec<_> = mlccore::auth::list_players(s)
            .into_iter()
            .map(|p| {
                json!({"uuid": p.uuid, "name": p.name, "avatar": p.avatar, "skinType": p.skin_type})
            })
            .collect();
        json!({
            "version": mlccore::GIT_DESCRIBE,
            "commit": mlccore::GIT_COMMIT_HASH,
            "gameFolder": folder,
            "gameFolderSet": !folder.is_empty(),
            "players": players,
            "selectedPlayer": selected,
        })
    })
    .unwrap_or_else(|| json!({}));
    leak_cstring(cfg.to_string())
}

/// 系统 Java 列表（JSON 数组：pathJava/pathFolder/display/majorVersion/is64Bit）
#[no_mangle]
pub extern "C" fn mlc_list_javas() -> *mut c_char {
    ensure_settings();
    let mc = mc_folder();
    let list: Vec<_> = mlccore::java::scan_system_java(&mc)
        .into_iter()
        .map(|j| {
            json!({
                "pathJava": j.path_java,
                "pathFolder": j.path_folder,
                "display": j.display(),
                "majorVersion": j.major_version,
                "is64Bit": j.is_64bit,
                "isUserImport": j.is_user_import,
            })
        })
        .collect();
    leak_cstring(serde_json::to_string(&list).unwrap_or_else(|_| "[]".into()))
}

// ---------------------------------------------------------------- 玩家

#[no_mangle]
pub extern "C" fn mlc_list_players() -> *mut c_char {
    ensure_settings();
    let list: Vec<_> = mlccore::settings::with_global(|s| {
        mlccore::auth::list_players(s)
            .into_iter()
            .map(|p| {
                json!({"uuid": p.uuid, "name": p.name, "avatar": p.avatar, "skinType": p.skin_type})
            })
            .collect::<Vec<_>>()
    })
    .unwrap_or_default();
    leak_cstring(serde_json::to_string(&list).unwrap_or_else(|_| "[]".into()))
}

#[no_mangle]
pub extern "C" fn mlc_add_player(
    name: *const c_char,
    avatar: *const c_char,
    skin_type: *const c_char,
    custom_uuid: *const c_char,
) -> *mut c_char {
    ensure_settings();
    let n = unsafe { cstr_to_str(name) }.unwrap_or("");
    let a = unsafe { cstr_to_str(avatar) }.unwrap_or("");
    let sk = unsafe { cstr_to_str(skin_type) }.unwrap_or("slim");
    let cu = unsafe { cstr_to_str(custom_uuid) }.unwrap_or("");
    let entry = mlccore::settings::with_global(|s| mlccore::auth::add_player(s, n, a, sk, cu))
        .map(|p| json!({"uuid": p.uuid, "name": p.name, "avatar": p.avatar, "skinType": p.skin_type}))
        .unwrap_or_else(|| json!({"uuid": ""}));
    leak_cstring(entry.to_string())
}

#[no_mangle]
pub extern "C" fn mlc_update_player(
    uuid: *const c_char,
    name: *const c_char,
    avatar: *const c_char,
    skin_type: *const c_char,
    new_uuid: *const c_char,
) -> bool {
    ensure_settings();
    let (Some(u), Some(n), Some(a), Some(sk), Some(nu)) = (
        unsafe { cstr_to_str(uuid) },
        unsafe { cstr_to_str(name) },
        unsafe { cstr_to_str(avatar) },
        unsafe { cstr_to_str(skin_type) },
        unsafe { cstr_to_str(new_uuid) },
    ) else {
        return false;
    };
    mlccore::settings::with_global(|s| mlccore::auth::update_player(s, u, n, a, sk, nu))
        .unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn mlc_remove_player(uuid: *const c_char) -> bool {
    ensure_settings();
    let Some(u) = (unsafe { cstr_to_str(uuid) }) else {
        return false;
    };
    mlccore::settings::with_global(|s| mlccore::auth::remove_player(s, u)).unwrap_or(false)
}

#[no_mangle]
pub extern "C" fn mlc_select_player(uuid: *const c_char) -> bool {
    ensure_settings();
    let Some(u) = (unsafe { cstr_to_str(uuid) }) else {
        return false;
    };
    mlccore::settings::with_global(|s| mlccore::auth::select_player(s, u)).unwrap_or(false)
}

/// 用离线账号构造 LoginResult（JSON：name/uuid/accessToken/loginType/clientToken）
/// skin_type: slim|wide|default
#[no_mangle]
pub extern "C" fn mlc_create_offline_login(
    name: *const c_char,
    skin_type: *const c_char,
) -> *mut c_char {
    let n = unsafe { cstr_to_str(name) }.unwrap_or("Player");
    let sk = unsafe { cstr_to_str(skin_type) }.unwrap_or("default");
    let login = mlccore::auth::create_offline_login(n, sk);
    let v = json!({
        "name": login.name,
        "uuid": login.uuid,
        "accessToken": login.access_token,
        "loginType": login.login_type,
        "clientToken": login.client_token,
    });
    leak_cstring(v.to_string())
}

// ---------------------------------------------------------------- Authlib / CF

#[no_mangle]
pub extern "C" fn mlc_logout_authlib() {
    ensure_settings();
    mlccore::settings::with_global(mlccore::auth::logout_authlib);
}

/// 当前外置登录（JSON）
#[no_mangle]
pub extern "C" fn mlc_current_authlib_login() -> *mut c_char {
    ensure_settings();
    let info = mlccore::settings::with_global(|s| {
        let i = mlccore::auth::current_authlib_login(s);
        json!({
            "loggedIn": i.logged_in,
            "server": i.server,
            "name": i.name,
            "uuid": i.uuid,
        })
    })
    .unwrap_or_else(|| json!({"loggedIn": false}));
    leak_cstring(info.to_string())
}

/// 设置/清除 CF key（空串 = 清除）
#[no_mangle]
pub extern "C" fn mlc_set_cf_api_key(key: *const c_char) {
    ensure_settings();
    let k = unsafe { cstr_to_str(key) }.unwrap_or("");
    mlccore::settings::with_global(|s| {
        if k.is_empty() {
            s.remove_key("CfApiKey");
        } else {
            s.set_encrypted("CfApiKey", k);
        }
    });
}

/// CF key 来源："user" / "embedded" / "none"（禁止回显 key 本体）
#[no_mangle]
pub extern "C" fn mlc_cf_api_key_source() -> *mut c_char {
    ensure_settings();
    let src = mlccore::settings::with_global(|s| {
        if !s.get_encrypted("CfApiKey").is_empty() {
            "user"
        } else if !mlccore::download::EMBEDDED_CF_KEY.is_empty() {
            "embedded"
        } else {
            "none"
        }
    })
    .unwrap_or("none");
    leak_cstring(src.to_string())
}

// ---------------------------------------------------------------- Mod 平台

fn platform_from_i32(v: i32) -> mlccore::download::Platform {
    match v {
        1 => mlccore::download::Platform::Modrinth,
        _ => mlccore::download::Platform::CurseForge,
    }
}

fn resource_type_from_i32(v: i32) -> mlccore::download::ResourceType {
    use mlccore::download::ResourceType as T;
    match v {
        1 => T::ModPack,
        2 => T::ResourcePack,
        3 => T::Shader,
        4 => T::DataPack,
        _ => T::Mod,
    }
}

fn mod_resource_json(r: &mlccore::download::ModResource) -> serde_json::Value {
    json!({
        "id": r.id,
        "name": r.name,
        "summary": r.summary,
        "description": r.description,
        "author": r.author,
        "iconUrl": r.icon_url,
        "websiteUrl": r.website_url,
        "downloadCount": r.download_count,
        "lastUpdated": r.last_updated,
        "versions": r.versions,
    })
}

/// 搜索 mod（阻塞至完成）。platform: 0=CF 1=Modrinth；rtype: 0=Mod 1=ModPack 2=ResourcePack 3=Shader 4=DataPack
/// 返回 JSON 数组：[{id,name,summary,author,iconUrl,downloadCount,versions,...}]
#[no_mangle]
pub extern "C" fn mlc_mod_search(
    platform: i32,
    rtype: i32,
    query: *const c_char,
    page: u32,
    page_size: u32,
) -> *mut c_char {
    ensure_settings();
    let q = unsafe { cstr_to_str(query) }.unwrap_or("");
    let plat = platform_from_i32(platform);
    let rt = resource_type_from_i32(rtype);
    let result = runtime().block_on(async {
        let mp = mlccore::download::ModPlatform::shared();
        mp.search_resources(plat, rt, q, page, page_size).await
    });
    let arr: Vec<_> = result
        .unwrap_or_default()
        .iter()
        .map(mod_resource_json)
        .collect();
    leak_cstring(serde_json::to_string(&arr).unwrap_or_else(|_| "[]".into()))
}

/// mod 详情（阻塞）。失败返回 JSON `{"error":"..."}`。
#[no_mangle]
pub extern "C" fn mlc_mod_details(platform: i32, mod_id: *const c_char) -> *mut c_char {
    ensure_settings();
    let id = unsafe { cstr_to_str(mod_id) }.unwrap_or("");
    let plat = platform_from_i32(platform);
    let result = runtime().block_on(async {
        let mp = mlccore::download::ModPlatform::shared();
        mp.get_mod_details(plat, id).await
    });
    let v = match result {
        Ok(r) => mod_resource_json(&r),
        Err(e) => json!({"error": e}),
    };
    leak_cstring(v.to_string())
}

/// mod 文件列表（阻塞）。JSON 数组：[{id,displayName,fileName,downloadUrl,gameVersions,loaders,fileSize,releaseDate,sha1,isRelease}]
#[no_mangle]
pub extern "C" fn mlc_mod_files(platform: i32, mod_id: *const c_char) -> *mut c_char {
    ensure_settings();
    let id = unsafe { cstr_to_str(mod_id) }.unwrap_or("");
    let plat = platform_from_i32(platform);
    let result = runtime().block_on(async {
        let mp = mlccore::download::ModPlatform::shared();
        mp.get_mod_files(plat, id).await
    });
    let arr: Vec<_> = result
        .unwrap_or_default()
        .iter()
        .map(|f| {
            json!({
                "id": f.id,
                "displayName": f.display_name,
                "fileName": f.file_name,
                "downloadUrl": f.download_url,
                "gameVersions": f.game_versions,
                "loaders": f.loaders,
                "fileSize": f.file_size,
                "releaseDate": f.release_date,
                "sha1": f.sha1,
                "isRelease": f.is_release,
            })
        })
        .collect();
    leak_cstring(serde_json::to_string(&arr).unwrap_or_else(|_| "[]".into()))
}

/// 下载 mod 文件到 dest_path（阻塞）
#[no_mangle]
pub extern "C" fn mlc_mod_download(
    platform: i32,
    mod_id: *const c_char,
    file_id: *const c_char,
    dest_path: *const c_char,
    on_progress: MlcProgressCallback,
) -> bool {
    ensure_settings();
    let mid = unsafe { cstr_to_str(mod_id) }.unwrap_or("");
    let fid = unsafe { cstr_to_str(file_id) }.unwrap_or("");
    let dest = unsafe { cstr_to_str(dest_path) }.unwrap_or("");
    let plat = platform_from_i32(platform);
    let dest = std::path::PathBuf::from(dest);
    emit_progress(on_progress, "Downloading mod...", 10);
    let ok = runtime().block_on(async {
        let mp = mlccore::download::ModPlatform::shared();
        mp.download_mod(plat, mid, fid, &dest, None).await
    });
    match ok {
        Ok(_) => {
            emit_progress(on_progress, "Complete", 100);
            true
        }
        Err(e) => {
            emit_progress(on_progress, &e, 100);
            false
        }
    }
}

// ---------------------------------------------------------------- 游戏目录

/// 设置游戏目录（持久化）
#[no_mangle]
pub extern "C" fn mlc_set_folder(path: *const c_char) -> bool {
    ensure_settings();
    let Some(p) = (unsafe { cstr_to_str(path) }) else {
        return false;
    };
    mlccore::settings::with_global(|s| {
        s.set_string("LaunchFolderSelect", p);
    });
    true
}

/// 当前游戏目录（JSON 字符串字段 path）
#[no_mangle]
pub extern "C" fn mlc_mc_folder() -> *mut c_char {
    ensure_settings();
    let mc = mc_folder();
    leak_cstring(mc.to_string_lossy().replace('\\', "/"))
}

// ---------------------------------------------------------------- 异步：install / authlib 登录

/// 同步等待安装原版 MC（阻塞至完成）
#[no_mangle]
pub extern "C" fn mlc_install_version(version_id: *const c_char) -> bool {
    ensure_settings();
    let vid = unsafe { cstr_to_str(version_id) }.unwrap_or("");
    let mc = mc_folder();
    runtime().block_on(async {
        let dl = mlccore::download::AssetDownloader::new(
            mlccore::download::manager::DownloadManager::new(),
        );
        mlccore::version::install_version(&mc, vid, &dl, None)
            .await
            .is_ok()
    })
}

/// Authlib 登录完成回调
pub type MlcAuthlibLoginCallback =
    Option<unsafe extern "C" fn(ok: bool, error_or_name: *const c_char, info_json: *const c_char)>;

/// Authlib 登录（阻塞至完成）
///
/// # Safety
/// `callback` 若 Some，须在本次调用期间有效。
#[no_mangle]
pub unsafe extern "C" fn mlc_login_authlib(
    server: *const c_char,
    username: *const c_char,
    password: *const c_char,
    callback: MlcAuthlibLoginCallback,
) {
    ensure_settings();
    let server = cstr_to_str(server).unwrap_or("");
    let username = cstr_to_str(username).unwrap_or("");
    let password = cstr_to_str(password).unwrap_or("");
    let result = runtime()
        .block_on(async { mlccore::auth::login_authlib(server, username, password).await });
    match result {
        Ok(login) => {
            mlccore::settings::with_global(|s| mlccore::auth::persist_authlib_login(s, &login));
            let name = CString::new(login.name.clone()).unwrap_or_default();
            let info = json!({
                "loggedIn": true,
                "server": login.server_url,
                "name": login.name,
                "uuid": login.uuid,
            });
            let info_c = CString::new(info.to_string()).unwrap_or_default();
            if let Some(cb) = callback {
                cb(true, name.as_ptr(), info_c.as_ptr());
            }
        }
        Err(e) => {
            let err = CString::new(e).unwrap_or_default();
            let info = json!({"loggedIn": false});
            let info_c = CString::new(info.to_string()).unwrap_or_default();
            if let Some(cb) = callback {
                cb(false, err.as_ptr(), info_c.as_ptr());
            }
        }
    }
}

// ---------------------------------------------------------------- 异步：import / launch / server

/// 导入进度回调
pub type MlcProgressCallback = Option<unsafe extern "C" fn(step: *const c_char, percent: i32)>;
/// 导入完成回调：data 为 JSON 数组字符串（可能为空数组）
pub type MlcImportCompleteCallback =
    Option<unsafe extern "C" fn(ok: bool, msg: *const c_char, data_json: *const c_char)>;
/// 日志行回调
pub type MlcLogCallback = Option<unsafe extern "C" fn(line: *const c_char)>;
/// 进程退出回调
pub type MlcExitCallback = Option<unsafe extern "C" fn(exit_code: i32)>;

fn emit_progress(cb: MlcProgressCallback, step: &str, percent: i32) {
    if let Some(cb) = cb {
        let s = CString::new(step).unwrap_or_default();
        unsafe { cb(s.as_ptr(), percent) };
    }
}

fn emit_done(ok: bool, msg: &str, data: &str, cb: MlcImportCompleteCallback) {
    if let Some(cb) = cb {
        let m = CString::new(msg).unwrap_or_default();
        let d = CString::new(data).unwrap_or_default();
        unsafe { cb(ok, m.as_ptr(), d.as_ptr()) };
    }
}

/// 导入整合包（阻塞至完成）。target_instance 仅 Mod 包使用，可为空。
///
/// # Safety
/// 回调在本次调用期间有效；传入的 C 字符串须 NUL 结尾。
#[no_mangle]
pub unsafe extern "C" fn mlc_import_modpack(
    file_path: *const c_char,
    instance_name: *const c_char,
    target_instance: *const c_char,
    on_progress: MlcProgressCallback,
    on_complete: MlcImportCompleteCallback,
) {
    ensure_settings();
    let file = cstr_to_str(file_path).unwrap_or("");
    let name = cstr_to_str(instance_name).unwrap_or("");
    let target = cstr_to_str(target_instance).unwrap_or("");
    let path = std::path::PathBuf::from(file);

    if !path.exists() {
        emit_done(false, &format!("File not found: {file}"), "[]", on_complete);
        return;
    }

    let outcome: Result<String, (String, String)> = runtime().block_on(async {
        let mc = mc_folder();
        emit_progress(on_progress, "Detecting...", 5);
        let pack_type = mlccore::modpack::detect_pack_type(&path);

        let extract = mlccore::modpack::pack_tmp_root(&mc).join("pack");
        mlccore::util::file::remove_tree(&extract);
        let _ = std::fs::create_dir_all(&extract);
        if let Err(e) = mlccore::util::file::extract_zip(&path, &extract) {
            return Err((e, "[]".into()));
        }
        emit_progress(on_progress, "Extracting...", 15);

        let mut effective = extract.clone();
        if let Ok(rd) = std::fs::read_dir(&extract) {
            let dirs: Vec<_> = rd
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect();
            if dirs.len() == 1 {
                let markers = [
                    "manifest.json",
                    "modrinth.index.json",
                    "mmc-pack.json",
                    "mcbbs.packmeta",
                    "modpack.json",
                    "modpack.zip",
                    "modpack.mrpack",
                ];
                if markers.iter().any(|m| dirs[0].join(m).exists()) {
                    effective = dirs[0].clone();
                }
            }
        }

        match pack_type {
            mlccore::modpack::PackType::Mod => {
                if target.is_empty() {
                    let list = mlccore::settings::with_global(|s| {
                        mlccore::version::list_instances(s, &mc)
                            .into_iter()
                            .map(|i| i.id)
                            .collect::<Vec<_>>()
                    })
                    .unwrap_or_default();
                    let data = serde_json::to_string(&list).unwrap_or_else(|_| "[]".into());
                    return Err(("此 zip 为 mod 包，需要加 --to <实例名>".into(), data));
                }
                emit_progress(on_progress, "Copying mods...", 50);
                let n = mlccore::settings::with_global(|s| {
                    mlccore::modpack::install_mod(s, &mc, &effective, target)
                })
                .ok_or_else(|| ("Settings not initialized".to_string(), "[]".to_string()))
                .and_then(|r| r.map_err(|e| (e, "[]".to_string())))?;
                Ok(format!("{n} mod(s) added to {target}"))
            }
            mlccore::modpack::PackType::Compressed | mlccore::modpack::PackType::Unknown => {
                emit_progress(on_progress, "Installing compressed pack...", 30);
                let n = mlccore::settings::with_global(|s| {
                    mlccore::modpack::install_compressed(s, &mc, &path, name)
                })
                .ok_or_else(|| ("Settings not initialized".to_string(), "[]".to_string()))
                .and_then(|r| r.map_err(|e| (e, "[]".to_string())))?;
                emit_progress(on_progress, "Complete", 100);
                Ok(n)
            }
            mlccore::modpack::PackType::CurseForge => {
                emit_progress(on_progress, "Preparing CurseForge pack...", 25);
                let prep = mlccore::settings::with_global(|s| {
                    mlccore::modpack::prepare_curseforge(s, &mc, &effective, name)
                })
                .ok_or_else(|| ("Settings not initialized".to_string(), "[]".to_string()))
                .and_then(|r| r.map_err(|e| (e, "[]".to_string())))?;
                let dl = mlccore::download::AssetDownloader::new(
                    mlccore::download::manager::DownloadManager::new(),
                );
                let mp = mlccore::download::ModPlatform::shared();
                let prep_name = prep.name.clone();
                let final_dir = prep.final_dir.clone();
                let mut settings =
                    mlccore::settings::Settings::load(&mlccore::settings::default_config_path());
                let r = mlccore::modpack::download_and_finalize(
                    &mut settings,
                    &mc,
                    &dl,
                    mp,
                    mlccore::modpack::FinalizeRequest {
                        final_dir: &final_dir,
                        name: &prep_name,
                        mc_version: &prep.mc_version,
                        forge_ver: &prep.forge,
                        neo_ver: &prep.neo,
                        fabric_ver: &prep.fabric,
                        mods: &prep.mods,
                    },
                )
                .await;
                match r {
                    Ok(()) => {
                        emit_progress(on_progress, "Complete", 100);
                        Ok(prep_name)
                    }
                    Err(e) => {
                        let mut s = mlccore::settings::Settings::load(
                            &mlccore::settings::default_config_path(),
                        );
                        mlccore::modpack::cleanup_on_error(&mut s, &mc, &final_dir);
                        Err((e, "[]".into()))
                    }
                }
            }
            other => Err((
                format!("{} 安装器尚未移植 / installer not yet ported", other.name()),
                "[]".into(),
            )),
        }
    });

    match outcome {
        Ok(msg) => emit_done(true, &msg, "[]", on_complete),
        Err((msg, data)) => {
            let mut msg = msg;
            let mut data = data;
            // Mod 包缺 target 时错误串后附加实例列表（| 分隔）
            if let Some(idx) = msg.find('|') {
                let d = msg[idx + 1..].to_string();
                msg.truncate(idx);
                data = d;
            }
            emit_done(false, &msg, &data, on_complete);
        }
    }
}

/// 启动游戏（阻塞至进程退出）。返回是否成功拉起进程。
///
/// # Safety
/// 回调在本次调用期间有效。
#[no_mangle]
pub unsafe extern "C" fn mlc_launch_version(
    version_id: *const c_char,
    on_log: MlcLogCallback,
    on_exit: MlcExitCallback,
) -> bool {
    ensure_settings();
    let vid = cstr_to_str(version_id).unwrap_or("");
    let mc = mc_folder();

    // 解析版本
    let version = mlccore::settings::with_global(|s| {
        if let Some(dir) = s.dir_for_display_name(vid) {
            mlccore::version::load_instance_version(&mc, &dir)
        } else {
            mlccore::version::load_version(&mc, vid)
        }
    })
    .unwrap_or_default();
    if !version.is_valid {
        return false;
    }

    // 登录：外置刷新失败回退离线
    let login = mlccore::settings::with_global(|s| mlccore::auth::current_authlib_login(s))
        .unwrap_or_default();
    let player = mlccore::settings::with_global(|s| {
        let selected = s.selected_player().unwrap_or_default();
        mlccore::auth::list_players(s)
            .into_iter()
            .find(|p| p.uuid == selected)
            .map(|p| (p.name, p.skin_type))
            .unwrap_or_else(|| ("Player".into(), "default".into()))
    })
    .unwrap_or_else(|| ("Player".into(), "default".into()));

    let auth_login = runtime().block_on(async {
        if login.logged_in {
            let access = mlccore::settings::with_global(|s| s.get_encrypted("Authlib/AccessToken"))
                .unwrap_or_default();
            let client = mlccore::settings::with_global(|s| s.get_encrypted("Authlib/ClientToken"))
                .unwrap_or_default();
            match mlccore::auth::refresh_authlib(&login.server, &access, &client).await {
                Ok(mut r) => {
                    if r.name.is_empty() {
                        r.name = login.name.clone();
                    }
                    if r.uuid.is_empty() {
                        r.uuid = login.uuid.clone();
                    }
                    mlccore::settings::with_global(|s| mlccore::auth::persist_authlib_login(s, &r));
                    r
                }
                Err(_) => mlccore::auth::create_offline_login(&player.0, &player.1),
            }
        } else {
            mlccore::auth::create_offline_login(&player.0, &player.1)
        }
    });

    let javas = mlccore::java::scan_system_java(&mc);
    let Some(java) =
        mlccore::java::select_java_for_version(&javas, &version).or_else(|| javas.first().cloned())
    else {
        return false;
    };

    let json_path = std::path::PathBuf::from(&version.path_json);
    let Some(vj) = mlccore::version::resolve_inheritance_chain(&mc, &json_path) else {
        return false;
    };

    let settings_snap =
        mlccore::settings::Settings::load(&mlccore::settings::default_config_path());
    let opts = mlccore::settings::with_global(|s| mlccore::launch::LaunchOptions {
        max_memory_mb: s
            .get_string("LaunchMaxMemory")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0),
        min_memory_mb: s
            .get_string("LaunchMinMemory")
            .and_then(|v| v.parse().ok())
            .unwrap_or(512),
        fullscreen: s
            .get_string("LaunchFullscreen")
            .map(|v| v == "true" || v == "1")
            .unwrap_or(false),
        window_width: s
            .get_string("LaunchWidth")
            .and_then(|v| v.parse().ok())
            .unwrap_or(854),
        window_height: s
            .get_string("LaunchHeight")
            .and_then(|v| v.parse().ok())
            .unwrap_or(480),
        ..Default::default()
    })
    .unwrap_or_default();

    let cmd = match mlccore::launch::build_launch_command(
        &vj,
        &version,
        &java,
        &auth_login.to_launch_login(),
        &opts,
        &settings_snap,
        &mc,
    ) {
        Ok(c) => c,
        Err(_) => return false,
    };

    use std::process::{Command as StdCommand, Stdio};
    let mut child = match StdCommand::new(&cmd.java_path)
        .args(&cmd.argv[1..])
        .current_dir(&cmd.working_dir)
        .envs(cmd.env.iter().map(|(k, v)| (k.as_str(), v.as_str())))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(_) => return false,
    };

    let stdout = child.stdout.take();
    let stderr = child.stderr.take();
    let h1 = std::thread::spawn(move || {
        if let Some(pipe) = stdout {
            use std::io::{BufRead, BufReader};
            for line in BufReader::new(pipe).lines().map_while(Result::ok) {
                if let Some(cb) = on_log {
                    let s = CString::new(line).unwrap_or_default();
                    cb(s.as_ptr());
                }
            }
        }
    });
    let h2 = std::thread::spawn(move || {
        if let Some(pipe) = stderr {
            use std::io::{BufRead, BufReader};
            for line in BufReader::new(pipe).lines().map_while(Result::ok) {
                if let Some(cb) = on_log {
                    let s = CString::new(line).unwrap_or_default();
                    cb(s.as_ptr());
                }
            }
        }
    });
    let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
    let _ = h1.join();
    let _ = h2.join();
    if let Some(cb) = on_exit {
        cb(code);
    }
    true
}

/// 安装服务端（阻塞至完成）
#[no_mangle]
pub extern "C" fn mlc_install_server(
    version_id: *const c_char,
    loader_type: *const c_char,
    loader_version: *const c_char,
    on_progress: MlcProgressCallback,
) -> bool {
    ensure_settings();
    let vid = unsafe { cstr_to_str(version_id) }.unwrap_or("");
    let lt = unsafe { cstr_to_str(loader_type) }.unwrap_or("");
    let lv = unsafe { cstr_to_str(loader_version) }.unwrap_or("");
    let mc = mc_folder();
    emit_progress(on_progress, "Installing server...", 30);
    let ok = runtime().block_on(async {
        let dlm = mlccore::download::manager::DownloadManager::new();
        mlccore::server::install_server(&dlm, &mc, vid, lt, lv).await
    });
    match ok {
        Ok(_) => {
            emit_progress(on_progress, "Complete", 100);
            true
        }
        Err(e) => {
            emit_progress(on_progress, &e, 100);
            false
        }
    }
}

/// 启动服务端（前台，阻塞至退出）。返回 exit code；失败返回 -1。
///
/// # Safety
/// 回调在本次调用期间有效。
#[no_mangle]
pub unsafe extern "C" fn mlc_start_server(
    server_id: *const c_char,
    _on_log: MlcLogCallback,
    on_exit: MlcExitCallback,
) -> i32 {
    ensure_settings();
    let id = cstr_to_str(server_id).unwrap_or("");
    let mc = mc_folder();
    let settings = mlccore::settings::Settings::load(&mlccore::settings::default_config_path());
    match mlccore::server::start_server(&mc, id, &settings) {
        Ok(code) => {
            if let Some(cb) = on_exit {
                cb(code);
            }
            code
        }
        Err(_) => {
            if let Some(cb) = on_exit {
                cb(-1);
            }
            -1
        }
    }
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 版本与字符串释放() {
        let v = mlc_version();
        assert!(!v.is_null());
        let s = leak_cstring("hello".into());
        assert!(!s.is_null());
        unsafe { mlc_string_free(s) };
    }

    #[test]
    fn 列表返回json数组() {
        let p = mlc_list_players();
        assert!(!p.is_null());
        let s = unsafe { cstr_to_str(p) }.unwrap();
        assert!(s.starts_with('['));
        unsafe { mlc_string_free(p) };
    }

    #[test]
    fn cf_key来源() {
        let p = mlc_cf_api_key_source();
        let s = unsafe { cstr_to_str(p) }.unwrap();
        assert!(matches!(s, "user" | "embedded" | "none"));
        unsafe { mlc_string_free(p) };
    }

    #[test]
    fn 导入不存在文件立即失败() {
        let path = CString::new("/nonexistent/pack.zip").unwrap();
        static mut DONE: bool = false;
        unsafe extern "C" fn on_done(ok: bool, _msg: *const c_char, _data: *const c_char) {
            unsafe { DONE = ok };
        }
        unsafe {
            mlc_import_modpack(
                path.as_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                None,
                Some(on_done),
            );
            assert!(!DONE);
        }
    }
}
