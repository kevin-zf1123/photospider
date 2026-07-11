#pragma once

#include <string>

#include "photospider/host/host.hpp"

namespace ps::cli {

std::string format_node_metadata(const NodeInspectionView& node);
std::string format_node_inspection(const NodeInspectionView& node);
std::string format_graph_inspection(const GraphInspectionView& graph);
/**
 * @brief Formats a Host dirty-region snapshot for CLI inspection output.
 *
 * @param snapshot Public Host dirty-region inspection snapshot.
 * @return Human-readable dirty-region report.
 * @throws std::bad_alloc if string construction allocates and fails.
 * @note The formatter consumes only public Host values and does not depend on
 *       kernel dirty-region or scheduler implementation types.
 */
std::string format_dirty_snapshot(
    const DirtyRegionInspectionSnapshot& snapshot);
std::string format_dependency_tree(const HostDependencyTreeSnapshot& tree,
                                   bool show_parameters,
                                   bool show_metadata = false);

}  // namespace ps::cli
