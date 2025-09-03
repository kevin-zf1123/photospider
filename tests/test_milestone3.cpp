#include <iostream>
#include <cassert>
#include <optional>
#include "ps_types.hpp"

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

    // 1. 注册一个默认的 CPU op
    registry.register_op("test", "cpu_default", dummy_cpu_op);

    // 2. 注册一个显式声明的 CPU op
    ps::OpMetadata cpu_meta;
    cpu_meta.device_preference = ps::Device::CPU;
    registry.register_op("test", "cpu_explicit", dummy_cpu_op, cpu_meta);

    // 3. 注册一个显式声明的 GPU op
    ps::OpMetadata gpu_meta;
    gpu_meta.device_preference = ps::Device::GPU_METAL;
    registry.register_op("test", "gpu_explicit", dummy_gpu_op, gpu_meta);

    // 4. 验证元数据
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

int main() {
    try {
        test_device_preference_metadata();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}