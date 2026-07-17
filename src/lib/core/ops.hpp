#pragma once

#include <string>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)

namespace ps {
namespace ops {

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
 * The core registration set currently contains named-data analyzers and scalar
 * math callbacks plus their propagation contracts. Optional image algorithm
 * providers are composed separately by the process plugin owner.
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
