/**
 * @file kernel_execution_facade.cpp
 * @brief Implements resource-neutral Graph execution-route replacement and
 * copied inspection.
 */
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "compute/execution_service.hpp"
#include "runtime/kernel.hpp"

namespace ps {

/** @copydoc Kernel::replace_execution */
bool Kernel::replace_execution(const std::string& name, ComputeIntent intent,
                               const std::string& type) {
  auto runtime_it = graphs_.find(name);
  if (runtime_it == graphs_.end() ||
      !compute::ExecutionService::is_execution_type(type)) {
    return false;
  }
  GraphRuntime& runtime = *runtime_it->second;
  try {
    runtime
        .submit_compute_request([&runtime, intent, type]() {
          runtime.replace_execution_route(intent, type);
        })
        .get();
    return true;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return false;
  }
}

/** @copydoc Kernel::get_execution_info */
std::optional<std::pair<std::string, std::string>> Kernel::get_execution_info(
    const std::string& name, ComputeIntent intent) {
  auto runtime_it = graphs_.find(name);
  if (runtime_it == graphs_.end()) {
    return std::nullopt;
  }
  GraphRuntime& runtime = *runtime_it->second;
  try {
    return runtime
        .submit_compute_request([this, &runtime, intent]() {
          const GraphRuntime::ExecutionRouteBinding route =
              runtime.execution_route(intent);
          return std::make_pair(route.execution_type,
                                execution_service_->get_stats());
        })
        .get();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace ps
