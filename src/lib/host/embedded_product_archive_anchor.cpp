/**
 * @file embedded_product_archive_anchor.cpp
 * @brief Provides a generator-visible source for final embedded archives.
 *
 * Photospider assembles its production and internal-test embedded products
 * from role-owned object libraries. Some native generators require a source
 * compiled directly by the final static-library target before they create the
 * archive action. This translation unit supplies that build-graph boundary
 * without participating in runtime behavior.
 *
 * @note The compile-time assertion emits no externally linked symbol, public
 *       API, initialization, mutable state, or ABI surface.
 */

/**
 * @brief Confirms that the embedded archive anchor is compile-only.
 * @note This declaration has no storage and introduces no linked symbol.
 */
static_assert(true, "embedded product archive anchor must compile");
