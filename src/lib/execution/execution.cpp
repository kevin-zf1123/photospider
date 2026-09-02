#include "photospider/execution/execution.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

/**
 * @brief Process-local exact modeled-byte admission ledger.
 *
 * Acquisition is nonblocking: callers receive ordinary backpressure when the
 * configured bound would be exceeded. Every successful acquisition returns a
 * move-only lease whose destruction releases the exact byte count.
 */
class ResourceLedger final {
 public:
  /**
   * @brief Move-only exact-release token for one admission.
   *
   * @note A default token owns no bytes and is safe to destroy.
   */
  class Lease final {
   public:
    /**
     * @brief Constructs an empty non-owning lease.
     * @throws Nothing.
     * @note Destruction performs no ledger mutation.
     */
    Lease() noexcept = default;

    /**
     * @brief Transfers exact-release ownership.
     * @param other Source lease invalidated by the move.
     * @throws Nothing.
     */
    Lease(Lease&& other) noexcept
        : ledger_(std::exchange(other.ledger_, nullptr)),
          bytes_(std::exchange(other.bytes_, 0U)),
          live_after_acquire_(other.live_after_acquire_) {}

    /**
     * @brief Releases current ownership before taking another lease.
     * @param other Source lease invalidated by the move.
     * @return This lease.
     * @throws Nothing.
     */
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        ledger_ = std::exchange(other.ledger_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0U);
        live_after_acquire_ = other.live_after_acquire_;
      }
      return *this;
    }

    /**
     * @brief Releases the exact owned byte count.
     * @throws Nothing.
     * @note A moved-from or default lease releases nothing.
     */
    ~Lease() noexcept { release(); }

    /**
     * @brief Forbids duplicating exact byte-release ownership.
     * @param other Source lease that cannot be copied.
     * @throws Nothing; the operation is deleted.
     * @note Move operations preserve exactly-once release.
     */
    Lease(const Lease& other) = delete;
    /**
     * @brief Forbids copy assignment of exact byte-release ownership.
     * @param other Source lease that cannot be assigned.
     * @return No value; the operation is deleted.
     * @throws Nothing; the operation is deleted.
     * @note A ledger admission can have only one releasing lease.
     */
    Lease& operator=(const Lease& other) = delete;

    /**
     * @brief Returns shared live bytes immediately after this acquisition.
     * @return Global live-byte observation.
     * @throws Nothing.
     */
    [[nodiscard]] std::uint64_t live_after_acquire() const noexcept {
      return live_after_acquire_;
    }

   private:
    friend class ResourceLedger;

    /**
     * @brief Constructs one owning lease.
     * @param ledger Owning ledger.
     * @param bytes Exact admitted byte count.
     * @param live_after_acquire Shared live-byte observation.
     * @throws Nothing.
     */
    Lease(ResourceLedger* ledger, std::uint64_t bytes,
          std::uint64_t live_after_acquire) noexcept
        : ledger_(ledger),
          bytes_(bytes),
          live_after_acquire_(live_after_acquire) {}

    /**
     * @brief Releases owned bytes once and becomes empty.
     * @throws Nothing.
     * @note Ledger lifetime exceeds every lease by ExecutionContext contract.
     */
    void release() noexcept {
      if (ledger_) {
        ledger_->release(bytes_);
        ledger_ = nullptr;
        bytes_ = 0U;
      }
    }

    /** @brief Ledger receiving exact release, or null. */
    ResourceLedger* ledger_ = nullptr;
    /** @brief Exact admitted byte count. */
    std::uint64_t bytes_ = 0U;
    /** @brief Shared live-byte count after acquisition. */
    std::uint64_t live_after_acquire_ = 0U;
  };

  /**
   * @brief Constructs a positive fixed-capacity ledger.
   * @param maximum_bytes Maximum simultaneous modeled bytes.
   * @throws std::invalid_argument If capacity is zero.
   * @note Admission is global to one ExecutionContext, not per graph.
   */
  explicit ResourceLedger(std::uint64_t maximum_bytes)
      : maximum_bytes_(maximum_bytes) {
    if (maximum_bytes == 0U) {
      throw std::invalid_argument("resource-ledger capacity must be positive");
    }
  }

  /**
   * @brief Attempts immediate exact byte admission.
   * @param bytes Planned invocation byte demand.
   * @return Owning lease or `ResourceExhausted` backpressure.
   * @throws std::bad_alloc If diagnostic allocation fails.
   * @note Zero-byte demand still returns a valid empty accounting lease.
   */
  [[nodiscard]] Result<Lease> acquire(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > maximum_bytes_ || live_bytes_ > maximum_bytes_ - bytes) {
      return Result<Lease>(Status::failure(
          ErrorCode::ResourceExhausted,
          "local modeled-byte capacity is temporarily exhausted"));
    }
    live_bytes_ += bytes;
    return Result<Lease>(Lease(this, bytes, live_bytes_));
  }

  /**
   * @brief Returns currently admitted modeled bytes.
   * @return Exact shared live count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint64_t live_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_bytes_;
  }

 private:
  /**
   * @brief Releases one previously admitted exact byte count.
   * @param bytes Exact lease count.
   * @throws Nothing.
   * @note Underflow is prevented by Lease move-only ownership.
   */
  void release(std::uint64_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes <= live_bytes_) {
      live_bytes_ -= bytes;
    } else {
      live_bytes_ = 0U;
    }
  }

  /** @brief Serializes aggregate accounting. */
  mutable std::mutex mutex_;
  /** @brief Fixed maximum simultaneous modeled bytes. */
  const std::uint64_t maximum_bytes_;
  /** @brief Current exactly admitted bytes. */
  std::uint64_t live_bytes_ = 0U;
};

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
  constexpr char kDomain[] = "photospider.result-digest.v1";
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

/**
 * @brief Materializes one immutable Value into another local backend residency.
 * @param source Valid producer Value.
 * @return Deep-copied Value preserving descriptor, Region, layout, and facets.
 * @throws std::bad_alloc If transfer allocation fails.
 * @note Backend residency is tracked by the owning ExecutionRun; Value itself
 * remains backend-neutral and exposes no native device handle.
 */
Result<Value> transfer_value(const Value& source) {
  return Value::create(source.descriptor(), source.region(), source.layout(),
                       source.bytes(), source.facets());
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
        ledger(requested.maximum_live_bytes),
        waiting_admission(maximum_waiting_callbacks),
        cpu_pool(cpu_worker_count, Backend::Cpu) {
    if (!operation_registry || !operation_registry->frozen()) {
      throw std::invalid_argument(
          "ExecutionContext requires a frozen operation registry");
    }
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
  ResourceLedger ledger;
  /** @brief Shared CPU/GPU waiting-callback admission owner. */
  WaitingAdmission waiting_admission;
  /** @brief Required fixed CPU callback pool. */
  ThreadPool cpu_pool;
  /** @brief Optional single local GPU callback lane. */
  std::unique_ptr<ThreadPool> gpu_pool;
};

namespace {

/**
 * @brief Coordinates one dependency-ordered execution through shared pools.
 *
 * The coordinator owns all per-execution mutable state, publishes values only
 * after cancellation/currentness checks, drains every started callback before
 * returning, and never stores itself in global state.
 */
class ExecutionRun final : public std::enable_shared_from_this<ExecutionRun> {
 public:
  /**
   * @brief Builds dependency counters and initial deterministic ready set.
   * @param cpu_pool Required CPU callback pool with context lifetime.
   * @param gpu_pool Optional GPU callback pool with context lifetime.
   * @param waiting_admission Context-wide waiting-callback owner.
   * @param ledger Context-wide modeled-byte owner.
   * @param operations Frozen registry shared by every callback.
   * @param plan Immutable validated physical plan.
   * @param cancellation Cooperative caller token.
   * @param maximum_parallelism Positive per-execution in-flight bound.
   * @throws std::invalid_argument If plan topology/output indexes are invalid.
   * @throws std::bad_alloc If per-execution state allocation fails.
   * @note No callback is submitted during construction.
   */
  ExecutionRun(ThreadPool* cpu_pool, ThreadPool* gpu_pool,
               WaitingAdmission* waiting_admission, ResourceLedger* ledger,
               std::shared_ptr<OperationRegistry> operations,
               const ExecutionPlan* plan, CancellationToken cancellation,
               std::uint32_t maximum_parallelism)
      : cpu_pool_(cpu_pool),
        gpu_pool_(gpu_pool),
        waiting_admission_(waiting_admission),
        ledger_(ledger),
        operations_(std::move(operations)),
        plan_(plan),
        cancellation_(std::move(cancellation)),
        maximum_parallelism_(maximum_parallelism),
        values_(plan->steps().size()),
        value_backends_(plan->steps().size(), Backend::Cpu),
        completed_(plan->steps().size(), false),
        remaining_dependencies_(plan->steps().size(), 0U),
        dependents_(plan->steps().size()) {
    if (!cpu_pool_ || !waiting_admission_ || !ledger_ || !operations_ ||
        !plan_ || plan_->revision() == 0U || plan_->steps().empty() ||
        maximum_parallelism_ == 0U) {
      throw std::invalid_argument("execution plan or bounds are invalid");
    }
    for (std::size_t step_index = 0; step_index < plan_->steps().size();
         ++step_index) {
      const PlanStep& step = plan_->steps()[step_index];
      if (step.input_demands.size() != step.input_steps.size() ||
          step.output_demand.empty() ||
          !step.output_demand.validate(step.output_descriptor.shape).ok()) {
        throw std::invalid_argument(
            "execution plan Region demand metadata is invalid");
      }
      remaining_dependencies_[step_index] = step.input_steps.size();
      for (std::size_t input_position = 0U;
           input_position < step.input_steps.size(); ++input_position) {
        const std::size_t input_index = step.input_steps[input_position];
        if (input_index >= step_index) {
          throw std::invalid_argument(
              "execution plan input must name an earlier step");
        }
        if (step.input_demands[input_position].empty() ||
            !step.input_demands[input_position]
                 .validate(plan_->steps()[input_index].output_descriptor.shape)
                 .ok()) {
          throw std::invalid_argument(
              "execution plan input Region demand is invalid");
        }
        dependents_[input_index].push_back(step_index);
      }
      if (remaining_dependencies_[step_index] == 0U) {
        ready_.push(step_index);
      }
    }
    for (const auto& output : plan_->outputs()) {
      if (output.first.empty() || output.second >= plan_->steps().size()) {
        throw std::invalid_argument("execution plan output mapping is invalid");
      }
    }
  }

  /**
   * @brief Schedules ready steps and waits for terminal drained state.
   * @return Complete result or first typed failure.
   * @throws std::bad_alloc If final publication allocation fails.
   * @note Failure stops new admission while already started callbacks drain.
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
        lock.unlock();
        submit_attempt(step_index, backend);
        lock.lock();
        observe_external_stop_locked();
      }

      if ((failure_.has_value() || completed_count_ == values_.size()) &&
          in_flight_ == 0U) {
        break;
      }
      state_changed_.wait(lock);
    }

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
      result.values.emplace(output.first, values_[output.second]);
    }
    result.diagnostics = std::move(diagnostics_);
    result.diagnostics.plan_digest = plan_->digest().value;
    result.diagnostics.result_digest = result_digest(result.values);
    result.diagnostics.execute_us = duration_us(started);
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
   * @throws std::bad_alloc If diagnostic allocation fails.
   * @note Caller holds `mutex_`.
   */
  void observe_external_stop_locked() {
    if (failure_.has_value()) {
      return;
    }
    if (cancellation_.cancelled()) {
      failure_ = Status::failure(ErrorCode::Cancelled,
                                 "execution cancellation was requested");
    } else if (!plan_->current()) {
      failure_ = Status::failure(
          ErrorCode::Stale, "execution plan revision is no longer current");
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
      std::function<void()> callback = [self, step_index, backend] {
        self->execute_attempt(step_index, backend);
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
   * @throws Nothing across the worker thread boundary.
   * @note GPU unavailability may resubmit the same step on CPU exactly once.
   */
  void execute_attempt(std::size_t step_index, Backend backend) noexcept {
    try {
      const PlanStep& step = plan_->steps()[step_index];
      std::vector<Value> inputs;
      std::vector<bool> transfer_inputs;
      inputs.reserve(step.input_steps.size());
      transfer_inputs.reserve(step.input_steps.size());
      std::uint64_t transfer_count = 0U;
      std::uint64_t transfer_bytes = 0U;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_.has_value()) {
          finish_abandoned_attempt_locked();
          return;
        }
        for (std::size_t input_position = 0U;
             input_position < step.input_steps.size(); ++input_position) {
          const std::size_t input_index = step.input_steps[input_position];
          if (!completed_[input_index]) {
            finish_failure_locked(Status::failure(
                ErrorCode::Internal,
                "ready step observed an incomplete dependency"));
            return;
          }
          const Status demand_status = validate_input_demand(
              values_[input_index], step.input_demands[input_position]);
          if (!demand_status.ok()) {
            finish_failure_locked(demand_status);
            return;
          }
          inputs.push_back(values_[input_index]);
          const bool requires_transfer =
              value_backends_[input_index] != backend;
          transfer_inputs.push_back(requires_transfer);
          if (requires_transfer) {
            if (transfer_count == std::numeric_limits<std::uint64_t>::max()) {
              finish_failure_locked(
                  Status::failure(ErrorCode::ResourceExhausted,
                                  "transfer count overflows uint64"));
              return;
            }
            ++transfer_count;
            auto sum = checked_add(transfer_bytes,
                                   values_[input_index].bytes().size());
            if (!sum.ok()) {
              finish_failure_locked(sum.status());
              return;
            }
            transfer_bytes = sum.value();
          }
        }
      }

      for (std::size_t input_index = 0U; input_index < inputs.size();
           ++input_index) {
        if (!transfer_inputs[input_index]) {
          continue;
        }
        auto transferred = transfer_value(inputs[input_index]);
        if (!transferred.ok()) {
          finish_failure(transferred.status());
          return;
        }
        inputs[input_index] = transferred.take_value();
      }

      auto lease_result = ledger_->acquire(step.planned_bytes);
      if (!lease_result.ok()) {
        finish_failure(lease_result.status());
        return;
      }
      ResourceLedger::Lease lease = lease_result.take_value();
      const auto started = std::chrono::steady_clock::now();
      Result<Value> invocation_result = operations_->invoke(
          step.operation,
          OperationInvocation{inputs, step.input_demands, step.parameters,
                              backend, cancellation_});
      const std::uint64_t elapsed = duration_us(started);

      bool should_fallback = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.operation_timings.push_back(OperationTiming{
            step.node_id, backend, elapsed, invocation_result.status().code});
        diagnostics_.peak_live_bytes =
            std::max(diagnostics_.peak_live_bytes, lease.live_after_acquire());
        auto count_sum =
            checked_add(diagnostics_.transfer_count, transfer_count);
        auto byte_sum =
            checked_add(diagnostics_.transfer_bytes, transfer_bytes);
        if (!count_sum.ok() || !byte_sum.ok()) {
          lease = ResourceLedger::Lease();
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

      // Release modeled bytes before making the attempt terminal so execute()
      // cannot return while a completed callback still owns accounting state.
      lease = ResourceLedger::Lease();
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
      if (execution_testing::fail_failure_status_construction()) {
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
   * @brief Releases a callback slot abandoned after another failure.
   * @throws Nothing.
   * @note Caller holds `mutex_`.
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
  ResourceLedger* ledger_;
  /** @brief Frozen operation registry retained through every callback. */
  std::shared_ptr<OperationRegistry> operations_;
  /** @brief Immutable caller-owned plan valid until `run` returns. */
  const ExecutionPlan* plan_;
  /** @brief Cooperative cancellation observation. */
  CancellationToken cancellation_;
  /** @brief Positive per-execution callback bound. */
  std::uint32_t maximum_parallelism_;
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
  /** @brief Deterministic smallest-index ready ordering. */
  std::priority_queue<std::size_t, std::vector<std::size_t>,
                      std::greater<std::size_t>>
      ready_;
  /** @brief Number of callbacks/fallback chains not yet terminal. */
  std::uint32_t in_flight_ = 0U;
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

/**
 * @brief Implements one bounded local execution Run.
 * @copydetails ExecutionContext::execute
 */
Result<ExecutionResult> ExecutionContext::execute(
    const ExecutionPlan& plan, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
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
  const std::uint32_t parallelism = options.maximum_parallelism == 0U
                                        ? impl_->cpu_worker_count
                                        : options.maximum_parallelism;
  try {
    auto coordinator = std::make_shared<ExecutionRun>(
        &impl_->cpu_pool, impl_->gpu_pool.get(), &impl_->waiting_admission,
        &impl_->ledger, impl_->operation_registry, &plan, cancellation,
        parallelism);
    return coordinator->run();
  } catch (const std::invalid_argument& error) {
    return Result<ExecutionResult>(
        Status::failure(ErrorCode::InvalidArgument, error.what()));
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
