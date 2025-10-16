#pragma once

#include "benchmark/benchmark_types.hpp"
#include "kernel/interaction.hpp"

namespace ps {

/**
 * @class BenchmarkService
 * @brief 封装了运行和分析基准测试的逻辑。
 */
class BenchmarkService {
 public:
  explicit BenchmarkService(ps::InteractionService& svc);

  /**
   * @brief 运行单个基准测试配置。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录路径。
   * @param config 测试会话的配置。
   * @param runs 重复运行的次数，用于统计分析。
   * @return 聚合和分析后的 BenchmarkResult。
   */
  // 修复：添加 benchmark_dir 参数
  BenchmarkResult Run(const std::string& benchmark_dir,
                      const BenchmarkSessionConfig& config, int runs = 10);

  /**
   * @brief 运行一个目录下的所有已启用的基准测试。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
   * @return 每个已启用会话的聚合结果。
   */
  std::vector<BenchmarkResult> RunAll(const std::string& benchmark_dir);

  /**
   * @brief 加载指定目录下的所有基准测试配置。
   * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
   * @return 解析后的基准测试配置列表。
   */
  std::vector<BenchmarkSessionConfig> LoadConfigs(
      const std::string& benchmark_dir);

  /**
   * @brief 清理基准目录中的临时会话与自动生成的文件。
   * @param benchmark_dir 目标基准目录。
   */
  void CleanupArtifacts(const std::string& benchmark_dir);

 private:
  ps::InteractionService& svc_;

  /**
   * @brief 分析多次运行的原始数据，计算统计指标。
   * @param final_result 用于填充聚合统计数据。
   * @param all_runs 所有单次运行的原始结果。
   */
  void analyze_results(BenchmarkResult& final_result,
                       const std::vector<BenchmarkResult>& all_runs);

  std::vector<BenchmarkSessionConfig> load_configs_internal(
      const std::string& benchmark_dir);
};

}  // namespace ps
