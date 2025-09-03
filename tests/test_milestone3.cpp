#include <iostream>
#include <cassert>
#include <optional>
#include "ps_types.hpp"
#include "kernel/interaction.hpp" // For InteractionService
#include "kernel/kernel.hpp"      // For Kernel

// Dummy op function, does nothing.
ps::NodeOutput dummy_cpu_op(const ps::Node&, const std::vector<const ps::NodeOutput*>&) {
    return ps::NodeOutput{};
}

ps::NodeOutput dummy_gpu_op(const ps::Node&, const std::vector<const ps::NodeOutput*>&) {
    return ps::NodeOutput{};
}


void test_device_preference_metadata() {
    std::cout << "--- Running Test: Device Preference Metadata ---" << std::endl;

    auto& registry = ps::OpRegistry::instance();

    registry.register_op("test", "cpu_default", dummy_cpu_op);
    ps::OpMetadata cpu_meta;
    cpu_meta.device_preference = ps::Device::CPU;
    registry.register_op("test", "cpu_explicit", dummy_cpu_op, cpu_meta);
    ps::OpMetadata gpu_meta;
    gpu_meta.device_preference = ps::Device::GPU_METAL;
    registry.register_op("test", "gpu_explicit", dummy_gpu_op, gpu_meta);

    std::cout << "  Verifying 'test:cpu_default'..." << std::endl;
    auto meta1 = registry.get_metadata("test", "cpu_default");
    assert(meta1.has_value() && "Metadata for cpu_default should exist.");
    assert(meta1->device_preference == ps::Device::CPU && "Default op should have CPU device preference.");
    std::cout << "  OK." << std::endl;

    std::cout << "  Verifying 'test:cpu_explicit'..." << std::endl;
    auto meta2 = registry.get_metadata("test", "cpu_explicit");
    assert(meta2.has_value() && "Metadata for cpu_explicit should exist.");
    assert(meta2->device_preference == ps::Device::CPU && "Explicit CPU op should have CPU device preference.");
    std::cout << "  OK." << std::endl;

    std::cout << "  Verifying 'test:gpu_explicit'..." << std::endl;
    auto meta3 = registry.get_metadata("test", "gpu_explicit");
    assert(meta3.has_value() && "Metadata for gpu_explicit should exist.");
    assert(meta3->device_preference == ps::Device::GPU_METAL && "Explicit GPU op should have GPU_METAL device preference.");
    std::cout << "  OK." << std::endl;

    std::cout << "  Verifying non-existent op..." << std::endl;
    auto meta4 = registry.get_metadata("test", "non_existent");
    assert(!meta4.has_value() && "Metadata for non_existent op should not exist.");
    std::cout << "  OK." << std::endl;
    
    std::cout << "--- Test Passed ---" << std::endl;
}

void test_gpu_context_initialization() {
    std::cout << "--- Running Test: GPU Context Initialization ---" << std::endl;

    ps::Kernel kernel;
    ps::InteractionService svc(kernel);

    std::string graph_name = "test_gpu_graph";
    auto loaded_name = svc.cmd_load_graph(graph_name, "sessions", "");
    assert(loaded_name.has_value() && "Graph should be loadable.");
    assert(*loaded_name == graph_name && "Graph name should match.");
    std::cout << "  Graph session created." << std::endl;

#ifdef __APPLE__
    std::cout << "  Platform is Apple, checking for Metal device..." << std::endl;
    id metal_device = svc.cmd_get_metal_device(graph_name);
    assert(metal_device != nullptr && "On Apple platform, Metal device should be initialized and not null.");
    std::cout << "  Metal device successfully retrieved. OK." << std::endl;
#else
    std::cout << "  Platform is not Apple, skipping Metal device check." << std::endl;
    id metal_device = svc.cmd_get_metal_device(graph_name);
    assert(metal_device == nullptr && "On non-Apple platform, Metal device should be null.");
    std::cout << "  Metal device is correctly null. OK." << std::endl;
#endif
    
    svc.cmd_close_graph(graph_name);
    std::cout << "--- Test Passed ---" << std::endl;
}

int main() {
    try {
        test_device_preference_metadata();
        test_gpu_context_initialization();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}