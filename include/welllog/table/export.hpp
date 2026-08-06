#pragma once

#if defined(_WIN32) && defined(WELLLOG_TABLE_SHARED)
#if defined(WELLLOG_TABLE_BUILD)
#define WELLLOG_TABLE_API __declspec(dllexport)
#else
#define WELLLOG_TABLE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_TABLE_SHARED)
#define WELLLOG_TABLE_API __attribute__((visibility("default")))
#else
#define WELLLOG_TABLE_API
#endif
