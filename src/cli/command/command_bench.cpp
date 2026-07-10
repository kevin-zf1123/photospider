// in: src/cli/command/command_bench.cpp (OVERWRITE)
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "benchmark/benchmark_service.hpp"
#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"

namespace fs = std::filesystem;
namespace {

/**
 * @brief Serializes benchmark duration samples into one CSV-safe field.
 *
 * @param values Duration samples to serialize in stable input order.
 * @return Bracketed semicolon-separated values at three-decimal precision.
 * @throws std::bad_alloc if stream or result string storage exhausts memory.
 * @note The returned string contains no comma, so the caller may quote it as a
 * single CSV cell without changing the benchmark data model.
 */
std::string serialize_benchmark_samples(const std::vector<double>& values) {
  std::ostringstream output;
  output.setf(std::ios::fixed);
  output.precision(3);
  output << "[";
  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ";";
    }
    output << values[index];
  }
  output << "]";
  return output.str();
}

/**
 * @brief Writes benchmark summary and raw CSV result files.
 *
 * @param output_dir Directory created for benchmark result artifacts.
 * @param results Completed benchmark results to serialize.
 * @return Nothing.
 * @throws std::bad_alloc if path, stream, or serialization storage exhausts
 * memory.
 * @throws std::filesystem::filesystem_error when output directory creation
 * fails.
 * @note The local std::ofstream objects do not enable exceptions and their
 * states are not returned, so open/write failures are not observable by the
 * caller. This helper does not reinterpret Host-side compute failures.
 */
void save_benchmark_results(const std::string& output_dir,
                            const std::vector<ps::BenchmarkResult>& results) {
  fs::create_directories(output_dir);

  // 1. 保存 Markdown 总结报告
  std::ofstream summary_file(fs::path(output_dir) / "summary.md");
  summary_file << "# Photospider Benchmark Summary\n\n";
  summary_file << "| Benchmark Name | Operation | Dimensions | Threads | Total "
                  "Time (ms) | Typical Exec Time (ms) | IO Time (ms) |\n";
  summary_file << "|---|---|---|---|---|---|---|\n";
  for (const auto& res : results) {
    summary_file << "| " << res.benchmark_name << " | " << res.op_name << " | "
                 << res.width << "x" << res.height << " | " << res.num_threads
                 << " | " << res.total_duration_ms << " | "
                 << res.typical_execution_time_ms << " | " << res.io_duration_ms
                 << " |\n";
  }
  summary_file.close();

  // 2. 保存 CSV 原始数据
  std::ofstream csv_file(fs::path(output_dir) / "raw_data.csv");
  csv_file
      << "benchmark_name,op_name,width,height,num_threads,total_duration_ms,"
         "typical_execution_time_ms,io_duration_ms,exec_times_main_op_ms\n";
  for (const auto& res : results) {
    csv_file << res.benchmark_name << "," << res.op_name << "," << res.width
             << "," << res.height << "," << res.num_threads << ","
             << res.total_duration_ms << "," << res.typical_execution_time_ms
             << "," << res.io_duration_ms << "," << '"'
             << serialize_benchmark_samples(res.exec_times_main_op_ms) << '"'
             << "\n";
  }
  csv_file.close();
}

}  // namespace

/**
 * @brief Executes the CLI `bench` command through BenchmarkService and Host.
 *
 * @param iss Command arguments containing benchmark and output directories.
 * @param svc Product Host boundary used by BenchmarkService.
 * @param current_graph Unused current-session compatibility argument.
 * @param modified Unused graph-modification compatibility argument.
 * @param config Unused CLI configuration compatibility argument.
 * @return True after command handling, including recoverable failures.
 * @throws std::bad_alloc if argument, benchmark, Host, or result storage
 * exhausts memory.
 * @note Recoverable standard exceptions are printed as CLI failures. Resource
 * exhaustion remains exceptional across the Host-facing CLI call chain.
 */
bool handle_bench(std::istringstream& iss, ps::Host& svc,
                  std::string& current_graph, bool& modified,
                  CliConfig& config) {
  (void)current_graph;
  (void)modified;
  (void)config;
  std::string benchmark_dir, output_dir;
  iss >> benchmark_dir >> output_dir;

  if (benchmark_dir.empty() || output_dir.empty()) {
    print_help_bench({});
    return true;
  }

  try {
    // 在这里添加UI反馈
    std::cout << "Cleaning up previous benchmark artifacts in '"
              << benchmark_dir << "'..." << std::endl;
    ps::BenchmarkService benchmark_service(svc);
    benchmark_service.CleanupArtifacts(benchmark_dir);

    std::cout << "Running all enabled benchmarks in '" << benchmark_dir
              << "'..." << std::endl;

    auto results = benchmark_service.RunAll(benchmark_dir);
    save_benchmark_results(output_dir, results);
    std::cout << "Benchmark finished. Results saved to '" << output_dir << "'."
              << std::endl;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    std::cerr << "Benchmark failed: " << e.what() << std::endl;
  }

  return true;
}

/**
 * @brief Prints the maintained help text for the `bench` command.
 *
 * @param config Unused CLI configuration compatibility argument.
 * @return Nothing.
 * @throws std::bad_alloc if help path or output storage exhausts memory.
 * @note Help content is loaded by the shared CLI help utility.
 */
void print_help_bench(const CliConfig& config) {
  (void)config;
  print_help_from_file("help_bench.txt");
}
