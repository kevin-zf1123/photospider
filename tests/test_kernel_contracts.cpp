#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "adapter/buffer_adapter_opencv.hpp"
#include "graph_model.hpp"
#include "kernel/kernel.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_io_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "ps_types.hpp"

namespace ps {
namespace {

void register_contract_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = OpRegistry::instance();

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const int width = node.runtime_parameters["width"].as<int>(17);
          const int height = node.runtime_parameters["height"].as<int>(3);
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              width, height, 1, DataType::FLOAT32);
          cv::Mat mat = toCvMat(output.image_buffer);
          mat.setTo(1.0f);
          return output;
        }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "process",
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>& inputs) {
          if (inputs.empty()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "process requires an input");
          }
          NodeOutput output;
          const auto& input = inputs.front()->image_buffer;
          output.image_buffer = make_aligned_cpu_image_buffer(
              input.width, input.height, input.channels, input.type);
          cv::Mat src = toCvMat(input);
          cv::Mat dst = toCvMat(output.image_buffer);
          src.copyTo(dst);
          return output;
        }));

    registry.register_op_rt_tiled(
        "kernel_contract_test", "process",
        TileOpFunc([](const Node&, const Tile& output_tile,
                      const std::vector<Tile>& input_tiles) {
          if (input_tiles.empty()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "process requires input tiles");
          }
          cv::Mat src = toCvMat(input_tiles.front());
          cv::Mat dst = toCvMat(output_tile);
          src.copyTo(dst);
        }),
        OpMetadata{});
  });
}

Node make_contract_node() {
  Node node;
  node.id = 1;
  node.name = "contract_source";
  node.type = "kernel_contract_test";
  node.subtype = "source";
  node.parameters = YAML::Node(YAML::NodeType::Map);
  node.parameters["width"] = 17;
  node.parameters["height"] = 3;
  return node;
}

Node make_contract_process_node() {
  Node node;
  node.id = 2;
  node.name = "contract_process";
  node.type = "kernel_contract_test";
  node.subtype = "process";
  node.image_inputs.push_back(ImageInput{1, "image"});
  return node;
}

std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

}  // namespace

TEST(ImageBufferContract, AlignedCpuRowsAndPaddedStep) {
  ImageBuffer buffer =
      make_aligned_cpu_image_buffer(17, 5, 3, DataType::FLOAT32);

  const auto base = reinterpret_cast<std::uintptr_t>(buffer.data.get());
  EXPECT_EQ(base % 64, 0u);
  EXPECT_EQ(buffer.step % 64, 0u);
  EXPECT_GT(buffer.step, 17u * 3u * sizeof(float));

  for (int y = 0; y < buffer.height; ++y) {
    const auto row = base + static_cast<std::uintptr_t>(y) * buffer.step;
    EXPECT_EQ(row % 64, 0u);
  }
}

TEST(ImageBufferContract, OpenCvAndTileAccessRespectPaddedStep) {
  ImageBuffer buffer =
      make_aligned_cpu_image_buffer(17, 4, 1, DataType::FLOAT32);
  cv::Mat mat = toCvMat(buffer);
  ASSERT_EQ(mat.step, buffer.step);
  ASSERT_FALSE(mat.isContinuous());
  mat.setTo(0.0f);

  Node node;
  Tile output_tile{&buffer, cv::Rect(3, 1, 5, 2)};
  TileOpFunc write_tile = [](const Node&, const Tile& tile,
                             const std::vector<Tile>&) {
    cv::Mat tile_mat = toCvMat(tile);
    tile_mat.setTo(7.0f);
  };
  write_tile(node, output_tile, {});

  EXPECT_FLOAT_EQ(mat.at<float>(1, 3), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(2, 7), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(0, 3), 0.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(1, 8), 0.0f);
}

TEST(CacheSemantics, HpAndRtComputePopulateFormalCaches) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache;
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  GraphModel graph(temp_path("photospider-contract-cache"));
  graph.add_node(make_contract_node());
  graph.add_node(make_contract_process_node());
  graph.validate_topology();

  // HP compute populates cached_output_high_precision only
  NodeOutput& hp = compute.compute(graph, ComputeIntent::GlobalHighPrecision, 2,
                                   "int8", false, false, true, nullptr,
                                   std::nullopt);
  EXPECT_TRUE(graph.nodes.at(2).cached_output_high_precision.has_value());
  EXPECT_EQ(hp.image_buffer.width,
            graph.nodes.at(2).cached_output_high_precision->image_buffer.width);

  // Snapshot HP cache dimensions before RT compute
  int hp_w_before =
      graph.nodes.at(2).cached_output_high_precision->image_buffer.width;
  int hp_h_before =
      graph.nodes.at(2).cached_output_high_precision->image_buffer.height;

  // RT compute populates cached_output_real_time only;
  // cached_output_high_precision must remain unchanged.
  NodeOutput& rt = compute.compute(graph, ComputeIntent::RealTimeUpdate, 2,
                                   "int8", false, false, true, nullptr,
                                   cv::Rect(0, 0, 8, 8));
  EXPECT_EQ(&rt, &*graph.nodes.at(2).cached_output_real_time);
  EXPECT_TRUE(graph.nodes.at(2).cached_output_real_time.has_value());

  // Key contract: RT compute must NOT alter the formal HP cache
  ASSERT_TRUE(graph.nodes.at(2).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.nodes.at(2).cached_output_high_precision->image_buffer.width,
            hp_w_before)
      << "RT compute must not change HP cache width";
  EXPECT_EQ(graph.nodes.at(2).cached_output_high_precision->image_buffer.height,
            hp_h_before)
      << "RT compute must not change HP cache height";

  // RT output should be downscaled relative to HP
  EXPECT_LE(graph.nodes.at(2).cached_output_real_time->image_buffer.width,
            hp_w_before)
      << "RT output should be <= HP output width";
}

TEST(CacheSemantics, DiskSaveAndSyncIgnoreRtOnlyState) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache;
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  auto root = temp_path("photospider-contract-disk-cache");
  std::filesystem::remove_all(root);
  GraphModel graph(root);

  // Node 1: source (no caches entry → won't be persisted to disk)
  graph.add_node(make_contract_node());

  // Node 2: process with HP cache + caches entry → should be saved to disk
  graph.add_node(make_contract_process_node());
  graph.nodes.at(2).caches.push_back({"image", "output.png"});

  // Node 3: process with caches entry but only RT state (simulates a node
  // that was computed via interactive RT path but never had HP computed)
  Node rt_only_node;
  rt_only_node.id = 3;
  rt_only_node.name = "rt_only";
  rt_only_node.type = "kernel_contract_test";
  rt_only_node.subtype = "process";
  rt_only_node.image_inputs.push_back(ImageInput{1, "image"});
  rt_only_node.caches.push_back({"image", "rt_output.png"});
  graph.add_node(rt_only_node);

  graph.validate_topology();

  // HP compute for node 2 — also computes node 1 as dependency
  compute.compute(graph, ComputeIntent::GlobalHighPrecision, 2, "int8", false,
                  false, true, nullptr, std::nullopt);
  ASSERT_TRUE(graph.nodes.at(2).cached_output_high_precision.has_value());

  // Populate RT-only node (node 3) with RT state; it never had HP compute.
  graph.nodes.at(3).cached_output_real_time = NodeOutput{};
  graph.nodes.at(3).cached_output_real_time->image_buffer =
      make_aligned_cpu_image_buffer(8, 8, 1, DataType::FLOAT32);

  // Also give node 2 an RT snapshot — RT presence must not affect sync
  graph.nodes.at(2).cached_output_real_time = NodeOutput{};
  graph.nodes.at(2).cached_output_real_time->image_buffer =
      make_aligned_cpu_image_buffer(4, 4, 1, DataType::FLOAT32);

  // Create stale disk file for RT-only node 3 (simulating leftover from a
  // previous HP run that no longer has valid HP cache).
  auto dir3 = cache.node_cache_dir(graph, 3);
  std::filesystem::create_directories(dir3);
  auto stale_file = dir3 / "rt_output.png";
  {
    std::ofstream out(stale_file, std::ios::binary);
    out << "stale";
  }
  ASSERT_TRUE(std::filesystem::exists(stale_file));

  // --- Perform sync ---
  auto sync_result = cache.synchronize_disk_cache(graph, "int8");

  // Contract 1: Node 2 has HP cache → should be saved to disk
  EXPECT_GE(sync_result.saved_nodes, 1)
      << "Nodes with HP cache should be saved to disk";

  // Contract 2: RT-only node (node 3) has NO HP cache → stale disk files
  // MUST be cleaned up. RT presence alone must not protect stale files.
  EXPECT_FALSE(std::filesystem::exists(stale_file))
      << "Stale disk files for RT-only nodes should be removed";
  EXPECT_GE(sync_result.removed_files, 1)
      << "Sync should report removed stale files for nodes without HP cache";

  // Contract 3: Node 2 has HP cache + RT state, so only HP is written to disk.
  // The disk file for node 2 should exist after sync.
  auto dir2 = cache.node_cache_dir(graph, 2);
  auto hp_file = dir2 / "output.png";
  EXPECT_TRUE(std::filesystem::exists(hp_file))
      << "HP cache file should exist on disk after sync";

  // Clean up
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, RealTimeUpdateWithoutDirtyRoiFailsClearly) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache;
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  GraphModel graph(temp_path("photospider-contract-rt-error"));
  graph.add_node(make_contract_node());

  EXPECT_THROW(
      compute.compute(graph, ComputeIntent::RealTimeUpdate, 1, "int8", false,
                      false, true, nullptr, std::nullopt),
      GraphError);
}

TEST(GraphModelContract, ClearResetsModelRuntimeState) {
  GraphModel graph(temp_path("photospider-contract-clear"));
  graph.add_node(make_contract_node());
  graph.timing_results.node_timings.push_back({1, "node", 1.0, "computed"});
  graph.timing_results.total_ms = 10.0;
  graph.total_io_time_ms.store(4.0);
  graph.set_skip_save_cache(true);
  graph.set_quiet(false);

  graph.clear();

  EXPECT_TRUE(graph.nodes.empty());
  EXPECT_TRUE(graph.timing_results.node_timings.empty());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_DOUBLE_EQ(graph.total_io_time_ms.load(), 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
  EXPECT_TRUE(graph.is_quiet());
}

TEST(GraphIoContract, FailedReloadPreservesPreviousGraph) {
  const auto valid_path = temp_path("photospider-contract-valid.yaml");
  const auto invalid_path = temp_path("photospider-contract-invalid.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: valid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(invalid_path,
             "- id: 1\n"
             "  name: invalid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n"
             "  image_inputs:\n"
             "    - from_node_id: 99\n");

  GraphModel graph(temp_path("photospider-contract-reload-cache"));
  GraphIOService io;
  io.load(graph, valid_path);
  ASSERT_EQ(graph.nodes.at(1).name, "valid");

  EXPECT_THROW(io.load(graph, invalid_path), GraphError);
  ASSERT_TRUE(graph.has_node(1));
  EXPECT_EQ(graph.nodes.at(1).name, "valid");
}

TEST(GraphMutationContract, InvalidNodeReplacementPreservesPreviousNode) {
  const auto root = temp_path("photospider-contract-kernel-root");
  const auto yaml_path = temp_path("photospider-contract-kernel.yaml");
  std::filesystem::remove_all(root);
  write_text(yaml_path,
             "- id: 1\n"
             "  name: valid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");

  Kernel kernel;
  auto loaded = kernel.load_graph("contract_graph", root.string(),
                                  yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  const std::string invalid_replacement =
      "id: 1\n"
      "name: invalid\n"
      "type: kernel_contract_test\n"
      "subtype: source\n"
      "image_inputs:\n"
      "  - from_node_id: 99\n";
  EXPECT_FALSE(kernel.set_node_yaml("contract_graph", 1, invalid_replacement));

  auto node_yaml = kernel.get_node_yaml("contract_graph", 1);
  ASSERT_TRUE(node_yaml.has_value());
  EXPECT_NE(node_yaml->find("valid"), std::string::npos);
  kernel.close_graph("contract_graph");
}

}  // namespace ps
