#pragma once

#include <memory>
#include <string>

#include "photospider/core/export.hpp"
#include "photospider/host/host.hpp"

/**
 * @file host.hpp
 * @brief Installed complete IPC-backed Photospider Host factory.
 *
 * The factory exposes the existing frontend Host contract over the protected
 * local version 2 daemon protocol. Transport, polling workers, artifact files,
 * and JSON/socket implementation details remain private to the client product.
 */

namespace ps::ipc {

/**
 * @brief Creates a complete Host backed by one local daemon socket path.
 *
 * Every current non-destructor `Host` virtual delegates to the corresponding
 * typed version 2 Client primitive. Ordinary calls use independent short-lived
 * connections. Compute calls compose daemon jobs through submit, polling,
 * terminal result, and best-effort release without automatic resubmission.
 * Graph load/reload/save calls resolve nonempty relative Host paths against
 * the IPC client process working directory after connection and before their
 * typed Client call; the wire contract remains absolute-path-only.
 *
 * @param socket_path Absolute Unix-domain socket path of `photospiderd`.
 * @return Unique complete IPC Host implementation.
 * @throws std::bad_alloc if adapter state, path, polling policy, or worker
 *         tracking allocation fails.
 * @throws std::system_error if production polling synchronization primitives
 *         cannot be initialized.
 * @note Construction does not connect, start a daemon, load a graph, or
 *       validate daemon availability. Adapter destruction stops and joins its
 *       polling workers but never closes daemon graph sessions, unloads
 *       plugins, or falls back to an embedded Host.
 */
PHOTOSPIDER_API std::unique_ptr<Host> create_ipc_host(
    const std::string& socket_path);

}  // namespace ps::ipc
