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

    auto& model = runtime.model();
    runtime.graph_state()
        .submit([nosave = request.cache.nosave](GraphModel& graph) {
          graph.set_skip_save_cache(nosave);
          return 0;
        })
        .get();
    model.set_quiet(request.execution.quiet);

    ComputeService compute_service(traversal_service_, cache_service_,
                                   runtime.event_service());
    if (request.execution.parallel) {
      (void)run_compute_request(compute_service, runtime, model, request);
    } else {
      runtime.graph_state()
          .submit([this, &runtime, request](GraphModel& graph) {
            ComputeService service(traversal_service_, cache_service_,
                                   runtime.event_service());
            (void)run_compute_request(service, runtime, graph, request);
            return 0;
          })
          .get();
    }

    last_error_.erase(request.name);
    runtime.graph_state()
        .submit([](GraphModel& graph) {
          graph.set_skip_save_cache(false);
          return 0;
        })
        .get();
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

    ComputeService compute_service(traversal_service_, cache_service_,
                                   runtime.event_service());
    if (request.execution.parallel) {
      output = run_compute_request(compute_service, runtime, runtime.model(),
                                   request);
    } else {
      output =
          runtime.graph_state()
              .submit([this, &runtime,
                       request](GraphModel& graph) -> NodeOutput {
                ComputeService service(traversal_service_, cache_service_,
                                       runtime.event_service());
                return run_compute_request(service, runtime, graph, request);
              })
              .get();
    }

    if (output.image_buffer.width == 0) {
      return std::nullopt;
    }
    return toCvMat(output.image_buffer).clone();
  } catch (...) {
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
  if (request.execution.parallel) {
    return std::optional<std::future<bool>>(std::async(
        std::launch::async,
        [this, runtime_ptr, request = std::move(request)]() {
          try {
            if (!runtime_ptr->running()) {
              runtime_ptr->start();
            }

            GraphModel& model = runtime_ptr->model();
            ComputeService compute_service(traversal_service_, cache_service_,
                                           runtime_ptr->event_service());
            const bool prev_quiet = model.is_quiet();
            model.set_quiet(request.execution.quiet);
            model.set_skip_save_cache(request.cache.nosave);
            (void)run_compute_request(compute_service, *runtime_ptr, model,
                                      request);
            model.set_skip_save_cache(false);
            model.set_quiet(prev_quiet);
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
          }
        }));
  }

  if (!runtime_ptr->running()) {
    runtime_ptr->start();
  }
  return runtime_ptr->graph_state().submit(
      [this, runtime_ptr, request = std::move(request)](GraphModel& model) {
        try {
          ComputeService compute_service(traversal_service_, cache_service_,
                                         runtime_ptr->event_service());
          const bool prev_quiet = model.is_quiet();
          model.set_quiet(request.execution.quiet);
          model.set_skip_save_cache(request.cache.nosave);
          (void)run_compute_request(compute_service, *runtime_ptr, model,
                                    request);
          model.set_skip_save_cache(false);
          model.set_quiet(prev_quiet);
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
