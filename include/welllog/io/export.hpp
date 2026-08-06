#pragma once

#if defined(_WIN32) && defined(WELLLOG_IO_SHARED)
#if defined(WELLLOG_IO_BUILD)
#define WELLLOG_IO_API __declspec(dllexport)
#else
#define WELLLOG_IO_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_IO_SHARED)
#define WELLLOG_IO_API __attribute__((visibility("default")))
#else
#define WELLLOG_IO_API
#endif
