/*
 * miniz_export.h
 *
 * Upstream miniz generates this with CMake's GenerateExportHeader, which is
 * only meaningful for a shared library. This target links miniz statically
 * into the firmware, so every symbol has default visibility and the export
 * macro is empty - which is exactly what upstream's own static-build output
 * contains.
 */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED __attribute__((__deprecated__))

#endif /* MINIZ_EXPORT_H */
