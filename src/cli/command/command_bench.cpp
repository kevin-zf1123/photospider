// in: src/cli/command/command_bench.cpp (OVERWRITE)
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "benchmark/benchmark_service.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

// [修改] 辅助函数：将结果保存到文件
void save_benchmark_results(const std::string& output_dir, const std::vector<ps::BenchmarkResult>& results) {
    fs::create_directories(output_dir);
    
    // 1. 保存 Markdown 总结报告
    std::ofstream summary_file(fs::path(output_dir) / "summary.md");
    summary_file << "# Photospider Benchmark Summary\n\n";
    summary_file << "| Benchmark Name | Operation | Dimensions | Threads | Total Time (ms) | Typical Exec Time (ms) | IO Time (ms) |\n";
    summary_file << "|---|---|---|---|---|---|---|\n";
    for (const auto& res : results) {
        summary_file << "| " << res.benchmark_name
                     << " | " << res.op_name
                     << " | " << res.width << "x" << res.height
                     << " | " << res.num_threads
                     << " | " << res.total_duration_ms
                     << " | " << res.typical_execution_time_ms
                     << " | " << res.io_duration_ms
                     << " |\n";
    }
    summary_file.close();
    
    // 2. 保存 CSV 原始数据
    std::ofstream csv_file(fs::path(output_dir) / "raw_data.csv");
    csv_file << "benchmark_name,op_name,width,height,num_threads,total_duration_ms,typical_execution_time_ms,io_duration_ms\n";
    for (const auto& res : results) {
        csv_file << res.benchmark_name << ","
                 << res.op_name << ","
                 << res.width << "," << res.height << ","
                 << res.num_threads << ","
                 << res.total_duration_ms << ","
                 << res.typical_execution_time_ms << ","
                 << res.io_duration_ms << "\n";
    }
    csv_file.close();
}


bool handle_bench(std::istringstream& iss,
                  ps::InteractionService& svc,
                  std::string& /*current_graph*/,
                  bool& /*modified*/,
                  CliConfig& /*config*/) {
    std::string benchmark_dir, output_dir;
    iss >> benchmark_dir >> output_dir;

    if (benchmark_dir.empty() || output_dir.empty()) {
        print_help_bench({});
        return true;
    }

    try {
        ps::BenchmarkService benchmark_svc(svc);
        auto results = benchmark_svc.RunAll(benchmark_dir);
        save_benchmark_results(output_dir, results);
        std::cout << "Benchmark finished. Results saved to '" << output_dir << "'." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed: " << e.what() << std::endl;
    }

    return true;
}

void print_help_bench(const CliConfig& /*config*/) {
    print_help_from_file("help_bench.txt");
}