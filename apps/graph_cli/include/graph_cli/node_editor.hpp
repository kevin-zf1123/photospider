#pragma once

#include <optional>
#include <string>

namespace ps {
class Host;
}

/**
 * @brief Runs the interactive Host-backed YAML node editor.
 *
 * The editor loads copied node ids and YAML through Host, presents node,
 * editor, dependency-tree, and containing-tree panes, and processes edits on
 * the FTXUI event thread. Applying YAML checks traversal availability;
 * traversal failure triggers a best-effort revert, while successful edits are
 * followed by a best-effort graph save. The external-editor action writes and
 * rereads a session-local YAML file and waits for the configured editor.
 *
 * @param svc Borrowed public Host used throughout the interactive loop.
 * @param graph_name Opaque graph session name edited and used for the local
 *        session path.
 * @param initial_id Optional node id selected initially when present in the
 *        copied node inventory; otherwise the first node is selected.
 * @return Nothing after the UI exits, or immediately when node listing fails
 *         or returns no nodes.
 * @throws std::bad_alloc if Host result, UI, path, stream, or text allocation
 *         exhausts memory.
 * @note `svc` and `graph_name` must outlive the fullscreen event loop. The
 *       function is single-thread-affine and supplies no Host synchronization.
 *       Recoverable Host mutations, graph save failures, ordinary file-stream
 *       failures, and the external editor exit status remain best-effort under
 *       the existing UI behavior.
 */
void run_node_editor_decoupled(ps::Host& svc, const std::string& graph_name,
                               std::optional<int> initial_id);
