#include <iostream>
#include <cassert>
#include <cmath>
#include "node_graph.hpp"
#include "kernel/ops.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/kernel.hpp" // [修复] 缺少头文件
#include "kernel/interaction.hpp" // [修复] 缺少头文件

// 一个简单的图像内容校验函数
double image_checksum(const ps::NodeOutput& output) {
    if (output.image_buffer.width == 0) return 0.0;
    cv::Mat mat = ps::toCvMat(output.image_buffer);
    cv::Scalar sum = cv::sum(mat);
    return sum[0] + sum[1] + sum[2] + sum[3];
}

void print_test_status(const std::string& name, bool success) {
    std::cout << "[ " << (success ? "PASS" : "FAIL") << " ] " << name << std::endl;
    assert(success);
}

// 模拟的 GraphRuntime
class MockGraphRuntime {
public:
    ps::NodeGraph graph;
    MockGraphRuntime() : graph("cache") {}
};

void test_preserved_node_logic() {
    std::cout << "--- Running Test: 'preserved' node logic ---" << std::endl;
    bool test_success = false;
    try {
        ps::ops::register_builtin();
        MockGraphRuntime runtime;
        auto& graph = runtime.graph;
        
        ps::Node noise_node;
        noise_node.id = 0;
        noise_node.name = "Perlin Noise";
        noise_node.type = "image_generator";
        noise_node.subtype = "perlin_noise";
        noise_node.preserved = true; // 标记为 preserved
        noise_node.parameters["width"] = 64;
        noise_node.parameters["height"] = 64;
        noise_node.parameters["seed"] = 42;

        ps::Node blur_node;
        blur_node.id = 1;
        blur_node.name = "Blur";
        blur_node.type = "image_process";
        blur_node.subtype = "gaussian_blur";
        blur_node.image_inputs.push_back({0});

        graph.add_node(noise_node);
        graph.add_node(blur_node);

        // 第一次计算
        graph.compute(1, "int8", false, true); // enable timing
        assert(graph.timing_results.node_timings.size() == 2);
        assert(graph.timing_results.node_timings[0].source == "computed"); // noise
        assert(graph.timing_results.node_timings[1].source == "computed"); // blur

        // 第二次计算，force_recache=true
        graph.compute(1, "int8", true, true); // force recache
        assert(graph.timing_results.node_timings.size() == 2);
        assert(graph.timing_results.node_timings[0].source == "memory_cache"); // noise should be cached
        assert(graph.timing_results.node_timings[1].source == "computed");     // blur should be recomputed
        
        test_success = true;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
    }
    print_test_status("Preserved node is not cleared by force_recache", test_success);
}

void test_parallel_correctness() {
    std::cout << "\n--- Running Test: 'compute_parallel' correctness ---" << std::endl;
    bool test_success = false;
    double checksum_seq = 0.0;
    double checksum_par = 0.0;
    try {
        ps::ops::register_builtin();
        ps::Kernel kernel;
        auto graph_name_opt = kernel.load_graph("test_graph", "sessions", "");
        assert(graph_name_opt.has_value());
        std::string graph_name = *graph_name_opt;

        kernel.post(graph_name, [](ps::NodeGraph& g){
            g.clear();
            ps::Node n0; n0.id=0; n0.type="image_generator"; n0.subtype="perlin_noise"; n0.parameters["width"]=128; n0.parameters["height"]=128; n0.parameters["seed"] = 123;
            ps::Node n1; n1.id=1; n1.type="image_process"; n1.subtype="gaussian_blur"; n1.image_inputs.push_back({0});
            ps::Node n2; n2.id=2; n2.type="image_process"; n2.subtype="curve_transform"; n2.image_inputs.push_back({1});
            g.add_node(n0); g.add_node(n1); g.add_node(n2);
            return 0;
        }).get();
        
        // 顺序计算
        auto img_seq_opt = kernel.compute_and_get_image(graph_name, 2, "int8", false, false, false);
        assert(img_seq_opt.has_value());
        ps::NodeOutput out_seq; out_seq.image_buffer = ps::fromCvMat(*img_seq_opt);
        checksum_seq = image_checksum(out_seq);
        
        // 清理缓存并并行计算
        kernel.clear_memory_cache(graph_name);
        auto img_par_opt = kernel.compute_and_get_image(graph_name, 2, "int8", false, false, true); // parallel = true
        assert(img_par_opt.has_value());
        ps::NodeOutput out_par; out_par.image_buffer = ps::fromCvMat(*img_par_opt);
        checksum_par = image_checksum(out_par);

        assert(std::abs(checksum_seq - checksum_par) < 1e-3);
        test_success = true;

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
    }
    print_test_status("Parallel result matches sequential result", test_success);
    std::cout << "  Sequential Checksum: " << checksum_seq << std::endl;
    std::cout << "  Parallel Checksum:   " << checksum_par << std::endl;
}

int main() {
    bool all_passed = true;
    try {
        test_preserved_node_logic();
    } catch(...) { all_passed = false; }
    try {
        test_parallel_correctness();
    } catch(...) { all_passed = false; }

    if (all_passed) {
        std::cout << "\n✅ All milestone 2 tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << "\n❌ Some milestone 2 tests failed." << std::endl;
        return 1;
    }
}