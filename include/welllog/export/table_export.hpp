#pragma once

// DLL visibility macros for the table-export library (CSV / versioned XML /
// XLSX writers over a TableProjection, #155). Mirrors pdf_export.hpp, driven by
// the table-export library's own BUILD/SHARED defines so its symbols are
// namespaced independently of the vector/pdf libraries.

#if defined(_WIN32) && defined(WELLLOG_EXPORT_TABLE_SHARED)
#if defined(WELLLOG_EXPORT_TABLE_BUILD)
#define WELLLOG_EXPORT_TABLE_API __declspec(dllexport)
#else
#define WELLLOG_EXPORT_TABLE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_EXPORT_TABLE_SHARED)
#define WELLLOG_EXPORT_TABLE_API __attribute__((visibility("default")))
#else
#define WELLLOG_EXPORT_TABLE_API
#endif
