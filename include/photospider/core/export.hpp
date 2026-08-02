#pragma once

/**
 * @file export.hpp
 * @brief Defines public Photospider symbol visibility annotations.
 *
 * The supported embedded product remains a static archive. The separate
 * `Photospider::operation_runtime` is a shared library whose ordinary external
 * C++ symbols are exported by its CMake target (including a generated Windows
 * export table), while operation plugins export only
 * `register_photospider_ops_v2` through
 * `PHOTOSPIDER_OPERATION_PLUGIN_EXPORT`. `PHOTOSPIDER_API` therefore remains
 * the static embedded-product annotation and does not control operation
 * runtime exports.
 *
 * @note The macro only affects declarations that explicitly opt in. Header-only
 *       value types and operation-runtime declarations continue to use
 *       ordinary external linkage.
 */

#if defined(PHOTOSPIDER_STATIC)
/**
 * @brief Leaves Photospider declarations unannotated for static-link builds.
 *
 * The installable `photospider` target defines this macro for itself and for
 * consumers so public declarations remain ordinary C++ symbols when linked
 * from `libphotospider.a`.
 *
 * @note Static consumers should receive this macro through the exported CMake
 *       target instead of defining backend build/import macros manually.
 */
#define PHOTOSPIDER_API
#elif defined(PHOTOSPIDER_PLUGIN_BUILD)
/**
 * @brief Leaves Photospider declarations unannotated inside operation plugins.
 *
 * Operation plugin targets are loaded through a registrar ABI and may link only
 * the public operation runtime and optional adapter components rather than the
 * private backend library. Keeping `PHOTOSPIDER_API` empty in those plugin
 * builds prevents Windows `dllimport` annotations from requiring the static
 * backend product for public value contracts such as `ps::GraphError`. The
 * separately linked operation-runtime import library remains independent of
 * this macro.
 *
 * @note Plugin entry points must use
 *       `PHOTOSPIDER_OPERATION_PLUGIN_EXPORT`; this macro does not export
 *       plugin registrar symbols or change callback ownership/lifetime rules.
 */
#define PHOTOSPIDER_API
#else
/**
 * @brief Leaves source-tree Photospider declarations unannotated.
 *
 * Internal repository targets and header-only checks may include this public
 * boundary without inheriting the exported target's `PHOTOSPIDER_STATIC`
 * definition. They use ordinary C++ linkage, matching the static product.
 *
 * @note Dynamic-library backend import/export compatibility is intentionally
 *       absent. The narrow shared operation runtime does not make the complete
 *       backend dynamic and does not revive the removed dynamic-backend macro
 *       branch.
 */
#define PHOTOSPIDER_API
#endif
