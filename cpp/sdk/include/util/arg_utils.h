#include "core/mlccore_export.h"
#ifndef MLC_ARG_UTILS_H
#define MLC_ARG_UTILS_H

#include <QStringList>

namespace ArgUtils {

/// Split Java argument string respecting quotes
MLCCORE_EXPORT QStringList splitJavaArgs(const QString &str);

/// Deduplicate Java arguments (last occurrence wins for same prefix;
/// known flag/value pairs such as -cp/--server are kept or dropped together)
MLCCORE_EXPORT QStringList deduplicateArgs(const QStringList &args);

} // namespace ArgUtils

#endif // MLC_ARG_UTILS_H
