//! 下载引擎本地测试：自起 HTTP 服务器（std TcpListener + 线程），
//! 走真实 reqwest 栈，不依赖外网。超时/重试参数用构建器调短。

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use mlccore::download::manager::{DownloadError, DownloadManager, TimeoutPhase};
use mlccore::download::{forgecdn_fallback, version_json_url, version_manifest_url};

// ---------------------------------------------------------------- 本地服务器

/// 服务器对单个请求的动作
enum Action {
    Respond {
        status: u16,
        body: Vec<u8>,
    },
    /// 发响应头 + 部分 body 后休眠不关流（停滞超时）
    Stall {
        status: u16,
        declared_len: usize,
        written: usize,
        sleep: Duration,
    },
    /// 先休眠再响应（首字节超时）
    SleepFirst {
        sleep: Duration,
        status: u16,
        body: Vec<u8>,
    },
}

/// 起一个本地服务器；handler 按请求序号（0 起）给动作。每连接一线程（并发）。
/// 另返回全局请求计数器供断言。
fn spawn_server(
    handler: impl Fn(usize) -> Action + Send + Sync + 'static,
) -> (SocketAddr, Arc<AtomicUsize>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");
    let counter = Arc::new(AtomicUsize::new(0));
    let counter2 = counter.clone();
    let handler = Arc::new(handler);
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let n = counter2.fetch_add(1, Ordering::SeqCst);
            let handler = handler.clone();
            std::thread::spawn(move || handle_conn(stream, n, handler.as_ref()));
        }
    });
    (addr, counter)
}

fn handle_conn(mut stream: TcpStream, n: usize, handler: &(impl Fn(usize) -> Action + ?Sized)) {
    // 读请求头到 \r\n\r\n（GET 无 body，够用）
    let mut buf = Vec::new();
    let mut byte = [0u8; 1];
    while let Ok(1) = stream.read(&mut byte) {
        buf.push(byte[0]);
        if buf.ends_with(b"\r\n\r\n") {
            break;
        }
    }
    let request_text = String::from_utf8_lossy(&buf);

    match handler(n) {
        Action::Respond { status, body } => {
            let head = format!(
                "HTTP/1.1 {status} T\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            );
            let _ = stream.write_all(head.as_bytes());
            let _ = stream.write_all(&body);
        }
        Action::Stall {
            status,
            declared_len,
            written,
            sleep,
        } => {
            let head =
                format!("HTTP/1.1 {status} T\r\nContent-Length: {declared_len}\r\nConnection: close\r\n\r\n");
            let _ = stream.write_all(head.as_bytes());
            let _ = stream.write_all(&vec![0u8; written]);
            let _ = stream.flush();
            std::thread::sleep(sleep); // 保持连接不开流，客户端应停滞超时
        }
        Action::SleepFirst {
            sleep,
            status,
            body,
        } => {
            std::thread::sleep(sleep); // 收到请求后装死
            let head = format!(
                "HTTP/1.1 {status} T\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            );
            let _ = stream.write_all(head.as_bytes());
            let _ = stream.write_all(&body);
        }
    }
    let _ = request_text; // 头部检查测试在 handler 闭包内用不到，如需再扩展
}

fn fast_manager(max_retries: u32) -> DownloadManager {
    DownloadManager::builder()
        .max_retries(max_retries)
        .retry_delay(Duration::from_millis(10))
        .timeouts(
            Duration::from_millis(400),
            Duration::from_millis(400),
            Duration::from_millis(300),
        )
        .build()
}

fn temp_path(tag: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("mlc-dl-test-{}-{tag}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    dir.join("out.bin")
}

// ---------------------------------------------------------------- 用例

#[tokio::test]
async fn 文件下载成功落盘与进度回调() {
    let body = b"hello mlc download engine".to_vec();
    let (addr, counter) = spawn_server(move |_| Action::Respond {
        status: 200,
        body: body.clone(),
    });
    let path = temp_path("ok");
    let mgr = fast_manager(0);

    let seen = Arc::new(AtomicUsize::new(0));
    let seen2 = seen.clone();
    let progress: mlccore::download::ProgressCallback = Arc::new(move |recv, total| {
        seen2.fetch_add(recv as usize, Ordering::SeqCst);
        assert_eq!(total, 25); // Content-Length
    });

    let n = mgr
        .download_file(&format!("http://{addr}/file.bin"), &path, Some(progress))
        .await
        .expect("下载应成功");
    assert_eq!(n, 25);
    assert_eq!(std::fs::read(&path).unwrap(), b"hello mlc download engine");
    assert_eq!(counter.load(Ordering::SeqCst), 1);
    assert_eq!(seen.load(Ordering::SeqCst), 25); // 进度回调累计字节数
    let _ = std::fs::remove_file(&path);
}

#[tokio::test]
async fn 失败重试后成功() {
    let (addr, counter) = spawn_server(|n| {
        if n < 2 {
            Action::Respond {
                status: 500,
                body: b"boom".into(),
            }
        } else {
            Action::Respond {
                status: 200,
                body: b"ok".into(),
            }
        }
    });
    let path = temp_path("retry");
    let mgr = fast_manager(2); // 3 次尝试：500、500、200

    let n = mgr
        .download_file(&format!("http://{addr}/f"), &path, None)
        .await
        .expect("第三次应成功");
    assert_eq!(n, 2);
    assert_eq!(counter.load(Ordering::SeqCst), 3);
    assert_eq!(std::fs::read(&path).unwrap(), b"ok");
    let _ = std::fs::remove_file(&path);
}

#[tokio::test]
async fn 重试耗尽后报状态码() {
    let (addr, counter) = spawn_server(|_| Action::Respond {
        status: 404,
        body: b"no".into(),
    });
    let path = temp_path("exhaust");
    let mgr = fast_manager(1); // 2 次尝试

    let err = mgr
        .download_file(&format!("http://{addr}/f"), &path, None)
        .await
        .expect_err("应失败");
    match err {
        DownloadError::HttpStatus { code, .. } => assert_eq!(code, 404),
        other => panic!("应为 HttpStatus: {other:?}"),
    }
    assert_eq!(counter.load(Ordering::SeqCst), 2);
    assert!(!path.exists(), "失败后不得留半成品");
}

#[tokio::test]
async fn 首字节超时文件与文本() {
    let (addr, _) = spawn_server(|_| Action::SleepFirst {
        sleep: Duration::from_secs(5),
        status: 200,
        body: b"late".into(),
    });
    let mgr = fast_manager(0);
    let url = format!("http://{addr}/slow");

    let err = mgr
        .download_file(&url, &temp_path("fb"), None)
        .await
        .expect_err("文件下载应首字节超时");
    assert!(matches!(
        err,
        DownloadError::Timeout {
            phase: TimeoutPhase::FirstByte,
            ..
        }
    ));

    let err = mgr
        .download_text(&url, &[])
        .await
        .expect_err("文本下载应首字节超时");
    assert!(matches!(
        err,
        DownloadError::Timeout {
            phase: TimeoutPhase::FirstByte,
            ..
        }
    ));
}

#[tokio::test]
async fn 传输停滞超时() {
    let (addr, _) = spawn_server(|_| Action::Stall {
        status: 200,
        declared_len: 100,
        written: 10,
        sleep: Duration::from_secs(5),
    });
    let path = temp_path("stall");
    let mgr = fast_manager(0);

    let err = mgr
        .download_file(&format!("http://{addr}/stall"), &path, None)
        .await
        .expect_err("应停滞超时");
    assert!(matches!(
        err,
        DownloadError::Timeout {
            phase: TimeoutPhase::Stall,
            ..
        }
    ));
    assert!(!path.exists(), "停滞失败后不得留半成品");
}

#[tokio::test]
async fn 并发限流不超上限() {
    static IN_FLIGHT: AtomicUsize = AtomicUsize::new(0);
    static MAX_SEEN: AtomicUsize = AtomicUsize::new(0);
    let (addr, _) = spawn_server(|_| {
        let cur = IN_FLIGHT.fetch_add(1, Ordering::SeqCst) + 1;
        MAX_SEEN.fetch_max(cur, Ordering::SeqCst);
        std::thread::sleep(Duration::from_millis(80));
        IN_FLIGHT.fetch_sub(1, Ordering::SeqCst);
        Action::Respond {
            status: 200,
            body: b"x".into(),
        }
    });
    let mgr = DownloadManager::builder()
        .max_in_flight(4)
        .max_retries(0)
        .timeouts(
            Duration::from_secs(5),
            Duration::from_secs(5),
            Duration::from_secs(5),
        )
        .build();

    let url = format!("http://{addr}/f");
    let mut tasks = Vec::new();
    for i in 0..12 {
        let mgr = &mgr;
        let url = url.clone();
        let path = temp_path("conc");
        tasks.push(async move {
            mgr.download_file(&format!("{url}?{i}"), &path, None)
                .await
                .unwrap();
            let _ = std::fs::remove_file(&path);
        });
    }
    futures_util::future::join_all(tasks).await;

    let max_seen = MAX_SEEN.load(Ordering::SeqCst);
    assert!(max_seen <= 4, "并发峰值 {max_seen} 超过限流上限 4");
    assert_eq!(max_seen, 4, "限流应放满 4 路并发");
}

#[tokio::test]
async fn 文本下载返回状态码与请求头() {
    // 服务器仅在带 x-api-key 头时 200（为 CF 镜像链铺路的行为验证）
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let mut s = stream;
            let mut buf = Vec::new();
            let mut byte = [0u8; 1];
            while let Ok(1) = s.read(&mut byte) {
                buf.push(byte[0]);
                if buf.ends_with(b"\r\n\r\n") {
                    break;
                }
            }
            let text = String::from_utf8_lossy(&buf);
            let authed = text.contains("x-api-key: secret");
            let (status, body) = if authed {
                (200, "{\"ok\":true}".to_string())
            } else {
                (401, "{\"error\":\"no key\"}".to_string())
            };
            let head = format!(
                "HTTP/1.1 {status} T\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            );
            let _ = s.write_all(head.as_bytes());
            let _ = s.write_all(body.as_bytes());
        }
    });

    let mgr = fast_manager(0);
    let url = format!("http://{addr}/api");

    // 无 key → 401 状态码可见（上层据 401/403/429 降级）
    let err = mgr.download_text(&url, &[]).await.expect_err("应 401");
    match err {
        DownloadError::HttpStatus { code, .. } => assert_eq!(code, 401),
        other => panic!("应为 HttpStatus: {other:?}"),
    }
    // 带 key → 200 + 文本 + JSON 解析
    let headers = vec![("x-api-key".to_string(), "secret".to_string())];
    let (code, text) = mgr.download_text(&url, &headers).await.expect("应 200");
    assert_eq!(code, 200);
    let (code, json) = mgr
        .download_json(&url, &headers)
        .await
        .expect("JSON 应解析");
    assert_eq!(code, 200);
    assert_eq!(json["ok"], true);
    let _ = text;
}

// ---------------------------------------------------------------- 纯函数

#[test]
fn forgecdn回退函数() {
    assert_eq!(
        forgecdn_fallback("https://edge.forgecdn.net/files/1234/567/mod.jar"),
        Some("https://mod.mcimirror.top/files/1234/567/mod.jar".to_string())
    );
    assert_eq!(
        forgecdn_fallback("https://mediafilez.forgecdn.net/files/x.jar"),
        None
    );
    assert_eq!(forgecdn_fallback("https://launchermeta.mojang.com/x"), None);
    assert_eq!(forgecdn_fallback("not a url"), None);
}

#[test]
fn url辅助函数钉死() {
    assert_eq!(
        version_manifest_url(),
        "https://launchermeta.mojang.com/mc/game/version_manifest.json"
    );
    // sha1("1.20.1") = 6614907faadad518c2deae727c8599e1fadd2513（sha1sum 钉死）
    assert_eq!(
        version_json_url("1.20.1"),
        "https://launchermeta.mojang.com/v1/packages/66/1.20.1.json"
    );
    assert_eq!(
        mlccore::download::assets_index_url("5"),
        "https://launchermeta.mojang.com/v1/packages/5.json"
    );
}
