//! mlc — 命令行前端（Rust 重写版）
//!
//! 只做参数解析与派发，不写业务逻辑（对应原 C++ cli/ 的分工）。
//! 30 个命令的名称/参数/中英 help/退出码必须逐条复刻，清单见
//! docs/rust-inventory.md 表 1；本骨架先实现 version 证明链路，其余随阶段 2 补齐。

use clap::Parser;

#[derive(Parser)]
#[command(
    name = "mlc",
    version,
    about = "MLC — MinecraftLauncherCLI (Rust rewrite WIP)"
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(clap::Subcommand)]
enum Command {
    /// 显示版本号 / Show version number
    Version,
}

fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .init();

    let cli = Cli::parse();
    match cli.command {
        // 与原实现一致：`mlc <name> <version>` 单行输出
        Some(Command::Version) | None => {
            println!("mlc {}", mlccore::GIT_DESCRIBE);
        }
    }
}
