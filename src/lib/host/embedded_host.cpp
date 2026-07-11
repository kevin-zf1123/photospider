#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "compute/dirty_region_snapshot.hpp"
#include "photospider/host/host.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "yaml-cpp/yaml.h"

namespace ps {
namespace {

/**
 * @brief Owns the backend objects used by one embedded Host adapter.
 *
 * @throws std::bad_alloc if Kernel or InteractionService dependencies allocate
 *         during construction.
 * @note Futures returned by the adapter capture this shared state so destroying
 *       the Host object before an async compute completes cannot leave the
 *       backend references dangling.
 */
struct EmbeddedHostState {
  /**
   * @brief Tracked async compute submitted through the Host adapter.
   *
   * @throws Nothing for destruction.
   * @note The shared future is copied into this table so graph close can wait
   *       for backend work before releasing the graph runtime.
   */
  struct TrackedAsyncCompute {
    /** @brief Adapter-local tracking id. */
    uint64_t id = 0;

    /** @brief Session whose runtime is used by the async compute. */
    GraphSessionId session;

    /**
     * @brief Shared backend completion future for the compute request.
     *
     * @note The entry stays in outstanding_async_ until the Host wrapper has
     *       converted the backend bool into OperationStatus. A ready backend
     *       future alone is not enough to close the session because the wrapper
     *       may still need the session's LastError diagnostic.
     */
    std::shared_future<bool> future;
  };

  /**
   * @brief Result of scheduling backend async compute under Host tracking lock.
   *
   * @throws Nothing for destruction.
   * @note `scheduled=false` means no backend task was queued, either because
   *       the backend rejected the request or because the session is closing.
   */
  struct AsyncComputeRegistration {
    /** @brief True when backend work was scheduled and tracked atomically. */
    bool scheduled = false;

    /** @brief Adapter-local tracking id for the scheduled backend future. */
    uint64_t tracking_id = 0;

    /** @brief Shared backend completion future consumed by the Host wrapper. */
    std::shared_future<bool> future;
  };

  /** @brief Backend Kernel instance owned by the embedded adapter. */
  Kernel kernel;

  /** @brief Internal interaction facade used only by this Host adapter. */
  InteractionService interaction;

  /** @brief Creates the interaction facade after constructing the Kernel. */
  EmbeddedHostState() : interaction(kernel) {}

  /**
   * @brief Waits for all tracked async computes before destroying the backend.
   *
   * @throws Nothing.
   * @note Waiting here preserves runtime lifetime if a Host is destroyed while
   *       callers still hold adapter-created futures.
   */
  ~EmbeddedHostState() { wait_for_all_async_compute(); }

  /**
   * @brief Schedules backend async compute while atomically tracking it for
   * close.
   *
   * @tparam Scheduler Callable returning `std::optional<std::future<bool>>`.
   * @param session Session whose runtime will be captured by backend work.
   * @param scheduler Backend scheduling callable executed under async_mutex_.
   * @return Registration containing a tracked shared future, or
   * `scheduled=false`.
   * @throws Whatever the scheduler or pre-scheduling tracking allocation may
   *         throw.
   * @note Holding async_mutex_ across allocation, scheduling, and future
   *       publication closes the race where close_graph could observe no
   *       outstanding work after the backend had already queued a task that
   *       captured the session runtime. Tracking storage is allocated before
   *       backend scheduling so no post-schedule Host allocation can leave a
   *       queued task untracked.
   */
  template <typename Scheduler>
  AsyncComputeRegistration schedule_and_track_async_compute(
      const GraphSessionId& session, Scheduler&& scheduler) {
    static_assert(noexcept(std::declval<std::future<bool>&>().share()),
                  "future::share must not allocate after backend scheduling");

    std::lock_guard<std::mutex> lock(async_mutex_);
    if (session_close_in_progress_locked(session)) {
      return AsyncComputeRegistration{};
    }

    const uint64_t id = next_async_id_.fetch_add(1, std::memory_order_relaxed);
    outstanding_async_.push_back(
        TrackedAsyncCompute{id, session, std::shared_future<bool>{}});

    auto remove_placeholder = [&] {
      outstanding_async_.erase(
          std::remove_if(outstanding_async_.begin(), outstanding_async_.end(),
                         [id](const TrackedAsyncCompute& tracked) {
                           return tracked.id == id;
                         }),
          outstanding_async_.end());
    };

    std::shared_future<bool> shared_future;
    try {
      auto future = scheduler();
      if (!future) {
        remove_placeholder();
        return AsyncComputeRegistration{};
      }
      shared_future = future->share();
    } catch (...) {
      remove_placeholder();
      throw;
    }

    auto tracked = std::find_if(
        outstanding_async_.begin(), outstanding_async_.end(),
        [id](const TrackedAsyncCompute& entry) { return entry.id == id; });
    if (tracked != outstanding_async_.end()) {
      tracked->future = shared_future;
    }

    AsyncComputeRegistration registration;
    registration.scheduled = true;
    registration.tracking_id = id;
    registration.future = std::move(shared_future);
    return registration;
  }

  /**
   * @brief Removes one status-mapped async compute from the tracking table.
   *
   * @param id Tracking id returned by track_async_compute().
   * @throws Nothing.
   * @note The Host wrapper calls this only after it has converted the backend
   *       bool into OperationStatus. close_graph waits for this removal before
   *       clearing backend LastError state during graph close.
   */
  void mark_async_compute_finished(uint64_t id) {
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      outstanding_async_.erase(
          std::remove_if(outstanding_async_.begin(), outstanding_async_.end(),
                         [id](const TrackedAsyncCompute& tracked) {
                           return tracked.id == id;
                         }),
          outstanding_async_.end());
    }
    async_cv_.notify_all();
  }

  /**
   * @brief Marks a session closing and waits for tracked async compute users.
   *
   * @param session Session about to be closed.
   * @throws std::bad_alloc if recording the closing marker allocates.
   * @note The closing marker is inserted while holding async_mutex_ before
   *       waiting for Host wrapper status mapping. New compute_async calls for
   *       the same session are rejected until finish_session_close() removes
   *       the marker.
   */
  void begin_session_close_and_wait(const GraphSessionId& session) {
    bool marked_closing = false;
    try {
      std::unique_lock<std::mutex> lock(async_mutex_);
      mark_session_closing_locked(session);
      marked_closing = true;
      async_cv_.wait(
          lock, [&] { return !has_outstanding_async_compute_locked(session); });
    } catch (...) {
      if (marked_closing) {
        finish_session_close(session);
      }
      throw;
    }
  }

  /**
   * @brief Ends a Host close lifecycle for one session.
   *
   * @param session Session whose backend close attempt has returned.
   * @throws Nothing.
   * @note This method must be called after begin_session_close_and_wait() even
   *       when backend close reports NotFound so later load attempts using the
   *       same label are not rejected as still closing.
   */
  void finish_session_close(const GraphSessionId& session) {
    std::lock_guard<std::mutex> lock(async_mutex_);
    closing_sessions_.erase(std::remove(closing_sessions_.begin(),
                                        closing_sessions_.end(), session.value),
                            closing_sessions_.end());
  }

  /**
   * @brief Waits for every tracked async compute wrapper to finish.
   *
   * @throws Nothing.
   * @note Used during adapter-state destruction before Kernel member teardown.
   *       Entries are removed only after Host OperationStatus mapping finishes.
   */
  void wait_for_all_async_compute() {
    std::unique_lock<std::mutex> lock(async_mutex_);
    async_cv_.wait(lock, [&] { return outstanding_async_.empty(); });
  }

 private:
  /**
   * @brief Returns whether one session still has Host async status work.
   *
   * @param session Session label to search.
   * @return True when a backend future may still need Host status conversion.
   * @throws Nothing.
   * @note Caller must hold async_mutex_. Entries remain until
   *       mark_async_compute_finished() runs after LastError has been captured.
   */
  bool has_outstanding_async_compute_locked(
      const GraphSessionId& session) const {
    return std::any_of(outstanding_async_.begin(), outstanding_async_.end(),
                       [&session](const TrackedAsyncCompute& tracked) {
                         return tracked.session.value == session.value;
                       });
  }

  /**
   * @brief Returns whether one session is already in close lifecycle.
   *
   * @param session Session label to search.
   * @return True when close_graph has marked the session closing.
   * @throws Nothing.
   * @note Caller must hold async_mutex_.
   */
  bool session_close_in_progress_locked(const GraphSessionId& session) const {
    return std::find(closing_sessions_.begin(), closing_sessions_.end(),
                     session.value) != closing_sessions_.end();
  }

  /**
   * @brief Records that close_graph has started for one session.
   *
   * @param session Session label being closed.
   * @throws std::bad_alloc if inserting the label allocates.
   * @note Caller must hold async_mutex_. Duplicate markers are ignored so
   *       concurrent close attempts for the same session share the same gate.
   */
  void mark_session_closing_locked(const GraphSessionId& session) {
    if (!session_close_in_progress_locked(session)) {
      closing_sessions_.push_back(session.value);
    }
  }

  /** @brief Protects outstanding_async_ tracking mutations. */
  std::mutex async_mutex_;

  /** @brief Notifies close waiters when Host async status mapping completes. */
  std::condition_variable async_cv_;

  /** @brief Adapter-local async compute tracking table. */
  std::vector<TrackedAsyncCompute> outstanding_async_;

  /** @brief Sessions currently being closed by the Host adapter. */
  std::vector<std::string> closing_sessions_;

  /** @brief Monotonic id source for async tracking entries. */
  std::atomic<uint64_t> next_async_id_{1};
};

/**
 * @brief Marks one Host async compute complete when a wrapper exits.
 *
 * @throws Nothing from destruction.
 * @note The guard is constructed inside the adapter wrapper future before it
 *       waits on the backend future. Its destructor runs only after the wrapper
 *       has converted the backend bool or exception into OperationStatus, so
 *       close_graph cannot clear backend LastError before status mapping reads
 *       it.
 */
class AsyncComputeCompletionGuard {
 public:
  /**
   * @brief Creates a completion guard for one tracked async request.
   *
   * @param state Shared adapter state that owns the tracking table.
   * @param tracking_id Tracking id returned by
   * schedule_and_track_async_compute().
   * @throws Nothing.
   */
  AsyncComputeCompletionGuard(std::shared_ptr<EmbeddedHostState> state,
                              uint64_t tracking_id) noexcept
      : state_(std::move(state)), tracking_id_(tracking_id) {}

  /**
   * @brief Releases the tracking entry and wakes close waiters.
   *
   * @throws Nothing.
   */
  ~AsyncComputeCompletionGuard() {
    if (state_) {
      state_->mark_async_compute_finished(tracking_id_);
    }
  }

  /** @brief Copying would double-release a tracking entry and is disabled. */
  AsyncComputeCompletionGuard(const AsyncComputeCompletionGuard&) = delete;

  /** @brief Copy assignment would double-release a tracking entry and is
   * disabled. */
  AsyncComputeCompletionGuard& operator=(const AsyncComputeCompletionGuard&) =
      delete;

 private:
  /** @brief Adapter state containing the async tracking entry. */
  std::shared_ptr<EmbeddedHostState> state_;

  /** @brief Adapter-local tracking id to release on wrapper exit. */
  uint64_t tracking_id_ = 0;
};

/**
 * @brief Returns a successful OperationStatus.
 *
 * @return Status with ok=true and no diagnostic text.
 * @throws Nothing.
 * @note Success uses GraphErrc::Unknown by convention because callers should
 *       branch on ok first.
 */
OperationStatus success_status() {
  return OperationStatus{};
}

/**
 * @brief Builds a failed OperationStatus.
 *
 * @param code Stable error code for the failure.
 * @param message Human-readable diagnostic text.
 * @return Status with ok=false.
 * @throws std::bad_alloc if copying message allocates and fails.
 * @note The message is diagnostic and should not be parsed for control flow.
 */
OperationStatus failure_status(GraphErrc code, std::string message) {
  OperationStatus status;
  status.ok = false;
  status.code = code;
  status.message = std::move(message);
  return status;
}

/**
 * @brief Wraps a value in a successful Result.
 *
 * @tparam Value Result payload type.
 * @param value Value to move into the result.
 * @return Successful Result carrying value.
 * @throws Whatever moving Value may throw.
 * @note The helper keeps status construction consistent across adapter
 *       methods.
 */
template <typename Value>
Result<Value> success_result(Value value) {
  Result<Value> result;
  result.status = success_status();
  result.value = std::move(value);
  return result;
}

/**
 * @brief Builds a failed Result.
 *
 * @tparam Value Result payload type.
 * @param code Stable error code for the failure.
 * @param message Human-readable diagnostic text.
 * @return Failed Result with a default payload.
 * @throws std::bad_alloc if copying message allocates and fails.
 * @note Callers must ignore value when status.ok is false.
 */
template <typename Value>
Result<Value> failure_result(GraphErrc code, std::string message) {
  Result<Value> result;
  result.status = failure_status(code, std::move(message));
  return result;
}

/**
 * @brief Builds a successful VoidResult.
 *
 * @return Success status with no payload.
 * @throws Nothing.
 */
VoidResult success_void() {
  return VoidResult{success_status()};
}

/**
 * @brief Builds a failed VoidResult.
 *
 * @param code Stable error code for the failure.
 * @param message Human-readable diagnostic text.
 * @return Failure status with no payload.
 * @throws std::bad_alloc if copying message allocates and fails.
 */
VoidResult failure_void(GraphErrc code, std::string message) {
  return VoidResult{failure_status(code, std::move(message))};
}

/**
 * @brief Maps a caught GraphError into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param error Backend graph error.
 * @return Failed status preserving the backend error code.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 * @note Public Host methods use this path to avoid leaking exceptions through
 *       the frontend seam for recoverable backend errors.
 */
OperationStatus status_from_exception(const char* operation,
                                      const GraphError& error) {
  return failure_status(error.code(),
                        std::string(operation) + " failed: " + error.what());
}

/**
 * @brief Maps a caught YAML exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param error YAML parser or emitter exception.
 * @return InvalidYaml failure status with diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 * @note YAML remains an implementation dependency; Host callers only receive a
 *       stable status value.
 */
OperationStatus status_from_exception(const char* operation,
                                      const YAML::Exception& error) {
  return failure_status(
      GraphErrc::InvalidYaml,
      std::string(operation) + " YAML failed: " + error.what());
}

/**
 * @brief Maps a filesystem exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param error Filesystem exception from backend IO or testable path handling.
 * @return Io failure status with diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 */
OperationStatus status_from_exception(
    const char* operation, const std::filesystem::filesystem_error& error) {
  return failure_status(GraphErrc::Io,
                        std::string(operation) + " IO failed: " + error.what());
}

/**
 * @brief Maps a standard exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param fallback_code Error code used when no more specific mapping exists.
 * @param error Standard exception raised by a recoverable backend operation.
 * @return Failure status with copied diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 * @note std::bad_alloc is intentionally handled by callers before this helper
 *       so allocation failure semantics remain the language default.
 */
OperationStatus status_from_exception(const char* operation,
                                      GraphErrc fallback_code,
                                      const std::exception& error) {
  return failure_status(fallback_code,
                        std::string(operation) + " failed: " + error.what());
}

/**
 * @brief Maps an unknown exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param fallback_code Error code used for the unknown failure.
 * @return Failure status with stable generic diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 */
OperationStatus status_from_unknown_exception(const char* operation,
                                              GraphErrc fallback_code) {
  return failure_status(
      fallback_code, std::string(operation) + " failed with unknown exception");
}

/**
 * @brief Executes a value-returning Host method with exception-to-status
 * translation.
 *
 * @tparam Value Public Result payload type.
 * @tparam Fn Callable returning Result<Value>.
 * @param operation Frontend operation name used in diagnostics.
 * @param fallback_code Error code used for generic std::exception failures.
 * @param fn Host method body to execute.
 * @return Result from fn, or a failed Result when a recoverable exception is
 *         caught.
 * @throws std::bad_alloc when allocation failure prevents reliable status
 *         construction.
 * @note This is the public seam guard for backend and serialization
 *       exceptions.
 */
template <typename Value, typename Fn>
Result<Value> guarded_result(const char* operation, GraphErrc fallback_code,
                             Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, error);
    return result;
  } catch (const YAML::Exception& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, error);
    return result;
  } catch (const std::filesystem::filesystem_error& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, error);
    return result;
  } catch (const std::exception& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, fallback_code, error);
    return result;
  } catch (...) {
    Result<Value> result;
    result.status = status_from_unknown_exception(operation, fallback_code);
    return result;
  }
}

/**
 * @brief Executes a void Host method with exception-to-status translation.
 *
 * @tparam Fn Callable returning VoidResult.
 * @param operation Frontend operation name used in diagnostics.
 * @param fallback_code Error code used for generic std::exception failures.
 * @param fn Host method body to execute.
 * @return VoidResult from fn, or a failed status when a recoverable exception
 *         is caught.
 * @throws std::bad_alloc when allocation failure prevents reliable status
 *         construction.
 * @note Backend exceptions are normalized at the adapter boundary.
 */
template <typename Fn>
VoidResult guarded_void(const char* operation, GraphErrc fallback_code,
                        Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    return VoidResult{status_from_exception(operation, error)};
  } catch (const YAML::Exception& error) {
    return VoidResult{status_from_exception(operation, error)};
  } catch (const std::filesystem::filesystem_error& error) {
    return VoidResult{status_from_exception(operation, error)};
  } catch (const std::exception& error) {
    return VoidResult{status_from_exception(operation, fallback_code, error)};
  } catch (...) {
    return VoidResult{status_from_unknown_exception(operation, fallback_code)};
  }
}

/**
 * @brief Executes a status-returning Host method with exception translation.
 *
 * @tparam Fn Callable returning OperationStatus.
 * @param operation Frontend operation name used in diagnostics.
 * @param fallback_code Error code used for generic std::exception failures.
 * @param fn Host method body to execute.
 * @return OperationStatus from fn, or a failed status on recoverable exception.
 * @throws std::bad_alloc when allocation failure prevents reliable status
 *         construction.
 */
template <typename Fn>
OperationStatus guarded_status(const char* operation, GraphErrc fallback_code,
                               Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    return status_from_exception(operation, error);
  } catch (const YAML::Exception& error) {
    return status_from_exception(operation, error);
  } catch (const std::filesystem::filesystem_error& error) {
    return status_from_exception(operation, error);
  } catch (const std::exception& error) {
    return status_from_exception(operation, fallback_code, error);
  } catch (...) {
    return status_from_unknown_exception(operation, fallback_code);
  }
}

/**
 * @brief Checks whether a graph session is currently loaded.
 *
 * @param state Embedded Host backend state.
 * @param session Session id to find.
 * @return True when the backend lists the session.
 * @throws std::bad_alloc if graph listing allocation fails.
 * @note This helper is used only to distinguish missing-graph failures from
 *       optional empty inspection state.
 */
bool session_exists(const EmbeddedHostState& state,
                    const GraphSessionId& session) {
  const auto names = state.interaction.cmd_list_graphs();
  return std::find(names.begin(), names.end(), session.value) != names.end();
}

/**
 * @brief Converts the backend LastError for a graph into a failure status.
 *
 * @param state Embedded Host backend state.
 * @param session Session whose LastError should be inspected.
 * @param fallback_code Error code used when no LastError is available.
 * @param fallback_message Diagnostic used when no LastError is available.
 * @return Failed status with backend diagnostic or fallback text.
 * @throws std::bad_alloc if diagnostic text allocation fails.
 * @note Kernel LastError is best-effort; not every quiet facade writes it.
 */
OperationStatus failure_from_last_error(const EmbeddedHostState& state,
                                        const GraphSessionId& session,
                                        GraphErrc fallback_code,
                                        const std::string& fallback_message) {
  const auto error = state.interaction.cmd_last_error(session.value);
  if (error) {
    return failure_status(error->code, error->message);
  }
  return failure_status(fallback_code, fallback_message);
}

/**
 * @brief Converts a public PixelRect into an OpenCV rectangle.
 *
 * @param rect Public rectangle.
 * @return cv::Rect with identical coordinates and size.
 * @throws Nothing.
 */
cv::Rect to_cv_rect(const PixelRect& rect) {
  return cv::Rect(rect.x, rect.y, rect.width, rect.height);
}

/**
 * @brief Converts an OpenCV rectangle into a public PixelRect.
 *
 * @param rect Internal rectangle.
 * @return Public rectangle with identical coordinates and size.
 * @throws Nothing.
 */
PixelRect to_pixel_rect(const cv::Rect& rect) {
  return PixelRect{rect.x, rect.y, rect.width, rect.height};
}

/**
 * @brief Converts a public dirty domain into the compute-service domain enum.
 *
 * @param domain Public dirty domain.
 * @return Internal dirty domain.
 * @throws Nothing.
 */
compute::DirtyDomain to_compute_dirty_domain(DirtyDomain domain) {
  switch (domain) {
    case DirtyDomain::RealTime:
      return compute::DirtyDomain::RealTime;
    case DirtyDomain::HighPrecision:
    default:
      return compute::DirtyDomain::HighPrecision;
  }
}

/**
 * @brief Converts an internal dirty domain into the public dirty domain enum.
 *
 * @param domain Internal dirty domain.
 * @return Public dirty domain.
 * @throws Nothing.
 */
DirtyDomain to_public_dirty_domain(compute::DirtyDomain domain) {
  switch (domain) {
    case compute::DirtyDomain::RealTime:
      return DirtyDomain::RealTime;
    case compute::DirtyDomain::HighPrecision:
    default:
      return DirtyDomain::HighPrecision;
  }
}

/**
 * @brief Converts an internal dirty-source lifecycle value.
 *
 * @param state Internal lifecycle state.
 * @return Public lifecycle state.
 * @throws Nothing.
 */
DirtySourceLifecycleState to_public_dirty_lifecycle(
    compute::DirtySourceLifecycleState state) {
  switch (state) {
    case compute::DirtySourceLifecycleState::Updating:
      return DirtySourceLifecycleState::Updating;
    case compute::DirtySourceLifecycleState::Settled:
      return DirtySourceLifecycleState::Settled;
    case compute::DirtySourceLifecycleState::Idle:
    default:
      return DirtySourceLifecycleState::Idle;
  }
}

/**
 * @brief Removes trailing newline characters from formatted YAML text.
 *
 * @param text Text to normalize.
 * @return Text without trailing newline or carriage-return characters.
 * @throws std::bad_alloc if string move/copy allocation fails.
 * @note YAML::Dump often appends a newline; frontend parameter maps are easier
 *       to compare when scalar values are normalized.
 */
std::string trim_trailing_newlines(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

/**
 * @brief Converts a YAML parameter map into public string values.
 *
 * @param parameters Backend YAML parameters.
 * @return Map from parameter name to display/serialization text.
 * @throws YAML::Exception/std::bad_alloc if YAML conversion fails.
 * @note Host clients receive YAML as text so they do not depend on yaml-cpp.
 */
std::map<std::string, std::string> parameter_strings_from_yaml(
    const YAML::Node& parameters) {
  std::map<std::string, std::string> out;
  if (!parameters || !parameters.IsMap()) {
    return out;
  }
  for (auto it = parameters.begin(); it != parameters.end(); ++it) {
    std::string key;
    if (it->first.IsScalar()) {
      key = it->first.as<std::string>();
    } else {
      key = trim_trailing_newlines(YAML::Dump(it->first));
    }
    out[key] = trim_trailing_newlines(YAML::Dump(it->second));
  }
  return out;
}

/**
 * @brief Converts debug metadata into a public snapshot.
 *
 * @param debug Backend debug metadata.
 * @return Public debug snapshot.
 * @throws std::bad_alloc if copying strings allocates and fails.
 */
DebugMetadataSnapshot to_public_debug(const DebugMeta& debug) {
  DebugMetadataSnapshot snapshot;
  snapshot.computed_by_worker_id = debug.computed_by_worker_id;
  snapshot.timestamp_us = debug.timestamp_us;
  snapshot.execution_time_ms = debug.execution_time_ms;
  snapshot.min_val = debug.min_val;
  snapshot.max_val = debug.max_val;
  snapshot.has_nan = debug.has_nan;
  snapshot.compute_device = debug.compute_device;
  return snapshot;
}

/**
 * @brief Converts spatial metadata into a public snapshot.
 *
 * @param space Backend spatial context.
 * @param output_width Cached output width in local pixels.
 * @param output_height Cached output height in local pixels.
 * @return Public spatial snapshot.
 * @throws Nothing.
 * @note The local output extent can differ from absolute ROI when an operation
 *       resizes, crops, or scales pixels while preserving graph-space coverage.
 */
SpatialSnapshot to_public_space(const SpatialContext& space, int output_width,
                                int output_height) {
  SpatialSnapshot snapshot;
  snapshot.absolute_roi = to_pixel_rect(space.absolute_roi);
  snapshot.extent = PixelSize{output_width, output_height};
  snapshot.global_scale_x = space.global_scale_x;
  snapshot.global_scale_y = space.global_scale_y;
  std::copy(space.transform_matrix.begin(), space.transform_matrix.end(),
            snapshot.transform_matrix);
  std::copy(space.inverse_matrix.begin(), space.inverse_matrix.end(),
            snapshot.inverse_matrix);
  std::copy(space.local_inverse_matrix.begin(),
            space.local_inverse_matrix.end(), snapshot.local_inverse_matrix);
  return snapshot;
}

/**
 * @brief Converts backend node inspection into a public value snapshot.
 *
 * @param info Backend node inspection result.
 * @return Public node inspection view.
 * @throws YAML::Exception/std::bad_alloc if parameter conversion allocates or
 *         fails.
 * @note Backend Node and YAML objects are copied into public value fields.
 */
NodeInspectionView to_public_node(const GraphNodeInspectInfo& info) {
  NodeInspectionView view;
  view.id = NodeId{info.id};
  view.name = info.name;
  view.type = info.type;
  view.subtype = info.subtype;
  view.parameters = parameter_strings_from_yaml(info.parameters);
  if (info.metadata) {
    view.has_cached_output = info.metadata->has_cached_output;
    if (!info.metadata->source_label.empty()) {
      view.source_label = info.metadata->source_label;
    }
    if (info.metadata->has_cached_output) {
      view.debug = to_public_debug(info.metadata->debug);
      view.space =
          to_public_space(info.metadata->space, info.metadata->output_width,
                          info.metadata->output_height);
    }
  }
  return view;
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Injects resource exhaustion inside the graph-view adapter loop.
 *
 * @param info Backend inspection value already produced by
 * GraphInspectService traversal.
 * @return Nothing.
 * @throws std::bad_alloc when info carries the private test probe name.
 * @note This translation-unit-local BUILD_TESTING probe has no mutable state,
 * is safe across concurrent inspections, and adds no installed/public Host
 * seam. Production builds compile out both the check and sentinel behavior.
 */
void throw_if_graph_adapter_bad_alloc_probe(const GraphNodeInspectInfo& info) {
  if (info.name == "__photospider_test_bad_alloc_inspect_adapter__") {
    throw std::bad_alloc{};
  }
}
#endif

/**
 * @brief Converts backend graph inspection into a public graph view.
 *
 * @param session Session that was inspected.
 * @param snapshot Backend graph inspection snapshot.
 * @return Public graph inspection view.
 * @throws std::bad_alloc when public node, parameter, or result storage
 * exhausts memory.
 * @throws YAML::Exception when backend YAML parameters cannot be converted.
 * @note BUILD_TESTING may compile an immutable-name failpoint inside the real
 * adapter loop. Production builds compile out the probe and expose no callable
 * test seam.
 */
GraphInspectionView to_public_graph_view(
    const GraphSessionId& session, const GraphInspectionSnapshot& snapshot) {
  GraphInspectionView view;
  view.session = session;
  view.nodes.reserve(snapshot.nodes.size());
  for (const auto& node : snapshot.nodes) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    throw_if_graph_adapter_bad_alloc_probe(node);
#endif
    view.nodes.push_back(to_public_node(node));
  }
  return view;
}

/**
 * @brief Converts an internal topology edge kind into a public edge kind.
 *
 * @param kind Backend topology edge kind.
 * @return Public edge kind.
 * @throws Nothing.
 */
HostGraphEdgeKind to_public_edge_kind(GraphTopologyEdgeKind kind) {
  switch (kind) {
    case GraphTopologyEdgeKind::ParameterInput:
      return HostGraphEdgeKind::ParameterInput;
    case GraphTopologyEdgeKind::ImageInput:
    default:
      return HostGraphEdgeKind::ImageInput;
  }
}

/**
 * @brief Converts backend topology edge metadata into a public snapshot.
 *
 * @param edge Backend topology edge.
 * @return Public edge snapshot.
 * @throws std::bad_alloc if strings allocate and fail.
 */
HostGraphEdgeSnapshot to_public_edge(const GraphTopologyEdge& edge) {
  HostGraphEdgeSnapshot snapshot;
  snapshot.from_node = NodeId{edge.from_node_id};
  snapshot.to_node = NodeId{edge.to_node_id};
  snapshot.kind = to_public_edge_kind(edge.kind);
  snapshot.from_output_name = edge.from_output_name;
  snapshot.to_input_name = edge.to_input_name;
  snapshot.input_index = edge.input_index;
  return snapshot;
}

/**
 * @brief Converts backend dependency tree scope into a public scope.
 *
 * @param scope Backend dependency tree scope.
 * @return Public dependency tree scope.
 * @throws Nothing.
 */
HostDependencyTreeScope to_public_tree_scope(DependencyTree::Scope scope) {
  switch (scope) {
    case DependencyTree::Scope::StartNode:
      return HostDependencyTreeScope::StartNode;
    case DependencyTree::Scope::EndingNodes:
    default:
      return HostDependencyTreeScope::EndingNodes;
  }
}

/**
 * @brief Converts backend dependency tree into a public snapshot.
 *
 * @param tree Backend dependency tree.
 * @return Public dependency tree snapshot.
 * @throws YAML::Exception/std::bad_alloc from node conversion.
 */
HostDependencyTreeSnapshot to_public_dependency_tree(
    const DependencyTree& tree) {
  HostDependencyTreeSnapshot snapshot;
  snapshot.scope = to_public_tree_scope(tree.scope);
  if (tree.start_node_id) {
    snapshot.start_node = NodeId{*tree.start_node_id};
  }
  snapshot.graph_empty = tree.graph_empty;
  snapshot.start_node_found = tree.start_node_found;
  snapshot.no_ending_nodes = tree.no_ending_nodes;
  snapshot.root_nodes.reserve(tree.root_node_ids.size());
  for (int root : tree.root_node_ids) {
    snapshot.root_nodes.push_back(NodeId{root});
  }
  snapshot.entries.reserve(tree.entries.size());
  for (const auto& entry : tree.entries) {
    HostDependencyTreeEntry public_entry;
    public_entry.depth = entry.depth;
    if (entry.incoming_edge) {
      public_entry.incoming_edge = to_public_edge(*entry.incoming_edge);
    }
    public_entry.node = to_public_node(entry.node);
    public_entry.cycle = entry.cycle;
    snapshot.entries.push_back(std::move(public_entry));
  }
  return snapshot;
}

/**
 * @brief Converts traversal node metadata into a public snapshot.
 *
 * @param info Backend traversal node info.
 * @return Public traversal node snapshot.
 * @throws std::bad_alloc if copying the name allocates and fails.
 */
HostTraversalNodeSnapshot to_public_traversal_node(
    const Kernel::TraversalNodeInfo& info) {
  HostTraversalNodeSnapshot snapshot;
  snapshot.node = NodeId{info.id};
  snapshot.name = info.name;
  snapshot.has_memory_cache = info.has_memory_cache;
  snapshot.has_disk_cache = info.has_disk_cache;
  return snapshot;
}

/**
 * @brief Converts backend traversal orders into public node id vectors.
 *
 * @param orders Backend traversal order map.
 * @return Public traversal order map.
 * @throws std::bad_alloc if container allocation fails.
 */
std::map<int, std::vector<NodeId>> to_public_traversal_orders(
    const std::map<int, std::vector<int>>& orders) {
  std::map<int, std::vector<NodeId>> out;
  for (const auto& [end_node, nodes] : orders) {
    auto& converted = out[end_node];
    converted.reserve(nodes.size());
    for (int node : nodes) {
      converted.push_back(NodeId{node});
    }
  }
  return out;
}

/**
 * @brief Converts backend traversal details into public snapshots.
 *
 * @param details Backend traversal details keyed by ending node.
 * @return Public traversal details.
 * @throws std::bad_alloc if container/string allocation fails.
 */
std::map<int, std::vector<HostTraversalNodeSnapshot>>
to_public_traversal_details(
    const std::map<int, std::vector<Kernel::TraversalNodeInfo>>& details) {
  std::map<int, std::vector<HostTraversalNodeSnapshot>> out;
  for (const auto& [end_node, nodes] : details) {
    auto& converted = out[end_node];
    converted.reserve(nodes.size());
    for (const auto& node : nodes) {
      converted.push_back(to_public_traversal_node(node));
    }
  }
  return out;
}

/**
 * @brief Converts backend timing collector into a public timing snapshot.
 *
 * @param timing Backend timing collector.
 * @return Public timing snapshot.
 * @throws std::bad_alloc if container/string allocation fails.
 */
TimingSnapshot to_public_timing(const TimingCollector& timing) {
  TimingSnapshot snapshot;
  snapshot.total_ms = timing.total_ms;
  snapshot.node_timings.reserve(timing.node_timings.size());
  for (const auto& row : timing.node_timings) {
    NodeTimingSnapshot public_row;
    public_row.node = NodeId{row.id};
    public_row.name = row.name;
    public_row.elapsed_ms = row.elapsed_ms;
    public_row.source = row.source;
    snapshot.node_timings.push_back(std::move(public_row));
  }
  return snapshot;
}

/**
 * @brief Converts backend compute events into public snapshots.
 *
 * @param events Backend events drained from the graph.
 * @return Public compute event snapshots.
 * @throws std::bad_alloc if container/string allocation fails.
 */
std::vector<ComputeEventSnapshot> to_public_compute_events(
    const std::vector<GraphEventService::ComputeEvent>& events) {
  std::vector<ComputeEventSnapshot> out;
  out.reserve(events.size());
  for (const auto& event : events) {
    ComputeEventSnapshot snapshot;
    snapshot.node = NodeId{event.id};
    snapshot.name = event.name;
    snapshot.source = event.source;
    snapshot.elapsed_ms = event.elapsed_ms;
    out.push_back(std::move(snapshot));
  }
  return out;
}

/**
 * @brief Converts backend scheduler action into a public action label.
 *
 * @param action Backend scheduler action.
 * @return Public scheduler trace action.
 * @throws Nothing.
 */
HostSchedulerTraceAction to_public_scheduler_action(
    GraphRuntime::SchedulerEvent::Action action) {
  switch (action) {
    case GraphRuntime::SchedulerEvent::ASSIGN_INITIAL:
      return HostSchedulerTraceAction::AssignInitial;
    case GraphRuntime::SchedulerEvent::EXECUTE:
      return HostSchedulerTraceAction::Execute;
    case GraphRuntime::SchedulerEvent::EXECUTE_TILE:
      return HostSchedulerTraceAction::ExecuteTile;
    case GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE:
      return HostSchedulerTraceAction::ExecuteDirtySource;
    case GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE:
      return HostSchedulerTraceAction::ExecuteDirtyDownstreamNode;
    case GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE:
      return HostSchedulerTraceAction::ExecuteDirtyDownstreamTile;
    case GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION:
      return HostSchedulerTraceAction::SkipStaleGeneration;
    case GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION:
      return HostSchedulerTraceAction::RethrowException;
  }
  return HostSchedulerTraceAction::Unknown;
}

/**
 * @brief Converts backend scheduler trace events into public snapshots.
 *
 * @param events Backend scheduler events.
 * @return Public scheduler trace event snapshots.
 * @throws std::bad_alloc if vector allocation fails.
 */
std::vector<SchedulerTraceEventSnapshot> to_public_scheduler_trace(
    const std::vector<GraphRuntime::SchedulerEvent>& events) {
  std::vector<SchedulerTraceEventSnapshot> out;
  out.reserve(events.size());
  for (const auto& event : events) {
    SchedulerTraceEventSnapshot snapshot;
    snapshot.epoch = event.epoch;
    snapshot.node = NodeId{event.node_id};
    snapshot.worker_id = event.worker_id;
    snapshot.action = to_public_scheduler_action(event.action);
    snapshot.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            event.timestamp.time_since_epoch())
            .count());
    out.push_back(snapshot);
  }
  return out;
}

/**
 * @brief Converts backend planned task shape into a public label.
 *
 * @param kind Backend task shape.
 * @return Stable lowercase display label.
 * @throws Nothing.
 */
std::string to_public_planning_task_kind(compute::PlannedTaskKind kind) {
  switch (kind) {
    case compute::PlannedTaskKind::Node:
      return "node";
    case compute::PlannedTaskKind::Tile:
      return "tile";
    case compute::PlannedTaskKind::Monolithic:
      return "monolithic";
  }
  return "unknown";
}

/**
 * @brief Converts one backend planned task sample into a public snapshot.
 *
 * @param task Backend planned task sample.
 * @return Public task sample snapshot.
 * @throws std::bad_alloc if dependency vector copying allocates.
 * @note The result is diagnostic value data and carries no runtime task
 *       closure, queue entry, or mutable graph object.
 */
ComputePlanningTaskSnapshot to_public_planning_task(
    const compute::PlannedTask& task) {
  ComputePlanningTaskSnapshot snapshot;
  snapshot.task_id = task.task_id;
  snapshot.node = NodeId{task.node_id};
  snapshot.kind = to_public_planning_task_kind(task.kind);
  snapshot.domain = to_public_dirty_domain(task.domain);
  snapshot.output_roi = to_pixel_rect(task.output_roi);
  snapshot.tile_x = task.tile_x;
  snapshot.tile_y = task.tile_y;
  snapshot.tile_size = task.tile_size;
  snapshot.whole_output = task.whole_output;
  snapshot.dirty_selected = task.dirty_selected;
  snapshot.dirty_generation = task.dirty_generation;
  snapshot.dependency_task_ids = task.dependency_task_ids;
  return snapshot;
}

/**
 * @brief Converts one backend planning summary into a public snapshot.
 *
 * @param summary Backend planning summary copied from graph state.
 * @return Public planning inspection snapshot.
 * @throws std::bad_alloc if string/vector allocation fails.
 * @note Deep backend plan references are intentionally ignored; the Host seam
 *       exposes bounded value summaries only.
 */
ComputePlanningInspectionSnapshot to_public_planning_snapshot(
    const compute::ComputePlanSummary& summary) {
  ComputePlanningInspectionSnapshot snapshot;
  snapshot.intent = summary.intent;
  snapshot.target_node = NodeId{summary.target_node_id};
  snapshot.parallel = summary.parallel;
  snapshot.topology_generation = summary.topology_generation;
  snapshot.expansion_cache_key = summary.full_graph_cache_key;
  snapshot.planned_node_count = summary.planned_node_count;
  snapshot.task_count = summary.task_count;
  snapshot.tile_task_count = summary.tile_task_count;
  snapshot.monolithic_task_count = summary.monolithic_task_count;
  snapshot.node_task_count = summary.node_task_count;
  snapshot.dependency_count = summary.dependency_count;
  snapshot.initial_task_count = summary.initial_task_count;
  snapshot.active_task_count = summary.active_task_count;
  snapshot.dirty_source_task_count = summary.dirty_source_task_count;
  snapshot.downstream_task_count = summary.downstream_task_count;
  snapshot.initial_downstream_task_count =
      summary.initial_downstream_task_count;
  snapshot.planned_node_sample.reserve(summary.planned_node_sample.size());
  for (int node_id : summary.planned_node_sample) {
    snapshot.planned_node_sample.push_back(NodeId{node_id});
  }
  snapshot.task_sample.reserve(summary.task_sample.size());
  for (const auto& task : summary.task_sample) {
    snapshot.task_sample.push_back(to_public_planning_task(task));
  }
  return snapshot;
}

/**
 * @brief Converts backend planning summary history into public snapshots.
 *
 * @param summaries Backend bounded summary history.
 * @return Public planning snapshot history.
 * @throws std::bad_alloc if vector/string allocation fails.
 */
std::vector<ComputePlanningInspectionSnapshot> to_public_planning_snapshots(
    const std::vector<compute::ComputePlanSummary>& summaries) {
  std::vector<ComputePlanningInspectionSnapshot> out;
  out.reserve(summaries.size());
  for (const auto& summary : summaries) {
    out.push_back(to_public_planning_snapshot(summary));
  }
  return out;
}

/**
 * @brief Converts backend dirty source state into a public snapshot.
 *
 * @param state Backend dirty source state.
 * @return Public dirty source snapshot.
 * @throws std::bad_alloc if ROI vector allocation fails.
 */
DirtySourceSnapshot to_public_dirty_source(
    const compute::DirtySourceNodeState& state) {
  DirtySourceSnapshot snapshot;
  snapshot.node = NodeId{state.node_id};
  snapshot.domain = to_public_dirty_domain(state.domain);
  snapshot.lifecycle = to_public_dirty_lifecycle(state.lifecycle);
  snapshot.generation = state.generation;
  snapshot.source_rois.reserve(state.source_rois.size());
  for (const auto& roi : state.source_rois) {
    snapshot.source_rois.push_back(to_pixel_rect(roi));
  }
  return snapshot;
}

/**
 * @brief Converts backend dirty tile state into a public snapshot.
 *
 * @param tile Backend dirty tile key.
 * @return Public dirty tile snapshot.
 * @throws Nothing.
 */
DirtyTileSnapshot to_public_dirty_tile(const compute::DirtyTileKey& tile) {
  DirtyTileSnapshot snapshot;
  snapshot.node = NodeId{tile.node_id};
  snapshot.domain = to_public_dirty_domain(tile.domain);
  snapshot.tile_x = tile.tile_x;
  snapshot.tile_y = tile.tile_y;
  snapshot.tile_size = tile.tile_size;
  snapshot.pixel_roi = to_pixel_rect(tile.pixel_roi);
  return snapshot;
}

/**
 * @brief Converts backend dirty edge direction into a public value.
 *
 * @param direction Backend dirty propagation direction.
 * @return Public dirty propagation direction.
 * @throws Nothing.
 */
DirtyEdgeDirection to_public_dirty_edge_direction(
    compute::DirtyEdgeDirection direction) {
  switch (direction) {
    case compute::DirtyEdgeDirection::ForwardAffected:
      return DirtyEdgeDirection::ForwardAffected;
    case compute::DirtyEdgeDirection::BackwardDemand:
      return DirtyEdgeDirection::BackwardDemand;
  }
  return DirtyEdgeDirection::BackwardDemand;
}

/**
 * @brief Converts backend monolithic dirty work into a public snapshot.
 *
 * @param region Backend monolithic dirty record.
 * @return Public monolithic dirty record snapshot.
 * @throws Nothing.
 */
DirtyMonolithicRegionSnapshot to_public_dirty_monolithic_region(
    const compute::DirtyMonolithicRegion& region) {
  DirtyMonolithicRegionSnapshot snapshot;
  snapshot.node = NodeId{region.node_id};
  snapshot.domain = to_public_dirty_domain(region.domain);
  snapshot.pixel_roi = to_pixel_rect(region.pixel_roi);
  snapshot.whole_output = region.whole_output;
  return snapshot;
}

/**
 * @brief Converts backend dirty edge provenance into a public snapshot.
 *
 * @param mapping Backend dirty edge mapping.
 * @return Public dirty edge mapping snapshot.
 * @throws Nothing.
 */
DirtyEdgeMappingSnapshot to_public_dirty_edge_mapping(
    const compute::DirtyEdgeMapping& mapping) {
  DirtyEdgeMappingSnapshot snapshot;
  snapshot.from_node = NodeId{mapping.from_node_id};
  snapshot.to_node = NodeId{mapping.to_node_id};
  snapshot.domain = to_public_dirty_domain(mapping.domain);
  snapshot.from_roi = to_pixel_rect(mapping.from_roi);
  snapshot.to_roi = to_pixel_rect(mapping.to_roi);
  snapshot.direction = to_public_dirty_edge_direction(mapping.direction);
  return snapshot;
}

/**
 * @brief Converts backend dirty-region state into a public snapshot.
 *
 * @param snapshot Backend dirty-region snapshot.
 * @return Public dirty-region inspection snapshot.
 * @throws std::bad_alloc if container allocation fails.
 * @note Only frontend-facing source, tile, monolithic work, actual ROI, and
 *       propagation provenance values are copied.
 */
DirtyRegionInspectionSnapshot to_public_dirty_snapshot(
    const compute::DirtyRegionSnapshot& snapshot) {
  DirtyRegionInspectionSnapshot out;
  out.graph_generation = snapshot.graph_generation;

  std::vector<int> source_ids;
  source_ids.reserve(snapshot.dirty_source_state.size());
  for (const auto& [node_id, _] : snapshot.dirty_source_state) {
    source_ids.push_back(node_id);
  }
  std::sort(source_ids.begin(), source_ids.end());
  out.sources.reserve(source_ids.size());
  for (int node_id : source_ids) {
    out.sources.push_back(
        to_public_dirty_source(snapshot.dirty_source_state.at(node_id)));
  }

  out.dirty_tiles.reserve(snapshot.dirty_tiles.size());
  for (const auto& tile : snapshot.dirty_tiles) {
    out.dirty_tiles.push_back(to_public_dirty_tile(tile));
  }

  out.dirty_monolithic_nodes.reserve(snapshot.dirty_monolithic_nodes.size());
  for (const auto& region : snapshot.dirty_monolithic_nodes) {
    out.dirty_monolithic_nodes.push_back(
        to_public_dirty_monolithic_region(region));
  }

  for (const auto& [node_id, rois] : snapshot.actual_dirty_rois) {
    auto& converted = out.actual_dirty_rois[node_id];
    converted.reserve(rois.size());
    for (const auto& roi : rois) {
      converted.push_back(to_pixel_rect(roi));
    }
  }

  out.edge_mappings.reserve(snapshot.edge_mappings.size());
  for (const auto& mapping : snapshot.edge_mappings) {
    out.edge_mappings.push_back(to_public_dirty_edge_mapping(mapping));
  }
  return out;
}

/**
 * @brief Converts public compute request values into the Kernel request.
 *
 * @param request Public Host compute request.
 * @return Kernel request with internal OpenCV dirty ROI.
 * @throws std::bad_alloc if copying strings allocates and fails.
 */
Kernel::ComputeRequest to_kernel_compute_request(
    const HostComputeRequest& request) {
  Kernel::ComputeRequest kernel_request;
  kernel_request.name = request.session.value;
  kernel_request.node_id = request.node.value;
  kernel_request.cache.precision = request.cache.precision;
  kernel_request.cache.force_recache = request.cache.force_recache;
  kernel_request.cache.disable_disk_cache = request.cache.disable_disk_cache;
  kernel_request.cache.nosave = request.cache.nosave;
  kernel_request.execution.parallel = request.execution.parallel;
  kernel_request.execution.quiet = request.execution.quiet;
  kernel_request.telemetry.enable_timing = request.telemetry.enable_timing;
  kernel_request.intent = request.intent;
  if (request.dirty_roi) {
    kernel_request.dirty_roi = to_cv_rect(*request.dirty_roi);
  }
  return kernel_request;
}

/**
 * @brief Converts backend plugin load report into a public report.
 *
 * @param report Backend plugin load report.
 * @return Public plugin load report.
 * @throws std::bad_alloc if container/string allocation fails.
 */
HostPluginLoadReport to_public_plugin_report(const PluginLoadResult& report) {
  HostPluginLoadReport out;
  out.attempted = report.attempted;
  out.loaded = report.loaded;
  out.new_op_keys = report.new_op_keys;
  out.errors.reserve(report.errors.size());
  for (const auto& error : report.errors) {
    HostPluginLoadError public_error;
    public_error.path = error.path;
    public_error.code = error.code;
    public_error.message = error.message;
    out.errors.push_back(std::move(public_error));
  }
  return out;
}

/**
 * @brief Embedded in-process Host implementation.
 *
 * @throws std::bad_alloc if adapter state allocation fails.
 * @note Public methods translate values to InteractionService and Kernel calls
 *       while keeping all implementation-only objects inside this translation
 *       unit. Per-adapter graph state is independent, while operation plugin
 *       state comes from the one process owner shared by every adapter.
 */
class EmbeddedHost final : public Host {
 public:
  /**
   * @brief Creates a Host with a fresh embedded backend state.
   *
   * @throws std::bad_alloc if backend state allocation fails.
   * @note The state owns per-Host implementation objects and outlives adapter
   *       futures captured by compute_async(). It does not own or unload the
   *       process operation plugin manager.
   */
  EmbeddedHost() : state_(std::make_shared<EmbeddedHostState>()) {}

  /**
   * @brief Loads one graph through the embedded backend.
   *
   * @param request Public graph load request.
   * @return Loaded session id, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Recoverable backend and filesystem failures are converted to
   *       OperationStatus. The backend preallocates its return label before
   *       publishing a runtime, and this adapter moves that label into the
   *       result, so an allocation exception leaves no newly published
   *       session.
   */
  Result<GraphSessionId> load_graph(const GraphLoadRequest& request) override {
    return guarded_result<GraphSessionId>(
        "load_graph", GraphErrc::InvalidParameter, [&] {
          auto loaded = state_->interaction.cmd_load_graph(
              request.session.value, request.root_dir, request.yaml_path,
              request.config_path, request.cache_root_dir);
          if (!loaded) {
            return failure_result<GraphSessionId>(
                GraphErrc::InvalidParameter,
                "failed to load graph session '" + request.session.value + "'");
          }
          return success_result(GraphSessionId{std::move(*loaded)});
        });
  }

  /**
   * @brief Closes a graph after adapter-submitted async status mapping
   * completes.
   *
   * @param session Session to close.
   * @return Success or NotFound when the graph session does not exist.
   * @throws std::bad_alloc on diagnostic allocation failure.
   * @note The adapter first marks the session closing, then waits for tracked
   *       async wrappers to finish backend bool-to-OperationStatus conversion
   *       before invoking the backend close. New async computes for that
   * session are rejected while the marker is present, and failed async requests
   * can still read LastError before close clears backend diagnostics.
   */
  VoidResult close_graph(const GraphSessionId& session) override {
    return guarded_void("close_graph", GraphErrc::NotFound, [&] {
      state_->begin_session_close_and_wait(session);
      bool closed = false;
      try {
        closed = state_->interaction.cmd_close_graph(session.value);
      } catch (...) {
        state_->finish_session_close(session);
        throw;
      }
      state_->finish_session_close(session);
      if (!closed) {
        return failure_void(GraphErrc::NotFound,
                            "graph session not found: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Lists currently loaded graph sessions.
   *
   * @return Copied session labels.
   * @throws std::bad_alloc on allocation failure.
   * @note Backend exceptions are converted to a generic status failure.
   */
  Result<std::vector<GraphSessionId>> list_graphs() const override {
    return guarded_result<std::vector<GraphSessionId>>(
        "list_graphs", GraphErrc::Unknown, [&] {
          std::vector<GraphSessionId> sessions;
          const auto names = state_->interaction.cmd_list_graphs();
          sessions.reserve(names.size());
          for (const auto& name : names) {
            sessions.push_back(GraphSessionId{name});
          }
          return success_result(std::move(sessions));
        });
  }

  /**
   * @brief Reloads a graph session from YAML.
   *
   * @param session Session to reload.
   * @param yaml_path Source YAML path.
   * @return Success, NotFound for missing sessions, Io for unreadable reload
   *         input, or InvalidYaml for rejected YAML content.
   * @throws std::bad_alloc on allocation failure.
   * @note Host checks session existence before calling the backend bool API so
   *       lifecycle errors are not reported as malformed YAML. Backend
   *       LastError preserves the reload failure classification for existing
   *       sessions.
   */
  VoidResult reload_graph(const GraphSessionId& session,
                          const std::string& yaml_path) override {
    return guarded_void("reload_graph", GraphErrc::InvalidYaml, [&] {
      if (!session_exists(*state_, session)) {
        return failure_void(GraphErrc::NotFound,
                            "graph session not found: " + session.value);
      }
      if (!state_->interaction.cmd_reload_yaml(session.value, yaml_path)) {
        return VoidResult{failure_from_last_error(
            *state_, session, GraphErrc::InvalidYaml,
            "failed to reload graph session: " + session.value)};
      }
      return success_void();
    });
  }

  /**
   * @brief Saves a graph session to YAML.
   *
   * @param session Session to save.
   * @param yaml_path Destination YAML path.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Recoverable serialization and IO failures return OperationStatus.
   */
  VoidResult save_graph(const GraphSessionId& session,
                        const std::string& yaml_path) override {
    return guarded_void("save_graph", GraphErrc::Io, [&] {
      if (!state_->interaction.cmd_save_yaml(session.value, yaml_path)) {
        return failure_void(GraphErrc::Io,
                            "failed to save graph session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Clears graph model state for a loaded session.
   *
   * @param session Session to clear.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note The clear request is serialized by the backend graph-state boundary.
   */
  VoidResult clear_graph(const GraphSessionId& session) override {
    return guarded_void("clear_graph", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_graph(session.value)) {
        return failure_void(GraphErrc::NotFound,
                            "failed to clear graph session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Runs synchronous compute through the embedded backend.
   *
   * @param request Public compute request.
   * @return Success, NotFound for a missing or closed session, or compute
   *         failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note The Host checks session existence before calling the backend bool
   * API; Backend LastError is used only when the compute facade reports failure
   * for an existing session.
   */
  VoidResult compute(const HostComputeRequest& request) override {
    return guarded_void("compute", GraphErrc::ComputeError, [&] {
      if (!session_exists(*state_, request.session)) {
        return failure_void(GraphErrc::NotFound, "graph session not found: " +
                                                     request.session.value);
      }
      const auto kernel_request = to_kernel_compute_request(request);
      if (!state_->interaction.cmd_compute(kernel_request)) {
        return VoidResult{failure_from_last_error(
            *state_, request.session, GraphErrc::ComputeError,
            "compute failed for graph session: " + request.session.value)};
      }
      return success_void();
    });
  }

  /**
   * @brief Schedules async compute and tracks runtime and diagnostic lifetime.
   *
   * @param request Public compute request captured by value.
   * @return Future resolving to OperationStatus, or scheduling failure.
   * @throws std::bad_alloc on allocation failure.
   * @note Backend scheduling and Host tracking are performed under the same
   *       lifecycle lock. close_graph either waits for the tracked Host wrapper
   *       to convert backend failure into OperationStatus, or marks the session
   *       closing before backend work is queued.
   */
  Result<std::future<OperationStatus>> compute_async(
      HostComputeRequest request) override {
    return guarded_result<std::future<OperationStatus>>(
        "compute_async", GraphErrc::ComputeError, [&] {
          auto kernel_request = to_kernel_compute_request(request);
          GraphSessionId session = request.session;
          auto state = state_;
          auto registration = state->schedule_and_track_async_compute(
              request.session,
              [state, kernel_request = std::move(kernel_request)]() mutable {
                return state->interaction.cmd_compute_async(
                    std::move(kernel_request));
              });
          if (!registration.scheduled) {
            return failure_result<std::future<OperationStatus>>(
                GraphErrc::NotFound,
                "failed to schedule compute for graph session: " +
                    request.session.value);
          }

          std::shared_future<bool> shared_future =
              std::move(registration.future);
          const uint64_t tracking_id = registration.tracking_id;
          std::future<OperationStatus> wrapped;
          try {
            // After registration, all wrapper setup must either install the
            // completion guard or release tracking through this catch block.
            wrapped = std::async(std::launch::async, [state, session,
                                                      tracking_id,
                                                      shared_future]() mutable {
              AsyncComputeCompletionGuard completion(state, tracking_id);
              try {
                const bool ok = shared_future.get();
                if (ok) {
                  return success_status();
                }
                return failure_from_last_error(
                    *state, session, GraphErrc::ComputeError,
                    "async compute failed for graph session: " + session.value);
              } catch (const std::bad_alloc&) {
                throw;
              } catch (const GraphError& error) {
                return status_from_exception("compute_async", error);
              } catch (const std::exception& error) {
                return status_from_exception("compute_async",
                                             GraphErrc::ComputeError, error);
              } catch (...) {
                return status_from_unknown_exception("compute_async",
                                                     GraphErrc::ComputeError);
              }
            });
          } catch (...) {
            try {
              if (shared_future.valid()) {
                shared_future.wait();
              }
            } catch (...) {
            }
            state->mark_async_compute_finished(tracking_id);
            throw;
          }
          return success_result(std::move(wrapped));
        });
  }

  /**
   * @brief Computes a node and returns a copied image descriptor.
   *
   * @param request Public compute request.
   * @return ImageBuffer value, a successful empty ImageBuffer when compute
   *         completes without image output, NotFound for a missing or closed
   *         session, or a compute failure status for existing sessions.
   * @throws std::bad_alloc on allocation failure.
   * @note The Host checks session existence before dispatch and again before
   *       accepting an empty no-LastError backend result. Missing lifecycle
   *       state is therefore not collapsed into a generic compute failure or a
   *       successful empty image.
   * @note Backend LastError is used to distinguish handled failures from
   *       successful no-image output, and backend image memory is cloned before
   *       conversion to the public descriptor.
   */
  Result<ImageBuffer> compute_and_get_image(
      const HostComputeRequest& request) override {
    return guarded_result<ImageBuffer>(
        "compute_and_get_image", GraphErrc::ComputeError, [&] {
          if (!session_exists(*state_, request.session)) {
            return failure_result<ImageBuffer>(
                GraphErrc::NotFound,
                "graph session not found: " + request.session.value);
          }
          const auto kernel_request = to_kernel_compute_request(request);
          auto image =
              state_->interaction.cmd_compute_and_get_image(kernel_request);
          if (!image) {
            if (!session_exists(*state_, request.session)) {
              return failure_result<ImageBuffer>(
                  GraphErrc::NotFound,
                  "graph session not found: " + request.session.value);
            }
            const auto error =
                state_->interaction.cmd_last_error(request.session.value);
            if (!error) {
              return success_result(ImageBuffer{});
            }
            Result<ImageBuffer> result;
            result.status = failure_status(error->code, error->message);
            return result;
          }
          return success_result(fromCvMat(*image));
        });
  }

  /**
   * @brief Reads timing rows for a graph session.
   *
   * @param session Session to inspect.
   * @return Timing snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing timing data is reported as NotFound.
   */
  Result<TimingSnapshot> timing(const GraphSessionId& session) override {
    return guarded_result<TimingSnapshot>("timing", GraphErrc::NotFound, [&] {
      auto timing_result = state_->interaction.cmd_timing(session.value);
      if (!timing_result) {
        return failure_result<TimingSnapshot>(
            GraphErrc::NotFound,
            "timing not available for graph session: " + session.value);
      }
      return success_result(to_public_timing(*timing_result));
    });
  }

  /**
   * @brief Reads the backend IO-time accumulator for a graph session.
   *
   * @param session Session to inspect.
   * @return Latest IO duration in milliseconds, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note The value is copied from Kernel diagnostic state and may be zero for
   *       compute paths that performed no measured disk IO.
   */
  Result<double> last_io_time(const GraphSessionId& session) const override {
    return guarded_result<double>("last_io_time", GraphErrc::NotFound, [&] {
      auto io_time = state_->interaction.cmd_get_last_io_time(session.value);
      if (!io_time) {
        return failure_result<double>(
            GraphErrc::NotFound,
            "IO timing not available for graph session: " + session.value);
      }
      return success_result(*io_time);
    });
  }

  /**
   * @brief Reads the backend last-error snapshot.
   *
   * @param session Session to inspect.
   * @return Last failure status, or ok when no failure was recorded.
   * @throws std::bad_alloc on allocation failure.
   * @note Exceptions while reading diagnostic state become status failures.
   */
  OperationStatus last_error(const GraphSessionId& session) const override {
    return guarded_status("last_error", GraphErrc::Unknown, [&] {
      auto error = state_->interaction.cmd_last_error(session.value);
      if (!error) {
        return success_status();
      }
      return failure_status(error->code, error->message);
    });
  }

  /**
   * @brief Lists node ids for a graph session.
   *
   * @param session Session to inspect.
   * @return Copied node ids, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note The result is a point-in-time value snapshot.
   */
  Result<std::vector<NodeId>> list_node_ids(
      const GraphSessionId& session) override {
    return guarded_result<std::vector<NodeId>>(
        "list_node_ids", GraphErrc::NotFound, [&] {
          auto ids = state_->interaction.cmd_list_node_ids(session.value);
          if (!ids) {
            return failure_result<std::vector<NodeId>>(
                GraphErrc::NotFound,
                "node ids not available for graph session: " + session.value);
          }
          std::vector<NodeId> out;
          out.reserve(ids->size());
          for (int id : *ids) {
            out.push_back(NodeId{id});
          }
          return success_result(std::move(out));
        });
  }

  /**
   * @brief Lists ending node ids for a graph session.
   *
   * @param session Session to inspect.
   * @return Copied ending node ids, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Topology errors are returned as OperationStatus failures.
   */
  Result<std::vector<NodeId>> ending_nodes(
      const GraphSessionId& session) override {
    return guarded_result<std::vector<NodeId>>(
        "ending_nodes", GraphErrc::NotFound, [&] {
          auto ids = state_->interaction.cmd_ending_nodes(session.value);
          if (!ids) {
            return failure_result<std::vector<NodeId>>(
                GraphErrc::NotFound,
                "ending nodes not available for graph session: " +
                    session.value);
          }
          std::vector<NodeId> out;
          out.reserve(ids->size());
          for (int id : *ids) {
            out.push_back(NodeId{id});
          }
          return success_result(std::move(out));
        });
  }

  /**
   * @brief Serializes a node to YAML text.
   *
   * @param session Session containing the node.
   * @param node Node to serialize.
   * @return YAML text, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Serialization exceptions are converted to Host status values.
   */
  Result<std::string> get_node_yaml(const GraphSessionId& session,
                                    NodeId node) override {
    return guarded_result<std::string>(
        "get_node_yaml", GraphErrc::NotFound, [&] {
          auto yaml =
              state_->interaction.cmd_get_node_yaml(session.value, node.value);
          if (!yaml) {
            return failure_result<std::string>(
                GraphErrc::NotFound, "node YAML not available for node " +
                                         std::to_string(node.value));
          }
          return success_result(*yaml);
        });
  }

  /**
   * @brief Replaces a node from YAML text.
   *
   * @param session Session containing the node.
   * @param node Node id to preserve.
   * @param yaml_text Replacement node YAML.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note YAML parser failures are reported as InvalidYaml.
   */
  VoidResult set_node_yaml(const GraphSessionId& session, NodeId node,
                           const std::string& yaml_text) override {
    return guarded_void("set_node_yaml", GraphErrc::InvalidYaml, [&] {
      if (!state_->interaction.cmd_set_node_yaml(session.value, node.value,
                                                 yaml_text)) {
        return failure_void(
            GraphErrc::InvalidYaml,
            "failed to set node YAML for node " + std::to_string(node.value));
      }
      return success_void();
    });
  }

  /**
   * @brief Inspects one graph node.
   *
   * @param session Session containing the node.
   * @param node Node to inspect.
   * @return Public node snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note YAML parameter formatting exceptions are converted to status.
   */
  Result<NodeInspectionView> inspect_node(const GraphSessionId& session,
                                          NodeId node) override {
    return guarded_result<NodeInspectionView>(
        "inspect_node", GraphErrc::NotFound, [&] {
          auto info =
              state_->interaction.cmd_inspect_node(session.value, node.value);
          if (!info) {
            return failure_result<NodeInspectionView>(
                GraphErrc::NotFound, "node inspection not available for node " +
                                         std::to_string(node.value));
          }
          return success_result(to_public_node(*info));
        });
  }

  /**
   * @brief Inspects all nodes in one graph session.
   *
   * @param session Session to inspect.
   * @return Public graph snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Serialization failures in node parameters do not escape the Host
   *       boundary.
   */
  Result<GraphInspectionView> inspect_graph(
      const GraphSessionId& session) override {
    return guarded_result<GraphInspectionView>(
        "inspect_graph", GraphErrc::NotFound, [&] {
          auto snapshot = state_->interaction.cmd_inspect_graph(session.value);
          if (!snapshot) {
            return failure_result<GraphInspectionView>(
                GraphErrc::NotFound,
                "graph inspection not available for session: " + session.value);
          }
          return success_result(to_public_graph_view(session, *snapshot));
        });
  }

  /**
   * @brief Builds a dependency-tree snapshot.
   *
   * @param session Session to inspect.
   * @param node Optional start node.
   * @param include_metadata Whether metadata should be copied.
   * @return Public dependency tree, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Backend topology and metadata objects are copied into public values.
   */
  Result<HostDependencyTreeSnapshot> dependency_tree(
      const GraphSessionId& session, std::optional<NodeId> node,
      bool include_metadata) override {
    return guarded_result<HostDependencyTreeSnapshot>(
        "dependency_tree", GraphErrc::NotFound, [&] {
          std::optional<int> node_id;
          if (node) {
            node_id = node->value;
          }
          auto tree = state_->interaction.cmd_dependency_tree(
              session.value, node_id, include_metadata);
          if (!tree) {
            return failure_result<HostDependencyTreeSnapshot>(
                GraphErrc::NotFound,
                "dependency tree not available for session: " + session.value);
          }
          return success_result(to_public_dependency_tree(*tree));
        });
  }

  /**
   * @brief Returns traversal orders keyed by ending node.
   *
   * @param session Session to inspect.
   * @return Public traversal order map, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Node ids are copied values and do not expose mutable topology state.
   */
  Result<std::map<int, std::vector<NodeId>>> traversal_orders(
      const GraphSessionId& session) override {
    return guarded_result<std::map<int, std::vector<NodeId>>>(
        "traversal_orders", GraphErrc::NotFound, [&] {
          auto orders = state_->interaction.cmd_traversal_orders(session.value);
          if (!orders) {
            return failure_result<std::map<int, std::vector<NodeId>>>(
                GraphErrc::NotFound,
                "traversal orders not available for session: " + session.value);
          }
          return success_result(to_public_traversal_orders(*orders));
        });
  }

  /**
   * @brief Returns traversal details keyed by ending node.
   *
   * @param session Session to inspect.
   * @return Public traversal metadata map, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Cache flags are observational and may become stale after mutation.
   */
  Result<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
  traversal_details(const GraphSessionId& session) override {
    return guarded_result<
        std::map<int, std::vector<HostTraversalNodeSnapshot>>>(
        "traversal_details", GraphErrc::NotFound, [&] {
          auto details =
              state_->interaction.cmd_traversal_details(session.value);
          if (!details) {
            return failure_result<
                std::map<int, std::vector<HostTraversalNodeSnapshot>>>(
                GraphErrc::NotFound,
                "traversal details not available for session: " +
                    session.value);
          }
          return success_result(to_public_traversal_details(*details));
        });
  }

  /**
   * @brief Finds ending-node trees that contain a node.
   *
   * @param session Session to inspect.
   * @param node Node to search for.
   * @return Ending node ids, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note The query is converted to copied public node ids.
   */
  Result<std::vector<NodeId>> trees_containing_node(
      const GraphSessionId& session, NodeId node) override {
    return guarded_result<std::vector<NodeId>>(
        "trees_containing_node", GraphErrc::NotFound, [&] {
          auto roots = state_->interaction.cmd_trees_containing_node(
              session.value, node.value);
          if (!roots) {
            return failure_result<std::vector<NodeId>>(
                GraphErrc::NotFound,
                "trees not available for node " + std::to_string(node.value));
          }
          std::vector<NodeId> out;
          out.reserve(roots->size());
          for (int id : *roots) {
            out.push_back(NodeId{id});
          }
          return success_result(std::move(out));
        });
  }

  /**
   * @brief Projects a source ROI forward to a target node.
   *
   * @param session Session containing the graph.
   * @param start_node Source node.
   * @param start_roi Source ROI in public coordinates.
   * @param target_node Target node.
   * @return Projected ROI, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note OpenCV rectangles remain implementation-local.
   */
  Result<PixelRect> project_roi(const GraphSessionId& session,
                                NodeId start_node, const PixelRect& start_roi,
                                NodeId target_node) override {
    return guarded_result<PixelRect>(
        "project_roi", GraphErrc::InvalidParameter, [&] {
          auto roi = state_->interaction.cmd_project_roi(
              session.value, start_node.value, to_cv_rect(start_roi),
              target_node.value);
          if (!roi) {
            return failure_result<PixelRect>(
                GraphErrc::InvalidParameter,
                "failed to project ROI for session: " + session.value);
          }
          return success_result(to_pixel_rect(*roi));
        });
  }

  /**
   * @brief Projects a target ROI backward to a source node.
   *
   * @param session Session containing the graph.
   * @param target_node Target node.
   * @param target_roi Target ROI in public coordinates.
   * @param source_node Source node.
   * @return Projected source ROI, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Projection failures are reported as InvalidParameter.
   */
  Result<PixelRect> project_roi_backward(const GraphSessionId& session,
                                         NodeId target_node,
                                         const PixelRect& target_roi,
                                         NodeId source_node) override {
    return guarded_result<PixelRect>(
        "project_roi_backward", GraphErrc::InvalidParameter, [&] {
          auto roi = state_->interaction.cmd_project_roi_backward(
              session.value, target_node.value, to_cv_rect(target_roi),
              source_node.value);
          if (!roi) {
            return failure_result<PixelRect>(
                GraphErrc::InvalidParameter,
                "failed to project ROI backward for session: " + session.value);
          }
          return success_result(to_pixel_rect(*roi));
        });
  }

  /**
   * @brief Reads the latest dirty-region snapshot.
   *
   * @param session Session to inspect.
   * @return Public dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Loaded sessions without dirty state return an empty successful
   *       snapshot.
   */
  Result<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const GraphSessionId& session) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "dirty_region_snapshot", GraphErrc::NotFound, [&] {
          auto snapshot =
              state_->interaction.cmd_dirty_region_snapshot(session.value);
          if (!snapshot) {
            if (session_exists(*state_, session)) {
              return success_result(DirtyRegionInspectionSnapshot{});
            }
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "dirty-region snapshot not available for session: " +
                    session.value);
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Reads the latest compute planning snapshot for a graph session.
   *
   * @param session Session to inspect.
   * @return Public optional planning snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions fail with NotFound; loaded sessions without compute
   *       history return an empty optional with success status.
   */
  Result<std::optional<ComputePlanningInspectionSnapshot>>
  compute_planning_snapshot(const GraphSessionId& session) override {
    return guarded_result<std::optional<ComputePlanningInspectionSnapshot>>(
        "compute_planning_snapshot", GraphErrc::NotFound, [&] {
          auto snapshot =
              state_->interaction.cmd_compute_planning_snapshot(session.value);
          if (!snapshot) {
            if (session_exists(*state_, session)) {
              return success_result(
                  std::optional<ComputePlanningInspectionSnapshot>{});
            }
            return failure_result<
                std::optional<ComputePlanningInspectionSnapshot>>(
                GraphErrc::NotFound,
                "compute planning snapshot not available for session: " +
                    session.value);
          }
          return success_result(
              std::optional<ComputePlanningInspectionSnapshot>(
                  to_public_planning_snapshot(*snapshot)));
        });
  }

  /**
   * @brief Reads recent compute planning snapshots for a graph session.
   *
   * @param session Session to inspect.
   * @return Public bounded planning snapshot history, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Empty history is a successful loaded-session state before compute.
   */
  Result<std::vector<ComputePlanningInspectionSnapshot>>
  recent_compute_planning_snapshots(const GraphSessionId& session) override {
    return guarded_result<std::vector<ComputePlanningInspectionSnapshot>>(
        "recent_compute_planning_snapshots", GraphErrc::NotFound, [&] {
          auto snapshots =
              state_->interaction.cmd_recent_compute_planning_snapshots(
                  session.value);
          if (!snapshots) {
            return failure_result<
                std::vector<ComputePlanningInspectionSnapshot>>(
                GraphErrc::NotFound,
                "compute planning history not available for session: " +
                    session.value);
          }
          return success_result(to_public_planning_snapshots(*snapshots));
        });
  }

  /**
   * @brief Begins a dirty-source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Dirty source node.
   * @param domain Dirty domain.
   * @param source_roi Source-local ROI.
   * @return Updated dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions are reported as NotFound before entering the
   *       backend; missing nodes and invalid ROIs preserve backend LastError
   *       classifications.
   */
  Result<DirtyRegionInspectionSnapshot> begin_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "begin_dirty_source", GraphErrc::InvalidParameter, [&] {
          if (!session_exists(*state_, session)) {
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "graph session not found: " + session.value);
          }
          auto snapshot = state_->interaction.cmd_begin_dirty_source(
              session.value, node.value, to_compute_dirty_domain(domain),
              to_cv_rect(source_roi));
          if (!snapshot) {
            Result<DirtyRegionInspectionSnapshot> result;
            result.status = failure_from_last_error(
                *state_, session, GraphErrc::InvalidParameter,
                "failed to begin dirty source for node " +
                    std::to_string(node.value));
            return result;
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Updates a dirty-source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Dirty source node.
   * @param domain Dirty domain.
   * @param source_roi Additional source-local ROI.
   * @return Updated dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions are reported as NotFound before entering the
   *       backend; missing nodes and invalid ROIs preserve backend LastError
   *       classifications.
   */
  Result<DirtyRegionInspectionSnapshot> update_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "update_dirty_source", GraphErrc::InvalidParameter, [&] {
          if (!session_exists(*state_, session)) {
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "graph session not found: " + session.value);
          }
          auto snapshot = state_->interaction.cmd_update_dirty_source(
              session.value, node.value, to_compute_dirty_domain(domain),
              to_cv_rect(source_roi));
          if (!snapshot) {
            Result<DirtyRegionInspectionSnapshot> result;
            result.status = failure_from_last_error(
                *state_, session, GraphErrc::InvalidParameter,
                "failed to update dirty source for node " +
                    std::to_string(node.value));
            return result;
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Ends a dirty-source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Dirty source node.
   * @param domain Dirty domain.
   * @return Updated dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions are reported as NotFound before entering the
   *       backend; missing nodes preserve backend LastError classifications.
   */
  Result<DirtyRegionInspectionSnapshot> end_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "end_dirty_source", GraphErrc::InvalidParameter, [&] {
          if (!session_exists(*state_, session)) {
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "graph session not found: " + session.value);
          }
          auto snapshot = state_->interaction.cmd_end_dirty_source(
              session.value, node.value, to_compute_dirty_domain(domain));
          if (!snapshot) {
            Result<DirtyRegionInspectionSnapshot> result;
            result.status = failure_from_last_error(
                *state_, session, GraphErrc::InvalidParameter,
                "failed to end dirty source for node " +
                    std::to_string(node.value));
            return result;
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Drains compute events for a graph session.
   *
   * @param session Session whose event buffer should be drained.
   * @return Public event snapshots, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Draining mutates only backend diagnostic event storage.
   */
  Result<std::vector<ComputeEventSnapshot>> drain_compute_events(
      const GraphSessionId& session) override {
    return guarded_result<std::vector<ComputeEventSnapshot>>(
        "drain_compute_events", GraphErrc::NotFound, [&] {
          auto events =
              state_->interaction.cmd_drain_compute_events(session.value);
          if (!events) {
            return failure_result<std::vector<ComputeEventSnapshot>>(
                GraphErrc::NotFound,
                "compute events not available for session: " + session.value);
          }
          return success_result(to_public_compute_events(*events));
        });
  }

  /**
   * @brief Reads scheduler trace events for a graph session.
   *
   * @param session Session to inspect.
   * @return Public scheduler trace snapshots, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Trace events are copied and do not expose scheduler queues.
   */
  Result<std::vector<SchedulerTraceEventSnapshot>> scheduler_trace(
      const GraphSessionId& session) override {
    return guarded_result<std::vector<SchedulerTraceEventSnapshot>>(
        "scheduler_trace", GraphErrc::NotFound, [&] {
          auto events = state_->interaction.cmd_scheduler_trace(session.value);
          if (!events) {
            return failure_result<std::vector<SchedulerTraceEventSnapshot>>(
                GraphErrc::NotFound,
                "scheduler trace not available for session: " + session.value);
          }
          return success_result(to_public_scheduler_trace(*events));
        });
  }

  /**
   * @brief Clears all cache layers for a graph session.
   *
   * @param session Session whose caches should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Cache service exceptions are converted to Host status values.
   */
  VoidResult clear_cache(const GraphSessionId& session) override {
    return guarded_void("clear_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_cache(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to clear cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Clears disk cache for a graph session.
   *
   * @param session Session whose disk cache should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Detailed cache counts are not exposed by this Host slice.
   */
  VoidResult clear_drive_cache(const GraphSessionId& session) override {
    return guarded_void("clear_drive_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_drive_cache(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to clear drive cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Clears memory cache for a graph session.
   *
   * @param session Session whose memory cache should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Persistent graph and disk cache state are left intact.
   */
  VoidResult clear_memory_cache(const GraphSessionId& session) override {
    return guarded_void("clear_memory_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_memory_cache(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to clear memory cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Saves all node caches for a graph session.
   *
   * @param session Session whose nodes should be cached.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Precision handling is delegated to the backend cache service.
   */
  VoidResult cache_all_nodes(const GraphSessionId& session,
                             const std::string& precision) override {
    return guarded_void("cache_all_nodes", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_cache_all_nodes(session.value, precision)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to cache nodes for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Releases transient memory cache for a graph session.
   *
   * @param session Session whose transient memory should be freed.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note The graph remains loaded after transient memory is released.
   */
  VoidResult free_transient_memory(const GraphSessionId& session) override {
    return guarded_void("free_transient_memory", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_free_transient_memory(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to free transient memory for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Synchronizes disk cache for a graph session.
   *
   * @param session Session whose cache should be synchronized.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Synchronization failures are mapped to status values.
   */
  VoidResult synchronize_disk_cache(const GraphSessionId& session,
                                    const std::string& precision) override {
    return guarded_void("synchronize_disk_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_synchronize_disk_cache(session.value,
                                                          precision)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to synchronize disk cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Loads operation plugins and returns a structured report.
   *
   * @param dirs Directories or glob-like inputs to scan.
   * @return Public plugin load report, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Successful handles stay in the process-global plugin owner and remain
   *       visible after this adapter is destroyed.
   */
  Result<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& dirs) override {
    return guarded_result<HostPluginLoadReport>(
        "plugins_load_report", GraphErrc::Io, [&] {
          return success_result(to_public_plugin_report(
              state_->interaction.cmd_plugins_load_report(dirs)));
        });
  }

  /**
   * @brief Loads operation plugins and returns status only.
   *
   * @param dirs Directories or glob-like inputs to scan.
   * @return Success or first plugin-load failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note This method preserves the status-only frontend contract while
   *       mutating the same process owner as every other Host.
   */
  VoidResult plugins_load(const std::vector<std::string>& dirs) override {
    return guarded_void("plugins_load", GraphErrc::Io, [&] {
      const auto report = state_->interaction.cmd_plugins_load_report(dirs);
      if (!report.errors.empty()) {
        return failure_void(report.errors.front().code,
                            report.errors.front().message);
      }
      return success_void();
    });
  }

  /**
   * @brief Unloads all process-global operation plugins.
   *
   * @return Number of active operation keys removed or restored, or a failed
   *         status.
   * @throws std::bad_alloc on allocation failure.
   * @note Every Host observes the registry/source mutation. Copied callbacks
   *       and returned values retain their library lease until destruction.
   */
  Result<int> plugins_unload_all() override {
    return guarded_result<int>("plugins_unload_all", GraphErrc::Io, [&] {
      return success_result(state_->interaction.cmd_plugins_unload_all());
    });
  }

  /**
   * @brief Seeds built-in operation source labels.
   *
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Built-in callback registration runs once process-wide; later calls
   *       reconcile source metadata without replaying over plugin overrides.
   */
  VoidResult seed_builtin_ops() override {
    return guarded_void("seed_builtin_ops", GraphErrc::Unknown, [&] {
      state_->interaction.cmd_seed_builtin_ops();
      return success_void();
    });
  }

  /**
   * @brief Lists operation source labels.
   *
   * @return Source labels keyed by operation key, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Returned labels are copied diagnostics, not plugin handles.
   */
  Result<std::map<std::string, std::string>> ops_sources() const override {
    return guarded_result<std::map<std::string, std::string>>(
        "ops_sources", GraphErrc::Unknown,
        [&] { return success_result(state_->interaction.cmd_ops_sources()); });
  }

  /**
   * @brief Lists combined operation keys.
   *
   * @return Combined operation keys, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Combined keys are suitable for frontend operation pickers.
   */
  Result<std::vector<std::string>> ops_combined_keys() const override {
    return guarded_result<std::vector<std::string>>(
        "ops_combined_keys", GraphErrc::Unknown, [&] {
          return success_result(state_->interaction.cmd_ops_combined_keys());
        });
  }

  /**
   * @brief Lists source labels for combined operation keys.
   *
   * @return Source labels keyed by combined operation key, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing source labels default through backend helper behavior.
   */
  Result<std::map<std::string, std::string>> ops_combined_sources()
      const override {
    return guarded_result<std::map<std::string, std::string>>(
        "ops_combined_sources", GraphErrc::Unknown, [&] {
          return success_result(state_->interaction.cmd_ops_combined_sources());
        });
  }

  /**
   * @brief Lists available scheduler type names.
   *
   * @return Scheduler type names, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Built-in and plugin-provided scheduler names are copied.
   */
  Result<std::vector<std::string>> scheduler_available_types() const override {
    return guarded_result<std::vector<std::string>>(
        "scheduler_available_types", GraphErrc::Unknown, [&] {
          return success_result(
              state_->interaction.cmd_scheduler_available_types());
        });
  }

  /**
   * @brief Reads a scheduler description.
   *
   * @param type_name Scheduler type name.
   * @return Description text, or NotFound when the scheduler type is
   * unavailable.
   * @throws std::bad_alloc on allocation failure.
   * @note The adapter checks the available type list before calling the backend
   *       description helper because that helper has a display fallback string.
   */
  Result<std::string> scheduler_description(
      const std::string& type_name) const override {
    return guarded_result<std::string>(
        "scheduler_description", GraphErrc::NotFound, [&] {
          const auto types =
              state_->interaction.cmd_scheduler_available_types();
          if (std::find(types.begin(), types.end(), type_name) == types.end()) {
            return failure_result<std::string>(
                GraphErrc::NotFound, "scheduler type not found: " + type_name);
          }
          return success_result(
              state_->interaction.cmd_scheduler_description(type_name));
        });
  }

  /**
   * @brief Scans directories for scheduler plugins.
   *
   * @param dirs Directories to scan.
   * @return Number of loaded scheduler types, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Plugin load exceptions are normalized to Host status values.
   */
  Result<size_t> scheduler_scan(const std::vector<std::string>& dirs) override {
    return guarded_result<size_t>("scheduler_scan", GraphErrc::Io, [&] {
      return success_result(state_->interaction.cmd_scheduler_scan(dirs));
    });
  }

  /**
   * @brief Loads one scheduler plugin.
   *
   * @param path Dynamic library path.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Failed loads return Io status instead of throwing through Host.
   */
  VoidResult scheduler_load(const std::string& path) override {
    return guarded_void("scheduler_load", GraphErrc::Io, [&] {
      if (!state_->interaction.cmd_scheduler_load(path)) {
        return failure_void(GraphErrc::Io,
                            "failed to load scheduler plugin: " + path);
      }
      return success_void();
    });
  }

  /**
   * @brief Lists loaded scheduler plugin labels.
   *
   * @return Plugin labels, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Labels are diagnostic strings copied from backend state.
   */
  Result<std::vector<std::string>> scheduler_loaded_plugins() const override {
    return guarded_result<std::vector<std::string>>(
        "scheduler_loaded_plugins", GraphErrc::Unknown, [&] {
          return success_result(
              state_->interaction.cmd_scheduler_loaded_plugins());
        });
  }

  /**
   * @brief Applies scheduler defaults for subsequently loaded graph sessions.
   *
   * @param config Public scheduler default values.
   * @return Success status.
   * @throws std::bad_alloc if scheduler type strings allocate while copied.
   * @note Existing runtime schedulers are not replaced by this method.
   */
  VoidResult configure_scheduler_defaults(
      const HostSchedulerConfig& config) override {
    return guarded_void("configure_scheduler_defaults", GraphErrc::Unknown,
                        [&] {
                          Kernel::SchedulerConfig backend_config;
                          backend_config.hp_type = config.hp_type;
                          backend_config.rt_type = config.rt_type;
                          backend_config.worker_count = config.worker_count;
                          state_->kernel.set_scheduler_config(backend_config);
                          return success_void();
                        });
  }

  /**
   * @brief Reads scheduler information for a graph intent.
   *
   * @param session Session to inspect.
   * @param intent Compute intent served by the scheduler.
   * @return Scheduler info snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Scheduler implementation objects never leave this translation unit.
   */
  Result<SchedulerInfoSnapshot> scheduler_info(
      const GraphSessionId& session, ComputeIntent intent) const override {
    return guarded_result<SchedulerInfoSnapshot>(
        "scheduler_info", GraphErrc::NotFound, [&] {
          const auto info =
              state_->kernel.get_scheduler_info(session.value, intent);
          if (!info) {
            return failure_result<SchedulerInfoSnapshot>(
                GraphErrc::NotFound,
                "scheduler info not available for session: " + session.value);
          }
          SchedulerInfoSnapshot snapshot;
          snapshot.intent = intent;
          snapshot.scheduler_name = info->first;
          snapshot.stats = info->second;
          return success_result(std::move(snapshot));
        });
  }

  /**
   * @brief Replaces the scheduler for one graph intent.
   *
   * @param session Session to update.
   * @param intent Compute intent whose scheduler is replaced.
   * @param type Scheduler type name.
   * @return Success, NotFound for a missing or closed session, or
   *         InvalidParameter for an unavailable scheduler type.
   * @throws std::bad_alloc on allocation failure.
   * @note The Host pre-checks session existence before calling the backend bool
   *       facade because the backend uses the same false return for missing
   *       runtimes and unsupported scheduler types.
   */
  VoidResult replace_scheduler(const GraphSessionId& session,
                               ComputeIntent intent,
                               const std::string& type) override {
    return guarded_void("replace_scheduler", GraphErrc::InvalidParameter, [&] {
      if (!session_exists(*state_, session)) {
        return failure_void(GraphErrc::NotFound,
                            "graph session not found: " + session.value);
      }
      if (!state_->kernel.replace_scheduler(session.value, intent, type)) {
        return failure_void(
            GraphErrc::InvalidParameter,
            "failed to replace scheduler for session: " + session.value);
      }
      return success_void();
    });
  }

 private:
  /** @brief Shared embedded backend state owned by this Host and async futures.
   */
  std::shared_ptr<EmbeddedHostState> state_;
};

}  // namespace

std::unique_ptr<Host> create_embedded_host() {
  return std::make_unique<EmbeddedHost>();
}

}  // namespace ps
