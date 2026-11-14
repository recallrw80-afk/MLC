// 帮助渲染：总表（help）与参数详解（<命令> -h）都从 COMMANDS[] 生成
#include "commands.h"
#include "i18n.h"

#include <iostream>

void printHelpTable() {
    std::cout << T("Usage: mlc <command> [args]\n", "Usage: mlc <command> [args]\n").toStdString();
    std::cout << "\n" << T("命令 / Commands:\n", "Commands:\n").toStdString();
    for (int i = 0; i < COMMANDS_COUNT; ++i) {
        const Command &c = COMMANDS[i];
        std::cout << QString("  %1 %2\n")
                         .arg(g_lang == CN ? c.usageCn : c.usageEn, -20)
                         .arg(T(c.descCn, c.descEn)).toStdString();
    }
    std::cout.flush();
}

void printCommandHelp(const QString &cmd) {
    for (int i = 0; i < COMMANDS_COUNT; ++i) {
        if (cmd == QLatin1String(COMMANDS[i].name)) {
            std::cout << (g_lang == CN ? COMMANDS[i].detailCn : COMMANDS[i].detailEn) << std::endl;
            return;
        }
    }
    std::cerr << T("error:  未知命令: %1\n", "error:  unknown command: %1\n").arg(cmd).toStdString();
    printHelpTable();
}
