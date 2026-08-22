#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"

namespace ps {
namespace ops {

/**
 * @brief Source-private Region-aware monolithic core callback.
 *
 * @param node Borrowed node snapshot for one invocation.
 * @param inputs Borrowed destination-indexed input outputs.
 * @param region Exact normalized logical work selection.
 * @return Independently owned complete output.
 * @throws Any validation, allocation, or compute exception from the core
 *         operation.
 * @note This callback type is not installed, serialized, or added to the
 *       operation-plugin ABI and carries no resource/device metadata.
 */
using CoreRegionMonolithicOpFunc = std::function<NodeOutput(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const RegionSet& region)>;

/**
 * @brief Finds a source-private Region-aware core implementation.
 *
 * @param type Already-selected operation type.
 * @param subtype Already-selected operation subtype.
 * @param selected_operation Exact monolithic callback selected by the frozen
 *        operation snapshot.
 * @return Owned callback only when the selected callback is the matching core
 *         implementation, otherwise nullopt.
 * @throws std::bad_alloc when std::function target storage cannot allocate.
 * @note The lookup is deliberately narrow and does not implement metadata
 *       routing, device selection, or provider ABI v3. Comparing the selected
 *       callback identity prevents a plugin override from being replaced by
 *       the source-private core implementation.
 */
std::optional<CoreRegionMonolithicOpFunc> find_core_region_monolithic_operation(
    const std::string& type, const std::string& subtype,
    const MonolithicOpFunc& selected_operation);

/**
 * @brief Computes the bounded input halo used by built-in neighborhood ops.
 *
 * @param type Operation type.
 * @param subtype Operation subtype.
 * @param parameters Exact effective parameter snapshot for this request.
 * @return Non-negative HP-space halo; zero for non-neighborhood operations.
 * @throws Nothing; unsupported or non-numeric parameter alternatives fall
 *         back to the operation's execution default.
 * @note Dirty planning and built-in ROI callbacks share this function so they
 *       cannot derive different halo geometry from the same snapshot.
 */
int builtin_input_halo_radius(const std::string& type,
                              const std::string& subtype,
                              const plugin::ParameterMap& parameters) noexcept;

/**
 * @brief Publishes dependency-neutral built-in operations.
 *
 * The core registration set contains named-data analyzers, scalar math
 * callbacks, and the dependency-neutral Value-backed dense image inversion,
 * plus their propagation contracts. Optional image algorithm providers are
 * composed separately by the process plugin owner.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry key or callback storage allocation fails.
 * @note This function has no OpenCV or provider dependency. Repeated calls
 *       replace the same core slots and are used by deterministic restoration
 *       tests; normal process startup calls it once through the configured
 *       provider composition boundary.
 */
void register_core_operations();

}  // namespace ops
}  // namespace ps
