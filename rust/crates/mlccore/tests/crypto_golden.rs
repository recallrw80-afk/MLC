//! 加密兼容层 golden 测试：与 C++ 实现的输出逐字节对照。
//!
//! 向量文件 tests/golden/vectors.txt 由 tests/golden/gen_vectors.cpp 生成——
//! 该生成器的 DES 代码从 sdk/src/util/crypto_utils.cpp 逐行拷贝，
//! 因此本测试通过 = Rust 实现与 C++ 实现字节级兼容（用户档案可无缝读写）。
//! 重新生成命令见 gen_vectors.cpp 文件头。

use std::fs;

#[test]
fn golden_向量逐条对照() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/golden/vectors.txt");
    let text = fs::read_to_string(path).expect("golden 向量文件缺失");

    let mut count = 0;
    for line in text.lines() {
        if line.starts_with('#') || line.trim().is_empty() {
            continue;
        }
        let mut parts = line.splitn(3, '\t');
        let (Some(plain), Some(key), Some(cipher)) = (parts.next(), parts.next(), parts.next())
        else {
            panic!("向量行格式错误（应为 plain\\tkey\\tcipher）: {line:?}");
        };

        assert_eq!(
            mlccore::util::crypto::des_encrypt(plain, key),
            cipher,
            "加密不一致: plain={plain:?} key={key:?}"
        );
        assert_eq!(
            mlccore::util::crypto::des_decrypt(cipher, key),
            plain,
            "解密不一致: cipher={cipher:?} key={key:?}"
        );
        count += 1;
    }
    assert!(count >= 13, "golden 向量数量异常（应为 13 条）: {count}");
}
