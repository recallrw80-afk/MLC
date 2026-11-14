#ifndef MLCCORE_EXPORT_H
#define MLCCORE_EXPORT_H

#include <QtCore/QtGlobal>

#ifdef MLCCORE_LIBRARY
#  define MLCCORE_EXPORT Q_DECL_EXPORT
#else
#  define MLCCORE_EXPORT Q_DECL_IMPORT
#endif

#endif // MLCCORE_EXPORT_H
