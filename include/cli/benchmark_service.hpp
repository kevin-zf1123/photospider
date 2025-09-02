#pragma once

#include "kernel/interaction.hpp"
#include "kernel/benchmark_types.hpp"

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
     * @param config 测试会话的配置。
     * @param runs 重复运行的次数，用于统计分析。
     * @return 聚合和分析后的 BenchmarkResult。
     */
    BenchmarkResult Run(const BenchmarkSessionConfig& config, int runs = 10);

    /**
     * @brief 运行一个目录下的所有已启用的基准测试。
     * @param benchmark_dir 包含 benchmark_config.yaml 的目录。
     * @return 每个已启用会话的聚合结果。
     */
    std::vector<BenchmarkResult> RunAll(const std::string& benchmark_dir);

private:
    ps::InteractionService& svc_;

    /**
     * @brief 分析多次运行的原始数据，计算统计指标。
     * @param final_result 用于填充聚合统计数据。
     * @param all_runs 所有单次运行的原始结果。
     */
    void analyze_results(BenchmarkResult& final_result, const std::vector<BenchmarkResult>& all_runs);

    // [新增] 辅助函数，用于加载配置文件
    std::vector<BenchmarkSessionConfig> load_configs(const std::string& benchmark_dir);
};

} // namespace ps