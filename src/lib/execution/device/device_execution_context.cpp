#include "execution/device/device_execution_context.hpp"

#include <stdexcept>

namespace ps::execution {
namespace {

/**
 * @brief Current borrowed Metal invocation context on this thread.
 *
 * @note The shared operation-runtime image owns this one TLS slot for Host and
 * operation DSOs. `ScopedMetalExecutionContext` is its only mutator.
 */
thread_local MetalExecutionContext* tls_metal_execution_context = nullptr;

}  // namespace

/** @copydoc current_metal_execution_context */
MetalExecutionContext* current_metal_execution_context() noexcept {
  return tls_metal_execution_context;
}

/** @copydoc require_current_metal_execution_context */
MetalExecutionContext& require_current_metal_execution_context() {
  MetalExecutionContext* context = current_metal_execution_context();
  if (context == nullptr) {
    throw std::logic_error(
        "Metal operation requires DeviceExecutorRegistry entry.");
  }
  return *context;
}

/** @copydoc ScopedMetalExecutionContext::ScopedMetalExecutionContext */
ScopedMetalExecutionContext::ScopedMetalExecutionContext(
    MetalExecutionContext& context) noexcept
    : previous_(tls_metal_execution_context) {
  tls_metal_execution_context = &context;
}

/** @copydoc ScopedMetalExecutionContext::~ScopedMetalExecutionContext */
ScopedMetalExecutionContext::~ScopedMetalExecutionContext() noexcept {
  tls_metal_execution_context = previous_;
}

}  // namespace ps::execution
