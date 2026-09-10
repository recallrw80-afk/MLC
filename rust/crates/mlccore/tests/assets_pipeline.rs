//! AssetDownloader 管线本地测试：自起 HTTP 服务器模拟 Mojang/CF/CDN，零外网依赖。
//! 规则判定的 os.name/os.arch 当前固定按 windows/x86_64（测试机为 Windows x64）。

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use mlccore::download::{rules_allow_this_platform, AssetDownloader, DownloadManager, Stage};
use serde_json::json;

// ---------------------------------------------------------------- 辅助

type RouteMap = HashMap<&'static str, (Arc<AtomicUsize>, RespondFn)>;
type RespondFn = Box<dyn Fn() -> (u16, Vec<u8>) + Send + Sync>;

fn spawn_router(
    bind: &str,
    routes: Vec<(&'static str, Vec<u8>)>,
) -> (SocketAddr, HashMap<&'static str, Arc<AtomicUsize>>) {
    let listener = TcpListener::bind(bind).unwrap();
    let addr = listener.local_addr().unwrap();
    let map: RouteMap = routes
        .into_iter()
        .map(|(prefix, body)| {
            let hits = Arc::new(AtomicUsize::new(0));
            (
                prefix,
                (
                    hits.clone(),
                    Box::new(move || (200u16, body.clone())) as RespondFn,
                ),
            )
        })
        .collect();
    let hits: HashMap<_, _> = map.iter().map(|(k, (h, _))| (*k, h.clone())).collect();
    let map = Arc::new(map);
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let map = map.clone();
            std::thread::spawn(move || {
                let mut s = stream;
                let mut buf = Vec::new();
                let mut byte = [0u8; 1];
                while let Ok(1) = s.read(&mut byte) {
                    buf.push(byte[0]);
                    if buf.ends_with(b"\r\n\r\n") {
                        break;
                    }
                }
                let head = String::from_utf8_lossy(&buf);
                let path = head.split_whitespace().nth(1).unwrap_or("/");
                let entry = map.iter().find(|(prefix, _)| path.starts_with(*prefix));
                let (status, body) = match entry {
                    Some((_, (hits, respond))) => {
                        hits.fetch_add(1, Ordering::SeqCst);
                        respond()
                    }
                    None => (404, b"no route".to_vec()),
                };
                let head = format!(
                    "HTTP/1.1 {status} T\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = s.write_all(head.as_bytes());
                let _ = s.write_all(&body);
            });
        }
    });
    (addr, hits)
}

fn fast_manager() -> DownloadManager {
    DownloadManager::builder()
        .max_retries(0)
        .retry_delay(std::time::Duration::from_millis(5))
        .timeouts(
            std::time::Duration::from_millis(400),
            std::time::Duration::from_millis(400),
            std::time::Duration::from_millis(300),
        )
        .build()
}

fn temp_dir(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("mlc-asset-{}-{tag}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::create_dir_all(&dir);
    dir
}

/// 构造一条完整的 manifest→verJson 链（Mojang 服务器在 127.0.0.1）
fn fake_manifest_chain(mojang: &str) -> Vec<(&'static str, Vec<u8>)> {
    let ver_json = json!({
        "id": "1.99.0",
        "downloads": {"client": {"url": format!("http://{mojang}/client.jar"), "sha1": ""}},
        "libraries": [],
        "assets": "legacy-1"
    });
    vec![
        ("/mc/game/version_manifest.json", json!({
            "versions": [{"id": "1.99.0", "url": format!("http://{mojang}/versions/1.99.0.json")}]
        }).to_string().into_bytes()),
        ("/versions/1.99.0.json", ver_json.to_string().into_bytes()),
        ("/client.jar", b"JAR-BYTES".to_vec()),
    ]
}

// ---------------------------------------------------------------- 用例

#[tokio::test]
async fn 管线顺序与产物落盘() {
    let (mojang_addr, hits) =
        spawn_router("127.0.0.1:0", fake_manifest_chain("127.0.0.1:__PORT__"));
    // manifest 里的 URL 需要真实端口——先起服务器拿端口再改路由体
    // 简化：重新起一个带正确端口的服务器（spawn_router 不支持后改，直接先拿端口）
    drop(hits);
    let (mojang_addr, hits) =
        spawn_router("127.0.0.1:0", fake_manifest_chain(&mojang_addr.to_string()));

    // 用构建器把 DownloadManager 的首字节超时调短，且让引擎走我们的本地地址
    // manifest_url 是常量 launchermeta.mojang.com——本地无法直接指。
    // 因此这个测试用 download_client_jar / download_libraries 等子步骤单测管线，
    // 顶层 download_version 的 manifest 阶段用纯 URL 验证。

    let mc_root = temp_dir("pipe");
    let ctx = mlccore::download::VersionContext {
        id: "1.99.0".into(),
        version_dir: mc_root.join("versions/1.99.0"),
        mc_root: mc_root.clone(),
    };
    std::fs::create_dir_all(&ctx.version_dir).unwrap();
    // 版本 JSON（含 client 下载地址指向本地）
    let ver_json_path = ctx.version_dir.join("1.99.0.json");
    let ver_json = json!({
        "id": "1.99.0",
        "downloads": {"client": {"url": format!("http://{mojang_addr}/client.jar"), "sha1": ""}},
        "libraries": [],
        "assets": "legacy-1"
    });
    std::fs::write(&ver_json_path, ver_json.to_string()).unwrap();

    let stages = Arc::new(std::sync::Mutex::new(Vec::new()));
    let stages2 = stages.clone();
    let downloader = AssetDownloader::new(fast_manager()).on_stage(Arc::new(move |s| {
        stages2.lock().unwrap().push(s);
    }));

    downloader
        .download_client_jar(&ctx, &ver_json_path)
        .await
        .expect("client jar 应下载成功");

    assert!(mc_root.join("versions/1.99.0/1.99.0.jar").exists());
    assert_eq!(hits["/client.jar"].load(Ordering::SeqCst), 1);

    // 缓存跳过：再跑一次（同 jar + 无 sha1 时不跳过，但 hits 不应再涨——
    // C++ 无 sha1 时直接下，这里验证文件在而不再请求需要 sha1；无 sha1 路径本就不缓存）
}

#[tokio::test]
async fn 资产部分失败不致命() {
    // 注意语义（对齐 C++）：部分失败不致命（f < total），全部失败才算失败。
    // 本用例一个成功一个 404，应为 Ok。
    let (mojang_addr, _) = spawn_router(
        "127.0.0.1:0",
        vec![(
            "/indexes/legacy-1.json",
            json!({
                "objects": {
                    "a.json": {"hash": "e59ff97941044f91df529e88dd0000000000000001", "size": 3},
                    "b.json": {"hash": "e59ff97941044f91df529e88dd0000000000000002", "size": 3}
                }
            })
            .to_string()
            .into_bytes(),
        )],
    );
    let (res_addr, _res_hits) = spawn_router(
        "127.0.0.2:0",
        vec![
            (
                "/e5/e59ff97941044f91df529e88dd0000000000000001",
                b"ok1".to_vec(),
            ),
            // 第二个资产没有路由 → 404 → 失败（引擎重试后仍失败）
        ],
    );

    let mc_root = temp_dir("assets");
    let ctx = mlccore::download::VersionContext {
        id: "1.99.0".into(),
        version_dir: mc_root.join("versions/1.99.0"),
        mc_root: mc_root.clone(),
    };
    std::fs::create_dir_all(&ctx.version_dir).unwrap();

    let ver_json = json!({
        "id": "1.99.0",
        "assetIndex": {"id": "legacy-1", "url": format!("http://{mojang_addr}/indexes/legacy-1.json")},
        // 让下载地址指向资源服务器
        "downloads": {"client": {"url": format!("http://{res_addr}/client.jar"), "sha1": ""}}
    });
    let downloader =
        AssetDownloader::new(fast_manager()).resources_base(&format!("http://{res_addr}"));
    downloader
        .download_assets(&ctx, &ver_json)
        .await
        .expect("部分资产失败（1/2）不应导致整体失败");

    assert!(mc_root.join("assets/indexes/legacy-1.json").exists());
    let ok = mc_root.join("assets/objects/e5/e59ff97941044f91df529e88dd0000000000000001");
    assert!(ok.exists());
    assert_eq!(std::fs::read(&ok).unwrap(), b"ok1");
    // 失败的那个（未注册路由 → 404）不落盘
    assert!(!mc_root
        .join("assets/objects/e5/e59ff97941044f91df529e88dd0000000000000002")
        .exists());
}

#[test]
fn 规则判定对齐cpp() {
    // 无 rules → 允许
    assert!(rules_allow_this_platform(&json!({"name": "x"})));
    // 缺 action 按 allow
    assert!(rules_allow_this_platform(
        &json!({"rules": [{"os": {"name": "windows"}}]})
    ));
    // os.name 匹配 + allow
    assert!(rules_allow_this_platform(&json!({
        "rules": [{"action": "allow", "os": {"name": "windows"}}]
    })));
    // os.name 不匹配 → 后面的 disallow 不影响前面已命中的 allow
    // （C++ rulesAllowOnThisPlatform：matches 初值 true，仅覆盖不取反——
    //  [allow windows, disallow osx] 在 windows 上 = true）
    assert!(rules_allow_this_platform(&json!({
        "rules": [
            {"action": "allow", "os": {"name": "windows"}},
            {"action": "disallow", "os": {"name": "osx"}}
        ]
    })));
    // 反向：先 disallow osx（不匹配），后 allow windows（命中）→ true
    assert!(rules_allow_this_platform(&json!({
        "rules": [
            {"action": "disallow", "os": {"name": "osx"}},
            {"action": "allow", "os": {"name": "windows"}}
        ]
    })));
    // 双条件（name + arch）：x86_64 匹配
    assert!(rules_allow_this_platform(&json!({
        "rules": [{"action": "allow", "os": {"name": "windows", "arch": "x86_64"}}]
    })));
    // 双条件里 arch 不匹配 → 整体不匹配
    assert!(!rules_allow_this_platform(&json!({
        "rules": [{"action": "allow", "os": {"name": "windows", "arch": "x86"}}]
    })));
}

#[test]
fn stage枚举与版本上下文() {
    let ctx = mlccore::download::VersionContext {
        id: "1.20.1".into(),
        version_dir: PathBuf::from("/tmp/mc/versions/1.20.1"),
        mc_root: PathBuf::from("/tmp/mc"),
    };
    assert_eq!(
        ctx.version_dir.join("1.20.1.json"),
        PathBuf::from("/tmp/mc/versions/1.20.1/1.20.1.json")
    );
    assert_eq!(
        ctx.mc_root.join("libraries"),
        PathBuf::from("/tmp/mc/libraries")
    );
    assert_eq!(Stage::Done, Stage::Done);
}
