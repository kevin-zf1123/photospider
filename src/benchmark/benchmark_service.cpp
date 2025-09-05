#include "benchmark/benchmark_service.hpp"
#include "benchmark/benchmark_yaml_generator.hpp"
#include <numeric>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include "kernel/kernel.hpp" // 包含 Kernel 头文件以访问 InteractionService

namespace fs = std::filesystem;

namespace ps {

// [新增] 清理函数，用于删除上次运行生成的临时文件
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
            std::cerr << "Warning: Could not remove temporary session directory: " << e.what() << std::endl;
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
                    std::cerr << "Warning: Could not remove generated YAML file: " << e.what() << std::endl;
                }
            }
        }
    }
}


BenchmarkService::BenchmarkService(ps::InteractionService& svc) : svc_(svc) {}

// [修复] 更新函数签名以接收 benchmark_dir
BenchmarkResult BenchmarkService::Run(const std::string& benchmark_dir, const BenchmarkSessionConfig& config, int runs) {
    std::vector<BenchmarkResult> all_runs;
    all_runs.reserve(runs);

    std::string graph_yaml_path;
    if (config.auto_generate) {
        graph_yaml_path = (fs::path(benchmark_dir) / ("__generated_" + config.name + ".yaml")).string();
        YAML::Node graph_yaml = YamlGenerator::Generate(config.generator_config);
        std::ofstream fout(graph_yaml_path);
        fout << graph_yaml;
    } else {
        graph_yaml_path = config.yaml_path;
    }
    
    YAML::Node root = YAML::LoadFile(graph_yaml_path);
    if (!root.IsSequence() || root.size() == 0) {
        throw std::runtime_error("Benchmark YAML is not a valid sequence or is empty.");
    }
    int target_node_id = root[root.size()-1]["id"].as<int>();

    for (int i = 0; i < runs; ++i) {
        const std::string session_name = "__benchmark_temp";
        
        // 注意：这里 root_dir 我们直接使用 benchmark_dir，以确保 session 文件在 benchmark 目录下创建
        auto loaded_name = svc_.cmd_load_graph(session_name, benchmark_dir, graph_yaml_path);
        if (!loaded_name) {
            throw std::runtime_error("Failed to load temporary benchmark graph into session root: " + benchmark_dir);
        }

        BenchmarkResult single_run_result;
        single_run_result.benchmark_name = config.name;
        // [修改] 设置线程数
        single_run_result.num_threads = config.execution.threads > 0 ? config.execution.threads : std::thread::hardware_concurrency();
        
        auto start_total = std::chrono::high_resolution_clock::now();
        
        bool success = svc_.cmd_compute(session_name, target_node_id, "int8", 
                                        true,  // force (clears memory cache)
                                        true,  // timing
                                        config.execution.parallel,  // parallel
                                        true,  // quiet
                                        true,  // [修复] disable_disk_cache = true, 确保测量纯计算性能
                                        &single_run_result.events);

        auto end_total = std::chrono::high_resolution_clock::now();
        single_run_result.total_duration_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();
        
        auto last_io_time = svc_.cmd_get_last_io_time(session_name);
        if (last_io_time) {
            single_run_result.io_duration_ms = *last_io_time;
        }
        // --- 核心修复逻辑 ---
        if (!success) {
            svc_.cmd_close_graph(session_name); // 在抛出异常前，先清理session
            auto last_err = svc_.cmd_last_error(session_name);
            std::string reason = last_err ? last_err->message : "Unknown error during compute.";
            throw std::runtime_error("Benchmark run " + std::to_string(i) + " for '" + config.name + "' failed. Reason: " + reason);
        }
        // --- 修复结束 ---

        svc_.cmd_close_graph(session_name);
        all_runs.push_back(single_run_result);
    }
    
    BenchmarkResult final_result;
    final_result.benchmark_name = config.name;
    final_result.op_name = config.auto_generate ? config.generator_config.main_op_type : "custom";
    final_result.width = config.auto_generate ? config.generator_config.width : 0;
    final_result.height = config.auto_generate ? config.generator_config.height : 0;
    // [修改] 设置最终报告的线程数
    final_result.num_threads = config.execution.threads > 0 ? config.execution.threads : std::thread::hardware_concurrency();
    
    analyze_results(final_result, all_runs);

    return final_result;
}


/**
 * @brief 分析多次基准测试运行结果并汇总到 final_result 中
 *
 * 对提供的多次运行结果执行以下统计：
 *   1. 计算所有运行的平均总耗时（total_duration_ms）
 *   2. 提取与 final_result.op_name 匹配的事件执行时间，排序后去掉前后 20% 离群值，计算典型执行时间（typical_execution_time_ms）
 *   3. 计算所有运行的平均 IO 耗时（io_duration_ms）
 *   4. 占位计算调度器开销（scheduler_overhead_ms，目前暂为 0）
 *   5. 填充 CPU、操作系统和编译器信息字段（cpu_info、os_info、compiler_info）
 *
 * @param[out] final_result 用于存放汇总后的基准测试结果
 * @param[in]  all_runs     包含多次运行详细结果的向量，每个元素包含总耗时、事件列表和 IO 耗时等信息
 */
void BenchmarkService::analyze_results(BenchmarkResult& final_result, const std::vector<BenchmarkResult>& all_runs) {
    if (all_runs.empty()) return;

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
    
    size_t outliers_count = execution_times.size() / 5; // 去掉前后20%
    size_t start_index = outliers_count;
    size_t count = execution_times.size() - 2 * outliers_count;

    if (count > 0) {
        double sum = std::accumulate(execution_times.begin() + start_index, execution_times.begin() + start_index + count, 0.0);
        final_result.typical_execution_time_ms = sum / count;
    } else if (!execution_times.empty()) {
        final_result.typical_execution_time_ms = execution_times[0]; // 如果数据太少，则取第一个
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


std::vector<BenchmarkResult> BenchmarkService::RunAll(const std::string& benchmark_dir) {
    // [修复] 在所有测试开始前，执行清理操作
    // std::cout << "Cleaning up previous benchmark artifacts in '" << benchmark_dir << "'..." << std::endl;
    cleanup_generated_files(benchmark_dir);

    auto configs = load_configs(benchmark_dir);
    std::vector<BenchmarkResult> results;
    for (const auto& config : configs) {
        if (config.enabled) {
            // std::cout << "Running benchmark: " << config.name << "..." << std::endl;
            try {
                // [修复] 将 benchmark_dir 和 runs 次数传递给 Run 函数
                results.push_back(Run(benchmark_dir, config, config.execution.runs));
            } catch (const std::exception& e) {
                std::cerr << "Error running benchmark '" << config.name << "': " << e.what() << std::endl;
            }
        }
    }
    return results;
}


std::vector<BenchmarkSessionConfig> BenchmarkService::load_configs(const std::string& benchmark_dir) {
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
            // 对于手动指定的YAML，我们假定它是相对于 benchmark_dir 的路径
            cfg.yaml_path = (fs::path(benchmark_dir) / session_node["yaml_path"].as<std::string>()).string();
        }
        if (session_node["statistics"] && session_node["statistics"].IsSequence()) {
            cfg.statistics = session_node["statistics"].as<std::vector<std::string>>();
        }
        if (session_node["execution"]) {
            cfg.execution.runs = session_node["execution"]["runs"].as<int>(10);
            cfg.execution.threads = session_node["execution"]["threads"].as<int>(0);
            cfg.execution.parallel = session_node["execution"]["parallel"].as<bool>(true);
        }
        configs.push_back(cfg);
    }
    return configs;
}

} // namespace ps
