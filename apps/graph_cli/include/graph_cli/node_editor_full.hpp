#pragma once

#include <optional>
#include <string>

namespace ps {
class Host;
}

/**
 * @brief Runs the full node-editor entry point through the Host-only editor.
 *
 * The current entry point delegates directly to `run_node_editor_decoupled`,
 * preserving its node selection, edit/revert/save, external-editor, and event
 * loop behavior.
 *
 * @param svc Borrowed public Host forwarded to the interactive editor.
 * @param graph_name Opaque graph session name forwarded without copying.
 * @param initial_id Optional initially selected node id.
 * @return Nothing after the delegated editor returns.
 * @throws std::bad_alloc if delegated Host, UI, path, stream, or text
 *         allocation exhausts memory.
 * @note `svc` and `graph_name` must outlive the delegated event loop. The
 *       function adds no ownership, synchronization, or fallback behavior.
 */
void run_node_editor_full(ps::Host& svc, const std::string& graph_name,
                          std::optional<int> initial_id);
