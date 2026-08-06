#pragma once

#if defined(_WIN32) && defined(WELLLOG_CORE_SHARED)
#if defined(WELLLOG_CORE_BUILD)
#define WELLLOG_CORE_API __declspec(dllexport)
#else
#define WELLLOG_CORE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_CORE_SHARED)
#define WELLLOG_CORE_API __attribute__((visibility("default")))
#else
#define WELLLOG_CORE_API
#endif
