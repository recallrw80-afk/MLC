//! 文件工具，对应 C++ `file_utils.cpp`。
//! 要点：zip 解压的 GBK 中文文件名用 encoding_rs 单一代码路径（消灭 iconv 三分支）；
//! 原子写入/重命名服务于 .incomplete 回滚语义。
