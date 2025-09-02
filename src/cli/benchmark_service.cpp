// FILE: src/cli/benchmark_service.cpp (新建文件)
#include "cli/benchmark_service.hpp"
#include "cli/benchmark_yaml_generator.hpp"
#include <numeric>   // for std::accumulate
#include <algorithm> // for std::sort
#include <fstream>
#include <filesystem>
#include "kernel/kernel.hpp" // For GraphError

namespace ps {

BenchmarkService::BenchmarkService(ps::InteractionService& svc) : svc_(svc) {}

BenchmarkResult BenchmarkService::Run(const BenchmarkSessionConfig& config, int runs) {
    std::vector<BenchmarkResult> all_runs;
    all_runs.reserve(runs);

    // 1. 生成或定位 YAML
    std::string temp_yaml_path = (fs::temp_directory_path() / (config.name + ".yaml")).string();
    if (config.auto_generate) {
        YAML::Node graph_yaml = YamlGenerator::Generate(config.generator_config);
        std::ofstream fout(temp_yaml_path);
        fout << graph_yaml;
    } else {
        // 如果是手动指定的yaml，我们直接使用它
        temp_yaml_path = config.yaml_path;
    }
    
    // 寻找图中的最后一个节点作为计算目标
    YAML::Node root = YAML::LoadFile(temp_yaml_path);
    if (!root.IsSequence() || root.size() == 0) {
        throw std::runtime_error("Benchmark YAML is not a valid sequence or is empty.");
    }
    int target_node_id = root[root.size()-1]["id"].as<int>();


    for (int i = 0; i < runs; ++i) {
        const std::string session_name = "__benchmark_temp";
        
        // 2. 加载图会话
        auto loaded_name = svc_.cmd_load_graph(session_name, "sessions", temp_yaml_path);
        if (!loaded_name) {
            throw std::runtime_error("Failed to load temporary benchmark graph.");
        }

        BenchmarkResult single_run_result;
        single_run_result.benchmark_name = config.name;
        
        auto start_total = std::chrono::high_resolution_clock::now();
        
        // 3. 运行计算并收集事件
        // 注意：这里的 compute_parallel 最终会调用到我们修改过的 compute_internal
        bool success = svc_.cmd_compute(session_name, target_node_id, "int8", true, true, true, true, false, &single_run_result.events);

        auto end_total = std::chrono::high_resolution_clock::now();
        single_run_result.total_duration_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

        if (!success) {
            std::cerr << "Warning: Benchmark run " << i << " for '" << config.name << "' failed." << std::endl;
        }

        // 4. 清理
        svc_.cmd_close_graph(session_name);

        all_runs.push_back(single_run_result);
    }
    
    // 5. 分析并返回聚合结果
    BenchmarkResult final_result;
    final_result.benchmark_name = config.name;
    final_result.op_name = config.auto_generate ? config.generator_config.main_op_type : "custom";
    final_result.width = config.auto_generate ? config.generator_config.width : 0;
    final_result.height = config.auto_generate ? config.generator_config.height : 0;
    
    analyze_results(final_result, all_runs);

    if (!config.auto_generate) {
        // 不需要保留临时文件
         fs::remove(temp_yaml_path);
    }

    return final_result;
}

void BenchmarkService::analyze_results(BenchmarkResult& final_result, const std::vector<BenchmarkResult>& all_runs) {
    if (all_runs.empty()) return;

    // 1. 计算总时间 (取第一次运行的总时间)
    final_result.total_duration_ms = all_runs[0].total_duration_ms;

    // 2. 计算典型计算时间 (去除最高和最低的20%)
    std::vector<double> execution_times;
    for (const auto& run : all_runs) {
        double total_exec_time = 0.0;
        for (const auto& event : run.events) {
            total_exec_time += event.execution_duration_ms;
        }
        execution_times.push_back(total_exec_time);
    }

    std::sort(execution_times.begin(), execution_times.end());
    
    size_t outliers_count = execution_times.size() / 5;
    size_t start_index = outliers_count;
    size_t count = execution_times.size() - 2 * outliers_count;

    if (count > 0) {
        double sum = std::accumulate(execution_times.begin() + start_index, execution_times.begin() + start_index + count, 0.0);
        final_result.typical_execution_time_ms = sum / count;
    } else if (!execution_times.empty()) {
        final_result.typical_execution_time_ms = execution_times[0]; // 如果数据太少，就取第一个
    }
    
    // 填充环境信息 (占位)
    final_result.cpu_info = "Unknown CPU";
    final_result.os_info = "Unknown OS";
    final_result.compiler_info = "Unknown Compiler";
}


// [新增] 实现 RunAll 和 load_configs
std::vector<BenchmarkResult> BenchmarkService::RunAll(const std::string& benchmark_dir) {
    auto configs = load_configs(benchmark_dir);
    std::vector<BenchmarkResult> results;
    for (const auto& config : configs) {
        if (config.enabled) {
            std::cout << "Running benchmark: " << config.name << "..." << std::endl;
            try {
                results.push_back(Run(config));
            } catch (const std::exception& e) {
                std::cerr << "Error running benchmark '" << config.name << "': " << e.what() << std::endl;
            }
        }
    }
    return results;
}

std::vector<BenchmarkSessionConfig> BenchmarkService::load_configs(const std::string& benchmark_dir) {
    // 这个函数将在里程碑3中被TUI使用，现在我们先为`RunAll`提供一个基础实现。
    // 它会读取 `benchmark_config.yaml`。
    fs::path config_path = fs::path(benchmark_dir) / "benchmark_config.yaml";
    if (!fs::exists(config_path)) {
        throw std::runtime_error("benchmark_config.yaml not found in: " + benchmark_dir);
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
            cfg.generator_config.input_op_type = gen_cfg["input_op_type"].as<std::string>();
            cfg.generator_config.main_op_type = gen_cfg["main_op_type"].as<std::string>();
            cfg.generator_config.width = gen_cfg["width"].as<int>();
            cfg.generator_config.height = gen_cfg["height"].as<int>();
            cfg.generator_config.chain_length = gen_cfg["chain_length"].as<int>(1);
            cfg.generator_config.num_outputs = gen_cfg["num_outputs"].as<int>(1);
        } else {
            cfg.yaml_path = (fs::path(benchmark_dir) / session_node["yaml_path"].as<std::string>()).string();
        }
        configs.push_back(cfg);
    }
    return configs;
}


} // namespace ps