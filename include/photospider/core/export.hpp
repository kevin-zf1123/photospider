#pragma once

/**
 * @file export.hpp
 * @brief Defines public Photospider symbol visibility annotations.
 *
 * The macro in this file is shared by stable public value contracts and legacy
 * internal include paths. It centralizes platform-specific import/export
 * spelling without introducing runtime state, allocation, or ownership.
 *
 * @note The macro only affects declarations that explicitly opt in. Header-only
 *       value types that do not cross a dynamic-library ABI do not need it.
 */

#if defined(PHOTOSPIDER_PLUGIN_BUILD)
/**
 * @brief Leaves Photospider declarations unannotated inside operation plugins.
 *
 * Operation plugin targets are loaded through a registrar ABI and may link only
 * the narrow operation-plugin shim rather than the backend library. Keeping
 * `PHOTOSPIDER_API` empty in those plugin builds prevents Windows
 * `dllimport` annotations from requiring backend DLL symbols for public value
 * contracts such as `ps::GraphError`.
 *
 * @note Plugin entry points must use `PLUGIN_API`; this macro does not export
 *       plugin registrar symbols or change callback ownership/lifetime rules.
 */
#define PHOTOSPIDER_API
#elif defined(_WIN32)
#if defined(PHOTOSPIDER_LIB_BUILD)
/**
 * @brief Marks Photospider symbols exported by the backend library.
 *
 * @note On Windows the macro expands to `__declspec(dllexport)` while building
 *       the backend and `__declspec(dllimport)` for consumers. On other
 *       platforms it expands to default visibility for backend builds and to an
 *       empty annotation for consumers.
 */
#define PHOTOSPIDER_API __declspec(dllexport)
#else
/**
 * @brief Marks Photospider symbols imported by external consumers.
 *
 * @note On Windows the macro expands to `__declspec(dllimport)` for consumers.
 *       On non-Windows platforms the consumer-side expansion is empty.
 */
#define PHOTOSPIDER_API __declspec(dllimport)
#endif
#else
#if defined(PHOTOSPIDER_LIB_BUILD)
/**
 * @brief Marks Photospider backend symbols with default visibility.
 *
 * @note This keeps dynamic-library exports explicit on compilers that support
 *       ELF/Mach-O visibility attributes. Static-link consumers can include the
 *       same headers without special ownership or lifecycle constraints.
 */
#define PHOTOSPIDER_API __attribute__((visibility("default")))
#else
/**
 * @brief Leaves Photospider consumer declarations unannotated.
 *
 * @note Public value types remain ordinary C++ declarations for static-link and
 *       header-only consumers when the backend build macro is absent.
 */
#define PHOTOSPIDER_API
#endif
#endif
