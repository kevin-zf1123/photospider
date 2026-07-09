#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "benchmark/benchmark_yaml_generator.hpp"
#include "kernel/interaction.hpp"

namespace fs = std::filesystem;

namespace ps {
namespace {

/**
 * @brief Removes transient benchmark artifacts created by legacy runs.
 *
 * @param benchmark_dir Directory that owns benchmark_config.yaml and temporary
 *        benchmark sessions.
 * @throws Nothing directly; filesystem errors are reported to stderr and
 *         cleanup continues for remaining entries.
 * @note Only the reserved `__benchmark_temp` directory and auto-generated
 *       `__generated_*` YAML files are removed. User-authored benchmark YAMLs
 *       are never removed by this helper.
 */
void cleanup_generated_files(const std::string& benchmark_dir) {
  fs::path dir(benchmark_dir);
  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    return;
  }

  fs::path temp_session_dir = dir / "__benchmark_temp";
  if (fs::exists(temp_session_dir)) {
    try {
      fs::remove_all(temp_session_dir);
    } catch (const fs::filesystem_error& e) {
      std::cerr << "Warning: Could not remove temporary session directory: "
                << e.what() << std::endl;
    }
  }

  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (filename.rfind("__generated_", 0) != 0) {
      continue;
    }
    try {
      fs::remove(entry.path());
    } catch (const fs::filesystem_error& e) {
      std::cerr << "Warning: Could not remove generated YAML file: " << e.what()
                << std::endl;
    }
  }
}

/**
 * @brief Parses benchmark_config.yaml for the legacy InteractionService API.
 *
 * @param benchmark_dir Directory containing benchmark_config.yaml.
 * @return Parsed benchmark session configs.
 * @throws std::runtime_error when the config file is missing.
 * @throws YAML::Exception when the config file is malformed.
 * @note Manual YAML paths are resolved against benchmark_dir to match the
 *       Host-backed BenchmarkService path.
 */
std::vector<BenchmarkSessionConfig> load_configs_internal(
    const std::string& benchmark_dir) {
  fs::path config_path = fs::path(benchmark_dir) / "benchmark_config.yaml";
  if (!fs::exists(config_path)) {
    throw std::runtime_error("benchmark_config.yaml not found in: " +
                             benchmark_dir);
  }

  std::vector<BenchmarkSessionConfig> configs;
  YAML::Node root = YAML::LoadFile(config_path.string());
  for (const auto& session_node : root["sessions"]) {
    BenchmarkSessionConfig cfg;
    cfg.name = session_node["name"].as<std::string>();
    cfg.enabled = session_node["enabled"].as<bool>(true);
    cfg.auto_generate = session_node["auto_generate"].as<bool>(true);
    if (cfg.auto_generate) {
      const auto& gen_cfg = session_node["config"];
      cfg.generator_config.input_op_type =
          gen_cfg["input_op_type"].as<std::string>();
      cfg.generator_config.main_op_type =
          gen_cfg["main_op_type"].as<std::string>();
      cfg.generator_config.width = gen_cfg["width"].as<int>();
      cfg.generator_config.height = gen_cfg["height"].as<int>();
      cfg.generator_config.chain_length = gen_cfg["chain_length"].as<int>(1);
      cfg.generator_config.num_outputs = gen_cfg["num_outputs"].as<int>(1);
    } else {
      cfg.yaml_path = (fs::path(benchmark_dir) /
                       session_node["yaml_path"].as<std::string>())
                          .string();
    }
    if (session_node["statistics"] && session_node["statistics"].IsSequence()) {
      cfg.statistics =
          session_node["statistics"].as<std::vector<std::string>>();
    }
    if (session_node["execution"]) {
      cfg.execution.runs = session_node["execution"]["runs"].as<int>(10);
      cfg.execution.threads = session_node["execution"]["threads"].as<int>(0);
      cfg.execution.parallel =
          session_node["execution"]["parallel"].as<bool>(true);
    }
    configs.push_back(cfg);
  }
  return configs;
}

/**
 * @brief Aggregates raw benchmark runs into summary statistics.
 *
 * @param final_result Result object that receives aggregate metrics.
 * @param all_runs Raw run results produced by the legacy compute path.
 * @throws std::bad_alloc when aggregation vectors allocate.
 * @note Typical execution time sums computed events whose op_name matches
 *       final_result.op_name, then removes the lowest and highest 20 percent
 *       when enough samples are present.
 */
void analyze_results(BenchmarkResult& final_result,
                     const std::vector<BenchmarkResult>& all_runs) {
  if (all_runs.empty()) {
    return;
  }

  double total_duration_sum = 0.0;
  for (const auto& run : all_runs) {
    total_duration_sum += run.total_duration_ms;
  }
  final_result.total_duration_ms = total_duration_sum / all_runs.size();

  std::vector<double> execution_times;
  const std::string& target_op_name = final_result.op_name;
  for (const auto& run : all_runs) {
    double total_exec_time_for_target_op = 0.0;
    for (const auto& event : run.events) {
      if (event.op_name == target_op_name && event.source == "computed") {
        total_exec_time_for_target_op += event.execution_duration_ms;
      }
    }
    execution_times.push_back(total_exec_time_for_target_op);
  }

  final_result.exec_times_main_op_ms = execution_times;
  std::sort(execution_times.begin(), execution_times.end());

  const size_t outliers_count = execution_times.size() / 5;
  const size_t start_index = outliers_count;
  const size_t count = execution_times.size() - 2 * outliers_count;
  if (count > 0) {
    const double sum =
        std::accumulate(execution_times.begin() + start_index,
                        execution_times.begin() + start_index + count, 0.0);
    final_result.typical_execution_time_ms = sum / count;
  } else if (!execution_times.empty()) {
    final_result.typical_execution_time_ms = execution_times[0];
  }

  double total_io_sum = 0.0;
  for (const auto& run : all_runs) {
    total_io_sum += run.io_duration_ms;
  }
  final_result.io_duration_ms = total_io_sum / all_runs.size();
  final_result.scheduler_overhead_ms = 0.0;
  final_result.cpu_info = "Unknown CPU";
  final_result.os_info = "Unknown OS";
  final_result.compiler_info = "Unknown Compiler";
}

/**
 * @brief Runs one benchmark config through the legacy InteractionService path.
 *
 * @param svc Interaction facade bound to the caller's Kernel.
 * @param benchmark_dir Directory used for generated YAML and temporary session
 *        storage.
 * @param config Benchmark session config to execute.
 * @param runs Number of repetitions to aggregate.
 * @return Aggregated benchmark result.
 * @throws std::runtime_error when YAML loading, graph loading, or compute
 *         fails.
 * @note This preserves the historical API for non-CLI callers while the CLI
 *       itself now uses Host-backed BenchmarkService.
 */
BenchmarkResult run_benchmark_internal(InteractionService& svc,
                                       const std::string& benchmark_dir,
                                       const BenchmarkSessionConfig& config,
                                       int runs) {
  std::vector<BenchmarkResult> all_runs;
  all_runs.reserve(runs);

  std::string graph_yaml_path;
  if (config.auto_generate) {
    graph_yaml_path =
        (fs::path(benchmark_dir) / ("__generated_" + config.name + ".yaml"))
            .string();
    YAML::Node graph_yaml = YamlGenerator::Generate(config.generator_config);
    std::ofstream fout(graph_yaml_path);
    fout << graph_yaml;
  } else {
    graph_yaml_path = config.yaml_path;
  }

  YAML::Node root = YAML::LoadFile(graph_yaml_path);
  if (!root.IsSequence() || root.size() == 0) {
    throw std::runtime_error(
        "Benchmark YAML is not a valid sequence or is empty.");
  }
  const int target_node_id = root[root.size() - 1]["id"].as<int>();

  for (int i = 0; i < runs; ++i) {
    const std::string session_name = "__benchmark_temp";
    auto loaded_name =
        svc.cmd_load_graph(session_name, benchmark_dir, graph_yaml_path);
    if (!loaded_name) {
      throw std::runtime_error(
          "Failed to load temporary benchmark graph into session root: " +
          benchmark_dir);
    }

    BenchmarkResult single_run_result;
    single_run_result.benchmark_name = config.name;
    single_run_result.num_threads = config.execution.threads > 0
                                        ? config.execution.threads
                                        : std::thread::hardware_concurrency();

    auto start_total = std::chrono::high_resolution_clock::now();

    Kernel::ComputeRequest request;
    request.name = session_name;
    request.node_id = target_node_id;
    request.cache.precision = "int8";
    request.cache.force_recache = true;
    request.cache.disable_disk_cache = true;
    request.cache.nosave = false;
    request.execution.parallel = config.execution.parallel;
    request.execution.quiet = true;
    request.telemetry.enable_timing = true;
    request.telemetry.benchmark_events = &single_run_result.events;
    const bool success = svc.cmd_compute(request);

    auto end_total = std::chrono::high_resolution_clock::now();
    single_run_result.total_duration_ms =
        std::chrono::duration<double, std::milli>(end_total - start_total)
            .count();

    if (auto last_io_time = svc.cmd_get_last_io_time(session_name)) {
      single_run_result.io_duration_ms = *last_io_time;
    }

    if (!success) {
      auto last_err = svc.cmd_last_error(session_name);
      svc.cmd_close_graph(session_name);
      std::string reason =
          last_err ? last_err->message : "Unknown error during compute.";
      throw std::runtime_error("Benchmark run " + std::to_string(i) + " for '" +
                               config.name + "' failed. Reason: " + reason);
    }

    svc.cmd_close_graph(session_name);
    all_runs.push_back(single_run_result);
  }

  BenchmarkResult final_result;
  final_result.benchmark_name = config.name;
  final_result.op_name =
      config.auto_generate ? config.generator_config.main_op_type : "custom";
  final_result.width = config.auto_generate ? config.generator_config.width : 0;
  final_result.height =
      config.auto_generate ? config.generator_config.height : 0;
  final_result.num_threads = config.execution.threads > 0
                                 ? config.execution.threads
                                 : std::thread::hardware_concurrency();

  analyze_results(final_result, all_runs);
  return final_result;
}

}  // namespace

BenchmarkResult InteractionService::cmd_run_benchmark(
    const std::string& benchmark_dir, const BenchmarkSessionConfig& session,
    int runs) {
  return run_benchmark_internal(*this, benchmark_dir, session, runs);
}

std::vector<BenchmarkResult> InteractionService::cmd_run_all_benchmarks(
    const std::string& benchmark_dir) {
  cleanup_generated_files(benchmark_dir);

  auto configs = load_configs_internal(benchmark_dir);
  std::vector<BenchmarkResult> results;
  for (const auto& config : configs) {
    if (!config.enabled) {
      continue;
    }
    try {
      results.push_back(run_benchmark_internal(*this, benchmark_dir, config,
                                               config.execution.runs));
    } catch (const std::exception& e) {
      std::cerr << "Error running benchmark '" << config.name
                << "': " << e.what() << std::endl;
    }
  }
  return results;
}

std::vector<BenchmarkSessionConfig>
InteractionService::cmd_load_benchmark_configs(
    const std::string& benchmark_dir) {
  return load_configs_internal(benchmark_dir);
}

void InteractionService::cmd_cleanup_benchmark_artifacts(
    const std::string& benchmark_dir) {
  cleanup_generated_files(benchmark_dir);
}

}  // namespace ps
