#include <algorithm>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "execution/demand_context.hpp"
#include "execution/execution_context_state.hpp"
#include "execution/execution_device.hpp"
#include "execution/memory_budget.hpp"
#include "execution/native_gpu.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_checkpoints.hpp"
#include "execution/result_native_upload.hpp"
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif
namespace ps {
using execution_internal::execution_device;
using execution_internal::MemoryBudget;
using execution_internal::ThreadPool;
namespace {
std::uint32_t resolve_cpu_workers(std::uint32_t configured) noexcept {
  if (configured != 0U) {
    return configured;
  }
  const std::uint32_t hardware = std::thread::hardware_concurrency();
  return std::max(1U, std::min(32U, hardware == 0U ? 1U : hardware));
}

}  // namespace
ExecutionContext::Impl::Impl(std::shared_ptr<OperationRegistry> operations,
                             ExecutionContextConfig requested)
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    : cpu_worker_count(resolve_cpu_workers(requested.cpu_workers)),
      // NOLINTNEXTLINE(whitespace/indent_namespace)
      budget(std::make_shared<MemoryBudget>(
          requested.maximum_live_bytes,
          [&] {
            auto limits =
                requested.managed_resources.value_or(ResourceLimits{});
            limits.capacity[ResourceKind::Payload] =
                std::min(limits.capacity[ResourceKind::Payload],
                         requested.maximum_live_bytes);
            return std::make_shared<ResourceBudget>(std::move(limits));
            // NOLINTNEXTLINE(whitespace/indent_namespace)
          }())),
      native_device(
          execution_device(requested.gpu_enabled, budget->resources())),
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      gpu_available(execution_testing::use_native_device()
                        ? native_device && native_device->available()
                        : requested.gpu_enabled),
#else
      gpu_available(native_device && native_device->available()),
#endif
      maximum_waiting_callbacks(requested.maximum_queued_tasks),
      maximum_live_bytes(requested.maximum_live_bytes),
      operation_registry(std::move(operations)),
      waiting_admission(maximum_waiting_callbacks),
      cpu_pool(cpu_worker_count, Backend::Cpu, waiting_admission,
               requested.collect_scheduler_timing) {
  if (!operation_registry || !operation_registry->frozen()) {
    throw std::invalid_argument(
        "ExecutionContext requires a frozen operation registry");
  }
  if (!requested.maximum_demands || requested.maximum_demands > 65536)
    throw std::invalid_argument("invalid demand handle limit");
  demands = execution_internal::make_demand_coordinator(
      requested.maximum_demands,
      static_cast<std::uint64_t>(maximum_waiting_callbacks) + cpu_worker_count +
          1);
  if (!requested.maximum_result_checkpoint_scopes ||
      requested.maximum_result_checkpoint_scopes > 1048576)
    throw std::invalid_argument("invalid Result checkpoint scope limit");
  result_checkpoints = std::make_unique<execution_internal::ResultCheckpoints>(
      requested.maximum_result_checkpoint_scopes);
  if (!requested.maximum_dependency_cache_metadata ||
      requested.maximum_dependency_cache_metadata > 1048576)
    throw std::invalid_argument("invalid dependency cache metadata limit");
  if (requested.result_cache_bytes > requested.maximum_live_bytes)
    throw std::invalid_argument("cache limit exceeds execution budget");
  if (requested.result_cache_bytes != 0)
    cache = std::make_shared<execution_internal::ResultCache>(
        requested.result_cache_bytes, budget,
        requested.maximum_dependency_cache_metadata);
  native_uploads = std::make_shared<execution_internal::NativeUploadRegistry>(
      *budget->resources());
  {
    budget->resources()->set_reclaimer(
        [weak = std::weak_ptr<execution_internal::ResultCache>(cache),
         uploads = std::weak_ptr<execution_internal::NativeUploadRegistry>(
             native_uploads)](const ResourceCapacity& capacity) {
          if (auto temporary = uploads.lock())
            temporary->reclaim_capacity(capacity);
          if (auto retained = weak.lock())
            retained->reclaim_capacity(capacity);
        });
  }
  if (gpu_available) {
    gpu_pool = std::make_unique<ThreadPool>(1U, Backend::Gpu, waiting_admission,
                                            requested.collect_scheduler_timing);
  }
}

ExecutionContext::Impl::~Impl() = default;
/**
 * @brief Implements fixed local execution-resource construction.
 * @copydetails ExecutionContext::ExecutionContext
 */
ExecutionContext::ExecutionContext(
    std::shared_ptr<OperationRegistry> operations,
    ExecutionContextConfig config)
    : impl_(std::make_unique<Impl>(std::move(operations), config)) {
  execution_internal::bind_demand_context(impl_->demands, this);
}

/**
 * @brief Implements exact local worker/resource teardown.
 * @copydetails ExecutionContext::~ExecutionContext
 */
ExecutionContext::~ExecutionContext() noexcept {
  if (impl_)
    impl_->shared_results.shutdown();
  if (impl_ && impl_->demands)
    execution_internal::close_demand_coordinator(impl_->demands);
  if (impl_ && impl_->cache)
    impl_->cache->close();
}
void ExecutionContext::clear_result_cache() {
  impl_->result_checkpoints->clear();
  if (impl_->cache)
    impl_->cache->clear();
}
SchedulerStatistics ExecutionContext::scheduler_statistics() const {
  SchedulerStatistics result;
  result.enabled = impl_->cpu_pool.timing_enabled();
  if (result.enabled) {
    result.cpu = impl_->cpu_pool.statistics();
    if (impl_->gpu_pool)
      result.gpu = impl_->gpu_pool->statistics();
  }
  return result;
}
ResultCacheStatistics ExecutionContext::cache_statistics() const {
  auto result =
      impl_->cache ? impl_->cache->statistics() : ResultCacheStatistics{};
  const auto structured = impl_->shared_results.statistics();
  result.in_flight += structured.first;
  result.shared_computations += structured.second;
  return result;
}

/**
 * @brief Implements resolved CPU-worker count observation.
 * @copydetails ExecutionContext::cpu_workers
 */
Result<ResourceBudget> ExecutionContext::resource_budget() const {
  if (!impl_->budget->resources())
    return Result<ResourceBudget>(Status::failure(
        ErrorCode::NotFound, "managed resource root is not configured"));
  return Result<ResourceBudget>(*impl_->budget->resources());
}
std::uint32_t ExecutionContext::cpu_workers() const noexcept {
  return impl_ ? impl_->cpu_worker_count : 0U;
}

/**
 * @brief Implements optional local GPU-lane observation.
 * @copydetails ExecutionContext::gpu_enabled
 */
bool ExecutionContext::gpu_enabled() const noexcept {
  return impl_ && impl_->gpu_available &&
         (!impl_->native_device || impl_->native_device->available());
}

}  // namespace ps
