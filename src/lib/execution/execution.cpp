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
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "execution/dependency_records.hpp"
#include "execution/disk_cache.hpp"
#include "execution/memory_budget.hpp"
#include "execution/native_gpu.hpp"
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

/** @brief Creates production native resources; scheduler fixtures use fake
 * lanes. */
std::shared_ptr<gpu_internal::Device> execution_device(bool enabled) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  // The noninstalled scheduler test kernel controls callback completion without
  // requiring hardware. Native tests and installed consumers use the product.
  static_cast<void>(enabled);
  return {};
#else
  return enabled ? gpu_internal::Device::create() : nullptr;
#endif
}
/** @brief Keys a Run-local upload by retained allocation and complete view. */
std::string upload_view_key(const Value& value) {
  std::ostringstream key;
  key << value.storage().get() << ':' << value.layout().byte_offset << ':'
      << static_cast<unsigned>(value.descriptor().element_type);
  key << ':' << value.descriptor().shape.size();
  for (auto n : value.descriptor().shape)
    key << ':' << n;
  key << ':' << value.layout().origin.size();
  for (auto n : value.layout().origin)
    key << ':' << n;
  key << ':' << value.layout().byte_strides.size();
  for (auto n : value.layout().byte_strides)
    key << ':' << n;
  key << ':' << value.region().rank();
  for (auto d : value.region().dimensions())
    key << ':' << d.offset << ':' << d.extent;
  return key.str();
}

/** @brief Content identity for an immutable packed upload, independent of
 * owners. */
Result<std::string> upload_content_key(const Value& value,
                                       const std::string& device,
                                       const CancellationToken& cancellation) {
  content_internal::Sha256 hash;
  hash.text("photospider.native-upload.v1");
  hash.text(device);
  hash.integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash.integer(value.descriptor().shape.size());
  for (auto n : value.descriptor().shape)
    hash.integer(n);
  for (auto d : value.region().dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  hash.integer(value.facets().size());
  for (const auto& f : value.facets()) {
    hash.text(f.key);
    hash.integer(f.version);
    hash.integer(f.payload.size());
    hash.bytes(f.payload.data(), f.payload.size());
  }
  auto count = value.region().element_count();
  if (!count.ok())
    return Result<std::string>(count.status());
  const auto width = Value::element_size(value.descriptor().element_type);
  std::vector<std::uint64_t> coordinate(value.region().rank());
  for (std::uint64_t i = 0; i < count.value(); ++i) {
    if ((i & 1023) == 0 && cancellation.cancelled())
      return Result<std::string>(Status::failure(
          ErrorCode::Cancelled, "native upload identity cancelled"));
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
        native_device(execution_device(requested.gpu_enabled)),
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        gpu_available(requested.gpu_enabled),
#else
        gpu_available(native_device && native_device->available()),
#endif
        maximum_waiting_callbacks(requested.maximum_queued_tasks),
        operation_registry(std::move(operations)),
        budget(std::make_shared<MemoryBudget>(requested.maximum_live_bytes)),
        waiting_admission(maximum_waiting_callbacks),
        cpu_pool(cpu_worker_count, Backend::Cpu) {
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
  std::shared_ptr<gpu_internal::Device> native_device;
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
  std::shared_ptr<execution_internal::DemandCoordinator> demands;
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
      binding.snapshot =
          std::make_shared<const InputSnapshot>(*binding.snapshot);
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
        auto status = snapshot->read(r, bytes, size,
                                     SnapshotAccessOptions{UINT64_MAX, token});
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
  /** @brief Drives unresolved dependency records on the calling coordinator.
   * @note Uses the context's existing worker/admission/allocator owners. No
   * worker waits for an upstream record or holds unused stage reservations.
   */
  static Result<ExecutionResult> run_dependencies(
      ThreadPool* pool, WaitingAdmission* admission,
      const std::shared_ptr<MemoryBudget>& budget,
      const std::shared_ptr<OperationRegistry>& operations,
      const std::function<Result<Value>(const std::string&,
                                        const OperationInvocation&)>& invoke,
      const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
      const CancellationToken& cancellation, const ExecutionOptions& options,
      const ExecutionSink* sink,
      const std::function<void(std::uint64_t)>& reclaim,
      const DemandQuery* requested = nullptr,
      std::map<std::string, ValueFragments>* fragment_outputs = nullptr,
      const std::string& snapshot_identity = {}) {
    const auto started = std::chrono::steady_clock::now();
    const auto stop = [&] { return binding_stop(plan, cancellation); };
    const auto fail = [&](Status status) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        status.code = code;
        status.message.clear();
      }
      return Result<ExecutionResult>(std::move(status));
    };
    auto limits = options.dependencies.sets;
    limits.cancellation = cancellation;
    auto observation =
        std::make_shared<execution_internal::MemoryObservation>();
    ExecutionResult result;
    auto& diagnostics = result.diagnostics;
    diagnostics.plan_digest = plan.digest().value;
    std::set<const CpuStorage*> external;
    for (const auto& binding : bindings)
      if (binding.value.valid() &&
          external.insert(binding.value.storage().get()).second) {
        auto bytes = checked_add(diagnostics.retained_input_bytes,
                                 binding.value.storage()->capacity());
        if (!bytes.ok())
          return fail(bytes.status());
        diagnostics.retained_input_bytes = bytes.value();
      }
    struct Seal {
      std::shared_ptr<MemoryReservation> reservation;
      ~Seal() {
        if (reservation)
          reservation->seal();
      }
    };
    const auto reserve = [&](std::uint64_t bytes) {
      if (reclaim)
        reclaim(bytes);
      // Deliberately omit the blocking stop callback. All already-live owners
      // remain charged; an impossible minimum stage fails finitely.
      return budget->reserve(bytes, {}, observation);
    };
    std::uint64_t work = options.maximum_dependency_work;
    const auto consume = [&](std::uint64_t count = 1) -> Status {
      if (stop() != ErrorCode::Ok)
        return Status{stop(), {}};
      if (count > work)
        return Status::failure(ErrorCode::ResourceExhausted,
                               "dependency Run work limit");
      work -= count;
      return Status::success();
    };
    static std::atomic<std::uint64_t> sequence{1};
    auto nonce = sequence.load();
    do {
      if (nonce == UINT64_MAX)
        return fail(Status::failure(ErrorCode::ResourceExhausted,
                                    "dependency Run identities exhausted"));
    } while (!sequence.compare_exchange_weak(nonce, nonce + 1));
    const auto identity = snapshot_identity.empty()
                              ? "run-" + std::to_string(nonce)
                              : snapshot_identity;
    execution_internal::DependencyRecords records(plan, identity, limits);
    if (!records.status().ok())
      return fail(records.status());
    const auto metadata = [&](const PlanInput& input) -> OperationMetadata {
      if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
        const auto& step = plan.steps().at(producer->step_index);
        return {step.output_descriptor, step.output_facets};
      }
      const auto& declaration = plan.input_declarations().at(
          std::get<PlanWorkflowInput>(input).declaration_index);
      return {declaration.descriptor, declaration.facets};
    };
    struct Frame {
      enum class State {
        Initial,
        Expand,
        Waiting,
        Poll,
        Legacy,
        Complete
      } state = State::Initial;
      PlanInput target;
      Footprint outputs;
      bool unit = false, terminal_allowed = false;
      std::vector<Footprint> parts;
      std::size_t next = 0;
      std::vector<Value> values;
      std::vector<ValueFragments> ready;
      std::shared_ptr<DependencySession> session;
      std::optional<ValueFragments> complete;
      Frame(PlanInput target, Footprint outputs, bool unit = false,
            bool terminal = false)
          : target(std::move(target)),
            outputs(std::move(outputs)),
            unit(unit),
            terminal_allowed(terminal) {}
    };
    // This explicit stack is a deterministic ready order. Parent frames in
    // Waiting retain state and actual input owners, but no active reservation.
    std::vector<Frame> frames;
    struct Query {
      std::size_t step;
      std::string name;
      Footprint samples;
      bool boundary;
    };
    DemandQuery wanted;
    std::uint64_t query_entries = 0;
    if (requested) {
      for (const auto& item : *requested) {
        const auto found = plan.outputs().find(item.first);
        if (found == plan.outputs().end() || !item.second.valid())
          return fail(Status{ErrorCode::InvalidArgument,
                             "unknown or invalid named query"});
        const auto weight = 1 + item.second.boxes().size();
        if (weight > limits.maximum_boxes ||
            query_entries > limits.maximum_boxes - weight)
          return fail(
              Status{ErrorCode::ResourceExhausted, "query metadata limit"});
        query_entries += weight;
        const auto& step = plan.steps().at(found->second);
        if (item.second.shape() != step.output_descriptor.shape)
          return fail(
              Status{ErrorCode::TypeMismatch, "query output domain mismatch"});
        auto scope = Footprint::from_regions(
            step.output_descriptor.shape,
            {plan.output_regions().at(item.first)}, limits);
        if (!scope.ok())
          return fail(scope.status());
        auto outside = item.second.subtract(scope.value(), limits);
        if (!outside.ok())
          return fail(outside.status());
        if (!outside.value().empty())
          return fail(Status{ErrorCode::InvalidArgument,
                             "query exceeds compiled output region"});
        for (const auto& box : item.second.boxes())
          if (!input_internal::complete_image_channels(step.output_descriptor,
                                                       step.output_facets, box))
            return fail(Status{ErrorCode::InvalidArgument,
                               "query requires complete image channels"});
        wanted.emplace(item.first, item.second);
      }
    } else {
      for (const auto& named : plan.outputs()) {
        auto samples = Footprint::from_regions(
            plan.steps()[named.second].output_descriptor.shape,
            {plan.output_regions().at(named.first)}, limits);
        if (!samples.ok())
          return fail(samples.status());
        wanted.emplace(named.first, samples.take_value());
      }
    }
    const bool nonempty =
        std::any_of(wanted.begin(), wanted.end(),
                    [](const auto& q) { return !q.second.empty(); });
    std::vector<Query> queries;
    std::map<std::size_t, ValueFragments> whole_records;
    if (nonempty) {
      for (std::size_t i = 0; i < plan.steps().size(); ++i) {
        if (plan.steps()[i].whole_boundary &&
            plan.steps()[i].traits.observation_kind !=
                ObservationKind::RequestRecord) {
          auto all =
              Footprint::all(plan.steps()[i].output_descriptor.shape, limits);
          if (!all.ok())
            return fail(all.status());
          queries.push_back({i, {}, all.take_value(), true});
        }
      }
    }
    for (const auto& named : wanted) {
      const auto step_index = plan.outputs().at(named.first);
      const auto& step = plan.steps()[step_index];
      if (!sink ||
          step.traits.observation_kind == ObservationKind::RequestRecord) {
        queries.push_back({step_index, named.first, named.second, false});
        continue;
      }
      for (const auto& region : named.second.boxes()) {
        const auto rank = region.rank();
        std::vector<std::uint64_t> geometry(rank, 1), cursor;
        const bool image = std::any_of(
            step.output_facets.begin(), step.output_facets.end(),
            [](const auto& facet) { return facet.key == "photospider.image"; });
        if (image) {
          geometry[0] = plan.tile_height();
          geometry[1] = plan.tile_width();
          geometry[2] = step.output_descriptor.shape[2];
        } else {
          geometry[rank - 1] = plan.tile_width();
          if (rank > 1)
            geometry[rank - 2] = plan.tile_height();
        }
        for (const auto& dimension : region.dimensions())
          cursor.push_back(dimension.offset);
        for (;;) {
          auto status = consume();
          if (!status.ok())
            return fail(status);
          std::vector<RegionDimension> dimensions;
          for (std::size_t axis = 0; axis < rank; ++axis) {
            const auto& extent = region.dimensions()[axis];
            dimensions.push_back(
                {cursor[axis],
                 std::min(geometry[axis],
                          extent.offset + extent.extent - cursor[axis])});
          }
          auto tile = Footprint::from_regions(step.output_descriptor.shape,
                                              {Region(dimensions)}, limits);
          if (!tile.ok())
            return fail(tile.status());
          queries.push_back(
              {step_index, named.first, tile.take_value(), false});
          std::size_t axis = rank;
          while (axis) {
            --axis;
            cursor[axis] += dimensions[axis].extent;
            if (cursor[axis] < region.dimensions()[axis].offset +
                                   region.dimensions()[axis].extent)
              break;
            cursor[axis] = region.dimensions()[axis].offset;
          }
          if (axis == 0 && cursor[0] == region.dimensions()[0].offset)
            break;
        }
      }
    }
    for (const auto& named : queries) {
      frames.emplace_back(PlanStepInput{named.step}, named.samples, false,
                          !named.boundary);
      std::optional<ValueFragments> returned;
      while (!frames.empty()) {
        auto status = consume();
        if (!status.ok())
          return fail(status);
        auto& frame = frames.back();
        const auto output = metadata(frame.target);
        if (returned) {
          if (frame.state == Frame::State::Expand) {
            frame.values.insert(frame.values.end(),
                                returned->fragments().begin(),
                                returned->fragments().end());
          } else if (frame.state == Frame::State::Waiting) {
            frame.ready.push_back(std::move(*returned));
          } else {
            return fail(Status::failure(
                ErrorCode::Internal,
                "dependency child returned to nonwaiting record"));
          }
          returned.reset();
        }
        if (frame.state == Frame::State::Complete) {
          if (const auto* producer =
                  std::get_if<PlanStepInput>(&frame.target)) {
            const auto& step = plan.steps()[producer->step_index];
            if (step.whole_boundary && frame.unit &&
                step.traits.observation_kind !=
                    ObservationKind::RequestRecord) {
              auto whole = Footprint::all(step.output_descriptor.shape, limits);
              if (!whole.ok())
                return fail(whole.status());
              if (frame.complete->coverage() != whole.value())
                return fail(
                    Status::failure(ErrorCode::Internal,
                                    "incomplete Whole record publication"));
              whole_records.emplace(producer->step_index, *frame.complete);
            }
          }
          returned = std::move(frame.complete);
          frames.pop_back();
          continue;
        }
        if (frame.outputs.empty()) {
          if (const auto* producer =
                  std::get_if<PlanStepInput>(&frame.target)) {
            if (plan.steps()[producer->step_index].traits.observation_kind ==
                    ObservationKind::RequestRecord &&
                !frame.terminal_allowed)
              return fail(Status{ErrorCode::InvalidArgument,
                                 "RequestRecord cannot supply a DAG input"});
            status = records.append_empty(producer->step_index, frame.outputs);
            if (!status.ok())
              return fail(status);
          }
          auto empty = ValueFragments::create(output.descriptor, output.facets,
                                              frame.outputs, {}, limits);
          if (!empty.ok())
            return fail(empty.status());
          frame.complete = empty.take_value();
          frame.state = Frame::State::Complete;
          continue;
        }
        if (const auto* input = std::get_if<PlanWorkflowInput>(&frame.target)) {
          const auto& binding = bindings.at(input->declaration_index);
          if (binding.value.valid()) {
            auto view =
                ValueFragments::create(output.descriptor, output.facets,
                                       frame.outputs, {binding.value}, limits);
            if (!view.ok())
              return fail(view.status());
            frame.complete = view.take_value();
            frame.state = Frame::State::Complete;
            continue;
          }
          for (const auto& box : frame.outputs.boxes()) {
            status = consume();
            if (!status.ok())
              return fail(status);
            auto bytes = region_bytes(output.descriptor, box);
            if (!bytes.ok())
              return fail(bytes.status());
            auto capacity =
                checked_add(bytes.value(), binding.source->workspace_bytes);
            if (!capacity.ok())
              return fail(capacity.status());
            auto reserved = reserve(capacity.value());
            if (!reserved.ok())
              return fail(reserved.status());
            Seal seal{reserved.take_value()};
            auto allocation = MutableValue::allocate(
                output.descriptor, box, seal.reservation->allocator());
            if (!allocation.ok())
              return fail(allocation.status());
            auto writer = allocation.take_value();
            auto read = dependency_stage<Region>(pool, admission, [&] {
              if (stop() != ErrorCode::Ok)
                return Result<Region>(Status{stop(), {}});
              return binding.source->read(
                  box, writer.data(), writer.size(),
                  seal.reservation->allocator(binding.source->workspace_bytes),
                  cancellation);
            });
            if (!read.ok())
              return fail(read.status());
            if (!same_region(read.value(), box))
              return fail(
                  Status::failure(ErrorCode::TypeMismatch,
                                  "source returned different coverage"));
            if (stop() != ErrorCode::Ok)
              return fail(Status{stop(), {}});
            auto published = std::move(writer).publish(output.facets);
            if (!published.ok())
              return fail(published.status());
            frame.values.push_back(published.take_value());
            ++diagnostics.source_read_count;
            auto total =
                checked_add(diagnostics.source_read_bytes, bytes.value());
            if (!total.ok())
              return fail(total.status());
            diagnostics.source_read_bytes = total.value();
            diagnostics.peak_active_tasks = 1;
          }
          auto fragments =
              ValueFragments::create(output.descriptor, output.facets,
                                     frame.outputs, frame.values, limits);
          if (!fragments.ok())
            return fail(fragments.status());
          frame.complete = fragments.take_value();
          frame.values.clear();
          frame.state = Frame::State::Complete;
          continue;
        }
        const auto step_index =
            std::get<PlanStepInput>(frame.target).step_index;
        const auto& step = plan.steps().at(step_index);
        const auto retained = whole_records.find(step_index);
        if (frame.state == Frame::State::Initial &&
            retained != whole_records.end()) {
          auto restricted = retained->second.restrict(frame.outputs, limits);
          if (!restricted.ok())
            return fail(restricted.status());
          frame.complete = restricted.take_value();
          frame.state = Frame::State::Complete;
          continue;
        }
        const bool terminal =
            step.traits.observation_kind == ObservationKind::RequestRecord;
        if (terminal && !frame.terminal_allowed)
          return fail(
              Status::failure(ErrorCode::InvalidArgument,
                              "RequestRecord cannot supply a DAG input"));
        if (!terminal && !step.effective_atomic)
          return fail(
              Status::failure(ErrorCode::InvalidArgument,
                              "dependency producer has non-atomic ancestry"));
        if (step.backend != Backend::Cpu)
          return fail(Status::failure(
              ErrorCode::BackendUnavailable,
              "native dependency stage requires fragment access plan"));
        if (frame.state == Frame::State::Initial && !frame.unit && !terminal) {
          if (step.traits.dependency_version == 0 && step.whole_boundary) {
            // The legacy Whole contract observes global validation for every
            // request. Preserve that actual dependency; never relabel a tile.
            auto whole = Footprint::all(output.descriptor.shape, limits);
            if (!whole.ok())
              return fail(whole.status());
            frame.parts.push_back(whole.take_value());
          } else {
            auto observations =
                operation_observations(output, frame.outputs, limits);
            if (!observations.ok())
              return fail(observations.status());
            status = observations.value().visit(
                [&](const auto& coordinate) {
                  auto charged = consume();
                  if (!charged.ok())
                    return charged;
                  std::vector<RegionDimension> dimensions;
                  for (auto c : coordinate)
                    dimensions.push_back({c, 1});
                  auto atom = Footprint::from_regions(
                      observations.value().shape(),
                      {Region(std::move(dimensions))}, limits);
                  if (!atom.ok())
                    return atom.status();
                  auto samples =
                      observation_samples(output, atom.value(), limits);
                  if (!samples.ok())
                    return samples.status();
                  frame.parts.push_back(samples.take_value());
                  return Status::success();
                },
                work, cancellation);
            if (!status.ok())
              return fail(status);
          }
          frame.state = Frame::State::Expand;
        }
        if (frame.state == Frame::State::Expand) {
          if (frame.next < frame.parts.size()) {
            const auto query = frame.parts[frame.next++];
            const auto target = frame.target;
            frames.emplace_back(target, query, true);
            continue;
          }
          auto fragments =
              ValueFragments::create(output.descriptor, output.facets,
                                     frame.outputs, frame.values, limits);
          if (!fragments.ok())
            return fail(fragments.status());
          frame.complete = fragments.take_value();
          frame.values.clear();
          frame.state = Frame::State::Complete;
          continue;
        }
        if (frame.state == Frame::State::Initial) {
          frame.parts.clear();
          frame.next = 0;
          if (step.traits.dependency_version == 1) {
            DependencyRequest request;
            for (const auto& input : step.inputs)
              request.inputs.push_back(metadata(input));
            request.parameters = step.parameters;
            request.outputs = frame.outputs;
            request.snapshot_identity = identity;
            request.cancellation = cancellation;
            request.limits = options.dependencies;
            request.limits.maximum_work =
                std::min(request.limits.maximum_work, work);
            auto reserved =
                reserve(std::min(step.traits.continuation_bytes,
                                 options.dependencies.maximum_state_bytes));
            if (!reserved.ok())
              return fail(reserved.status());
            Seal seal{reserved.take_value()};
            auto session = dependency_stage<std::shared_ptr<DependencySession>>(
                pool, admission, [&] {
                  if (stop() != ErrorCode::Ok)
                    return Result<std::shared_ptr<DependencySession>>(
                        Status{stop(), {}});
                  return operations->start_dependency(
                      step.operation, std::move(request),
                      seal.reservation->allocator());
                });
            if (!session.ok())
              return fail(session.status());
            frame.session = session.take_value();
            status = consume(frame.session->consumed_work());
            if (!status.ok())
              return fail(status);
            frame.state = Frame::State::Poll;
          } else {
            if (frame.outputs.boxes().size() != 1)
              return fail(Status::failure(ErrorCode::InvalidArgument,
                                          "synchronous terminal requires a "
                                          "rectangular complete request"));
            for (std::size_t port = 0; port < step.inputs.size(); ++port) {
              auto demand = input_internal::derive_input_demand(
                  step.traits, frame.outputs.boxes()[0],
                  output.descriptor.shape,
                  metadata(step.inputs[port]).descriptor.shape,
                  step.traits.input_schema[port].kind);
              if (!demand.ok())
                return fail(demand.status());
              auto footprint = Footprint::from_regions(
                  metadata(step.inputs[port]).descriptor.shape,
                  {demand.take_value()}, limits);
              if (!footprint.ok())
                return fail(footprint.status());
              frame.parts.push_back(footprint.take_value());
            }
            frame.state = Frame::State::Waiting;
          }
        }
        if (frame.state == Frame::State::Waiting) {
          if (frame.next < frame.parts.size()) {
            const auto port = frame.next++;
            const auto query = frame.parts[port];
            const auto target = step.inputs[port];
            frames.emplace_back(target, query);
            continue;
          }
          if (frame.session) {
            status = frame.session->supply(std::move(frame.ready), identity);
            if (!status.ok())
              return fail(status);
            frame.state = Frame::State::Poll;
          } else {
            frame.state = Frame::State::Legacy;
          }
        }
        if (frame.state == Frame::State::Poll ||
            frame.state == Frame::State::Legacy) {
          auto elements = frame.outputs.element_count();
          const auto width =
              Value::element_size(output.descriptor.element_type);
          if (!elements.ok() || elements.value() > UINT64_MAX / width)
            return fail(Status::failure(ErrorCode::ResourceExhausted,
                                        "dependency output bytes overflow"));
          auto capacity = checked_add(
              std::max(step.traits.estimated_bytes, elements.value() * width),
              step.traits.workspace_bytes);
          if (!capacity.ok())
            return fail(capacity.status());
          // The pending input footprint remains available after supply moved
          // ready owners into the session. It describes exact stage scratch.
          for (std::size_t port = 0; port < frame.parts.size(); ++port) {
            auto count = frame.parts[port].element_count();
            const auto scale =
                Value::element_size(
                    metadata(step.inputs[port]).descriptor.element_type) *
                (step.traits.workspace_input_multiplier +
                 (frame.session ? 0 : 1));
            if (scale &&
                (!count.ok() ||
                 count.value() > (UINT64_MAX - capacity.value()) / scale))
              return fail(
                  Status::failure(ErrorCode::ResourceExhausted,
                                  "dependency stage input bytes overflow"));
            if (scale)
              capacity = Result<std::uint64_t>(capacity.value() +
                                               count.value() * scale);
          }
          auto reserved = reserve(capacity.value());
          if (!reserved.ok())
            return fail(reserved.status());
          Seal seal{reserved.take_value()};
          std::uint64_t callback_us = 0;
          if (frame.session) {
            const auto charged_before = frame.session->consumed_work();
            auto progress =
                dependency_stage<DependencyProgress>(pool, admission, [&] {
                  if (stop() != ErrorCode::Ok)
                    return Result<DependencyProgress>(Status{stop(), {}});
                  const auto callback_started =
                      std::chrono::steady_clock::now();
                  auto result =
                      frame.session->poll(seal.reservation->allocator());
                  callback_us = duration_us(callback_started);
                  return result;
                });
            status = consume(frame.session->consumed_work() - charged_before);
            if (!status.ok())
              return fail(status);
            if (!progress.ok())
              return fail(progress.status());
            if (stop() != ErrorCode::Ok)
              return fail(Status{stop(), {}});
            auto event = progress.take_value();
            if (auto* complete = std::get_if<DependencyResult>(&event)) {
              status = records.append(step_index, *complete);
              if (!status.ok())
                return fail(status);
              std::vector<Value> owned;
              const auto allocator = seal.reservation->allocator();
              for (const auto& fragment : complete->value.fragments()) {
                if (allocator.owns(*fragment.storage()) ||
                    external.count(fragment.storage().get())) {
                  owned.push_back(fragment);
                } else {
                  auto imported = transfer_value(fragment, allocator, true);
                  if (!imported.ok())
                    return fail(imported.status());
                  owned.push_back(imported.take_value());
                }
              }
              auto fragments =
                  ValueFragments::create(output.descriptor, output.facets,
                                         frame.outputs, owned, limits);
              if (!fragments.ok())
                return fail(fragments.status());
              frame.complete = fragments.take_value();
              frame.session.reset();
              frame.state = Frame::State::Complete;
            } else {
              auto pending = frame.session->pending_reads();
              if (!pending.ok())
                return fail(pending.status());
              frame.parts.clear();
              frame.ready.clear();
              frame.next = 0;
              for (const auto& input : step.inputs) {
                auto none =
                    Footprint::none(metadata(input).descriptor.shape, limits);
                if (!none.ok())
                  return fail(none.status());
                frame.parts.push_back(none.take_value());
              }
              for (const auto& need : pending.value()) {
                auto united =
                    frame.parts.at(need.port).unite(need.samples, limits);
                if (!united.ok())
                  return fail(united.status());
                frame.parts[need.port] = united.take_value();
              }
              frame.state = Frame::State::Waiting;
            }
          } else {
            auto value = dependency_stage<Value>(
                pool, admission, [&]() -> Result<Value> {
                  if (stop() != ErrorCode::Ok)
                    return Result<Value>(Status{stop(), {}});
                  std::vector<Value> inputs;
                  std::vector<Region> demands;
                  for (std::size_t port = 0; port < frame.parts.size();
                       ++port) {
                    const auto& box = frame.parts[port].boxes().at(0);
                    auto dense = frame.ready[port].collect(
                        box, seal.reservation->allocator(), limits);
                    if (!dense.ok())
                      return Result<Value>(dense.status());
                    inputs.push_back(dense.take_value());
                    demands.push_back(box);
                  }
                  OperationInvocation call{inputs,
                                           demands,
                                           step.parameters,
                                           Backend::Cpu,
                                           cancellation,
                                           frame.outputs.boxes()[0],
                                           seal.reservation->allocator()};
                  const auto callback_started =
                      std::chrono::steady_clock::now();
                  auto computed = invoke(step.operation, call);
                  callback_us = duration_us(callback_started);
                  if (!computed.ok())
                    return computed;
                  if (!call.allocator.owns(*computed.value().storage()))
                    return transfer_value(computed.value(), call.allocator,
                                          true);
                  return computed;
                });
            if (!value.ok())
              return fail(value.status());
            status =
                records.append_legacy(step_index, frame.outputs, frame.parts);
            if (!status.ok())
              return fail(status);
            auto fragments = ValueFragments::create(
                output.descriptor, output.facets, frame.outputs,
                {value.take_value()}, limits);
            if (!fragments.ok())
              return fail(fragments.status());
            frame.complete = fragments.take_value();
            frame.ready.clear();
            frame.state = Frame::State::Complete;
          }
          diagnostics.peak_active_tasks = 1;
          diagnostics.selected_backends[step.node_id] = Backend::Cpu;
          diagnostics.operation_timings.push_back(OperationTiming{
              step.node_id, Backend::Cpu, callback_us, ErrorCode::Ok, 1,
              frame.state == Frame::State::Complete ? elements.value() : 0});
        }
      }
      if (!returned)
        return fail(Status::failure(ErrorCode::Internal,
                                    "dependency output record missing"));
      if (named.boundary)
        continue;
      auto recorded = records.output(named.name, named.step, named.samples);
      if (!recorded.ok())
        return fail(recorded);
      if (fragment_outputs) {
        const auto& descriptor = returned->descriptor();
        std::vector<std::uint64_t> geometry(descriptor.shape.size(), 1);
        const bool image = std::any_of(
            returned->facets().begin(), returned->facets().end(),
            [](const auto& f) { return f.key == "photospider.image"; });
        if (image) {
          geometry[0] = plan.tile_height();
          geometry[1] = plan.tile_width();
          geometry[2] = descriptor.shape[2];
        } else {
          geometry.back() = plan.tile_width();
          if (geometry.size() > 1)
            geometry[geometry.size() - 2] = plan.tile_height();
        }
        auto tiles = named.samples.tile_cover(geometry, limits);
        if (!tiles.ok())
          return fail(tiles.status());
        auto count = tiles.value().element_count();
        if (!count.ok())
          return fail(count.status());
        auto total = checked_add(diagnostics.tile_count, count.value());
        if (!total.ok())
          return fail(total.status());
        diagnostics.tile_count = total.value();
        fragment_outputs->emplace(named.name, std::move(*returned));
        continue;
      }
      if (named.samples.boxes().size() != 1)
        return fail(Status{ErrorCode::InvalidArgument,
                           "dense output requires one nonempty rectangle"});
      const auto& region = named.samples.boxes()[0];
      auto bytes = region_bytes(returned->descriptor(), region);
      if (!bytes.ok())
        return fail(bytes.status());
      auto reserved = reserve(bytes.value());
      if (!reserved.ok())
        return fail(reserved.status());
      Seal seal{reserved.take_value()};
      auto value =
          returned->collect(region, seal.reservation->allocator(), limits);
      if (!value.ok())
        return fail(value.status());
      returned.reset();
      if (stop() != ErrorCode::Ok)
        return fail(Status{stop(), {}});
      if (sink) {
        auto status = (*sink)(named.name, ValueView(value.value()));
        if (!status.ok())
          return fail(status);
      } else {
        result.values.emplace(named.name, value.take_value());
      }
      ++diagnostics.tile_count;
    }
    const auto peaks = budget->peaks(observation);
    diagnostics.peak_live_bytes = peaks.first;
    diagnostics.planned_peak_bytes = peaks.second;
    diagnostics.execute_us = duration_us(started);
    result.dependencies = std::move(records).finish();
    if (!sink && !fragment_outputs)
      diagnostics.result_digest = result_digest(result.values);
    if (stop() != ErrorCode::Ok)
      return fail(Status{stop(), {}});
    return Result<ExecutionResult>(std::move(result));
  }

 private:
  /** @brief Runs exactly one ready stage and drains its callback ownership. */
  template <class T>
  static Result<T> dependency_stage(ThreadPool* pool,
                                    WaitingAdmission* admission,
                                    std::function<Result<T>()> work) {
    struct Completion {
      std::promise<Result<T>> promise;
      Result<T> result{Status{ErrorCode::Internal, {}}};
      std::function<Result<T>()> work;
    };
    auto completion = std::make_shared<Completion>();
    completion->work = std::move(work);
    auto future = completion->promise.get_future();
    auto slot = admission->try_acquire();
    if (!slot)
      return Result<T>(Status::failure(ErrorCode::ResourceExhausted,
                                       "dependency waiting queue exhausted"));
    QueuedCallback callback{
        [completion] {
          // The callable's owners retire before the completion notification.
          auto work = std::move(completion->work);
          try {
            completion->result = work();
          } catch (const std::bad_alloc&) {
            completion->result =
                Result<T>(Status{ErrorCode::ResourceExhausted, {}});
          } catch (...) {
            completion->result =
                Result<T>(Status{ErrorCode::OperationFailed, {}});
          }
        },
        std::move(*slot),
        [completion] {
          completion->promise.set_value(std::move(completion->result));
        }};
    if (!pool->submit(std::move(callback)))
      return Result<T>(Status::failure(ErrorCode::ResourceExhausted,
                                       "dependency callback queue stopped"));
    return future.get();
  }

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
      std::function<void(std::size_t, const Value&, Backend)> retain = {},
      std::shared_ptr<gpu_internal::Device> native_device = {},
      execution_internal::ResultCache* native_cache = nullptr,
      std::uint64_t cache_epoch = 0)
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
        native_device_(std::move(native_device)),
        native_cache_(native_cache),
        cache_epoch_(cache_epoch),
        fallback_taint_(plan->steps().size(), false),
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
          fallback_taint_[i] = value_backends_[i] != plan_->steps()[i].backend;
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
      if (native_device_ && value_backends_[output.second] == Backend::Gpu)
        ++diagnostics_.host_access_count;
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
      if (backend == Backend::Gpu &&
          (!gpu_pool_ || (native_device_ && !native_device_->available()))) {
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
      std::uint64_t upload_hits = 0;
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
          const bool requires_transfer =
              native_device_ ? backend == Backend::Gpu &&
                                   !native_device_->owns(*value->storage())
                             : source_backend != backend;
          if (native_device_ && backend == Backend::Cpu &&
              source_backend == Backend::Gpu)
            ++diagnostics_.host_access_count;
          transfer_inputs.push_back(requires_transfer);
          if (requires_transfer && !native_device_) {
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

      auto allocator = reservation_->allocator();
      auto callback_allocator = reservation_->allocator(step.planned_bytes);
      if (native_device_ && backend == Backend::Gpu) {
        allocator = native_device_->allocator(allocator);
        callback_allocator = native_device_->allocator(callback_allocator);
      }
      for (std::size_t input_index = 0U; input_index < inputs.size();
           ++input_index) {
        if (!transfer_inputs[input_index]) {
          continue;
        }
        const auto key = native_device_ ? upload_view_key(inputs[input_index])
                                        : std::string{};
        if (native_device_) {
          const auto found = native_inputs_.find(key);
          if (found != native_inputs_.end()) {
            // Reuse bytes, not semantic metadata: Values sharing an allocation
            // and layout may carry different valid facets in this same Run.
            const auto& uploaded = found->second.second;
            auto reused = Value::from_storage(
                uploaded.descriptor(), uploaded.region(), uploaded.layout(),
                uploaded.storage(), inputs[input_index].facets());
            if (!reused.ok()) {
              finish_failure(reused.status());
              return;
            }
            inputs[input_index] = reused.take_value();
            continue;
          }
        }
        std::string retained_key;
        if (native_device_ && native_cache_) {
          auto identity = upload_content_key(
              inputs[input_index], native_device_->identity(), cancellation_);
          if (!identity.ok()) {
            finish_failure(identity.status());
            return;
          }
          retained_key = identity.take_value();
          auto retained = native_cache_->get(retained_key);
          if (retained.valid() && native_device_->owns(*retained.storage())) {
            inputs[input_index] = std::move(retained);
            ++upload_hits;
            continue;
          }
        }
        auto transferred = transfer_value(inputs[input_index], allocator,
                                          native_device_ ? true : regional_);
        if (!transferred.ok()) {
          finish_failure(transferred.status());
          return;
        }
        if (native_device_) {
          const auto action = std::find_if(
              plan_->physical_steps().begin(), plan_->physical_steps().end(),
              [&](const PhysicalStep& p) {
                return p.kind == PhysicalStepKind::Upload &&
                       p.step_index == step_index &&
                       p.input_index == input_index;
              });
          // Missing planned uploads are permitted only for a predecessor that
          // was planned on GPU and actually fell back before this invocation.
          if (action != plan_->physical_steps().end()) {
            if (transferred.value().bytes().size() != action->packed_bytes ||
                transferred.value().storage()->capacity() >
                    action->allocation_bytes) {
              finish_failure(Status::failure(
                  ErrorCode::Internal,
                  "native upload contradicts physical access bounds"));
              return;
            }
          } else if (!std::holds_alternative<PlanStepInput>(
                         step.inputs[input_index]) ||
                     plan_->steps()[std::get<PlanStepInput>(
                                        step.inputs[input_index])
                                        .step_index]
                             .backend != Backend::Gpu) {
            finish_failure(
                Status::failure(ErrorCode::Internal,
                                "native upload has no physical plan action"));
            return;
          }
          ++transfer_count;
          auto total =
              checked_add(transfer_bytes, transferred.value().bytes().size());
          if (!total.ok()) {
            finish_failure(total.status());
            return;
          }
          transfer_bytes = total.value();
          native_inputs_.emplace(key,
                                 std::make_pair(inputs[input_index].storage(),
                                                transferred.value()));
        }
        if (native_cache_ && native_device_ && !cancellation_.cancelled() &&
            plan_->current())
          native_cache_->put(retained_key, transferred.value(), cache_epoch_,
                             true);
        inputs[input_index] = transferred.take_value();
      }

      // Validate bounded computed values after every predecessor/cache path,
      // before the consuming callback; direct bindings retain preflight errors.
      for (std::size_t port = 0; port < step.inputs.size(); ++port) {
        if (!std::holds_alternative<PlanStepInput>(step.inputs[port]) ||
            (step.traits.input_schema[port].kind !=
                 OperationPortKind::Float32Mask &&
             step.traits.input_schema[port].kind !=
                 OperationPortKind::Float32Scalar))
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
      std::optional<gpu_internal::Invocation> native;
      OperationInvocation call{inputs,
                               step.input_demands,
                               step.parameters,
                               backend,
                               cancellation_,
                               regional_ ? step.output_demand : Region{},
                               callback_allocator};
      if (native_device_ && backend == Backend::Gpu) {
        native.emplace(native_device_, cancellation_);
        call.gpu = native->service();
      }
      Result<Value> invocation_result = invoke_(step.operation, call);
      if (native && !native->status().ok())
        invocation_result = Result<Value>(native->status());
      if (native && invocation_result.ok() &&
          native->statistics().dispatches == 0)
        invocation_result = Result<Value>(
            Status::failure(ErrorCode::BackendUnavailable,
                            "GPU callback submitted no native work"));
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
        if (native) {
          diagnostics_.native_dispatch_count += native->statistics().dispatches;
          diagnostics_.native_submission_count +=
              native->statistics().submissions;
          diagnostics_.native_compute_us += native->statistics().device_us;
          diagnostics_.native_constant_bytes +=
              native->statistics().constant_bytes;
          diagnostics_.operation_timings.back().native_dispatch_count =
              native->statistics().dispatches;
          diagnostics_.operation_timings.back().native_compute_us =
              native->statistics().device_us;
        }
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
        diagnostics_.native_upload_hits += upload_hits;
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

      native.reset();
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
    fallback_taint_[step_index] = plan_->steps()[step_index].backend != backend;
    for (const auto& source : plan_->steps()[step_index].inputs)
      if (const auto* producer = std::get_if<PlanStepInput>(&source))
        fallback_taint_[step_index] = fallback_taint_[step_index] ||
                                      fallback_taint_[producer->step_index];
    if (retain_ && !fallback_taint_[step_index])
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
  std::shared_ptr<gpu_internal::Device> native_device_;
  execution_internal::ResultCache* native_cache_;
  std::uint64_t cache_epoch_;
  std::vector<bool> fallback_taint_;
  // GPU-lane-only map; retained sources prevent allocation address reuse.
  std::map<std::string, std::pair<std::shared_ptr<const CpuStorage>, Value>>
      native_inputs_;
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
    : impl_(std::make_unique<Impl>(std::move(operations), config)) {
  impl_->demands->context = this;
}

/**
 * @brief Implements exact local worker/resource teardown.
 * @copydetails ExecutionContext::~ExecutionContext
 */
ExecutionContext::~ExecutionContext() noexcept {
  if (impl_ && impl_->demands)
    impl_->demands->close();
}
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
Status unite_named(DemandQuery* target, const DemandQuery& values,
                   const FootprintLimits& limits) {
  for (const auto& item : values) {
    auto found = target->find(item.first);
    auto next = found == target->end()
                    ? Footprint::from_regions(item.second.shape(),
                                              item.second.boxes(), limits)
                    : found->second.unite(item.second, limits);
    if (!next.ok())
      return next.status();
    target->insert_or_assign(item.first, next.take_value());
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
Result<DemandQuery> changed_inputs(const ExecutionBindings& before,
                                   const ExecutionBindings& after,
                                   const DemandQuery& support,
                                   const SnapshotAccessOptions& access,
                                   const FootprintLimits& limits) {
  std::map<std::string, const ExecutionBinding*> old, next;
  for (const auto& input : before.inputs)
    old.emplace(input.name, &input);
  for (const auto& input : after.inputs)
    next.emplace(input.name, &input);
  std::uint64_t remaining = access.maximum_samples;
  DemandQuery changes;
  for (const auto& required : support) {
    const auto& a = *old.at(required.first);
    const auto& b = *next.at(required.first);
    OperationMetadata metadata =
        a.snapshot
            ? OperationMetadata{a.snapshot->descriptor(), a.snapshot->facets()}
            : OperationMetadata{a.value.descriptor(), a.value.facets()};
    auto observations =
        operation_observations(metadata, required.second, limits);
    if (!observations.ok())
      return Result<DemandQuery>(observations.status());
    const bool image =
        observations.value().shape().size() != metadata.descriptor.shape.size();
    const auto channels = image ? metadata.descriptor.shape.back() : 1;
    const auto width = Value::element_size(metadata.descriptor.element_type);
    if (!channels || channels > SIZE_MAX / width)
      return Result<DemandQuery>(Status{ErrorCode::ResourceExhausted, {}});
    std::vector<std::uint8_t> left(channels * width), right(channels * width);
    std::vector<Region> dirty;
    auto status = observations.value().visit(
        [&](const auto& coordinate) {
          if (channels > remaining)
            return Status{ErrorCode::ResourceExhausted,
                          "demand update sample limit"};
          remaining -= channels;
          std::vector<RegionDimension> dimensions;
          for (const auto at : coordinate)
            dimensions.push_back({at, 1});
          if (image)
            dimensions.push_back({0, channels});
          Region region(dimensions);
          const auto read = [&](const ExecutionBinding& input,
                                std::vector<std::uint8_t>* bytes) -> Status {
            if (input.snapshot)
              return input.snapshot->read(
                  region, bytes->data(), bytes->size(),
                  SnapshotAccessOptions{channels, access.cancellation});
            auto at = coordinate;
            if (image)
              at.push_back(0);
            for (std::uint64_t c = 0; c < channels; ++c) {
              if (image)
                at.back() = c;
              auto offset = input.value.byte_address(at);
              if (!offset.ok())
                return offset.status();
              std::memcpy(bytes->data() + c * width,
                          input.value.bytes().data() + offset.value(), width);
            }
            return Status::success();
          };
          auto status = read(a, &left);
          if (!status.ok())
            return status;
          status = read(b, &right);
          if (!status.ok())
            return status;
          if (left != right) {
            if (dirty.size() >= limits.maximum_boxes)
              return Status{ErrorCode::ResourceExhausted,
                            "demand edit footprint limit"};
            dirty.push_back(std::move(region));
          }
          return Status::success();
        },
        access.maximum_samples, access.cancellation);
    if (!status.ok())
      return Result<DemandQuery>(status);
    auto samples =
        Footprint::from_regions(required.second.shape(), dirty, limits);
    if (!samples.ok())
      return Result<DemandQuery>(samples.status());
    if (!samples.value().empty())
      changes.emplace(required.first, samples.take_value());
  }
  return Result<DemandQuery>(std::move(changes));
}
}  // namespace

Result<FrozenExecution> ExecutionContext::freeze(
    const ExecutionPlan& plan, ExecutionBindings bindings) const {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock().get() != impl_->operation_registry.get())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid stale or foreign plan"));
  for (auto& binding : bindings.inputs) {
    if (binding.source)
      return Result<FrozenExecution>(Status::failure(
          ErrorCode::InvalidArgument,
          "freeze requires immutable Values or kernel snapshots"));
    if (binding.snapshot)
      binding.snapshot =
          std::make_shared<const InputSnapshot>(*binding.snapshot);
  }
  auto validated = preflight_regional_bindings(plan, bindings, {});
  if (!validated.ok())
    return Result<FrozenExecution>(validated.status());
  auto identity = frozen_identity();
  if (!identity.ok())
    return Result<FrozenExecution>(identity.status());
  FrozenExecution frozen;
  frozen.execution_identity_ = identity.take_value();
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
Result<DemandResult> ExecutionContext::execute_fragments(
    const FrozenExecution& frozen, const DemandQuery& query,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  if (!impl_ || !frozen.valid() || !frozen.plan_.current() ||
      frozen.operations_ != impl_->operation_registry)
    return Result<DemandResult>(
        Status{ErrorCode::Stale, "invalid or foreign frozen demand"});
  const auto stop = [&] { return binding_stop(frozen.plan_, cancellation); };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok)
      status = Status{code, {}};
    return Result<DemandResult>(std::move(status));
  };
  if (stop() != ErrorCode::Ok)
    return failure(Status{stop(), {}});
  auto key =
      demand_key(query, frozen.plan_, options.dependencies.sets.maximum_boxes);
  if (!key.ok())
    return failure(key.status());
  auto validated =
      preflight_regional_bindings(frozen.plan_, frozen.bindings_, cancellation);
  if (!validated.ok())
    return failure(validated.status());
  for (const auto& item : query)
    if (item.second.empty()) {
      const auto& step =
          frozen.plan_.steps().at(frozen.plan_.outputs().at(item.first));
      if (step.traits.dependency_version == 1) {
        std::vector<OperationMetadata> inputs;
        for (const auto& input : step.inputs) {
          if (const auto* source = std::get_if<PlanStepInput>(&input)) {
            const auto& p = frozen.plan_.steps().at(source->step_index);
            inputs.push_back({p.output_descriptor, p.output_facets});
          } else {
            const auto& p = frozen.plan_.input_declarations().at(
                std::get<PlanWorkflowInput>(input).declaration_index);
            inputs.push_back({p.descriptor, p.facets});
          }
        }
        auto status = impl_->operation_registry->validate_dependency_metadata(
            step.operation, inputs, step.parameters);
        if (!status.ok())
          return failure(status);
      }
    }
  DemandResult result;
  auto run = ExecutionRun::run_dependencies(
      &impl_->cpu_pool, &impl_->waiting_admission, impl_->budget,
      impl_->operation_registry,
      [operations = impl_->operation_registry, &frozen](
          const std::string& key, const OperationInvocation& call) {
        return operations->invoke_current(
            key, call, [&] { return frozen.plan_.current(); });
      },
      frozen.plan_, validated.take_value(), cancellation, options, nullptr,
      [this](std::uint64_t bytes) {
        if (impl_->disk && impl_->budget->available() < bytes)
          impl_->disk->drop_pending();
        if (impl_->cache)
          impl_->cache->reclaim_for(bytes);
      },
      &query, &result.values, frozen.execution_identity_);
  if (!run.ok())
    return failure(run.status());
  auto completed = run.take_value();
  result.diagnostics = std::move(completed.diagnostics);
  result.dependencies = std::move(completed.dependencies);
  if (stop() != ErrorCode::Ok)
    return failure(Status{stop(), {}});
  return Result<DemandResult>(std::move(result));
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
  auto begun = owner->acquire(impl_, cancellation);
  if (!begun.ok())
    return Result<DemandResult>(begun.status());
  auto lease = begun.take_value();
  const auto failure = [&](Status status) {
    return Result<DemandResult>(lease->stop(std::move(status)));
  };
  auto key = demand_key(query, lease->bundle->plan(),
                        std::min(impl_->config.maximum_metadata_entries,
                                 options.dependencies.sets.maximum_boxes));
  if (!key.ok())
    return failure(key.status());
  DemandQuery original = query;
  auto run = *lease->bundle;
  run.plan_.current_check_ = [handle = impl_, generation = lease->generation] {
    return handle->generation.load(std::memory_order_acquire) == generation;
  };
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
  auto key = demand_key(query, lease->bundle->plan(),
                        impl_->config.maximum_metadata_entries);
  if (!key.ok())
    return lease->stop(key.status());
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return status;
  auto found = impl_->publications.find(key.value().value);
  if (found == impl_->publications.end() || found->second->query != query)
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
    ExecutionBindings bindings, const SnapshotAccessOptions& options) const {
  if (!impl_)
    return Result<DemandUpdate>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<DemandUpdate>(Status{ErrorCode::Cancelled, {}});
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
      owner->context->freeze(lease->bundle->plan_, std::move(bindings));
  if (!frozen.ok())
    return failure(frozen.status());
  auto next = std::make_shared<const FrozenExecution>(frozen.take_value());
  FootprintLimits limits{impl_->config.maximum_metadata_entries, 1048576,
                         lease->cancellation};
  DemandQuery support;
  for (const auto& item : publications) {
    auto source = item.second->dependencies.source_support(limits);
    if (!source.ok())
      return failure(source.status());
    auto status = unite_named(&support, source.value(), limits);
    if (!status.ok())
      return failure(status);
  }
  auto access = options;
  access.cancellation = lease->cancellation;
  auto changes = changed_inputs(lease->bundle->bindings_, next->bindings_,
                                support, access, limits);
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
          change.first, change.second, 7, limits);
      if (!dirty.ok())
        return failure(dirty.status());
      auto status = unite_named(&publication->dirty, dirty.value(), limits);
      if (!status.ok())
        return failure(status);
    }
    auto key = demand_key(publication->query, next->plan_,
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
  if (plan.dependency_network())
    return execute_regions(plan, std::move(bindings), nullptr, cancellation,
                           options);
  const bool spatial = std::any_of(
      plan.steps().begin(), plan.steps().end(), [](const PlanStep& step) {
        return step.traits.output_schema.kind ==
                   OperationPortKind::RgbaFloat32 ||
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
          sum = checked_add(working_bytes, gpu_internal::allocation_capacity(
                                               dense.value().bytes));
          if (!sum.ok())
            return Result<ExecutionResult>(sum.status());
          working_bytes = sum.value();
        }
      }
    }
    if (impl_->disk && impl_->budget->available() < working_bytes)
      impl_->disk->drop_pending();
    if (impl_->cache)
      impl_->cache->reclaim_for(working_bytes);
    auto reserved = impl_->budget->reserve(
        working_bytes, [&] { return binding_stop(plan, cancellation); }, {},
        [this, working_bytes] {
          if (impl_->disk)
            impl_->disk->drop_pending();
          if (impl_->cache)
            impl_->cache->reclaim_for(working_bytes);
        });
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
        &plan, prepared.take_value(), cancellation, parallelism, false,
        std::map<std::uint64_t, Value>{}, std::map<std::uint64_t, Backend>{},
        std::function<void(std::size_t, const Value&, Backend)>{},
        impl_->native_device, impl_->cache.get(),
        impl_->cache ? impl_->cache->epoch() : 0);
    auto result = coordinator->run();
    if (impl_->cache && impl_->native_device &&
        !impl_->native_device->available())
      impl_->cache->clear();
    return result;
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
    if (plan.dependency_network())
      return ExecutionRun::run_dependencies(
          &impl_->cpu_pool, &impl_->waiting_admission, impl_->budget,
          impl_->operation_registry,
          [operations = impl_->operation_registry, &plan](
              const std::string& key, const OperationInvocation& call) {
            return operations->invoke_current(
                key, call, [&plan] { return plan.current(); });
          },
          plan, std::move(snapshot), cancellation, options, sink,
          [this](std::uint64_t bytes) {
            if (impl_->disk && impl_->budget->available() < bytes)
              impl_->disk->drop_pending();
            if (impl_->cache)
              impl_->cache->reclaim_for(bytes);
          });
    auto observation =
        std::make_shared<execution_internal::MemoryObservation>();
    std::map<std::uint64_t, Value> cached;
    std::map<std::uint64_t, Backend> cached_backends;
    // Whole values survive separate materializations within this Run. Their
    // actual backend alone cannot describe fallback ancestry/cache eligibility.
    std::set<std::uint64_t> uncacheable_whole;
    ExecutionDiagnostics diagnostics;
    diagnostics.plan_digest = plan.digest().value;
    if (impl_->cache && impl_->native_device &&
        !impl_->native_device->available())
      impl_->cache->clear();
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
      diagnostics.native_dispatch_count += part.native_dispatch_count;
      diagnostics.native_submission_count += part.native_submission_count;
      diagnostics.native_compute_us += part.native_compute_us;
      diagnostics.native_constant_bytes += part.native_constant_bytes;
      diagnostics.native_upload_hits += part.native_upload_hits;
      diagnostics.host_access_count += part.host_access_count;
      diagnostics.result_copy_bytes += part.result_copy_bytes;
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
                               timing.computed_elements),
                std::make_pair(&found->native_dispatch_count,
                               timing.native_dispatch_count),
                std::make_pair(&found->native_compute_us,
                               timing.native_compute_us)}) {
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
      auto reserved =
          impl_->budget->reserve(bytes, stop, observation, [this, bytes] {
            if (impl_->disk)
              impl_->disk->drop_pending();
            if (impl_->cache)
              impl_->cache->reclaim_for(bytes);
          });
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
    // Shared coordinators execute one ready node. Dependency traversal stays on
    // the caller, so bounded coordinators never wait for another coordinator.
    const auto run_tile =
        [this, snapshot, observation, parallelism](
            const ExecutionPlan& tile,
            const std::map<std::uint64_t, Value>& tile_cached,
            const std::map<std::uint64_t, Backend>& tile_backends,
            const std::vector<std::string>& keys, std::uint64_t cache_epoch,
            const CancellationToken& token) -> Result<ExecutionResult> {
      const auto stop = [&] { return binding_stop(tile, token); };
      ExecutionDiagnostics diagnostics;
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
            sum = checked_add(working,
                              gpu_internal::allocation_capacity(bytes.value()));
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
      auto reserved =
          impl_->budget->reserve(working, stop, observation, [this, working] {
            if (impl_->disk)
              impl_->disk->drop_pending();
            if (impl_->cache)
              impl_->cache->reclaim_for(working);
          });
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
                completion->result = source->read(
                    demand, writer.data(), writer.size(), scratch, token);
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
          &tile, std::move(values), token, parallelism, true, tile_cached,
          tile_backends,
          [this, keys, cache_epoch, &tile, token](
              std::size_t index, const Value& value, Backend backend) {
            if (impl_->cache && index < keys.size() &&
                backend == tile.steps()[index].backend)
              impl_->cache->put(
                  keys[index], value, cache_epoch,
                  impl_->native_device &&
                      impl_->native_device->owns(*value.storage()));
            if (impl_->disk &&
                tile.execution_mode() == ExecutionMode::CpuExact &&
                index < keys.size() && impl_->cache->epoch() == cache_epoch)
              impl_->disk->put(keys[index], value,
                               [&] { return binding_stop(tile, token); });
          },
          impl_->native_device, impl_->cache.get(), cache_epoch);
      auto result = coordinator->run();
      if (impl_->cache && impl_->native_device &&
          !impl_->native_device->available())
        impl_->cache->clear();
      if (result.ok()) {
        auto completed = result.take_value();
        completed.diagnostics.source_read_count +=
            diagnostics.source_read_count;
        completed.diagnostics.source_read_bytes +=
            diagnostics.source_read_bytes;
        return Result<ExecutionResult>(std::move(completed));
      }
      return result;
    };
    const auto materialize =
        [&](const ExecutionPlan& tile) -> Result<ExecutionResult> {
      auto tile_cached = cached;
      auto tile_backends = cached_backends;
      std::vector<std::string> keys;
      const auto cache_epoch = producer_epoch != UINT64_MAX
                                   ? producer_epoch
                                   : (impl_->cache ? impl_->cache->epoch() : 0);
      if (impl_->cache) {
        keys = execution_internal::result_keys(
            tile, snapshot,
            impl_->native_device && impl_->native_device->available()
                ? impl_->native_device->identity()
                : std::string{},
            cancellation);
        // Invalidate before lookup: a cached Whole value may be GPU-backed
        // while still derived from a CPU fallback earlier in this Run.
        for (std::size_t i = 0; i < tile.steps().size(); ++i) {
          if (uncacheable_whole.count(tile.steps()[i].node_id))
            keys[i].clear();
          for (const auto& source : tile.steps()[i].inputs)
            if (const auto* producer = std::get_if<PlanStepInput>(&source))
              if (keys[producer->step_index].empty())
                keys[i].clear();
        }
        std::vector<bool> needed(keys.size(), false);
        for (const auto& output : tile.outputs())
          needed[output.second] = true;
        for (std::size_t reverse = keys.size(); reverse > 0; --reverse) {
          const auto i = reverse - 1;
          if (!needed[i] || tile_cached.count(tile.steps()[i].node_id))
            continue;
          auto retained = impl_->cache->get_output(
              keys[i], tile.steps()[i], stop,
              execution_internal::dynamic_opaque_output(tile, i));
          if (!retained.ok())
            return Result<ExecutionResult>(retained.status());
          auto hit = retained.take_value();
          if (!hit.valid() && impl_->disk &&
              tile.execution_mode() == ExecutionMode::CpuExact) {
            const auto& step = tile.steps()[i];
            hit =
                impl_->disk->get(keys[i], step.output_descriptor,
                                 step.output_demand, step.output_facets, stop);
            if (hit.valid())
              impl_->cache->put(keys[i], hit, cache_epoch);
          }
          if (hit.valid()) {
            tile_cached[tile.steps()[i].node_id] = std::move(hit);
            tile_backends[tile.steps()[i].node_id] = tile.steps()[i].backend;
            diagnostics.selected_backends[tile.steps()[i].node_id] =
                tile.steps()[i].backend;
            ++diagnostics.cache_hits;
          } else {
            for (const auto& input : tile.steps()[i].inputs)
              if (const auto* producer = std::get_if<PlanStepInput>(&input))
                needed[producer->step_index] = true;
          }
        }
      }
      if (!impl_->cache)
        return run_tile(tile, tile_cached, tile_backends, keys, cache_epoch,
                        cancellation);
      const auto required = required_steps(tile, tile_cached);
      for (std::size_t i = 0; i < tile.steps().size(); ++i) {
        const auto& step = tile.steps()[i];
        if (!required[i] || tile_cached.count(step.node_id))
          continue;
        for (const auto& source : step.inputs)
          if (const auto* producer = std::get_if<PlanStepInput>(&source))
            if (keys[producer->step_index].empty())
              keys[i].clear();
        auto node = tile;
        const std::string name = "__shared_node";
        node.outputs_ = {{name, i}};
        node.output_regions_ = {{name, step.output_demand}};
        const bool share = !keys[i].empty();
        if (share)
          node.current_check_ = [] { return true; };
        auto work = [this, run_tile, node, tile_cached, tile_backends, keys,
                     cache_epoch, i, name](const CancellationToken& token) {
          // A flight can finish between the initial lookup and subscription.
          auto retained = impl_->cache->get_output(
              keys[i], node.steps()[i],
              [&] { return binding_stop(node, token); },
              execution_internal::dynamic_opaque_output(node, i));
          if (!retained.ok())
            return Result<ExecutionResult>(retained.status());
          auto hit = retained.take_value();
          if (hit.valid()) {
            ExecutionResult result;
            result.values.emplace(name, std::move(hit));
            result.diagnostics.cache_hits = 1;
            return Result<ExecutionResult>(std::move(result));
          }
          return run_tile(node, tile_cached, tile_backends, keys, cache_epoch,
                          token);
        };
        auto result =
            share ? impl_->cache->compute(keys[i], stop, std::move(work))
                  : work(cancellation);
        if (!result.ok())
          return result;
        auto completed = result.take_value();
        if (share && !completed.diagnostics.selected_backends.empty()) {
          const auto selected =
              completed.diagnostics.selected_backends.begin()->second;
          completed.diagnostics.selected_backends = {{step.node_id, selected}};
        }
        auto status = accumulate(completed.diagnostics);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        tile_cached[step.node_id] = completed.values.at(name);
        const auto backend =
            completed.diagnostics.selected_backends.find(step.node_id);
        tile_backends[step.node_id] =
            backend == completed.diagnostics.selected_backends.end()
                ? step.backend
                : backend->second;
        if (tile_backends[step.node_id] != step.backend)
          keys[i].clear();
        // Keep only ancestors still read by an unfinished node or output.
        const auto remaining = required_steps(tile, tile_cached);
        for (std::size_t j = 0; j < tile.steps().size(); ++j)
          if (!remaining[j]) {
            tile_cached.erase(tile.steps()[j].node_id);
            tile_backends.erase(tile.steps()[j].node_id);
          }
      }
      for (const auto& output : tile.outputs())
        if (tile.steps()[output.second].whole_boundary &&
            keys[output.second].empty())
          uncacheable_whole.insert(tile.steps()[output.second].node_id);
      ExecutionResult result;
      for (const auto& output : tile.outputs())
        result.values.emplace(
            output.first, tile_cached.at(tile.steps()[output.second].node_id));
      return Result<ExecutionResult>(std::move(result));
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
            if (status.ok()) {
              auto copied = region_bytes(value.descriptor(), value.region());
              if (!copied.ok())
                return failure(copied.status());
              diagnostics.result_copy_bytes += copied.value();
            }
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
  return impl_ && impl_->gpu_available &&
         (!impl_->native_device || impl_->native_device->available());
}

}  // namespace ps
