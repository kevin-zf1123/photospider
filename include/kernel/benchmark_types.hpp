#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "cli/benchmark_yaml_generator.hpp"
namespace ps {

/**
 * @struct BenchmarkEvent
 * @brief 记录单个计算单元（如一个节点的完整计算）的详细性能事件。
 *
 * 这个结构体是数据采集的最小单位，未来在并行调度器中可以用于记录
 * 每一个 TileTask 的执行情况。
 */
struct BenchmarkEvent {
    int node_id;
    int thread_id = 0; // 线程ID，在当前顺序实现中默认为0

    // 时间点记录
    std::chrono::high_resolution_clock::time_point dependency_start_time; // 开始解析依赖的时间
    std::chrono::high_resolution_clock::time_point execution_start_time;  // 核心计算逻辑开始的时间
    std::chrono::high_resolution_clock::time_point execution_end_time;    // 核心计算逻辑结束的时间

    // 计算出的耗时 (单位：毫秒)
    double dependency_duration_ms = 0.0; // 依赖解析和上游节点计算的耗时
    double execution_duration_ms = 0.0;  // 节点自身计算的耗时
    
    std::string source; // 结果来源: "memory_cache", "disk_cache", "computed"
};

/**
 * @struct BenchmarkResult
 * @brief 存储一次完整基准测试运行的所有数据和聚合统计。
 *
 * 这是 `bench` 命令最终生成的核心产物。
 */
struct BenchmarkResult {
    // --- 配置信息 ---
    std::string benchmark_name; // 测试会话名称, e.g., "small_square_blur"
    std::string op_name;        // 核心测试的操作, e.g., "image_process:gaussian_blur"
    int width = 0, height = 0;
    int num_threads = 1;        // 本次运行的线程数

    // --- 原始数据 ---
    std::vector<BenchmarkEvent> events; // 所有节点的性能事件记录

    // --- 聚合统计 (由 BenchmarkService 后续填充) ---
    double total_duration_ms = 0.0;     // 从调用 compute 到返回的总时间
    double typical_execution_time_ms = 0.0; // 典型计算时间 (去除异常值后)
    double io_duration_ms = 0.0;        // (未来填充) 磁盘IO总耗时
    double scheduler_overhead_ms = 0.0; // (未来填充) 调度器开销

    // --- 环境快照 ---
    std::string cpu_info;
    std::string os_info;
    std::string compiler_info;
};

} // namespace ps

namespace ps {

/**
 * @struct BenchmarkSessionConfig
 * @brief 描述一个独立的基准测试会话。
 *
 * 这个结构体对应于未来 benchmark_config.yaml 中的一个 session 条目。
 */
struct BenchmarkSessionConfig {
    std::string name;       // "small_square_blur"
    bool enabled = true;
    bool auto_generate = true;
    
    // 如果 auto_generate 为 true, 使用此配置
    GraphGenConfig generator_config; 

    // 如果 auto_generate 为 false, 使用此现有 yaml 文件
    std::string yaml_path; 

    // 需要统计和报告的指标
    std::vector<std::string> statistics;
};

} // namespace ps