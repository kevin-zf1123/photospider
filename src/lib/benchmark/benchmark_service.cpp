#include "benchmark/benchmark_service.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/benchmark_yaml_generator.hpp"
#include "scheduler/scheduler_worker_budget.hpp"

namespace fs = std::filesystem;

namespace ps {

/**
 * @brief Removes transient benchmark artifacts created by previous runs.
 *
 * @param benchmark_dir Directory that owns benchmark_config.yaml and generated
 *        sessions.
 * @return Nothing.
 * @throws std::bad_alloc if path or diagnostic storage exhausts memory.
 * @throws std::filesystem::filesystem_error when directory inspection fails.
 * @note Only the reserved `__benchmark_temp` session directory and generated
 *       `__generated_*` YAML files are removed. User-authored benchmark inputs
 *       in the same directory are left untouched. Individual remove failures
 *       are reported to stderr and cleanup continues.
 */
static void cleanup_generated_files(const std::string& benchmark_dir) {
  fs::path dir(benchmark_dir);
  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    return;
  }

  // 1. 清理临时 session 目录
  fs::path temp_session_dir = dir / "__benchmark_temp";
  if (fs::exists(temp_session_dir)) {
    try {
      fs::remove_all(temp_session_dir);
    } catch (const fs::filesystem_error& e) {
      std::cerr << "Warning: Could not remove temporary session directory: "
                << e.what() << std::endl;
    }
  }

  // 2. 清理自动生成的 YAML 文件 (以 "__generated_" 开头)
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      std::string filename = entry.path().filename().string();
      if (filename.rfind("__generated_", 0) == 0) {
        try {
          fs::remove(entry.path());
        } catch (const fs::filesystem_error& e) {
          std::cerr << "Warning: Could not remove generated YAML file: "
                    << e.what() << std::endl;
        }
      }
    }
  }
}

/**
 * @brief Builds the canonical operation key used by benchmark events.
 *
 * @param node Node inspection snapshot copied through the Host boundary.
 * @return `type:subtype` when both fields are present, otherwise the best
 *         available non-empty label.
 * @throws std::bad_alloc if string allocation fails.
 * @note This mirrors the backend benchmark event key while avoiding any direct
 *       dependency on backend Node or Kernel types.
 */
static std::string benchmark_op_key(const NodeInspectionView& node) {
  if (!node.type.empty() && !node.subtype.empty()) {
    return node.type + ":" + node.subtype;
  }
  if (!node.type.empty()) {
    return node.type;
  }
  return node.subtype;
}

/**
 * @brief Indexes inspected graph nodes by id for timing-row attribution.
 *
 * @param graph Public graph inspection snapshot from Host.
 * @return Map from node id to canonical benchmark operation key.
 * @throws std::bad_alloc if container/string allocation fails.
 * @note Missing or empty operation labels are intentionally omitted so callers
 *       can fall back to a conservative benchmark label.
 */
static std::map<int, std::string> benchmark_op_names_by_node(
    const GraphInspectionView& graph) {
  std::map<int, std::string> op_names;
  for (const auto& node : graph.nodes) {
    std::string key = benchmark_op_key(node);
    if (!key.empty()) {
      op_names.emplace(node.id.value, std::move(key));
    }
  }
  return op_names;
}

/**
 * @brief Validates and resolves one benchmark scheduler worker request.
 *
 * @param configured_threads Benchmark `execution.threads` value.
 * @return Exact nonzero scheduler worker grant in the range one through eight.
 * @throws std::invalid_argument if configured_threads is negative or greater
 *         than the public scheduler request maximum.
 * @note Zero uses the same bounded hardware-concurrency resolution as product
 *       scheduler construction; positive legal values remain exact.
 */
static unsigned int resolve_benchmark_worker_count(int configured_threads) {
  if (configured_threads < 0) {
    throw std::invalid_argument(
        "benchmark execution.threads must be between zero and eight");
  }
  return resolve_scheduler_worker_count(
      static_cast<unsigned int>(configured_threads),
      std::thread::hardware_concurrency());
}

/**
 * @brief Applies one benchmark worker request to future Host graph defaults.
 *
 * @param host Host whose HP and RT CPU scheduler defaults are updated.
 * @param configured_threads Validated benchmark request; zero remains the
 *        public automatic-selection sentinel.
 * @return Nothing.
 * @throws std::bad_alloc if Host request or status storage exhausts memory.
 * @throws std::runtime_error if Host rejects the scheduler configuration.
 * @note The request is applied before benchmark graph load. Existing graph
 *       runtimes, if any, retain their scheduler configuration.
 */
static void configure_benchmark_scheduler(Host& host, int configured_threads) {
  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = "cpu_work_stealing";
  scheduler_config.rt_type = "cpu_work_stealing";
  scheduler_config.worker_count = static_cast<unsigned int>(configured_threads);
  const VoidResult configured =
      host.configure_scheduler_defaults(scheduler_config);
  if (!configured.status.ok) {
    std::string reason = configured.status.message;
    if (reason.empty()) {
      reason = "Host rejected benchmark scheduler defaults";
    }
    throw std::runtime_error(reason);
  }
}

/**
 * @brief Binds benchmark orchestration to a borrowed Host.
 *
 * @param svc Host used for graph lifecycle, compute, inspection, and telemetry.
 * @throws Nothing directly.
 * @note The Host must outlive this service. No backend object is retained.
 */
BenchmarkService::BenchmarkService(ps::Host& svc) : svc_(svc) {}

/**
 * @brief Runs and aggregates one benchmark configuration through Host.
 *
 * The method generates or loads the graph YAML, creates a temporary Host
 * session for every repetition, computes the target node, copies timing and IO
 * telemetry, closes successful sessions, and aggregates all completed runs.
 *
 * @param benchmark_dir Directory containing benchmark inputs and temporary
 * session storage.
 * @param config Enabled session configuration to execute.
 * @param runs Number of repetitions used by result aggregation.
 * @return Aggregated result for the requested session.
 * @throws std::bad_alloc if Host execution or local result storage exhausts
 * memory.
 * @throws std::invalid_argument when `execution.threads` is outside zero
 * through eight.
 * @throws std::runtime_error when graph input, loading, or compute fails.
 * @throws YAML::Exception when generated or user-provided YAML is malformed.
 * @note The worker request configures future HP and RT CPU schedulers before
 * graph load, and results report the same bounded resolved grant. Temporary
 * sessions are closed after successful runs. Host owns graph state and
 * preserves the documented resource-exhaustion exception boundary.
 */
BenchmarkResult BenchmarkService::Run(const std::string& benchmark_dir,
                                      const BenchmarkSessionConfig& config,
                                      int runs) {
  const unsigned int resolved_workers =
      resolve_benchmark_worker_count(config.execution.threads);
  configure_benchmark_scheduler(svc_, config.execution.threads);

  std::vector<BenchmarkResult> all_runs;
  all_runs.reserve(runs);
  const std::string fallback_op_name =
      config.auto_generate ? config.generator_config.main_op_type : "custom";

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
  int target_node_id = root[root.size() - 1]["id"].as<int>();

  for (int i = 0; i < runs; ++i) {
    const std::string session_name = "__benchmark_temp";

    // 注意：这里 root_dir 我们直接使用 benchmark_dir，以确保 session 文件在
    // benchmark 目录下创建
    auto loaded = svc_.load_graph(
        ps::GraphLoadRequest{ps::GraphSessionId{session_name}, benchmark_dir,
                             graph_yaml_path, "", ""});
    if (!loaded.status.ok) {
      throw std::runtime_error(
          "Failed to load temporary benchmark graph into session root: " +
          benchmark_dir);
    }
    auto graph_view = svc_.inspect_graph(GraphSessionId{session_name});
    const std::map<int, std::string> op_names =
        graph_view.status.ok ? benchmark_op_names_by_node(graph_view.value)
                             : std::map<int, std::string>{};

    BenchmarkResult single_run_result;
    single_run_result.benchmark_name = config.name;
    single_run_result.num_threads = static_cast<int>(resolved_workers);

    auto start_total = std::chrono::high_resolution_clock::now();

    HostComputeRequest request;
    request.session = GraphSessionId{session_name};
    request.node = NodeId{target_node_id};
    request.cache.precision = "int8";
    request.cache.force_recache = true;
    request.cache.disable_disk_cache = true;
    request.cache.nosave = false;
    request.execution.parallel = config.execution.parallel;
    request.execution.quiet = true;
    request.telemetry.enable_timing = true;
    auto compute_status = svc_.compute(request).status;

    auto end_total = std::chrono::high_resolution_clock::now();
    single_run_result.total_duration_ms =
        std::chrono::duration<double, std::milli>(end_total - start_total)
            .count();

    auto timing = svc_.timing(GraphSessionId{session_name});
    if (timing.status.ok) {
      for (const auto& row : timing.value.node_timings) {
        BenchmarkEvent event;
        event.node_id = row.node.value;
        auto op_it = op_names.find(row.node.value);
        event.op_name =
            op_it != op_names.end() ? op_it->second : fallback_op_name;
        event.execution_duration_ms = row.elapsed_ms;
        event.source = row.source;
        single_run_result.events.push_back(std::move(event));
      }
    }
    auto last_io_time = svc_.last_io_time(GraphSessionId{session_name});
    if (last_io_time.status.ok) {
      single_run_result.io_duration_ms = last_io_time.value;
    }
    if (!compute_status.ok) {
      auto last_err = svc_.last_error(GraphSessionId{session_name});
      std::string reason =
          !last_err.message.empty() ? last_err.message : compute_status.message;
      if (reason.empty()) {
        reason = "Unknown error during compute.";
      }
      svc_.close_graph(GraphSessionId{session_name});
      throw std::runtime_error("Benchmark run " + std::to_string(i) + " for '" +
                               config.name + "' failed. Reason: " + reason);
    }

    svc_.close_graph(GraphSessionId{session_name});
    all_runs.push_back(single_run_result);
  }

  BenchmarkResult final_result;
  final_result.benchmark_name = config.name;
  final_result.op_name = fallback_op_name;
  final_result.width = config.auto_generate ? config.generator_config.width : 0;
  final_result.height =
      config.auto_generate ? config.generator_config.height : 0;
  final_result.num_threads = static_cast<int>(resolved_workers);

  analyze_results(final_result, all_runs);

  return final_result;
}

/**
 * @brief 分析多次基准测试运行结果并汇总到 final_result 中
 *
 * 对提供的多次运行结果执行以下统计：
 *   1. 计算所有运行的平均总耗时（total_duration_ms）
 *   2. 提取与 final_result.op_name 匹配的事件执行时间，排序后去掉前后 20%
 * 离群值，计算典型执行时间（typical_execution_time_ms）
 *   3. 计算所有运行的平均 IO 耗时（io_duration_ms）
 *   4. 占位计算调度器开销（scheduler_overhead_ms，目前暂为 0）
 *   5. 填充 CPU、操作系统和编译器信息字段（cpu_info、os_info、compiler_info）
 *
 * @param[out] final_result 用于存放汇总后的基准测试结果
 * @param[in]  all_runs
 * 包含多次运行详细结果的向量，每个元素包含总耗时、事件列表和 IO 耗时等信息
 * @return Nothing.
 * @throws std::bad_alloc if aggregation vector or environment string storage
 * exhausts memory.
 * @note Only events matching final_result.op_name with source `computed`
 * contribute to the typical execution-time distribution.
 */
void BenchmarkService::analyze_results(
    BenchmarkResult& final_result,
    const std::vector<BenchmarkResult>& all_runs) {
  if (all_runs.empty())
    return;

  // 1. 聚合总时间 (平均值)
  double total_duration_sum = 0.0;
  for (const auto& run : all_runs) {
    total_duration_sum += run.total_duration_ms;
  }
  final_result.total_duration_ms = total_duration_sum / all_runs.size();

  // 2. 聚合典型执行时间 (去除离群值)
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

  // 将每次运行的匹配主操作的总执行时间保存到结果，便于导出原始分布
  final_result.exec_times_main_op_ms = execution_times;
  std::sort(execution_times.begin(), execution_times.end());

  size_t outliers_count = execution_times.size() / 5;  // 去掉前后20%
  size_t start_index = outliers_count;
  size_t count = execution_times.size() - 2 * outliers_count;

  if (count > 0) {
    double sum =
        std::accumulate(execution_times.begin() + start_index,
                        execution_times.begin() + start_index + count, 0.0);
    final_result.typical_execution_time_ms = sum / count;
  } else if (!execution_times.empty()) {
    final_result.typical_execution_time_ms =
        execution_times[0];  // 如果数据太少，则取第一个
  }

  // 3. [修改] 聚合IO时间 (平均值)
  double total_io_sum = 0.0;
  for (const auto& run : all_runs) {
    total_io_sum += run.io_duration_ms;
  }
  final_result.io_duration_ms = total_io_sum / all_runs.size();

  // 4. 计算调度器开销 (占位符)
  final_result.scheduler_overhead_ms = 0.0;

  // 5. 填充环境信息 (占位符)
  final_result.cpu_info = "Unknown CPU";
  final_result.os_info = "Unknown OS";
  final_result.compiler_info = "Unknown Compiler";
}

/**
 * @brief Runs every enabled benchmark configuration through the active Host.
 *
 * @param benchmark_dir Directory containing benchmark_config.yaml and graph
 * fixtures.
 * @return Results for enabled sessions that completed successfully.
 * @throws std::bad_alloc if configuration, Host execution, or result storage
 * exhausts memory.
 * @throws std::runtime_error or YAML::Exception when configuration loading
 * fails before per-session execution begins.
 * @throws std::filesystem::filesystem_error when pre-run artifact directory
 * inspection fails.
 * @note Recoverable per-session standard exceptions are logged and skipped so
 * later sessions can run. Resource exhaustion is never reduced to a skipped
 * benchmark because callers must be able to apply process-level recovery.
 */
std::vector<BenchmarkResult> BenchmarkService::RunAll(
    const std::string& benchmark_dir) {
  // [修复] 在所有测试开始前，执行清理操作
  // std::cout << "Cleaning up previous benchmark artifacts in '" <<
  // benchmark_dir << "'..." << std::endl;
  cleanup_generated_files(benchmark_dir);

  auto configs = load_configs_internal(benchmark_dir);
  std::vector<BenchmarkResult> results;
  for (const auto& config : configs) {
    if (config.enabled) {
      // std::cout << "Running benchmark: " << config.name << "..." <<
      // std::endl;
      try {
        // [修复] 将 benchmark_dir 和 runs 次数传递给 Run 函数
        results.push_back(Run(benchmark_dir, config, config.execution.runs));
      } catch (const std::bad_alloc&) {
        throw;
      } catch (const std::exception& e) {
        std::cerr << "Error running benchmark '" << config.name
                  << "': " << e.what() << std::endl;
      }
    }
  }
  return results;
}

/**
 * @brief Parses benchmark_config.yaml into executable session configs.
 *
 * @param benchmark_dir Directory containing benchmark_config.yaml.
 * @return Parsed configurations in file order.
 * @throws std::bad_alloc if YAML, path, string, or vector storage exhausts
 * memory.
 * @throws std::runtime_error when benchmark_config.yaml does not exist.
 * @throws YAML::Exception when configuration values are malformed.
 * @note Relative custom YAML paths are resolved against benchmark_dir.
 */
std::vector<BenchmarkSessionConfig> BenchmarkService::load_configs_internal(
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
      // 对于手动指定的YAML，我们假定它是相对于 benchmark_dir 的路径
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
 * @brief Loads benchmark configurations without executing them.
 *
 * @param benchmark_dir Directory containing benchmark_config.yaml.
 * @return Parsed configurations in file order.
 * @throws std::bad_alloc if parsing storage exhausts memory.
 * @throws std::runtime_error or YAML::Exception when input is unavailable or
 * malformed.
 * @note This is the editor-facing counterpart to RunAll.
 */
std::vector<BenchmarkSessionConfig> BenchmarkService::LoadConfigs(
    const std::string& benchmark_dir) {
  return load_configs_internal(benchmark_dir);
}

/**
 * @brief Removes transient benchmark sessions and generated YAML files.
 *
 * @param benchmark_dir Directory whose reserved benchmark artifacts are
 * removed.
 * @return Nothing.
 * @throws std::bad_alloc if path or diagnostic string construction exhausts
 * memory.
 * @throws std::filesystem::filesystem_error when directory inspection fails.
 * @note Individual file-removal errors are reported to stderr and cleanup
 * continues; user-authored graph inputs are not removed.
 */
void BenchmarkService::CleanupArtifacts(const std::string& benchmark_dir) {
  cleanup_generated_files(benchmark_dir);
}

}  // namespace ps
