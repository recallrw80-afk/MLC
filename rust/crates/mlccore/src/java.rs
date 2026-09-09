//! Java 管理，对应 C++ `javamanager.cpp`。无现成 crate（全是领域逻辑）：
//! 候选路径枚举（JAVA_HOME/PATH/注册表//usr/lib/jvm/java_home）→ `java -version` 解析
//! → MC 版本兼容矩阵 → Adoptium API 下载。基于 std::process / std::fs。
