#pragma once

#if defined(_WIN32) && defined(WELLLOG_SCENE_SHARED)
#if defined(WELLLOG_SCENE_BUILD)
#define WELLLOG_SCENE_API __declspec(dllexport)
#else
#define WELLLOG_SCENE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_SCENE_SHARED)
#define WELLLOG_SCENE_API __attribute__((visibility("default")))
#else
#define WELLLOG_SCENE_API
#endif
