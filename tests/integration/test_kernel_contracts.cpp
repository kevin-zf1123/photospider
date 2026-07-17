#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "compute/compute_service.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_cache_service.hpp"
#include "graph/graph_io_service.hpp"
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
#include "graph/graph_io_service_test_access.hpp"
#endif
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "support/kernel_test_access.hpp"

namespace ps {
namespace {

/** @brief Serializes the blocking contract operation's release future. */
std::mutex g_blocking_source_mutex;

/** @brief Test-controlled release copied by the blocking contract operation. */
std::shared_future<void> g_blocking_source_release;

/** @brief Publishes entry into the blocking contract operation callback. */
std::atomic<bool> g_blocking_source_started{false};

/**
 * @brief Configures the blocking contract op release signal for one test.
 *
 * @param release Future that the blocking source op waits on after signalling
 * that execution has started.
 * @throws std::bad_alloc if shared_future state copying allocates.
 * @note The op reads this state under g_blocking_source_mutex so tests can
 * safely install a fresh release future before submitting compute work.
 */
void configure_blocking_contract_source(std::shared_future<void> release) {
  std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
  g_blocking_source_started.store(false, std::memory_order_release);
  g_blocking_source_release = std::move(release);
}

/**
 * @brief Clears the blocking contract op release signal after a test.
 *
 * @throws Nothing directly.
 * @note Leaving the future unset would make later blocking-source computes
 * wait on stale test state.
 */
void reset_blocking_contract_source() {
  std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
  g_blocking_source_release = std::shared_future<void>();
  g_blocking_source_started.store(false, std::memory_order_release);
}

/**
 * @brief Waits until the blocking contract op reports that it is running.
 *
 * @param timeout Maximum time to wait for the op to start.
 * @return true when the op started before timeout, otherwise false.
 * @throws Nothing directly.
 * @note Tests use this to know the graph-state compute closure has entered
 * scheduler-backed work and is holding the graph-state executor boundary.
 */
bool wait_for_blocking_contract_source(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_blocking_source_started.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_blocking_source_started.load(std::memory_order_acquire);
}

/**
 * @brief Registers deterministic operations used by Kernel contract tests.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry keys, callbacks, or metadata cannot be
 * allocated during the one-time registration.
 * @note Registration is process-wide and idempotent through std::call_once.
 * The blocking operation borrows only the separately synchronized test future.
 */
void register_contract_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = OpRegistry::instance();

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
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
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
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

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "blocking_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              std::shared_future<void> release;
              {
                std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
                release = g_blocking_source_release;
              }
              g_blocking_source_started.store(true, std::memory_order_release);
              if (release.valid()) {
                release.wait();
              }

              const int width = node.runtime_parameters["width"].as<int>(17);
              const int height = node.runtime_parameters["height"].as<int>(3);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(3.0f);
              return output;
            }));

    registry.register_op_rt_tiled(
        "kernel_contract_test", "process",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
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

/**
 * @brief Removes and returns a deterministic temporary cache root.
 *
 * @param name Directory name appended to the system temporary directory.
 * @return Clean root path ready for GraphModel construction.
 * @throws std::filesystem::filesystem_error if cleanup fails.
 * @note Tests use deterministic names so failed runs leave inspectable paths.
 */
std::filesystem::path clean_temp_path(const std::string& name) {
  auto root = temp_path(name);
  std::filesystem::remove_all(root);
  return root;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

/**
 * @brief Writes a single-node graph whose operation intentionally is missing.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The graph is valid topology, so compute reaches operation resolution
 * and fails inside the compute request boundary.
 */
void write_missing_op_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: missing_op
  type: kernel_contract_test
  subtype: missing_op
  parameters:
    width: 8
    height: 8
)YAML");
}

/**
 * @brief Writes a graph that runs the blocking contract source operation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The source has explicit dimensions so planned parallel dispatch emits
 * a deterministic single monolithic scheduler task.
 */
void write_blocking_source_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: blocking_source
  type: kernel_contract_test
  subtype: blocking_source
  parameters:
    width: 8
    height: 8
)YAML");
}

/**
 * @brief Builds a process node with one image disk-cache entry.
 *
 * @param location CacheEntry location to attach to the process node.
 * @return Node configured for disk-cache service tests.
 * @throws std::bad_alloc if node strings or vectors cannot allocate.
 * @note The node does not need to be added to GraphModel for cache load tests
 * because GraphCacheService resolves disk paths from node id and cache entry.
 */
Node make_cached_process_node(const std::string& location) {
  Node node = make_contract_process_node();
  node.caches.push_back({"image", location});
  return node;
}

/**
 * @brief Owns common disk-cache diagnostic test state.
 *
 * The context prepares a clean cache root, constructs a GraphModel, creates one
 * cached process node, and removes the root in the destructor with
 * `std::error_code` so cleanup never masks assertion failures.
 *
 * @note The helper keeps the four disk-cache diagnostic tests focused on their
 * distinct miss/hit/error assertions instead of repeating filesystem setup.
 */
struct DiskCacheDiagnosticContext {
  GraphCacheService cache;
  std::filesystem::path root;
  GraphModel graph;
  Node node;

  /**
   * @brief Creates a clean graph cache root and one cached process node.
   *
   * @param root_name Temporary directory name for this test case.
   * @param cache_location CacheEntry location to configure on the node.
   * @throws std::filesystem::filesystem_error if root cleanup or creation
   * fails.
   */
  DiskCacheDiagnosticContext(const std::string& root_name,
                             const std::string& cache_location)
      : root(clean_temp_path(root_name)),
        graph(root),
        node(make_cached_process_node(cache_location)) {}

  /**
   * @brief Removes the temporary cache root without throwing.
   *
   * @note Cleanup errors are intentionally ignored at teardown because the test
   * assertions already captured the behavior under validation.
   */
  ~DiskCacheDiagnosticContext() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  /**
   * @brief Returns the image cache path for the configured node.
   *
   * @return Resolved path for the node's first cache entry.
   * @throws std::bad_alloc if path construction cannot allocate.
   */
  std::filesystem::path cache_file() const {
    return cache.node_cache_dir(graph, node.id) / node.caches.front().location;
  }

  /**
   * @brief Returns the YAML metadata path for the configured node.
   *
   * @return Resolved `.yml` path paired with `cache_file()`.
   * @throws std::bad_alloc if path construction cannot allocate.
   */
  std::filesystem::path metadata_file() const {
    auto path = cache_file();
    path.replace_extension(".yml");
    return path;
  }
};

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
  OutputTile output_tile{&buffer, PixelRect{3, 1, 5, 2}};
  TileOpFunc write_tile = [](const Node&, const OutputTile& tile,
                             const std::vector<InputTile>&) {
    cv::Mat tile_mat = toCvMat(tile);
    tile_mat.setTo(7.0f);
  };
  write_tile(node, output_tile, {});

  EXPECT_FLOAT_EQ(mat.at<float>(1, 3), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(2, 7), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(0, 3), 0.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(1, 8), 0.0f);
}

TEST(InteractionInspectionContracts,
     ReturnsStructuredDependencyTreeAndInspection) {
  auto root = temp_path("photospider-interaction-inspect-contract");
  std::filesystem::remove_all(root);
  const auto yaml_path = root / "graph.yaml";
  write_text(yaml_path, R"YAML(
- id: 1
  name: source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 17
    height: 3
- id: 2
  name: process
  type: kernel_contract_test
  subtype: process
  image_inputs:
    - from_node_id: 1
      from_output_name: image
)YAML");

  Kernel kernel;
  InteractionService svc(kernel);
  auto loaded =
      svc.cmd_load_graph("inspect_contract", root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  auto tree = svc.cmd_dependency_tree(*loaded, 2, true);
  ASSERT_TRUE(tree.has_value());
  EXPECT_EQ(tree->scope, DependencyTree::Scope::StartNode);
  ASSERT_TRUE(tree->start_node_id.has_value());
  EXPECT_EQ(*tree->start_node_id, 2);
  EXPECT_TRUE(tree->start_node_found);
  ASSERT_EQ(tree->root_node_ids, std::vector<int>({2}));
  ASSERT_EQ(tree->entries.size(), 2u);

  EXPECT_EQ(tree->entries[0].depth, 0);
  EXPECT_FALSE(tree->entries[0].incoming_edge.has_value());
  EXPECT_EQ(tree->entries[0].node.id, 2);
  EXPECT_EQ(tree->entries[0].node.name, "process");
  ASSERT_TRUE(tree->entries[0].node.metadata.has_value());
  EXPECT_FALSE(tree->entries[0].node.metadata->has_cached_output);

  EXPECT_EQ(tree->entries[1].depth, 2);
  ASSERT_TRUE(tree->entries[1].incoming_edge.has_value());
  EXPECT_EQ(tree->entries[1].incoming_edge->kind,
            GraphTopologyEdgeKind::ImageInput);
  EXPECT_EQ(tree->entries[1].incoming_edge->from_node_id, 1);
  EXPECT_EQ(tree->entries[1].incoming_edge->to_node_id, 2);
  EXPECT_EQ(tree->entries[1].incoming_edge->from_output_name, "image");
  EXPECT_EQ(tree->entries[1].node.id, 1);
  EXPECT_EQ(tree->entries[1].node.parameters["width"].as<int>(), 17);

  auto graph = svc.cmd_inspect_graph(*loaded);
  ASSERT_TRUE(graph.has_value());
  ASSERT_EQ(graph->nodes.size(), 2u);
  EXPECT_EQ(graph->nodes[0].id, 1);
  EXPECT_EQ(graph->nodes[1].id, 2);
  ASSERT_TRUE(graph->nodes[0].metadata.has_value());
  EXPECT_FALSE(graph->nodes[0].metadata->has_cached_output);

  std::filesystem::remove_all(root);
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
  ComputeService::Request hp_request;
  hp_request.node_id = 2;
  hp_request.cache.precision = "int8";
  hp_request.cache.disable_disk_cache = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  NodeOutput& hp = compute.compute(graph, hp_request);
  EXPECT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(hp.image_buffer.width,
            graph.node(2).cached_output_high_precision->image_buffer.width);

  // Snapshot HP cache dimensions before RT compute
  int hp_w_before =
      graph.node(2).cached_output_high_precision->image_buffer.width;
  int hp_h_before =
      graph.node(2).cached_output_high_precision->image_buffer.height;

  // RT compute returns proxy output; cached_output_high_precision must remain
  // unchanged.
  ComputeService::Request rt_request = hp_request;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = PixelRect{0, 0, 8, 8};
  NodeOutput& rt = compute.compute(graph, rt_request);

  // Key contract: RT compute must NOT alter the formal HP cache
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(2).cached_output_high_precision->image_buffer.width,
            hp_w_before)
      << "RT compute must not change HP cache width";
  EXPECT_EQ(graph.node(2).cached_output_high_precision->image_buffer.height,
            hp_h_before)
      << "RT compute must not change HP cache height";

  // RT output should be downscaled relative to HP
  EXPECT_LE(rt.image_buffer.width, hp_w_before)
      << "RT output should be <= HP output width";
}

TEST(CacheSemantics, DiskSaveAndSyncIgnoreNodesWithoutHpState) {
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
  graph.mutate_node_runtime_state(
      2, [](auto& state) { state.caches.push_back({"image", "output.png"}); });

  // Node 3: process with caches entry but no HP state. RT proxy state is not
  // stored on GraphModel and therefore cannot protect disk cache files.
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
  ComputeService::Request hp_request;
  hp_request.node_id = 2;
  hp_request.cache.precision = "int8";
  hp_request.cache.disable_disk_cache = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  compute.compute(graph, hp_request);
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());

  // Create stale disk file for node 3 (simulating leftover from a previous HP
  // run that no longer has valid HP cache).
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

  // Contract 2: node 3 has NO HP cache, so stale disk files must be cleaned
  // up. RT proxy state is outside GraphModel and cannot protect stale files.
  EXPECT_FALSE(std::filesystem::exists(stale_file))
      << "Stale disk files for nodes without HP cache should be removed";
  EXPECT_GE(sync_result.removed_files, 1)
      << "Sync should report removed stale files for nodes without HP cache";

  // Contract 3: Node 2 has HP cache, so HP is written to disk.
  // The disk file for node 2 should exist after sync.
  auto dir2 = cache.node_cache_dir(graph, 2);
  auto hp_file = dir2 / "output.png";
  EXPECT_TRUE(std::filesystem::exists(hp_file))
      << "HP cache file should exist on disk after sync";

  // Clean up
  std::filesystem::remove_all(root);
}

TEST(CacheSemantics, DiskCacheMissRecordsDiagnostic) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-miss",
                                 "missing.png");

  NodeOutput out;
  EXPECT_FALSE(
      ctx.cache.try_load_from_disk_cache_into(ctx.graph, ctx.node, out));

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Miss);
  EXPECT_EQ(result->code, GraphErrc::Unknown);
  EXPECT_EQ(result->node_id, ctx.node.id);
  EXPECT_EQ(result->location, "missing.png");
  EXPECT_NE(result->message.find("No disk cache files"), std::string::npos);
}

TEST(CacheSemantics, DiskCacheMetadataHitPreservesTryLoadBehavior) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-hit",
                                 "output.png");
  auto metadata_file = ctx.metadata_file();
  write_text(metadata_file, "answer: 42\nlabel: cached\n");

  NodeOutput out;
  EXPECT_TRUE(
      ctx.cache.try_load_from_disk_cache_into(ctx.graph, ctx.node, out));
  ASSERT_NE(out.data.find("answer"), out.data.end());
  ASSERT_NE(out.data.find("label"), out.data.end());
  EXPECT_EQ(out.data["answer"].as<int>(), 42);
  EXPECT_EQ(out.data["label"].as<std::string>(), "cached");

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Hit);
  EXPECT_EQ(result->code, GraphErrc::Unknown);
  EXPECT_EQ(result->metadata_file, metadata_file);
}

TEST(CacheSemantics, DiskCacheInvalidMetadataRecordsErrorDiagnostic) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-bad-yaml",
                                 "output.png");
  auto metadata_file = ctx.metadata_file();
  write_text(metadata_file, "answer: [1, 2\n");

  NodeOutput out;
  EXPECT_FALSE(
      ctx.cache.try_load_from_disk_cache_into(ctx.graph, ctx.node, out));

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::InvalidYaml);
  EXPECT_EQ(result->metadata_file, metadata_file);
  EXPECT_NE(result->message.find("Failed to parse disk cache metadata"),
            std::string::npos);
}

TEST(CacheSemantics, DiskCacheCorruptImageRecordsErrorWithoutHpMutation) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-bad-image",
                                 "output.png");
  auto image_file = ctx.cache_file();
  write_text(image_file, "not an image");

  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache(ctx.graph, ctx.node));
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::Io);
  EXPECT_EQ(result->cache_file, image_file);
  EXPECT_NE(result->message.find("Failed to decode disk cache image"),
            std::string::npos);
}

TEST(CacheSemantics, DiskCacheDiagnosticSnapshotSupportsConcurrentWriters) {
  GraphModel graph(
      temp_path("photospider-contract-disk-cache-diagnostic-lock"));
  std::promise<void> release;
  auto ready = release.get_future().share();
  std::vector<std::future<void>> writers;
  constexpr int kWriterCount = 32;

  for (int i = 0; i < kWriterCount; ++i) {
    writers.push_back(std::async(std::launch::async, [&, i]() {
      ready.wait();
      GraphModel::DiskCacheLoadResult result;
      result.node_id = i;
      result.location = "entry-" + std::to_string(i) + ".png";
      result.status = GraphModel::DiskCacheLoadStatus::Miss;
      result.message = "concurrent diagnostic " + std::to_string(i);
      graph.record_disk_cache_load_result(std::move(result));

      const auto snapshot = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(snapshot.has_value());
      EXPECT_FALSE(snapshot->message.empty());
    }));
  }

  release.set_value();
  for (auto& writer : writers) {
    writer.get();
  }

  const auto final_snapshot = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(final_snapshot.has_value());
  EXPECT_GE(final_snapshot->node_id, 0);
  EXPECT_LT(final_snapshot->node_id, kWriterCount);
  EXPECT_EQ(final_snapshot->status, GraphModel::DiskCacheLoadStatus::Miss);
}

TEST(ComputeContracts, RealTimeUpdateWithoutDirtyRoiFailsClearly) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache;
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  GraphModel graph(temp_path("photospider-contract-rt-error"));
  graph.add_node(make_contract_node());

  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.intent = ComputeIntent::RealTimeUpdate;
  EXPECT_THROW(compute.compute(graph, request), GraphError);
}

TEST(KernelExceptionContracts, BadAllocEscapesRuntimeAndGraphStateWrappers) {
  const std::string graph_name = "contract_bad_alloc_wrappers";
  const auto root = clean_temp_path("photospider-contract-bad-alloc-root");
  const auto yaml_path = temp_path("photospider-contract-bad-alloc.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel;
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  EXPECT_THROW(
      (void)testing::KernelTestAccess::inject_bad_alloc_through_runtime(
          kernel, graph_name),
      std::bad_alloc);
  EXPECT_THROW(
      (void)testing::KernelTestAccess::inject_bad_alloc_through_graph_state(
          kernel, graph_name),
      std::bad_alloc);
  EXPECT_THROW(
      (void)testing::KernelTestAccess::
          inject_bad_alloc_through_last_error_graph_state(kernel, graph_name),
      std::bad_alloc);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, SyncFailureRestoresRequestScopedGraphState) {
  register_contract_ops();
  const std::string graph_name = "contract_sync_state_restore";
  const auto root = clean_temp_path("photospider-contract-sync-state-root");
  const auto yaml_path = temp_path("photospider-contract-sync-state.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.set_quiet(false);
        graph.set_skip_save_cache(true);
        return 0;
      })
      .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.cache.nosave = false;
  request.execution.quiet = true;

  EXPECT_FALSE(kernel.compute(request));
  auto restored =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::make_pair(graph.is_quiet(), graph.skip_save_cache());
          })
          .get();
  EXPECT_FALSE(restored.first);
  EXPECT_TRUE(restored.second);
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, AsyncParallelFailureRestoresRequestScopedGraphState) {
  register_contract_ops();
  const std::string graph_name = "contract_async_state_restore";
  const auto root = clean_temp_path("photospider-contract-async-state-root");
  const auto yaml_path = temp_path("photospider-contract-async-state.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.set_quiet(false);
        graph.set_skip_save_cache(true);
        return 0;
      })
      .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.cache.nosave = false;
  request.execution.parallel = true;
  request.execution.quiet = true;

  auto future = kernel.compute_async(request);
  ASSERT_TRUE(future.has_value());
  const Kernel::AsyncComputeResult outcome = future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_FALSE(outcome.error->message.empty());
  auto restored =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::make_pair(graph.is_quiet(), graph.skip_save_cache());
          })
          .get();
  EXPECT_FALSE(restored.first);
  EXPECT_TRUE(restored.second);
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves queued asynchronous failures retain work-item-owned errors.
 *
 * @throws Nothing when each future carries the exact request failure after both
 * work items have completed and the shared LastError has changed.
 * @note The two requests target one session and are submitted before either
 * result is consumed. One reaches operation lookup and the other fails node
 * lookup, making their GraphErrc values observably distinct.
 */
TEST(ComputeContracts, OverlappingAsyncFailuresOwnExactKernelResults) {
  register_contract_ops();
  const std::string graph_name = "contract_async_exact_errors";
  const auto root = clean_temp_path("photospider-contract-async-exact-root");
  const auto yaml_path = temp_path("photospider-contract-async-exact.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest missing_op_request;
  missing_op_request.name = graph_name;
  missing_op_request.node_id = 1;
  missing_op_request.cache.precision = "int8";
  missing_op_request.cache.force_recache = true;
  missing_op_request.cache.disable_disk_cache = true;
  missing_op_request.execution.parallel = true;
  Kernel::ComputeRequest missing_node_request = missing_op_request;
  missing_node_request.node_id = 99;

  auto missing_op_future = kernel.compute_async(missing_op_request);
  auto missing_node_future = kernel.compute_async(missing_node_request);
  ASSERT_TRUE(missing_op_future.has_value());
  ASSERT_TRUE(missing_node_future.has_value());
  ASSERT_EQ(missing_op_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(missing_node_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  const Kernel::AsyncComputeResult missing_op = missing_op_future->get();
  const Kernel::AsyncComputeResult missing_node = missing_node_future->get();
  EXPECT_FALSE(missing_op.ok);
  ASSERT_TRUE(missing_op.error.has_value());
  EXPECT_EQ(missing_op.error->code, GraphErrc::ComputeError);
  EXPECT_FALSE(missing_op.error->message.empty());
  EXPECT_FALSE(missing_node.ok);
  ASSERT_TRUE(missing_node.error.has_value());
  EXPECT_EQ(missing_node.error->code, GraphErrc::NotFound);
  EXPECT_FALSE(missing_node.error->message.empty());
  EXPECT_NE(missing_op.error->message, missing_node.error->message);

  const auto shared_error = kernel.last_error(graph_name);
  ASSERT_TRUE(shared_error.has_value());
  EXPECT_TRUE(shared_error->code == GraphErrc::NoOperation ||
              shared_error->code == GraphErrc::NotFound);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, ParallelComputeSerializesGraphStateOperations) {
  register_contract_ops();
  const std::string graph_name = "contract_parallel_graph_state";
  const auto root = clean_temp_path("photospider-contract-parallel-state-root");
  const auto yaml_path = temp_path("photospider-contract-parallel-state.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;

  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  EXPECT_TRUE(
      wait_for_blocking_contract_source(std::chrono::milliseconds(2000)));

  std::atomic<bool> post_ran{false};
  auto post_future = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [&post_ran](GraphModel& graph) {
        post_ran.store(true, std::memory_order_release);
        return graph.node_count();
      });

  EXPECT_EQ(post_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_FALSE(post_ran.load(std::memory_order_acquire));

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_EQ(post_future.wait_for(std::chrono::milliseconds(2000)),
            std::future_status::ready);
  EXPECT_EQ(post_future.get(), 1u);
  EXPECT_TRUE(post_ran.load(std::memory_order_acquire));

  reset_blocking_contract_source();
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies scheduler info and replacement wait for active compute.
 *
 * @throws Nothing when both scheduler calls remain pending until the blocking
 * compute releases the graph-state serialization boundary.
 * @note SerialDebugScheduler executes the blocking operation on its caller.
 * Without graph-state serialization, replacement could destroy that scheduler
 * while one of its member calls is still on the stack.
 */
TEST(ComputeContracts, SchedulerObservationAndReplacementWaitForCompute) {
  register_contract_ops();
  const std::string graph_name = "contract_scheduler_lifetime";
  const auto root = clean_temp_path("photospider-contract-scheduler-life-root");
  const auto yaml_path = temp_path("photospider-contract-scheduler-life.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(kernel.replace_scheduler(
      graph_name, ComputeIntent::GlobalHighPrecision, "serial_debug"));

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "blocking scheduler compute did not start";
  }

  std::promise<void> info_entered;
  auto info_entered_future = info_entered.get_future();
  auto info_future = std::async(std::launch::async, [&] {
    info_entered.set_value();
    return kernel.get_scheduler_info(graph_name,
                                     ComputeIntent::GlobalHighPrecision);
  });
  std::promise<void> replace_entered;
  auto replace_entered_future = replace_entered.get_future();
  auto replace_future = std::async(std::launch::async, [&] {
    replace_entered.set_value();
    return kernel.replace_scheduler(
        graph_name, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing");
  });

  EXPECT_EQ(info_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(replace_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(info_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_EQ(replace_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult compute_outcome = compute_future->get();
  EXPECT_TRUE(compute_outcome.ok);
  EXPECT_FALSE(compute_outcome.error.has_value());

  const auto observed_info = info_future.get();
  ASSERT_TRUE(observed_info.has_value());
  EXPECT_TRUE(observed_info->first == "serial_debug" ||
              observed_info->first == "CpuWorkStealingScheduler");
  EXPECT_FALSE(observed_info->second.empty());
  EXPECT_TRUE(replace_future.get());

  const auto final_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(final_info.has_value());
  EXPECT_EQ(final_info->first, "CpuWorkStealingScheduler");

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies graph close waits behind accepted asynchronous compute.
 *
 * @throws Nothing when close remains pending until the graph-state work item
 * completes, then removes the runtime without invalidating its owned outcome.
 * @note The blocking operation creates a deterministic close/compute race and
 * avoids relying on a fixed operation sleep duration.
 */
TEST(ComputeContracts, CloseWaitsForAcceptedAsyncGraphStateWork) {
  register_contract_ops();
  const std::string graph_name = "contract_close_async_lifetime";
  const auto root = clean_temp_path("photospider-contract-close-life-root");
  const auto yaml_path = temp_path("photospider-contract-close-life.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "blocking compute did not start before close";
  }

  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return kernel.close_graph(graph_name);
  });
  EXPECT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_TRUE(close_future.get());
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());

  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies a stopped runtime restarts only inside graph-state execution.
 *
 * @throws Nothing when an accepted compute stays queued with the runtime
 * stopped until the preceding graph-state task releases, then starts and
 * completes normally.
 * @note This directly guards the scheduler start/info/replace/close lifetime
 * rule: compute submission itself must not call GraphRuntime::start() outside
 * the serialization boundary.
 */
TEST(ComputeContracts, RuntimeRestartWaitsForGraphStateSerialization) {
  register_contract_ops();
  const std::string graph_name = "contract_serialized_runtime_restart";
  const auto root =
      clean_temp_path("photospider-contract-serialized-restart-root");
  const auto yaml_path =
      temp_path("photospider-contract-serialized-restart.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel;
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  GraphRuntime& runtime =
      testing::KernelTestAccess::runtime(kernel, graph_name);
  runtime.stop();
  ASSERT_FALSE(runtime.running());

  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::promise<void> blocker_entered;
  auto blocker_entered_future = blocker_entered.get_future();
  auto blocker = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [&blocker_entered, blocker_release](GraphModel&) {
        blocker_entered.set_value();
        blocker_release.wait();
        return 0;
      });
  if (blocker_entered_future.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    release_blocker.set_value();
    (void)blocker.get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "graph-state blocker did not start";
  }

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  if (!compute_future) {
    release_blocker.set_value();
    (void)blocker.get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "serialized restart compute was not accepted";
  }
  EXPECT_FALSE(runtime.running());
  EXPECT_EQ(compute_future->wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_blocker.set_value();
  EXPECT_EQ(blocker.get(), 0);
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_TRUE(runtime.running());

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
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

  EXPECT_TRUE(graph.empty());
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
  ASSERT_EQ(graph.node(1).name, "valid");

  EXPECT_THROW(io.load(graph, invalid_path), GraphError);
  ASSERT_TRUE(graph.has_node(1));
  EXPECT_EQ(graph.node(1).name, "valid");
}

TEST(GraphIoContract, SuccessfulReloadResetsRuntimeMetadata) {
  const auto valid_path = temp_path("photospider-contract-runtime-old.yaml");
  const auto replacement_path =
      temp_path("photospider-contract-runtime-new.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: old_graph\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(replacement_path,
             "- id: 3\n"
             "  name: new_graph\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");

  GraphModel graph(temp_path("photospider-contract-reload-runtime-cache"));
  GraphIOService io;
  io.load(graph, valid_path);
  ASSERT_EQ(graph.node(1).name, "old_graph");

  graph.timing_results.node_timings.push_back(
      {1, "old_graph", 9.0, "computed"});
  graph.timing_results.total_ms = 9.0;
  graph.total_io_time_ms.store(7.0);
  graph.set_skip_save_cache(true);
  graph.dirty_generation_counter = 42;
  graph.dirty_source_hp_commit_generation[1] = 42;
  graph.last_dirty_region_snapshot_debug = "stale dirty snapshot";
  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 42;
  snapshot.dirty_source_nodes.push_back(1);
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  compute::ComputePlan plan;
  plan.target_node_id = 1;
  graph.last_compute_plan = plan;
  graph.recent_compute_plans.push_back(plan);

  io.load(graph, replacement_path);

  ASSERT_FALSE(graph.has_node(1));
  ASSERT_TRUE(graph.has_node(3));
  EXPECT_EQ(graph.node(3).name, "new_graph");
  EXPECT_TRUE(graph.timing_results.node_timings.empty());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_DOUBLE_EQ(graph.total_io_time_ms.load(), 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
  EXPECT_EQ(graph.dirty_generation_counter, 0u);
  EXPECT_TRUE(graph.dirty_source_hp_commit_generation.empty());
  EXPECT_FALSE(graph.last_dirty_region_snapshot_debug.has_value());
  EXPECT_FALSE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_TRUE(graph.recent_dirty_region_snapshots.empty());
  EXPECT_FALSE(graph.last_compute_plan.has_value());
  EXPECT_TRUE(graph.recent_compute_plans.empty());
}

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
/**
 * @brief Reports a stream failure that occurs after the destination opens.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The private failpoint marks the real stream bad only after its YAML
 * write. It does not throw or replace the writer, so this test remains red
 * unless GraphIOService observes the late stream state itself.
 */
TEST(GraphIoContract, SaveReportsPostOpenWriteFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-late-write-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io;
  const auto output_path =
      temp_path("photospider-contract-save-late-write.yaml");
  std::filesystem::remove(output_path);

  testing::arm_graph_io_save_failure(
      output_path, testing::GraphIoSaveFailureStage::AfterWrite);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count = testing::graph_io_save_failure_hit_count();
  testing::clear_graph_io_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}

/**
 * @brief Reports a destination flush failure after YAML bytes are emitted.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The private failpoint changes only the real stream state after flush,
 * so a passing test proves GraphIOService observes that late status.
 */
TEST(GraphIoContract, SaveReportsPostWriteFlushFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-flush-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io;
  const auto output_path = temp_path("photospider-contract-save-flush.yaml");
  std::filesystem::remove(output_path);

  testing::arm_graph_io_save_failure(
      output_path, testing::GraphIoSaveFailureStage::AfterFlush);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count = testing::graph_io_save_failure_hit_count();
  testing::clear_graph_io_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}

/**
 * @brief Reports a destination close failure after successful flushing.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Explicit close makes the final stream state observable before the
 * ofstream destructor; the failpoint only marks that real stream failed.
 */
TEST(GraphIoContract, SaveReportsPostFlushCloseFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-close-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io;
  const auto output_path = temp_path("photospider-contract-save-close.yaml");
  std::filesystem::remove(output_path);

  testing::arm_graph_io_save_failure(
      output_path, testing::GraphIoSaveFailureStage::AfterClose);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count = testing::graph_io_save_failure_hit_count();
  testing::clear_graph_io_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}
#endif

/**
 * @brief Preserves the previous node when exact YAML replacement validation
 *        fails.
 *
 * @return Nothing; GoogleTest assertions report error-category or model-state
 *         mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup cannot
 *         allocate or create its deterministic graph inputs.
 * @note The required-node Kernel boundary reports InvalidYaml while the
 *       candidate-map validation keeps the visible node unchanged.
 */
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
  auto loaded =
      kernel.load_graph("contract_graph", root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  const std::string invalid_replacement =
      "id: 1\n"
      "name: invalid\n"
      "type: kernel_contract_test\n"
      "subtype: source\n"
      "image_inputs:\n"
      "  - from_node_id: 99\n";
  try {
    kernel.set_node_yaml("contract_graph", 1, invalid_replacement);
    FAIL() << "invalid node replacement unexpectedly succeeded";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidYaml);
  }

  auto node_yaml = kernel.get_node_yaml("contract_graph", 1);
  ASSERT_TRUE(node_yaml.has_value());
  EXPECT_NE(node_yaml->find("valid"), std::string::npos);
  kernel.close_graph("contract_graph");
}

}  // namespace ps
