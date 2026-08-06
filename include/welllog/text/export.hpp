#pragma once

#if defined(_WIN32) && defined(WELLLOG_TEXT_SHARED)
#if defined(WELLLOG_TEXT_BUILD)
#define WELLLOG_TEXT_API __declspec(dllexport)
#else
#define WELLLOG_TEXT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_TEXT_SHARED)
#define WELLLOG_TEXT_API __attribute__((visibility("default")))
#else
#define WELLLOG_TEXT_API
#endif
