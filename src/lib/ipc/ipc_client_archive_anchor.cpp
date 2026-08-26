/**
 * @file ipc_client_archive_anchor.cpp
 * @brief Provides a generator-visible source for the installed IPC archive.
 *
 * The IPC client static product aggregates its implementation from a private
 * object library. Some native generators require a source compiled directly
 * by the final static-library target before they create the archive action.
 * This translation unit supplies that build-graph boundary without changing
 * IPC framing, transport, Host adaptation, or client lifecycle behavior.
 *
 * @note The compile-time assertion emits no externally linked symbol, public
 *       API, initialization, mutable state, or ABI surface.
 */

/**
 * @brief Confirms that the IPC client archive anchor is compile-only.
 * @note This declaration has no storage and introduces no linked symbol.
 */
static_assert(true, "IPC client archive anchor must compile");
