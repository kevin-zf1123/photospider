// Simple node editor using InteractionService (decoupled from NodeGraph)
#pragma once

#include <optional>
#include <string>

namespace ps { class InteractionService; }

// Launch a simple YAML-based node editor for the selected graph.
// If initial_id is not set, opens a picker first.
void run_node_editor_decoupled(ps::InteractionService& svc, const std::string& graph_name, std::optional<int> initial_id);

