#pragma once
#include "ps_types.hpp"
#include "node.hpp"
#include <opencv2/core.hpp>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include "benchmark/benchmark_types.hpp"

namespace ps {

// 前向声明，以避免循环依赖
class GraphRuntime;

struct NodeTiming {
    int id = -1;
    std::string name;
    double elapsed_ms = 0.0;
    std::string source; // "memory_cache", "disk_cache", or "computed"
};
struct TimingCollector {
    std::vector<NodeTiming> node_timings;
    double total_ms = 0.0;
};
// Concurrency model (high level):
// - 每个加载的图由一个GraphRuntime拥有，该Runtime维护一个工作线程池。
// - `compute()` 在调用线程上以单线程、深度优先的方式执行，用于简单的或调试性的计算。
// - `compute_parallel()` 将计算子图分解为微观任务，并提交给GraphRuntime的**工作窃取调度器**。
// - 工作线程从各自的本地队列（LIFO）获取任务，或从其他线程的队列（FIFO）窃取任务，以实现高性能并行计算。
// - `timing_results` 和 `event_buffer_` 由互斥锁保护，以支持跨线程的安全访问。

/**
 * @brief NodeGraph 管理计算图中的节点，处理节点的依赖、缓存、并行计算、时序收集和事件推送。
 *
 * 主要功能：
 * - 节点管理：添加(add_node)、查询(has_node)、清空(clear)节点。
 * - 缓存管理：
 *   - 内存缓存(clear_memory_cache, free_transient_memory)、磁盘缓存(clear_drive_cache, synchronize_disk_cache)。
 *   - 全量缓存(cache_all_nodes)、清理(clear_cache)。
 *   - 根据节点 id 获取缓存目录(node_cache_dir)。
 * - 图操作：
 *   - 加载(load_yaml)/保存(save_yaml)图数据于 YAML 文件。
 *   - 获取终止节点(ending_nodes)、拓扑后序遍历(topo_postorder_from)、打印依赖树(print_dependency_tree)。
 *   - 查找包含指定节点的子图(get_trees_containing_node)。
 * - 计算执行：
 *   - compute：单线程模式，支持强制刷新(force_recache)、计时(enable_timing)、禁用磁盘缓存(disable_disk_cache)。
 *   - compute_parallel：利用 `GraphRuntime` 的多线程工作窃取调度器，对子图任务进行高性能并行计算。
 *   - clear_timing_results：重置已收集的节点计时信息。
 * - 性能与事件：
 *   - timing_results：全局时序(TimingCollector)；total_io_time_ms：原子累计 I/O 时间。
 *   - drain_compute_events：提取并清空 ComputeEvent 缓冲，用于实时监控。
 *   - push_compute_event：内部线程安全推送节点计算事件。
 * - 内部工具方法（多线程安全）：
 *   - compute_internal：深度优先计算辅助，处理递归、缓存加载/保存、时序记录、基准事件(BenchmarkEvent)。
 *   - save_cache_if_configured / try_load_from_disk_cache：磁盘缓存读写。
 *   - is_ancestor / parents_of：判断依赖关系，避免循环。
 *
 * 线程安全：
 * - graph_mutex_：序列化图结构修改、全局缓存清理等操作。
 * - event_mutex_：保护事件缓冲区并发访问。
 * - timing_mutex_：保护 timing_results 并发写入。
 */
class NodeGraph {
public:
    TimingCollector timing_results;
    std::unordered_map<int, Node> nodes;
    fs::path cache_root;

    explicit NodeGraph(fs::path cache_root_dir = "cache") : cache_root(std::move(cache_root_dir)) {
        if (!cache_root.empty()) {
            fs::create_directories(cache_root);
        }
    }

    void set_quiet(bool q) { quiet_ = q; }
    bool is_quiet() const { return quiet_; }

    void clear();
    void add_node(const Node& node);
    bool has_node(int id) const;

    void load_yaml(const fs::path& yaml_path);
    void save_yaml(const fs::path& yaml_path) const;
    struct DriveClearResult { uintmax_t removed_entries = 0; };
    struct MemoryClearResult { int cleared_nodes = 0; };
    struct CacheSaveResult { int saved_nodes = 0; };
    struct DiskSyncResult { int saved_nodes = 0; int removed_files = 0; int removed_dirs = 0; };

    DriveClearResult clear_drive_cache();
    MemoryClearResult clear_memory_cache();
    void clear_cache();
    CacheSaveResult cache_all_nodes(const std::string& cache_precision);
    MemoryClearResult free_transient_memory();
    DiskSyncResult synchronize_disk_cache(const std::string& cache_precision);
    fs::path node_cache_dir(int node_id) const;

    NodeOutput& compute(int node_id, const std::string& cache_precision,
                        bool force_recache = false, bool enable_timing = false,
                        bool disable_disk_cache = false,
                        std::vector<BenchmarkEvent>* benchmark_events = nullptr);
    // Phase 1: Intent-driven overload (keeps old API intact)
    NodeOutput& compute(ComputeIntent intent,
                        int node_id, const std::string& cache_precision,
                        bool force_recache = false, bool enable_timing = false,
                        bool disable_disk_cache = false,
                        std::vector<BenchmarkEvent>* benchmark_events = nullptr,
                        std::optional<cv::Rect> dirty_roi = std::nullopt);

    NodeOutput& compute_parallel(GraphRuntime& runtime, int node_id, const std::string& cache_precision,
                                 bool force_recache = false, bool enable_timing = false,
                                 bool disable_disk_cache = false,
                                 std::vector<BenchmarkEvent>* benchmark_events = nullptr);
    // Phase 1: Intent-driven overload (keeps old API intact)
    NodeOutput& compute_parallel(GraphRuntime& runtime, ComputeIntent intent,
                                 int node_id, const std::string& cache_precision,
                                 bool force_recache = false, bool enable_timing = false,
                                 bool disable_disk_cache = false,
                                 std::vector<BenchmarkEvent>* benchmark_events = nullptr,
                                 std::optional<cv::Rect> dirty_roi = std::nullopt);
    // Used by parallel scheduler: compute a single node assuming its dependencies are ready.
    NodeOutput& compute_node_no_recurse(int node_id, const std::string& cache_precision,
                                        bool enable_timing, bool allow_disk_cache,
                                        std::vector<BenchmarkEvent>* benchmark_events = nullptr);
    void clear_timing_results();
    std::vector<int> ending_nodes() const;
    void print_dependency_tree(std::ostream& os, bool show_parameters = true) const;
    void print_dependency_tree(std::ostream& os, int start_node_id, bool show_parameters = true) const;
    std::vector<int> topo_postorder_from(int end_node_id) const;
    std::vector<int> get_trees_containing_node(int node_id) const;

    struct ComputeEvent { int id; std::string name; std::string source; double elapsed_ms; };
    std::vector<ComputeEvent> drain_compute_events();
    
    std::atomic<double> total_io_time_ms{0.0};

    // 控制本次计算是否跳过磁盘保存（由上层临时设置）
    void set_skip_save_cache(bool v) { skip_save_cache_.store(v, std::memory_order_relaxed); }

public: 
    // ** INTERNAL USE ONLY ** - Public for lambda access in parallel scheduler
    NodeOutput& compute_internal(int node_id, const std::string& cache_precision, 
                                 std::unordered_map<int, bool>& visiting, bool enable_timing, 
                                 bool allow_disk_cache,
                                 std::vector<BenchmarkEvent>* benchmark_events);
private:
    NodeOutput& compute_high_precision_update(GraphRuntime* runtime,
                                              int node_id,
                                              const std::string& cache_precision,
                                              bool force_recache,
                                              bool enable_timing,
                                              bool disable_disk_cache,
                                              std::vector<BenchmarkEvent>* benchmark_events,
                                              const cv::Rect& dirty_roi);
    NodeOutput& compute_real_time_update(GraphRuntime* runtime,
                                         int node_id,
                                         const std::string& cache_precision,
                                         bool force_recache,
                                         bool enable_timing,
                                         bool disable_disk_cache,
                                         std::vector<BenchmarkEvent>* benchmark_events,
                                         const cv::Rect& dirty_roi);

    bool is_ancestor(int potential_ancestor_id, int node_id, std::unordered_set<int>& visited) const; 
    std::vector<int> parents_of(int node_id) const;

    void save_cache_if_configured(const Node& node, const std::string& cache_precision) const;
    bool try_load_from_disk_cache(Node& node);
    // Pure load helper for parallel scheme B: load disk cache into an output without mutating the node.
    bool try_load_from_disk_cache_into(const Node& node, NodeOutput& out) const;
    
    std::mutex graph_mutex_;
    bool quiet_ = true;
    
    std::mutex event_mutex_;
    std::vector<ComputeEvent> event_buffer_;
    void push_compute_event(int id, const std::string& name, const std::string& source, double ms);
    
    mutable std::mutex timing_mutex_;

    std::atomic<bool> skip_save_cache_{false};
};

} // namespace ps
