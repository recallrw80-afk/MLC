//! 整合包管线，对应 C++ `sdk/src/modpack/`：detect → common → installers → pipeline。
//! Forge/NeoForge/Fabric 安装器 processor 流程边缘 case 最多，放最后移植；
//! .incomplete 标记 + 任一步失败整体回滚的语义原样保留。
