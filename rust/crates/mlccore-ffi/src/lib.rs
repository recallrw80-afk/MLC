//! mlccore 的 C ABI 包装，供 Qt GUI（mlc-gui 的 4 个 bridge）链接。
//!
//! 约定（阶段 3 实施时遵守）：
//! - 字符串出参所有权归调用方，统一用 `mlc_string_free` 释放；入参为 NUL 结尾 UTF-8
//! - 进度/日志事件经回调函数指针上报，GUI 侧转 Qt signal
//! - tokio runtime 句柄常驻（Qt 调用进来，Rust 内部 block_on）
//! - 头文件由 cbindgen 生成（见本目录 cbindgen.toml），API 子集见 docs/rust-inventory.md 表 2

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::sync::OnceLock;

/// 返回版本字符串（静态存储，调用方禁止释放）
#[no_mangle]
pub extern "C" fn mlc_version() -> *const c_char {
    // 注意：GIT_DESCRIBE 由 mlccore 的 build.rs 注入，只对该 crate 可见，
    // 这里必须经 mlccore::GIT_DESCRIBE 常量取（env! 在本 crate 拿不到）。
    static VERSION: OnceLock<CString> = OnceLock::new();
    VERSION
        .get_or_init(|| CString::new(mlccore::GIT_DESCRIBE).expect("版本号不含 NUL"))
        .as_ptr()
}

/// 释放由本库返回的堆字符串（占位约定；现阶段无 API 返回堆字符串）
///
/// # Safety
/// `s` 必须来自本库返回的 CString::into_raw，且只能释放一次。
#[no_mangle]
pub unsafe extern "C" fn mlc_string_free(s: *mut c_char) {
    if !s.is_null() {
        // SAFETY: 调用方守约（指针来自本库 into_raw）
        drop(unsafe { CString::from_raw(s) });
    }
}

// CStr 辅助预留给阶段 3 的入参解析：
#[allow(dead_code)]
unsafe fn cstr_to_str<'a>(s: *const c_char) -> Option<&'a str> {
    if s.is_null() {
        return None;
    }
    // SAFETY: 调用方保证 NUL 结尾 UTF-8
    unsafe { CStr::from_ptr(s) }.to_str().ok()
}
