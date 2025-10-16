#include <cassert>
#include <filesystem>
#include <iostream>

#include "adapter/buffer_adapter_opencv.hpp"
#include "graph_model.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/ops.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

// Helper for asserting conditions
void ps_assert(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << std::endl;
    std::exit(1);
  }
}

void test_simple_sequential_compute() {
  std::cout << "--- Running test: test_simple_sequential_compute ---\n";
  ps::ops::register_builtin();
  ps::GraphModel graph;
  ps::GraphTraversalService traversal;
  ps::GraphCacheService cache;
  ps::GraphEventService events;
  ps::ComputeService compute(traversal, cache, events);

  ps::Node n1, n2, n3;
  n1.id = 1;
  n1.name = "const100";
  n1.type = "image_generator";
  n1.subtype = "constant";
  n1.parameters["width"] = 10;
  n1.parameters["height"] = 10;
  n1.parameters["value"] = 100;

  n2.id = 2;
  n2.name = "const50";
  n2.type = "image_generator";
  n2.subtype = "constant";
  n2.parameters["width"] = 10;
  n2.parameters["height"] = 10;
  n2.parameters["value"] = 50;

  n3.id = 3;
  n3.name = "add";
  n3.type = "image_mixing";
  n3.subtype = "add_weighted";
  n3.image_inputs.push_back({1, "image"});
  n3.image_inputs.push_back({2, "image"});
  n3.parameters["alpha"] = 0.5;
  n3.parameters["beta"] = 0.5;

  graph.add_node(n1);
  graph.add_node(n2);
  graph.add_node(n3);

  auto& output =
      compute.compute(graph, 3, "int8", false, false, false, nullptr);
  cv::Mat result_mat = ps::toCvMat(output.image_buffer);

  float expected = (100.0f / 255.0f * 0.5f) + (50.0f / 255.0f * 0.5f);
  ps_assert(std::abs(result_mat.at<float>(0, 0) - expected) < 1e-6,
            "Pixel value mismatch in sequential compute");

  std::cout << "PASS\n";
}

void test_parallel_scheduler_simple() {
  std::cout << "--- Running test: test_parallel_scheduler_simple ---\n";
  ps::ops::register_builtin();

  ps::GraphRuntime::Info info{"test_session", "sessions/test_session"};
  ps::GraphRuntime runtime(info);
  runtime.start();

  auto& graph = runtime.model();
  ps::GraphTraversalService traversal;
  ps::GraphCacheService cache;

  ps::Node n1, n2, n3;
  n1.id = 1;
  n1.name = "const100";
  n1.type = "image_generator";
  n1.subtype = "constant";
  n1.parameters["width"] = 20;
  n1.parameters["height"] = 20;
  n1.parameters["value"] = 100;

  n2.id = 2;
  n2.name = "const50";
  n2.type = "image_generator";
  n2.subtype = "constant";
  n2.parameters["width"] = 20;
  n2.parameters["height"] = 20;
  n2.parameters["value"] = 50;

  n3.id = 3;
  n3.name = "add";
  n3.type = "image_mixing";
  n3.subtype = "add_weighted";
  n3.image_inputs.push_back({1, "image"});
  n3.image_inputs.push_back({2, "image"});
  n3.parameters["alpha"] = 1.0;
  n3.parameters["beta"] = 1.0;

  graph.add_node(n1);
  graph.add_node(n2);
  graph.add_node(n3);

  ps::ComputeService compute(traversal, cache, runtime.event_service());
  auto& output = compute.compute_parallel(graph, runtime, 3, "int8", false,
                                          false, false, nullptr);
  cv::Mat result_mat = ps::toCvMat(output.image_buffer);

  float expected = (100.0f / 255.0f) + (50.0f / 255.0f);
  ps_assert(std::abs(result_mat.at<float>(5, 5) - expected) < 1e-6,
            "Pixel value mismatch in parallel compute");

  std::cout << "PASS\n";
  runtime.stop();
}

// [新增] 专门用于验证本次修复的测试用例
void test_parallel_mixing_with_resize() {
  std::cout << "--- Running test: test_parallel_mixing_with_resize ---\n";
  ps::ops::register_builtin();

  ps::GraphRuntime::Info info{"resize_test", "sessions/resize_test"};
  ps::GraphRuntime runtime(info);
  runtime.start();

  auto& graph = runtime.model();
  ps::GraphTraversalService traversal;
  ps::GraphCacheService cache;

  // Node 1: Base image, 100x100
  ps::Node n1;
  n1.id = 1;
  n1.name = "base_img";
  n1.type = "image_generator";
  n1.subtype = "constant";
  n1.parameters["width"] = 100;
  n1.parameters["height"] = 100;
  n1.parameters["value"] = 200;

  // Node 2: Smaller image, 50x50, to be resized
  ps::Node n2;
  n2.id = 2;
  n2.name = "small_img";
  n2.type = "image_generator";
  n2.subtype = "constant";
  n2.parameters["width"] = 50;
  n2.parameters["height"] = 50;
  n2.parameters["value"] = 50;

  // Node 3: Add them. Default merge_strategy is "resize"
  ps::Node n3;
  n3.id = 3;
  n3.name = "add_diff_size";
  n3.type = "image_mixing";
  n3.subtype = "add_weighted";
  n3.image_inputs.push_back({1, "image"});
  n3.image_inputs.push_back({2, "image"});
  n3.parameters["alpha"] = 1.0;
  n3.parameters["beta"] = 1.0;

  graph.add_node(n1);
  graph.add_node(n2);
  graph.add_node(n3);

  bool threw = false;
  try {
    ps::ComputeService compute(traversal, cache, runtime.event_service());
    auto& output = compute.compute_parallel(graph, runtime, 3, "int8", false,
                                            false, false, nullptr);
    cv::Mat result_mat = ps::toCvMat(output.image_buffer);

    ps_assert(result_mat.cols == 100, "Result width should match first input");
    ps_assert(result_mat.rows == 100, "Result height should match first input");

    float expected =
        (200.0f / 255.0f) +
        (50.0f / 255.0f);  // smaller image is resized and then added
    ps_assert(std::abs(result_mat.at<float>(50, 50) - expected) < 1e-6,
              "Pixel value mismatch after resize");
  } catch (const std::exception& e) {
    std::cerr << "Caught exception: " << e.what() << std::endl;
    threw = true;
  }

  ps_assert(!threw,
            "compute_parallel should not throw for different-sized inputs with "
            "default resize strategy");

  std::cout << "PASS\n";
  runtime.stop();
}

int main() {
  try {
    // 清理旧的测试会话，以防干扰
    std::filesystem::remove_all("sessions");

    test_simple_sequential_compute();
    test_parallel_scheduler_simple();
    test_parallel_mixing_with_resize();  // 运行新测试

    std::cout << "\nAll Milestone 2 tests passed!\n";
  } catch (const std::exception& e) {
    std::cerr << "\nMilestone 2 test suite failed: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
