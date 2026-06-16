#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/services/compute-service/task_graph_planning.hpp"
#include "node.hpp"      // NOLINT(build/include_subdir)
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {

class GraphCacheService;
class GraphIOService;
class GraphTraversalService;
class ComputeService;
namespace compute {
class ComputeTaskDispatcher;
class DownsampleExecutor;
class HighPrecisionDirtyExecutor;
class RealTimeDirtyExecutor;
}  // namespace compute

struct NodeTiming {
  int id = -1;
  std::string name;
  double elapsed_ms = 0.0;
  std::string source;
};

struct TimingCollector {
  std::vector<NodeTiming> node_timings;
  double total_ms = 0.0;
};

enum class GraphTopologyEdgeKind {
  ImageInput,
  ParameterInput,
};

struct GraphTopologyEdge {
  int from_node_id = -1;
  int to_node_id = -1;
  GraphTopologyEdgeKind kind = GraphTopologyEdgeKind::ImageInput;
  std::string from_output_name;
  std::string to_input_name;
  size_t input_index = 0;
};

struct GraphTopologyIndex {
  std::unordered_map<int, std::vector<GraphTopologyEdge>> incoming_by_node;
  std::unordered_map<int, std::vector<GraphTopologyEdge>> outgoing_by_node;

  void clear();
  bool empty() const;
};

class GraphModel {
 public:
  using NodeMap = std::unordered_map<int, Node>;

  TimingCollector timing_results;
  fs::path cache_root;
  std::optional<std::string> last_dirty_region_snapshot_debug;
  std::optional<compute::DirtyRegionSnapshot> last_dirty_region_snapshot;
  std::vector<compute::DirtyRegionSnapshot> recent_dirty_region_snapshots;
  uint64_t dirty_generation_counter = 0;
  std::unordered_map<int, uint64_t> dirty_source_hp_commit_generation;
  std::unordered_map<int, uint64_t> dirty_source_rt_commit_generation;
  std::optional<compute::ComputePlan> last_compute_plan;
  std::vector<compute::ComputePlan> recent_compute_plans;

  struct DriveClearResult {
    uintmax_t removed_entries = 0;
  };
  struct MemoryClearResult {
    int cleared_nodes = 0;
  };
  struct CacheSaveResult {
    int saved_nodes = 0;
  };
  struct DiskSyncResult {
    int saved_nodes = 0;
    int removed_files = 0;
    int removed_dirs = 0;
  };

  /**
   * @brief Status of the most recent disk-cache load attempt.
   *
   * The status is diagnostic state recorded by GraphCacheService. It separates
   * true misses from skipped attempts, successful disk hits, and read/parse
   * failures while preserving the legacy bool-returning cache-load API.
   *
   * @note Status values describe only disk-cache loading. In-memory HP cache
   * reuse can cause a load attempt to be skipped while the legacy wrapper still
   * returns true because the node already has reusable output.
   */
  enum class DiskCacheLoadStatus {
    Skipped,
    Miss,
    Hit,
    Error,
  };

  /**
   * @brief Diagnostic record for one disk-cache load attempt.
   *
   * The record captures the node, cache entry, resolved disk paths, status,
   * optional error code, and human-readable message produced by
   * GraphCacheService. It is intentionally value-type state so tests,
   * frontends, and future event/LastError bridges can inspect the last attempt
   * without owning cached image data.
   *
   * @note `code` is meaningful when `status == DiskCacheLoadStatus::Error`.
   * For misses, hits, and skipped attempts it remains `GraphErrc::Unknown`.
   */
  struct DiskCacheLoadResult {
    /** @brief Node id whose disk-cache entry was evaluated. */
    int node_id = -1;
    /** @brief Cache type from the selected CacheEntry, usually `image`. */
    std::string cache_type;
    /** @brief Cache location from the selected CacheEntry. */
    std::string location;
    /** @brief Resolved image cache path, when an image cache was considered. */
    fs::path cache_file;
    /** @brief Resolved YAML metadata path paired with the image cache path. */
    fs::path metadata_file;
    /** @brief Outcome category for the attempt. */
    DiskCacheLoadStatus status = DiskCacheLoadStatus::Skipped;
    /** @brief Error category for read/parse failures. */
    GraphErrc code = GraphErrc::Unknown;
    /** @brief Reader-facing diagnostic message for the attempt. */
    std::string message;
  };

  std::optional<DiskCacheLoadResult> last_disk_cache_load_result;

  struct NodeRuntimeState {
    YAML::Node& runtime_parameters;
    std::vector<OutputPort>& outputs;
    std::vector<CacheEntry>& caches;
    bool& preserved;
    std::optional<NodeOutput>& cached_output_real_time;
    std::optional<NodeOutput>& cached_output_high_precision;
    int& rt_version;
    int& hp_version;
    std::optional<cv::Rect>& rt_roi;
    std::optional<cv::Rect>& hp_roi;
    std::optional<cv::Size>& last_input_size_hp;
    std::optional<SpatialDependencyMap>& dependency_lut;
    uint64_t& dependency_lut_version;
    uint64_t& parameters_version;
  };

  explicit GraphModel(fs::path cache_root_dir = "cache");

  void set_quiet(bool q);
  bool is_quiet() const;

  void clear();
  void add_node(const Node& node);
  void replace_node(const Node& node);
  void remove_node(int id);
  void rewire_image_input(int node_id, size_t input_index, int from_node_id,
                          std::string from_output_name);
  void rewire_parameter_input(int node_id, size_t input_index, int from_node_id,
                              std::string from_output_name,
                              std::string to_parameter_name);
  void replace_nodes(NodeMap nodes);
  bool has_node(int id) const;
  bool empty() const;
  size_t node_count() const;
  std::vector<int> node_ids() const;
  const Node* find_node(int id) const;
  const Node& node(int id) const;
  void mutate_node_runtime_state(
      int id, const std::function<void(NodeRuntimeState&)>& mutator);
  void for_each_node(const std::function<void(const Node&)>& visitor) const;
  void validate_topology() const;
  void rebuild_topology_index();
  const GraphTopologyIndex& topology() const;
  const std::vector<GraphTopologyEdge>& upstream_edges(int node_id) const;
  const std::vector<GraphTopologyEdge>& downstream_edges(int node_id) const;

  void set_skip_save_cache(bool v);
  bool skip_save_cache() const;

  std::atomic<double> total_io_time_ms{0.0};

 private:
  friend class GraphCacheService;
  friend class GraphIOService;
  friend class GraphTraversalService;
  friend class ComputeService;
  friend class compute::ComputeTaskDispatcher;
  friend class compute::DownsampleExecutor;
  friend class compute::HighPrecisionDirtyExecutor;
  friend class compute::RealTimeDirtyExecutor;

  void reset_runtime_state();
  Node* find_node_mutable(int id);
  Node& mutable_node(int id);

  NodeMap nodes_;
  GraphTopologyIndex topology_;
  std::mutex graph_mutex_;
  mutable std::mutex timing_mutex_;
  bool quiet_ = true;
  std::atomic<bool> skip_save_cache_{false};
};

}  // namespace ps
