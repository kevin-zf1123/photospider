#include "kernel/interaction.hpp"
#include "benchmark/benchmark_service.hpp"

namespace ps {

BenchmarkResult InteractionService::cmd_run_benchmark(const std::string& benchmark_dir,
                                                      const BenchmarkSessionConfig& session,
                                                      int runs) {
    BenchmarkService svc(*this);
    return svc.Run(benchmark_dir, session, runs);
}

std::vector<BenchmarkResult> InteractionService::cmd_run_all_benchmarks(const std::string& benchmark_dir) {
    BenchmarkService svc(*this);
    return svc.RunAll(benchmark_dir);
}

std::vector<BenchmarkSessionConfig> InteractionService::cmd_load_benchmark_configs(const std::string& benchmark_dir) {
    BenchmarkService svc(*this);
    return svc.LoadConfigs(benchmark_dir);
}

void InteractionService::cmd_cleanup_benchmark_artifacts(const std::string& benchmark_dir) {
    BenchmarkService svc(*this);
    svc.CleanupArtifacts(benchmark_dir);
}

} // namespace ps

