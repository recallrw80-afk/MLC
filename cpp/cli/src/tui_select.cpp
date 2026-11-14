// 终端上下键选择器：termios 原始模式 + ANSI 转义序列，无外部依赖
#include "tui_select.h"

#include <cstdio>
#include <cstring>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>

namespace {

struct TermiosGuard {
    termios saved{};
    bool active = false;
    ~TermiosGuard() { restore(); }
    void restore() {
        if (active) { tcsetattr(STDIN_FILENO, TCSANOW, &saved); active = false; }
    }
};

TermiosGuard g_guard;
void onSignal(int) {
    g_guard.restore();
    _exit(130);
}

// 写一行到终端（换行用 \r\n，原始模式下 \n 不会自动回车）
void tuiWrite(const QByteArray &s) {
    (void)::write(STDOUT_FILENO, s.constData(), s.size());
}

// 可视窗口行数：跟随终端高度（留 4 行给标题/提示/边界），收在 [5, 12]
int visibleRows(int itemCount) {
    int rows = 10;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        rows = qBound(5, int(ws.ws_row) - 4, 12);
    return qMin(rows, itemCount);
}

// 终端列宽（读不到按 80）
int termCols() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

// 单码点列宽：CJK 全角/宽字符占 2 列，控制字符 0，其余 1（不依赖 locale）
int charCols(uint ucs4) {
    if (ucs4 < 0x20 || (ucs4 >= 0x7F && ucs4 < 0xA0)) return 0;
    if ((ucs4 >= 0x1100 && ucs4 <= 0x115F) || (ucs4 >= 0x2E80 && ucs4 <= 0xA4CF) ||
        (ucs4 >= 0xAC00 && ucs4 <= 0xD7A3) || (ucs4 >= 0xF900 && ucs4 <= 0xFAFF) ||
        (ucs4 >= 0xFE30 && ucs4 <= 0xFE6F) || (ucs4 >= 0xFF00 && ucs4 <= 0xFF60) ||
        (ucs4 >= 0xFFE0 && ucs4 <= 0xFFE6) || (ucs4 >= 0x20000 && ucs4 <= 0x3FFFD))
        return 2;
    return 1;
}

// 去掉控制字符（C0/C1，含 ESC）：名称可能来自玩家输入或整合包清单，
// 原样写进终端会注入转义序列（改光标/改颜色/伪造输出）
QString stripControl(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u < 0x20 || (u >= 0x7F && u <= 0x9F)) continue;
        out.append(ch);
    }
    return out;
}

// 截断到 maxCols 列：超长补 …，保证返回值渲染后不换行（换行会打乱菜单的行数数学）
QString fitCols(const QString &textIn, int maxCols) {
    const QString text = stripControl(textIn);
    const auto cps = text.toUcs4();
    int cols = 0;
    int kept = 0;
    for (uint cp : cps) {
        int w = charCols(cp);
        if (cols + w > maxCols) break;
        cols += w;
        kept++;
    }
    if (kept >= cps.size()) return text;
    // 超长了：留 1 列给省略号
    QString out;
    cols = 0;
    for (int i = 0; i < kept; ++i) {
        int w = charCols(cps[i]);
        if (cols + w > maxCols - 1) break;
        cols += w;
        out.append(QString::fromUcs4(reinterpret_cast<const char32_t *>(&cps[i]), 1));
    }
    return out + QChar(0x2026);  // …
}

// 只渲染窗口内 visible 行（重绘/清理的行数恒定，长列表不再刷屏错位）。
// 窗口外还有内容时，在首/末行的边栏画 ↑/↓ 提示。
void drawMenu(const QString &title, const QStringList &items, int current,
              int top, int visible, bool first) {
    if (!first) {
        // 光标移回菜单顶部（标题行 + visible 条目行）
        tuiWrite("\x1b[" + QByteArray::number(visible + 1) + "A");
    }
    const int cols = termCols();
    // 标题行
    tuiWrite("\x1b[2K\x1b[1m" + fitCols(title, cols).toUtf8() + "\x1b[0m\r\n");
    for (int row = 0; row < visible; ++row) {
        int i = top + row;
        tuiWrite("\x1b[2K");
        if (i == current)
            tuiWrite("\x1b[7m> " + fitCols(items[i], cols - 2).toUtf8() + "\x1b[0m\r\n");
        else {
            QByteArray gutter = "  ";
            if (row == 0 && top > 0) gutter = "\x1b[90m↑ \x1b[0m";
            else if (row == visible - 1 && top + visible < items.size()) gutter = "\x1b[90m↓ \x1b[0m";
            tuiWrite(gutter + fitCols(items[i], cols - 2).toUtf8() + "\r\n");
        }
    }
    bool paged = items.size() > visible;
    QString hint = paged ? QStringLiteral("(↑/↓ 选择, PgUp/PgDn 翻页, Enter 确认, q/ESC 取消)")
                         : QStringLiteral("(↑/↓ 选择, Enter 确认, q/ESC 取消)");
    tuiWrite("\x1b[2K\x1b[90m" + fitCols(hint, cols).toUtf8() + "\x1b[0m");
    fflush(stdout);
}

// 读一个键：返回值 0=上 1=下 2=Home 3=End 4=确认 5=PgUp 6=PgDn -1=取消 -2=其他
int readKey() {
    char c;
    if (::read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == 'q' || c == 'Q' || c == 3) return -1;   // q 或 Ctrl+C
    if (c == '\r' || c == '\n') return 4;
    if (c == 27) {  // ESC：等 40ms 看是不是转义序列；单独 ESC 直接取消
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 40) <= 0) return -1;
        char seq[2] = {0, 0};
        if (::read(STDIN_FILENO, &seq[0], 1) != 1) return -1;
        if (seq[0] == 'O') {
            // SS3 序列（如 ESC O P）：吞掉最终字节，不算取消
            (void)::read(STDIN_FILENO, &seq[1], 1);
            return -2;
        }
        if (seq[0] != '[') return -1;
        // CSI 序列：读参数（数字/;）直到最终字节 0x40-0x7E，上限防异常序列死循环
        int param = 0;
        char fin = 0;
        for (int i = 0; i < 16; ++i) {
            if (::read(STDIN_FILENO, &seq[1], 1) != 1) return -1;
            if (seq[1] >= 0x40 && seq[1] <= 0x7E) { fin = seq[1]; break; }
            if (seq[1] >= '0' && seq[1] <= '9') param = param * 10 + (seq[1] - '0');
        }
        switch (fin) {
        case 'A': return 0;  // ↑
        case 'B': return 1;  // ↓
        case 'H': return 2;  // Home
        case 'F': return 3;  // End
        case '~':
            if (param == 5) return 5;  // PgUp
            if (param == 6) return 6;  // PgDn
            return -2;                 // Delete(3) 等忽略
        default: return -2;
        }
    }
    return -2;
}

} // namespace

int tuiSelect(const QString &title, const QStringList &items, int initial) {
    if (items.isEmpty()) return -1;

    // 切原始模式（非 TTY 直接失败——调用方应已用 isatty 判断）
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_guard.saved) != 0) return -1;
    termios raw = g_guard.saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    g_guard.active = true;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    const int visible = visibleRows(items.size());
    int current = (initial >= 0 && initial < items.size()) ? initial : 0;
    int top = 0;
    int result = -1;
    bool first = true;

    // 让 current 落在窗口内：current < top 上卷，current >= top+visible 下卷
    auto adjustWindow = [&]() {
        if (current < top) top = current;
        if (current >= top + visible) top = current - visible + 1;
    };
    adjustWindow();
    drawMenu(title, items, current, top, visible, first);
    first = false;

    for (;;) {
        int key = readKey();
        if (key == -1) { result = -1; break; }
        if (key == 4) { result = current; break; }
        if (key == 0 && current > 0) current--;
        else if (key == 1 && current < items.size() - 1) current++;
        else if (key == 2) current = 0;
        else if (key == 3) current = items.size() - 1;
        else if (key == 5) current = qMax(0, current - visible);                      // PgUp
        else if (key == 6) current = qMin(items.size() - 1, current + visible);       // PgDn
        else continue;
        adjustWindow();
        drawMenu(title, items, current, top, visible, first);
    }

    // 结束后：清理菜单区，留一行干净的输出位置。
    // 此时光标在尾行（提示行）行尾；菜单共 标题+visible条目+尾行 的 visible+2 行。
    tuiWrite("\r\x1b[2K");  // 先清掉光标所在的尾行
    tuiWrite("\x1b[" + QByteArray::number(visible + 1) + "A");  // 回到标题行
    for (int i = 0; i <= visible; ++i)
        tuiWrite("\x1b[2K" + QByteArray(i < visible ? "\x1b[1B" : ""));
    // 光标在最后一个条目行（已清），换行离开菜单区，后续输出从干净行开始
    tuiWrite("\r\n");

    g_guard.restore();
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    return result;
}
