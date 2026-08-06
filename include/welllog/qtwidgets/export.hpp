#pragma once

#if defined(_WIN32) && defined(WELLLOG_QTWIDGETS_SHARED)
#if defined(WELLLOG_QTWIDGETS_BUILD)
#define WELLLOG_QTWIDGETS_API __declspec(dllexport)
#else
#define WELLLOG_QTWIDGETS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_QTWIDGETS_SHARED)
#define WELLLOG_QTWIDGETS_API __attribute__((visibility("default")))
#else
#define WELLLOG_QTWIDGETS_API
#endif
