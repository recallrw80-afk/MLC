//! TUI 交互：create-vite / clack 风格选择器与问答。
//! 无 TTY 时调用方应回退到错误提示（C++ 同语义）。

use inquire::{Confirm, Password, Select, Text};

/// stdin/stdout 是否 TTY（无 TTY 时不弹交互）
pub fn is_tty() -> bool {
    // Windows: 用 std::io::IsTerminal
    use std::io::IsTerminal;
    std::io::stdin().is_terminal() && std::io::stdout().is_terminal()
}

/// 单选列表；取消/错误返回 None
pub fn select(title: &str, items: &[String]) -> Option<String> {
    if items.is_empty() {
        return None;
    }
    Select::new(title, items.to_vec()).prompt().ok()
}

/// 文本输入；def 为默认值（回车采用）
pub fn input(label: &str, def: &str, placeholder: &str) -> Option<String> {
    let mut t = Text::new(label);
    if !def.is_empty() {
        t = t.with_default(def);
    }
    if !placeholder.is_empty() {
        t = t.with_placeholder(placeholder);
    }
    t.prompt().ok()
}

/// 密码输入（掩码）
pub fn password(label: &str) -> Option<String> {
    Password::new(label).without_confirmation().prompt().ok()
}

/// 确认对话框
pub fn confirm(label: &str, def: bool) -> Option<bool> {
    Confirm::new(label).with_default(def).prompt().ok()
}
