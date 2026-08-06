#pragma once

#if defined(_WIN32) && defined(WELLLOG_RENDER_GL_SHARED)
#if defined(WELLLOG_RENDER_GL_BUILD)
#define WELLLOG_RENDER_GL_API __declspec(dllexport)
#else
#define WELLLOG_RENDER_GL_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(WELLLOG_RENDER_GL_SHARED)
#define WELLLOG_RENDER_GL_API __attribute__((visibility("default")))
#else
#define WELLLOG_RENDER_GL_API
#endif
