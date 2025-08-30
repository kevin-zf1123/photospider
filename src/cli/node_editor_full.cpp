// Full-featured node editor entry point.
// Currently delegates to the decoupled/simple editor implementation.

#include "cli/node_editor_full.hpp"
#include "cli/node_editor.hpp"
#include "kernel/interaction.hpp"

#include <optional>
#include <string>

void run_node_editor_full(ps::InteractionService& svc,
                          const std::string& graph_name,
                          std::optional<int> initial_id) {
  // Delegate to the minimal decoupled editor for now.
  run_node_editor_decoupled(svc, graph_name, initial_id);
}
