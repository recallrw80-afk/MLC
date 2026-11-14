// 交互式问答提示实现：clack/create-vite 风格的单行渲染
#include "tui_prompt.h"

#include <cstdio>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

namespace {

struct TermGuard {
    termios saved{};
    bool active = false;
    ~TermGuard() { restore(); }
    void restore() {
        if (active) { tcsetattr(STDIN_FILENO, TCSANOW, &saved); active = false; }
    }
};

TermGuard g_guard;
void onSignal(int) {
    g_guard.restore();
    _exit(130);
}

void wr(const QByteArray &s) { (void)::write(STDOUT_FILENO, s.constData(), s.size()); }

bool enterRaw() {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &g_guard.saved) != 0) return false;
    termios raw = g_guard.saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    g_guard.active = true;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    return true;
}

void leaveRaw() {
    g_guard.restore();
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

// 读一个字节；单独 ESC 返回 27（调用方视为取消），转义序列（方向键等）吞掉返回 0
int readByte() {
    char c;
    if (::read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == 3) return 27;          // Ctrl+C 视为取消
    if (c != 27) return (unsigned char)c;
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 40) <= 0) return 27;   // 单独 ESC
    char c1;
    if (::read(STDIN_FILENO, &c1, 1) != 1) return 27;
    if (c1 == 'O') {
        // SS3 序列（如 ESC O P）：吞掉最终字节
        (void)::read(STDIN_FILENO, &c1, 1);
        return 0;
    }
    if (c1 == '[') {
        // CSI 序列：持续吞到最终字节 0x40-0x7E（如 ESC [ 3 ~ 是 Delete），上限防异常序列死循环
        for (int i = 0; i < 16; ++i) {
            if (::read(STDIN_FILENO, &c1, 1) != 1) break;
            if (c1 >= 0x40 && c1 <= 0x7E) break;
        }
    }
    return 0;
}

void drawInput(const QString &label, const QByteArray &text, const QString &placeholder) {
    wr("\r\x1b[2K\x1b[36m◇\x1b[0m " + label.toUtf8() + ": ");
    if (text.isEmpty() && !placeholder.isEmpty())
        wr("\x1b[90m" + placeholder.toUtf8() + "\x1b[0m");
    else
        wr(text);
}

} // namespace

std::optional<QString> tuiInput(const QString &label, const QString &def, const QString &placeholder) {
    if (!enterRaw()) return std::nullopt;

    QByteArray text = def.toUtf8();
    std::optional<QString> result;
    drawInput(label, text, placeholder);

    for (;;) {
        int b = readByte();
        if (b < 0 || b == 27) { result = std::nullopt; break; }   // 取消
        if (b == '\r' || b == '\n') {                             // 确认
            result = QString::fromUtf8(text).trimmed();
            break;
        }
        if (b == 127 || b == 8) {                                 // Backspace：删一个 UTF-8 码点
            while (!text.isEmpty() && (text.back() & 0xC0) == 0x80) text.chop(1);
            if (!text.isEmpty()) text.chop(1);
        } else if (b >= 0x20) {                                   // 可打印字节（含 UTF-8 前导/延续）
            text.append((char)b);
        } else {
            continue;                                             // 其他控制键忽略，不重绘
        }
        drawInput(label, text, placeholder);
    }

    // 定稿行：保留一问一答的记录（clack 风格），取消则换行离开
    wr("\r\x1b[2K");
    leaveRaw();
    if (result) {
        QString v = *result;
        wr("\x1b[32m◇\x1b[0m " + label.toUtf8() + ": " +
           (v.isEmpty() ? "\x1b[90m" + placeholder.toUtf8() + "\x1b[0m" : v.toUtf8()) + "\n");
    } else {
        wr("\n");
    }
    return result;
}

std::optional<QString> tuiPassword(const QString &label) {
    if (!enterRaw()) return std::nullopt;

    QByteArray text;
    std::optional<QString> result;
    auto draw = [&] {
        wr("\r\x1b[2K\x1b[36m◇\x1b[0m " + label.toUtf8() + ": " + QByteArray(text.size(), '*'));
    };
    draw();

    for (;;) {
        int b = readByte();
        if (b < 0 || b == 27) { result = std::nullopt; break; }
        if (b == '\r' || b == '\n') {
            result = QString::fromUtf8(text);
            break;
        }
        if (b == 127 || b == 8) {
            while (!text.isEmpty() && (text.back() & 0xC0) == 0x80) text.chop(1);
            if (!text.isEmpty()) text.chop(1);
        } else if (b >= 0x20) {
            text.append((char)b);
        } else {
            continue;
        }
        draw();
    }

    // 定稿行只留星号，密码本身不上屏
    wr("\r\x1b[2K");
    leaveRaw();
    if (result)
        wr("\x1b[32m◇\x1b[0m " + label.toUtf8() + ": " + QByteArray(text.size(), '*') + "\n");
    else
        wr("\n");
    return result;
}

std::optional<bool> tuiConfirm(const QString &label, bool def) {
    if (!enterRaw()) return std::nullopt;

    QByteArray suffix = def ? " (Y/n): " : " (y/N): ";
    std::optional<bool> result;
    wr("\x1b[36m◇\x1b[0m " + label.toUtf8() + suffix);

    for (;;) {
        int b = readByte();
        if (b < 0 || b == 27) { result = std::nullopt; break; }
        if (b == '\r' || b == '\n') { result = def; break; }
        if (b == 'y' || b == 'Y') { result = true; break; }
        if (b == 'n' || b == 'N') { result = false; break; }
    }

    wr("\r\x1b[2K");
    leaveRaw();
    if (result)
        wr("\x1b[32m◇\x1b[0m " + label.toUtf8() + ": " + (*result ? "y" : "n") + "\n");
    else
        wr("\n");
    return result;
}
