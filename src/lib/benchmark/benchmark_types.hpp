#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace ps {

/**
 * @struct GraphGenConfig
 * @brief 配置参数，用于动态生成计算图的YAML定义。
 */
struct GraphGenConfig {
  std::string input_op_type = "image_generator:constant";
  std::string main_op_type = "image_process:gaussian_blur";
  std::string output_op_type = "analyzer:get_dimensions";
  int width = 256;
  int height = 256;
  int chain_length = 1;  // 图中主要操作节点的串联数量
  int num_outputs = 1;   // 从最后一个操作节点引出的输出节点数量 (用于测试扇出)
};

/**
 * @struct BenchmarkEvent
 * @brief 记录单个计算单元（如一个节点的完整计算）的详细性能事件。
 *
 * 这个结构体是数据采集的最小单位，未来在并行调度器中可以用于记录
 * 每一个 TileTask 的执行情况。
 */
struct BenchmarkEvent {
  int node_id;
  std::string op_name;  // e.g., "image_process:gaussian_blur"
  int thread_id = 0;    // 线程ID，在当前顺序实现中默认为0

  // 时间点记录
  std::chrono::high_resolution_clock::time_point
      dependency_start_time;  // 开始解析依赖的时间
  std::chrono::high_resolution_clock::time_point
      execution_start_time;  // 核心计算逻辑开始的时间
  std::chrono::high_resolution_clock::time_point
      execution_end_time;  // 核心计算逻辑结束的时间

  // 计算出的耗时 (单位：毫秒)
  double dependency_duration_ms = 0.0;  // 依赖解析和上游节点计算的耗时
  double execution_duration_ms = 0.0;   // 节点自身计算的耗时

  std::string source;  // 结果来源: "memory_cache", "disk_cache", "computed"
};

/**
 * @struct BenchmarkResult
 * @brief 存储一次完整基准测试运行的所有数据和聚合统计。
 *
 * 这是 `bench` 命令最终生成的核心产物。
 */
struct BenchmarkResult {
  // --- 配置信息 ---
  std::string benchmark_name;  // 测试会话名称, e.g., "small_square_blur"
  std::string op_name;  // 核心测试的操作, e.g., "image_process:gaussian_blur"
  int width = 0, height = 0;
  /**
   * @brief Resolved Run maximum-parallelism cap for this benchmark.
   * @note The fixed process worker ceiling may be lower; this field reports the
   *       benchmark control applied to each Run, not a resized pool.
   */
  int num_threads = 1;

  // --- 原始数据 ---
  std::vector<BenchmarkEvent> events;  // 所有节点的性能事件记录

  // --- 聚合统计 (由 BenchmarkService 后续填充) ---
  double total_duration_ms = 0.0;          // 从调用 compute 到返回的总时间
  double typical_execution_time_ms = 0.0;  // 典型计算时间 (去除异常值后)
  double io_duration_ms = 0.0;             // (未来填充) 磁盘IO总耗时
  /** @brief Reserved process execution-routing overhead metric in milliseconds.
   */
  double execution_overhead_ms = 0.0;

  // 用于导出：每次运行匹配主操作(op_name)的总执行时间列表（毫秒）。
  // 这是一维分布，供分析 typical_execution_time_ms 时参考。
  std::vector<double> exec_times_main_op_ms;

  // --- 环境快照 ---
  std::string cpu_info;
  std::string os_info;
  std::string compiler_info;
};

}  // namespace ps

namespace ps {

/**
 * @brief 基准测试会话配置结构体
 * @details
 * 用于描述一次基准测试的整体参数，包括名称、启用状态、自动生成或加载现有配置、
 * 执行策略以及需要收集的统计指标等。
 */
struct BenchmarkSessionConfig {
  /**
   * @brief 会话名称
   * @default "small_square_blur"
   */
  std::string name;  // "small_square_blur"
  /**
   * @brief 是否启用该基准测试
   * @default true
   */
  bool enabled = true;
  /**
   * @brief 是否自动生成图结构配置
   * @details
   * 如果为 true，则使用 generator_config 所指定的参数动态生成图；
   * 否则使用 yaml_path 指定的现有 YAML 文件。
   * @default true
   */
  bool auto_generate = true;

  /**
   * @brief 自动生成模式下的图生成配置
   */

  GraphGenConfig generator_config;

  /**
   * @brief 非自动生成模式下的 YAML 文件路径
   */
  std::string yaml_path;

  /**
   * @brief Product execution and repetition controls for this session.
   * @throws Nothing for value construction; string-free fields do not
   *         allocate.
   * @note Thread selection becomes Run QoS. Process execution infrastructure
   *       remains Host-owned and fixed independently of individual sessions.
   */
  struct ExecutionConfig {
    /**
     * @brief 执行次数
     * @default 10
     */
    int runs = 10;
    /**
     * @brief Run maximum-parallelism request from zero through eight.
     * @details Zero resolves to bounded hardware concurrency in `[1,8]`;
     * positive values remain exact. The resolved value caps callbacks for this
     * benchmark Run without resizing the process worker pool.
     * @default 0
     */
    int threads = 0;

    /**
     * @brief Whether compute dispatch uses the parallel execution service.
     * @default true
     */
    bool parallel = true;
  } execution;
  /**
   * @brief 要收集和报告的统计指标列表
   */

  std::vector<std::string> statistics;
};

}  // namespace ps
