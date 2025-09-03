#include <iostream>
#include <cassert>
#include <cmath>
#include "node_graph.hpp"
#include "kernel/ops.hpp"
#include "adapter/buffer_adapter_opencv.hpp"

// 一个简单的图像内容校验函数
double image_checksum(const ps::NodeOutput& output) {
    if (output.image_buffer.width == 0) return 0.0;
    cv::Mat mat = ps::toCvMat(output.image_buffer);
    cv::Scalar sum = cv::sum(mat);
    return sum[0] + sum[1] + sum[2] + sum[3];
}

void print_test_status(const std::string& name, bool success) {
    std::cout << "[ " << (success ? "PASS" : "FAIL") << " ] " << name << std::endl;
}

int main() {
    // 准备工作
    ps::ops::register_builtin();
    ps::NodeGraph graph;
    
    // Test 1: 验证 preserved 节点逻辑
    std::cout << "--- Running Test 1: 'preserved' node logic ---" << std::endl;
    bool test1_success = false;
    try {
        ps::Node noise_node;
        noise_node.id = 0;
        noise_node.name = "Perlin Noise";
        noise_node.type = "image_generator";
        noise_node.subtype = "perlin_noise";
        noise_node.preserved = true; // 标记为 preserved
        noise_node.parameters["width"] = 64;
        noise_node.parameters["height"] = 64;
        noise_node.parameters["seed"] = 42; // *** FIX: 添加固定种子 ***

        ps::Node blur_node;
        blur_node.id = 1;
        blur_node.name = "Blur";
        blur_node.type = "image_process";
        blur_node.subtype = "gaussian_blur";
        blur_node.image_inputs.push_back({0});

        graph.add_node(noise_node);
        graph.add_node(blur_node);

        // 第一次计算，所有节点都应被计算
        graph.compute(1, "int8", false, true); // enable timing
        assert(graph.timing_results.node_timings.size() == 2);
        assert(graph.timing_results.node_timings[0].source == "computed"); // noise
        assert(graph.timing_results.node_timings[1].source == "computed"); // blur

        // 第二次计算，force_recache=true
        // preserved 节点 (0) 应该从缓存加载，非 preserved 节点 (1) 应该重新计算
        graph.compute(1, "int8", true, true); // force recache
        assert(graph.timing_results.node_timings.size() == 2);
        assert(graph.timing_results.node_timings[0].source == "memory_cache"); // noise should be cached
        assert(graph.timing_results.node_timings[1].source == "computed");     // blur should be recomputed
        
        test1_success = true;
    } catch (const std::exception& e) {
        std::cerr << "Test 1 failed with exception: " << e.what() << std::endl;
    }
    print_test_status("Preserved node is not cleared by force_recache", test1_success);
    
    // Test 2: 验证 compute_parallel 结果与 compute 一致
    std::cout << "\n--- Running Test 2: 'compute_parallel' correctness ---" << std::endl;
    bool test2_success = false;
    double checksum_seq = 0.0;
    double checksum_par = 0.0;
    try {
        graph.clear();
        graph.clear_cache();

        ps::Node n0; n0.id=0; n0.type="image_generator"; n0.subtype="perlin_noise"; n0.parameters["width"]=128; n0.parameters["height"]=128;
        n0.parameters["seed"] = 123; // *** FIX: 添加固定种子以保证确定性 ***
        ps::Node n1; n1.id=1; n1.type="image_process"; n1.subtype="gaussian_blur"; n1.image_inputs.push_back({0});
        ps::Node n2; n2.id=2; n2.type="image_process"; n2.subtype="curve_transform"; n2.image_inputs.push_back({1});
        graph.add_node(n0); graph.add_node(n1); graph.add_node(n2);
        
        // 顺序计算
        auto& out_seq = graph.compute(2, "int8");
        checksum_seq = image_checksum(out_seq);
        
        // 清理缓存并并行计算
        graph.clear_memory_cache();
        auto& out_par = graph.compute_parallel(2, "int8");
        checksum_par = image_checksum(out_par);

        assert(std::abs(checksum_seq - checksum_par) < 1e-3);
        test2_success = true;

    } catch (const std::exception& e) {
        std::cerr << "Test 2 failed with exception: " << e.what() << std::endl;
    }
    print_test_status("Parallel result matches sequential result", test2_success);
    std::cout << "  Sequential Checksum: " << checksum_seq << std::endl;
    std::cout << "  Parallel Checksum:   " << checksum_par << std::endl;

    if (test1_success && test2_success) {
        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << "\n❌ Some tests failed." << std::endl;
        return 1;
    }
}