#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "graph/graph_io_service.hpp"
#include "graph/graph_model.hpp"
#include "graph/graph_traversal_service.hpp"
#include "support/graph_document_test_dependencies.hpp"

namespace ps {
namespace {

Node make_source(int id) {
  Node node;
  node.id = id;
  node.name = "source" + std::to_string(id);
  node.type = "image_generator";
  node.subtype = "constant";
  node.parameters["width"] = 16;
  node.parameters["height"] = 16;
  return node;
}

Node make_image_child(int id, int parent_id) {
  Node node;
  node.id = id;
  node.name = "image_child" + std::to_string(id);
  node.type = "image_process";
  node.subtype = "identity";
  node.image_inputs.push_back(ImageInput{parent_id, "image"});
  return node;
}

Node make_param_child(int id, int parent_id) {
  Node node;
  node.id = id;
  node.name = "param_child" + std::to_string(id);
  node.type = "image_process";
  node.subtype = "identity";
  node.parameter_inputs.push_back(
      ParameterInput{parent_id, "threshold", "threshold"});
  return node;
}

std::filesystem::path temp_file(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
}

template <typename T, typename = void>
struct HasPublicMutableNode : std::false_type {};

template <typename T>
struct HasPublicMutableNode<
    T, std::void_t<decltype(std::declval<T&>().mutable_node(1))>>  // NOLINT
    : std::true_type {};  // NOLINT(whitespace/indent_namespace)

template <typename T, typename = void>
struct HasPublicFindNodeMutable : std::false_type {};

template <typename T>
struct HasPublicFindNodeMutable<
    T,
    std::void_t<decltype(std::declval<T&>().find_node_mutable(1))>>  // NOLINT
    : std::true_type {};  // NOLINT(whitespace/indent_namespace)

static_assert(!HasPublicMutableNode<GraphModel>::value,
              "GraphModel::mutable_node must not be public.");
static_assert(!HasPublicFindNodeMutable<GraphModel>::value,
              "GraphModel::find_node_mutable must not be public.");

}  // namespace

TEST(GraphTopologyBoundaries, MutableNodeHelpersAreNotPublicBypass) {
  EXPECT_FALSE((HasPublicMutableNode<GraphModel>::value));
  EXPECT_FALSE((HasPublicFindNodeMutable<GraphModel>::value));
}

TEST(GraphTopologyBoundaries, RecordsUpstreamAndDownstreamEdges) {
  GraphModel graph(temp_file("photospider-topology-cache"));
  graph.add_node(make_source(1));
  graph.add_node(make_image_child(2, 1));
  graph.add_node(make_param_child(3, 1));

  const auto& incoming_image = graph.upstream_edges(2);
  ASSERT_EQ(incoming_image.size(), 1u);
  EXPECT_EQ(incoming_image[0].from_node_id, 1);
  EXPECT_EQ(incoming_image[0].to_node_id, 2);
  EXPECT_EQ(incoming_image[0].kind, GraphTopologyEdgeKind::ImageInput);
  EXPECT_EQ(incoming_image[0].from_output_name, "image");

  const auto& incoming_param = graph.upstream_edges(3);
  ASSERT_EQ(incoming_param.size(), 1u);
  EXPECT_EQ(incoming_param[0].from_node_id, 1);
  EXPECT_EQ(incoming_param[0].to_node_id, 3);
  EXPECT_EQ(incoming_param[0].kind, GraphTopologyEdgeKind::ParameterInput);
  EXPECT_EQ(incoming_param[0].from_output_name, "threshold");
  EXPECT_EQ(incoming_param[0].to_input_name, "threshold");

  GraphTraversalService traversal;
  EXPECT_EQ(traversal.dependencies_of(graph, 2), std::vector<int>({1}));
  EXPECT_EQ(traversal.dependents_of(graph, 1), std::vector<int>({2, 3}));
  EXPECT_EQ(traversal.ending_nodes(graph), std::vector<int>({2, 3}));
  EXPECT_EQ(traversal.topo_postorder_from(graph, 2), std::vector<int>({1, 2}));
}

TEST(GraphTopologyBoundaries, StructuralMutationsRefreshOrPreserveTopology) {
  GraphModel graph(temp_file("photospider-topology-mutation-cache"));
  graph.add_node(make_source(1));
  graph.add_node(make_source(4));
  graph.add_node(make_image_child(2, 1));

  GraphTraversalService traversal;
  EXPECT_EQ(traversal.dependencies_of(graph, 2), std::vector<int>({1}));

  Node invalid_replacement = make_image_child(2, 99);
  EXPECT_THROW(graph.replace_node(invalid_replacement), GraphError);
  EXPECT_EQ(traversal.dependencies_of(graph, 2), std::vector<int>({1}));
  ASSERT_EQ(graph.upstream_edges(2).size(), 1u);
  EXPECT_EQ(graph.upstream_edges(2)[0].from_node_id, 1);

  graph.rewire_image_input(2, 0, 4, "image");
  EXPECT_EQ(traversal.dependencies_of(graph, 2), std::vector<int>({4}));
  EXPECT_TRUE(traversal.dependents_of(graph, 1).empty());
  EXPECT_EQ(traversal.dependents_of(graph, 4), std::vector<int>({2}));

  graph.clear();
  EXPECT_TRUE(graph.empty());
  EXPECT_TRUE(graph.topology().empty());
}

TEST(GraphTopologyBoundaries, RemoveNodeDisconnectsInputsAndRefreshesTopology) {
  GraphModel graph(temp_file("photospider-topology-remove-cache"));
  graph.add_node(make_source(1));
  graph.add_node(make_source(4));
  graph.add_node(make_image_child(2, 1));
  graph.add_node(make_param_child(3, 1));
  graph.add_node(make_image_child(5, 2));

  GraphTraversalService traversal;
  EXPECT_EQ(traversal.dependents_of(graph, 1), std::vector<int>({2, 3}));
  ASSERT_EQ(graph.downstream_edges(1).size(), 2u);

  graph.remove_node(1);

  EXPECT_FALSE(graph.has_node(1));
  ASSERT_TRUE(graph.has_node(2));
  ASSERT_TRUE(graph.has_node(3));
  EXPECT_EQ(graph.node(2).image_inputs[0].from_node_id, -1);
  EXPECT_EQ(graph.node(3).parameter_inputs[0].from_node_id, -1);
  EXPECT_TRUE(graph.upstream_edges(2).empty());
  EXPECT_TRUE(graph.upstream_edges(3).empty());
  EXPECT_TRUE(graph.downstream_edges(1).empty());
  EXPECT_EQ(traversal.dependencies_of(graph, 5), std::vector<int>({2}));
  EXPECT_EQ(traversal.dependents_of(graph, 2), std::vector<int>({5}));

  EXPECT_NO_THROW(graph.validate_topology());
}

TEST(GraphTopologyBoundaries, FailedLoadDoesNotExposePartialTopology) {
  const auto valid_path = temp_file("photospider-topology-valid.yaml");
  const auto invalid_path = temp_file("photospider-topology-invalid.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: source\n"
             "  type: image_generator\n"
             "  subtype: constant\n"
             "  parameters:\n"
             "    width: 16\n"
             "    height: 16\n"
             "- id: 2\n"
             "  name: child\n"
             "  type: image_process\n"
             "  subtype: identity\n"
             "  image_inputs:\n"
             "    - from_node_id: 1\n"
             "      from_output_name: image\n");
  write_text(invalid_path,
             "- id: 1\n"
             "  name: replacement\n"
             "  type: image_generator\n"
             "  subtype: constant\n"
             "- id: 2\n"
             "  name: broken\n"
             "  type: image_process\n"
             "  subtype: identity\n"
             "  image_inputs:\n"
             "    - from_node_id: 99\n"
             "      from_output_name: image\n");

  GraphModel graph(temp_file("photospider-topology-load-cache"));
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  io.load(graph, valid_path);

  GraphTraversalService traversal;
  EXPECT_EQ(traversal.dependencies_of(graph, 2), std::vector<int>({1}));

  EXPECT_THROW(io.load(graph, invalid_path), GraphError);
  ASSERT_TRUE(graph.has_node(1));
  ASSERT_TRUE(graph.has_node(2));
  EXPECT_EQ(graph.node(1).name, "source");
  EXPECT_EQ(traversal.dependencies_of(graph, 2), std::vector<int>({1}));

  std::filesystem::remove(valid_path);
  std::filesystem::remove(invalid_path);
}

}  // namespace ps
