#pragma once

#include <string>

#include "kernel/services/graph_inspect_service.hpp"

namespace ps::cli {

std::string format_node_metadata(const NodeMetadataSummary& metadata);
std::string format_node_inspection(const GraphNodeInspectInfo& node);
std::string format_graph_inspection(const GraphInspectionSnapshot& graph);
std::string format_dependency_tree(const DependencyTree& tree,
                                   bool show_parameters,
                                   bool show_metadata = false);

}  // namespace ps::cli
