//! 启动管线，对应 C++ `launchbuilder.cpp` + `launcher.cpp`：
//! JVM 参数构建、内存自动 sizing（sysinfo：可用内存 50%、上限 16G）、GC 档位、
//! fcitx/ibus XIM 崩溃规避（GLFW 3.4 替换）、启动日志落盘（mc/logs/mlc-launch-*，留 10 份）。
