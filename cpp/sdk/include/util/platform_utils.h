#include "core/mlccore_export.h"
#ifndef MLC_PLATFORM_UTILS_H
#define MLC_PLATFORM_UTILS_H

#include <QString>
#include <QSysInfo>

enum class Platform {
    Windows,
    Linux,
    MacOS,
    Unknown
};

inline Platform currentPlatform() {
#if defined(Q_OS_WIN)
    return Platform::Windows;
#elif defined(Q_OS_LINUX)
    return Platform::Linux;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#else
    return Platform::Unknown;
#endif
}

inline bool is64BitSystem() {
    return QSysInfo::currentCpuArchitecture().contains("64");
}

inline QString platformName() {
    switch (currentPlatform()) {
    case Platform::Windows: return "windows";
    case Platform::Linux:   return "linux";
    case Platform::MacOS:   return "osx";
    default:                return "unknown";
    }
}

#endif // MLC_PLATFORM_UTILS_H
