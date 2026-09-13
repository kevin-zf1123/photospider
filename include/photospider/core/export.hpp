#pragma once

/**
 * @file export.hpp
 * @brief Defines public shared/static library visibility for the C++ API.
 *
 * @note Windows producers and consumers use explicit export/import markers;
 * static consumers define `PHOTOSPIDER_STATIC`. Clang
 * shared builds publish annotated API symbols despite the target's hidden
 * default visibility.
 */

#if defined(_WIN32)
#if defined(PHOTOSPIDER_STATIC)
#define PHOTOSPIDER_API
#elif defined(PHOTOSPIDER_BUILD)
#define PHOTOSPIDER_API __declspec(dllexport)
#else
#define PHOTOSPIDER_API __declspec(dllimport)
#endif
#elif defined(__clang__)
#define PHOTOSPIDER_API __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_API
#endif
