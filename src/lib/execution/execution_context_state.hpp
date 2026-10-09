#pragma once
#include <cstdint>
#include <memory>

#include "execution/callback_pool.hpp"
#include "execution/shared_results.hpp"
#include "photospider/execution/execution.hpp"
namespace ps::data_internal {
class MemoryBudget;
}
namespace ps::gpu_internal {
class Device;
}
namespace ps::execution_internal {
class ResultCache;
class ResultCheckpoints;
class NativeUploadRegistry;
struct DemandCoordinator;
}  // namespace ps::execution_internal
namespace ps {
// Fixed context owner. Shared/demand coordinators drain before callback pools;
// pools retire before admission, registry, device and budget. Field order is
// the retirement order; methods in other TUs borrow it only under context or
// admitted demand-call lifetime.
struct ExecutionContext::Impl final {
  Impl(std::shared_ptr<OperationRegistry>, ExecutionContextConfig);
  ~Impl();
  /** @brief Fixed resolved CPU worker count. */
  const std::uint32_t cpu_worker_count;
  /** @brief Shared exact modeled-byte capacity, retained beyond the device. */
  std::shared_ptr<data_internal::MemoryBudget> budget;
  std::shared_ptr<gpu_internal::Device> native_device;
  /** @brief Fixed optional GPU-lane availability. */
  const bool gpu_available;
  /** @brief Context-wide maximum callbacks waiting across all lanes. */
  const std::uint32_t maximum_waiting_callbacks;
  const std::uint64_t maximum_live_bytes;
  /** @brief Frozen operation registry retained beyond all callbacks. */
  std::shared_ptr<OperationRegistry> operation_registry;
  /** @brief Shared CPU/GPU waiting-callback admission owner. */
  execution_internal::WaitingAdmission waiting_admission;
  /** @brief Required fixed CPU callback pool. */
  execution_internal::ThreadPool cpu_pool;
  /** @brief Optional single local GPU callback lane. */
  std::unique_ptr<execution_internal::ThreadPool> gpu_pool;
  // Destroy coordinators before callback pools and their allocation budget.
  std::shared_ptr<execution_internal::ResultCache> cache;
  std::shared_ptr<execution_internal::NativeUploadRegistry> native_uploads;
  std::shared_ptr<execution_internal::DemandCoordinator> demands;
  execution_internal::SharedResults shared_results;
  std::unique_ptr<execution_internal::ResultCheckpoints> result_checkpoints;
};
}  // namespace ps
