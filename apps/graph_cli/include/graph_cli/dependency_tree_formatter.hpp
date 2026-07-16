#pragma once

#include <string>

#include "photospider/host/host.hpp"

namespace ps::cli {

/**
 * @brief Formats cached-output metadata from one copied Host node snapshot.
 *
 * @param node Public node inspection value to render.
 * @return Human-readable source, debug, and spatial metadata, or a stable
 *         no-cache message when the required copied fields are absent.
 * @throws std::bad_alloc if stream or result-string construction exhausts
 *         memory.
 * @note The function does not call Host, retain `node`, or inspect backend
 *       cache ownership. It is safe only while the borrowed snapshot exists.
 */
std::string format_node_metadata(const NodeInspectionView& node);

/**
 * @brief Formats one copied Host node snapshot for CLI inspection.
 *
 * @param node Public node inspection value to render.
 * @return Node identity followed by its cached-output metadata report.
 * @throws std::bad_alloc if stream or result-string construction exhausts
 *         memory.
 * @note Formatting is read-only, performs no Host call, and retains no
 *       reference to `node`.
 */
std::string format_node_inspection(const NodeInspectionView& node);

/**
 * @brief Formats every node in a copied Host graph snapshot.
 *
 * @param graph Public graph inspection value to render in snapshot order.
 * @return Human-readable node sections separated by blank lines.
 * @throws std::bad_alloc if stream or result-string construction exhausts
 *         memory.
 * @note Node order and duplicates are preserved. Formatting performs no Host
 *       call and retains no reference to `graph`.
 */
std::string format_graph_inspection(const GraphInspectionView& graph);

/**
 * @brief Formats a Host dirty-region snapshot for CLI inspection output.
 *
 * @param snapshot Public Host dirty-region inspection snapshot.
 * @return Human-readable dirty-region report.
 * @throws std::bad_alloc if string construction allocates and fails.
 * @note The formatter consumes only public Host values and does not depend on
 *       kernel dirty-region or scheduler implementation types. It sorts the
 *       copied per-node ROI keys for stable output and retains no snapshot
 *       storage.
 */
std::string format_dirty_snapshot(
    const DirtyRegionInspectionSnapshot& snapshot);

/**
 * @brief Formats a flattened public Host dependency-tree snapshot.
 *
 * The formatter emits scope-specific headers and empty/not-found diagnostics,
 * then renders incoming edges and depth-indented nodes. Optional metadata and
 * static parameters are taken only from the copied snapshot.
 *
 * @param tree Public dependency-tree snapshot to render.
 * @param show_parameters Whether to include copied static parameters.
 * @param show_metadata Whether to include copied cached-output metadata.
 * @return Human-readable dependency-tree report.
 * @throws std::bad_alloc if stream, temporary-line, or result construction
 *         exhausts memory.
 * @note Entry order, duplicate rows, and cycle markers are preserved. The
 *       function performs no Host call and retains no reference to `tree`.
 */
std::string format_dependency_tree(const HostDependencyTreeSnapshot& tree,
                                   bool show_parameters,
                                   bool show_metadata = false);

}  // namespace ps::cli
