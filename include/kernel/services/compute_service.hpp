#pragma once

#include <optional>
#include <unordered_map>

#include "graph_model.hpp"

namespace ps {

class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
struct BenchmarkEvent;

class ComputeService {
 public:
  ComputeService(GraphTraversalService& traversal, GraphCacheService& cache,
                 GraphEventService& events);

  NodeOutput& compute(GraphModel& graph, int node_id,
                      const std::string& cache_precision, bool force_recache,
                      bool enable_timing, bool disable_disk_cache,
                      std::vector<BenchmarkEvent>* benchmark_events);

  NodeOutput& compute(GraphModel& graph, ComputeIntent intent, int node_id,
                      const std::string& cache_precision, bool force_recache,
                      bool enable_timing, bool disable_disk_cache,
                      std::vector<BenchmarkEvent>* benchmark_events,
                      std::optional<cv::Rect> dirty_roi);

  NodeOutput& compute_parallel(GraphModel& graph, GraphRuntime& runtime,
                               int node_id, const std::string& cache_precision,
                               bool force_recache, bool enable_timing,
                               bool disable_disk_cache,
                               std::vector<BenchmarkEvent>* benchmark_events);

  NodeOutput& compute_parallel(GraphModel& graph, GraphRuntime& runtime,
                               ComputeIntent intent, int node_id,
                               const std::string& cache_precision,
                               bool force_recache, bool enable_timing,
                               bool disable_disk_cache,
                               std::vector<BenchmarkEvent>* benchmark_events,
                               std::optional<cv::Rect> dirty_roi);

  NodeOutput& compute_node_no_recurse(
      GraphModel& graph, int node_id, const std::string& cache_precision,
      bool enable_timing, bool allow_disk_cache,
      std::vector<BenchmarkEvent>* benchmark_events);

  NodeOutput& compute_internal(GraphModel& graph, int node_id,
                               const std::string& cache_precision,
                               std::unordered_map<int, bool>& visiting,
                               bool enable_timing, bool allow_disk_cache,
                               std::vector<BenchmarkEvent>* benchmark_events);

  NodeOutput& compute_high_precision_update(
      GraphModel& graph, GraphRuntime* runtime, int node_id,
      const std::string& cache_precision, bool force_recache,
      bool enable_timing, bool disable_disk_cache,
      std::vector<BenchmarkEvent>* benchmark_events, const cv::Rect& dirty_roi);

  NodeOutput& compute_real_time_update(
      GraphModel& graph, GraphRuntime* runtime, int node_id,
      const std::string& cache_precision, bool force_recache,
      bool enable_timing, bool disable_disk_cache,
      std::vector<BenchmarkEvent>* benchmark_events, const cv::Rect& dirty_roi);

  void clear_timing_results(GraphModel& graph);

 private:
  NodeOutput& compute_sequential_impl(
      GraphModel& graph, int node_id, const std::string& cache_precision,
      bool force_recache, bool enable_timing, bool disable_disk_cache,
      std::vector<BenchmarkEvent>* benchmark_events);

  NodeOutput& compute_with_intent_impl(
      GraphModel& graph, ComputeIntent intent, int node_id,
      const std::string& cache_precision, bool force_recache,
      bool enable_timing, bool disable_disk_cache,
      std::vector<BenchmarkEvent>* benchmark_events,
      std::optional<cv::Rect> dirty_roi);

  GraphTraversalService& traversal_;
  GraphCacheService& cache_;
  GraphEventService& events_;
};

}  // namespace ps
