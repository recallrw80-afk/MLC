MLC —— 单文件命令行启动器（Rust 构建）
========================================

安装
----
把 mlc 放到 PATH 中的任意目录即可。Rust 版是**单文件**，不需要 C++ 版那样的
lib/ 与 plugins/ 配套目录。

    chmod +x mlc
    # 方式一：安装到用户目录（无需 sudo）
    mkdir -p ~/.local/bin && mv mlc ~/.local/bin/
    # 方式二：安装到系统目录
    sudo install -m755 mlc /usr/local/bin/mlc

运行时依赖
----------
均为系统自带，一般无需手动安装：

  Linux  : glibc、libgcc_s、liblzma（xz 解包/自更新用）
  macOS  : 系统自带，无额外依赖
  Windows: 系统自带 VC 运行库

快速验证
--------
    mlc version      # 显示版本
    mlc help         # 命令总表
    mlc <命令> -h    # 单条命令的详细参数说明，如 mlc inpack -h

主要命令
--------
    mc-install       下载原版 MC
    inpack           导入整合包（CurseForge/Modrinth/...）
    launch           启动游戏
    list / mods      查看实例与 Mod
    server-install   安装本地服务端
    server-start     启动服务端
