//! 认证，对应 C++ `sdk/src/auth/` + `mlc.cpp` 的玩家/外置登录 API：
//! offline / authlib-injector（会话加密持久化 + 启动时在线刷新，失败回退离线）。
//! Microsoft OAuth 仍是最大单点风险，单独 spike（占位见 [`ms`]）。

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use md5::{Digest, Md5};
use serde_json::{json, Value};

use crate::settings::Settings;

// ---------------------------------------------------------------- 类型

/// 对齐 C++ LoginResult（启动侧使用的子集）
#[derive(Debug, Clone, Default)]
pub struct AuthLogin {
    pub name: String,
    pub uuid: String,
    pub access_token: String,
    /// "Legacy" / "Auth" / "Nide" / "Ms"
    pub login_type: String,
    pub client_token: String,
    pub profile_json: String,
    pub server_url: String,
    pub error_message: String,
}

impl AuthLogin {
    pub fn is_valid(&self) -> bool {
        !self.name.is_empty() && !self.uuid.is_empty()
    }

    /// 转启动侧 LoginResult
    pub fn to_launch_login(&self) -> crate::launch::LoginResult {
        crate::launch::LoginResult {
            name: self.name.clone(),
            uuid: self.uuid.clone(),
            access_token: self.access_token.clone(),
            login_type: self.login_type.clone(),
            client_token: self.client_token.clone(),
            profile_json: self.profile_json.clone(),
            server_url: self.server_url.clone(),
        }
    }
}

/// 对齐 PlayerEntry
#[derive(Debug, Clone, Default)]
pub struct PlayerEntry {
    pub uuid: String,
    pub name: String,
    pub avatar: String,
    pub skin_type: String,
}

/// 对齐 AuthlibLoginInfo
#[derive(Debug, Clone, Default)]
pub struct AuthlibLoginInfo {
    pub logged_in: bool,
    pub server: String,
    pub name: String,
    pub uuid: String,
}

// ---------------------------------------------------------------- 离线登录

/// Mojang offline UUID = MD5("OfflinePlayer:"+name)，v3 变体（对齐 generateOfflineUuid）
pub fn generate_offline_uuid(username: &str) -> String {
    let input = format!("OfflinePlayer:{username}");
    let mut hash = Md5::digest(input.as_bytes());
    hash[6] = (hash[6] & 0x0f) | 0x30; // version 3
    hash[8] = (hash[8] & 0x3f) | 0x80; // variant
    let hex: String = hash.iter().map(|b| format!("{b:02x}")).collect();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

fn random_bytes16() -> [u8; 16] {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let pid = std::process::id() as u128;
    let seq = COUNTER.fetch_add(1, Ordering::Relaxed) as u128;
    // 栈地址作弱熵；与时间/进程号/序号混合
    let addr = &nanos as *const u128 as usize as u128;
    let mut mixed = nanos.wrapping_mul(0x9e37_79b9_7f4a_7c15) ^ (pid << 64) ^ seq ^ addr;
    let mut out = [0u8; 16];
    for chunk in out.chunks_mut(8) {
        mixed = mixed
            .wrapping_add(0x9e37_79b9_7f4a_7c15)
            .wrapping_mul(0xbf58_476d_1ce4_e5b9);
        let x = (mixed ^ (mixed >> 30)).wrapping_mul(0x94d0_49bb_1331_11eb);
        let x = (x ^ (x >> 31)) as u64;
        chunk.copy_from_slice(&x.to_le_bytes());
    }
    out
}

/// 随机 client token（对齐 QUuid 无花括号形式）
pub fn generate_client_token() -> String {
    let b = random_bytes16();
    // 形成 UUID v4 形态便于日志阅读
    let mut b = b;
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let hex: String = b.iter().map(|x| format!("{x:02x}")).collect();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

/// UUID 默认皮肤判定（同 PCL McSkinSex）：第 7/15/23/31 位 XOR mod 2
pub fn skin_sex_from_uuid(uuid: &str) -> &'static str {
    let u: String = uuid.chars().filter(|c| *c != '-').collect();
    if u.len() != 32 {
        return "Steve";
    }
    let nib = |i: usize| (u.as_bytes()[i] as char).to_digit(16).unwrap_or(0);
    if (nib(7) ^ nib(15) ^ nib(23) ^ nib(31)) % 2 == 1 {
        "Alex"
    } else {
        "Steve"
    }
}

/// 递增 UUID 末 5 位直到默认皮肤为目标性别
fn uuid_for_skin_sex(uuid: &str, target_sex: &str) -> String {
    let mut u: String = uuid.chars().filter(|c| *c != '-').collect();
    if u.len() != 32 {
        return uuid.to_string();
    }
    while skin_sex_from_uuid(&u) != target_sex {
        let tail = u64::from_str_radix(&u[27..32], 16).unwrap_or(0);
        let tail = (tail + 1) & 0xF_FFFF;
        u = format!("{}{:05x}", &u[..27], tail);
    }
    format!(
        "{}-{}-{}-{}-{}",
        &u[0..8],
        &u[8..12],
        &u[12..16],
        &u[16..20],
        &u[20..]
    )
}

/// 对齐 createOfflineLogin：skinType=default/slim|alex/wide|steve
pub fn create_offline_login(username: &str, skin_type: &str) -> AuthLogin {
    let name = username.trim().to_string();
    let mut uuid = generate_offline_uuid(&name);
    let st = skin_type.to_ascii_lowercase();
    let target = if st == "slim" || st == "alex" {
        Some("Alex")
    } else if st == "wide" || st == "steve" {
        Some("Steve")
    } else {
        None
    };
    if let Some(t) = target {
        uuid = uuid_for_skin_sex(&uuid, t);
    }
    AuthLogin {
        name,
        uuid,
        access_token: "0".into(),
        login_type: "Legacy".into(),
        client_token: generate_client_token(),
        ..Default::default()
    }
}

// ---------------------------------------------------------------- 玩家档案

/// listPlayers
pub fn list_players(settings: &Settings) -> Vec<PlayerEntry> {
    settings
        .player_profiles()
        .into_iter()
        .map(|uuid| PlayerEntry {
            uuid: uuid.clone(),
            name: settings.get_profile(&uuid, "Name").unwrap_or_default(),
            avatar: settings.get_profile(&uuid, "Avatar").unwrap_or_default(),
            skin_type: settings
                .get_profile(&uuid, "SkinType")
                .unwrap_or_else(|| "slim".into()),
        })
        .collect()
}

/// addPlayer：customUuid 空则随机；首个玩家自动选中
pub fn add_player(
    settings: &mut Settings,
    name: &str,
    avatar: &str,
    skin_type: &str,
    custom_uuid: &str,
) -> PlayerEntry {
    let mut uuid = custom_uuid.replace(['{', '}'], "");
    if uuid.is_empty() {
        uuid = generate_client_token();
    }
    settings.set_profile(&uuid, "Name", name);
    settings.set_profile(&uuid, "Avatar", avatar);
    settings.set_profile(&uuid, "SkinType", skin_type);
    if settings.player_profiles().len() == 1 {
        settings.select_player(&uuid);
    }
    PlayerEntry {
        uuid,
        name: name.into(),
        avatar: avatar.into(),
        skin_type: skin_type.into(),
    }
}

/// updatePlayer：支持 newUuid 迁移配置键
pub fn update_player(
    settings: &mut Settings,
    uuid: &str,
    name: &str,
    avatar: &str,
    skin_type: &str,
    new_uuid: &str,
) -> bool {
    if !settings.player_profiles().contains(&uuid.to_string()) {
        return false;
    }
    let target = new_uuid.replace(['{', '}'], "");
    if !target.is_empty() && target != uuid && settings.player_profiles().contains(&target) {
        return false;
    }
    settings.set_profile(uuid, "Name", name);
    settings.set_profile(uuid, "Avatar", avatar);
    settings.set_profile(uuid, "SkinType", skin_type);
    if !target.is_empty() && target != uuid {
        settings.set_profile(&target, "Name", name);
        settings.set_profile(&target, "Avatar", avatar);
        settings.set_profile(&target, "SkinType", skin_type);
        let was_selected = settings.selected_player().as_deref() == Some(uuid);
        settings.remove_profile(uuid);
        if was_selected {
            settings.select_player(&target);
        }
    }
    true
}

/// removePlayer：删选中玩家时回退剩余首个
pub fn remove_player(settings: &mut Settings, uuid: &str) -> bool {
    if uuid.is_empty() || !settings.player_profiles().contains(&uuid.to_string()) {
        return false;
    }
    settings.remove_profile(uuid);
    if settings.selected_player().as_deref() == Some(uuid) {
        let rest = settings.player_profiles();
        let next = rest.first().cloned().unwrap_or_default();
        settings.select_player(&next);
    }
    true
}

/// selectPlayer
pub fn select_player(settings: &mut Settings, uuid: &str) -> bool {
    if !settings.player_profiles().contains(&uuid.to_string()) {
        return false;
    }
    settings.select_player(uuid);
    true
}

// ---------------------------------------------------------------- Authlib 持久化

fn strip_trailing_slashes(s: &str) -> String {
    s.trim_end_matches('/').to_string()
}

/// accessToken/clientToken 加密落盘
pub fn persist_authlib_login(settings: &mut Settings, r: &AuthLogin) {
    let server = strip_trailing_slashes(&r.server_url);
    settings.set_string("Authlib/Server", &server);
    settings.set_encrypted("Authlib/AccessToken", &r.access_token);
    settings.set_encrypted("Authlib/ClientToken", &r.client_token);
    settings.set_string("Authlib/Name", &r.name);
    settings.set_string("Authlib/Uuid", &r.uuid);
}

/// logoutAuthlib：清空会话（保留用户档案）
pub fn logout_authlib(settings: &mut Settings) {
    settings.set_string("Authlib/Server", "");
    settings.set_encrypted("Authlib/AccessToken", "");
    settings.set_encrypted("Authlib/ClientToken", "");
    settings.set_string("Authlib/Name", "");
    settings.set_string("Authlib/Uuid", "");
}

/// currentAuthlibLogin
pub fn current_authlib_login(settings: &Settings) -> AuthlibLoginInfo {
    let server = settings.get_string("Authlib/Server").unwrap_or_default();
    let token = settings.get_encrypted("Authlib/AccessToken");
    if !server.is_empty() && !token.is_empty() {
        AuthlibLoginInfo {
            logged_in: true,
            server,
            name: settings.get_string("Authlib/Name").unwrap_or_default(),
            uuid: settings.get_string("Authlib/Uuid").unwrap_or_default(),
        }
    } else {
        AuthlibLoginInfo::default()
    }
}

// ---------------------------------------------------------------- Authlib 网络

fn auth_client() -> reqwest::Client {
    reqwest::Client::builder()
        .user_agent(crate::download::manager::USER_AGENT)
        .build()
        .unwrap_or_default()
}

async fn post_json(url: &str, body: &Value) -> Result<(u16, Value), String> {
    let bytes = serde_json::to_vec(body).map_err(|e| e.to_string())?;
    let resp = auth_client()
        .post(url)
        .header(reqwest::header::CONTENT_TYPE, "application/json")
        .body(bytes)
        .send()
        .await
        .map_err(|e| e.to_string())?;
    let status = resp.status().as_u16();
    let text = resp.text().await.map_err(|e| e.to_string())?;
    let root: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
    Ok((status, root))
}

fn error_message_from(root: &Value, status: u16, fallback: &str) -> String {
    root.get("errorMessage")
        .and_then(|v| v.as_str())
        .map(|s| s.to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| {
            if status == 0 {
                fallback.to_string()
            } else {
                format!("HTTP {status}")
            }
        })
}

fn parse_auth_response(
    root: &Value,
    client_token: &str,
    server: &str,
    kind: &str,
) -> Result<AuthLogin, String> {
    let mut profile = root.get("selectedProfile").cloned().unwrap_or(Value::Null);
    if profile.as_object().map(|o| o.is_empty()).unwrap_or(true) {
        if let Some(first) = root
            .get("availableProfiles")
            .and_then(|a| a.as_array())
            .and_then(|a| a.first())
        {
            profile = first.clone();
        }
    }
    let name = profile
        .get("name")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let uuid = profile
        .get("id")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    let access_token = root
        .get("accessToken")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    if name.is_empty() || access_token.is_empty() {
        let why = root
            .get("errorMessage")
            .and_then(|v| v.as_str())
            .unwrap_or("响应缺少角色或 accessToken")
            .to_string();
        return Err(why);
    }
    Ok(AuthLogin {
        name,
        uuid,
        access_token,
        login_type: kind.into(),
        client_token: client_token.to_string(),
        server_url: server.to_string(),
        ..Default::default()
    })
}

/// 对齐 AuthlibAuth::doAuthlibLogin（Yggdrasil authenticate）
pub async fn login_authlib(
    server_url: &str,
    username: &str,
    password: &str,
) -> Result<AuthLogin, String> {
    let server = strip_trailing_slashes(server_url);
    if server.is_empty() || username.trim().is_empty() || password.is_empty() {
        return Err("Username or password empty".into());
    }
    let client_token = generate_client_token();
    let url = format!("{server}/authserver/authenticate");
    let body = json!({
        "agent": {"name": "Minecraft", "version": 1},
        "username": username,
        "password": password,
        "clientToken": client_token,
        "requestUser": true
    });
    let (status, root) = post_json(&url, &body).await?;
    if !(200..300).contains(&status) {
        return Err(error_message_from(&root, status, "authlib login failed"));
    }
    let mut login = parse_auth_response(&root, &client_token, &server, "Auth")?;
    login.server_url = server;
    Ok(login)
}

/// 对齐 AuthlibAuth::doNideLogin
pub async fn login_nide(
    server_url: &str,
    username: &str,
    password: &str,
) -> Result<AuthLogin, String> {
    let server = strip_trailing_slashes(server_url);
    if server.is_empty() || username.trim().is_empty() || password.is_empty() {
        return Err("Username or password empty".into());
    }
    let client_token = generate_client_token();
    let url = format!("{server}/api/yggdrasil/authserver/authenticate");
    let body = json!({
        "agent": {"name": "Minecraft", "version": 1},
        "username": username,
        "password": password,
        "clientToken": client_token,
        "requestUser": true
    });
    let (status, root) = post_json(&url, &body).await?;
    if !(200..300).contains(&status) {
        return Err(error_message_from(&root, status, "nide login failed"));
    }
    let mut login = parse_auth_response(&root, &client_token, &server, "Nide")?;
    login.server_url = server;
    Ok(login)
}

/// 对齐 AuthlibAuth::refresh：clientToken 不变；name/uuid 可空则由调用方沿用
pub async fn refresh_authlib(
    server_url: &str,
    access_token: &str,
    client_token: &str,
) -> Result<AuthLogin, String> {
    let server = strip_trailing_slashes(server_url);
    if server.is_empty() || access_token.is_empty() {
        return Err("missing server or token".into());
    }
    let url = format!("{server}/authserver/refresh");
    let mut body = json!({
        "accessToken": access_token,
        "requestUser": true
    });
    if !client_token.is_empty() {
        body["clientToken"] = json!(client_token);
    }
    let (status, root) = post_json(&url, &body).await?;
    if !(200..300).contains(&status) {
        return Err(error_message_from(&root, status, "authlib refresh failed"));
    }
    let new_token = root
        .get("accessToken")
        .and_then(|v| v.as_str())
        .unwrap_or("");
    if new_token.is_empty() {
        return Err("响应缺少 accessToken".into());
    }
    let profile = root.get("selectedProfile").cloned().unwrap_or(Value::Null);
    Ok(AuthLogin {
        name: profile
            .get("name")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string(),
        uuid: profile
            .get("id")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string(),
        access_token: new_token.to_string(),
        login_type: "Auth".into(),
        client_token: client_token.to_string(),
        server_url: server,
        ..Default::default()
    })
}

/// 登录并持久化（对齐 loginAuthlib）
pub async fn login_authlib_and_persist(
    settings: &mut Settings,
    server_url: &str,
    username: &str,
    password: &str,
) -> Result<AuthlibLoginInfo, String> {
    let login = login_authlib(server_url, username, password).await?;
    persist_authlib_login(settings, &login);
    Ok(AuthlibLoginInfo {
        logged_in: true,
        server: strip_trailing_slashes(&login.server_url),
        name: login.name,
        uuid: login.uuid,
    })
}

/// 启动时解析登录态：外置在线刷新失败则离线回退（对齐 mlc.cpp launch 路径）
pub async fn resolve_launch_login(
    settings: &mut Settings,
    player_name: &str,
    skin_type: &str,
) -> AuthLogin {
    let info = current_authlib_login(settings);
    if info.logged_in {
        let access = settings.get_encrypted("Authlib/AccessToken");
        let client = settings.get_encrypted("Authlib/ClientToken");
        match refresh_authlib(&info.server, &access, &client).await {
            Ok(mut refreshed) => {
                if refreshed.name.is_empty() {
                    refreshed.name = info.name.clone();
                }
                if refreshed.uuid.is_empty() {
                    refreshed.uuid = info.uuid.clone();
                }
                if refreshed.login_type.is_empty() {
                    refreshed.login_type = "Auth".into();
                }
                persist_authlib_login(settings, &refreshed);
                return refreshed;
            }
            Err(e) => {
                tracing::warn!("authlib 刷新失败，回退离线: {e}");
            }
        }
    }
    create_offline_login(player_name, skin_type)
}

// ---------------------------------------------------------------- Microsoft OAuth（占位）

/// Microsoft 设备码 OAuth——最大单点风险，单独 spike 后再实现
pub mod ms {
    /// 阶段 1 spike 前返回未实现
    pub fn not_yet_implemented() -> &'static str {
        "Microsoft OAuth 尚未移植（计划：设备码 + token 刷新全流程 spike）"
    }
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_settings(tag: &str) -> Settings {
        let dir = std::env::temp_dir().join(format!("mlc-auth-{}-{tag}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        Settings::load(&dir.join("MLC.ini"))
    }

    #[test]
    fn 离线uuid算法钉死() {
        // 与 C++ OfflinePlayer:Notch 的 MD5 v3 结果一致性：只验证格式与版本位
        let u = generate_offline_uuid("Notch");
        assert_eq!(u.len(), 36);
        assert_eq!(&u[14..15], "3"); // version 3
        let variant = u.as_bytes()[19];
        assert!(matches!(variant, b'8' | b'9' | b'a' | b'b'));
        // 确定性
        assert_eq!(u, generate_offline_uuid("Notch"));
    }

    #[test]
    fn 皮肤性别与迭代() {
        // 构造一个 Steve/alex 探测：直接用已知奇偶
        let steve_uuid = "00000000-0000-0000-0000-000000000000";
        assert_eq!(skin_sex_from_uuid(steve_uuid), "Steve");
        // slim 应迭代到 Alex
        let login = create_offline_login("Tester", "slim");
        assert_eq!(skin_sex_from_uuid(&login.uuid), "Alex");
        let login = create_offline_login("Tester", "wide");
        assert_eq!(skin_sex_from_uuid(&login.uuid), "Steve");
        assert_eq!(login.login_type, "Legacy");
        assert_eq!(login.access_token, "0");
        assert!(!login.client_token.is_empty());
    }

    #[test]
    fn 玩家增删改选与首个自动选中() {
        let mut s = temp_settings("players");
        let p = add_player(&mut s, "甲", "", "slim", "");
        assert_eq!(s.selected_player().as_deref(), Some(p.uuid.as_str()));
        let p2 = add_player(&mut s, "乙", "a.png", "wide", "");
        assert_ne!(p.uuid, p2.uuid);
        assert_eq!(list_players(&s).len(), 2);

        assert!(select_player(&mut s, &p2.uuid));
        assert!(update_player(&mut s, &p2.uuid, "乙改", "b.png", "slim", ""));
        assert_eq!(s.get_profile(&p2.uuid, "Name").as_deref(), Some("乙改"));

        // 删选中 → 回退剩余首个
        let removed = p2.uuid.clone();
        assert!(remove_player(&mut s, &removed));
        assert_eq!(s.selected_player().as_deref(), Some(p.uuid.as_str()));
        assert!(!remove_player(&mut s, "nope"));
    }

    #[test]
    fn 外置会话加密落盘与读取() {
        let mut s = temp_settings("authlib");
        let login = AuthLogin {
            name: "LittleSkin用户".into(),
            uuid: "uuid-1".into(),
            access_token: "secret-token".into(),
            client_token: "client-1".into(),
            server_url: "https://littleskin.cn/api/yggdrasil/".into(),
            login_type: "Auth".into(),
            ..Default::default()
        };
        persist_authlib_login(&mut s, &login);
        let info = current_authlib_login(&s);
        assert!(info.logged_in);
        assert_eq!(info.server, "https://littleskin.cn/api/yggdrasil"); // 尾斜杠归一
        assert_eq!(info.name, "LittleSkin用户");
        // 密文不落明文
        let raw = s.get_string("Authlib/AccessToken").unwrap_or_default();
        assert!(!raw.contains("secret-token"));
        assert_eq!(s.get_encrypted("Authlib/AccessToken"), "secret-token");

        logout_authlib(&mut s);
        assert!(!current_authlib_login(&s).logged_in);
    }

    #[test]
    fn client_token格式() {
        let a = generate_client_token();
        let b = generate_client_token();
        assert_eq!(a.len(), 36);
        assert_ne!(a, b);
        assert_eq!(&a[14..15], "4");
    }

    #[tokio::test]
    async fn authlib错误响应映射() {
        // 空参直接失败，不发网络
        assert!(login_authlib("", "u", "p").await.is_err());
        assert!(login_authlib("https://x.example", "", "p").await.is_err());
        assert!(refresh_authlib("https://x.example", "", "").await.is_err());
    }
}
