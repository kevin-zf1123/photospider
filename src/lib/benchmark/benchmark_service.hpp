#pragma once

#include <mutex>
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
 * BenchmarkResult。它不持有 backend compute、graph model 或 execution
 * 对象，调用者负责保证传入 Host 在 BenchmarkService 生命周期内有效。
 *
 * @throws std::bad_alloc from non-destructor methods when configuration,
 * telemetry, result, or Host-side compute storage exhausts memory.
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
   * @throws std::bad_alloc if Host execution or result aggregation exhausts
   *         memory.
   * @throws std::invalid_argument when `config.execution.threads` is negative
   *         or greater than eight.
   * @throws std::runtime_error when YAML loading, graph loading, or compute
   *         fails.
   * @throws YAML::Exception when generated or user-provided graph YAML is
   *         malformed.
   * @note Before graph load, `execution.threads` is resolved once to a
   *       positive Run `maximum_parallelism` cap and reported by the result.
   *       The service prepares future HP/RT CPU routes at most once with an
   *       automatic/idempotent process worker request; the session cap never
   *       resizes the fixed pool. Temporary graph sessions are named
   *       `__benchmark_temp` and closed after each run.
   */
  BenchmarkResult Run(const std::string& benchmark_dir,
                      const BenchmarkSessionConfig& config, int runs = 10);

  /**
   * @brief 运行一个目录下的所有已启用的基准测试。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
   * @return 每个已启用会话的聚合结果。
   * @throws std::bad_alloc if configuration, Host execution, or result storage
   *         exhausts memory.
   * @throws std::runtime_error when configuration loading fails.
   * @throws YAML::Exception when benchmark configuration values are malformed.
   * @throws std::filesystem::filesystem_error when pre-run artifact directory
   *         inspection fails.
   * @note Disabled sessions are not thread-range preflighted or executed;
   *       configuration loading still validates their YAML structure and
   *       field types. Enabled thread requests are preflighted before one
   *       process-execution preparation; invalid sessions and later
   *       per-session failures are reported to stderr and skipped. A process
   *       preparation failure remains exceptional because no session can run
   *       safely.
   */
  std::vector<BenchmarkResult> RunAll(const std::string& benchmark_dir);

  /**
   * @brief 加载指定目录下的所有基准测试配置。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
   * @return 解析后的基准测试配置列表。
   * @throws std::bad_alloc if parsing storage exhausts memory.
   * @throws std::runtime_error or YAML::Exception when the config file is
   *         missing or malformed.
   */
  std::vector<BenchmarkSessionConfig> LoadConfigs(
      const std::string& benchmark_dir);

  /**
   * @brief 清理基准目录中的临时会话与自动生成的文件。
   * @param benchmark_dir 目标基准目录。
   * @return Nothing.
   * @throws std::bad_alloc if path or diagnostic allocation fails.
   * @throws std::filesystem::filesystem_error when directory inspection fails.
   * @note Individual file-removal errors are reported to stderr and skipped.
   */
  void CleanupArtifacts(const std::string& benchmark_dir);

 private:
  /** @brief Borrowed Host used for benchmark graph operations. */
  ps::Host& svc_;

  /**
   * @brief Serializes the one successful Host execution-default preparation.
   * @note `std::call_once` retries after an exception. The flag owns no Host
   *       state and is destroyed only after all service calls have joined.
   */
  std::once_flag execution_preparation_once_;

  /**
   * @brief Prepares CPU execution defaults once for this service lifetime.
   * @return Nothing.
   * @throws std::bad_alloc if Host request or diagnostic storage allocation
   *         fails.
   * @throws std::runtime_error if Host rejects CPU route preparation.
   * @note The Host receives `worker_count=0`, which initializes an unfixed
   *       pool automatically or preserves an already fixed pool. No benchmark
   *       session worker cap is forwarded through this process setting.
   */
  void prepare_execution();

  /**
   * @brief Executes one prevalidated benchmark with a Run QoS cap.
   * @param benchmark_dir Directory containing benchmark inputs and temporary
   *        session storage.
   * @param config Enabled session configuration.
   * @param runs Repetition count used for aggregation.
   * @param maximum_parallelism Positive resolved Run callback cap.
   * @return Aggregated benchmark result reporting `maximum_parallelism`.
   * @throws std::bad_alloc if Host or local result storage exhausts memory.
   * @throws std::runtime_error when graph input, loading, or compute fails.
   * @throws YAML::Exception when graph YAML is malformed.
   * @note The caller validates the cap and completes process preparation
   *       first. Every repetition receives the same immutable Run cap.
   */
  BenchmarkResult run_with_parallelism(const std::string& benchmark_dir,
                                       const BenchmarkSessionConfig& config,
                                       int runs,
                                       unsigned int maximum_parallelism);

  /**
   * @brief 分析多次运行的原始数据，计算统计指标。
   * @param final_result 用于填充聚合统计数据。
   * @param all_runs 所有单次运行的原始结果。
   * @return Nothing.
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
   * @throws std::bad_alloc if parsing storage exhausts memory.
   * @throws std::runtime_error or YAML::Exception on missing or invalid input.
   * @note Relative YAML paths are resolved against benchmark_dir.
   */
  std::vector<BenchmarkSessionConfig> load_configs_internal(
      const std::string& benchmark_dir);
};

}  // namespace ps
