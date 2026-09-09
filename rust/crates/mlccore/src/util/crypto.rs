//! 档案加密（DES + base64），对应 C++ `crypto_utils.cpp`。
//!
//! ⚠️ 硬约束（docs/rust-rewrite-plan.md 第 3 条）：密钥为 `"MLC" + "Liunx"`，
//! `Liunx` 拼写错误是有意的兼容负载，禁止"修正"。
//!
//! 兼容策略：C++ 的 DES 是手写实现（IP/FP 走 LSB 位序，自洽但**未必等同
//! 标准 DES/.NET**），已发布版本的用户数据（MLC.ini 的 CfApiKey、Authlib/*）
//! 由它写出——因此本模块按 C++ 代码逐行镜像，不使用标准 DES crate。
//! 字节级一致性由 golden 测试保证：向量见 `tests/golden/vectors.txt`
//! （由 `tests/golden/gen_vectors.cpp` 生成，其 DES 代码与 C++ 原文件逐行一致）。
//!
//! 算法链（与 C++ 一致）：密钥派生 = MD5(UTF-8 key) 前 8 个原始字节；
//! 明文转 UTF-8 → PKCS7 填充（块 8，对齐时补满块）→ DES-ECB → base64。

use base64::engine::general_purpose::STANDARD as B64;
use base64::Engine as _;
use md5::{Digest, Md5};

// ---- 置换表（与 C++ crypto_utils.cpp 逐字一致，禁止改动） ----

const IP: [usize; 64] = [
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61,
    53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
];

const FP: [usize; 64] = [
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25,
];

const E: [usize; 48] = [
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18,
    19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
];

// S[box][row][col]，对应 C++ S[8][4][16]
#[rustfmt::skip]
const S: [[[i32; 16]; 4]; 8] = [
    [[14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7],
     [0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8],
     [4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0],
     [15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13]],
    [[15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10],
     [3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5],
     [0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15],
     [13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9]],
    [[10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8],
     [13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1],
     [13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7],
     [1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12]],
    [[7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15],
     [13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9],
     [10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4],
     [3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14]],
    [[2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9],
     [14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6],
     [4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14],
     [11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3]],
    [[12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11],
     [10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8],
     [9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6],
     [4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13]],
    [[4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1],
     [13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6],
     [1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2],
     [6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12]],
    [[13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7],
     [1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2],
     [7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8],
     [2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11]],
];

const P: [usize; 32] = [
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19,
    13, 30, 6, 22, 11, 4, 25,
];

const PC1: [usize; 56] = [
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60,
    52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29,
    21, 13, 5, 28, 20, 12, 4,
];

const PC2: [usize; 48] = [
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2, 41, 52,
    31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
];

const SHIFTS: [u32; 16] = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1];

// ---- 块级原语（函数名对应 C++，循环顺序/位索引逐行镜像） ----

fn permute(input: u64, table: &[usize], mask: u64) -> u64 {
    let mut result = 0u64;
    for (i, &t) in table.iter().enumerate() {
        let bit = t - 1;
        if input & (1u64 << bit) != 0 {
            result |= 1u64 << i;
        }
    }
    result & mask
}

fn permute_64_to_56(key: u64) -> u64 {
    let mut result = 0u64;
    for (i, &pc) in PC1.iter().enumerate() {
        let bit = pc - 1;
        if key & (1u64 << (63 - bit)) != 0 {
            result |= 1u64 << (55 - i);
        }
    }
    result
}

fn permute_56_to_48(key: u64) -> u64 {
    let mut result = 0u64;
    for (i, &pc) in PC2.iter().enumerate() {
        let bit = pc - 1;
        if key & (1u64 << (55 - bit)) != 0 {
            result |= 1u64 << (47 - i);
        }
    }
    result
}

fn permute_ip(block: u64) -> u64 {
    permute(block, &IP, u64::MAX)
}

fn permute_fp(block: u64) -> u64 {
    permute(block, &FP, u64::MAX)
}

/// 轮函数，对应 C++ `f(r, subkey48)`
fn feistel(r: u32, subkey48: u64) -> u32 {
    // 扩展置换 E（32 → 48）
    let mut expanded = 0u64;
    for (i, &e) in E.iter().enumerate() {
        let bit = e - 1;
        if (r as u64) & (1u64 << (31 - bit)) != 0 {
            expanded |= 1u64 << (47 - i);
        }
    }
    let expanded = expanded ^ subkey48;

    // S 盒代换
    let mut result = 0u32;
    for (box_i, s_box) in S.iter().enumerate() {
        let shift = 42 - box_i * 6;
        let six_bits = ((expanded >> shift) & 0x3F) as u8;
        let row = (((six_bits >> 4) & 0x02) | (six_bits & 0x01)) as usize;
        let col = ((six_bits >> 1) & 0x0F) as usize;
        result = (result << 4) | s_box[row][col] as u32;
    }

    // 置换 P（32 → 32）
    let mut permuted = 0u32;
    for (i, &p) in P.iter().enumerate() {
        let bit = p - 1;
        if result & (1u32 << (31 - bit)) != 0 {
            permuted |= 1u32 << (31 - i);
        }
    }
    permuted
}

/// 16 轮子密钥。C++ 的 desEncrypt64/desDecrypt64 各自重算且代码相同，此处提取共享。
fn make_subkeys(key_bytes: &[u8]) -> [u64; 16] {
    let mut key = 0u64;
    for &b in key_bytes.iter().take(8) {
        key = (key << 8) | b as u64;
    }

    let pc1 = permute_64_to_56(key);
    let mut c = ((pc1 >> 28) & 0x0FFF_FFFF) as u32;
    let mut d = (pc1 & 0x0FFF_FFFF) as u32;

    let mut subkeys = [0u64; 16];
    for (round, sk) in subkeys.iter_mut().enumerate() {
        let s = SHIFTS[round];
        c = ((c << s) | (c >> (28 - s))) & 0x0FFF_FFFF;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFF_FFFF;
        let cd = ((c as u64) << 28) | d as u64;
        *sk = permute_56_to_48(cd);
    }
    subkeys
}

/// 对应 C++ `desEncrypt64`
fn des_encrypt64(mut block: u64, key_bytes: &[u8]) -> u64 {
    let subkeys = make_subkeys(key_bytes);
    block = permute_ip(block);
    let mut left = (block >> 32) as u32;
    let mut right = block as u32;

    for sk in subkeys.iter() {
        let temp = left;
        left = right;
        right = temp ^ feistel(right, *sk);
    }

    let out = ((right as u64) << 32) | left as u64;
    permute_fp(out)
}

/// 对应 C++ `desDecrypt64`（与加密仅轮序相反）
fn des_decrypt64(mut block: u64, key_bytes: &[u8]) -> u64 {
    let subkeys = make_subkeys(key_bytes);
    block = permute_ip(block);
    let mut left = (block >> 32) as u32;
    let mut right = block as u32;

    for sk in subkeys.iter().rev() {
        let temp = left;
        left = right;
        right = temp ^ feistel(right, *sk);
    }

    let out = ((right as u64) << 32) | left as u64;
    permute_fp(out)
}

/// DES-ECB，对应 C++ `desEcb`（尾块不足 8 字节补 0）
fn des_ecb(data: &[u8], key_bytes: &[u8], encrypt: bool) -> Vec<u8> {
    let mut result = Vec::with_capacity(data.len());
    for chunk in data.chunks(8) {
        let mut block_bytes = [0u8; 8];
        block_bytes[..chunk.len()].copy_from_slice(chunk);

        let mut block_val = 0u64;
        for &b in &block_bytes {
            block_val = (block_val << 8) | b as u64;
        }

        block_val = if encrypt {
            des_encrypt64(block_val, key_bytes)
        } else {
            des_decrypt64(block_val, key_bytes)
        };

        for j in (0..8).rev() {
            result.push((block_val >> (j * 8)) as u8);
        }
    }
    result
}

// ---- 填充 / 密钥派生 / 公开接口 ----

/// PKCS7（块 8；已对齐时补满块），对应 C++ `padPkcs7`
fn pad_pkcs7(data: &[u8], block_size: usize) -> Vec<u8> {
    let pad_len = block_size - data.len() % block_size;
    let mut padded = data.to_vec();
    padded.resize(padded.len() + pad_len, pad_len as u8);
    padded
}

/// 对应 C++ `unpadPkcs7`（padLen 越界时原样返回，不报错——按原实现保留）
fn unpad_pkcs7(data: &[u8]) -> &[u8] {
    if data.is_empty() {
        return data;
    }
    let pad_len = *data.last().unwrap() as usize;
    if !(1..=8).contains(&pad_len) {
        return data;
    }
    // C++ QByteArray::left 负数参数行为未定义，此路径实际不可达（数据恒为 8 的倍数），防御性返回
    if pad_len > data.len() {
        return data;
    }
    &data[..data.len() - pad_len]
}

/// DES 密钥 = MD5(UTF-8 key) 前 8 个**原始字节**（不是 hex 串），对应 C++ `deriveDesKey`
fn derive_des_key(key: &str) -> [u8; 8] {
    let mut hasher = Md5::new();
    hasher.update(key.as_bytes());
    let digest = hasher.finalize();
    let mut out = [0u8; 8];
    out.copy_from_slice(&digest[..8]);
    out
}

/// DES 加密：UTF-8 明文 → PKCS7 → DES-ECB → base64。对应 C++ `desEncrypt`
pub fn des_encrypt(plain_text: &str, key: &str) -> String {
    let key_bytes = derive_des_key(key);
    let data = pad_pkcs7(plain_text.as_bytes(), 8);
    B64.encode(des_ecb(&data, &key_bytes, true))
}

/// DES 解密：base64 → DES-ECB 解 → 去 PKCS7 → UTF-8。对应 C++ `desDecrypt`
/// （base64 非法时返回空串；C++ 侧宽松解码对非法输入同样不产生有意义结果）
pub fn des_decrypt(cipher_b64: &str, key: &str) -> String {
    let key_bytes = derive_des_key(key);
    let Ok(data) = B64.decode(cipher_b64.as_bytes()) else {
        return String::new();
    };
    if data.is_empty() {
        return String::new();
    }
    let decrypted = des_ecb(&data, &key_bytes, false);
    String::from_utf8_lossy(unpad_pkcs7(&decrypted)).into_owned()
}

/// MLC 加密密钥。⚠️ `"Liunx"` 拼写错误是有意的兼容负载，禁止修正。
/// 对应 C++ `pclEncryptKey()`（crypto_utils.h）
pub fn pcl_encrypt_key() -> &'static str {
    "MLCLiunx"
}

/// 用 MLC 标准密钥加密，对应 C++ `pclEncrypt`
pub fn pcl_encrypt(plain_text: &str) -> String {
    des_encrypt(plain_text, pcl_encrypt_key())
}

/// 用 MLC 标准密钥解密，对应 C++ `pclDecrypt`
pub fn pcl_decrypt(cipher_b64: &str) -> String {
    des_decrypt(cipher_b64, pcl_encrypt_key())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pcl_密钥是兼容负载() {
        assert_eq!(pcl_encrypt_key(), "MLCLiunx");
    }

    #[test]
    fn 空串与对齐边界往返() {
        // 空串 → 单个满填充块；8 字节对齐 → 补满块
        for plain in ["", "12345678", "1234567890123456", "a", "ab"] {
            let cipher = pcl_encrypt(plain);
            assert_eq!(pcl_decrypt(&cipher), plain, "roundtrip 失败: {plain:?}");
        }
        assert_eq!(pcl_encrypt("").len(), 12); // 8 字节块 → base64 12 字符
        assert_eq!(pcl_encrypt("12345678").len(), 24); // 8+8 填充 → 16 字节 → 24 字符
    }

    #[test]
    fn 中文与emoji往返() {
        for plain in [
            "测试中文abc",
            "emoji😀test",
            "有 空 格 与,逗号。句号！",
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        ] {
            assert_eq!(pcl_decrypt(&pcl_encrypt(plain)), plain);
        }
    }

    #[test]
    fn 非法base64与空密文返回空串() {
        assert_eq!(des_decrypt("!!!not-base64!!!", "MLCLiunx"), "");
        assert_eq!(des_decrypt("", "MLCLiunx"), "");
    }

    #[test]
    fn 自定义密钥往返() {
        let cipher = des_encrypt("user@example.com", "OtherKey123");
        assert_eq!(des_decrypt(&cipher, "OtherKey123"), "user@example.com");
        // 密钥不同必须解不出原文（MD5 派生使密钥空间差异生效）
        assert_ne!(des_decrypt(&cipher, "MLCLiunx"), "user@example.com");
    }

    #[test]
    fn 解密方向独立断言() {
        // 空串的 golden 向量（加密方向在 tests/crypto_golden.rs 全量覆盖）
        assert_eq!(pcl_decrypt("6w+LAFjWxG8="), "");
    }
}
