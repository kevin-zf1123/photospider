#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

#include "adapter/buffer_adapter_opencv.hpp"
#include "graph_model.hpp"
#include "kernel/ops.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps {
namespace {

Node make_source_node(int id, const std::string& name, int width, int height) {
  Node node;
  node.id = id;
  node.name = name;
  node.type = "image_generator";
  node.subtype = "constant";
  node.parameters = YAML::Node(YAML::NodeType::Map);
  if (width > 0)
    node.parameters["width"] = width;
  if (height > 0)
    node.parameters["height"] = height;
  node.parameters["value"] = 128;
  return node;
}

Node make_unparameterized_source_node(int id, const std::string& name) {
  Node node;
  node.id = id;
  node.name = name;
  node.type = "image_generator";
  node.subtype = "constant";
  node.parameters = YAML::Node(YAML::NodeType::Map);
  return node;
}

Node make_blur_node(int id, int parent_id, int ksize) {
  Node node;
  node.id = id;
  node.name = "blur";
  node.type = "image_process";
  node.subtype = "gaussian_blur";
  node.parameters = YAML::Node(YAML::NodeType::Map);
  node.parameters["ksize"] = ksize;
  node.image_inputs.push_back(ImageInput{parent_id, "image"});
  return node;
}

Node make_resize_node(int id, int parent_id, int width, int height,
                      const std::string& interpolation = "linear") {
  Node node;
  node.id = id;
  node.name = "resize";
  node.type = "image_process";
  node.subtype = "resize";
  node.parameters = YAML::Node(YAML::NodeType::Map);
  node.parameters["width"] = width;
  node.parameters["height"] = height;
  node.parameters["interpolation"] = interpolation;
  node.image_inputs.push_back(ImageInput{parent_id, "image"});
  return node;
}

Node make_curve_node(int id, int parent_id) {
  Node node;
  node.id = id;
  node.name = "curve";
  node.type = "image_process";
  node.subtype = "curve_transform";
  node.parameters = YAML::Node(YAML::NodeType::Map);
  node.image_inputs.push_back(ImageInput{parent_id, "image"});
  return node;
}

GraphModel make_graph() {
  return GraphModel(std::filesystem::temp_directory_path() /
                    "photospider-propagation-contracts");
}

void seed_hp_extent(GraphModel& graph, int node_id, int width, int height) {
  graph.mutate_node_runtime_state(node_id, [&](auto& state) {
    state.cached_output_high_precision = NodeOutput{};
    state.cached_output_high_precision->image_buffer =
        make_aligned_cpu_image_buffer(width, height, 1, DataType::FLOAT32);
  });
}

}  // namespace

TEST(PropagationContracts, BackwardLinearChainPropagatesPreciseDirtyRoi) {
  ops::register_builtin();
  GraphModel graph = make_graph();
  graph.add_node(make_source_node(1, "source", 1024, 1024));
  graph.add_node(make_blur_node(2, 1, 21));
  graph.add_node(make_resize_node(20, 2, 512, 512));
  graph.add_node(make_curve_node(200, 20));
  graph.validate_topology();
  seed_hp_extent(graph, 2, 1024, 1024);
  seed_hp_extent(graph, 20, 512, 512);
  seed_hp_extent(graph, 200, 512, 512);

  RoiPropagationService propagation;
  std::optional<cv::Rect> propagated =
      propagation.project_roi_backward(graph, 200, cv::Rect(10, 20, 30, 40), 1);

  ASSERT_TRUE(propagated.has_value());
  // curve_transform: identity.
  // resize 512->1024 with linear padding 1 yields [19,39,62x82].
  // The ROI propagation boundary also merges single-input local_inverse_matrix
  // propagation; with the default identity matrix, the resize step therefore
  // carries the union of [19,39,62x82] and [10,20,30x40] = [10,20,71x101].
  // gaussian_blur ksize=21 expands by radius 10, producing [0,10,91x121].
  EXPECT_EQ(*propagated, cv::Rect(0, 10, 91, 121));
}

TEST(PropagationContracts, BackwardResizeRequiresHpParentExtent) {
  ops::register_builtin();
  GraphModel graph = make_graph();
  graph.add_node(make_unparameterized_source_node(1, "source_without_hp"));
  graph.add_node(make_resize_node(2, 1, 4, 4, "nearest"));
  graph.validate_topology();

  RoiPropagationService propagation;
  EXPECT_FALSE(
      propagation.project_roi_backward(graph, 2, cv::Rect(1, 1, 1, 1), 1)
          .has_value())
      << "Missing HP state must not provide propagation extent.";

  graph.mutate_node_runtime_state(1, [](auto& state) {
    state.cached_output_high_precision = NodeOutput{};
    state.cached_output_high_precision->image_buffer =
        make_aligned_cpu_image_buffer(8, 8, 1, DataType::FLOAT32);
  });

  std::optional<cv::Rect> propagated =
      propagation.project_roi_backward(graph, 2, cv::Rect(1, 1, 1, 1), 1);
  ASSERT_TRUE(propagated.has_value());
  EXPECT_EQ(*propagated, cv::Rect(2, 2, 2, 2));
}

}  // namespace ps
