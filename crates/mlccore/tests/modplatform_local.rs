//! ModPlatform 本地服务器测试：镜像降级链与 download_mod 端到端，不依赖外网。

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use mlccore::download::{ModPlatform, Platform};

// ---------------------------------------------------------------- 路由服务器

struct Route {
    prefix: &'static str,
    hits: Arc<AtomicUsize>,
    respond: Box<dyn Fn() -> (u16, Vec<u8>) + Send + Sync>,
}

fn route(
    prefix: &'static str,
    respond: impl Fn() -> (u16, Vec<u8>) + Send + Sync + 'static,
) -> Route {
    Route {
        prefix,
        hits: Arc::new(AtomicUsize::new(0)),
        respond: Box::new(respond),
    }
}

type RespondFn = Box<dyn Fn() -> (u16, Vec<u8>) + Send + Sync>;
type RouteMap = HashMap<&'static str, (Arc<AtomicUsize>, RespondFn)>;

fn spawn_router_on(
    bind: &str,
    routes: Vec<Route>,
) -> (SocketAddr, HashMap<&'static str, Arc<AtomicUsize>>) {
    let listener = TcpListener::bind(bind).unwrap();
    let addr = listener.local_addr().unwrap();
    let map: RouteMap = routes
        .into_iter()
        .map(|r| (r.prefix, (r.hits.clone(), r.respond)))
        .collect();
    let hits_map: HashMap<_, _> = map.iter().map(|(k, (h, _))| (*k, h.clone())).collect();
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
                // 请求行：GET <path> HTTP/1.1
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
    (addr, hits_map)
}

fn fast_manager() -> mlccore::download::DownloadManager {
    mlccore::download::DownloadManager::builder()
        .max_retries(3)
        .retry_delay(Duration::from_millis(10))
        .timeouts(
            Duration::from_millis(400),
            Duration::from_millis(400),
            Duration::from_millis(300),
        )
        .build()
}

fn temp_path(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("mlc-mp-test-{}-{tag}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    dir.join("out.jar")
}

// ---------------------------------------------------------------- 用例

#[tokio::test]
async fn cf_key失效401自动降级镜像() {
    // 官方与镜像必须在不同主机上（真实的 CF/MCIM 就是两个域），
    // 否则 cf_api_url 的 host 替换会把"镜像路径"也改掉（自替换）
    let (official_addr, official_hits) = spawn_router_on(
        "127.0.0.1:0",
        vec![route("/official", || {
            (401, br#"{"error":"unauthorized"}"#.to_vec())
        })],
    );
    let (mirror_addr, mirror_hits) = spawn_router_on(
        "127.0.0.2:0",
        vec![route("/mirror", || {
            (
                200,
                br#"{"data":{"id":238222,"name":"JEI","summary":"s","description":"d","downloadCount":1}}"#
                    .to_vec(),
            )
        })],
    );
    let mut p = ModPlatform::for_test(
        &format!("http://{official_addr}/official"),
        &format!("http://{mirror_addr}/mirror"),
        &format!("http://{official_addr}/mr"),
        "dead-key",
    );
    p.set_manager(fast_manager());

    let r = p
        .get_mod_details(Platform::CurseForge, "238222")
        .await
        .expect("应经镜像成功");
    assert_eq!(r.name, "JEI");
    // 官方被重试 1+3 次后才降级（引擎对 HTTP 错误同样重试，对齐 C++）
    assert_eq!(official_hits["/official"].load(Ordering::SeqCst), 4);
    assert_eq!(mirror_hits["/mirror"].load(Ordering::SeqCst), 1);
}

#[tokio::test]
async fn 无key直连镜像不走官方() {
    let (official_addr, official_hits) = spawn_router_on(
        "127.0.0.1:0",
        vec![route("/official", || (200, br#"{"data":{}}"#.to_vec()))],
    );
    let (mirror_addr, mirror_hits) = spawn_router_on(
        "127.0.0.2:0",
        vec![route("/mirror", || {
            (200, br#"{"data":{"id":9,"name":"M"}}"#.to_vec())
        })],
    );
    let mut p = ModPlatform::for_test(
        &format!("http://{official_addr}/official"),
        &format!("http://{mirror_addr}/mirror"),
        &format!("http://{official_addr}/mr"),
        "", // 无 key
    );
    p.set_manager(fast_manager());

    let r = p
        .get_mod_details(Platform::CurseForge, "9")
        .await
        .expect("应直连镜像");
    assert_eq!(r.name, "M");
    assert_eq!(official_hits["/official"].load(Ordering::SeqCst), 0);
    assert_eq!(mirror_hits["/mirror"].load(Ordering::SeqCst), 1);
}

#[tokio::test]
async fn download_mod_cf端到端() {
    let (file_addr, _) = spawn_router_on(
        "127.0.0.2:0",
        vec![route("/files/jei.jar", || (200, b"JAR-DATA-1234".to_vec()))],
    );
    // 下载 URL 由响应体给出（对齐真实 CF 行为），其中端口是动态的——
    // 测试先起文件服务器拿到端口，再让官方路由返回拼好的地址
    let dl_url = format!("http://{file_addr}/files/jei.jar");
    let (official_addr, _) = spawn_router_on(
        "127.0.0.1:0",
        vec![route("/official/mods/1/files/2/download-url", move || {
            (200, format!("{{\"data\":\"{dl_url}\"}}").into_bytes())
        })],
    );
    let mut p = ModPlatform::for_test(
        &format!("http://{official_addr}/official"),
        &format!("http://{file_addr}/mirror"),
        &format!("http://{official_addr}/mr"),
        "k",
    );
    p.set_manager(fast_manager());

    let path = temp_path("cf");
    let n = p
        .download_mod(Platform::CurseForge, "1", "2", &path, None)
        .await
        .expect("CF 下载应成功");
    assert_eq!(n, 13); // "JAR-DATA-1234"
    assert_eq!(std::fs::read(&path).unwrap(), b"JAR-DATA-1234");
    let _ = std::fs::remove_file(&path);
}

#[tokio::test]
async fn download_mod_cf受限文件报空url() {
    let (official_addr, _) = spawn_router_on(
        "127.0.0.1:0",
        vec![
            // 受限文件 data 为 null（禁止第三方分发）
            route("/official/mods/1/files/3/download-url", || {
                (200, br#"{"data":null}"#.to_vec())
            }),
        ],
    );
    let a = official_addr.to_string();
    let mut p = ModPlatform::for_test(
        &format!("http://{a}/official"),
        "http://127.0.0.2:1/mirror",
        &format!("http://{a}/mr"),
        "k",
    );
    p.set_manager(fast_manager());

    let err = p
        .download_mod(
            Platform::CurseForge,
            "1",
            "3",
            &temp_path("restricted"),
            None,
        )
        .await
        .expect_err("受限文件应失败");
    assert_eq!(err, "Empty download URL");
}

#[tokio::test]
async fn download_mod_modrinth端到端与无文件错误() {
    let (file_addr, _) = spawn_router_on(
        "127.0.0.2:0",
        vec![route("/files/mr.jar", || (200, b"MR!\n".to_vec()))],
    );
    let dl_url = format!("http://{file_addr}/files/mr.jar");
    // 两个 Modrinth 项目共用一个 mr 基址（对应真实 MR API 的结构）
    let (mr_addr, _) = spawn_router_on(
        "127.0.0.1:0",
        vec![
            route("/mr/project/1/version/2", move || {
                (
                200,
                format!("{{\"id\":\"2\",\"files\":[{{\"filename\":\"m.jar\",\"url\":\"{dl_url}\",\"size\":4}}]}}")
                    .into_bytes(),
            )
            }),
            route("/mr/project/9/version/9", || {
                (200, br#"{"id":"9","files":[]}"#.to_vec())
            }),
        ],
    );
    let mut p = ModPlatform::for_test(
        &format!("http://{mr_addr}/official"),
        &format!("http://{file_addr}/mirror"),
        &format!("http://{mr_addr}/mr"),
        "",
    );
    p.set_manager(fast_manager());

    // 端到端：解析 URL → 下载
    let path = temp_path("mr");
    let n = p
        .download_mod(Platform::Modrinth, "1", "2", &path, None)
        .await
        .expect("MR 下载应成功");
    assert_eq!(n, 4); // "MR!\n"
    assert_eq!(std::fs::read(&path).unwrap(), b"MR!\n");
    let _ = std::fs::remove_file(&path);

    // files 为空 → 明确报错
    let err = p
        .download_mod(Platform::Modrinth, "9", "9", &temp_path("nofiles"), None)
        .await
        .expect_err("无文件应失败");
    assert_eq!(err, "No files in version");
}
