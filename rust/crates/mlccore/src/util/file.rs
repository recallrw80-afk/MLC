//! 文件工具，对应 C++ `file_utils.cpp`：SHA1 / maven 路径 / zip 列举与解压。
//! zip 文件名：UTF-8 优先，否则 GBK（encoding_rs 单一代码路径，消灭 iconv 三分支）。

use std::io::Read;
use std::path::{Component, Path};

use sha1::{Digest, Sha1};

/// zip 条目名：UTF-8 优先，失败则 GBK
fn decode_zip_name(raw: &[u8]) -> String {
    match std::str::from_utf8(raw) {
        Ok(s) => s.to_string(),
        Err(_) => {
            let (cow, _, _) = encoding_rs::GBK.decode(raw);
            cow.into_owned()
        }
    }
}

fn open_zip(path: &Path) -> Option<zip::ZipArchive<std::fs::File>> {
    let f = std::fs::File::open(path).ok()?;
    zip::ZipArchive::new(f).ok()
}

/// 列出 zip 全部条目名（含目录项的 `/` 结尾；对齐 listZipEntries）
pub fn list_zip_entries(zip_path: &Path) -> Vec<String> {
    let Some(mut archive) = open_zip(zip_path) else {
        return Vec::new();
    };
    let mut names = Vec::with_capacity(archive.len());
    for i in 0..archive.len() {
        let Ok(entry) = archive.by_index(i) else {
            continue;
        };
        names.push(decode_zip_name(entry.name_raw()));
    }
    names
}

/// 读取单个 zip 条目内容（对齐 readZipEntry）
pub fn read_zip_entry(zip_path: &Path, entry_name: &str) -> Option<Vec<u8>> {
    let mut archive = open_zip(zip_path)?;
    // 先按精确名找，再按解码名找（GBK 名）
    for i in 0..archive.len() {
        let Ok(mut entry) = archive.by_index(i) else {
            continue;
        };
        let raw = entry.name_raw().to_vec();
        if raw == entry_name.as_bytes() || decode_zip_name(&raw) == entry_name {
            let mut buf = Vec::new();
            entry.read_to_end(&mut buf).ok()?;
            return Some(buf);
        }
    }
    None
}

/// 解出单个 zip 条目到文件（对齐 extractZipEntry）
pub fn extract_zip_entry(zip_path: &Path, entry_name: &str, dest_path: &Path) -> bool {
    let Some(mut archive) = open_zip(zip_path) else {
        return false;
    };
    for i in 0..archive.len() {
        let Ok(mut entry) = archive.by_index(i) else {
            continue;
        };
        let raw = entry.name_raw().to_vec();
        if raw == entry_name.as_bytes() || decode_zip_name(&raw) == entry_name {
            if let Some(parent) = dest_path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            let Ok(mut out) = std::fs::File::create(dest_path) else {
                return false;
            };
            return std::io::copy(&mut entry, &mut out).is_ok();
        }
    }
    false
}

/// 路径穿越防护：绝对路径或含 `..` 的条目跳过
fn is_unsafe_zip_entry(name: &str) -> bool {
    if name.starts_with('/') || name.starts_with('\\') {
        return true;
    }
    Path::new(name)
        .components()
        .any(|c| matches!(c, Component::ParentDir))
}

/// 解压整个 zip 到 dest（对齐 extractZip：跳过目录项与 zip-slip 条目）
/// 返回 (成功条目数, 失败条目数)；合法空包 → (0,0)
pub fn extract_zip(zip_path: &Path, dest_dir: &Path) -> Result<(usize, usize), String> {
    let Some(mut archive) = open_zip(zip_path) else {
        return Err(format!("Not a valid zip archive: {}", zip_path.display()));
    };
    let _ = std::fs::create_dir_all(dest_dir);
    let mut ok = 0usize;
    let mut failed = 0usize;
    for i in 0..archive.len() {
        let Ok(mut entry) = archive.by_index(i) else {
            failed += 1;
            continue;
        };
        let name = decode_zip_name(entry.name_raw());
        if name.ends_with('/') {
            continue;
        }
        if is_unsafe_zip_entry(&name) {
            tracing::warn!("extract_zip: skip unsafe entry {name}");
            continue;
        }
        let dest = dest_dir.join(&name);
        if let Some(parent) = dest.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        match std::fs::File::create(&dest) {
            Ok(mut out) => {
                if std::io::copy(&mut entry, &mut out).is_ok() {
                    ok += 1;
                } else {
                    failed += 1;
                }
            }
            Err(_) => failed += 1,
        }
    }
    if failed > 0 {
        tracing::warn!("extract_zip: {failed} entries failed");
    }
    Ok((ok, failed))
}

/// 递归删除目录（不顺符号链接：对包内容不可控场景的安全语义）
pub fn remove_tree(path: &Path) -> bool {
    if path.is_symlink() || path.is_file() {
        return std::fs::remove_file(path).is_ok();
    }
    if !path.exists() {
        return true;
    }
    let Ok(meta) = std::fs::symlink_metadata(path) else {
        return false;
    };
    if meta.file_type().is_symlink() {
        return std::fs::remove_file(path).is_ok();
    }
    std::fs::remove_dir_all(path).is_ok()
}

/// 递归复制目录
pub fn copy_dir(src: &Path, dst: &Path) -> bool {
    if !src.is_dir() {
        return false;
    }
    if std::fs::create_dir_all(dst).is_err() {
        return false;
    }
    let Ok(entries) = std::fs::read_dir(src) else {
        return false;
    };
    for entry in entries.flatten() {
        let from = entry.path();
        let to = dst.join(entry.file_name());
        if from.is_dir() {
            if !copy_dir(&from, &to) {
                return false;
            }
        } else if std::fs::copy(&from, &to).is_err() {
            return false;
        }
    }
    true
}

/// 文件 SHA1（小写十六进制）与期望值比对（大小写不敏感，对齐 QCryptographicHash）
pub fn verify_sha1(path: &Path, expected: &str) -> bool {
    let Ok(mut file) = std::fs::File::open(path) else {
        return false;
    };
    let mut hasher = Sha1::new();
    if std::io::copy(&mut file, &mut hasher).is_err() {
        return false;
    }
    let actual = hasher
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    actual.eq_ignore_ascii_case(expected)
}

/// maven 坐标 → 相对路径（对齐 mavenNameToPath）：
/// `group:artifact:version[@ext][:classifier]` → `group/artifact/version/artifact-version[-classifier].ext`
/// 非法（<3 段）返回空串
pub fn maven_name_to_path(name: &str) -> String {
    let (name, ext) = match name.split_once('@') {
        Some((n, e)) => (n, e),
        None => (name, "jar"),
    };
    let parts: Vec<&str> = name.split(':').collect();
    if parts.len() < 3 {
        return String::new();
    }
    let mut file = format!("{}-{}", parts[1], parts[2]);
    if parts.len() > 3 {
        file += &format!("-{}", parts[3]);
    }
    let group = parts[0].replace('.', "/");
    format!("{}/{}/{}/{}.{}", group, parts[1], parts[2], file, ext)
}

/// 资产哈希 → 子路径（前 2 字符 / 完整哈希）
pub fn asset_path_from_hash(hash: &str) -> String {
    if hash.len() >= 2 {
        format!("{}/{}", &hash[..2], hash)
    } else {
        hash.to_string()
    }
}

/// natives 文件判定（对齐 C++ isNativeFile：META-INF/ 排除，其余全收）
fn is_native_file(name: &str) -> bool {
    !name.starts_with("META-INF/")
}

/// 解压 native jar：只取文件名（拍平到 dest 根目录），META-INF/ 排除。
/// 返回提取出的文件绝对路径列表（对齐 extractNativesJar 的行为 + 返回值）。
pub fn extract_natives_jar(jar_path: &Path, dest_dir: &Path) -> Vec<String> {
    let mut extracted = Vec::new();
    let Ok(file) = std::fs::File::open(jar_path) else {
        return extracted;
    };
    let _ = std::fs::create_dir_all(dest_dir);

    let mut archive = match zip::ZipArchive::new(file) {
        Ok(a) => a,
        Err(_) => return extracted,
    };
    for i in 0..archive.len() {
        let Ok(mut entry) = archive.by_index(i) else {
            continue;
        };
        let name = entry.name().to_string();
        if !is_native_file(&name) {
            continue;
        }
        // 只取文件名（拍平）：native jar 内部是扁平的，目录项与包路径均忽略
        let Some(file_name) = Path::new(&name).file_name().and_then(|f| f.to_str()) else {
            continue;
        };
        let dest = dest_dir.join(file_name);
        let mut out = match std::fs::File::create(&dest) {
            Ok(f) => f,
            Err(_) => continue,
        };
        if std::io::copy(&mut entry, &mut out).is_ok() {
            extracted.push(dest.to_string_lossy().into_owned());
        }
    }
    extracted
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn sha1_校验与大小写不敏感() {
        let dir = std::env::temp_dir().join(format!("mlc-file-{}-sha1", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("f.bin");
        std::fs::write(&path, b"hello").unwrap();
        // sha1("hello") = aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d
        assert!(verify_sha1(
            &path,
            "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d"
        ));
        assert!(verify_sha1(
            &path,
            "AAF4C61DDCC5E8A2DABEDE0F3B482CD9AEA9434D"
        )); // 大小写
        assert!(!verify_sha1(&path, "wrong"));
        assert!(!verify_sha1(&dir.join("nonexistent.bin"), "anything"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn maven坐标转路径() {
        assert_eq!(
            maven_name_to_path("com.google.code.gson:gson:2.10.1"),
            "com/google/code/gson/gson/2.10.1/gson-2.10.1.jar"
        );
        assert_eq!(
            maven_name_to_path("org.lwjgl:lwjgl-glfw:3.3.3:natives-windows"),
            "org/lwjgl/lwjgl-glfw/3.3.3/lwjgl-glfw-3.3.3-natives-windows.jar"
        );
        assert_eq!(maven_name_to_path("invalid"), "");
        assert_eq!(maven_name_to_path("a:b:c@pom"), "a/b/c/b-c.pom");
    }

    #[test]
    fn 资产路径从哈希() {
        assert_eq!(asset_path_from_hash("abc123"), "ab/abc123");
        assert_eq!(asset_path_from_hash("ab"), "ab/ab");
        assert_eq!(asset_path_from_hash(""), "");
    }

    #[test]
    fn natives提取拍平与排除metainf() {
        let dir = std::env::temp_dir().join(format!("mlc-file-{}-nat", std::process::id()));
        let jar = dir.join("natives.jar");
        let dest = dir.join("out");
        std::fs::create_dir_all(&dir).unwrap();

        // 造一个 jar：含 META-INF/（应排除）+ 子目录里的 .so（应拍平到根）
        {
            let f = std::fs::File::create(&jar).unwrap();
            let mut zip = zip::ZipWriter::new(f);
            zip.add_directory("META-INF/", zip::write::SimpleFileOptions::default())
                .unwrap();
            zip.start_file(
                "META-INF/MANIFEST.MF",
                zip::write::SimpleFileOptions::default(),
            )
            .unwrap();
            zip.write_all(b"Manifest-Version: 1.0").unwrap();
            zip.start_file(
                "org/lwjgl/liblwjgl.so",
                zip::write::SimpleFileOptions::default(),
            )
            .unwrap();
            zip.write_all(b"SO-BYTES").unwrap();
            zip.start_file("libglfw.so", zip::write::SimpleFileOptions::default())
                .unwrap();
            zip.write_all(b"GLFW").unwrap();
            zip.finish().unwrap();
        }

        let out = extract_natives_jar(&jar, &dest);
        let mut names: Vec<String> = out
            .iter()
            .map(|p| {
                Path::new(p)
                    .file_name()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();
        names.sort();
        assert_eq!(names, vec!["libglfw.so", "liblwjgl.so"]); // META-INF 排除
        assert!(dest.join("liblwjgl.so").exists());
        assert!(
            !dest.join("org").join("lwjgl").exists(),
            "不应保留子目录结构"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
