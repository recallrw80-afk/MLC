#ifndef MLC_CLI_TUI_SELECT_H
#define MLC_CLI_TUI_SELECT_H

#include <QString>
#include <QStringList>

/// 终端交互式上下键选择器（仅在 stdin 是 TTY 时由调用方使用）。
/// ↑/↓ 移动、Home/End 跳首尾、Enter 确认、q/ESC 取消。
/// 返回选中项索引（0 起始）；取消返回 -1。
/// 非 TTY 环境（管道/重定向）调用方必须退回输序号模式。
int tuiSelect(const QString &title, const QStringList &items, int initial = 0);

#endif // MLC_CLI_TUI_SELECT_H
