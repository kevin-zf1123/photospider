#include "execution/physical_execution_routes.hpp"

#include <limits>
#include <string>
#include <vector>

#include "photospider/core/graph_error.hpp"

namespace ps::execution {
namespace {

/** @brief Canonical process CPU route name. */
constexpr std::string_view kCpuRoute = "cpu";

/** @brief Canonical Host GPU-pipeline route name. */
constexpr std::string_view kGpuPipelineRoute = "gpu_pipeline";

/** @brief Canonical deterministic serial-debug route name. */
constexpr std::string_view kSerialDebugRoute = "serial_debug";

/**
 * @brief Reports whether a name belongs to the frozen route vocabulary.
 * @param type_name Exact route name.
 * @return True only for an exact canonical route name.
 * @throws Nothing.
 * @note Implemented as a member-free helper only through public state methods;
 * direct counter selection remains inside those methods below.
 */
bool known_route(std::string_view type_name) noexcept {
  return type_name == kCpuRoute || type_name == kGpuPipelineRoute ||
         type_name == kSerialDebugRoute;
}

}  // namespace

/** @copydoc PhysicalExecutionRoutes::available_types */
std::vector<std::string> PhysicalExecutionRoutes::available_types() {
  return {std::string(kCpuRoute), std::string(kGpuPipelineRoute),
          std::string(kSerialDebugRoute)};
}

/** @copydoc PhysicalExecutionRoutes::description */
std::string PhysicalExecutionRoutes::description(std::string_view type_name) {
  if (type_name == kCpuRoute) {
    return "Process-owned CPU execution route.";
  }
  if (type_name == kGpuPipelineRoute) {
    return "Host-owned GPU-pipeline execution route.";
  }
  if (type_name == kSerialDebugRoute) {
    return "Deterministic single-callback debug execution route.";
  }
  throw GraphError(GraphErrc::NotFound, "Unknown private execution route: " +
                                            std::string(type_name));
}

/** @copydoc PhysicalExecutionRoutes::is_supported */
bool PhysicalExecutionRoutes::is_supported(
    std::string_view type_name) noexcept {
  return known_route(type_name);
}

/** @copydoc PhysicalExecutionRoutes::can_start */
bool PhysicalExecutionRoutes::can_start(
    std::string_view type_name, int worker_id,
    std::uint64_t run_in_flight) const noexcept {
  if (stopping_) {
    return false;
  }
  if (type_name == kSerialDebugRoute) {
    return worker_id == 0 && run_in_flight == 0U &&
           serial_debug_in_flight_ == 0U;
  }
  return type_name == kCpuRoute || type_name == kGpuPipelineRoute;
}

/** @copydoc PhysicalExecutionRoutes::commit_start */
bool PhysicalExecutionRoutes::commit_start(
    std::string_view type_name) noexcept {
  if (stopping_) {
    return false;
  }
  std::uint64_t* counter = nullptr;
  if (type_name == kCpuRoute) {
    counter = &cpu_in_flight_;
  } else if (type_name == kGpuPipelineRoute) {
    counter = &gpu_pipeline_in_flight_;
  } else if (type_name == kSerialDebugRoute && serial_debug_in_flight_ == 0U) {
    counter = &serial_debug_in_flight_;
  }
  if (counter == nullptr ||
      *counter == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  ++*counter;
  return true;
}

/** @copydoc PhysicalExecutionRoutes::finish */
bool PhysicalExecutionRoutes::finish(std::string_view type_name) noexcept {
  std::uint64_t* counter = nullptr;
  if (type_name == kCpuRoute) {
    counter = &cpu_in_flight_;
  } else if (type_name == kGpuPipelineRoute) {
    counter = &gpu_pipeline_in_flight_;
  } else if (type_name == kSerialDebugRoute) {
    counter = &serial_debug_in_flight_;
  }
  if (counter == nullptr || *counter == 0U) {
    return false;
  }
  --*counter;
  return true;
}

/** @copydoc PhysicalExecutionRoutes::drained */
bool PhysicalExecutionRoutes::drained() const noexcept {
  return cpu_in_flight_ == 0U && gpu_pipeline_in_flight_ == 0U &&
         serial_debug_in_flight_ == 0U;
}

/** @copydoc PhysicalExecutionRoutes::in_flight */
std::uint64_t PhysicalExecutionRoutes::in_flight(
    std::string_view type_name) const noexcept {
  if (type_name == kCpuRoute) {
    return cpu_in_flight_;
  }
  if (type_name == kGpuPipelineRoute) {
    return gpu_pipeline_in_flight_;
  }
  if (type_name == kSerialDebugRoute) {
    return serial_debug_in_flight_;
  }
  return 0U;
}

}  // namespace ps::execution
