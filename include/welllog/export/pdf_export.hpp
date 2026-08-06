#pragma once

// DLL visibility macros for the PDF export library (mirrors export.hpp, but
// driven by the pdf library's own BUILD/SHARED defines so the pdf shared
// library's symbols are namespaced independently of the vector library).

#if defined(_WIN32) && defined(WELLLOG_EXPORT_PDF_SHARED)
#if defined(WELLLOG_EXPORT_PDF_BUILD)
#define WELLLOG_EXPORT_PDF_API __declspec(dllexport)
#else
#define WELLLOG_EXPORT_PDF_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_EXPORT_PDF_SHARED)
#define WELLLOG_EXPORT_PDF_API __attribute__((visibility("default")))
#else
#define WELLLOG_EXPORT_PDF_API
#endif
