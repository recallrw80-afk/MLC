// 交互式问答提示（Vite create-vite 风格）：文本输入 + y/N 确认
// 与 tui_select（列表单选）配套，均无外部依赖，termios 原始模式实现
#ifndef MLC_TUI_PROMPT_H
#define MLC_TUI_PROMPT_H

#include <QString>
#include <optional>

/// 文本输入：`◇ label: <输入>`。
/// def 非空时作为初始内容（可编辑，直接回车=保留）；placeholder 为空时的灰色占位提示。
/// 返回 std::nullopt 表示用户取消（ESC/Ctrl+C）。
std::optional<QString> tuiInput(const QString &label,
                                const QString &def = QString(),
                                const QString &placeholder = QString());

/// 密码输入：同 tuiInput，但回显为 *，定稿行不显示内容
std::optional<QString> tuiPassword(const QString &label);

/// 确认开关：`◇ label? (y/N)`，y/n 直接选择，回车取 def，ESC 取消。
std::optional<bool> tuiConfirm(const QString &label, bool def = false);

#endif // MLC_TUI_PROMPT_H
