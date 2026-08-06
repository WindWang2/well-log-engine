#pragma once

#if defined(_WIN32) && defined(WELLLOG_SESSION_SHARED)
#if defined(WELLLOG_SESSION_BUILD)
#define WELLLOG_SESSION_API __declspec(dllexport)
#else
#define WELLLOG_SESSION_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_SESSION_SHARED)
#define WELLLOG_SESSION_API __attribute__((visibility("default")))
#else
#define WELLLOG_SESSION_API
#endif
