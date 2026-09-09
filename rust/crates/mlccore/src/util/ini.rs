//! QSettings IniFormat 编解码器，逐规则镜像 Qt 6.11.1 `qsettings.cpp`
//! （iniEscapedKey / iniUnescapedKey / iniEscapedString / iniUnescapedStringList /
//!   readIniLine / readIniFile / readIniSection / writeIniFile）。
//!
//! 这是磁盘兼容层的地基：C++ 版写出的 MLC.ini 必须能被本模块无损读取，
//! 本模块写出的文件必须能被 C++ 版（QSettings）无损读取。
//! 逐条测试按 Qt 源码行为钉死（非猜测），见文件尾 #[cfg(test)]。
//!
//! 布局要点：QSettings 把完整键路径 `MLC/Instances/<dir>` 按第一个 `/` 分组——
//! 节 = 首段（[MLC]），其余段留在键内且 `/` 转为 `\`（键转义规则），
//! 因此 MLC 的全部键实际都在 [MLC] 一节里，以 `Instances\xxx`、
//! `Profile\<uuid>\Name` 这样的反斜杠嵌套键存在。

use std::collections::BTreeMap;

// ---------------------------------------------------------------- 键转义

/// 键/节名转义（UTF-16 码元粒度，对应 `iniEscapedKey`）：
/// `/`→`\`；`[A-Za-z0-9_-.]` 原样；≤0xFF → `%XX`（大写）；>0xFF → `%UXXXX`（大写）。
/// 必须按 UTF-16 码元迭代：增补平面字符（如 emoji）会拆成代理对、各转义一个 %U——
/// 与 Qt 字节级一致，不能用 Rust 的 char 迭代。
pub fn escape_key(key: &str) -> String {
    let mut result = String::with_capacity(key.len());
    for u in key.encode_utf16() {
        if u == 0x2F {
            // '/'
            result.push('\\');
        } else if matches!(u, 0x61..=0x7A | 0x41..=0x5A | 0x30..=0x39) // a-z A-Z 0-9
            || u == b'_' as u16
            || u == b'-' as u16
            || u == b'.' as u16
        {
            result.push(u as u8 as char);
        } else if u <= 0xFF {
            result.push('%');
            result.push(to_hex_upper((u >> 4) as u8));
            result.push(to_hex_upper((u & 0xF) as u8));
        } else {
            result.push_str(&format!("%U{u:04X}"));
        }
    }
    result
}

fn to_hex_upper(v: u8) -> char {
    match v {
        0..=9 => (b'0' + v) as char,
        _ => (b'A' + v - 10) as char,
    }
}

/// 键/节名反转义（对应 `iniUnescapedKey`）：`\`→`/`；`%XX`/`%UXXXX` 解码；
/// 非法 `%` 原样保留。大小写标志（lowercaseOnly）仅用于 Qt 的大小写敏感性
/// 决策，IniFormat 恒为 CaseSensitive，此处省略。
fn unescape_key(raw: &str) -> String {
    let chars: Vec<char> = raw.chars().collect();
    let mut out: Vec<u16> = Vec::with_capacity(chars.len());
    let n = chars.len();
    let mut i = 0;
    while i < n {
        let ch = chars[i];
        if ch == '\\' {
            out.push(0x2F); // '/' 的 UTF-16 码元
            i += 1;
            continue;
        }
        if ch != '%' || i == n - 1 {
            let mut buf = [0u16; 2];
            out.extend_from_slice(ch.encode_utf16(&mut buf));
            i += 1;
            continue;
        }
        // ch == '%'，且后面还有字符
        let mut num_digits = 2usize;
        let mut first = i + 1;
        if chars[first] == 'U' {
            first += 1;
            num_digits = 4;
        }
        if first + num_digits > n {
            out.push('%' as u16);
            i += 1;
            continue;
        }
        let hex: String = chars[first..first + num_digits].iter().collect();
        match u16::from_str_radix(&hex, 16) {
            Ok(v) => {
                out.push(v);
                i = first + num_digits;
            }
            Err(_) => {
                out.push('%' as u16);
                i += 1;
            }
        }
    }
    String::from_utf16_lossy(&out)
}

// ---------------------------------------------------------------- 值转义

/// 值转义（对应 `iniEscapedString`，QStringList 场景 MLC 不使用，不实现列表写）：
/// 含 `;` `,` `=` 或（转义后）首/尾为空格 → 加引号；控制字符 `\0\a\b\f\n\r\t\v` 用
/// 字母转义；≤0x1F 其余 → `\x` + 小写最短十六进制，且下一字符若为十六进制数字
/// 则强制再转义（防止读方向贪婪吞位）；`"` 与 `\` 反斜杠转义；其余（含 ≥0x7F）
/// 原样输出 UTF-8。
pub fn escape_value(s: &str) -> String {
    let mut result = String::with_capacity(s.len());
    let start_len = result.len(); // 恒为 0，占位以对齐 Qt 的 startPos 语义
    let _ = start_len;
    let mut needs_quotes = false;
    let mut escape_next_if_digit = false;

    for ch in s.chars() {
        let u = ch as u32;
        if u == 0x3B || u == 0x2C || u == 0x3D {
            // ';' ',' '='
            needs_quotes = true;
        }
        if escape_next_if_digit && ch.is_ascii_hexdigit() {
            result.push_str(&format!("\\x{u:x}"));
            continue;
        }
        escape_next_if_digit = false;

        match u {
            0x00 => {
                result.push_str("\\0");
                escape_next_if_digit = true;
            }
            0x07 => result.push_str("\\a"),
            0x08 => result.push_str("\\b"),
            0x0C => result.push_str("\\f"),
            0x0A => result.push_str("\\n"),
            0x0D => result.push_str("\\r"),
            0x09 => result.push_str("\\t"),
            0x0B => result.push_str("\\v"),
            0x22 | 0x5C => {
                // '"' '\'
                result.push('\\');
                result.push(ch);
            }
            _ => {
                if u <= 0x1F {
                    result.push_str(&format!("\\x{u:x}"));
                    escape_next_if_digit = true;
                } else {
                    // Qt 经 toUtf8 逐字符写入（状态机处理代理对）；
                    // Rust 的 char 即完整码点，直接 UTF-8 输出，结果一致
                    let mut buf = [0u8; 4];
                    result.push_str(ch.encode_utf8(&mut buf));
                }
            }
        }
    }

    if needs_quotes || (!result.is_empty() && (result.starts_with(' ') || result.ends_with(' '))) {
        result.insert(0, '"');
        result.push('"');
    }
    result
}

/// 值反转义结果。`List` 对应 QSettings 的 QStringList（MLC 数据不产生，
/// 读方向为兼容手改文件而保留）。
#[derive(Debug, Clone, PartialEq)]
pub enum IniValue {
    Single(String),
    List(Vec<String>),
}

/// 值反转义（对应 `iniUnescapedStringList` 的状态机移植）：
/// 跳过首部空白；引号切换（引号内 `,` 不分段）；转义表
/// `a b f n r t v " ? ' \`；`\x` 贪婪读十六进制（u16 回绕）；`\` + 八进制贪婪读；
/// `\` + `\n`/`\r` 为续行（含 `\r\n`/`\n\r` 配对）；`\` + 其他 → 该字符被丢弃；
/// 未加引号的值截尾空白（但不超过最后一段转义/起点）；`,`（引号外）分段为列表。
pub fn unescape_value(raw: &str) -> IniValue {
    let chars: Vec<char> = raw.chars().collect();
    let n = chars.len();
    let mut result = String::new();
    let mut list: Vec<String> = Vec::new();
    let mut is_list = false;
    let mut in_quotes = false;
    let mut current_value_quoted = false;
    let mut chop_limit; // 每轮 StNormal 入口都会先赋值
    let mut i = 0;

    'outer: loop {
        // StSkipSpaces
        while i < n && (chars[i] == ' ' || chars[i] == '\t') {
            i += 1;
        }
        // StNormal
        chop_limit = result.len();
        while i < n {
            match chars[i] {
                '\\' => {
                    i += 1;
                    if i >= n {
                        break 'outer; // 行尾孤立反斜杠：结束
                    }
                    let c = chars[i];
                    i += 1;
                    match c {
                        'a' => result.push('\x07'),
                        'b' => result.push('\x08'),
                        'f' => result.push('\x0C'),
                        'n' => result.push('\n'),
                        'r' => result.push('\r'),
                        't' => result.push('\t'),
                        'v' => result.push('\x0B'),
                        '"' => result.push('"'),
                        '?' => result.push('?'),
                        '\'' => result.push('\''),
                        '\\' => result.push('\\'),
                        'x' => {
                            // 贪婪读十六进制（u16 回绕，对齐 char16_t 溢出）
                            let mut val: u16 = 0;
                            while i < n && chars[i].is_ascii_hexdigit() {
                                val = val
                                    .wrapping_shl(4)
                                    .wrapping_add(chars[i].to_digit(16).unwrap() as u16);
                                i += 1;
                            }
                            result.push(char::from_u32(val as u32).unwrap_or('\u{FFFD}'));
                        }
                        c if ('0'..='7').contains(&c) => {
                            // 八进制贪婪读
                            let mut val: u16 = c.to_digit(8).unwrap() as u16;
                            while i < n && ('0'..='7').contains(&chars[i]) {
                                val = val
                                    .wrapping_shl(3)
                                    .wrapping_add(chars[i].to_digit(8).unwrap() as u16);
                                i += 1;
                            }
                            result.push(char::from_u32(val as u32).unwrap_or('\u{FFFD}'));
                        }
                        '\n' | '\r' => {
                            // 续行：\n、\r、\r\n、\n\r 均为合法续行终止组合
                            if let Some(&ch2) = chars.get(i) {
                                if (ch2 == '\n' || ch2 == '\r') && ch2 != c {
                                    i += 1;
                                }
                            }
                        }
                        _ => { /* 其他转义字符被丢弃（对齐 Qt） */ }
                    }
                    chop_limit = result.len();
                }
                '"' => {
                    i += 1;
                    current_value_quoted = true;
                    in_quotes = !in_quotes;
                    if !in_quotes {
                        continue 'outer; // 闭引号 → StSkipSpaces
                    }
                }
                ',' if !in_quotes => {
                    i += 1;
                    if !current_value_quoted {
                        chop_trailing_spaces(&mut result, chop_limit);
                    }
                    if !is_list {
                        is_list = true;
                        list.clear();
                    }
                    list.push(std::mem::take(&mut result));
                    current_value_quoted = false;
                    continue 'outer; // 逗号后 → StSkipSpaces
                }
                _ => {
                    // 默认：吃到下一个 `\` `"` `,` 为止
                    let mut j = i + 1;
                    while j < n && !matches!(chars[j], '\\' | '"' | ',') {
                        j += 1;
                    }
                    result.extend(&chars[i..j]);
                    i = j;
                }
            }
        }
        break;
    }

    if is_list {
        list.push(result);
        IniValue::List(list)
    } else {
        if !current_value_quoted {
            chop_trailing_spaces(&mut result, chop_limit);
        }
        IniValue::Single(result)
    }
}

fn chop_trailing_spaces(s: &mut String, limit: usize) {
    while s.len() > limit && matches!(s.as_bytes().last(), Some(b' ') | Some(b'\t')) {
        s.truncate(s.len() - 1);
    }
}

// ---------------------------------------------------------------- 文件读写

/// 单行类型（readIniLine 的产出）：字节区间 [start, end) + 引号外首个 `=` 的位置
struct LogicalLine {
    start: usize,
    end: usize,
    equals: Option<usize>,
}

/// 行扫描器，对应 `readIniLine`。Space = `\t \n \r 空格`；Special = `\n \r " ; = \`。
/// 规则：行首 `;` → 整个注释行透明跳过（含行尾后随空白）；`\` 吞下一字符
/// （`\`+`\n`/`\r` 为续行，`\r\n`/`\n\r` 成对吞）；`"` 切换引号态（引号内换行
/// 不截行）；引号外的首个 `=` 记为分隔符；行首孤立换行吸收进 lineStart。
fn next_logical_line(bytes: &[u8], pos: &mut usize) -> Option<LogicalLine> {
    let n = bytes.len();
    let mut line_start = *pos;
    while line_start < n && matches!(bytes[line_start], b' ' | b'\t' | b'\n' | b'\r') {
        line_start += 1;
    }
    if line_start >= n {
        *pos = n;
        return None;
    }

    let mut in_quotes = false;
    let mut equals: Option<usize> = None;
    let mut i = line_start;
    let mut line_end = n;
    while i < n {
        let ch = bytes[i];
        if !matches!(ch, b'\n' | b'\r' | b'"' | b';' | b'=' | b'\\') {
            i += 1;
            continue;
        }
        i += 1;
        if ch == b'=' {
            if !in_quotes && equals.is_none() {
                equals = Some(i - 1);
            }
        } else if ch == b'\n' || ch == b'\r' {
            if i == line_start + 1 {
                line_start += 1; // 行首孤立换行：吸收
            } else if !in_quotes {
                i -= 1; // 终止符留给下一轮的空白吸收
                line_end = i;
                break;
            }
            // 引号内换行：属于行内容，继续
        } else if ch == b'\\' {
            if i < n {
                let esc = bytes[i];
                i += 1;
                if i < n {
                    let ch2 = bytes[i];
                    if (esc == b'\n' && ch2 == b'\r') || (esc == b'\r' && ch2 == b'\n') {
                        i += 1;
                    }
                }
            }
        } else if ch == b'"' {
            in_quotes = !in_quotes;
        } else {
            // ';'
            if i == line_start + 1 {
                // 行首注释：跳到行尾，再吸收后随空白，行起点后移
                while i < n && bytes[i] != b'\n' && bytes[i] != b'\r' {
                    i += 1;
                }
                while i < n && matches!(bytes[i], b' ' | b'\t' | b'\n' | b'\r') {
                    i += 1;
                }
                line_start = i;
            } else if !in_quotes {
                i -= 1;
                line_end = i;
                break;
            }
        }
    }

    if line_end - line_start == 0 {
        *pos = n;
        return None;
    }
    *pos = i.max(line_start);
    Some(LogicalLine {
        start: line_start,
        end: line_end,
        equals,
    })
}

fn ini_trim(s: &[u8]) -> &[u8] {
    s.iter()
        .position(|b| !matches!(b, b' ' | b'\t' | b'\r' | b'\n' | 0x0B | 0x0C))
        .map(|st| {
            let en = s
                .iter()
                .rposition(|b| !matches!(b, b' ' | b'\t' | b'\r' | b'\n' | 0x0B | 0x0C))
                .unwrap();
            &s[st..=en]
        })
        .unwrap_or(&[])
}

/// 解析 INI 文本 → 完整键路径（`/` 连接）→ 值。
/// 返回 (条目按文件顺序, 是否有格式错误)。`None` 值 = `@Invalid()`（null，
/// QSettings 视为缺省）；QStringList 值以 ", " 连接为单串（MLC 数据不产生列表）。
/// 未知 `@xxx(...)` 前缀按原样透传（对齐 stringToVariant 的兜底分支）。
pub fn parse(text: &str) -> (Vec<(String, Option<String>)>, bool) {
    let data = text.strip_prefix('\u{FEFF}').unwrap_or(text); // UTF-8 BOM
    let bytes = data.as_bytes();
    let mut out = Vec::new();
    let mut format_error = false;
    let mut section_prefix = String::new(); // 如 "MLC/"；[General] 为空串
    let mut pos = 0usize;

    while let Some(line) = next_logical_line(bytes, &mut pos) {
        let raw = &bytes[line.start..line.end];
        if raw[0] == b'[' {
            // 节头：到行内首个 `]`（无则格式错误、取整行）
            match raw.iter().position(|b| *b == b']') {
                Some(idx) => {
                    let name_raw = String::from_utf8_lossy(ini_trim(&raw[1..idx])).into_owned();
                    if name_raw.eq_ignore_ascii_case("general") {
                        section_prefix.clear();
                    } else if name_raw.eq_ignore_ascii_case("%general") {
                        section_prefix = "general/".to_string();
                    } else {
                        section_prefix = format!("{}/", unescape_key(&name_raw));
                    }
                }
                None => {
                    format_error = true;
                }
            }
            continue;
        }
        match line.equals {
            None => {
                // 无 `=` 且非行首注释 → 格式错误（行首注释已在扫描器内跳过）
                format_error = true;
            }
            Some(eq) => {
                let key_raw =
                    String::from_utf8_lossy(ini_trim(&raw[..eq - line.start])).into_owned();
                let value_raw = String::from_utf8_lossy(&raw[eq + 1 - line.start..]).into_owned();
                let full_key = format!("{section_prefix}{}", unescape_key(&key_raw));
                // @ 前缀处理对应 stringToVariant（转义之后的下一层）：
                // @Invalid() → null；@@x → 剥一层 @；其余按原字符串
                let value = match unescape_value(&value_raw) {
                    IniValue::Single(s) => {
                        if s == "@Invalid()" {
                            None
                        } else if let Some(rest) = s.strip_prefix("@@") {
                            Some(rest.to_string())
                        } else {
                            Some(s)
                        }
                    }
                    IniValue::List(l) => Some(l.join(", ")),
                };
                out.push((full_key, value));
            }
        }
    }
    (out, format_error)
}

/// 序列化为 INI 文本，对应 `writeIniFile`：按完整键的首段分组为节；
/// 节按首键出现顺序排列、键按插入序；空节名 → `[General]`、
/// "general"（忽略大小写）→ `[%General]`；行尾符由平台决定（Q_OS_WIN → \r\n）。
pub fn serialize(entries: &[(String, String)]) -> String {
    serialize_with_eol(entries, if cfg!(windows) { "\r\n" } else { "\n" })
}

/// 指定行尾符的序列化（测试用，使钉死样例与平台无关）
pub fn serialize_with_eol(entries: &[(String, String)], eol: &str) -> String {
    // 节分组：保持节首现顺序与节内键的插入序（Settings 层保证同键唯一、更新原位）
    let mut sections: Vec<(String, Vec<(String, String)>)> = Vec::new();
    let mut section_index: BTreeMap<String, usize> = BTreeMap::new();
    for (full_key, value) in entries {
        let (section, rest) = match full_key.split_once('/') {
            Some((s, r)) => (s.to_string(), r.to_string()),
            None => (String::new(), full_key.clone()),
        };
        let idx = match section_index.get(&section) {
            Some(i) => *i,
            None => {
                sections.push((section.clone(), Vec::new()));
                section_index.insert(section, sections.len() - 1);
                sections.len() - 1
            }
        };
        debug_assert!(
            !sections[idx].1.iter().any(|(k, _)| k == &rest),
            "重复键 {full_key}（Settings 层应保证唯一）"
        );
        sections[idx].1.push((rest, value.clone()));
    }

    let mut out = String::new();
    for (sec_no, (section, keys)) in sections.iter().enumerate() {
        let escaped = escape_key(section);
        if sec_no != 0 {
            out.push_str(eol);
        }
        if escaped.is_empty() {
            out.push_str("[General]");
        } else if escaped.eq_ignore_ascii_case("general") {
            out.push_str("[%General]");
        } else {
            out.push('[');
            out.push_str(&escaped);
            out.push(']');
        }
        out.push_str(eol);
        for (key, value) in keys {
            out.push_str(&escape_key(key));
            out.push('=');
            out.push_str(&escape_value(value));
            out.push_str(eol);
        }
    }
    out
}

// ---------------------------------------------------------------- 测试

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 键转义与qt规则一致() {
        assert_eq!(escape_key("MLC"), "MLC");
        assert_eq!(escape_key("Profile/uuid-x/Name"), "Profile\\uuid-x\\Name");
        assert_eq!(escape_key("我的包"), "%U6211%U7684%U5305");
        assert_eq!(escape_key("a b"), "a%20b");
        assert_eq!(escape_key("100%"), "100%25");
        assert_eq!(escape_key("a.b-c_d"), "a.b-c_d");
        // 增补平面字符按 UTF-16 代理对逐码元转义（Qt 逐 QChar 迭代）
        assert_eq!(escape_key("😀"), "%UD83D%UDE00");
    }

    #[test]
    fn 键反转义还原与非法输入兜底() {
        assert_eq!(unescape_key("Profile\\uuid-x\\Name"), "Profile/uuid-x/Name");
        assert_eq!(unescape_key("%U6211%U7684%U5305"), "我的包");
        assert_eq!(unescape_key("a%20b"), "a b");
        // 非法 % → 原样；孤立 % → 原样；小写十六进制也接受（对齐 toUShort）
        assert_eq!(unescape_key("a%zzb"), "a%zzb");
        assert_eq!(unescape_key("abc%"), "abc%");
        assert_eq!(unescape_key("%2f"), "/");
        // 读写互逆
        for k in ["MLC", "Profile/uuid-x/Name", "我的包", "a b", "100%", "😀"] {
            assert_eq!(unescape_key(&escape_key(k)), k);
        }
    }

    #[test]
    fn 值转义与qt规则一致() {
        assert_eq!(escape_value("hello"), "hello");
        assert_eq!(escape_value(""), "");
        assert_eq!(escape_value("a=b"), "\"a=b\"");
        assert_eq!(escape_value("a;b"), "\"a;b\"");
        assert_eq!(escape_value("a,b"), "\"a,b\"");
        assert_eq!(escape_value(" x"), "\" x\"");
        assert_eq!(escape_value("x "), "\"x \"");
        assert_eq!(escape_value("a\nb"), "a\\nb");
        assert_eq!(escape_value("a\\b"), "a\\\\b");
        assert_eq!(escape_value("a\"b"), "a\\\"b");
        assert_eq!(escape_value("a\tb"), "a\\tb");
        assert_eq!(escape_value("\x01"), "\\x1");
        // \x 后跟十六进制数字 → 强制再转义（防读方向贪婪吞位）
        assert_eq!(escape_value("\x01a"), "\\x1\\x61");
        assert_eq!(escape_value("\x01x"), "\\x1x"); // 'x' 非十六进制数字，不再转义
                                                    // ≥0x7F（含中文）原样 UTF-8
        assert_eq!(escape_value("中文"), "中文");
        assert_eq!(escape_value("abc+/="), "\"abc+/=\"");
    }

    #[test]
    fn 值反转义按qt状态机() {
        // 写读互逆
        for v in [
            "hello", "a=b", " x ", "a\nb", "a\\b", "a\"b", "\x01a", "中文", "abc+/=",
        ] {
            match unescape_value(&escape_value(v)) {
                IniValue::Single(s) => assert_eq!(s, v, "roundtrip: {v:?}"),
                other => panic!("应为 Single: {other:?}"),
            }
        }
        // 读方向特例
        assert_eq!(unescape_value("\\x41"), IniValue::Single("A".into()));
        assert_eq!(unescape_value("\\10"), IniValue::Single("\x08".into())); // 八进制
        assert_eq!(unescape_value("val   "), IniValue::Single("val".into())); // 截尾空白
        assert_eq!(unescape_value(" val "), IniValue::Single("val".into())); // 未加引号：首尾空白都被去掉
        assert_eq!(
            unescape_value("\" val \""),
            IniValue::Single(" val ".into()) // 引号保空白
        );
        assert_eq!(
            unescape_value("\\x1\\x61"),
            IniValue::Single("\x01a".into())
        );
        assert_eq!(
            unescape_value("a, b"),
            IniValue::List(vec!["a".into(), "b".into()])
        );
        assert_eq!(unescape_value("\\z"), IniValue::Single("".into())); // 未知转义 → 丢弃
        assert_eq!(unescape_value(""), IniValue::Single("".into()));
    }

    #[test]
    fn 序列化钉死样例() {
        // 注意：值原样 UTF-8（%U 转义只用于键/节名），这是 Qt writeIniFile 的行为
        let entries = vec![
            (
                "MLC/LaunchFolderSelect".to_string(),
                "/home/u/mc/".to_string(),
            ),
            ("MLC/LoginType".to_string(), "0".to_string()),
            ("MLC/Instances/dir1".to_string(), "我的整合包".to_string()),
            ("MLC/Profile/uuid-1/Name".to_string(), "玩家".to_string()),
            ("MLC/Authlib/AccessToken".to_string(), "abc+/=".to_string()),
        ];
        let expected = "[MLC]\n\
            LaunchFolderSelect=/home/u/mc/\n\
            LoginType=0\n\
            Instances\\dir1=我的整合包\n\
            Profile\\uuid-1\\Name=玩家\n\
            Authlib\\AccessToken=\"abc+/=\"\n";
        assert_eq!(serialize_with_eol(&entries, "\n"), expected);
    }

    #[test]
    fn 解析钉死样例与往返() {
        // %U 转义出现在键位（QSettings 对键转义），值为原样 UTF-8
        let text = "\u{FEFF}[MLC]\r\n\
            LaunchFolderSelect=/home/u/mc/\r\n\
            ; 注释行\r\n\
            Instances\\dir1=我的整合包\r\n\
            Profile\\%U73A9%U5BB6\\Name=%E7%8E%A9%E5%AE%B6\r\n\
            Authlib\\AccessToken=\"abc+/=\"\r\n";
        let (entries, err) = parse(text);
        assert!(!err);
        assert_eq!(entries.len(), 4);
        assert_eq!(
            entries[0],
            ("MLC/LaunchFolderSelect".into(), Some("/home/u/mc/".into()))
        );
        assert_eq!(
            entries[1],
            ("MLC/Instances/dir1".into(), Some("我的整合包".into()))
        );
        // 键里的 %U 解码；值里的 %XX 不做特殊处理（Qt 读方向值解析无 %XX 语法）
        assert_eq!(
            entries[2],
            (
                "MLC/Profile/玩家/Name".into(),
                Some("%E7%8E%A9%E5%AE%B6".into())
            )
        );
        assert_eq!(
            entries[3],
            ("MLC/Authlib/AccessToken".into(), Some("abc+/=".into()))
        );

        // 往返：serialize → parse 原样恢复
        let original = vec![
            (
                "MLC/LaunchFolderSelect".to_string(),
                "/home/u/mc/".to_string(),
            ),
            ("MLC/Instances/dir1".to_string(), "我的整合包".to_string()),
            ("MLC/Profile/uuid-1/Name".to_string(), "玩家".to_string()),
        ];
        let (parsed, err) = parse(&serialize_with_eol(&original, "\n"));
        assert!(!err);
        assert_eq!(parsed.len(), 3);
        for ((k, v), (k2, v2)) in original.iter().zip(parsed.iter()) {
            assert_eq!(k, k2);
            assert_eq!(Some(v), v2.as_ref());
        }
    }

    #[test]
    fn 解析边缘行为对齐qt() {
        // 无 = 且非注释 → 格式错误但仍解析其余
        let (entries, err) = parse("[MLC]\nbroken line\nKey=1\n");
        assert!(err);
        assert_eq!(entries.len(), 1);
        // @Invalid() → None
        let (entries, err) = parse("[MLC]\nK=@Invalid()\n");
        assert!(!err);
        assert_eq!(entries, vec![("MLC/K".to_string(), None)]);
        // 转义值里的 = 不算分隔符；值内 = 保留
        let (entries, _) = parse("[MLC]\nK=\"a=b\"\n");
        assert_eq!(entries[0].1.as_deref(), Some("a=b"));
        // 空值
        let (entries, _) = parse("[MLC]\nK=\n");
        assert_eq!(entries[0].1.as_deref(), Some(""));
        // 值截尾空白；引号保空白
        let (entries, _) = parse("[MLC]\nA=v   \nB=\" v \"\n");
        assert_eq!(entries[0].1.as_deref(), Some("v"));
        assert_eq!(entries[1].1.as_deref(), Some(" v "));
        // [General] → 根节（无前缀）
        let (entries, _) = parse("[General]\nRoot=1\n");
        assert_eq!(entries[0].0, "Root");
    }
}
