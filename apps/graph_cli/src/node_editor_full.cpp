#include "graph_cli/node_editor_full.hpp"

#include <optional>
#include <string>

#include "graph_cli/node_editor.hpp"
#include "photospider/host/host.hpp"

/** @copydoc run_node_editor_full */
void run_node_editor_full(ps::Host& svc, const std::string& graph_name,
                          std::optional<int> initial_id) {
  run_node_editor_decoupled(svc, graph_name, initial_id);
}
