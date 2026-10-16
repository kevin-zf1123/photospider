#pragma once
#if defined(_WIN32) && !defined(PHOTOSPIDER_BENCHMARK_STATIC)
#if defined(PHOTOSPIDER_BENCHMARK_BUILD)
#define PHOTOSPIDER_BENCHMARK_API __declspec(dllexport)
#else
#define PHOTOSPIDER_BENCHMARK_API __declspec(dllimport)
#endif
#elif defined(__clang__)
#define PHOTOSPIDER_BENCHMARK_API __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_BENCHMARK_API
#endif
