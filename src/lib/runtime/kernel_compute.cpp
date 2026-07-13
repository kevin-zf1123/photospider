/**
 * @file kernel_compute.cpp
 * @brief Implements internal Kernel compute entry points through normalized
 * adapter request helpers.
 *
 * The adapter-to-Kernel compute boundary remains stable behind `ps::Host`.
 * This file collects the shared runtime-start, ComputeService construction,
 * execution-mode branching, and LastError mapping that used to be duplicated
 * across compute, compute_async, and compute_and_get_image. Frontends construct
 * `HostComputeRequest`; the embedded adapter translates it to
 * `Kernel::ComputeRequest` before these helpers run.
 */
#include <future>
#include <new>
#include <sstream>
#include <string>
#include <utility>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "runtime/kernel.hpp"

namespace ps {
namespace {

/**
 * @brief Builds the legacy synchronous std::exception LastError message.
 *
 * @param node_id Target node being evaluated by the internal Kernel request.
 * @param error_message Exception text reported by the compute boundary.
 * @return Message compatible with the previous Kernel::compute behavior.
 * @throws std::bad_alloc if string formatting cannot allocate.
 * @note The embedded Host adapter converts the stored LastError into a public
 * Host status; frontend code never calls this helper.
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
 * @param intent_aware Whether the adapter supplied explicit compute intent.
 * @param node_id Target node being evaluated by the internal Kernel request.
 * @param error_message Exception text reported by the compute boundary.
 * @return Message compatible with the selected internal async path and Host
 * status translation.
 * @throws std::bad_alloc if string formatting cannot allocate.
 * @note The legacy overload used the "std::exception:" prefix, while the
 * intent-aware overload used the "Async compute failed:" prefix. The request
 * intent flag preserves the distinction translated by the Host adapter.
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
 * @param request Internal Kernel compute request produced by the embedded Host
 * adapter or an internal backend/test caller.
 * @return Service request containing only target, cache, telemetry, intent, and
 * dirty ROI data.
 * @throws std::bad_alloc if copying the cache precision string allocates.
 * @note Graph name, quiet mode, skip-save, and scheduler selection stay in the
 * internal Kernel boundary because they belong to GraphRuntime orchestration.
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

/**
 * @brief Delegates the internal synchronous facade to compute_request().
 *
 * @param request Structured internal compute request.
 * @return true on success; false on missing graph or handled compute failure.
 * @throws std::bad_alloc if compute execution or handled-failure LastError
 *         construction exhausts memory.
 * @note Other GraphError, standard, and unknown failures are converted by
 *       compute_request() to false plus best-effort LastError state.
 */
bool Kernel::compute(const ComputeRequest& request) {
  return compute_request(request);
}

/**
 * @brief Executes one synchronous request under graph-state serialization.
 *
 * @param request Internal request captured into the graph-state work item.
 * @return true when the service finishes; false for missing graphs or handled
 *         failures.
 * @throws std::bad_alloc if compute execution or catch-path LastError
 *         construction exhausts memory.
 * @note Runtime start, request-state mutation, and compute all occur inside one
 *       GraphStateExecutor work item. The request-state guard restores flags on
 *       every exit; other compute exceptions map to false and LastError.
 */
bool Kernel::compute_request(const ComputeRequest& request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return false;
  }

  try {
    auto& runtime = *it->second;
    runtime.graph_state()
        .submit([this, &runtime, request](GraphModel& graph) {
          if (!runtime.running()) {
            runtime.start();
          }
          ScopedComputeRequestState request_state(graph, request);
          ComputeService service(traversal_service_, cache_service_,
                                 runtime.event_service());
          (void)run_compute_request(service, runtime, graph, request);
          return 0;
        })
        .get();

    clear_last_error(request.name);
    return true;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& ge) {
    store_last_error(request.name, LastError{ge.code(), ge.what()});
    return false;
  } catch (const std::exception& e) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             make_sync_exception_message(
                                                 request.node_id, e.what())});
    return false;
  } catch (...) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             std::string("unknown error")});
    return false;
  }
}

/**
 * @brief Delegates image compute to compute_and_get_image_request().
 *
 * @param request Structured internal image compute request.
 * @return Cloned image or nullopt under the internal preview/save contract.
 * @throws std::bad_alloc if compute/image execution or handled-failure
 *         LastError construction exhausts memory.
 * @note Other compute and image-conversion failures become nullopt.
 */
std::optional<cv::Mat> Kernel::compute_and_get_image(
    const ComputeRequest& request) {
  return compute_and_get_image_request(request);
}

/**
 * @brief Computes and clones one node image under graph-state serialization.
 *
 * @param request Internal image compute request captured into serialized work.
 * @return Cloned image, or nullopt for missing graph, handled failure, or empty
 *         output.
 * @throws std::bad_alloc if compute/image execution or catch-path LastError
 *         construction exhausts memory.
 * @note Runtime start and compute share one graph-state work item. Other
 *       compute, adapter, and clone exceptions become nullopt; successful empty
 *       output clears stale LastError state.
 */
std::optional<cv::Mat> Kernel::compute_and_get_image_request(
    const ComputeRequest& request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  try {
    NodeOutput output;
    auto& runtime = *it->second;
    output = runtime.graph_state()
                 .submit([this, &runtime, request](GraphModel& graph) {
                   if (!runtime.running()) {
                     runtime.start();
                   }
                   ScopedComputeRequestState request_state(graph, request);
                   ComputeService service(traversal_service_, cache_service_,
                                          runtime.event_service());
                   return run_compute_request(service, runtime, graph, request);
                 })
                 .get();

    if (output.image_buffer.width == 0) {
      clear_last_error(request.name);
      return std::nullopt;
    }
    clear_last_error(request.name);
    return toCvMat(output.image_buffer).clone();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& ge) {
    store_last_error(request.name, LastError{ge.code(), ge.what()});
    return std::nullopt;
  } catch (const std::exception& e) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             make_sync_exception_message(
                                                 request.node_id, e.what())});
    return std::nullopt;
  } catch (...) {
    store_last_error(request.name, LastError{GraphErrc::Unknown,
                                             std::string("unknown error")});
    return std::nullopt;
  }
}

/**
 * @brief Delegates asynchronous compute to compute_async_request().
 *
 * @param request Internal request transferred into asynchronous state.
 * @return Future resolving to the work item's owned exact result, or nullopt
 *         for a missing graph.
 * @throws std::bad_alloc if request, task, queue, or future-state allocation
 *         fails during submission.
 * @throws std::system_error if graph-state asynchronous work cannot launch.
 * @note Future get() may rethrow std::bad_alloc from compute execution or
 *       exact diagnostic allocation.
 */
std::optional<std::future<Kernel::AsyncComputeResult>> Kernel::compute_async(
    ComputeRequest request) {
  return compute_async_request(std::move(request));
}

/**
 * @brief Submits one compute request to the graph-state executor.
 *
 * @param request Internal request moved into the submitted work item.
 * @return Future resolving to the work item's immutable success or exact owned
 *         error, or nullopt for a missing graph.
 * @throws std::bad_alloc if request, task, queue, or future-state allocation
 *         fails during submission.
 * @throws std::system_error if graph-state asynchronous work cannot launch.
 * @note Runtime start and compute execute inside the submitted graph-state work
 *       item. Recoverable exceptions are captured in the returned result and
 *       mirrored into LastError for diagnostics; future get() rethrows
 *       std::bad_alloc from compute execution or diagnostic allocation.
 */
std::optional<std::future<Kernel::AsyncComputeResult>>
Kernel::compute_async_request(ComputeRequest request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  GraphRuntime* const runtime_ptr = it->second.get();
  return runtime_ptr->graph_state().submit(
      [this, runtime_ptr, request = std::move(request)](GraphModel& graph) {
        try {
          if (!runtime_ptr->running()) {
            runtime_ptr->start();
          }
          ScopedComputeRequestState request_state(graph, request);
          ComputeService service(traversal_service_, cache_service_,
                                 runtime_ptr->event_service());
          (void)run_compute_request(service, *runtime_ptr, graph, request);
          clear_last_error(request.name);
          return AsyncComputeResult{true, std::nullopt};
        } catch (const std::bad_alloc&) {
          throw;
        } catch (const GraphError& ge) {
          LastError error{ge.code(), ge.what()};
          store_last_error(request.name, error);
          return AsyncComputeResult{false, std::move(error)};
        } catch (const std::exception& e) {
          LastError error{GraphErrc::Unknown, make_async_exception_message(
                                                  request.intent.has_value(),
                                                  request.node_id, e.what())};
          store_last_error(request.name, error);
          return AsyncComputeResult{false, std::move(error)};
        } catch (...) {
          LastError error{GraphErrc::Unknown, std::string("unknown error")};
          store_last_error(request.name, error);
          return AsyncComputeResult{false, std::move(error)};
        }
      });
}

/**
 * @brief Translates and dispatches one request to ComputeService.
 *
 * @param compute_service Request-scoped compute collaborator.
 * @param runtime Runtime supplying scheduler and event services.
 * @param graph Visible graph model owned by serialized graph-state work.
 * @param request Internal Kernel request to translate.
 * @return Mutable output owned by the selected node cache.
 * @throws GraphError if ComputeService rejects the graph request.
 * @throws std::bad_alloc if request translation or ComputeService exhausts
 *         memory.
 * @throws std::exception for other failures propagated by ComputeService.
 * @note Parallel selection changes dispatch policy, not request ownership.
 */
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
