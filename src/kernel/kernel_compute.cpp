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

}  // namespace

bool Kernel::compute(const std::string& name, int node_id,
                     const std::string& cache_precision, bool force_recache,
                     bool enable_timing, bool parallel, bool quiet,
                     bool disable_disk_cache, bool nosave,
                     std::vector<BenchmarkEvent>* benchmark_events) {
  return compute_request(
      ComputeRequest{name, node_id, cache_precision, force_recache,
                     enable_timing, parallel, quiet, disable_disk_cache, nosave,
                     benchmark_events, std::nullopt, std::nullopt});
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
        .submit([nosave = request.nosave](GraphModel& graph) {
          graph.set_skip_save_cache(nosave);
          return 0;
        })
        .get();
    model.set_quiet(request.quiet);

    ComputeService compute_service(traversal_service_, cache_service_,
                                   runtime.event_service());
    if (request.parallel) {
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
    const std::string& name, int node_id, const std::string& cache_precision,
    bool force_recache, bool enable_timing, bool parallel,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events) {
  return compute_and_get_image_request(
      ComputeRequest{name, node_id, cache_precision, force_recache,
                     enable_timing, parallel, false, disable_disk_cache, false,
                     benchmark_events, std::nullopt, std::nullopt});
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
    if (request.parallel) {
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

std::optional<std::future<bool>> Kernel::compute_async(
    const std::string& name, int node_id, const std::string& cache_precision,
    bool force_recache, bool enable_timing, bool parallel, bool quiet,
    bool disable_disk_cache, bool nosave,
    std::vector<BenchmarkEvent>* benchmark_events) {
  return compute_async_request(
      ComputeRequest{name, node_id, cache_precision, force_recache,
                     enable_timing, parallel, quiet, disable_disk_cache, nosave,
                     benchmark_events, std::nullopt, std::nullopt});
}

std::optional<std::future<bool>> Kernel::compute_async(
    const std::string& name, int node_id, const std::string& cache_precision,
    bool force_recache, bool enable_timing, bool parallel, bool quiet,
    bool disable_disk_cache, bool nosave,
    std::vector<BenchmarkEvent>* benchmark_events, ComputeIntent intent,
    std::optional<cv::Rect> dirty_roi) {
  return compute_async_request(ComputeRequest{
      name, node_id, cache_precision, force_recache, enable_timing, parallel,
      quiet, disable_disk_cache, nosave, benchmark_events, intent, dirty_roi});
}

std::optional<std::future<bool>> Kernel::compute_async_request(
    ComputeRequest request) {
  auto it = graphs_.find(request.name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }

  GraphRuntime* const runtime_ptr = it->second.get();
  if (request.parallel) {
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
            model.set_quiet(request.quiet);
            model.set_skip_save_cache(request.nosave);
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
          model.set_quiet(request.quiet);
          model.set_skip_save_cache(request.nosave);
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
  if (request.parallel) {
    if (request.intent) {
      return compute_service.compute_parallel(
          graph, runtime, *request.intent, request.node_id,
          request.cache_precision, request.force_recache, request.enable_timing,
          request.disable_disk_cache, request.benchmark_events,
          request.dirty_roi);
    }
    return compute_service.compute_parallel(
        graph, runtime, request.node_id, request.cache_precision,
        request.force_recache, request.enable_timing,
        request.disable_disk_cache, request.benchmark_events);
  }

  if (request.intent) {
    return compute_service.compute(graph, *request.intent, request.node_id,
                                   request.cache_precision,
                                   request.force_recache, request.enable_timing,
                                   request.disable_disk_cache,
                                   request.benchmark_events, request.dirty_roi);
  }
  return compute_service.compute(
      graph, request.node_id, request.cache_precision, request.force_recache,
      request.enable_timing, request.disable_disk_cache,
      request.benchmark_events);
}

}  // namespace ps
