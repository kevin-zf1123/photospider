/**
 * @file kernel_compute.cpp
 * @brief Implements Kernel compute facade entry points through normalized
 * request helpers.
 *
 * The public Kernel compute API remains stable. This file collects the shared
 * runtime-start, ComputeService construction, execution-mode branching, and
 * LastError mapping that used to be duplicated across compute, compute_async,
 * and compute_and_get_image.
 */
#include <future>
#include <sstream>
#include <string>
#include <utility>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/kernel.hpp"

namespace ps {
namespace {

/**
 * @brief Builds the legacy synchronous std::exception LastError message.
 *
 * @param request Compute request whose target node is being evaluated.
 * @param error_message Exception text reported by the compute boundary.
 * @return Message compatible with the previous Kernel::compute behavior.
 * @throws std::bad_alloc if string formatting cannot allocate.
 */
std::string make_sync_exception_message(int node_id,
                                        const char* error_message) {
  std::stringstream ss;
  ss << "std::exception during compute: " << error_message
     << " (while computing node " << node_id << ")";
  return ss.str();
}

/**
 * @brief Builds the async std::exception LastError message for both overloads.
 *
 * @param request Async request being executed.
 * @param error_message Exception text reported by the compute boundary.
 * @return Message compatible with the selected public async overload.
 * @throws std::bad_alloc if string formatting cannot allocate.
 * @note The legacy overload used the "std::exception:" prefix, while the
 * intent-aware overload used the "Async compute failed:" prefix. The request
 * intent flag preserves that externally visible distinction.
 */
std::string make_async_exception_message(bool intent_aware, int node_id,
                                         const char* error_message) {
  if (intent_aware) {
    return std::string("Async compute failed: ") + error_message;
  }

  std::stringstream ss;
  ss << "std::exception: " << error_message << " (while computing node "
     << node_id << ")";
  return ss.str();
}

/**
 * @brief Converts a Kernel request into the narrower ComputeService request.
 *
 * @param request Public Kernel compute request being dispatched.
 * @return Service request containing only target, cache, telemetry, intent, and
 * dirty ROI data.
 * @throws std::bad_alloc if copying the cache precision string allocates.
 * @note Graph name, quiet mode, skip-save, and scheduler selection stay in the
 * Kernel facade because they belong to GraphRuntime orchestration.
 */
ComputeService::Request make_service_compute_request(
    const Kernel::ComputeRequest& request) {
  return ComputeService::Request{
      request.node_id,
      ComputeService::CacheOptions{request.cache.precision,
                                   request.cache.force_recache,
                                   request.cache.disable_disk_cache},
      ComputeService::TelemetryOptions{request.telemetry.enable_timing,
                                       request.telemetry.benchmark_events},
      request.intent, request.dirty_roi};
}

/**
 * @brief Applies and restores per-request GraphModel compute flags.
 *
 * The guard records the graph's visible quiet and skip-save flags before a
 * Kernel compute request mutates them, applies the request values for the
 * active compute call, and restores the exact previous values when the compute
 * boundary exits.
 *
 * @param graph Graph whose request-scoped flags are temporarily changed.
 * @param request Kernel compute request supplying quiet and nosave values.
 * @throws Nothing directly; GraphModel flag setters are non-throwing.
 * @note The guard must be created inside GraphStateExecutor work so request
 * flag mutation is serialized with graph-state operations and with the compute
 * that reads those flags.
 */
class ScopedComputeRequestState {
 public:
  ScopedComputeRequestState(GraphModel& graph,
                            const Kernel::ComputeRequest& request)
      : graph_(graph),
        previous_quiet_(graph.is_quiet()),
        previous_skip_save_cache_(graph.skip_save_cache()) {
    graph_.set_quiet(request.execution.quiet);
    graph_.set_skip_save_cache(request.cache.nosave);
  }

  ScopedComputeRequestState(const ScopedComputeRequestState&) = delete;
  ScopedComputeRequestState& operator=(const ScopedComputeRequestState&) =
      delete;

  /**
   * @brief Restores the graph flags captured before the compute request.
   *
   * @throws Nothing.
   * @note Restoration runs for success, GraphError, std::exception, and
   * unknown exception paths because the guard lives on the stack inside the
   * submitted graph-state closure.
   */
  ~ScopedComputeRequestState() noexcept {
    graph_.set_skip_save_cache(previous_skip_save_cache_);
    graph_.set_quiet(previous_quiet_);
  }

 private:
  /** @brief Graph whose flags are restored by the destructor. */
  GraphModel& graph_;

  /** @brief Quiet flag observed before the request was applied. */
  bool previous_quiet_ = true;

  /** @brief Skip-save flag observed before the request was applied. */
  bool previous_skip_save_cache_ = false;
};

}  // namespace

bool Kernel::compute(const ComputeRequest& request) {
  return compute_request(request);
}

bool Kernel::compute_request(const ComputeRequest& request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return false;
  }

  try {
    auto& runtime = *it->second;
    if (!runtime.running()) {
      runtime.start();
    }

    runtime.graph_state()
        .submit([this, &runtime, request](GraphModel& graph) {
          ScopedComputeRequestState request_state(graph, request);
          ComputeService service(traversal_service_, cache_service_,
                                 runtime.event_service());
          (void)run_compute_request(service, runtime, graph, request);
          return 0;
        })
        .get();

    last_error_.erase(request.name);
    return true;
  } catch (const GraphError& ge) {
    last_error_[request.name] = {ge.code(), ge.what()};
    return false;
  } catch (const std::exception& e) {
    last_error_[request.name] = {
        GraphErrc::Unknown,
        make_sync_exception_message(request.node_id, e.what())};
    return false;
  } catch (...) {
    last_error_[request.name] = {GraphErrc::Unknown,
                                 std::string("unknown error")};
    return false;
  }
}

std::optional<cv::Mat> Kernel::compute_and_get_image(
    const ComputeRequest& request) {
  return compute_and_get_image_request(request);
}

std::optional<cv::Mat> Kernel::compute_and_get_image_request(
    const ComputeRequest& request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  try {
    NodeOutput output;
    auto& runtime = *it->second;
    if (!runtime.running()) {
      runtime.start();
    }

    output = runtime.graph_state()
                 .submit([this, &runtime, request](GraphModel& graph) {
                   ScopedComputeRequestState request_state(graph, request);
                   ComputeService service(traversal_service_, cache_service_,
                                          runtime.event_service());
                   return run_compute_request(service, runtime, graph, request);
                 })
                 .get();

    if (output.image_buffer.width == 0) {
      last_error_.erase(request.name);
      return std::nullopt;
    }
    last_error_.erase(request.name);
    return toCvMat(output.image_buffer).clone();
  } catch (const GraphError& ge) {
    last_error_[request.name] = {ge.code(), ge.what()};
    return std::nullopt;
  } catch (const std::exception& e) {
    last_error_[request.name] = {
        GraphErrc::Unknown,
        make_sync_exception_message(request.node_id, e.what())};
    return std::nullopt;
  } catch (...) {
    last_error_[request.name] = {GraphErrc::Unknown,
                                 std::string("unknown error")};
    return std::nullopt;
  }
}

std::optional<std::future<bool>> Kernel::compute_async(ComputeRequest request) {
  return compute_async_request(std::move(request));
}

std::optional<std::future<bool>> Kernel::compute_async_request(
    ComputeRequest request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  GraphRuntime* const runtime_ptr = it->second.get();
  if (!runtime_ptr->running()) {
    runtime_ptr->start();
  }
  return runtime_ptr->graph_state().submit(
      [this, runtime_ptr, request = std::move(request)](GraphModel& graph) {
        try {
          ScopedComputeRequestState request_state(graph, request);
          ComputeService service(traversal_service_, cache_service_,
                                 runtime_ptr->event_service());
          (void)run_compute_request(service, *runtime_ptr, graph, request);
          last_error_.erase(request.name);
          return true;
        } catch (const GraphError& ge) {
          last_error_[request.name] = {ge.code(), ge.what()};
          return false;
        } catch (const std::exception& e) {
          last_error_[request.name] = {
              GraphErrc::Unknown,
              make_async_exception_message(request.intent.has_value(),
                                           request.node_id, e.what())};
          return false;
        } catch (...) {
          last_error_[request.name] = {GraphErrc::Unknown,
                                       std::string("unknown error")};
          return false;
        }
      });
}

NodeOutput& Kernel::run_compute_request(ComputeService& compute_service,
                                        GraphRuntime& runtime,
                                        GraphModel& graph,
                                        const ComputeRequest& request) {
  const ComputeService::Request service_request =
      make_service_compute_request(request);
  if (request.execution.parallel) {
    return compute_service.compute_parallel(graph, runtime, service_request);
  }

  return compute_service.compute(graph, service_request);
}

}  // namespace ps
