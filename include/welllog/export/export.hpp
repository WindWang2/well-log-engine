#pragma once

#if defined(_WIN32) && defined(WELLLOG_EXPORT_VECTOR_SHARED)
#if defined(WELLLOG_EXPORT_VECTOR_BUILD)
#define WELLLOG_EXPORT_VECTOR_API __declspec(dllexport)
#else
#define WELLLOG_EXPORT_VECTOR_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_EXPORT_VECTOR_SHARED)
#define WELLLOG_EXPORT_VECTOR_API __attribute__((visibility("default")))
#else
#define WELLLOG_EXPORT_VECTOR_API
#endif
