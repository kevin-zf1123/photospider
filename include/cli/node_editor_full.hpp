// Full Node Editor (decoupled) using InteractionService
#pragma once

#include <optional>
#include <string>

namespace ps {
class InteractionService;
}

void run_node_editor_full(ps::InteractionService& svc,
                          const std::string& graph_name,
                          std::optional<int> initial_id);
