//! 参数工具，对应 C++ `arg_utils.cpp`：引号切分与 JVM/游戏参数去重。

/// 引号感知的 Java 参数切分（对齐 ArgUtils::splitJavaArgs）
pub fn split_java_args(s: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut in_quote = false;
    let mut current = String::new();
    for c in s.chars() {
        if c == '"' {
            in_quote = !in_quote;
        } else if c == ' ' && !in_quote {
            if !current.is_empty() {
                let t = current.trim().to_string();
                if !t.is_empty() {
                    args.push(t);
                }
                current.clear();
            }
        } else {
            current.push(c);
        }
    }
    let t = current.trim().to_string();
    if !t.is_empty() {
        args.push(t);
    }
    args
}

fn is_repeatable_flag(a: &str) -> bool {
    matches!(
        a,
        "--add-opens" | "--add-exports" | "--add-reads" | "--patch-module"
    )
}

fn is_value_flag(a: &str) -> bool {
    matches!(
        a,
        "-cp"
            | "-classpath"
            | "--class-path"
            | "-p"
            | "--module-path"
            | "--add-modules"
            | "--upgrade-module-path"
            | "--limit-modules"
            | "--username"
            | "--version"
            | "--gameDir"
            | "--assetsDir"
            | "--assetIndex"
            | "--uuid"
            | "--accessToken"
            | "--clientId"
            | "--xuid"
            | "--userType"
            | "--versionType"
            | "--width"
            | "--height"
            | "--server"
            | "--port"
            | "--session"
            | "--userProperties"
            | "--tweakClass"
            | "--launchTarget"
            | "--fml.forgeVersion"
            | "--fml.mcVersion"
            | "--fml.forgeGroup"
            | "--fml.mcpVersion"
            | "--quickPlaySingleplayer"
            | "--quickPlayMultiplayer"
            | "--quickPlayRealms"
    )
}

/// 参数去重 key：可重复 flag 返回空（不去重）；-Xmx2G → -Xmx；其余取 `=`/空格 前段
fn key_of(a: &str) -> String {
    if is_repeatable_flag(a) {
        return String::new();
    }
    // ^(-X(?!X)[a-zA-Z]+)：-Xmx2G → -Xmx（排除 -XX:）
    if a.starts_with("-X") && !a.starts_with("-XX") {
        return a
            .chars()
            .take_while(|c| c.is_ascii_alphabetic() || *c == '-')
            .collect();
    }
    a.split(['=', ' ']).next().unwrap_or(a).to_string()
}

/// 对齐 ArgUtils::deduplicateArgs：保留每个 key 最后一次出现；带值 flag 与值同去同留
pub fn deduplicate_args(args: &[String]) -> Vec<String> {
    let n = args.len();
    let mut value_owner: Vec<Option<usize>> = vec![None; n];
    for i in 0..n.saturating_sub(1) {
        if is_value_flag(&args[i]) && value_owner[i].is_none() {
            value_owner[i + 1] = Some(i);
        }
    }

    let mut last_index: std::collections::HashMap<String, usize> = Default::default();
    for i in 0..n {
        if value_owner[i].is_some() {
            continue;
        }
        let key = key_of(&args[i]);
        if !key.is_empty() {
            last_index.insert(key, i);
        }
    }

    let mut result = Vec::new();
    for i in 0..n {
        if let Some(owner) = value_owner[i] {
            if last_index.get(&args[owner]).copied() == Some(owner) {
                result.push(args[i].clone());
            }
            continue;
        }
        let key = key_of(&args[i]);
        if key.is_empty() || last_index.get(&key).copied() == Some(i) {
            result.push(args[i].clone());
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(s: &[&str]) -> Vec<String> {
        s.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn 引号切分() {
        assert_eq!(
            split_java_args(r#"-Dfoo="a b" -Xmx1G"#),
            v(&["-Dfoo=a b", "-Xmx1G"])
        );
        assert_eq!(split_java_args("  a   b  "), v(&["a", "b"]));
    }

    #[test]
    fn 去重保留最后且值随行() {
        let args = v(&[
            "-Xmx1G",
            "-Xmx2G",
            "-cp",
            "old",
            "-cp",
            "new",
            "--add-opens",
            "java.base/a=ALL-UNNAMED",
            "--add-opens",
            "java.base/b=ALL-UNNAMED",
        ]);
        let out = deduplicate_args(&args);
        assert_eq!(
            out,
            v(&[
                "-Xmx2G",
                "-cp",
                "new",
                "--add-opens",
                "java.base/a=ALL-UNNAMED",
                "--add-opens",
                "java.base/b=ALL-UNNAMED",
            ])
        );
    }
}
