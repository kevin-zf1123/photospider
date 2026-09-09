#include "photospider/execution/execution.hpp"

#include <algorithm>
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
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "execution/disk_cache.hpp"
#include "execution/memory_budget.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_identity.hpp"

#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps {
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

/**
 * @brief ExecutionContext-wide nonblocking waiting-callback admission owner.
 *
 * Every successful acquisition represents one callback accepted by a CPU or
 * GPU queue but not yet popped by a worker. Move-only leases release exactly
 * once on worker start, enqueue rollback, exception unwinding, or queue drop.
 */
class WaitingAdmission final {
 public:
  /**
   * @brief Move-only exact-release token for one waiting callback.
   *
   * @note A default or moved-from token owns no admission and is safe to drop.
   */
  class Lease final {
   public:
    /**
     * @brief Constructs an empty non-owning token.
     * @throws Nothing.
     */
    Lease() noexcept = default;

    /**
     * @brief Transfers one waiting-callback admission.
     * @param other Source token left empty.
     * @throws Nothing.
     */
    Lease(Lease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}

    /**
     * @brief Releases current ownership before accepting another token.
     * @param other Source token left empty.
     * @return This token.
     * @throws Nothing.
     */
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
      }
      return *this;
    }

    /**
     * @brief Releases the owned waiting admission exactly once.
     * @throws Nothing under the ExecutionContext lifetime contract.
     */
    ~Lease() noexcept { release(); }

    /**
     * @brief Forbids duplicating exact-release ownership.
     * @param other Source token that cannot be copied.
     * @throws Nothing; the operation is deleted.
     */
    Lease(const Lease& other) = delete;
    /**
     * @brief Forbids assigning duplicate exact-release ownership.
     * @param other Source token that cannot be assigned.
     * @return No value; the operation is deleted.
     * @throws Nothing; the operation is deleted.
     */
    Lease& operator=(const Lease& other) = delete;

    /**
     * @brief Releases ownership early when a worker begins the callback.
     * @return No value.
     * @throws Nothing under the ExecutionContext lifetime contract.
     * @note Repeated calls are idempotent.
     */
    void release() noexcept {
      if (owner_) {
        owner_->release();
        owner_ = nullptr;
      }
    }

   private:
    friend class WaitingAdmission;

    /**
     * @brief Constructs one owning token.
     * @param owner Context-wide admission owner.
     * @throws Nothing.
     */
    explicit Lease(WaitingAdmission* owner) noexcept : owner_(owner) {}

    /** @brief Admission owner receiving release, or null. */
    WaitingAdmission* owner_ = nullptr;
  };

  /**
   * @brief Constructs a positive shared waiting-callback limit.
   * @param capacity Maximum aggregate callbacks waiting across all lanes.
   * @throws std::invalid_argument If capacity is zero.
   */
  explicit WaitingAdmission(std::uint32_t capacity) : capacity_(capacity) {
    if (capacity == 0U) {
      throw std::invalid_argument("maximum queued tasks must be positive");
    }
  }

  /**
   * @brief Attempts immediate aggregate waiting admission.
   * @return Owning token, or empty when the context-wide limit is full.
   * @throws std::system_error If mutex acquisition fails.
   * @note The comparison precedes increment, preventing unsigned overflow.
   */
  [[nodiscard]] std::optional<Lease> try_acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting_ >= capacity_) {
      return std::nullopt;
    }
    ++waiting_;
    return Lease(this);
  }

 private:
  /**
   * @brief Releases one previously acquired waiting admission.
   * @return No value.
   * @throws Nothing under the move-only token invariant.
   */
  void release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting_ != 0U) {
      --waiting_;
    }
  }

  /** @brief Serializes aggregate waiting count. */
  std::mutex mutex_;
  /** @brief Fixed positive aggregate limit. */
  const std::size_t capacity_;
  /** @brief Callbacks currently queued but not started. */
  std::size_t waiting_ = 0U;
};

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
  ThreadPool(std::uint32_t worker_count, Backend backend) : backend_(backend) {
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
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    const execution_testing::CallbackSubmitAction test_action =
        execution_testing::callback_submit_action(backend_);
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
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_callback_queued(backend_);
#endif
    ready_.notify_one();
    return true;
  }

 private:
  /**
   * @brief Runs callbacks until stop is requested.
   * @throws Nothing across the thread boundary.
   * @note Submitted callbacks are required to fence their own exceptions.
   */
  void worker_loop() noexcept {
    for (;;) {
      QueuedCallback callback;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !callbacks_.empty(); });
        if (stopping_) {
          return;
        }
        callback = std::move(callbacks_.front());
        callbacks_.pop_front();
      }
      callback.admission.release();
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
      std::lock_guard<std::mutex> lock(mutex_);
      if (!stopping_) {
        stopping_ = true;
        callbacks_.clear();
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
  /** @brief Deterministic FIFO governed by shared waiting admission. */
  std::deque<QueuedCallback> callbacks_;
  /** @brief Owned fixed worker set. */
  std::vector<std::thread> workers_;
  /** @brief Exact local backend lane served by this pool. */
  [[maybe_unused]] const Backend backend_;
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
 * @brief Updates one FNV-1a state with exact bytes.
 * @param state Current digest state.
 * @param data Byte pointer valid for `size` bytes.
 * @param size Byte count.
 * @return Updated non-cryptographic state.
 * @throws Nothing.
 */
std::uint64_t fnv_update(std::uint64_t state, const void* data,
                         std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    state ^= bytes[index];
    state *= 1099511628211ULL;
  }
  return state;
}

/**
 * @brief Appends one integer in fixed little-endian order to an FNV state.
 * @param state Current digest state.
 * @param value Canonical unsigned value.
 * @return Updated non-cryptographic state.
 * @throws Nothing.
 * @note Host endianness does not affect the result.
 */
std::uint64_t fnv_integer(std::uint64_t state, std::uint64_t value) noexcept {
  std::uint8_t encoded[8]{};
  for (std::size_t index = 0U; index < sizeof(encoded); ++index) {
    encoded[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
  }
  return fnv_update(state, encoded, sizeof(encoded));
}

/**
 * @brief Appends one length-framed byte range to an FNV state.
 * @param state Current digest state.
 * @param data Byte pointer valid for `size` bytes.
 * @param size Exact byte count.
 * @return Updated non-cryptographic state.
 * @throws Nothing.
 * @note Framing prevents concatenation ambiguity.
 */
std::uint64_t fnv_bytes(std::uint64_t state, const void* data,
                        std::size_t size) noexcept {
  state = fnv_integer(state, size);
  return fnv_update(state, data, size);
}

/**
 * @brief Formats one FNV state as stable lowercase hexadecimal text.
 * @param state Complete non-cryptographic state.
 * @return Sixteen-character digest.
 * @throws std::bad_alloc If stream storage allocation fails.
 */
std::string format_digest(std::uint64_t state) {
  std::ostringstream stream;
  stream << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
         << state;
  return stream.str();
}

/**
 * @brief Computes a deterministic non-security identity for named Values.
 * @param values Sorted named immutable Values.
 * @return Reproducibility digest over complete Value semantic/storage facts.
 * @throws std::bad_alloc If formatting allocation fails.
 * @note The digest is unsuitable for authentication, signing, or admission.
 */
std::string result_digest(const std::map<std::string, Value>& values) {
  std::uint64_t state = 14695981039346656037ULL;
  constexpr char kDomain[] = "photospider.result-digest.v2";
  state = fnv_bytes(state, kDomain, sizeof(kDomain) - 1U);
  for (const auto& entry : values) {
    state = fnv_bytes(state, entry.first.data(), entry.first.size());
    const Value& value = entry.second;
    state = fnv_integer(
        state, static_cast<std::uint32_t>(value.descriptor().element_type));
    state = fnv_integer(state, value.descriptor().shape.size());
    for (std::uint64_t extent : value.descriptor().shape) {
      state = fnv_integer(state, extent);
    }
    for (const RegionDimension& dimension : value.region().dimensions()) {
      state = fnv_integer(state, dimension.offset);
      state = fnv_integer(state, dimension.extent);
    }
    state = fnv_integer(state, value.layout().byte_offset);
    for (std::size_t axis = 0; axis < value.descriptor().shape.size(); ++axis)
      state = fnv_integer(state, value.layout().origin.empty()
                                     ? 0
                                     : value.layout().origin[axis]);
    for (std::int64_t stride : value.layout().byte_strides) {
      std::uint64_t stride_bits = 0U;
      std::memcpy(&stride_bits, &stride, sizeof(stride_bits));
      state = fnv_integer(state, stride_bits);
    }
    state = fnv_integer(state, value.facets().size());
    for (const ValueFacet& facet : value.facets()) {
      state = fnv_bytes(state, facet.key.data(), facet.key.size());
      state = fnv_integer(state, facet.version);
      state = fnv_bytes(state, facet.payload.data(), facet.payload.size());
    }
    state = fnv_bytes(state, value.bytes().data(), value.bytes().size());
  }
  return format_digest(state);
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

/** @brief Tests exact logical coverage, without comparing allocation layout. */
bool same_region(const Region& a, const Region& b) {
  if (a.rank() != b.rank())
    return false;
  for (std::size_t i = 0; i < a.rank(); ++i)
    if (a.dimensions()[i].offset != b.dimensions()[i].offset ||
        a.dimensions()[i].extent != b.dimensions()[i].extent)
      return false;
  return true;
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
                   const Region& available) {
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
 * @return Deep-copied Value preserving descriptor, Region, layout, and facets.
 * @throws std::bad_alloc If transfer allocation fails.
 * @note Backend residency is tracked by the owning ExecutionRun; Value itself
 * remains backend-neutral and exposes no native device handle.
 */
Result<Value> transfer_value(const Value& source,
                             const BufferAllocator& allocator,
                             bool compact = false) {
  if (compact) {
    auto allocated =
        MutableValue::allocate(source.descriptor(), source.region(), allocator);
    if (!allocated.ok())
      return Result<Value>(allocated.status());
    auto output = allocated.take_value();
    auto status = copy_region(ValueView(source), &output, source.region());
    if (!status.ok())
      return Result<Value>(status);
    return std::move(output).publish(source.facets());
  }
  auto allocated = allocator.allocate(source.bytes().size());
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto buffer = allocated.take_value();
  std::memcpy(buffer.data(), source.bytes().data(), source.bytes().size());
  return Value::from_storage(source.descriptor(), source.region(),
                             source.layout(), std::move(buffer).freeze(),
                             source.facets());
}

/**
 * @brief Validates that one published Value covers a planned logical demand.
 * @param value Complete immutable producer Value.
 * @param demand Validated plan demand for one consumer input.
 * @return Success or typed rank/containment failure.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Validation occurs before transfer/accounting/callback entry.
 */
Status validate_input_demand(const Value& value, const Region& demand) {
  const Status value_status = value.region().validate(value.descriptor().shape);
  const Status demand_status = demand.validate(value.descriptor().shape);
  if (!value_status.ok() || !demand_status.ok() || demand.empty()) {
    return Status::failure(
        ErrorCode::TypeMismatch,
        "execution input Region or planned demand is invalid");
  }
  for (std::size_t axis = 0U; axis < demand.rank(); ++axis) {
    const RegionDimension& available = value.region().dimensions()[axis];
    const RegionDimension& requested = demand.dimensions()[axis];
    const std::uint64_t available_end = available.offset + available.extent;
    const std::uint64_t requested_end = requested.offset + requested.extent;
    if (requested.offset < available.offset || requested_end > available_end) {
      return Status::failure(
          ErrorCode::TypeMismatch,
          "execution input Value does not cover its planned demand");
    }
  }
  return Status::success();
}

}  // namespace

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
        gpu_available(requested.gpu_enabled),
        maximum_waiting_callbacks(requested.maximum_queued_tasks),
        operation_registry(std::move(operations)),
        budget(std::make_shared<MemoryBudget>(requested.maximum_live_bytes)),
        waiting_admission(maximum_waiting_callbacks),
        cpu_pool(cpu_worker_count, Backend::Cpu) {
    if (!operation_registry || !operation_registry->frozen()) {
      throw std::invalid_argument(
          "ExecutionContext requires a frozen operation registry");
    }
    if (requested.result_cache_bytes > requested.maximum_live_bytes)
      throw std::invalid_argument("cache limit exceeds execution budget");
    if (requested.disk_cache) {
      if (requested.result_cache_bytes == 0)
        throw std::invalid_argument(
            "disk cache requires positive result cache capacity");
      disk = std::make_unique<execution_internal::DiskCache>(
          *requested.disk_cache,
          operation_registry->persistent_cache_identity(), budget,
          requested.result_cache_bytes);
    }
    if (requested.result_cache_bytes != 0)
      cache = std::make_unique<execution_internal::ResultCache>(
          requested.result_cache_bytes, budget,
          std::min<std::uint32_t>(cpu_worker_count, 4),
          maximum_waiting_callbacks);
    if (gpu_available) {
      gpu_pool = std::make_unique<ThreadPool>(1U, Backend::Gpu);
    }
  }

  /** @brief Fixed resolved CPU worker count. */
  const std::uint32_t cpu_worker_count;
  /** @brief Fixed optional GPU-lane availability. */
  const bool gpu_available;
  /** @brief Context-wide maximum callbacks waiting across all lanes. */
  const std::uint32_t maximum_waiting_callbacks;
  /** @brief Frozen operation registry retained beyond all callbacks. */
  std::shared_ptr<OperationRegistry> operation_registry;
  /** @brief Shared exact modeled-byte capacity. */
  std::shared_ptr<MemoryBudget> budget;
  /** @brief Shared CPU/GPU waiting-callback admission owner. */
  WaitingAdmission waiting_admission;
  /** @brief Required fixed CPU callback pool. */
  ThreadPool cpu_pool;
  /** @brief Optional single local GPU callback lane. */
  std::unique_ptr<ThreadPool> gpu_pool;
  std::unique_ptr<execution_internal::DiskCache> disk;
  // Destroy coordinators before callback pools and their allocation budget.
  std::unique_ptr<execution_internal::ResultCache> cache;
};

namespace {

/** @brief Allocation-free stop selection after a successful plan entry check.
 */
ErrorCode binding_stop(const ExecutionPlan& plan,
                       const CancellationToken& cancellation) noexcept {
  if (cancellation.cancelled())
    return ErrorCode::Cancelled;
  return plan.current() ? ErrorCode::Ok : ErrorCode::Stale;
}

/** @brief Validates the complete name multiset then all Values and consumers.
 */
Result<std::vector<Value>> preflight_bindings(
    const ExecutionPlan& plan, const ExecutionBindings& bindings,
    const CancellationToken& cancellation) {
  if (bindings.inputs.size() > 4096) {
    return Result<std::vector<Value>>(
        Status::failure(ErrorCode::InvalidArgument, "too many bindings"));
  }
  std::map<std::string, std::vector<const Value*>> by_name;
  for (const auto& binding : bindings.inputs)
    by_name[binding.name].push_back(&binding.value);
  for (const auto& entry : by_name) {
    if (!input_internal::valid_input_name(entry.first))
      return Result<std::vector<Value>>(
          Status::failure(ErrorCode::InvalidArgument,
                          "malformed binding name: " + entry.first));
  }
  for (const auto& entry : by_name) {
    if (entry.second.size() != 1)
      return Result<std::vector<Value>>(
          Status::failure(ErrorCode::InvalidArgument,
                          "duplicate binding name: " + entry.first));
  }
  std::map<std::string, std::size_t> declared_names;
  const auto& declarations = plan.input_declarations();
  for (std::size_t i = 0; i < declarations.size(); ++i)
    declared_names.emplace(declarations[i].name, i);
  for (const auto& entry : by_name) {
    if (declared_names.count(entry.first) == 0)
      return Result<std::vector<Value>>(Status::failure(
          ErrorCode::InvalidArgument, "extra binding name: " + entry.first));
  }
  for (const auto& entry : declared_names) {
    if (by_name.count(entry.first) == 0)
      return Result<std::vector<Value>>(Status::failure(
          ErrorCode::InvalidArgument, "missing binding name: " + entry.first));
  }
  std::vector<Value> values;
  values.reserve(declarations.size());
  for (const auto& declaration : declarations) {
    const auto& value = *by_name.at(declaration.name).front();
    const auto status = input_internal::validate_binding(declaration, value);
    if (!status.ok())
      return Result<std::vector<Value>>(status);
    values.push_back(value);
  }
  const auto stop = [&]() noexcept { return binding_stop(plan, cancellation); };
  for (const auto& step : plan.steps()) {
    for (std::size_t i = 0; i < step.inputs.size(); ++i) {
      const auto* input = std::get_if<PlanWorkflowInput>(&step.inputs[i]);
      if (!input)
        continue;
      const auto status = input_internal::validate_port_value(
          step.traits.input_schema[i], values.at(input->declaration_index),
          ErrorCode::InvalidArgument, stop);
      if (!status.ok())
        return Result<std::vector<Value>>(status);
    }
  }
  return Result<std::vector<Value>>(std::move(values));
}

/** @brief Validates/copies complete regional binding metadata before any source
 * callback. */
Result<std::vector<ExecutionBinding>> preflight_regional_bindings(
    const ExecutionPlan& plan, const ExecutionBindings& bindings,
    const CancellationToken& cancellation) {
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
    if (binding.snapshot) {
      if (binding.source || binding.value.valid() || !binding.snapshot->valid())
        return Result<std::vector<ExecutionBinding>>(
            Status::failure(ErrorCode::InvalidArgument,
                            "binding must select exactly one input"));
      auto source = std::make_shared<RegionalSource>();
      source->descriptor = binding.snapshot->descriptor();
      source->facets = binding.snapshot->facets();
      source->read = [snapshot = binding.snapshot](
                         const Region& r, std::uint8_t* bytes,
                         std::uint64_t size, const BufferAllocator&,
                         const CancellationToken& token) {
        if (token.cancelled())
          return Result<Region>(
              Status::failure(ErrorCode::Cancelled, "snapshot read cancelled"));
        auto status = snapshot->read(r, bytes, size);
        return status.ok() ? Result<Region>(r) : Result<Region>(status);
      };
      binding.source = std::move(source);
    }
    if (binding.source) {
      if (binding.value.valid() || !binding.source->read)
        return Result<std::vector<ExecutionBinding>>(
            Status::failure(ErrorCode::InvalidArgument,
                            "binding must select one valid source or Value"));
      auto source = std::make_shared<RegionalSource>(*binding.source);
      auto status = input_internal::canonicalize_facets(&source->facets);
      if (!status.ok())
        return Result<std::vector<ExecutionBinding>>(status);
      if (source->descriptor.element_type !=
              declaration.descriptor.element_type ||
          source->descriptor.shape != declaration.descriptor.shape ||
          !input_internal::same_facets(source->facets, declaration.facets))
        return Result<std::vector<ExecutionBinding>>(Status::failure(
            ErrorCode::TypeMismatch,
            "regional source metadata differs from declaration"));
      binding.source = std::move(source);
    } else {
      auto status =
          input_internal::validate_binding(declaration, binding.value);
      if (!status.ok())
        return Result<std::vector<ExecutionBinding>>(status);
    }
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
      if (constraint.kind != OperationPortKind::Float32Scalar)
        continue;
      if (binding.source)
        return Result<std::vector<ExecutionBinding>>(
            Status::failure(ErrorCode::InvalidArgument,
                            "scalar parameter requires a Value binding"));
      auto status = input_internal::validate_port_value(
          constraint, binding.value, ErrorCode::InvalidArgument, stop);
      if (!status.ok())
        return Result<std::vector<ExecutionBinding>>(status);
    }
  }
  return Result<std::vector<ExecutionBinding>>(std::move(result));
}

/** @brief Finds work needed for outputs, stopping at Run-local
 * materializations. */
std::vector<bool> required_steps(const ExecutionPlan& plan,
                                 const std::map<std::uint64_t, Value>& cached,
                                 std::size_t future_whole_begin = SIZE_MAX) {
  std::vector<bool> required(plan.steps().size(), false);
  for (const auto& output : plan.outputs())
    required[output.second] = true;
  for (std::size_t i = future_whole_begin; i < plan.steps().size(); ++i)
    if (plan.steps()[i].whole_boundary)
      required[i] = true;
  for (std::size_t reverse = plan.steps().size(); reverse > 0; --reverse) {
    const auto i = reverse - 1;
    if (!required[i] || cached.count(plan.steps()[i].node_id) != 0)
      continue;
    for (const auto& source : plan.steps()[i].inputs)
      if (const auto* producer = std::get_if<PlanStepInput>(&source))
        required[producer->step_index] = true;
  }
  return required;
}

/**
 * @brief Coordinates one dependency-ordered execution through shared pools.
 *
 * The coordinator owns all per-execution mutable state, drains every started
 * callback, assembles the complete local result under its mutex, and publishes
 * success only after a final cancellation-then-currentness recheck. It never
 * stores itself in global state.
 */
class ExecutionRun final : public std::enable_shared_from_this<ExecutionRun> {
 public:
  /**
   * @brief Builds dependency counters and initial deterministic ready set.
   * @param cpu_pool Required CPU callback pool with context lifetime.
   * @param gpu_pool Optional GPU callback pool with context lifetime.
   * @param waiting_admission Context-wide waiting-callback owner.
   * @param reservation Complete accounted working-set owner.
   * @param invoke Callback entry retaining registry ownership and currentness.
   * @param plan Immutable validated physical plan.
   * @param cancellation Cooperative caller token.
   * @param maximum_parallelism Positive per-execution in-flight bound.
   * @throws std::invalid_argument If plan topology/output indexes are invalid.
   * @throws std::bad_alloc If per-execution state allocation fails.
   * @note No callback is submitted during construction.
   */
  ExecutionRun(
      ThreadPool* cpu_pool, ThreadPool* gpu_pool,
      WaitingAdmission* waiting_admission,
      std::shared_ptr<MemoryReservation> reservation,
      std::function<Result<Value>(const std::string&,
                                  const OperationInvocation&)>
          invoke,
      const ExecutionPlan* plan, std::vector<Value> bindings,
      CancellationToken cancellation, std::uint32_t maximum_parallelism,
      bool regional = false, const std::map<std::uint64_t, Value>& cached = {},
      const std::map<std::uint64_t, Backend>& cached_backends = {},
      std::function<void(std::size_t, const Value&, Backend)> retain = {})
      : cpu_pool_(cpu_pool),
        gpu_pool_(gpu_pool),
        waiting_admission_(waiting_admission),
        reservation_(std::move(reservation)),
        invoke_(std::move(invoke)),
        plan_(plan),
        bindings_(std::move(bindings)),
        cancellation_(std::move(cancellation)),
        maximum_parallelism_(maximum_parallelism),
        regional_(regional),
        retain_(std::move(retain)),
        values_(plan->steps().size()),
        value_backends_(plan->steps().size(), Backend::Cpu),
        completed_(plan->steps().size(), false),
        remaining_dependencies_(plan->steps().size(), 0U),
        dependents_(plan->steps().size()),
        remaining_readers_(plan->steps().size(), 0),
        retained_output_(plan->steps().size(), false),
        binding_readers_(bindings_.size(), 0) {
    if (!cpu_pool_ || !waiting_admission_ || !reservation_ || !invoke_ ||
        !plan_ || plan_->revision() == 0U || plan_->steps().empty() ||
        maximum_parallelism_ == 0U) {
      throw std::invalid_argument("execution plan or bounds are invalid");
    }
    std::set<const CpuStorage*> external_owners;
    for (const auto& input : bindings_) {
      if (input.valid() &&
          external_owners.insert(input.storage().get()).second) {
        auto sum =
            checked_add(retained_input_bytes_, input.storage()->capacity());
        if (!sum.ok())
          throw std::invalid_argument(sum.status().message);
        retained_input_bytes_ = sum.value();
      }
    }
    const auto required = regional
                              ? required_steps(*plan_, cached)
                              : std::vector<bool>(plan_->steps().size(), true);
    for (std::size_t i = 0; i < plan_->steps().size(); ++i) {
      const auto found = cached.find(plan_->steps()[i].node_id);
      if (!required[i] || found != cached.end()) {
        completed_[i] = true;
        ++completed_count_;
        if (found != cached.end()) {
          values_[i] = found->second;
          const auto backend = cached_backends.find(plan_->steps()[i].node_id);
          if (backend != cached_backends.end())
            value_backends_[i] = backend->second;
        }
      }
    }
    for (std::size_t step_index = 0; step_index < plan_->steps().size();
         ++step_index) {
      const PlanStep& step = plan_->steps()[step_index];
      if (completed_[step_index])
        continue;
      if (step.input_demands.size() != step.inputs.size() ||
          step.output_demand.empty() ||
          !step.output_demand.validate(step.output_descriptor.shape).ok()) {
        throw std::invalid_argument(
            "execution plan Region demand metadata is invalid");
      }
      for (std::size_t position = 0; position < step.inputs.size();
           ++position) {
        const auto& input = step.inputs[position];
        const ValueDescriptor* descriptor = nullptr;
        if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
          if (producer->step_index >= step_index)
            throw std::invalid_argument("plan input must name earlier step");
          descriptor = &plan_->steps()[producer->step_index].output_descriptor;
          dependents_[producer->step_index].push_back(step_index);
          ++remaining_readers_[producer->step_index];
          if (!completed_[producer->step_index])
            ++remaining_dependencies_[step_index];
        } else {
          const auto index =
              std::get<PlanWorkflowInput>(input).declaration_index;
          if (index >= bindings_.size() ||
              index >= plan_->input_declarations().size())
            throw std::invalid_argument("plan declaration index is invalid");
          descriptor = &plan_->input_declarations()[index].descriptor;
          ++binding_readers_[index];
        }
        if (step.input_demands[position].empty() ||
            !step.input_demands[position].validate(descriptor->shape).ok())
          throw std::invalid_argument("plan input Region demand is invalid");
      }
      if (remaining_dependencies_[step_index] == 0U) {
        ready_.push(step_index);
      }
    }
    for (const auto& output : plan_->outputs()) {
      if (output.first.empty() || output.second >= plan_->steps().size()) {
        throw std::invalid_argument("execution plan output mapping is invalid");
      }
      retained_output_[output.second] = true;
    }
  }

  /**
   * @brief Schedules ready steps and waits for terminal drained state.
   * @return Complete result or first typed failure.
   * @throws std::bad_alloc If final publication allocation fails.
   * @note Failure stops new admission while already started callbacks drain.
   * Queue callback owners retire before result assembly, and Run-held Value
   * owners are cleared on every return. Complete assembly and stop checks occur
   * under the Run mutex; passing that recheck is the sole success-publication
   * linearization point.
   */
  [[nodiscard]] Result<ExecutionResult> run() {
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      observe_external_stop_locked();
      while (!failure_.has_value() && in_flight_ < maximum_parallelism_ &&
             !ready_.empty()) {
        const std::size_t step_index = ready_.top();
        ready_.pop();
        const Backend backend = plan_->steps()[step_index].backend;
        ++in_flight_;
        diagnostics_.peak_active_tasks =
            std::max(diagnostics_.peak_active_tasks, in_flight_);
        lock.unlock();
        submit_attempt(step_index, backend);
        lock.lock();
        observe_external_stop_locked();
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_post_submit_observation();
#endif
      }

      if ((failure_.has_value() || completed_count_ == values_.size()) &&
          in_flight_ == 0U && pending_callbacks_ == 0U) {
        break;
      }
      state_changed_.wait(lock);
    }

    struct ReleaseValues {
      std::vector<Value>* values;
      std::vector<Value>* bindings;
      ~ReleaseValues() {
        values->clear();
        bindings->clear();
      }
    } release_values{&values_, &bindings_};

    if (failure_.has_value()) {
      return Result<ExecutionResult>(*failure_);
    }
    if (completed_count_ != values_.size()) {
      return Result<ExecutionResult>(Status::failure(
          ErrorCode::Internal, "execution dependency state did not converge"));
    }
    if (cancellation_.cancelled()) {
      return Result<ExecutionResult>(Status::failure(
          ErrorCode::Cancelled, "execution was cancelled before publication"));
    }
    if (!plan_->current()) {
      return Result<ExecutionResult>(Status::failure(
          ErrorCode::Stale, "execution plan became stale before publication"));
    }

    ExecutionResult result;
    for (const auto& output : plan_->outputs()) {
      auto view =
          values_[output.second].view(plan_->output_regions().at(output.first));
      if (!view.ok())
        return Result<ExecutionResult>(view.status());
      result.values.emplace(output.first, view.take_value());
    }
    diagnostics_.peak_live_bytes = reservation_->peak();
    diagnostics_.planned_peak_bytes = reservation_->planned();
    diagnostics_.retained_input_bytes = retained_input_bytes_;
    result.diagnostics = std::move(diagnostics_);
    result.diagnostics.plan_digest = plan_->digest().value;
    if (!regional_)
      result.diagnostics.result_digest = result_digest(result.values);
    result.diagnostics.execute_us = duration_us(started);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    if (!regional_)
      execution_testing::notify_final_result_ready();
#endif
    if (cancellation_.cancelled()) {
      return Result<ExecutionResult>(Status::failure(
          ErrorCode::Cancelled,
          "execution was cancelled before final result publication"));
    }
    if (!plan_->current()) {
      return Result<ExecutionResult>(Status::failure(
          ErrorCode::Stale,
          "execution plan became stale before final result publication"));
    }
    return Result<ExecutionResult>(std::move(result));
  }

 private:
  /**
   * @brief Returns elapsed monotonic microseconds since one start point.
   * @param started Earlier steady-clock point.
   * @return Nonnegative duration clamped to uint64.
   * @throws Nothing.
   */
  static std::uint64_t duration_us(
      std::chrono::steady_clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return elapsed.count() <= 0 ? 0U
                                : static_cast<std::uint64_t>(elapsed.count());
  }

  /**
   * @brief Converts cancellation/currentness observations into first failure.
   * @return No value.
   * @throws Nothing.
   * @note Caller holds `mutex_` from either coordinator or worker-entry state.
   * Owned diagnostic/Status construction remains inside this no-throw
   * boundary. Its allocation-free fallback records the prioritized code with
   * an empty message without reacquiring `mutex_` or retiring an in-flight
   * callback, because this observation owns no callback completion. A worker
   * caller separately retires only its own abandoned slot.
   */
  void observe_external_stop_locked() noexcept {
    if (failure_.has_value()) {
      return;
    }
    ErrorCode code = ErrorCode::Ok;
    const char* diagnostic = nullptr;
    if (cancellation_.cancelled()) {
      code = ErrorCode::Cancelled;
      diagnostic = "execution cancellation was requested";
    } else if (!plan_->current()) {
      code = ErrorCode::Stale;
      diagnostic = "execution plan revision is no longer current";
    } else {
      return;
    }

    try {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      if (execution_testing::fail_failure_status_construction(
              execution_testing::FailureStatusConstructionPoint::
                  ExternalStop)) {
        throw std::bad_alloc();
      }
#endif
      failure_ = Status::failure(code, diagnostic);
    } catch (...) {
      failure_.emplace();
      failure_->code = prioritized_failure_code_locked(code);
      failure_->message.clear();
    }
  }

  /**
   * @brief Admits and submits one backend attempt or records backpressure.
   * @param step_index Valid physical step index.
   * @param backend CPU or optional local GPU lane.
   * @throws Nothing across callback/scheduler boundaries.
   * @note Fallback preserves the existing in-flight count. Queue ownership
   * releases shared admission before `execute_attempt` begins running. Every
   * scheduler rejection reaches the centralized first-failure selector after
   * the optional noninstalled pre-commit observation hook.
   */
  void submit_attempt(std::size_t step_index, Backend backend) noexcept {
    try {
      if (backend == Backend::Gpu && !gpu_pool_) {
        const PlanStep& step = plan_->steps()[step_index];
        const bool can_fallback =
            step.traits.allows_cpu_fallback && step.traits.supports_cpu &&
            !cancellation_.cancelled() && plan_->current();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          diagnostics_.operation_timings.push_back(OperationTiming{
              step.node_id, Backend::Gpu, 0U, ErrorCode::BackendUnavailable});
          if (can_fallback) {
            diagnostics_.fallback_reasons.push_back(
                "node " + std::to_string(step.node_id) +
                ": optional local GPU lane is unavailable");
          }
        }
        if (can_fallback) {
          submit_attempt(step_index, Backend::Cpu);
        } else {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
          execution_testing::notify_before_scheduler_failure(
              execution_testing::SchedulerFailurePoint::GpuBackendUnavailable);
#endif
          finish_failure(
              Status::failure(ErrorCode::BackendUnavailable,
                              "optional local GPU lane is unavailable"));
        }
        return;
      }
      auto self = shared_from_this();
      // Completion includes destruction of the queue's callback ownership, on
      // both normal retirement and submission failure. The ticket also covers
      // a fallback attempt while its predecessor is still returning.
      struct CallbackLifetime {
        explicit CallbackLifetime(std::shared_ptr<ExecutionRun> value)
            : owner(std::move(value)) {
          std::lock_guard<std::mutex> lock(owner->mutex_);
          ++owner->pending_callbacks_;
        }
        ~CallbackLifetime() {
          std::lock_guard<std::mutex> lock(owner->mutex_);
          --owner->pending_callbacks_;
          owner->state_changed_.notify_all();
        }
        std::shared_ptr<ExecutionRun> owner;
      };
      auto lifetime = std::make_shared<CallbackLifetime>(self);
      std::function<void()> callback = [lifetime, step_index, backend] {
        lifetime->owner->execute_attempt(step_index, backend);
      };
      auto admission = waiting_admission_->try_acquire();
      if (!admission.has_value()) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_before_scheduler_failure(
            execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
        finish_failure(Status::failure(
            ErrorCode::ResourceExhausted,
            "ExecutionContext waiting callback limit is exhausted"));
        return;
      }
      QueuedCallback queued{std::move(callback), std::move(admission.value())};
      bool accepted = false;
      if (backend == Backend::Gpu) {
        accepted = gpu_pool_ && gpu_pool_->submit(std::move(queued));
      } else {
        accepted = cpu_pool_->submit(std::move(queued));
      }
      if (!accepted) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_before_scheduler_failure(
            execution_testing::SchedulerFailurePoint::CallbackSubmitRejected);
#endif
        finish_failure(
            Status::failure(ErrorCode::ResourceExhausted,
                            "local backend callback queue is stopped"));
      }
    } catch (const std::bad_alloc&) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_before_scheduler_failure(
          execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
      finish_failure_safely(ErrorCode::ResourceExhausted,
                            "callback submission allocation failed");
    } catch (const std::exception& error) {
      finish_failure_safely(ErrorCode::Internal, error.what());
    } catch (...) {
      finish_failure_safely(ErrorCode::Internal,
                            "callback submission raised an exception");
    }
  }

  /**
   * @brief Gathers immutable inputs, accounts transfers/resources, and invokes.
   * @param step_index Valid dependency-ready step index.
   * @param backend Backend for this attempt.
   * @return No value.
   * @throws Nothing across the worker thread boundary.
   * @note The first Run-mutex critical section observes external stop before
   * dependency copies, transfers, resource admission, or callback entry. That
   * worker-entry observation is the admission cutoff for a queued attempt. A
   * cancellation or graph replacement after the cutoff may still race with a
   * non-preemptible in-process callback; completion and final publication
   * continue to reject every observed cancelled or stale result. GPU
   * unavailability may resubmit the same step on CPU exactly once, and the
   * fallback reaches this same cutoff while retaining its original slot.
   */
  void execute_attempt(std::size_t step_index, Backend backend) noexcept {
    try {
      const PlanStep& step = plan_->steps()[step_index];
      std::vector<Value> inputs;
      std::vector<bool> transfer_inputs;
      inputs.reserve(step.inputs.size());
      transfer_inputs.reserve(step.inputs.size());
      std::uint64_t transfer_count = 0U;
      std::uint64_t transfer_bytes = 0U;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        observe_external_stop_locked();
        if (failure_.has_value()) {
          finish_abandoned_attempt_locked();
          return;
        }
        for (std::size_t input_position = 0U;
             input_position < step.inputs.size(); ++input_position) {
          const auto& source = step.inputs[input_position];
          const Value* value = nullptr;
          Backend source_backend = Backend::Cpu;
          if (const auto* producer = std::get_if<PlanStepInput>(&source)) {
            const auto input_index = producer->step_index;
            if (!completed_[input_index]) {
              finish_failure_locked(
                  Status::failure(ErrorCode::Internal,
                                  "ready step observed incomplete dependency"));
              return;
            }
            value = &values_[input_index];
            source_backend = value_backends_[input_index];
          } else {
            value = &bindings_[std::get<PlanWorkflowInput>(source)
                                   .declaration_index];
          }
          const Status demand_status =
              validate_input_demand(*value, step.input_demands[input_position]);
          if (!demand_status.ok()) {
            finish_failure_locked(demand_status);
            return;
          }
          if (regional_) {
            auto part = value->view(step.input_demands[input_position]);
            if (!part.ok()) {
              finish_failure_locked(part.status());
              return;
            }
            inputs.push_back(part.take_value());
          } else {
            inputs.push_back(*value);
          }
          const bool requires_transfer = source_backend != backend;
          transfer_inputs.push_back(requires_transfer);
          if (requires_transfer) {
            if (transfer_count == std::numeric_limits<std::uint64_t>::max()) {
              finish_failure_locked(
                  Status::failure(ErrorCode::ResourceExhausted,
                                  "transfer count overflows uint64"));
              return;
            }
            ++transfer_count;
            auto bytes = regional_
                             ? region_bytes(value->descriptor(),
                                            step.input_demands[input_position])
                             : Result<std::uint64_t>(value->bytes().size());
            if (!bytes.ok()) {
              finish_failure_locked(bytes.status());
              return;
            }
            auto sum = checked_add(transfer_bytes, bytes.value());
            if (!sum.ok()) {
              finish_failure_locked(sum.status());
              return;
            }
            transfer_bytes = sum.value();
          }
        }
      }

      const auto allocator = reservation_->allocator();
      const auto callback_allocator =
          reservation_->allocator(step.planned_bytes);
      for (std::size_t input_index = 0U; input_index < inputs.size();
           ++input_index) {
        if (!transfer_inputs[input_index]) {
          continue;
        }
        auto transferred =
            transfer_value(inputs[input_index], allocator, regional_);
        if (!transferred.ok()) {
          finish_failure(transferred.status());
          return;
        }
        inputs[input_index] = transferred.take_value();
      }

      // Generic producers may supply masks without an image output guarantee.
      // Attribute invalid computed samples to that operation, not the caller.
      for (std::size_t port = 0; port < step.inputs.size(); ++port) {
        if (!std::holds_alternative<PlanStepInput>(step.inputs[port]) ||
            step.traits.input_schema[port].kind !=
                OperationPortKind::Float32Mask)
          continue;
        const auto valid = input_internal::validate_port_value(
            step.traits.input_schema[port], inputs[port],
            ErrorCode::OperationFailed,
            [&] { return binding_stop(*plan_, cancellation_); });
        if (!valid.ok()) {
          inputs.clear();
          finish_failure(valid);
          return;
        }
      }

      const auto started = std::chrono::steady_clock::now();
      Result<Value> invocation_result =
          invoke_(step.operation,
                  OperationInvocation{inputs, step.input_demands,
                                      step.parameters, backend, cancellation_,
                                      regional_ ? step.output_demand : Region{},
                                      callback_allocator});
      if (invocation_result.ok() &&
          !callback_allocator.owns(*invocation_result.value().storage())) {
        const auto storage = invocation_result.value().storage();
        const bool borrowed = std::any_of(
            inputs.begin(), inputs.end(),
            [&](const Value& input) { return input.storage() == storage; });
        if (!borrowed)
          invocation_result =
              transfer_value(invocation_result.value(), callback_allocator);
      }
      const std::uint64_t elapsed = duration_us(started);

      bool should_fallback = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.operation_timings.push_back(OperationTiming{
            step.node_id, backend, elapsed, invocation_result.status().code});
        auto elements = step.output_demand.element_count();
        if (elements.ok())
          diagnostics_.operation_timings.back().computed_elements =
              elements.value();

        auto count_sum =
            checked_add(diagnostics_.transfer_count, transfer_count);
        auto byte_sum =
            checked_add(diagnostics_.transfer_bytes, transfer_bytes);
        if (!count_sum.ok() || !byte_sum.ok()) {
          finish_failure_locked(!count_sum.ok() ? count_sum.status()
                                                : byte_sum.status());
          return;
        }
        diagnostics_.transfer_count = count_sum.value();
        diagnostics_.transfer_bytes = byte_sum.value();

        should_fallback =
            !invocation_result.ok() && backend == Backend::Gpu &&
            invocation_result.status().code == ErrorCode::BackendUnavailable &&
            step.traits.allows_cpu_fallback && step.traits.supports_cpu &&
            !cancellation_.cancelled() && plan_->current();
        if (should_fallback) {
          diagnostics_.fallback_reasons.push_back(
              "node " + std::to_string(step.node_id) + ": " +
              invocation_result.status().message);
        }
      }

      // Drop callback-local input/transfer owners before retiring the attempt.
      inputs.clear();
      if (should_fallback) {
        submit_attempt(step_index, Backend::Cpu);
        return;
      }
      finish_attempt(step_index, backend, std::move(invocation_result));
    } catch (const std::bad_alloc&) {
      finish_failure_safely(ErrorCode::ResourceExhausted,
                            "execution attempt allocation failed");
    } catch (const std::exception& error) {
      finish_failure_safely(ErrorCode::Internal, error.what());
    } catch (...) {
      finish_failure_safely(ErrorCode::Internal,
                            "execution attempt raised an exception");
    }
  }

  /**
   * @brief Publishes one successful completion or records its first failure.
   * @param step_index Completed step index.
   * @param backend Successful backend.
   * @param result Operation result ownership.
   * @throws std::bad_alloc If a first-failure diagnostic allocation fails.
   * @note Cancellation/currentness are rechecked before Value publication.
   * Failed callback results retain their contextual diagnostic selection and
   * then pass through the same centralized code-priority check as scheduler
   * failures before first-failure publication.
   */
  void finish_attempt(std::size_t step_index, Backend backend,
                      Result<Value> result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result.ok()) {
      Status failure = result.status();
      if (cancellation_.cancelled() || failure.code == ErrorCode::Cancelled) {
        failure = Status::failure(ErrorCode::Cancelled,
                                  "execution was cancelled during operation");
      } else if (!plan_->current()) {
        failure = Status::failure(
            ErrorCode::Stale,
            "execution plan became stale during operation completion");
      }
      finish_failure_locked(std::move(failure));
      return;
    }
    if (cancellation_.cancelled()) {
      finish_failure_locked(Status::failure(
          ErrorCode::Cancelled,
          "cancelled operation completion was rejected as stale publication"));
      return;
    }
    if (!plan_->current()) {
      finish_failure_locked(Status::failure(
          ErrorCode::Stale,
          "operation completion for a stale graph revision was rejected"));
      return;
    }
    if (completed_[step_index]) {
      finish_failure_locked(Status::failure(
          ErrorCode::Internal, "physical step completed more than once"));
      return;
    }

    values_[step_index] = result.take_value();
    if (retain_)
      retain_(step_index, values_[step_index], backend);
    for (const auto& input : plan_->steps()[step_index].inputs) {
      if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
        auto& readers = remaining_readers_[producer->step_index];
        if (readers == 0) {
          finish_failure_locked(
              Status::failure(ErrorCode::Internal, "reader counter underflow"));
          return;
        }
        --readers;
        if (readers == 0 && !retained_output_[producer->step_index])
          values_[producer->step_index] = Value();
      } else {
        const auto index = std::get<PlanWorkflowInput>(input).declaration_index;
        if (binding_readers_[index] == 0) {
          finish_failure_locked(Status::failure(
              ErrorCode::Internal, "binding reader counter underflow"));
          return;
        }
        --binding_readers_[index];
        if (binding_readers_[index] == 0)
          bindings_[index] = Value();
      }
    }
    if (remaining_readers_[step_index] == 0 && !retained_output_[step_index])
      values_[step_index] = Value();
    value_backends_[step_index] = backend;
    completed_[step_index] = true;
    ++completed_count_;
    diagnostics_.selected_backends.emplace(plan_->steps()[step_index].node_id,
                                           backend);
    for (std::size_t dependent : dependents_[step_index]) {
      if (remaining_dependencies_[dependent] == 0U) {
        finish_failure_locked(Status::failure(ErrorCode::Internal,
                                              "dependency counter underflow"));
        return;
      }
      --remaining_dependencies_[dependent];
      if (remaining_dependencies_[dependent] == 0U) {
        ready_.push(dependent);
      }
    }
    --in_flight_;
    state_changed_.notify_all();
  }

  /**
   * @brief Records an external submission/admission failure and drains a slot.
   * @param failure Non-success status.
   * @throws std::system_error If per-Run mutex acquisition fails.
   * @note The owned status is moved without allocation after cancellation and
   * plan currentness are rechecked under the first-failure lock.
   */
  void finish_failure(Status failure) {
    std::lock_guard<std::mutex> lock(mutex_);
    finish_failure_locked(std::move(failure));
  }

  /**
   * @brief Best-effort no-throw failure publication for exception fences.
   * @param code Stable failure category.
   * @param diagnostic Borrowed null-terminated diagnostic source, or null for
   * an empty diagnostic.
   * @return No value.
   * @throws Nothing.
   * @note The call boundary accepts only a pointer, so string and `Status`
   * materialization occur inside the protected block. Its allocation-free
   * fallback uses the same cancellation/stale/error code priority as the
   * ordinary first-failure path and clears diagnostics rather than allocating
   * a replacement message.
   */
  void finish_failure_safely(ErrorCode code, const char* diagnostic) noexcept {
    try {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      if (execution_testing::fail_failure_status_construction(
              execution_testing::FailureStatusConstructionPoint::
                  ExceptionFence)) {
        throw std::bad_alloc();
      }
#endif
      Status failure = Status::failure(
          code, std::string(diagnostic == nullptr ? "" : diagnostic));
      finish_failure(std::move(failure));
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failure_.has_value()) {
        failure_.emplace();
        failure_->code = prioritized_failure_code_locked(code);
        failure_->message.clear();
      }
      if (in_flight_ != 0U) {
        --in_flight_;
      }
      state_changed_.notify_all();
    }
  }

  /**
   * @brief Selects the terminal code at the first-failure linearization point.
   * @param original Scheduler, admission, callback, or exception category.
   * @return Cancellation when observed or explicit, otherwise graph Stale when
   * observed, otherwise the original non-Ok code (`Internal` for accidental
   * Ok input).
   * @throws Nothing.
   * @note Caller holds `mutex_`. The selector performs only atomic/weak-state
   * observations and scalar comparisons, so it is allocation-free and usable
   * by `finish_failure_safely` after diagnostic construction fails.
   */
  [[nodiscard]] ErrorCode prioritized_failure_code_locked(
      ErrorCode original) const noexcept {
    if (cancellation_.cancelled() || original == ErrorCode::Cancelled) {
      return ErrorCode::Cancelled;
    }
    if (!plan_->current()) {
      return ErrorCode::Stale;
    }
    return original == ErrorCode::Ok ? ErrorCode::Internal : original;
  }

  /**
   * @brief Prioritizes and stores the first failure, then drains one slot.
   * @param failure Non-success status.
   * @throws Nothing.
   * @note Caller holds `mutex_`. Moving the first owned status and clearing a
   * diagnostic after code promotion allocate no storage. Existing first
   * failure and in-flight accounting remain exact for later callbacks.
   */
  void finish_failure_locked(Status failure) noexcept {
    if (!failure_.has_value()) {
      const ErrorCode prioritized =
          prioritized_failure_code_locked(failure.code);
      if (prioritized != failure.code) {
        failure.code = prioritized;
        failure.message.clear();
      }
      failure_.emplace(std::move(failure));
    }
    if (in_flight_ != 0U) {
      --in_flight_;
    }
    state_changed_.notify_all();
  }

  /**
   * @brief Releases a callback slot abandoned after an observed failure.
   * @return No value.
   * @throws Nothing.
   * @note Caller holds `mutex_`. This is the sole slot retirement paired with
   * a worker-entry external-stop observation; the observation itself never
   * changes `in_flight_`.
   */
  void finish_abandoned_attempt_locked() noexcept {
    if (in_flight_ != 0U) {
      --in_flight_;
    }
    state_changed_.notify_all();
  }

  /** @brief Required CPU pool with longer context lifetime. */
  ThreadPool* cpu_pool_;
  /** @brief Optional local GPU pool with longer context lifetime. */
  ThreadPool* gpu_pool_;
  /** @brief Shared waiting admission with longer context lifetime. */
  WaitingAdmission* waiting_admission_;
  /** @brief Shared local byte ledger with longer context lifetime. */
  std::shared_ptr<MemoryReservation> reservation_;
  /** @brief Run callback entry retaining registry ownership and currentness. */
  std::function<Result<Value>(const std::string&, const OperationInvocation&)>
      invoke_;
  /** @brief Immutable caller-owned plan valid until `run` returns. */
  const ExecutionPlan* plan_;
  /** @brief Run-owned immutable Values in canonical declaration order. */
  std::vector<Value> bindings_;
  /** @brief Cooperative cancellation observation. */
  CancellationToken cancellation_;
  /** @brief Positive per-execution callback bound. */
  std::uint32_t maximum_parallelism_;
  bool regional_;
  std::function<void(std::size_t, const Value&, Backend)> retain_;
  /** @brief Serializes every per-execution state transition. */
  std::mutex mutex_;
  /** @brief Wakes the scheduling loop after a state transition. */
  std::condition_variable state_changed_;
  /** @brief Published step Values indexed by physical step. */
  std::vector<Value> values_;
  /** @brief Successful backend for each published Value. */
  std::vector<Backend> value_backends_;
  /** @brief Exact per-step publication guard. */
  std::vector<bool> completed_;
  /** @brief Remaining dependency count per step. */
  std::vector<std::size_t> remaining_dependencies_;
  /** @brief Reverse dependency adjacency. */
  std::vector<std::vector<std::size_t>> dependents_;
  std::vector<std::size_t> remaining_readers_;
  std::vector<bool> retained_output_;
  std::vector<std::size_t> binding_readers_;
  std::uint64_t retained_input_bytes_ = 0;
  /** @brief Deterministic smallest-index ready ordering. */
  std::priority_queue<std::size_t, std::vector<std::size_t>,
                      std::greater<std::size_t>>
      ready_;
  /** @brief Number of callbacks/fallback chains not yet terminal. */
  std::uint32_t in_flight_ = 0U;
  /** @brief Queue callback owners still alive, including finished bodies. */
  std::size_t pending_callbacks_ = 0;
  /** @brief Number of successfully published physical steps. */
  std::size_t completed_count_ = 0U;
  /** @brief First terminal failure, if any. */
  std::optional<Status> failure_;
  /** @brief Mutable raw diagnostics published only on success. */
  ExecutionDiagnostics diagnostics_;
};

}  // namespace

/**
 * @brief Implements fixed local execution-resource construction.
 * @copydetails ExecutionContext::ExecutionContext
 */
ExecutionContext::ExecutionContext(
    std::shared_ptr<OperationRegistry> operations,
    ExecutionContextConfig config)
    : impl_(std::make_unique<Impl>(std::move(operations), config)) {}

/**
 * @brief Implements exact local worker/resource teardown.
 * @copydetails ExecutionContext::~ExecutionContext
 */
ExecutionContext::~ExecutionContext() noexcept = default;
void ExecutionContext::clear_disk_cache() {
  if (impl_->disk)
    impl_->disk->clear();
}
void ExecutionContext::flush_disk_cache() {
  if (impl_->disk)
    impl_->disk->flush();
}
DiskCacheStatistics ExecutionContext::disk_cache_statistics() const {
  return impl_->disk ? impl_->disk->statistics() : DiskCacheStatistics{};
}
void ExecutionContext::clear_result_cache() {
  if (impl_->cache)
    impl_->cache->clear();
}
ResultCacheStatistics ExecutionContext::cache_statistics() const {
  return impl_->cache ? impl_->cache->statistics() : ResultCacheStatistics{};
}

/**
 * @brief Implements one bounded local execution Run.
 * @copydetails ExecutionContext::execute
 */
Result<FrozenExecution> FrozenExecution::for_region(
    const std::string& output, const Region& region) const {
  if (!valid())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid frozen execution"));
  auto tile = plan_.tile_plan(output, region);
  if (!tile.ok())
    return Result<FrozenExecution>(tile.status());
  auto result = *this;
  result.plan_ = tile.take_value();
  return Result<FrozenExecution>(std::move(result));
}

Result<FrozenExecution> ExecutionContext::freeze(
    const ExecutionPlan& plan, ExecutionBindings bindings) const {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock().get() != impl_->operation_registry.get())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid stale or foreign plan"));
  for (const auto& binding : bindings.inputs)
    if (binding.source)
      return Result<FrozenExecution>(Status::failure(
          ErrorCode::InvalidArgument,
          "freeze requires immutable Values or kernel snapshots"));
  auto validated = preflight_regional_bindings(plan, bindings, {});
  if (!validated.ok())
    return Result<FrozenExecution>(validated.status());
  FrozenExecution frozen;
  frozen.plan_ = plan;
  frozen.bindings_ = std::move(bindings);
  frozen.operations_ = impl_->operation_registry;
  // The explicit owner keeps registry and input lifetimes independent of the
  // editable graph. Ordinary plan predicates are never changed in place.
  frozen.plan_.current_check_ = [] { return true; };
  if (!plan.current())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "graph changed during freeze"));
  return Result<FrozenExecution>(std::move(frozen));
}
Result<ExecutionResult> ExecutionContext::execute(
    const FrozenExecution& frozen, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  return execute(frozen.plan_, frozen.bindings_, cancellation, options);
}
Result<ExecutionDiagnostics> ExecutionContext::execute_stream(
    const FrozenExecution& frozen, const ExecutionSink& sink,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  return execute_stream(frozen.plan_, frozen.bindings_, sink, cancellation,
                        options);
}

Result<ExecutionResult> ExecutionContext::execute(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  const bool spatial = std::any_of(
      plan.steps().begin(), plan.steps().end(), [](const PlanStep& step) {
        return step.traits.output_schema.kind ==
                   OperationPortKind::LinearPremultipliedRgbaFloat32 ||
               step.traits.output_schema.kind == OperationPortKind::Float32Mask;
      });
  const bool regional_demand = std::any_of(
      plan.outputs().begin(), plan.outputs().end(), [&](const auto& output) {
        return !input_internal::whole_region(
            plan.output_regions().at(output.first),
            plan.steps()[output.second].output_descriptor.shape);
      });
  const bool sourced =
      std::any_of(bindings.inputs.begin(), bindings.inputs.end(),
                  [](const ExecutionBinding& input) {
                    return input.source != nullptr || input.snapshot != nullptr;
                  });
  if (spatial || sourced || regional_demand)
    return execute_regions(plan, std::move(bindings), nullptr, cancellation,
                           options);
  if (!impl_) {
    return Result<ExecutionResult>(Status::failure(
        ErrorCode::Internal, "execution context has no implementation"));
  }
  if (!plan.current()) {
    return Result<ExecutionResult>(Status::failure(
        ErrorCode::Stale, "execution plan is invalid or stale"));
  }
  const auto plan_operations = plan.operation_registry_.lock();
  if (plan_operations.get() != impl_->operation_registry.get()) {
    return Result<ExecutionResult>(Status::failure(
        ErrorCode::Stale,
        "execution plan belongs to another frozen operation set"));
  }
  auto stopped = binding_stop(plan, cancellation);
  if (stopped != ErrorCode::Ok) {
    Status status;
    status.code = stopped;
    return Result<ExecutionResult>(std::move(status));
  }
  auto prepared = preflight_bindings(plan, bindings, cancellation);
  stopped = binding_stop(plan, cancellation);
  if (stopped != ErrorCode::Ok) {
    Status status;
    status.code = stopped;
    return Result<ExecutionResult>(std::move(status));
  }
  if (!prepared.ok())
    return Result<ExecutionResult>(prepared.status());
  const std::uint32_t parallelism = options.maximum_parallelism == 0U
                                        ? impl_->cpu_worker_count
                                        : options.maximum_parallelism;
  try {
    std::uint64_t working_bytes = 0;
    for (const auto& step : plan.steps()) {
      auto sum = checked_add(working_bytes, step.planned_bytes);
      if (!sum.ok())
        return Result<ExecutionResult>(sum.status());
      working_bytes = sum.value();
      for (const auto& source : step.inputs) {
        const auto* producer = std::get_if<PlanStepInput>(&source);
        const auto backend = producer
                                 ? plan.steps()[producer->step_index].backend
                                 : Backend::Cpu;
        if (backend != Backend::Cpu || step.backend != Backend::Cpu) {
          const auto& descriptor =
              producer
                  ? plan.steps()[producer->step_index].output_descriptor
                  : plan.input_declarations()
                        [std::get<PlanWorkflowInput>(source).declaration_index]
                            .descriptor;
          auto dense = input_internal::dense_metadata(descriptor);
          if (!dense.ok())
            return Result<ExecutionResult>(dense.status());
          sum = checked_add(working_bytes, dense.value().bytes);
          if (!sum.ok())
            return Result<ExecutionResult>(sum.status());
          working_bytes = sum.value();
        }
      }
    }
    auto reserved = impl_->budget->reserve(
        working_bytes, [&] { return binding_stop(plan, cancellation); });
    if (!reserved.ok())
      return Result<ExecutionResult>(reserved.status());
    auto reservation = reserved.take_value();
    struct Seal {
      std::shared_ptr<MemoryReservation> reservation;
      ~Seal() { reservation->seal(); }
    } seal{reservation};
    auto coordinator = std::make_shared<ExecutionRun>(
        &impl_->cpu_pool, impl_->gpu_pool.get(), &impl_->waiting_admission,
        reservation,
        [operations = impl_->operation_registry, &plan](
            const std::string& key, const OperationInvocation& invocation) {
          return operations->invoke_current(key, invocation,
                                            [&plan] { return plan.current(); });
        },
        &plan, prepared.take_value(), cancellation, parallelism);
    return coordinator->run();
  } catch (const std::invalid_argument& error) {
    return Result<ExecutionResult>(
        Status::failure(ErrorCode::InvalidArgument, error.what()));
  }
}

Result<ExecutionDiagnostics> ExecutionContext::execute_stream(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const ExecutionSink& sink, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  auto result =
      execute_regions(plan, std::move(bindings), &sink, cancellation, options);
  if (!result.ok())
    return Result<ExecutionDiagnostics>(result.status());
  return Result<ExecutionDiagnostics>(result.take_value().diagnostics);
}

Result<ExecutionResult> ExecutionContext::execute_regions(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const ExecutionSink* sink, const CancellationToken& cancellation,
    const ExecutionOptions& options, bool shared_producer,
    std::uint64_t producer_epoch) {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock() != impl_->operation_registry)
    return Result<ExecutionResult>(Status::failure(
        ErrorCode::Stale, "regional plan is invalid, stale or foreign"));
  const auto stop = [&] { return binding_stop(plan, cancellation); };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok) {
      status.code = code;
      status.message.clear();
    }
    return Result<ExecutionResult>(std::move(status));
  };
  if (stop() != ErrorCode::Ok) {
    Status status;
    status.code = stop();
    return failure(status);
  }
  if (sink && !*sink)
    return failure(
        Status::failure(ErrorCode::InvalidArgument, "stream sink is empty"));
  const auto started = std::chrono::steady_clock::now();
  try {
    auto validated = preflight_regional_bindings(plan, bindings, cancellation);
    if (!validated.ok())
      return failure(validated.status());
    auto snapshot = validated.take_value();
    auto observation =
        std::make_shared<execution_internal::MemoryObservation>();
    std::map<std::uint64_t, Value> cached;
    std::map<std::uint64_t, Backend> cached_backends;
    ExecutionDiagnostics diagnostics;
    diagnostics.plan_digest = plan.digest().value;
    std::set<const CpuStorage*> input_owners;
    for (const auto& binding : snapshot) {
      if (binding.value.valid() &&
          input_owners.insert(binding.value.storage().get()).second) {
        auto sum = checked_add(diagnostics.retained_input_bytes,
                               binding.value.storage()->capacity());
        if (!sum.ok())
          return failure(sum.status());
        diagnostics.retained_input_bytes = sum.value();
      }
    }
    const auto accumulate = [&](const ExecutionDiagnostics& part) -> Status {
      auto add = [](std::uint64_t* into, std::uint64_t count) {
        auto sum = checked_add(*into, count);
        if (!sum.ok())
          return sum.status();
        *into = sum.value();
        return Status::success();
      };
      if (!shared_producer)
        diagnostics.shared_peak_live_bytes = std::max(
            diagnostics.shared_peak_live_bytes,
            std::max(part.shared_peak_live_bytes, part.peak_live_bytes));
      diagnostics.cache_hits += part.cache_hits;
      diagnostics.shared_computations += part.shared_computations;
      diagnostics.source_read_count += part.source_read_count;
      diagnostics.source_read_bytes += part.source_read_bytes;
      auto status = add(&diagnostics.transfer_count, part.transfer_count);
      if (!status.ok())
        return status;
      status = add(&diagnostics.transfer_bytes, part.transfer_bytes);
      if (!status.ok())
        return status;
      diagnostics.peak_active_tasks =
          std::max(diagnostics.peak_active_tasks, part.peak_active_tasks);
      for (const auto& backend : part.selected_backends)
        diagnostics.selected_backends[backend.first] = backend.second;
      for (const auto& timing : part.operation_timings) {
        auto found = std::find_if(diagnostics.operation_timings.begin(),
                                  diagnostics.operation_timings.end(),
                                  [&](const OperationTiming& prior) {
                                    return prior.node_id == timing.node_id &&
                                           prior.backend == timing.backend;
                                  });
        if (found == diagnostics.operation_timings.end()) {
          diagnostics.operation_timings.push_back(timing);
        } else {
          for (const auto& pair :
               {std::make_pair(&found->duration_us, timing.duration_us),
                std::make_pair(&found->invocation_count,
                               timing.invocation_count),
                std::make_pair(&found->computed_elements,
                               timing.computed_elements)}) {
            status = add(pair.first, pair.second);
            if (!status.ok())
              return status;
          }
          found->outcome = timing.outcome;
        }
      }
      for (const auto& reason : part.fallback_reasons)
        if (diagnostics.fallback_reasons.size() < 2 * plan.steps().size() &&
            std::find(diagnostics.fallback_reasons.begin(),
                      diagnostics.fallback_reasons.end(),
                      reason) == diagnostics.fallback_reasons.end())
          diagnostics.fallback_reasons.push_back(reason);
      return Status::success();
    };
    struct Seal {
      std::shared_ptr<MemoryReservation> reservation;
      ~Seal() {
        if (reservation)
          reservation->seal();
      }
    };
    std::map<std::string, MutableValue> collected;
    std::map<std::string, std::vector<ValueFacet>> collected_facets;
    std::map<std::string, Value> shared_values;
    if (!sink && !shared_producer) {
      std::uint64_t bytes = 0;
      for (const auto& output : plan.outputs()) {
        auto size = region_bytes(plan.steps()[output.second].output_descriptor,
                                 plan.output_regions().at(output.first));
        if (!size.ok())
          return failure(size.status());
        auto sum = checked_add(bytes, size.value());
        if (!sum.ok())
          return failure(sum.status());
        bytes = sum.value();
      }
      if (impl_->disk && impl_->budget->available() < bytes)
        impl_->disk->drop_pending();
      if (impl_->cache)
        impl_->cache->reclaim_for(bytes);
      auto reserved = impl_->budget->reserve(bytes, stop, observation);
      if (!reserved.ok())
        return failure(reserved.status());
      Seal seal{reserved.take_value()};
      auto allocator = seal.reservation->allocator();
      for (const auto& output : plan.outputs()) {
        auto value = MutableValue::allocate(
            plan.steps()[output.second].output_descriptor,
            plan.output_regions().at(output.first), allocator);
        if (!value.ok())
          return failure(value.status());
        collected.emplace(output.first, value.take_value());
      }
      // Collector capacity is retained storage, never an active task to wait
      // on.
      seal.reservation->seal();
    }
    const std::uint32_t parallelism = options.maximum_parallelism == 0
                                          ? impl_->cpu_worker_count
                                          : options.maximum_parallelism;
    const auto materialize =
        [&](const ExecutionPlan& tile) -> Result<ExecutionResult> {
      auto tile_cached = cached;
      auto tile_backends = cached_backends;
      std::vector<std::string> keys;
      const auto cache_epoch = producer_epoch != UINT64_MAX
                                   ? producer_epoch
                                   : (impl_->cache ? impl_->cache->epoch() : 0);
      if (impl_->cache) {
        keys = execution_internal::result_keys(tile, snapshot);
        std::vector<bool> needed(keys.size(), false);
        for (const auto& output : tile.outputs())
          needed[output.second] = true;
        for (std::size_t reverse = keys.size(); reverse > 0; --reverse) {
          const auto i = reverse - 1;
          if (!needed[i] || tile_cached.count(tile.steps()[i].node_id))
            continue;
          auto hit = impl_->cache->get(keys[i]);
          if (!hit.valid() && impl_->disk) {
            const auto& step = tile.steps()[i];
            hit = impl_->disk->get(keys[i], step.output_descriptor,
                                   step.output_demand,
                                   step.traits.output_schema.kind);
            if (hit.valid())
              impl_->cache->put(keys[i], hit, cache_epoch);
          }
          if (hit.valid()) {
            tile_cached[tile.steps()[i].node_id] = std::move(hit);
            tile_backends[tile.steps()[i].node_id] = Backend::Cpu;
            ++diagnostics.cache_hits;
          } else {
            for (const auto& input : tile.steps()[i].inputs)
              if (const auto* producer = std::get_if<PlanStepInput>(&input))
                needed[producer->step_index] = true;
          }
        }
        const auto target = tile.outputs().begin()->second;
        if (!shared_producer && tile.outputs().size() == 1 &&
            !keys[target].empty() &&
            !tile_cached.count(tile.steps()[target].node_id)) {
          // Producers own copies, never a waiting caller's stack or stop token.
          auto pinned = tile;
          pinned.current_check_ = [] { return true; };
          pinned.tile_height_ = UINT64_MAX;
          pinned.tile_width_ = UINT64_MAX;
          auto result = impl_->cache->compute(
              keys[target], stop,
              [this, pinned, bindings, options,
               cache_epoch](const CancellationToken& token) {
                return execute_regions(pinned, bindings, nullptr, token,
                                       options, true, cache_epoch);
              });
          if (result.ok() && result.value().values.begin()->first !=
                                 tile.outputs().begin()->first) {
            auto renamed = result.take_value();
            auto value = renamed.values.begin()->second;
            renamed.values.clear();
            renamed.values.emplace(tile.outputs().begin()->first,
                                   std::move(value));
            return Result<ExecutionResult>(std::move(renamed));
          }
          return result;
        }
      }
      const auto required = required_steps(tile, tile_cached);
      std::vector<std::optional<Region>> input_demands(snapshot.size());
      std::uint64_t working = 0;
      for (std::size_t i = 0; i < tile.steps().size(); ++i) {
        const auto& step = tile.steps()[i];
        if (!required[i] || tile_cached.count(step.node_id))
          continue;
        auto sum = checked_add(working, step.planned_bytes);
        if (!sum.ok())
          return Result<ExecutionResult>(sum.status());
        working = sum.value();
        for (std::size_t port = 0; port < step.inputs.size(); ++port) {
          const auto& source = step.inputs[port];
          const auto* producer = std::get_if<PlanStepInput>(&source);
          if (!producer) {
            auto index = std::get<PlanWorkflowInput>(source).declaration_index;
            const auto& demand = step.input_demands[port];
            auto& merged = input_demands[index];
            if (!merged) {
              merged = demand;
            } else {
              std::vector<RegionDimension> dimensions;
              for (std::size_t axis = 0; axis < demand.rank(); ++axis) {
                const auto a = merged->dimensions()[axis],
                           b = demand.dimensions()[axis];
                const auto start = std::min(a.offset, b.offset);
                dimensions.push_back(
                    {start, std::max(a.offset + a.extent, b.offset + b.extent) -
                                start});
              }
              merged = Region(std::move(dimensions));
            }
          }
          const auto backend = producer
                                   ? tile.steps()[producer->step_index].backend
                                   : Backend::Cpu;
          if (backend != Backend::Cpu || step.backend != Backend::Cpu) {
            const auto& descriptor =
                producer
                    ? tile.steps()[producer->step_index].output_descriptor
                    : tile.input_declarations()[std::get<PlanWorkflowInput>(
                                                    source)
                                                    .declaration_index]
                          .descriptor;
            auto bytes = region_bytes(descriptor, step.input_demands[port]);
            if (!bytes.ok())
              return Result<ExecutionResult>(bytes.status());
            sum = checked_add(working, bytes.value());
            if (!sum.ok())
              return Result<ExecutionResult>(sum.status());
            working = sum.value();
          }
        }
      }
      for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (!input_demands[i] || !snapshot[i].source)
          continue;
        auto bytes =
            region_bytes(snapshot[i].source->descriptor, *input_demands[i]);
        if (!bytes.ok())
          return Result<ExecutionResult>(bytes.status());
        auto total =
            checked_add(bytes.value(), snapshot[i].source->workspace_bytes);
        if (!total.ok())
          return Result<ExecutionResult>(total.status());
        total = checked_add(working, total.value());
        if (!total.ok())
          return Result<ExecutionResult>(total.status());
        working = total.value();
      }
      if (impl_->disk && impl_->budget->available() < working)
        impl_->disk->drop_pending();
      if (impl_->cache)
        impl_->cache->reclaim_for(working);
      auto reserved = impl_->budget->reserve(working, stop, observation);
      if (!reserved.ok())
        return Result<ExecutionResult>(reserved.status());
      Seal seal{reserved.take_value()};
      std::vector<Value> values(snapshot.size());
      for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (!input_demands[i])
          continue;
        const auto& binding = snapshot[i];
        if (!binding.source) {
          auto view = binding.value.view(*input_demands[i]);
          if (!view.ok())
            return Result<ExecutionResult>(view.status());
          values[i] = view.take_value();
          continue;
        }
        if (stop() != ErrorCode::Ok) {
          Status status;
          status.code = stop();
          return Result<ExecutionResult>(status);
        }
        auto made = MutableValue::allocate(binding.source->descriptor,
                                           *input_demands[i],
                                           seal.reservation->allocator());
        if (!made.ok())
          return Result<ExecutionResult>(made.status());
        auto writer = made.take_value();
        struct ReadCompletion {
          std::promise<Result<Region>> promise;
          Result<Region> result{Status{ErrorCode::Internal, {}}};
        };
        auto completion = std::make_shared<ReadCompletion>();
        auto future = completion->promise.get_future();
        auto admission = impl_->waiting_admission.try_acquire();
        if (!admission)
          return Result<ExecutionResult>(Status::failure(
              ErrorCode::ResourceExhausted, "source waiting queue is full"));
        auto scratch =
            seal.reservation->allocator(binding.source->workspace_bytes);
        QueuedCallback callback{
            [&, completion, source = binding.source, demand = *input_demands[i],
             scratch]() {
              try {
                const auto code = stop();
                if (code != ErrorCode::Ok) {
                  Status status;
                  status.code = code;
                  completion->result = Result<Region>(status);
                  return;
                }
                completion->result =
                    source->read(demand, writer.data(), writer.size(), scratch,
                                 cancellation);
              } catch (const std::bad_alloc&) {
                Status status;
                status.code = ErrorCode::ResourceExhausted;
                completion->result = Result<Region>(status);
              } catch (const std::exception& error) {
                try {
                  completion->result = Result<Region>(
                      Status::failure(ErrorCode::OperationFailed,
                                      error.what() ? error.what() : ""));
                } catch (...) {
                  Status status;
                  status.code = ErrorCode::OperationFailed;
                  completion->result = Result<Region>(status);
                }
              } catch (...) {
                Status status;
                status.code = ErrorCode::OperationFailed;
                completion->result = Result<Region>(status);
              }
            },
            std::move(*admission),
            [completion] {
              completion->promise.set_value(std::move(completion->result));
            }};
        if (!impl_->cpu_pool.submit(std::move(callback)))
          return Result<ExecutionResult>(Status::failure(
              ErrorCode::ResourceExhausted, "source queue stopped"));
        auto written = future.get();
        if (!written.ok())
          return Result<ExecutionResult>(written.status());
        if (!same_region(written.value(), *input_demands[i]))
          return Result<ExecutionResult>(Status::failure(
              ErrorCode::TypeMismatch, "source returned different coverage"));
        auto sum = checked_add(diagnostics.source_read_bytes, writer.size());
        if (!sum.ok())
          return Result<ExecutionResult>(sum.status());
        diagnostics.source_read_bytes = sum.value();
        ++diagnostics.source_read_count;
        diagnostics.peak_active_tasks =
            std::max(diagnostics.peak_active_tasks, UINT32_C(1));
        auto published = std::move(writer).publish(binding.source->facets);
        if (!published.ok())
          return Result<ExecutionResult>(published.status());
        values[i] = published.take_value();
      }
      auto coordinator = std::make_shared<ExecutionRun>(
          &impl_->cpu_pool, impl_->gpu_pool.get(), &impl_->waiting_admission,
          seal.reservation,
          [operations = impl_->operation_registry, &tile](
              const std::string& key, const OperationInvocation& invocation) {
            return operations->invoke_current(
                key, invocation, [&tile] { return tile.current(); });
          },
          &tile, std::move(values), cancellation, parallelism, true,
          tile_cached, tile_backends,
          [this, keys, cache_epoch, &tile](
              std::size_t index, const Value& value, Backend backend) {
            if (impl_->cache && backend == Backend::Cpu && index < keys.size())
              impl_->cache->put(keys[index], value, cache_epoch);
            if (impl_->disk && index < keys.size() &&
                impl_->cache->epoch() == cache_epoch)
              impl_->disk->put(keys[index], value,
                               tile.steps()[index].traits.output_schema.kind);
          });
      return coordinator->run();
    };
    // Materialize Whole/effect boundaries once in source-topological order.
    for (std::size_t i = 0; i < plan.steps().size(); ++i) {
      const auto& step = plan.steps()[i];
      if (!step.whole_boundary)
        continue;
      ExecutionPlan whole = plan;
      const std::string name = "__s2_whole";
      const auto full = Region::whole(step.output_descriptor.shape);
      whole.outputs_ = {{name, i}};
      whole.output_regions_ = {{name, full}};
      auto subplan = whole.tile_plan(name, full);
      if (!subplan.ok())
        return failure(subplan.status());
      auto result = materialize(subplan.value());
      if (!result.ok())
        return failure(result.status());
      auto status = accumulate(result.value().diagnostics);
      if (!status.ok())
        return failure(status);
      cached[step.node_id] = result.value().values.at(name);
      const auto backend = diagnostics.selected_backends.find(step.node_id);
      cached_backends[step.node_id] =
          backend == diagnostics.selected_backends.end() ? step.backend
                                                         : backend->second;
      // Completed boundaries cut their ancestors from future demands. Retain
      // only cache entries needed by a remaining boundary or named output.
      const auto future = required_steps(plan, cached, i + 1);
      for (std::size_t prior = 0; prior <= i; ++prior)
        if (!future[prior]) {
          cached.erase(plan.steps()[prior].node_id);
          cached_backends.erase(plan.steps()[prior].node_id);
        }
    }
    // Whole results with no output-side reader can retire before streaming.
    const auto needed = required_steps(plan, cached);
    for (std::size_t i = 0; i < plan.steps().size(); ++i)
      if (!needed[i]) {
        cached.erase(plan.steps()[i].node_id);
        cached_backends.erase(plan.steps()[i].node_id);
      }

    ExecutionPlan remaining_outputs = plan;
    for (const auto& output : plan.outputs()) {
      const auto& requested = plan.output_regions().at(output.first);
      const bool spatial = requested.rank() >= 2;
      std::uint64_t y = spatial ? requested.dimensions()[0].offset : 0;
      const auto y_end = spatial ? y + requested.dimensions()[0].extent : 1;
      while (y < y_end) {
        const auto height =
            spatial ? std::min(plan.tile_height(), y_end - y) : 1;
        std::uint64_t x = spatial ? requested.dimensions()[1].offset : 0;
        const auto x_end = spatial ? x + requested.dimensions()[1].extent : 1;
        while (x < x_end) {
          if (stop() != ErrorCode::Ok) {
            Status status;
            status.code = stop();
            return failure(status);
          }
          const auto width =
              spatial ? std::min(plan.tile_width(), x_end - x) : 1;
          auto dimensions = requested.dimensions();
          if (spatial) {
            dimensions[0] = {y, height};
            dimensions[1] = {x, width};
          }
          const Region region(std::move(dimensions));
          auto tile = plan.tile_plan(output.first, region);
          if (!tile.ok())
            return failure(tile.status());
          auto result = materialize(tile.value());
          if (!result.ok())
            return failure(result.status());
          auto status = accumulate(result.value().diagnostics);
          if (!status.ok())
            return failure(status);
          if (stop() != ErrorCode::Ok) {
            status.code = stop();
            return failure(status);
          }
          const auto& value = result.value().values.at(output.first);
          if (sink) {
            status = (*sink)(output.first, ValueView(value));
          } else if (shared_producer) {
            shared_values.emplace(output.first, value);
          } else {
            auto found = collected_facets.find(output.first);
            if (found == collected_facets.end())
              collected_facets[output.first] = value.facets();
            else if (!input_internal::same_facets(found->second,
                                                  value.facets()))
              return failure(
                  Status::failure(ErrorCode::TypeMismatch,
                                  "output facets changed between tiles"));
            status = copy_region(ValueView(value), &collected.at(output.first),
                                 requested);
          }
          if (!status.ok())
            return failure(status);
          if (stop() != ErrorCode::Ok) {
            status.code = stop();
            return failure(status);
          }
          if (diagnostics.tile_count == UINT64_MAX)
            return failure(Status::failure(ErrorCode::ResourceExhausted,
                                           "tile count overflows"));
          ++diagnostics.tile_count;
          x += width;
        }
        y += height;
      }
      remaining_outputs.outputs_.erase(output.first);
      const auto future_needed = required_steps(remaining_outputs, cached);
      for (std::size_t i = 0; i < plan.steps().size(); ++i)
        if (!future_needed[i]) {
          cached.erase(plan.steps()[i].node_id);
          cached_backends.erase(plan.steps()[i].node_id);
        }
    }
    cached.clear();
    ExecutionResult result;
    result.values = std::move(shared_values);
    for (auto& output : collected) {
      auto value = std::move(output.second)
                       .publish(std::move(collected_facets.at(output.first)));
      if (!value.ok())
        return failure(value.status());
      result.values.emplace(output.first, value.take_value());
    }
    const auto peaks = impl_->budget->peaks(observation);
    diagnostics.peak_live_bytes = peaks.first;
    diagnostics.planned_peak_bytes = peaks.second;
    std::sort(diagnostics.operation_timings.begin(),
              diagnostics.operation_timings.end(),
              [](const OperationTiming& a, const OperationTiming& b) {
                return a.node_id != b.node_id ? a.node_id < b.node_id
                                              : a.backend < b.backend;
              });
    if (!sink)
      diagnostics.result_digest = result_digest(result.values);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    diagnostics.execute_us =
        elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    result.diagnostics = std::move(diagnostics);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_final_result_ready();
#endif
    if (stop() != ErrorCode::Ok) {
      Status status;
      status.code = stop();
      return failure(status);
    }
    return Result<ExecutionResult>(std::move(result));
  } catch (const std::bad_alloc&) {
    Status status;
    status.code = ErrorCode::ResourceExhausted;
    return failure(status);
  } catch (const std::exception& error) {
    return failure(Status::failure(ErrorCode::OperationFailed,
                                   error.what() ? error.what() : ""));
  } catch (...) {
    Status status;
    status.code = ErrorCode::OperationFailed;
    return failure(status);
  }
}

/**
 * @brief Implements resolved CPU-worker count observation.
 * @copydetails ExecutionContext::cpu_workers
 */
std::uint32_t ExecutionContext::cpu_workers() const noexcept {
  return impl_ ? impl_->cpu_worker_count : 0U;
}

/**
 * @brief Implements optional local GPU-lane observation.
 * @copydetails ExecutionContext::gpu_enabled
 */
bool ExecutionContext::gpu_enabled() const noexcept {
  return impl_ && impl_->gpu_available;
}

}  // namespace ps
