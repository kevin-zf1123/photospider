#pragma once

#if defined(_WIN32) && !defined(PHOTOSPIDER_OPS_STATIC)
#if defined(PHOTOSPIDER_OPS_BUILD)
#define PHOTOSPIDER_OPS_API __declspec(dllexport)
#else
#define PHOTOSPIDER_OPS_API __declspec(dllimport)
#endif
#elif defined(__clang__)
#define PHOTOSPIDER_OPS_API __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_OPS_API
#endif

#if defined(_WIN32) && !defined(PHOTOSPIDER_OPS_SDK_STATIC)
#if defined(PHOTOSPIDER_OPS_SDK_BUILD)
#define PHOTOSPIDER_OPS_SDK_API __declspec(dllexport)
#else
#define PHOTOSPIDER_OPS_SDK_API __declspec(dllimport)
#endif
#elif defined(__clang__)
#define PHOTOSPIDER_OPS_SDK_API __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_OPS_SDK_API
#endif
