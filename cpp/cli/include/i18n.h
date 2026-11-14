#ifndef MLC_CLI_I18N_H
#define MLC_CLI_I18N_H

// CLI 中英文切换（main.cpp / test.cpp 共用）

#include <QString>

enum Lang { CN, EN };

// inline 变量（C++17）：两个编译单元共享同一份
inline Lang g_lang = EN;

#define _(cn, en) (g_lang == CN ? cn : en)

inline void setLang(bool en) { g_lang = en ? EN : CN; }
inline QString T(const char *cn, const char *en) { return QString::fromUtf8(g_lang == CN ? cn : en); }

#endif // MLC_CLI_I18N_H
