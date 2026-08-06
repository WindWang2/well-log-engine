#pragma once

// DLL visibility for the CGM export library (B1.CGM.1 / ADR 0054).

#if defined(_WIN32) && defined(WELLLOG_EXPORT_CGM_SHARED)
#if defined(WELLLOG_EXPORT_CGM_BUILD)
#define WELLLOG_EXPORT_CGM_API __declspec(dllexport)
#else
#define WELLLOG_EXPORT_CGM_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_EXPORT_CGM_SHARED)
#define WELLLOG_EXPORT_CGM_API __attribute__((visibility("default")))
#else
#define WELLLOG_EXPORT_CGM_API
#endif
