#pragma once

#if defined(_WIN32) && defined(WELLLOG_ARROW_SHARED)
#if defined(WELLLOG_ARROW_BUILD)
#define WELLLOG_ARROW_API __declspec(dllexport)
#else
#define WELLLOG_ARROW_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_ARROW_SHARED)
#define WELLLOG_ARROW_API __attribute__((visibility("default")))
#else
#define WELLLOG_ARROW_API
#endif
