#include "photospider/execution/execution.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/numeric_diagnostics.hpp"
#include "core/stored_failure.hpp"
#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "data/whole_input_view.hpp"
#include "execution/accounted_regions.hpp"
#include "execution/callback_queue_timing.hpp"
#include "execution/cpu_range.hpp"
#include "execution/cpu_tiles.hpp"
#include "execution/dependency_records.hpp"
#include "execution/execution_timing.hpp"
#include "execution/memory_budget.hpp"
#include "execution/native_gpu.hpp"
#include "execution/publication_diagnostics.hpp"
#include "execution/resource_observation.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_checkpoints.hpp"
#include "execution/result_native_upload.hpp"
#include "execution/shared_results.hpp"
#include "execution/structured_execution.hpp"
#include "photospider/execution/data_movement.hpp"
#include "plugin/dependency_identity.hpp"

#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps {
/** @brief Shared backing for immutable frozen executions.
 * @details `allocate_shared` charges this state object and its shared control
 * block through `resources`. The state owns its plan, copied bindings,
 * operation registry, and identity; allocations inside the plan and standard
 * containers retain their existing allocator accounting.
 */
struct FrozenExecution::State {
  explicit State(const ResourceBudget& budget) : resources(budget) {}
  ResourceBudget resources;
  ExecutionPlan plan;
  ExecutionBindings bindings;
  std::shared_ptr<OperationRegistry> operations;
  std::string execution_identity;
};
namespace {

#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
/**
 * @brief Allocation-free test exception exposing a null diagnostic pointer.
 *
 * @note This type exists only in the noninstalled test-kernel variant and
 * exercises defensive `std::exception::what()` handling at the scheduler
 * exception fence.
 */
class NullDiagnosticException final : public std::exception {
 public:
  /**
   * @brief Returns the deliberately absent test diagnostic.
   * @return Null by design.
   * @throws Nothing.
   */
  [[nodiscard]] const char* what() const noexcept override { return nullptr; }
};
#endif

Status scheduler_exception_status(ErrorCode code,
                                  const char* diagnostic = nullptr) noexcept {
  const auto reason = code == ErrorCode::ResourceExhausted
                          ? FailureReason::CapacityLimit
                          : FailureReason::HostException;
  Status fallback{code, {}, reason};
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  if (execution_testing::fail_failure_status_construction(
          execution_testing::FailureStatusConstructionPoint::ExceptionFence))
    return fallback;
#endif
  try {
    fallback.message =
        diagnostic ? std::string(diagnostic).substr(0, 4096) : "";
  } catch (...) {
  }
  return fallback;
}

using execution_internal::WaitingAdmission;

/**
 * @brief Move-only backend-queue entry with shared admission ownership.
 *
 * @note Destruction before worker start rolls back the waiting token.
 */
struct QueuedCallback final {
  /** @brief Complete callback invoked by exactly one worker. */
  std::function<void()> callback;
  /** @brief Aggregate waiting admission released before callback entry. */
  WaitingAdmission::Lease admission;
  /** @brief Optional completion notification after the callback body retires.
   */
  std::function<void()> retired = {};
  /** @brief Shared envelope lease; only its waiting slot retires at entry. */
  ResourceLease managed_queue;
  std::chrono::steady_clock::time_point published_at{};
};

/**
 * @brief Fixed-size worker pool with one deterministic backend FIFO.
 *
 * Workers execute callbacks outside the queue mutex. Destruction rejects any
 * callback that has not started, requests every worker to stop, and joins the
 * complete worker set. Queue envelopes already own context-wide waiting
 * admission, so the lane has no independent capacity that could multiply the
 * public bound.
 *
 * @note Callers must arrange for no outstanding execution to depend on queued
 * callbacks when destroying the pool.
 */
class ThreadPool final {
 public:
  /**
   * @brief Starts a fixed number of callback workers.
   * @param worker_count Positive number of owned threads.
   * @param backend Exact backend lane owned by this pool.
   * @throws std::invalid_argument If worker_count is zero.
   * @throws std::system_error If a worker cannot be created.
   * @throws std::bad_alloc If worker or queue storage allocation fails.
   * @note Partially created workers are stopped and joined before rethrow.
   */
  ThreadPool(std::uint32_t worker_count, Backend backend,
             WaitingAdmission& admission, bool collect_timing)
      : ranges_(mutex_, ready_, worker_count, admission),
        backend_(backend),
        timing_(collect_timing) {
    if (worker_count == 0U) {
      throw std::invalid_argument("thread-pool worker count must be positive");
    }
    try {
      workers_.reserve(worker_count);
      for (std::uint32_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this] { worker_loop(); });
      }
    } catch (...) {
      stop_and_join();
      throw;
    }
  }

  /**
   * @brief Rejects pending callbacks and joins every worker.
   * @throws Nothing.
   * @note A callback that already started is allowed to return cooperatively.
   */
  ~ThreadPool() noexcept { stop_and_join(); }

  /**
   * @brief Forbids duplicating worker and callback-queue ownership.
   * @param other Source pool that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note One pool joins exactly its own fixed worker set.
   */
  ThreadPool(const ThreadPool& other) = delete;
  /**
   * @brief Forbids assigning active worker/queue ownership.
   * @param other Source pool that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Shutdown remains bound to the constructing pool.
   */
  ThreadPool& operator=(const ThreadPool& other) = delete;

  /**
   * @brief Attempts to enqueue one callback without blocking.
   * @param callback Complete callback and shared waiting-admission ownership.
   * @return True when queued; false when stopped.
   * @throws std::bad_alloc If queue allocation fails before mutation.
   * @throws std::exception In the noninstalled test variant when a
   * deterministic null-diagnostic standard exception is selected.
   * @note A true return transfers callback/token ownership to exactly one
   * worker; false or exception destroys the parameter and rolls admission back.
   */
  [[nodiscard]] bool submit(QueuedCallback callback) {
    const auto begin = timing_.enabled()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    const execution_testing::CallbackSubmitAction test_action =
        execution_testing::callback_submit_action(backend_);
    if (test_action == execution_testing::CallbackSubmitAction::Drop)
      return true;
    if (test_action == execution_testing::CallbackSubmitAction::Reject) {
      return false;
    }
    if (test_action == execution_testing::CallbackSubmitAction::ThrowBadAlloc) {
      throw std::bad_alloc();
    }
    if (test_action ==
        execution_testing::CallbackSubmitAction::ThrowNullDiagnostic) {
      throw NullDiagnosticException();
    }
#endif
    if (stopping_) {
      return false;
    }
    callbacks_.push_back(std::move(callback));
    if (timing_.enabled()) {
      const auto published = std::chrono::steady_clock::now();
      callbacks_.back().published_at = published;
      timing_.published(begin, published, callbacks_.size());
    }
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_callback_queued(backend_);
#endif
    ready_.notify_one();
    return true;
  }

 public:
  CallbackQueueStatistics statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    return timing_.statistics();
  }
  bool timing_enabled() const noexcept { return timing_.enabled(); }
  execution_internal::CpuRangeQueue& ranges() noexcept { return ranges_; }

 private:
  /**
   * @brief Runs callbacks until stop is requested.
   * @throws Nothing across the thread boundary.
   * @note Submitted callbacks are required to fence their own exceptions.
   */
  void worker_loop() noexcept {
    execution_internal::in_kernel_worker = true;
    for (;;) {
      QueuedCallback callback;
      execution_internal::CpuRangeQueue::Claim range;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] {
          return stopping_ || !callbacks_.empty() || ranges_.ready_locked();
        });
        if (stopping_) {
          return;
        }
        if (prefer_range_ || callbacks_.empty()) {
          range = ranges_.take_locked();
          if (range.job)
            prefer_range_ = false;
        }
        if (!range.job) {
          // Cancellation can remove range eligibility after the wait predicate.
          if (callbacks_.empty())
            continue;
          callback = std::move(callbacks_.front());
          callbacks_.pop_front();
          if (timing_.enabled())
            timing_.started(callback.published_at,
                            std::chrono::steady_clock::now());
          prefer_range_ = true;
        }
      }
      if (range.job) {
        ranges_.execute(range);
        continue;
      }
      callback.admission.release();
      if (callback.managed_queue.valid()) {
        ResourceCapacity waiting;
        waiting[ResourceKind::Queue] = 1;
        (void)callback.managed_queue.shrink(waiting);
      }
      try {
        callback.callback();
      } catch (...) {
        // Execution callbacks have their own status fence. This final fence
        // preserves pool liveness if a future callback violates that contract.
      }
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_callback_body_finished();
#endif
      if (callback.retired)
        callback.retired();
    }
  }

  /**
   * @brief Performs the idempotent stop, reject, and join sequence.
   * @throws Nothing.
   * @note Joining never occurs while the queue mutex is held.
   */
  void stop_and_join() noexcept {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!stopping_) {
        stopping_ = true;
        while (!callbacks_.empty()) {
          auto rejected = std::move(callbacks_.front());
          callbacks_.pop_front();
          lock.unlock();
          rejected.admission.release();
          rejected.callback = {};
          rejected.retired = {};
          rejected.managed_queue = {};
          lock.lock();
        }
      }
    }
    ready_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  /** @brief Serializes callback queue and stop state. */
  std::mutex mutex_;
  /** @brief Wakes workers for callbacks or shutdown. */
  std::condition_variable ready_;
  execution_internal::CpuRangeQueue ranges_;
  bool prefer_range_ = true;
  /** @brief Deterministic FIFO governed by shared waiting admission. */
  std::deque<QueuedCallback> callbacks_;
  /** @brief Owned fixed worker set. */
  std::vector<std::thread> workers_;
  /** @brief Exact local backend lane served by this pool. */
  [[maybe_unused]] const Backend backend_;
  execution_internal::CallbackQueueMeter timing_;
  /** @brief Monotonic stop flag guarded by `mutex_`. */
  bool stopping_ = false;
};

using execution_internal::MemoryBudget;
using execution_internal::MemoryReservation;

/**
 * @brief Resolves a bounded positive default CPU worker count.
 * @param configured Explicit caller value, or zero for hardware-based default.
 * @return Positive count no greater than 32 for the default path.
 * @throws Nothing.
 */
std::uint32_t resolve_cpu_workers(std::uint32_t configured) noexcept {
  if (configured != 0U) {
    return configured;
  }
  const std::uint32_t hardware = std::thread::hardware_concurrency();
  return std::max(1U, std::min(32U, hardware == 0U ? 1U : hardware));
}

/**
 * @brief Checked addition used by transfer diagnostics.
 * @param left First unsigned value.
 * @param right Second unsigned value.
 * @return Sum or `ResourceExhausted` on overflow.
 * @throws std::bad_alloc If diagnostic allocation fails.
 */
Result<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return Result<std::uint64_t>(
        Status::failure(ErrorCode::ResourceExhausted,
                        "execution diagnostic byte count overflows uint64"));
  }
  return Result<std::uint64_t>(left + right);
}

/** @brief Computes packed bytes for demanded coverage without allocating
 * payload. */
Result<std::uint64_t> region_bytes(const ValueDescriptor& descriptor,
                                   const Region& region) {
  if (!region.validate(descriptor.shape).ok() || region.empty())
    return Result<std::uint64_t>(
        Status::failure(ErrorCode::InvalidArgument, "invalid packed region"));
  auto count = region.element_count();
  const auto width = Value::element_size(descriptor.element_type);
  if (!count.ok() || count.value() > UINT64_MAX / width)
    return Result<std::uint64_t>(Status::failure(
        ErrorCode::ResourceExhausted, "regional byte count overflows"));
  return Result<std::uint64_t>(count.value() * width);
}
/** @brief Copies only logical coverage into a packed destination with global
 * origin. */
Status copy_region(ValueView source, MutableValue* destination,
                   const Region& available,
                   const std::function<ErrorCode()>& stop = {}) {
  const auto& region = source.region();
  if (region.rank() != available.rank())
    return Status::failure(ErrorCode::TypeMismatch, "copy region rank differs");
  std::vector<std::uint64_t> coordinate;
  for (std::size_t i = 0; i < region.rank(); ++i) {
    const auto part = region.dimensions()[i],
               bounds = available.dimensions()[i];
    if (part.offset < bounds.offset ||
        part.offset + part.extent > bounds.offset + bounds.extent)
      return Status::failure(ErrorCode::TypeMismatch,
                             "copy exceeds destination coverage");
    coordinate.push_back(part.offset);
  }
  auto count = region.element_count();
  if (!count.ok())
    return count.status();
  const auto width = Value::element_size(source.descriptor().element_type);
  for (std::uint64_t element = 0; element < count.value(); ++element) {
    if ((element & 1023) == 0 && stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Status{code, {}};
    }
    auto from = source.byte_address(coordinate);
    if (!from.ok())
      return from.status();
    std::uint64_t to = 0;
    for (std::size_t axis = 0; axis < coordinate.size(); ++axis)
      to +=
          (coordinate[axis] - available.dimensions()[axis].offset) *
          static_cast<std::uint64_t>(destination->layout().byte_strides[axis]);
    if (to > destination->size() || width > destination->size() - to)
      return Status::failure(ErrorCode::TypeMismatch,
                             "copy exceeds destination bytes");
    std::memcpy(destination->data() + to, source.bytes().data() + from.value(),
                width);
    for (std::size_t reverse = coordinate.size(); reverse > 0; --reverse) {
      const auto axis = reverse - 1;
      ++coordinate[axis];
      const auto dim = region.dimensions()[axis];
      if (coordinate[axis] < dim.offset + dim.extent)
        break;
      coordinate[axis] = dim.offset;
    }
  }
  return Status::success();
}

/**
 * @brief Materializes one immutable Value into another local backend residency.
 * @param source Valid producer Value.
 * @return Packed physical backing when compact, otherwise a copied Value
 * preserving its source metadata.
 * @throws std::bad_alloc If transfer allocation fails.
 * @note Backend residency is tracked by the owning ExecutionRun; Value itself
 * remains backend-neutral and exposes no native device handle.
 */
Result<Value> transfer_value(const Value& source,
                             const BufferAllocator& allocator,
                             bool compact = false,
                             const std::function<ErrorCode()>& stop = {}) {
  execution_testing::ExecutionTiming timing(
      execution_testing::TimingKind::ValueMaterialization);
  if (compact) {
    auto allocated =
        MutableValue::allocate(source.descriptor(), source.region(), allocator);
    if (!allocated.ok())
      return Result<Value>(allocated.status());
    auto output = allocated.take_value();
    auto status =
        copy_region(ValueView(source), &output, source.region(), stop);
    if (!status.ok())
      return Result<Value>(status);
    auto published = std::move(output).publish();
    if (published.ok())
      timing.success(published.value().bytes().size());
    return published;
  }
  auto allocated = allocator.allocate(source.bytes().size());
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto buffer = allocated.take_value();
  std::memcpy(buffer.data(), source.bytes().data(), source.bytes().size());
  auto published = Value::from_storage(
      source.descriptor(), source.region(), source.layout(),
      std::move(buffer).freeze(), source.facets(), source.resources());
  if (published.ok())
    timing.success(published.value().bytes().size());
  return published;
}

/** @brief Availability of the scheduling lane, independently of native
 * capabilities in the noninstalled scheduler test kernel. */
bool gpu_lane_available(ThreadPool* lane,
                        const std::shared_ptr<gpu_internal::Device>& device) {
  if (!lane)
    return false;
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  if (!device)
    return true;
#endif
  return device && device->available();
}

}  // namespace

/** @brief Creates production native resources; scheduler fixtures use fake
 * lanes. */
std::shared_ptr<gpu_internal::Device> execution_device(
    bool enabled, const std::shared_ptr<ResourceBudget>& budget) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  // Scheduler fixtures default to hardware-independent lanes. Native lifetime
  // and cancellation fixtures explicitly opt into the production device.
  if (!execution_testing::use_native_device())
    return {};
#endif
  return enabled ? gpu_internal::Device::create(budget) : nullptr;
}
/** @brief Keys immutable physical coverage; a cache entry separately proves
 * that the source allocation is still alive. Facets do not change bytes. */
std::string upload_view_key(const Value& value) {
  content_internal::Sha256 hash;
  hash.text("photospider.native-view.v2");
  hash.integer(reinterpret_cast<std::uintptr_t>(value.storage().get()));
  hash.integer(value.layout().byte_offset);
  hash.integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash.integer(value.descriptor().shape.size());
  for (auto n : value.descriptor().shape)
    hash.integer(n);
  hash.integer(value.layout().origin.size());
  for (auto n : value.layout().origin)
    hash.integer(n);
  hash.integer(value.layout().byte_strides.size());
  for (auto n : value.layout().byte_strides)
    hash.integer(static_cast<std::uint64_t>(n));
  hash.integer(value.region().rank());
  for (auto d : value.region().dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  return hash.finish();
}

/** @brief Native physical upload identity, excluding Result semantic facts. */
Result<std::string> upload_content_key(const Value& value,
                                       const std::string& device,
                                       const std::function<ErrorCode()>& stop) {
  content_internal::Sha256 hash;
  hash.text("photospider.native-upload.v2");
  hash.text(device);
  hash.integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash.integer(value.descriptor().shape.size());
  for (auto n : value.descriptor().shape)
    hash.integer(n);
  hash.integer(value.region().rank());
  for (auto d : value.region().dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  auto count = value.region().element_count();
  if (!count.ok())
    return Result<std::string>(count.status());
  const auto width = Value::element_size(value.descriptor().element_type);
  std::vector<std::uint64_t> coordinate(value.region().rank());
  for (std::uint64_t i = 0; i < count.value(); ++i) {
    if ((i & 1023) == 0) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Result<std::string>(Status{code, {}});
    }
    auto index = i;
    for (std::size_t axis = coordinate.size(); axis > 0; --axis) {
      const auto d = value.region().dimensions()[axis - 1];
      coordinate[axis - 1] = d.offset + index % d.extent;
      index /= d.extent;
    }
    auto address = value.byte_address(coordinate);
    if (!address.ok())
      return Result<std::string>(address.status());
    hash.bytes(value.bytes().data() + address.value(), width);
  }
  return Result<std::string>(hash.finish());
}

/** @brief Binds this observation's metadata to an immutable packed backing. */
Result<Value> uploaded_view(const Value& source,
                            std::shared_ptr<const CpuStorage> backing) {
  auto packed = source.descriptor();
  for (std::size_t i = 0; i < source.region().rank(); ++i)
    packed.shape[i] = source.region().dimensions()[i].extent;
  auto dense = input_internal::dense_metadata(packed);
  if (!dense.ok())
    return Result<Value>(dense.status());
  auto layout = dense.take_value().layout;
  for (const auto dim : source.region().dimensions())
    layout.origin.push_back(dim.offset);
  return Value::from_storage(source.descriptor(), source.region(),
                             std::move(layout), std::move(backing),
                             source.facets(), source.resources());
}

struct DemandHandle::Impl {
  struct Publication {
    DemandQuery query;
    ExecutionDependencies dependencies;
    DemandQuery dirty;
    std::uint64_t weight = 0;
  };
  std::weak_ptr<execution_internal::DemandCoordinator> owner;
  std::shared_ptr<const FrozenExecution> bundle;
  std::atomic<std::uint64_t> generation{1};
  CancellationSource cancellation;
  DemandConfig config;
  std::uint64_t revision = 0, metadata_entries = 0;
  std::map<std::string, std::shared_ptr<const Publication>> publications;
};
namespace execution_internal {
/** @brief One context lifetime/publication lock; owns no threads or pixels. */
struct DemandCoordinator : std::enable_shared_from_this<DemandCoordinator> {
  std::mutex mutex;
  std::condition_variable changed;
  CancellationSource shutdown;
  bool closing = false;
  std::size_t calls = 0;
  std::uint64_t next = 1;
  std::uint32_t maximum_handles;
  std::uint64_t maximum_calls;
  ExecutionContext* context = nullptr;
  struct HandleEntry {
    std::weak_ptr<DemandHandle::Impl> handle;
    CancellationToken cancellation;
  };
  std::map<std::uint64_t, HandleEntry> handles;
  DemandCoordinator(std::uint32_t maximum_handles, std::uint64_t maximum_calls)
      : maximum_handles(maximum_handles), maximum_calls(maximum_calls) {}
  struct Lease {
    std::shared_ptr<DemandCoordinator> owner;
    std::shared_ptr<DemandHandle::Impl> handle;
    std::shared_ptr<const FrozenExecution> bundle;
    std::uint64_t generation = 0, revision = 0;
    CancellationToken cancellation;
    bool active = false;
    ~Lease() {
      if (active) {
        std::lock_guard<std::mutex> lock(owner->mutex);
        --owner->calls;
        owner->changed.notify_all();
      }
    }
    Status stop(Status status = {}) const {
      if (status.detail.origin == FailureOrigin::Protocol)
        return status;
      if (cancellation.cancelled())
        return Status{ErrorCode::Cancelled, {}};
      if (handle->generation.load(std::memory_order_acquire) != generation)
        return Status{ErrorCode::Stale, {}};
      return status;
    }
  };
  Result<std::shared_ptr<Lease>> acquire(
      const std::shared_ptr<DemandHandle::Impl>& handle,
      const CancellationToken& cancellation) {
    auto lease = std::make_shared<Lease>();
    lease->owner = shared_from_this();
    lease->handle = handle;
    auto combined = CancellationToken::combine(
        {shutdown.token(), handle->cancellation.token(), cancellation});
    if (!combined.ok())
      return Result<std::shared_ptr<Lease>>(combined.status());
    lease->cancellation = combined.take_value();
    std::lock_guard<std::mutex> lock(mutex);
    if (closing || lease->cancellation.cancelled())
      return Result<std::shared_ptr<Lease>>(Status{ErrorCode::Cancelled, {}});
    if (!handle->bundle)
      return Result<std::shared_ptr<Lease>>(Status{ErrorCode::Stale, {}});
    if (calls >= maximum_calls)
      return Result<std::shared_ptr<Lease>>(
          Status{ErrorCode::ResourceExhausted, "active demand call limit"});
    lease->bundle = handle->bundle;
    lease->generation = handle->generation.load(std::memory_order_relaxed);
    lease->revision = handle->revision;
    ++calls;
    lease->active = true;
    return Result<std::shared_ptr<Lease>>(std::move(lease));
  }
  void close() noexcept {
    std::unique_lock<std::mutex> lock(mutex);
    closing = true;
    shutdown.cancel();
    for (const auto& item : handles)
      if (auto handle = item.second.handle.lock()) {
        handle->cancellation.cancel();
        handle->publications.clear();
        handle->metadata_entries = 0;
        auto retired = std::move(handle->bundle);
        lock.unlock();
        retired.reset();
        lock.lock();
      }
    changed.wait(lock, [&] { return calls == 0; });
    handles.clear();
    context = nullptr;
  }
};
}  // namespace execution_internal

/**
 * @brief Opaque fixed resource ownership for ExecutionContext.
 * @note Destruction order stops the optional GPU lane and required CPU pool
 * before releasing their shared waiting-admission, resource-ledger, and
 * registry owners.
 */
struct ExecutionContext::Impl final {
  /**
   * @brief Creates both backend pools and shared waiting/byte admission owners.
   * @param operations Frozen operation registry.
   * @param requested Caller configuration.
   * @throws std::invalid_argument If configuration or registry is invalid.
   * @throws std::bad_alloc If owned state cannot be allocated.
   * @throws std::system_error If a worker thread cannot be created.
   */
  Impl(std::shared_ptr<OperationRegistry> operations,
       ExecutionContextConfig requested)
      : cpu_worker_count(resolve_cpu_workers(requested.cpu_workers)),
        budget(std::make_shared<MemoryBudget>(
            requested.maximum_live_bytes,
            [&] {
              auto limits =
                  requested.managed_resources.value_or(ResourceLimits{});
              limits.capacity[ResourceKind::Payload] =
                  std::min(limits.capacity[ResourceKind::Payload],
                           requested.maximum_live_bytes);
              return std::make_shared<ResourceBudget>(std::move(limits));
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
    demands = std::make_shared<execution_internal::DemandCoordinator>(
        requested.maximum_demands,
        static_cast<std::uint64_t>(maximum_waiting_callbacks) +
            cpu_worker_count + 1);
    if (!requested.maximum_result_checkpoint_scopes ||
        requested.maximum_result_checkpoint_scopes > 1048576)
      throw std::invalid_argument("invalid Result checkpoint scope limit");
    result_checkpoints =
        std::make_unique<execution_internal::ResultCheckpoints>(
            requested.maximum_result_checkpoint_scopes);
    if (!requested.maximum_dependency_cache_metadata ||
        requested.maximum_dependency_cache_metadata > 1048576)
      throw std::invalid_argument("invalid dependency cache metadata limit");
    if (requested.result_cache_bytes > requested.maximum_live_bytes)
      throw std::invalid_argument("cache limit exceeds execution budget");
    if (requested.result_cache_bytes != 0)
      cache = std::make_shared<execution_internal::ResultCache>(
          requested.result_cache_bytes, budget,
          std::min<std::uint32_t>(cpu_worker_count, 4),
          maximum_waiting_callbacks,
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
      gpu_pool =
          std::make_unique<ThreadPool>(1U, Backend::Gpu, waiting_admission,
                                       requested.collect_scheduler_timing);
    }
  }

  /** @brief Fixed resolved CPU worker count. */
  const std::uint32_t cpu_worker_count;
  /** @brief Shared exact modeled-byte capacity, retained beyond the device. */
  std::shared_ptr<MemoryBudget> budget;
  std::shared_ptr<gpu_internal::Device> native_device;
  /** @brief Fixed optional GPU-lane availability. */
  const bool gpu_available;
  /** @brief Context-wide maximum callbacks waiting across all lanes. */
  const std::uint32_t maximum_waiting_callbacks;
  const std::uint64_t maximum_live_bytes;
  /** @brief Frozen operation registry retained beyond all callbacks. */
  std::shared_ptr<OperationRegistry> operation_registry;
  /** @brief Shared CPU/GPU waiting-callback admission owner. */
  WaitingAdmission waiting_admission;
  /** @brief Required fixed CPU callback pool. */
  ThreadPool cpu_pool;
  /** @brief Optional single local GPU callback lane. */
  std::unique_ptr<ThreadPool> gpu_pool;
  // Destroy coordinators before callback pools and their allocation budget.
  std::shared_ptr<execution_internal::ResultCache> cache;
  std::shared_ptr<execution_internal::NativeUploadRegistry> native_uploads;
  std::shared_ptr<execution_internal::DemandCoordinator> demands;
  execution_internal::SharedResults shared_results;
  std::unique_ptr<execution_internal::ResultCheckpoints> result_checkpoints;
};

namespace {

Status retain_managed_inputs(std::vector<ExecutionBinding>* bindings,
                             const std::shared_ptr<MemoryBudget>& budget) {
  if (!budget->resources())
    return Status{ErrorCode::InvalidArgument,
                  "Result execution requires managed_resources"};
  for (const auto& binding : *bindings)
    if (!binding.result.owned_by(*budget->resources()))
      return Status{ErrorCode::InvalidArgument,
                    "Result binding belongs to a different resource root"};
  return Status::success();
}

/** @brief Allocation-free stop selection after a successful plan entry check.
 */
ErrorCode binding_stop(const ExecutionPlan& plan,
                       const CancellationToken& cancellation) noexcept {
  if (cancellation.cancelled())
    return ErrorCode::Cancelled;
  return plan.current() ? ErrorCode::Ok : ErrorCode::Stale;
}

/** @brief Validates/copies complete regional binding metadata before any source
 * callback. */
Result<std::vector<ExecutionBinding>> preflight_regional_bindings(
    const ExecutionPlan& plan, const ExecutionBindings& bindings,
    const CancellationToken& cancellation, const ResourceBudget& root) {
  if (bindings.inputs.size() > 4096)
    return Result<std::vector<ExecutionBinding>>(
        Status::failure(ErrorCode::InvalidArgument, "too many bindings"));
  std::map<std::string, std::vector<const ExecutionBinding*>> named;
  for (const auto& binding : bindings.inputs)
    named[binding.name].push_back(&binding);
  for (const auto& entry : named)
    if (!input_internal::valid_input_name(entry.first))
      return Result<std::vector<ExecutionBinding>>(Status::failure(
          ErrorCode::InvalidArgument, "malformed binding name"));
  for (const auto& entry : named)
    if (entry.second.size() != 1)
      return Result<std::vector<ExecutionBinding>>(Status::failure(
          ErrorCode::InvalidArgument, "duplicate binding name"));
  std::set<std::string> declared;
  for (const auto& declaration : plan.input_declarations())
    declared.insert(declaration.name);
  for (const auto& entry : named)
    if (declared.count(entry.first) == 0)
      return Result<std::vector<ExecutionBinding>>(
          Status::failure(ErrorCode::InvalidArgument, "extra binding name"));
  for (const auto& name : declared)
    if (named.count(name) == 0)
      return Result<std::vector<ExecutionBinding>>(
          Status::failure(ErrorCode::InvalidArgument, "missing binding name"));
  std::vector<ExecutionBinding> result;
  for (const auto& declaration : plan.input_declarations()) {
    auto binding = *named.at(declaration.name)[0];
    if (!binding.result.valid())
      return Result<std::vector<ExecutionBinding>>(Status{
          ErrorCode::InvalidArgument, "binding requires an immutable Result"});
    if (!binding.result.owned_by(root))
      return Result<std::vector<ExecutionBinding>>(
          Status{ErrorCode::InvalidArgument,
                 "Result binding belongs to a different Root"});
    if (!declaration.result_schema ||
        !binding.result.schema().same_schema(*declaration.result_schema) ||
        !binding.result.descriptor().ok())
      return Result<std::vector<ExecutionBinding>>(
          Status{ErrorCode::TypeMismatch,
                 "Result binding schema or finality differs"});
    result.push_back(std::move(binding));
  }
  const auto stop = [&] { return binding_stop(plan, cancellation); };
  for (const auto& step : plan.steps()) {
    for (std::size_t port = 0; port < step.inputs.size(); ++port) {
      const auto* input = std::get_if<PlanWorkflowInput>(&step.inputs[port]);
      if (!input)
        continue;
      const auto& binding = result[input->declaration_index];
      const auto& constraint = step.traits.input_schema[port];
      if (constraint.scalar_bounds) {
        const auto& selected = step.traits.outputs[0].input_indices;
        if (selected && std::find(selected->begin(), selected->end(), port) ==
                            selected->end())
          continue;
        auto facts = binding.result.descriptor();
        if (!facts.ok())
          return Result<std::vector<ExecutionBinding>>(facts.status());
        OperationMetadata metadata;
        metadata.result_schema =
            plan.input_declarations()[input->declaration_index].result_schema;
        auto status = input_internal::validate_port_tensor(
            constraint, binding.result, facts.value(), metadata,
            ErrorCode::InvalidArgument, cancellation, stop);
        if (!status.ok()) {
          status.detail.input_id =
              plan.input_declarations()[input->declaration_index].id;
          if (status.detail.origin == FailureOrigin::Unspecified &&
              (status.code == ErrorCode::TypeMismatch ||
               status.code == ErrorCode::InvalidArgument ||
               status.code == ErrorCode::OperationFailed))
            status.detail.origin = status.code == ErrorCode::TypeMismatch
                                       ? FailureOrigin::Schema
                                       : FailureOrigin::Domain;
          return Result<std::vector<ExecutionBinding>>(status);
        }
        continue;
      }
    }
  }
  return Result<std::vector<ExecutionBinding>>(std::move(result));
}

/** @brief Finds work needed for outputs, stopping at Run-local
 * materializations. */
/**
 * @brief Coordinates one dependency-ordered execution through shared pools.
 *
 * The coordinator owns all per-execution mutable state, drains every started
 * callback, assembles the complete local result under its mutex, and publishes
 * success only after a final cancellation-then-currentness recheck. It never
 * stores itself in global state.
 */
class ExecutionRun final {
  template <class T>
  struct StageCompletion {
    ResourceLease lease;
    std::promise<Result<T>> promise;
    std::future<Result<T>> future{promise.get_future()};
    Result<T> result{Status{ErrorCode::Internal, {}}};
    std::function<Result<T>()> work;
  };
  template <class T>
  struct StageRetirement {
    std::shared_ptr<StageCompletion<T>> completion;
    std::atomic<bool> delivered{false};
    explicit StageRetirement(std::shared_ptr<StageCompletion<T>> owner)
        : completion(std::move(owner)) {}
    ~StageRetirement() { finish(true); }
    void finish(bool rejected = false) noexcept {
      if (delivered.exchange(true))
        return;
      completion->work = {};
      if (rejected) {
        completion->result = Result<T>(Status{ErrorCode::Cancelled, {}});
        if (completion->lease.valid()) {
          ResourceCapacity waiting;
          waiting[ResourceKind::Queue] = 1;
          (void)completion->lease.shrink(waiting);
        }
      }
      auto promise = std::move(completion->promise);
      try {
        promise.set_value(std::move(completion->result));
      } catch (...) {
        try {
          promise.set_exception(std::current_exception());
        } catch (...) {
        }
      }
    }
  };

 public:
  /** @brief Runs a structured Result plan on the calling coordinator.
   * @note The coordinator advances the Root-owned Actor graph and submits
   * ready stages through the context's existing worker, admission, and
   * allocator owners. Workers do not wait for upstream Result producers.
   */
  static Result<ExecutionResult> run_results(
      ThreadPool* pool, ThreadPool* gpu_pool,
      const std::shared_ptr<gpu_internal::Device>& native_device,
      const std::shared_ptr<execution_internal::NativeUploadRegistry>&
          native_uploads,
      WaitingAdmission* admission, const std::shared_ptr<MemoryBudget>& budget,
      const std::shared_ptr<OperationRegistry>& operations,
      const ExecutionPlan& plan, std::function<bool()> current,
      std::vector<ExecutionBinding> bindings,
      const CancellationToken& caller_cancellation,
      const ExecutionOptions& options, const DemandQuery* requested = nullptr,
      const std::string& snapshot_identity = {},
      execution_internal::ResultCache* dependency_cache = nullptr,
      execution_internal::SharedResults* shared_results = nullptr,
      bool atom_outcomes = false,
      execution_internal::ResultCheckpoints* result_checkpoints = nullptr,
      std::shared_ptr<const ExecutionPlan> plan_owner = {}) {
    auto payload_observation =
        budget->resources()
            ? std::allocate_shared<execution_internal::PayloadObservation>(
                  ResourceAllocator<execution_internal::PayloadObservation>(
                      *budget->resources()))
            : std::make_shared<execution_internal::PayloadObservation>();
    std::optional<execution_internal::ResourcePayloadScope> payload_scope;
    if (budget->resources())
      payload_scope.emplace(
          *budget->resources(),
          execution_internal::PayloadCapture{payload_observation, {}});
    ErrorCode coordinator_metadata_failure = ErrorCode::Ok;
    std::optional<ResourceAllocationScope> coordinator_resources;
    // Frozen cache/flight paths install their own optional-retention scopes;
    // keep their best-effort allocation failures outside this sticky fence.
    if (budget->resources() && !dependency_cache)
      coordinator_resources.emplace(*budget->resources(),
                                    &coordinator_metadata_failure);
    auto combined_cancellation =
        budget->resources()
            ? CancellationToken::combine(
                  {caller_cancellation, options.dependencies.sets.cancellation},
                  *budget->resources())
            : CancellationToken::combine(
                  {caller_cancellation,
                   options.dependencies.sets.cancellation});
    if (!combined_cancellation.ok())
      return Result<ExecutionResult>(combined_cancellation.status());
    const auto cancellation = combined_cancellation.take_value();
    const auto stop = [&] { return binding_stop(plan, cancellation); };
    const auto fail = [&](Status status) {
      const auto code = stop();
      if (code != ErrorCode::Ok &&
          status.detail.origin != FailureOrigin::Protocol) {
        status =
            Status{code,
                   {},
                   code == ErrorCode::Cancelled ? FailureReason::Cancelled
                                                : FailureReason::StaleVersion,
                   {FailureOrigin::Cancellation, FailureScope::Run}};
      }
      return Result<ExecutionResult>(std::move(status));
    };
    auto admitted_resources =
        budget->resources() ? plan.resources().reference(*budget->resources())
                            : Result<ResourceBindings>(plan.resources());
    if (!admitted_resources.ok())
      return fail(admitted_resources.status());
    const auto resources = admitted_resources.take_value();
    auto retained_inputs = retain_managed_inputs(&bindings, budget);
    if (!retained_inputs.ok())
      return fail(retained_inputs);
    if (atom_outcomes && (!plan.structured_network() || !budget->resources()))
      return fail(Status{
          ErrorCode::InvalidArgument,
          "atom execution requires a managed CPU Result dependency plan"});
    if (plan.dependency_network()) {
      if (plan.structured_network() && !budget->resources())
        return fail(Status{ErrorCode::InvalidArgument,
                           "structured execution requires managed_resources"});
      const auto stage_root = budget->resources()
                                  ? budget->resources()
                                  : std::make_shared<ResourceBudget>();
      std::shared_ptr<execution_internal::NativeRunUploads> uploads;
      if (native_device && native_device->available() &&
          std::any_of(
              plan.steps().begin(), plan.steps().end(),
              [](const auto& step) { return step.backend == Backend::Gpu; }))
        uploads = std::allocate_shared<execution_internal::NativeRunUploads>(
            ResourceAllocator<execution_internal::NativeRunUploads>(
                *stage_root),
            native_uploads, *stage_root);
      const auto cache_epoch = dependency_cache ? dependency_cache->epoch() : 0;
      auto result = execution_internal::execute_structured(
          plan, std::move(bindings), operations, *stage_root, options,
          cancellation,
          [current, cancellation] {
            return cancellation.cancelled() ? ErrorCode::Cancelled
                   : current && current()   ? ErrorCode::Ok
                                            : ErrorCode::Stale;
          },
          [pool, gpu_pool, native_device, budget, stage_root, admission,
           current, uploads, dependency_cache, cache_epoch,
           maximum_parallelism = options.maximum_parallelism](
              Backend backend, bool whole, bool tiles, bool frozen_producer,
              CancellationToken token,
              std::function<Status(
                  const execution_internal::StructuredServices&)>
                  task) -> Result<execution_internal::StructuredSubmission> {
            using Submission = execution_internal::StructuredSubmission;
            using Answer = Result<Submission>;
            // A frozen shared producer serves peers independently of its
            // originating caller generation. Its shared token still controls
            // cancellation, and the caller retains its original final check.
            const auto callback_current =
                frozen_producer ? std::function<bool()>([] { return true; })
                                : current;
            if (backend == Backend::Gpu &&
                !gpu_lane_available(gpu_pool, native_device)) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
              execution_testing::notify_before_scheduler_failure(
                  execution_testing::SchedulerFailurePoint::
                      GpuBackendUnavailable);
#endif
              return Answer(Status{ErrorCode::BackendUnavailable,
                                   "native GPU Result lane is unavailable"});
            }
            if (tiles) {
              auto issued = stage_root->consume({0, 0, 0, 1});
              if (!issued.ok())
                return Answer(issued);
              execution_internal::CpuTileScope scope(
                  pool->ranges(), token, stage_root.get(),
                  maximum_parallelism ? maximum_parallelism
                                      : pool->ranges().workers(),
                  callback_current);
              execution_internal::StructuredServices services;
              services.native_gpu_available =
                  gpu_lane_available(gpu_pool, native_device);
              services.native_storage = [native_device](const auto& storage) {
                return native_device && native_device->owns(storage);
              };
              services.cpu_tiles = scope.service();
              services.allocator = stage_root->allocator();
              services.observe = [&](auto& d) {
                d.cpu_stage_count += scope.stages();
                d.cpu_tile_callback_count += scope.tiles();
              };
              auto status = task(services);
              return Answer(
                  Submission{{},
                             {},
                             scope.status().ok() ? status : scope.status()});
            }
            auto submitted = submit_result_stage<int>(
                backend == Backend::Gpu ? gpu_pool : pool, admission,
                [pool, native_device, budget, stage_root, callback_current,
                 backend, whole, uploads, dependency_cache, cache_epoch,
                 native_gpu_available =
                     gpu_lane_available(gpu_pool, native_device),
                 token = std::move(token), task = std::move(task)] {
                  if (token.cancelled())
                    return Result<int>(Status{ErrorCode::Cancelled, {}});
                  if (callback_current && !callback_current())
                    return Result<int>(Status{ErrorCode::Stale, {}});
                  execution_internal::StructuredServices services;
                  services.native_storage =
                      [native_device](const auto& storage) {
                        return native_device && native_device->owns(storage);
                      };
                  services.native_gpu_available = native_gpu_available;
                  services.allocator = stage_root->allocator();
                  std::optional<gpu_internal::Invocation> native;
                  std::uint64_t upload_hits = 0;
                  if (backend == Backend::Gpu &&
                      (!native_device || !native_device->available())) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
                    if (native_device)
#endif
                      return Result<int>(Status{
                          ErrorCode::BackendUnavailable,
                          "native GPU Result device became unavailable"});
                  }
                  // Scheduler fixtures borrow a GPU queue label without native
                  // buffers/services or any claimed device dispatch work.
                  if (backend == Backend::Gpu && native_device &&
                      native_device->available()) {
                    services.allocator =
                        native_device->allocator(services.allocator);
                    native.emplace(native_device, token, services.allocator);
                    services.gpu = native->service();
                    services.gpu_status = [&] { return native->status(); };
                    services.gpu_dispatches = [&] {
                      return native->statistics().dispatches;
                    };
                    services.native_input =
                        [&](const Value& source,
                            const std::function<Status(std::uint64_t)>&
                                cache_work)
                        -> Result<std::pair<Value, std::uint64_t>> {
                      using Answer = Result<std::pair<Value, std::uint64_t>>;
                      const auto stop = [&] {
                        return token.cancelled() ? ErrorCode::Cancelled
                               : callback_current && !callback_current()
                                   ? ErrorCode::Stale
                                   : ErrorCode::Ok;
                      };
                      if (stop() != ErrorCode::Ok)
                        return Answer(Status{stop(), {}});
                      if (native_device->owns(*source.storage()))
                        return Answer(std::make_pair(source, std::uint64_t{0}));
                      auto bytes =
                          region_bytes(source.descriptor(), source.region());
                      if (!bytes.ok())
                        return Answer(bytes.status());
                      std::string view_key;
                      std::shared_ptr<const CpuStorage> physical;
                      if (cache_work) {
                        auto issued =
                            cache_work(32 + 5 * source.region().rank() +
                                       uploads->lookup_work());
                        if (!issued.ok() &&
                            issued.code != ErrorCode::ResourceExhausted)
                          return Answer(issued);
                        if (issued.ok()) {
                          view_key = upload_view_key(source);
                          physical = uploads->find(view_key, source.storage());
                        }
                      }
                      std::string content_key;
                      if (!physical && dependency_cache && cache_work &&
                          !view_key.empty()) {
                        const auto count = source.region().element_count();
                        const auto per_sample = 1 + source.region().rank();
                        if (!count.ok())
                          return Answer(count.status());
                        if (count.value() <=
                            (UINT64_MAX - bytes.value()) / per_sample) {
                          const auto device_identity =
                              native_device->identity();
                          const auto framing = 32 + device_identity.size() +
                                               3 * source.region().rank();
                          const auto sample_work =
                              bytes.value() + count.value() * per_sample;
                          auto issued =
                              sample_work <= UINT64_MAX - framing
                                  ? cache_work(sample_work + framing)
                                  : Status{ErrorCode::ResourceExhausted, {}};
                          if (!issued.ok() &&
                              issued.code != ErrorCode::ResourceExhausted)
                            return Answer(issued);
                          if (issued.ok()) {
                            auto key = upload_content_key(
                                source, device_identity, stop);
                            if (!key.ok())
                              return Answer(key.status());
                            content_key = key.take_value();
                            auto retained =
                                dependency_cache->get(content_key, cache_epoch);
                            if (retained.valid() &&
                                native_device->owns(*retained.storage()))
                              physical = retained.storage();
                          }
                        }
                      }
                      std::uint64_t copied_bytes = 0;
                      if (physical && native_device->owns(*physical)) {
                        ++upload_hits;
                      } else {
                        auto capacity =
                            native_device->allocation_capacity(bytes.value());
                        if (!capacity.ok())
                          return Answer(capacity.status());
                        if (capacity.value() < bytes.value())
                          return Answer(Status{ErrorCode::OperationFailed,
                                               "native capacity is too small"});
                        auto count = source.region().element_count();
                        const auto per_sample = 1 + source.region().rank();
                        if (!count.ok())
                          return Answer(count.status());
                        if (count.value() >
                            (UINT64_MAX - bytes.value()) / per_sample)
                          return Answer(
                              Status{ErrorCode::ResourceExhausted, {}});
                        auto issued = stage_root->consume(
                            {bytes.value() + count.value() * per_sample});
                        if (!issued.ok())
                          return Answer(issued);
                        if (stop() != ErrorCode::Ok)
                          return Answer(Status{stop(), {}});
                        auto copied = transfer_value(source, services.allocator,
                                                     true, stop);
                        if (!copied.ok())
                          return Answer(copied.status());
                        physical = copied.value().storage();
                        copied_bytes = bytes.value();
                        if (!content_key.empty() && stop() == ErrorCode::Ok)
                          dependency_cache->put(content_key, copied.value(),
                                                cache_epoch, true);
                      }
                      if (stop() != ErrorCode::Ok)
                        return Answer(Status{stop(), {}});
                      auto rebound = uploaded_view(source, physical);
                      if (!rebound.ok())
                        return Answer(rebound.status());
                      if (!view_key.empty())
                        uploads->put(view_key, source.storage(), physical);
                      return Answer(
                          std::make_pair(rebound.take_value(), copied_bytes));
                    };
                    services.observe = [&](auto& d) {
                      const auto& stats = native->statistics();
                      d.native_dispatch_count += stats.dispatches;
                      d.native_submission_count += stats.submissions;
                      d.native_compute_us += stats.device_us;
                      d.native_constant_bytes += stats.constant_bytes;
                      d.native_upload_hits += upload_hits;
                    };
                  }
                  execution_internal::CpuRangeScope ranges(
                      pool->ranges(), token, stage_root.get(),
                      callback_current);
                  if (backend == Backend::Cpu && whole)
                    services.cpu_parallel = ranges.service();
                  auto status = task(services);
                  if (!ranges.status().ok())
                    status = ranges.status();
                  if (native && !native->status().ok())
                    status = native->status();
                  return status.ok() ? Result<int>(1) : Result<int>(status);
                },
                stage_root.get());
            if (!submitted.ok())
              return Answer(submitted.status());
            auto completion = submitted.take_value();
            auto future = completion->future.share();
            return Answer(
                Submission{std::move(completion), std::move(future), {}});
          },
          requested, snapshot_identity, shared_results, result_checkpoints,
          dependency_cache, plan_owner, atom_outcomes,
          options.maximum_parallelism ? options.maximum_parallelism
                                      : pool->ranges().workers());
      if (!result.ok())
        return fail(result.status());
      auto completed = result.take_value();
      const auto peaks = payload_observation->peaks();
      completed.diagnostics.peak_live_bytes = peaks.first;
      completed.diagnostics.planned_peak_bytes = peaks.second;
      return Result<ExecutionResult>(std::move(completed));
    }
    return fail(Status{ErrorCode::InvalidArgument,
                       "fragment execution requires a Result plan"});
  }

 private:
  /** @brief Submits one ready Result stage through bounded context admission.
   * Readiness is signalled after callback retirement.
   */
  template <class T>
  static Result<std::shared_ptr<StageCompletion<T>>> submit_result_stage(
      ThreadPool* pool, WaitingAdmission* admission,
      std::function<Result<T>()> work,
      const ResourceBudget* resources = nullptr) try {
    using Completion = StageCompletion<T>;
    using Answer = Result<std::shared_ptr<Completion>>;
    ResourceLease lease;
    if (resources) {
      auto capacity =
          ResourceCapacity::host(sizeof(Completion), sizeof(Completion));
      capacity[ResourceKind::Queue] = 1;
      capacity[ResourceKind::Entries] = 1;
      auto admitted = resources->reserve(capacity);
      if (!admitted.ok()) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_before_scheduler_failure(
            execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
        return Answer(admitted.status());
      }
      lease = admitted.take_value();
    }
    auto completion = std::make_shared<Completion>();
    completion->lease = std::move(lease);
    completion->work = std::move(work);
    auto retirement =
        resources
            ? std::allocate_shared<StageRetirement<T>>(
                  ResourceAllocator<StageRetirement<T>>(*resources), completion)
            : std::make_shared<StageRetirement<T>>(completion);
    auto slot = admission->try_acquire();
    if (!slot) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_before_scheduler_failure(
          execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
      return Answer(Status::failure(ErrorCode::ResourceExhausted,
                                    "dependency waiting queue exhausted"));
    }
    if (resources) {
      auto issued = resources->consume({0, 0, 0, 1});
      if (!issued.ok()) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_before_scheduler_failure(
            execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
        return Answer(issued);
      }
    }
    const auto payload_capture =
        resources
            ? execution_internal::ResourcePayloadScope::capture(*resources)
            : execution_internal::PayloadCapture{};
    QueuedCallback callback{
        [completion, retirement, resources, payload_capture] {
          // The callable's owners retire before the completion notification.
          auto work = std::move(completion->work);
          try {
            ErrorCode metadata_failure = ErrorCode::Ok;
            std::optional<ResourceAllocationScope> scope;
            std::optional<execution_internal::ResourcePayloadScope> payload;
            if (resources) {
              scope.emplace(*resources, &metadata_failure);
              payload.emplace(*resources, payload_capture);
            }
            completion->result = work();
            if (metadata_failure != ErrorCode::Ok &&
                completion->result.status().detail.origin !=
                    FailureOrigin::Protocol &&
                completion->result.status().code !=
                    ErrorCode::ResourceExhausted &&
                completion->result.status().code != ErrorCode::Cancelled &&
                completion->result.status().code != ErrorCode::Stale)
              completion->result = Result<T>(
                  Status{metadata_failure,
                         "dependency stage metadata allocation failed",
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Unspecified}});
          } catch (const std::bad_alloc&) {
            completion->result = Result<T>(
                scheduler_exception_status(ErrorCode::ResourceExhausted));
          } catch (const std::exception& error) {
            completion->result = Result<T>(scheduler_exception_status(
                ErrorCode::OperationFailed, error.what()));
          } catch (...) {
            completion->result = Result<T>(
                scheduler_exception_status(ErrorCode::OperationFailed));
          }
        },
        std::move(*slot), [retirement] { retirement->finish(); },
        completion->lease};
    if (!pool->submit(std::move(callback))) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_before_scheduler_failure(
          execution_testing::SchedulerFailurePoint::CallbackSubmitRejected);
#endif
      return Answer(Status::failure(ErrorCode::ResourceExhausted,
                                    "dependency callback queue stopped"));
    }
    return Answer(std::move(completion));
  } catch (const std::bad_alloc&) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_before_scheduler_failure(
        execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
    return Result<std::shared_ptr<StageCompletion<T>>>(
        scheduler_exception_status(ErrorCode::ResourceExhausted));
  } catch (const std::exception& error) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_before_scheduler_failure(
        execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
    return Result<std::shared_ptr<StageCompletion<T>>>(
        scheduler_exception_status(ErrorCode::Internal, error.what()));
  } catch (...) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_before_scheduler_failure(
        execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
    return Result<std::shared_ptr<StageCompletion<T>>>(
        scheduler_exception_status(ErrorCode::Internal));
  }
};

}  // namespace

/**
 * @brief Implements fixed local execution-resource construction.
 * @copydetails ExecutionContext::ExecutionContext
 */
ExecutionContext::ExecutionContext(
    std::shared_ptr<OperationRegistry> operations,
    ExecutionContextConfig config)
    : impl_(std::make_unique<Impl>(std::move(operations), config)) {
  impl_->demands->context = this;
}

/**
 * @brief Implements exact local worker/resource teardown.
 * @copydetails ExecutionContext::~ExecutionContext
 */
ExecutionContext::~ExecutionContext() noexcept {
  if (impl_)
    impl_->shared_results.shutdown();
  if (impl_ && impl_->demands)
    impl_->demands->close();
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

/** @brief Returns the plan pinned by this frozen state.
 * @return A borrowed plan reference, or a static empty plan for a default
 * object. The reference remains valid while this object retains its state.
 */
const ExecutionPlan& FrozenExecution::plan() const noexcept {
  static const ExecutionPlan empty;
  return state_ ? state_->plan : empty;
}
Result<FrozenExecution> FrozenExecution::for_region(
    const std::string& output, const Region& region) const {
  if (!valid())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid frozen execution"));
  auto tile = state_->plan.tile_plan(output, region);
  if (!tile.ok())
    return Result<FrozenExecution>(tile.status());
  auto state = std::allocate_shared<State>(
      ResourceAllocator<State>(state_->resources), state_->resources);
  state->plan = tile.take_value();
  state->bindings = state_->bindings;
  state->operations = state_->operations;
  state->execution_identity = state_->execution_identity;
  FrozenExecution result;
  result.state_ = std::move(state);
  return Result<FrozenExecution>(std::move(result));
}

namespace {
Result<std::string> frozen_identity() {
  static std::atomic<std::uint64_t> sequence{1};
  auto next = sequence.load();
  do {
    if (next == UINT64_MAX)
      return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
  } while (!sequence.compare_exchange_weak(next, next + 1));
  return Result<std::string>("frozen-" + std::to_string(next));
}
struct DemandKey {
  std::string value;
  std::uint64_t entries;
};
Result<DemandKey> demand_key(const DemandQuery& query,
                             const ExecutionPlan& plan, std::uint64_t maximum) {
  content_internal::Sha256 hash;
  hash.text("photospider.demand-query.v1");
  hash.integer(query.size());
  std::uint64_t entries = 1;
  if (query.size() > maximum || !maximum)
    return Result<DemandKey>(Status{ErrorCode::ResourceExhausted, {}});
  for (const auto& item : query) {
    if (item.first.empty() || item.first.size() > 1024 ||
        !item.second.valid() || !plan.outputs().count(item.first))
      return Result<DemandKey>(Status{ErrorCode::InvalidArgument,
                                      "unknown or invalid demand query"});
    const auto weight = 1 + item.second.boxes().size();
    if (weight > maximum || entries > maximum - weight)
      return Result<DemandKey>(
          Status{ErrorCode::ResourceExhausted, "demand query metadata limit"});
    entries += weight;
    hash.text(item.first);
    hash.integer(item.second.shape().size());
    for (const auto n : item.second.shape())
      hash.integer(n);
    hash.integer(item.second.boxes().size());
    for (const auto& box : item.second.boxes())
      for (const auto& d : box.dimensions()) {
        hash.integer(d.offset);
        hash.integer(d.extent);
      }
  }
  return Result<DemandKey>(DemandKey{hash.finish(), entries});
}
Result<DemandQuery> close_color_demands(const DemandQuery& query,
                                        const ExecutionPlan& plan,
                                        const FootprintLimits& limits) {
  auto checked = demand_key(query, plan, limits.maximum_boxes);
  if (!checked.ok())
    return Result<DemandQuery>(checked.status());
  DemandQuery result;
  for (const auto& item : query) {
    const auto found = plan.outputs().find(item.first);
    if (found == plan.outputs().end())
      return Result<DemandQuery>(
          Status{ErrorCode::InvalidArgument, "unknown color demand output"});
    const auto& step = plan.steps()[found->second];
    auto closed =
        step.output_result_schema && !step.output_result_schema->tensors.empty()
            ? step.output_result_schema->tensors[0].close_samples(item.second,
                                                                  limits)
            : input_internal::color_output_samples(
                  {step.output_descriptor, step.output_facets}, item.second,
                  limits);
    if (!closed.ok())
      return Result<DemandQuery>(closed.status());
    result.emplace(item.first, closed.take_value());
  }
  return Result<DemandQuery>(std::move(result));
}
template <class NamedValues>
Status unite_named(DemandQuery* target, const NamedValues& values,
                   const FootprintLimits& limits) {
  for (const auto& item : values) {
    auto found = target->find(std::string(item.first));
    auto next = found == target->end()
                    ? Footprint::from_regions(item.second.shape(),
                                              item.second.boxes(), limits)
                    : found->second.unite(item.second, limits);
    if (!next.ok())
      return next.status();
    target->insert_or_assign(std::string(item.first), next.take_value());
  }
  std::uint64_t entries = 0;
  for (const auto& item : *target) {
    const auto cost = 1 + item.second.boxes().size();
    if (cost > limits.maximum_boxes || entries > limits.maximum_boxes - cost)
      return Status{ErrorCode::ResourceExhausted, {}};
    entries += cost;
  }
  return Status::success();
}
Result<ResourceVector<SourceObservation>> changed_observations(
    const ExecutionBindings& before, const ExecutionBindings& after,
    const ResourceVector<SourceObservation>& observations,
    SnapshotAccessOptions access, const FootprintLimits& limits) {
  using Answer = Result<ResourceVector<SourceObservation>>;
  using Key = std::tuple<ResourceString, ResultSupportTarget, std::uint32_t>;
  std::map<Key, Footprint, std::less<Key>,
           ResourceAllocator<std::pair<const Key, Footprint>>>
      physical, changes;
  std::map<ResourceString, const ExecutionBinding*, ResourceStringLess,
           ResourceAllocator<
               std::pair<const ResourceString, const ExecutionBinding*>>>
      old, next;
  for (const auto& input : before.inputs)
    old.emplace(ResourceString(input.name.data(), input.name.size()), &input);
  for (const auto& input : after.inputs)
    next.emplace(ResourceString(input.name.data(), input.name.size()), &input);
  for (const auto& observation : observations) {
    Key key{observation.input, observation.target, observation.slot};
    auto found = physical.find(key);
    auto united = found == physical.end()
                      ? Result<Footprint>(observation.samples)
                      : found->second.unite(observation.samples, limits);
    if (!united.ok())
      return Answer(united.status());
    physical.insert_or_assign(key, united.take_value());
    if (physical.size() > limits.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
  }
  std::uint64_t remaining = access.maximum_samples;
  for (const auto& item : physical) {
    const auto& name = std::get<0>(item.first);
    const auto kind = std::get<1>(item.first);
    const auto slot = std::get<2>(item.first);
    const auto& samples = item.second;
    if (samples.empty())
      continue;
    const auto& a = *old.at(name);
    const auto& b = *next.at(name);
    auto count = samples.element_count();
    if (!count.ok())
      return Answer(count.status());
    if (count.value() > remaining)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "demand update sample limit"});
    if (!a.result.valid() || !b.result.valid() ||
        !a.result.schema().same_schema(b.result.schema()))
      return Answer(
          Status{ErrorCode::TypeMismatch, "Result source schema changed"});
    auto left = a.result.descriptor();
    auto right = b.result.descriptor();
    if (!left.ok())
      return Answer(left.status());
    if (!right.ok())
      return Answer(right.status());
    if (kind == ResultSupportTarget::Descriptor) {
      --remaining;
      bool dirty = left.value().sealed() != right.value().sealed();
      for (std::uint32_t i = 0; i < left.value().field_count(); ++i)
        dirty |= left.value().rows(i) != right.value().rows(i);
      for (std::uint32_t i = 0; i < left.value().tensor_count(); ++i)
        dirty |=
            left.value().tensor_coverage(i) != right.value().tensor_coverage(i);
      if (dirty)
        changes.emplace(item.first, samples);
      continue;
    }
    execution_internal::AccountedRegions boxes;
    auto status = samples.visit(
        [&](const auto& at) -> Status {
          if (!remaining)
            return Status{ErrorCode::ResourceExhausted, {}};
          --remaining;
          bool dirty = false;
          if (kind == ResultSupportTarget::Tensor) {
            if (slot >= a.result.schema().tensors.size())
              return Status{ErrorCode::TypeMismatch, {}};
            const auto width = Value::element_size(
                a.result.schema().tensors[slot].descriptor.element_type);
            std::uint8_t first[8]{}, second[8]{};
            if (!right.value().tensor_coverage(slot).contains(at)) {
              dirty = true;
            } else {
              auto read = a.result.read_tensor(left.value(), slot, at, first,
                                               width, access.cancellation);
              if (!read.ok())
                return read;
              read = b.result.read_tensor(right.value(), slot, at, second,
                                          width, access.cancellation);
              if (!read.ok())
                return read;
              dirty = std::memcmp(first, second, width) != 0;
            }
          } else {
            if (kind != ResultSupportTarget::Field ||
                slot >= a.result.schema().fields.size() || at.size() != 1)
              return Status{ErrorCode::TypeMismatch, {}};
            if (at[0] >= right.value().rows(slot)) {
              dirty = true;
            } else {
              auto first = a.result.prepare_read(left.value(), slot, at[0], 1);
              if (!first.ok())
                return first.status();
              auto second =
                  b.result.prepare_read(right.value(), slot, at[0], 1);
              if (!second.ok())
                return second.status();
              auto bytes = a.result.schema().row_bytes(slot);
              if (!bytes.ok())
                return bytes.status();
              auto first_bytes =
                  first.value().load(bytes.value(), access.cancellation);
              if (!first_bytes.ok())
                return first_bytes.status();
              auto second_bytes =
                  second.value().load(bytes.value(), access.cancellation);
              if (!second_bytes.ok())
                return second_bytes.status();
              dirty = std::memcmp(first_bytes.value()->bytes().data(),
                                  second_bytes.value()->bytes().data(),
                                  bytes.value()) != 0;
            }
          }
          if (dirty) {
            if (boxes.boxes.size() >= limits.maximum_boxes)
              return Status{ErrorCode::ResourceExhausted, {}};
            std::array<RegionDimension, 8> dimensions{};
            for (std::size_t i = 0; i < at.size(); ++i)
              dimensions[i] = {at[i], 1};
            auto admitted = boxes.append(dimensions.data(), at.size());
            if (!admitted.ok())
              return admitted;
          }
          return Status::success();
        },
        count.value(), access.cancellation);
    if (!status.ok())
      return Answer(status);
    auto dirty = Footprint::from_regions(samples.shape(), boxes.boxes, limits);
    if (!dirty.ok())
      return Answer(dirty.status());
    if (!dirty.value().empty())
      changes.emplace(item.first, dirty.take_value());
  }
  ResourceVector<SourceObservation> answer;
  for (const auto& observation : observations) {
    auto found =
        changes.find({observation.input, observation.target, observation.slot});
    if (found == changes.end())
      continue;
    auto dirty = found->second.intersect(observation.samples, limits);
    if (!dirty.ok())
      return Answer(dirty.status());
    if (!dirty.value().empty())
      answer.push_back({observation.input, observation.target, observation.slot,
                        observation.roles, dirty.take_value()});
  }
  return Answer(std::move(answer));
}

}  // namespace

Result<FrozenExecution> ExecutionContext::freeze(
    const ExecutionPlan& plan, ExecutionBindings bindings) const {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock().get() != impl_->operation_registry.get())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid stale or foreign plan"));
  if (!impl_->budget->resources())
    return Result<FrozenExecution>(
        Status{ErrorCode::InvalidArgument,
               "Result capture requires managed_resources"});
  auto validated = preflight_regional_bindings(plan, bindings, {},
                                               *impl_->budget->resources());
  if (!validated.ok())
    return Result<FrozenExecution>(validated.status());
  auto identity = frozen_identity();
  if (!identity.ok())
    return Result<FrozenExecution>(identity.status());
  const auto& root = *impl_->budget->resources();
  auto state = std::allocate_shared<FrozenExecution::State>(
      ResourceAllocator<FrozenExecution::State>(root), root);
  state->execution_identity = identity.take_value();
  state->plan = plan;
  state->bindings = std::move(bindings);
  state->operations = impl_->operation_registry;
  // The explicit owner keeps registry and input lifetimes independent of the
  // editable graph. Ordinary plan predicates are never changed in place.
  state->plan.current_check_ = [] { return true; };
  if (!plan.current())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "graph changed during freeze"));
  FrozenExecution frozen;
  frozen.state_ = std::move(state);
  return Result<FrozenExecution>(std::move(frozen));
}
Result<DemandResult> ExecutionContext::execute_fragments(
    const FrozenExecution& frozen, const DemandQuery& query,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  const auto captured = frozen;
  if (!impl_ || !captured.valid() || !captured.state_->plan.current() ||
      captured.state_->operations != impl_->operation_registry)
    return Result<DemandResult>(
        Status{ErrorCode::Stale, "invalid or foreign frozen demand"});
  const auto stop = [&] {
    if (options.dependencies.sets.cancellation.cancelled())
      return ErrorCode::Cancelled;
    return binding_stop(captured.state_->plan, cancellation);
  };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok &&
        status.detail.origin != FailureOrigin::Protocol)
      status = Status{code, {}};
    return Result<DemandResult>(std::move(status));
  };
  try {
    if (stop() != ErrorCode::Ok)
      return failure(Status{stop(), {}});
    auto combined = CancellationToken::combine(
        {cancellation, options.dependencies.sets.cancellation});
    if (!combined.ok())
      return failure(combined.status());
    auto key = demand_key(query, captured.state_->plan,
                          options.dependencies.sets.maximum_boxes);
    if (!key.ok())
      return failure(key.status());
    auto validated = preflight_regional_bindings(
        captured.state_->plan, captured.state_->bindings, cancellation,
        *impl_->budget->resources());
    if (!validated.ok())
      return failure(validated.status());
    DemandResult result;
    auto run = ExecutionRun::run_results(
        &impl_->cpu_pool, impl_->gpu_pool.get(), impl_->native_device,
        impl_->native_uploads, &impl_->waiting_admission, impl_->budget,
        impl_->operation_registry, captured.state_->plan,
        captured.state_->plan.current_check_, validated.take_value(),
        combined.value(), options, &query, captured.state_->execution_identity,
        impl_->cache.get(), &impl_->shared_results, false,
        impl_->result_checkpoints.get(),
        std::shared_ptr<const ExecutionPlan>(captured.state_,
                                             &captured.state_->plan));
    if (!run.ok())
      return failure(run.status());
    auto completed = run.take_value();
    result.diagnostics = std::move(completed.diagnostics);
    result.dependencies = std::move(completed.dependencies);
    result.results = std::move(completed.results);
    if (stop() != ErrorCode::Ok)
      return failure(Status{stop(), {}});
    return Result<DemandResult>(std::move(result));
  } catch (const std::bad_alloc&) {
    return failure(Status{ErrorCode::ResourceExhausted,
                          {},
                          FailureReason::CapacityLimit,
                          {FailureOrigin::Resource, FailureScope::Run}});
  } catch (const std::exception& error) {
    return failure(Status{ErrorCode::OperationFailed,
                          error.what() ? error.what() : "",
                          FailureReason::HostException});
  } catch (...) {
    return failure(
        Status{ErrorCode::OperationFailed, {}, FailureReason::HostException});
  }
}
Result<DemandHandle> ExecutionContext::open_demand(const ExecutionPlan& plan,
                                                   ExecutionBindings bindings,
                                                   DemandConfig config) {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock() != impl_->operation_registry)
    return Result<DemandHandle>(
        Status{ErrorCode::Stale, "invalid or foreign demand plan"});
  if (!config.maximum_metadata_entries ||
      config.maximum_metadata_entries > 1048576)
    return Result<DemandHandle>(
        Status{ErrorCode::InvalidArgument, "invalid demand metadata limit"});
  auto frozen = freeze(plan, std::move(bindings));
  if (!frozen.ok())
    return Result<DemandHandle>(frozen.status());
  auto state = std::make_shared<DemandHandle::Impl>();
  state->owner = impl_->demands;
  state->config = config;
  state->bundle = std::make_shared<const FrozenExecution>(frozen.take_value());
  auto& owner = *impl_->demands;
  std::lock_guard<std::mutex> lock(owner.mutex);
  if (owner.closing)
    return Result<DemandHandle>(Status{ErrorCode::Cancelled, {}});
  for (auto i = owner.handles.begin(); i != owner.handles.end();) {
    // Do not acquire a temporary last owner under the publication lock:
    // input allocation deleters may reenter another demand in this context.
    if (i->second.handle.expired() || i->second.cancellation.cancelled())
      i = owner.handles.erase(i);
    else
      ++i;
  }
  if (owner.handles.size() >= owner.maximum_handles || owner.next == UINT64_MAX)
    return Result<DemandHandle>(
        Status{ErrorCode::ResourceExhausted, "demand handle limit"});
  owner.handles.emplace(owner.next++,
                        execution_internal::DemandCoordinator::HandleEntry{
                            state, state->cancellation.token()});
  return Result<DemandHandle>(DemandHandle(std::move(state)));
}
DemandHandle::DemandHandle(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Result<DemandResult> DemandHandle::request(
    const DemandQuery& query, const CancellationToken& cancellation,
    const ExecutionOptions& options) const {
  if (!impl_)
    return Result<DemandResult>(
        Status{ErrorCode::Stale, "invalid demand handle"});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<DemandResult>(
        Status{ErrorCode::Cancelled, "demand context retired"});
  if (cancellation.cancelled() ||
      options.dependencies.sets.cancellation.cancelled())
    return Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
  auto combined = CancellationToken::combine(
      {cancellation, options.dependencies.sets.cancellation});
  if (!combined.ok())
    return Result<DemandResult>(combined.status());
  auto begun = owner->acquire(impl_, combined.value());
  if (!begun.ok())
    return Result<DemandResult>(begun.status());
  auto lease = begun.take_value();
  const auto failure = [&](Status status) {
    return Result<DemandResult>(lease->stop(std::move(status)));
  };
  auto normalized = close_color_demands(query, lease->bundle->plan(),
                                        options.dependencies.sets);
  if (!normalized.ok())
    return failure(normalized.status());
  DemandQuery original = normalized.take_value();
  auto key = demand_key(original, lease->bundle->plan(),
                        std::min(impl_->config.maximum_metadata_entries,
                                 options.dependencies.sets.maximum_boxes));
  if (!key.ok())
    return failure(key.status());
  auto state = std::allocate_shared<FrozenExecution::State>(
      ResourceAllocator<FrozenExecution::State>(
          lease->bundle->state_->resources),
      *lease->bundle->state_);
  state->plan.current_check_ = [handle = impl_,
                                generation = lease->generation] {
    return handle->generation.load(std::memory_order_acquire) == generation;
  };
  FrozenExecution run;
  run.state_ = std::move(state);
  auto executed = owner->context->execute_fragments(
      run, original, lease->cancellation, options);
  if (!executed.ok())
    return failure(executed.status());
  auto result = executed.take_value();
  result.generation = lease->generation;
  auto publication = std::make_shared<Impl::Publication>();
  publication->query = std::move(original);
  publication->dependencies = result.dependencies;
  auto limits = options.dependencies.sets;
  limits.cancellation = lease->cancellation;
  for (const auto& item : publication->query) {
    auto empty = Footprint::none(item.second.shape(), limits);
    if (!empty.ok())
      return failure(empty.status());
    publication->dirty.emplace(item.first, empty.take_value());
  }
  const auto base = checked_add(
      key.value().entries, execution_internal::DependencyRecords::metadata_size(
                               result.dependencies));
  if (!base.ok())
    return failure(base.status());
  auto weight = checked_add(base.value(), publication->dirty.size());
  if (!weight.ok())
    return failure(weight.status());
  publication->weight = weight.value();
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return Result<DemandResult>(status);
  if (impl_->revision == UINT64_MAX)
    return Result<DemandResult>(Status{ErrorCode::ResourceExhausted, {}});
  auto old = impl_->publications.find(key.value().value);
  if (old != impl_->publications.end() &&
      old->second->query != publication->query)
    return Result<DemandResult>(
        Status{ErrorCode::Internal, "query identity collision"});
  const auto remainder =
      impl_->metadata_entries -
      (old == impl_->publications.end() ? 0 : old->second->weight);
  if (publication->weight > impl_->config.maximum_metadata_entries ||
      remainder > impl_->config.maximum_metadata_entries - publication->weight)
    return Result<DemandResult>(
        Status{ErrorCode::ResourceExhausted, "retained demand metadata limit"});
  impl_->publications.insert_or_assign(key.value().value, publication);
  impl_->metadata_entries = remainder + publication->weight;
  ++impl_->revision;
  return Result<DemandResult>(std::move(result));
}
Result<FrozenExecution> DemandHandle::freeze() const {
  if (!impl_)
    return Result<FrozenExecution>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<FrozenExecution>(Status{ErrorCode::Cancelled, {}});
  std::lock_guard<std::mutex> lock(owner->mutex);
  if (owner->closing || impl_->cancellation.token().cancelled())
    return Result<FrozenExecution>(Status{ErrorCode::Cancelled, {}});
  return Result<FrozenExecution>(*impl_->bundle);
}
Result<std::uint64_t> DemandHandle::generation() const {
  if (!impl_)
    return Result<std::uint64_t>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<std::uint64_t>(Status{ErrorCode::Cancelled, {}});
  std::lock_guard<std::mutex> lock(owner->mutex);
  if (owner->closing || impl_->cancellation.token().cancelled())
    return Result<std::uint64_t>(Status{ErrorCode::Cancelled, {}});
  return Result<std::uint64_t>(impl_->generation.load());
}
Status DemandHandle::release(const DemandQuery& query) const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  auto owner = impl_->owner.lock();
  if (!owner)
    return Status{ErrorCode::Cancelled, {}};
  auto begun = owner->acquire(impl_, {});
  if (!begun.ok())
    return begun.status();
  auto lease = begun.take_value();
  FootprintLimits limits;
  limits.maximum_boxes = impl_->config.maximum_metadata_entries;
  auto normalized = close_color_demands(query, lease->bundle->plan(), limits);
  if (!normalized.ok())
    return lease->stop(normalized.status());
  auto key = demand_key(normalized.value(), lease->bundle->plan(),
                        impl_->config.maximum_metadata_entries);
  if (!key.ok())
    return lease->stop(key.status());
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return status;
  auto found = impl_->publications.find(key.value().value);
  if (found == impl_->publications.end() ||
      found->second->query != normalized.value())
    return Status{ErrorCode::NotFound, {}};
  if (impl_->revision == UINT64_MAX)
    return Status{ErrorCode::ResourceExhausted, {}};
  impl_->metadata_entries -= found->second->weight;
  impl_->publications.erase(found);
  ++impl_->revision;
  return Status::success();
}
bool DemandHandle::cancel() const noexcept {
  if (!impl_)
    return false;
  const bool first = impl_->cancellation.cancel();
  std::shared_ptr<const FrozenExecution> retired;
  if (auto owner = impl_->owner.lock()) {
    std::lock_guard<std::mutex> lock(owner->mutex);
    impl_->publications.clear();
    impl_->metadata_entries = 0;
    retired = std::move(impl_->bundle);
  }
  return first;
}
Result<DemandUpdate> DemandHandle::replace_bindings(
    ExecutionBindings bindings, const SnapshotAccessOptions& options) const
    try {
  if (!impl_)
    return Result<DemandUpdate>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<DemandUpdate>(Status{ErrorCode::Cancelled, {}});
  auto root = owner->context->resource_budget();
  std::optional<ResourceAllocationScope> root_scope;
  if (root.ok())
    root_scope.emplace(root.value());
  auto begun = owner->acquire(impl_, options.cancellation);
  if (!begun.ok())
    return Result<DemandUpdate>(begun.status());
  auto lease = begun.take_value();
  const auto failure = [&](Status status) {
    return Result<DemandUpdate>(lease->stop(std::move(status)));
  };
  std::map<std::string, std::shared_ptr<const Impl::Publication>> publications;
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    auto status = lease->stop();
    if (!status.ok())
      return Result<DemandUpdate>(status);
    publications = impl_->publications;
    lease->revision = impl_->revision;
  }
  auto frozen =
      owner->context->freeze(lease->bundle->state_->plan, std::move(bindings));
  if (!frozen.ok())
    return failure(frozen.status());
  auto next = std::make_shared<const FrozenExecution>(frozen.take_value());
  FootprintLimits limits{impl_->config.maximum_metadata_entries, 1048576,
                         lease->cancellation};
  ResourceVector<SourceObservation> support;
  for (const auto& item : publications) {
    auto source = item.second->dependencies.source_observations(limits);
    if (!source.ok())
      return failure(source.status());
    if (source.value().size() > limits.maximum_boxes - support.size())
      return failure(Status{ErrorCode::ResourceExhausted, {}});
    support.insert(support.end(), source.value().begin(), source.value().end());
  }
  auto access = options;
  access.cancellation = lease->cancellation;
  auto changes =
      changed_observations(lease->bundle->state_->bindings,
                           next->state_->bindings, support, access, limits);
  if (!changes.ok())
    return failure(changes.status());
  DemandUpdate update;
  if (lease->generation == UINT64_MAX)
    return failure(Status{ErrorCode::ResourceExhausted, {}});
  update.generation = lease->generation + 1;
  std::uint64_t entries = 0;
  for (auto& item : publications) {
    auto publication = std::make_shared<Impl::Publication>(*item.second);
    for (const auto& change : changes.value()) {
      auto dirty = publication->dependencies.potential_dirty(
          std::string(change.input), change.samples, change.roles, limits,
          change.target, change.slot);
      if (!dirty.ok())
        return failure(dirty.status());
      auto status = unite_named(&publication->dirty, dirty.value(), limits);
      if (!status.ok())
        return failure(status);
    }
    auto key = demand_key(publication->query, next->state_->plan,
                          impl_->config.maximum_metadata_entries);
    if (!key.ok())
      return failure(key.status());
    auto weight =
        checked_add(key.value().entries,
                    execution_internal::DependencyRecords::metadata_size(
                        publication->dependencies));
    if (!weight.ok())
      return failure(weight.status());
    std::uint64_t cost = weight.value();
    for (const auto& dirty : publication->dirty) {
      weight = checked_add(cost, 1 + dirty.second.boxes().size());
      if (!weight.ok())
        return failure(weight.status());
      cost = weight.value();
    }
    publication->weight = cost;
    if (cost > impl_->config.maximum_metadata_entries ||
        entries > impl_->config.maximum_metadata_entries - cost)
      return failure(Status{ErrorCode::ResourceExhausted,
                            "updated demand metadata limit"});
    entries += cost;
    auto status = unite_named(&update.coverage, publication->query, limits);
    if (!status.ok())
      return failure(status);
    status = unite_named(&update.potential_dirty, publication->dirty, limits);
    if (!status.ok())
      return failure(status);
    item.second = std::move(publication);
  }
  std::shared_ptr<const FrozenExecution> retired;
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return Result<DemandUpdate>(status);
  if (impl_->revision != lease->revision)
    return Result<DemandUpdate>(Status{
        ErrorCode::Stale, "demand publications changed during replacement"});
  if (impl_->revision == UINT64_MAX)
    return Result<DemandUpdate>(Status{ErrorCode::ResourceExhausted, {}});
  retired = std::move(impl_->bundle);
  impl_->bundle = std::move(next);
  impl_->publications.swap(publications);
  impl_->metadata_entries = entries;
  ++impl_->revision;
  impl_->generation.store(update.generation, std::memory_order_release);
  return Result<DemandUpdate>(std::move(update));
} catch (const std::bad_alloc&) {
  return Result<DemandUpdate>(Status{ErrorCode::ResourceExhausted, {}});
}

Result<ExecutionResult> ExecutionContext::execute(
    const FrozenExecution& frozen, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  const auto captured = frozen;
  if (!captured.valid())
    return Result<ExecutionResult>(Status{ErrorCode::Stale, {}});
  if (captured.state_->plan.structured_network())
    return execute_regions(captured.state_->plan, captured.state_->bindings,
                           cancellation, options,
                           captured.state_->execution_identity, false, nullptr,
                           std::shared_ptr<const ExecutionPlan>(
                               captured.state_, &captured.state_->plan));
  return execute(captured.state_->plan, captured.state_->bindings, cancellation,
                 options);
}
Result<ExecutionResult> ExecutionContext::execute(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  return execute_regions(plan, std::move(bindings), cancellation, options);
}

Result<ExecutionResult> ExecutionContext::execute_atoms(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const DemandQuery& requested, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  auto root = resource_budget();
  if (!root.ok())
    return Result<ExecutionResult>(root.status());
  if (!plan.dependency_network() || !plan.structured_network())
    return Result<ExecutionResult>(
        Status{ErrorCode::InvalidArgument,
               "atom execution requires a Result dependency network"});
  ResourceAllocationScope scope(root.value());
  return execute_regions(plan, std::move(bindings), cancellation, options, {},
                         true, &requested);
}
Result<ExecutionResult> ExecutionContext::execute_regions(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options,
    const std::string& snapshot_identity, bool atom_outcomes,
    const DemandQuery* requested,
    std::shared_ptr<const ExecutionPlan> plan_owner) {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock() != impl_->operation_registry)
    return Result<ExecutionResult>(
        Status{ErrorCode::Stale, "Result plan is invalid, stale or foreign"});
  const auto stop = [&] { return binding_stop(plan, cancellation); };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok &&
        status.detail.origin != FailureOrigin::Protocol)
      status =
          Status{code,
                 {},
                 code == ErrorCode::Cancelled ? FailureReason::Cancelled
                                              : FailureReason::StaleVersion,
                 {FailureOrigin::Cancellation, FailureScope::Run}};
    return Result<ExecutionResult>(std::move(status));
  };
  if (stop() != ErrorCode::Ok)
    return failure(Status{stop(), {}});
  try {
    auto validated = preflight_regional_bindings(plan, bindings, cancellation,
                                                 *impl_->budget->resources());
    if (!validated.ok())
      return failure(validated.status());
    return ExecutionRun::run_results(
        &impl_->cpu_pool, impl_->gpu_pool.get(), impl_->native_device,
        impl_->native_uploads, &impl_->waiting_admission, impl_->budget,
        impl_->operation_registry, plan, plan.current_check_,
        validated.take_value(), cancellation, options, requested,
        snapshot_identity, impl_->cache.get(), &impl_->shared_results,
        atom_outcomes, impl_->result_checkpoints.get(), std::move(plan_owner));
  } catch (const std::bad_alloc&) {
    return failure(Status{ErrorCode::ResourceExhausted, {}});
  } catch (const std::exception& error) {
    return failure(
        Status{ErrorCode::OperationFailed, error.what() ? error.what() : ""});
  } catch (...) {
    return failure(Status{ErrorCode::OperationFailed, {}});
  }
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
