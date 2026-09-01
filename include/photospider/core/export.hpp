#pragma once

#if defined(_WIN32) && !defined(PHOTOSPIDER_STATIC)
#if defined(PHOTOSPIDER_BUILD)
#define PHOTOSPIDER_API __declspec(dllexport)
#else
#define PHOTOSPIDER_API __declspec(dllimport)
#endif
#else
#define PHOTOSPIDER_API
#endif
