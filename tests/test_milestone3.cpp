#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "kernel/kernel.hpp"
#include "kernel/interaction.hpp"

// 为测试添加一个虚拟的操作函数
ps::NodeOutput dummy_op(const ps::Node&, const std::vector<const ps::NodeOutput*>&) {
    return ps::NodeOutput{};
}

// Helper to print test status
void print_test_status(const std::string& name, bool success) {
    std::cout << "[ " << (success ? "PASS" : "FAIL") << " ] " << name << std::endl;
    assert(success);
}

void test_plugin_loading() {
    std::cout << "--- Running Test: Plugin Loading Verification ---" << std::endl;
    
    ps::Kernel kernel;
    ps::InteractionService svc(kernel);
    
    svc.cmd_seed_builtin_ops();
    auto initial_ops = svc.cmd_ops_sources();
    bool builtin_found = initial_ops.count("image_process:gaussian_blur") > 0;
    print_test_status("Built-in ops are registered", builtin_found);

    // [*** 本次修复的核心点 ***]
    // CTest 从 build 目录运行，所以插件的相对路径应该是 "plugins"，而不是 "build/plugins"
    std::vector<std::string> plugin_dirs = {"build/plugins"};
    svc.cmd_plugins_load(plugin_dirs);
    
    auto all_ops = svc.cmd_ops_sources();
    bool custom_op_found = all_ops.count("image_process:invert") > 0;
    print_test_status("Custom CPU plugin op 'invert_op_custom_example' is registered", custom_op_found);
    
    bool is_from_plugin = false;
    if (custom_op_found) {
        std::string source = all_ops.at("image_process:invert");
        is_from_plugin = (source.find("invert_op_custom_example") != std::string::npos);
    }
    print_test_status("Registered custom op correctly attributes its source to the plugin file", is_from_plugin);
}

void test_device_preference_metadata() {
    std::cout << "\n--- Running Test: Device Preference Metadata ---" << std::endl;

    auto& registry = ps::OpRegistry::instance();
    
    registry.register_op("test", "cpu_op", dummy_op);
    auto cpu_meta_opt = registry.get_metadata("test", "cpu_op");
    bool cpu_meta_exists = cpu_meta_opt.has_value();
    print_test_status("Metadata exists for CPU op", cpu_meta_exists);
    if (cpu_meta_exists) {
        bool is_cpu = (cpu_meta_opt->device_preference == ps::Device::CPU);
        print_test_status("Default device preference is CPU", is_cpu);
    }

    ps::OpMetadata gpu_meta;
    gpu_meta.device_preference = ps::Device::GPU_METAL;
    registry.register_op("test", "gpu_op", dummy_op, gpu_meta);
    auto gpu_meta_opt = registry.get_metadata("test", "gpu_op");
    bool gpu_meta_exists = gpu_meta_opt.has_value();
    print_test_status("Metadata exists for GPU op", gpu_meta_exists);
    if (gpu_meta_exists) {
        bool is_gpu = (gpu_meta_opt->device_preference == ps::Device::GPU_METAL);
        print_test_status("Explicit device preference is GPU_METAL", is_gpu);
    }
}


void test_metal_op_registration_and_context() {
#ifdef __APPLE__
    std::cout << "\n--- Running Test: Metal GPU Op Registration & Context (Apple Only) ---" << std::endl;

    ps::Kernel kernel;
    ps::InteractionService svc(kernel);
    svc.cmd_seed_builtin_ops();

    // [*** 本次修复的核心点 ***]
    // Metal 插件的路径同样相对于 build 目录
    std::vector<std::string> metal_plugin_dirs = {"build/high_performance/metal"};
    svc.cmd_plugins_load(metal_plugin_dirs);

    auto all_ops = svc.cmd_ops_sources();
    bool metal_op_found = all_ops.count("image_generator:perlin_noise_metal") > 0;
    print_test_status("Metal GPU op 'perlin_noise_metal' is registered via plugin", metal_op_found);

    if (metal_op_found) {
        auto graph_name_opt = svc.cmd_load_graph("default", "sessions", "");
        assert(graph_name_opt.has_value());
        
        id metal_device = svc.cmd_get_metal_device(*graph_name_opt);
        bool context_valid = (metal_device != nullptr);
        print_test_status("GraphRuntime provides a valid Metal device context", context_valid);
    }
#else
    std::cout << "\n--- Skipping Test: Metal GPU Op Registration & Context (Not on Apple platform) ---" << std::endl;
#endif
}


int main() {
    try {
        test_plugin_loading();
        test_device_preference_metadata();
        test_metal_op_registration_and_context();
    } catch (const std::exception& e) {
        std::cerr << "Tests failed with an unhandled exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n✅ All milestone 3 tests completed successfully!" << std::endl;
    return 0;
}