#pragma once

#include <string>

#include "photospider/host/host.hpp"

namespace ps::cli {

std::string format_node_metadata(const NodeInspectionView& node);
std::string format_node_inspection(const NodeInspectionView& node);
std::string format_graph_inspection(const GraphInspectionView& graph);
std::string format_dependency_tree(const HostDependencyTreeSnapshot& tree,
                                   bool show_parameters,
                                   bool show_metadata = false);

}  // namespace ps::cli
