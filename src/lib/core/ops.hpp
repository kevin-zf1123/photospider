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
 * @brief Initializes the OpenCV CPU policy and publishes built-in operations.
 * @return Nothing.
 * @throws std::bad_alloc if registry key or callback storage allocation fails.
 * @throws std::system_error if one-time OpenCV initialization cannot
 *         synchronize.
 * @note The process plugin owner calls this before built-in callbacks become
 *       visible. `cv::setNumThreads(1)` runs exactly once to prevent nested
 *       OpenCV CPU parallelism; repository code never reconfigures it while
 *       callbacks may execute. Registered CPU providers use `cv::Mat`, own no
 *       shared operation mutex, and rely on scheduler worker admission.
 */
void register_builtin();

}  // namespace ops
}  // namespace ps
