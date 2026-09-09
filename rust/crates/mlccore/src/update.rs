//! 自更新与卸载，对应 C++ `update/uninstall`：GitHub→Gitee 双源、tar.xz 解包（xz2+tar）、
//! SemVer 预发布比较（同数字段 正式版>预发布、beta<rc）。
//! 需处理新旧包布局过渡（旧 Qt 捆绑目录 → 新单文件）。
