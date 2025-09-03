#include "node_graph.hpp"
#include "kernel/ops.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/kernel.hpp"

// Helper to check if two CV_32F mats are approximately equal
bool are_mats_close(const cv::Mat& m1, const cv::Mat& m2, float tol = 1e-5) {
    if (m1.size() != m2.size() || m1.type() != m2.type()) {
        std::cerr << "Matrix dimensions or types differ." << std::endl;
        return false;
    }
    cv::Mat diff;
    cv::absdiff(m1, m2, diff);
    double minVal, maxVal;
    cv::minMaxLoc(diff, &minVal, &maxVal);
    // std::cout << "Max difference: " << maxVal << std::endl;
    return maxVal < tol;
}

void test_sequential_vs_parallel() {
    std::cout << "--- Test: Sequential vs Parallel Equivalence ---" << std::endl;
    
    ps::Kernel kernel;
    kernel.plugins().seed_builtins_from_registry();

    ps::Node constant_node;
    constant_node.id = 1;
    constant_node.name = "Source";
    constant_node.type = "image_generator";
    constant_node.subtype = "constant";
    constant_node.parameters["width"] = 128;
    constant_node.parameters["height"] = 128;
    constant_node.parameters["value"] = 100;

    ps::Node blur_node;
    blur_node.id = 2;
    blur_node.name = "Blur";
    blur_node.type = "image_process";
    blur_node.subtype = "gaussian_blur";
    blur_node.parameters["ksize"] = 5;
    blur_node.image_inputs.push_back({1, "image"});

    auto graph_name_opt = kernel.load_graph("test_graph", "sessions", "");
    assert(graph_name_opt.has_value());
    std::string graph_name = *graph_name_opt;
    
    kernel.post(graph_name, [&](ps::NodeGraph& g){
        g.clear();
        g.add_node(constant_node);
        g.add_node(blur_node);
        return 0;
    }).get();
    
    auto mat_seq_opt = kernel.compute_and_get_image(graph_name, 2, "int8", false, false, false, false, nullptr);
    assert(mat_seq_opt.has_value() && !mat_seq_opt->empty());

    auto mat_par_opt = kernel.compute_and_get_image(graph_name, 2, "int8", false, false, true, false, nullptr);
    assert(mat_par_opt.has_value() && !mat_par_opt->empty());

    assert(are_mats_close(*mat_seq_opt, *mat_par_opt));

    std::cout << "Sequential and Parallel outputs are equivalent. OK." << std::endl;
}

void test_parallel_scheduler_complex_graph() {
    std::cout << "--- Test: Parallel Scheduler on Complex (Diamond) Graph ---" << std::endl;

    ps::Kernel kernel;
    kernel.plugins().seed_builtins_from_registry();

    ps::Node node_a;
    node_a.id = 1; node_a.name = "A"; node_a.type = "image_generator"; node_a.subtype = "constant";
    node_a.parameters["width"] = 64; node_a.parameters["height"] = 64; node_a.parameters["value"] = 128;

    ps::Node node_b;
    node_b.id = 2; node_b.name = "B"; node_b.type = "image_process"; node_b.subtype = "gaussian_blur";
    node_b.parameters["ksize"] = 3;
    node_b.image_inputs.push_back({1, "image"});

    ps::Node node_c;
    node_c.id = 3; node_c.name = "C"; node_c.type = "image_process"; node_c.subtype = "curve_transform";
    node_c.parameters["k"] = 0.5;
    node_c.image_inputs.push_back({1, "image"});
    
    ps::Node node_d;
    node_d.id = 4; node_d.name = "D"; node_d.type = "image_mixing"; node_d.subtype = "add_weighted";
    node_d.parameters["alpha"] = 0.5; node_d.parameters["beta"] = 0.5;
    node_d.image_inputs.push_back({2, "image"});
    node_d.image_inputs.push_back({3, "image"});

    auto graph_name_opt = kernel.load_graph("diamond_test", "sessions", "");
    assert(graph_name_opt.has_value());
    std::string graph_name = *graph_name_opt;
    
    kernel.post(graph_name, [&](ps::NodeGraph& g){
        g.clear();
        g.add_node(node_a);
        g.add_node(node_b);
        g.add_node(node_c);
        g.add_node(node_d);
        return 0;
    }).get();

    auto mat_par_opt = kernel.compute_and_get_image(graph_name, 4, "int8", true, true, true, false, nullptr);
    assert(mat_par_opt.has_value() && !mat_par_opt->empty());

    std::cout << "Diamond graph computed successfully with new parallel scheduler. OK." << std::endl;
}

int main() {
    try {
        test_sequential_vs_parallel();
        test_parallel_scheduler_complex_graph();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "All tests passed!" << std::endl;
    return 0;
}