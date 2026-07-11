// Full-featured node editor entry point.
// Currently delegates to the decoupled/simple editor implementation.

#include "graph_cli/node_editor_full.hpp"

#include <optional>
#include <string>

#include "graph_cli/node_editor.hpp"
#include "photospider/host/host.hpp"

void run_node_editor_full(ps::Host& svc, const std::string& graph_name,
                          std::optional<int> initial_id) {
  // Delegate to the minimal decoupled editor for now.
  run_node_editor_decoupled(svc, graph_name, initial_id);
}
