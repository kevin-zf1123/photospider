#pragma once
#include "ps_types.hpp"
#include "node.hpp"
#include <unordered_set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include "kernel/benchmark_types.hpp"

namespace ps {
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
// - Each loaded graph is owned by a GraphRuntime that serializes API calls via a worker thread.
// - compute() runs on that worker thread and may recurse; it is single-threaded.
// - compute_parallel() temporarily spawns a per-invocation worker pool to evaluate the subgraph in parallel,
//   then joins all threads before returning. It uses its own internal queue + mutex/CV (separate from graph_mutex_).
// - timing_results is mutated from both sequential and parallel paths; protect with timing_mutex_.
// - event_buffer_ is a cross-thread stream used by the CLI; protect with event_mutex_.
// - graph_mutex_ guards occasional coarse operations over the whole graph (e.g., mass cache resets for force_recache).

/**
 * @brief NodeGraph 类用于管理节点图，包括节点数据管理、磁盘和内存缓存的处理、以及节点间依赖关系的计算与调度。
 *
 * 主要功能：
 * - 管理节点（nodes）的添加、删除和查询。
 * - 配置和管理磁盘缓存目录（cache_root），并在构造时自动创建目录（如果非空）。
 * - 提供 YAML 格式的加载与保存功能，便于持久化节点数据。
 * - 提供节点计算接口，支持单线程和并行计算，并可通过参数控制是否强制重新缓存、是否启用计时、以及是否禁用磁盘缓存。
 * - 提供缓存清理功能，包括清空磁盘缓存、内存缓存以及整体缓存操作。
 * - 支持节点依赖树的打印和拓扑排序，帮助理解节点间的依赖关系。
 * - 收集和导出计算和基准测试事件，便于性能分析与调试。
 *
 * 类成员变量：
 * - TimingCollector timing_results: 用于记录和收集计算过程中的时序信息。
 * - std::unordered_map<int, Node> nodes: 用于存放各个节点，键为节点 id，值为对应的 Node 对象。
 * - fs::path cache_root: 表示磁盘缓存的根目录，用于存储每个节点对应的缓存数据。
 * - bool quiet_: 控制标准输出的冗余信息，静默模式下减少输出。
 *
 * 主要公有方法：
 * - set_quiet(bool q) / is_quiet(): 用于设置和检索静默模式状态，控制标准输出信息。
 * - clear(): 清除节点图中所有的节点数据及相关状态。
 * - add_node(const Node& node): 将新节点添加到节点图中。
 * - has_node(int id): 查询节点图中是否存在指定 id 的节点。
 *
 * YAML 文件操作：
 * - load_yaml(const fs::path& yaml_path): 从指定 YAML 文件加载节点图数据。
 * - save_yaml(const fs::path& yaml_path): 将当前节点图数据保存到指定 YAML 文件中。
 *
 * 缓存操作：
 * - DriveClearResult clear_drive_cache(): 清除磁盘缓存，并返回移除的条目数量。
 * - MemoryClearResult clear_memory_cache(): 清除内存中缓存的节点数据，并返回清理的节点数量。
 * - clear_cache(): 同时清除内存和磁盘缓存的数据。
 * - CacheSaveResult cache_all_nodes(const std::string& cache_precision): 缓存所有节点信息，返回保存的节点数量。
 * - MemoryClearResult free_transient_memory(): 释放临时内存，返回释放的节点数量。
 * - DiskSyncResult synchronize_disk_cache(const std::string& cache_precision): 同步磁盘缓存数据，返回保存、移除文件和目录的数目。
 * - fs::path node_cache_dir(int node_id) const: 根据节点 id 返回对应的缓存目录路径。
 *
 * 计算相关操作：
 * - NodeOutput& compute(...): 对指定节点执行计算操作，支持强制重新缓存、计时以及禁用磁盘缓存的功能，并可接受基准测试事件参数。
 * - NodeOutput& compute_parallel(...): 与 compute 类似，但支持并行计算，适用于多线程环境。
 * - clear_timing_results(): 清空已收集的计时信息。
 *
 * 依赖关系和调试工具：
 * - std::vector<int> ending_nodes() const: 获取所有结束计算的节点 id。
 * - print_dependency_tree(...): 将节点间的依赖树打印到输出流中，支持显示参数信息或从指定节点开始打印。
 * - topo_postorder_from(int end_node_id) const: 返回从指定节点开始的拓扑后序遍历序列。
 * - get_trees_containing_node(int node_id) const: 查找所有包含指定节点的依赖树。
 *
 * 事件记录：
 * - drain_compute_events(): 提取并清空存储的计算事件，返回事件列表，用于实时监控和调试。
 * - push_compute_event(...): 内部方法，用于将计算事件推送至事件缓冲区（线程安全）。
 *
 * 内部辅助函数：
 * - compute_internal(...): 递归执行计算的深度优先搜索辅助函数，允许或禁用磁盘缓存，并支持计时和基准测试事件传递。
 * - is_ancestor(...): 判断某个节点是否为另一个节点的祖先，以避免依赖循环。
 * - parents_of(int node_id) const: 返回指定节点的父节点列表。
 * - save_cache_if_configured(...): 根据配置判断是否保存节点的缓存数据到磁盘。
 * - try_load_from_disk_cache(Node& node): 尝试从磁盘加载节点缓存数据。
 * - execute_op_for_node(...): 根据节点 id 执行节点特定的操作（计算逻辑）。
 *
 * 线程安全性：
 * - 使用 std::mutex 对图数据、事件和计时信息等关键部分进行保护，以确保多线程并行计算时的数据一致性和安全性。
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

    // Control stdout verbosity for compute/save messages.
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

    // disable_disk_cache: when true, do not load from disk caches during this compute.
    NodeOutput& compute(int node_id, const std::string& cache_precision,
                        bool force_recache = false, bool enable_timing = false,
                        bool disable_disk_cache = false,
                        std::vector<BenchmarkEvent>* benchmark_events = nullptr); // [新增] benchmark 参数

    NodeOutput& compute_parallel(int node_id, const std::string& cache_precision,
                                 bool force_recache = false, bool enable_timing = false,
                                 bool disable_disk_cache = false,
                                 std::vector<BenchmarkEvent>* benchmark_events = nullptr); // [新增] benchmark 参数
    void clear_timing_results();
    std::vector<int> ending_nodes() const;
    void print_dependency_tree(std::ostream& os, bool show_parameters = true) const;
    void print_dependency_tree(std::ostream& os, int start_node_id, bool show_parameters = true) const;
    std::vector<int> topo_postorder_from(int end_node_id) const;
    std::vector<int> get_trees_containing_node(int node_id) const;
    // Streaming compute events (separate from timers)
    struct ComputeEvent { int id; std::string name; std::string source; double elapsed_ms; };
    std::vector<ComputeEvent> drain_compute_events();
    // [新增] 用于累加IO时间的原子变量
    std::atomic<double> total_io_time_ms{0.0};
private:
    // Internal DFS compute helper; allow_disk_cache controls whether disk caches may be used.
    NodeOutput& compute_internal(int node_id, const std::string& cache_precision, 
                                 std::unordered_map<int, bool>& visiting, bool enable_timing, 
                                 bool allow_disk_cache,
                                 std::vector<BenchmarkEvent>* benchmark_events); // [新增] benchmark 参数
    bool is_ancestor(int potential_ancestor_id, int node_id, std::unordered_set<int>& visited) const; 
    std::vector<int> parents_of(int node_id) const;

    void save_cache_if_configured(const Node& node, const std::string& cache_precision) const;
    bool try_load_from_disk_cache(Node& node);
    void execute_op_for_node(int node_id, const std::string& cache_precision, bool enable_timing);

    // Guards coarse graph-wide mutations (e.g., bulk cache reset when force_recache is true).
    std::mutex graph_mutex_;
    bool quiet_ = true;
    // Streaming of compute events to frontends; drained concurrently with computations.
    std::mutex event_mutex_;
    std::vector<ComputeEvent> event_buffer_;
    void push_compute_event(int id, const std::string& name, const std::string& source, double ms);
    // timing_results is updated from multiple threads during compute_parallel().
    std::mutex timing_mutex_;
};

} // namespace ps