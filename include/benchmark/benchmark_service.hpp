#pragma once

#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "photospider/host/host.hpp"

namespace ps {

/**
 * @class BenchmarkService
 * @brief 封装了运行和分析基准测试的逻辑。
 *
 * BenchmarkService 通过 Host frontend seam 加载临时 graph、执行 compute、
 * 读取 timing/inspection/IO telemetry，并把多次运行聚合为
 * BenchmarkResult。它不持有 backend compute、graph model 或 scheduler
 * 对象，调用者负责保证传入 Host 在 BenchmarkService 生命周期内有效。
 */
class BenchmarkService {
 public:
  /**
   * @brief Binds benchmark orchestration to a Host instance.
   *
   * @param svc Host implementation used for graph lifecycle, compute, and
   *        telemetry reads.
   * @throws Nothing directly.
   * @note The Host reference is borrowed; BenchmarkService must not outlive it.
   */
  explicit BenchmarkService(ps::Host& svc);

  /**
   * @brief 运行单个基准测试配置。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录路径。
   * @param config 测试会话的配置。
   * @param runs 重复运行的次数，用于统计分析。
   * @return 聚合和分析后的 BenchmarkResult。
   * @throws std::runtime_error when YAML loading, graph loading, or compute
   *         fails.
   * @note Temporary graph sessions are named `__benchmark_temp` and closed
   *       after each run.
   */
  BenchmarkResult Run(const std::string& benchmark_dir,
                      const BenchmarkSessionConfig& config, int runs = 10);

  /**
   * @brief 运行一个目录下的所有已启用的基准测试。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
   * @return 每个已启用会话的聚合结果。
   * @throws std::runtime_error when configuration loading fails.
   * @note Individual session failures are reported to stderr and skipped so
   *       later enabled sessions can still run.
   */
  std::vector<BenchmarkResult> RunAll(const std::string& benchmark_dir);

  /**
   * @brief 加载指定目录下的所有基准测试配置。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
   * @return 解析后的基准测试配置列表。
   * @throws std::runtime_error or YAML::Exception when the config file is
   *         missing or malformed.
   */
  std::vector<BenchmarkSessionConfig> LoadConfigs(
      const std::string& benchmark_dir);

  /**
   * @brief 清理基准目录中的临时会话与自动生成的文件。
   * @param benchmark_dir 目标基准目录。
   * @throws Nothing directly; cleanup errors are reported to stderr.
   */
  void CleanupArtifacts(const std::string& benchmark_dir);

 private:
  /** @brief Borrowed Host used for benchmark graph operations. */
  ps::Host& svc_;

  /**
   * @brief 分析多次运行的原始数据，计算统计指标。
   * @param final_result 用于填充聚合统计数据。
   * @param all_runs 所有单次运行的原始结果。
   * @throws std::bad_alloc if aggregation vectors allocate.
   * @note Only events with op_name matching final_result.op_name and source
   *       `"computed"` contribute to typical execution time.
   */
  void analyze_results(BenchmarkResult& final_result,
                       const std::vector<BenchmarkResult>& all_runs);

  /**
   * @brief Parses benchmark_config.yaml from a benchmark directory.
   *
   * @param benchmark_dir Directory containing benchmark_config.yaml.
   * @return Parsed session configs.
   * @throws std::runtime_error or YAML::Exception on missing or invalid input.
   * @note Relative YAML paths are resolved against benchmark_dir.
   */
  std::vector<BenchmarkSessionConfig> load_configs_internal(
      const std::string& benchmark_dir);
};

}  // namespace ps
