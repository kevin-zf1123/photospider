// Full Node Editor (decoupled) using Host
#pragma once

#include <optional>
#include <string>

namespace ps {
class Host;
}

void run_node_editor_full(ps::Host& svc, const std::string& graph_name,
                          std::optional<int> initial_id);
