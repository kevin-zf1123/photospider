#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"  // NOLINT(build/include_subdir)
#include "node.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_OP_PLUGIN_DIR
#define PS_TEST_OP_PLUGIN_DIR "build/test_plugins"
#endif

namespace ps {
namespace {

/**
 * @brief Registers deterministic operations used by embedded Host tests.
 *
 * @throws std::bad_alloc if registry storage allocation fails.
 * @note The operation is intentionally tiny and CPU-only so Host seam tests
 *       exercise frontend behavior without depending on external plugins or
 *       GPU availability.
 */
void register_host_adapter_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const YAML::Node& params = node.runtime_parameters
                                             ? node.runtime_parameters
                                             : node.parameters;
              const int width = params["width"].as<int>(6);
              const int height = params["height"].as<int>(4);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(7.0f);
              output.space.absolute_roi = cv::Rect(0, 0, width, height);
              output.debug.compute_device = "host-adapter-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "slow_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const YAML::Node& params = node.runtime_parameters
                                             ? node.runtime_parameters
                                             : node.parameters;
              std::this_thread::sleep_for(
                  std::chrono::milliseconds(params["sleep_ms"].as<int>(50)));
              const int width = params["width"].as<int>(5);
              const int height = params["height"].as<int>(3);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(3.0f);
              output.space.absolute_roi = cv::Rect(0, 0, width, height);
              output.debug.compute_device = "host-adapter-slow-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "resized_extent",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const YAML::Node& params = node.runtime_parameters
                                             ? node.runtime_parameters
                                             : node.parameters;
              const int width = params["width"].as<int>(6);
              const int height = params["height"].as<int>(4);
              const int roi_width = params["roi_width"].as<int>(12);
              const int roi_height = params["roi_height"].as<int>(9);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(5.0f);
              output.space.absolute_roi = cv::Rect(0, 0, roi_width, roi_height);
              output.debug.compute_device = "host-adapter-resized-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "identity",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty() || inputs.front() == nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "host adapter identity requires one input");
              }
              const NodeOutput& input = *inputs.front();
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  input.image_buffer.width, input.image_buffer.height,
                  input.image_buffer.channels, input.image_buffer.type);
              toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
              output.space.absolute_roi = input.space.absolute_roi;
              output.debug.compute_device = "host-adapter-identity-test";
              (void)node;
              return output;
            }));
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "identity",
        DirtyRoiPropFunc([](const Node&, const cv::Rect& roi,
                            const GraphModel&) { return roi; }));
    OpRegistry::instance().register_forward_propagator(
        "host_adapter_test", "identity",
        ForwardRoiPropFunc([](const Node&, const cv::Rect& roi,
                              const GraphModel&, const cv::Size&,
                              const cv::Size&) { return roi; }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "offset_identity",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>& inputs) {
          if (inputs.empty() || inputs.front() == nullptr) {
            throw GraphError(GraphErrc::InvalidParameter,
                             "host adapter offset_identity requires one input");
          }
          const NodeOutput& input = *inputs.front();
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              input.image_buffer.width, input.image_buffer.height,
              input.image_buffer.channels, input.image_buffer.type);
          toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
          output.space.absolute_roi = input.space.absolute_roi;
          output.debug.compute_device = "host-adapter-offset-identity-test";
          (void)node;
          return output;
        }));
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "offset_identity",
        DirtyRoiPropFunc(
            [](const Node&, const cv::Rect& roi, const GraphModel&) {
              return cv::Rect(roi.x + 64, roi.y, roi.width, roi.height);
            }));
  });
}

/**
 * @brief Owns a unique temporary directory for one Host adapter test.
 *
 * @throws std::filesystem::filesystem_error if setup cleanup or directory
 *         creation fails.
 * @note The destructor uses an error_code cleanup path so test assertions are
 *       not masked by best-effort removal failures.
 */
class ScopedTempDir {
 public:
  /**
   * @brief Creates an empty unique temporary directory.
   *
   * @param name Directory name below the platform temporary directory.
   * @throws std::filesystem::filesystem_error if directory creation fails.
   */
  explicit ScopedTempDir(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              (name + "_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  /**
   * @brief Removes the temporary directory.
   *
   * @throws Nothing.
   * @note Cleanup is best-effort so it cannot hide a test failure.
   */
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /**
   * @brief Returns the root path for the temporary directory.
   *
   * @return Temporary root path.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const { return root_; }

 private:
  /** @brief Temporary directory root owned by this helper. */
  std::filesystem::path root_;
};

/**
 * @brief Writes a single-node Host adapter test graph.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note The graph has one ending node so traversal, dependency-tree, compute,
 *       and image-returning Host APIs all observe the same deterministic node.
 */
void write_host_adapter_graph(const std::filesystem::path& path, int width = 6,
                              int height = 4,
                              const std::string& subtype = "source") {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: host_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: " << subtype << "\n"
      << "  parameters:\n"
      << "    width: " << width << "\n"
      << "    height: " << height << "\n";
  if (subtype == "slow_source") {
    out << "    sleep_ms: 75\n";
  }
  if (subtype == "resized_extent") {
    out << "    roi_width: 12\n"
        << "    roi_height: 9\n";
  }
}

/**
 * @brief Writes a graph whose node has no registered operation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Loading succeeds because operation lookup is deferred to compute, so
 *       Host image-compute failure mapping can verify GraphErrc::NoOperation.
 */
void write_host_adapter_unregistered_op_graph(
    const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: missing_op_source\n"
      << "  type: host_adapter_missing\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 6\n"
      << "    height: 4\n";
}

/**
 * @brief Writes a two-node graph with identity ROI propagation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 depends on node 1 through an image input and uses an explicit
 *       identity propagator, giving Host ROI tests deterministic forward and
 *       backward rectangles.
 */
void write_host_adapter_roi_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: roi_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 8\n"
      << "    height: 6\n"
      << "- id: 2\n"
      << "  name: roi_identity\n"
      << "  type: host_adapter_test\n"
      << "  subtype: identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Writes a two-node graph whose backward dirty ROI differs by edge side.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 uses a deterministic test-only dirty propagator that shifts the
 *       upstream demand by one HP micro-tile, so Host conversion tests can
 *       catch accidental swaps of `from_roi` and `to_roi`.
 */
void write_host_adapter_offset_roi_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: roi_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 256\n"
      << "    height: 128\n"
      << "- id: 2\n"
      << "  name: roi_offset_identity\n"
      << "  type: host_adapter_test\n"
      << "  subtype: offset_identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Returns the lifecycle operation plugin fixture directory.
 *
 * @return Directory containing the platform-specific lifecycle plugin library.
 * @throws std::bad_alloc if path construction allocates and fails.
 * @note CMake injects `PS_TEST_OP_PLUGIN_DIR` so the path follows the active
 *       binary directory instead of assuming a source-relative `build/` tree.
 */
std::filesystem::path lifecycle_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR) / "lifecycle";
}

/**
 * @brief Returns YAML text for replacing the single Host test node.
 *
 * @param name Replacement node name.
 * @param width Replacement width parameter.
 * @param height Replacement height parameter.
 * @return YAML text accepted by set_node_yaml().
 * @throws std::bad_alloc if string construction allocates and fails.
 * @note The backend preserves the target node id supplied separately.
 */
std::string replacement_node_yaml(const std::string& name, int width,
                                  int height) {
  std::ostringstream out;
  out << R"YAML(
id: 1
name: )YAML"
      << name << R"YAML(
type: host_adapter_test
subtype: source
parameters:
  width: )YAML"
      << width << R"YAML(
  height: )YAML"
      << height << "\n";
  return out.str();
}

/**
 * @brief Loads a deterministic Host adapter graph.
 *
 * @param host Host under test.
 * @param root Temporary root containing source and session folders.
 * @param session Session label to load.
 * @param subtype Operation subtype to write into the graph YAML.
 * @return Loaded session id.
 * @throws std::bad_alloc if path or diagnostic strings allocate and fail.
 * @note Test assertions fail immediately if loading is rejected.
 */
GraphSessionId load_test_graph(Host& host, const std::filesystem::path& root,
                               const std::string& session,
                               const std::string& subtype = "source") {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = (root / "source" / (session + ".yaml")).string();
  request.cache_root_dir = (root / "cache").string();
  write_host_adapter_graph(request.yaml_path, 6, 4, subtype);
  auto loaded = host.load_graph(request);
  EXPECT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.value, session);
  return loaded.value;
}

/**
 * @brief Builds the graph load request used by Host adapter tests.
 *
 * @param root Temporary root containing source, sessions, and cache folders.
 * @return GraphLoadRequest pointing at a deterministic single-node graph.
 * @throws std::bad_alloc if path string conversion allocates and fails.
 * @note The request exercises the embedded adapter's copy/load path by
 *       providing an explicit YAML source file.
 */
GraphLoadRequest make_load_request(const std::filesystem::path& root) {
  const auto yaml_path = root / "source" / "host_graph.yaml";
  write_host_adapter_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"host_adapter_graph"};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Builds a compute request for the Host adapter test graph.
 *
 * @param session Session to compute.
 * @return HostComputeRequest for node 1 with timing enabled.
 * @throws std::bad_alloc if precision string allocation fails.
 */
HostComputeRequest make_compute_request(const GraphSessionId& session) {
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{1};
  request.cache.precision = "fp32";
  request.telemetry.enable_timing = true;
  return request;
}

/**
 * @brief Reports whether a string vector contains a value.
 *
 * @param values Values to search.
 * @param needle String to find.
 * @return True when needle is present.
 * @throws Nothing directly.
 */
bool contains_string(const std::vector<std::string>& values,
                     const std::string& needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

TEST(EmbeddedHostAdapter,
     CoversInteractionCoreWithPublicSnapshotsAndNoKernelExposure) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphLoadRequest load_request = make_load_request(temp.root());
  const GraphSessionId session = load_request.session;
  auto loaded = host->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.value, session.value);

  auto graphs = host->list_graphs();
  ASSERT_TRUE(graphs.status.ok) << graphs.status.message;
  ASSERT_EQ(graphs.value.size(), 1u);
  EXPECT_EQ(graphs.value.front().value, session.value);

  auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  ASSERT_EQ(ids.value.size(), 1u);
  EXPECT_EQ(ids.value.front().value, 1);

  auto ending = host->ending_nodes(session);
  ASSERT_TRUE(ending.status.ok) << ending.status.message;
  ASSERT_EQ(ending.value.size(), 1u);
  EXPECT_EQ(ending.value.front().value, 1);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  EXPECT_EQ(graph_view.value.session.value, session.value);
  EXPECT_EQ(graph_view.value.nodes.front().name, "host_source");
  EXPECT_EQ(graph_view.value.nodes.front().parameters.at("width"), "6");

  auto node_view = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node_view.status.ok) << node_view.status.message;
  EXPECT_EQ(node_view.value.type, "host_adapter_test");
  EXPECT_EQ(node_view.value.subtype, "source");

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  EXPECT_EQ(tree.value.scope, HostDependencyTreeScope::EndingNodes);
  ASSERT_EQ(tree.value.entries.size(), 1u);
  EXPECT_EQ(tree.value.entries.front().node.id.value, 1);

  auto traversal = host->traversal_orders(session);
  ASSERT_TRUE(traversal.status.ok) << traversal.status.message;
  ASSERT_EQ(traversal.value.size(), 1u);
  ASSERT_EQ(traversal.value.at(1).size(), 1u);
  EXPECT_EQ(traversal.value.at(1).front().value, 1);

  const HostComputeRequest compute_request = make_compute_request(session);
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto image = host->compute_and_get_image(compute_request);
  ASSERT_TRUE(image.status.ok) << image.status.message;
  EXPECT_EQ(image.value.width, 6);
  EXPECT_EQ(image.value.height, 4);
  EXPECT_EQ(image.value.channels, 1);
  EXPECT_EQ(image.value.device, Device::CPU);
  ASSERT_NE(image.value.data, nullptr);

  auto async_compute = host->compute_async(compute_request);
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;
  OperationStatus async_status = async_compute.value.get();
  EXPECT_TRUE(async_status.ok) << async_status.message;

  auto timing = host->timing(session);
  ASSERT_TRUE(timing.status.ok) << timing.status.message;
  EXPECT_FALSE(timing.value.node_timings.empty());

  auto io_time = host->last_io_time(session);
  ASSERT_TRUE(io_time.status.ok) << io_time.status.message;
  EXPECT_GE(io_time.value, 0.0);

  auto events = host->drain_compute_events(session);
  ASSERT_TRUE(events.status.ok) << events.status.message;

  auto dirty = host->dirty_region_snapshot(session);
  ASSERT_TRUE(dirty.status.ok) << dirty.status.message;

  auto scheduler_types = host->scheduler_available_types();
  ASSERT_TRUE(scheduler_types.status.ok) << scheduler_types.status.message;
  EXPECT_TRUE(contains_string(scheduler_types.value, "serial_debug"));

  auto replaced = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  ASSERT_TRUE(replaced.status.ok) << replaced.status.message;

  auto scheduler_info =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(scheduler_info.status.ok) << scheduler_info.status.message;
  EXPECT_EQ(scheduler_info.value.scheduler_name, "serial_debug");
  EXPECT_NE(scheduler_info.value.stats.find("SerialDebugScheduler"),
            std::string::npos);

  auto scheduler_trace = host->scheduler_trace(session);
  ASSERT_TRUE(scheduler_trace.status.ok) << scheduler_trace.status.message;

  auto description = host->scheduler_description("serial_debug");
  ASSERT_TRUE(description.status.ok) << description.status.message;
  EXPECT_NE(description.value.find("Single-threaded"), std::string::npos);

  auto missing_description =
      host->scheduler_description("missing_scheduler_type");
  EXPECT_FALSE(missing_description.status.ok);
  EXPECT_EQ(missing_description.status.code, GraphErrc::NotFound);

  auto plugins = host->plugins_load_report({});
  ASSERT_TRUE(plugins.status.ok) << plugins.status.message;
  EXPECT_EQ(plugins.value.loaded, 0);

  auto seed = host->seed_builtin_ops();
  ASSERT_TRUE(seed.status.ok) << seed.status.message;

  auto op_sources = host->ops_combined_sources();
  ASSERT_TRUE(op_sources.status.ok) << op_sources.status.message;
  EXPECT_EQ(op_sources.value.at("host_adapter_test:source"), "built-in");

  auto yaml = host->get_node_yaml(session, NodeId{1});
  ASSERT_TRUE(yaml.status.ok) << yaml.status.message;
  EXPECT_NE(yaml.value.find("host_source"), std::string::npos);

  auto clear_memory = host->clear_memory_cache(session);
  ASSERT_TRUE(clear_memory.status.ok) << clear_memory.status.message;

  auto close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

TEST(EmbeddedHostAdapter,
     SpatialSnapshotPreservesOutputExtentSeparatelyFromRoi) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_spatial_extent_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "spatial_extent", "resized_extent");
  const HostComputeRequest compute_request = make_compute_request(session);
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto node_view = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node_view.status.ok) << node_view.status.message;
  ASSERT_TRUE(node_view.value.space.has_value());
  EXPECT_EQ(node_view.value.space->extent.width, 6);
  EXPECT_EQ(node_view.value.space->extent.height, 4);
  EXPECT_EQ(node_view.value.space->absolute_roi.width, 12);
  EXPECT_EQ(node_view.value.space->absolute_roi.height, 9);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  ASSERT_TRUE(graph_view.value.nodes.front().space.has_value());
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.width, 6);
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.height, 4);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.width, 12);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.height, 9);

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  ASSERT_EQ(tree.value.entries.size(), 1u);
  ASSERT_TRUE(tree.value.entries.front().node.space.has_value());
  EXPECT_EQ(tree.value.entries.front().node.space->extent.width, 6);
  EXPECT_EQ(tree.value.entries.front().node.space->extent.height, 4);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.width, 12);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.height, 9);
}

TEST(EmbeddedHostAdapter, AsyncComputeCanFinishAfterCloseGraphRequest) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "async_close_graph", "slow_source");
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;

  auto async_compute = host->compute_async(request);
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;

  auto close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;

  OperationStatus async_status = async_compute.value.get();
  EXPECT_TRUE(async_status.ok) << async_status.message;

  auto ids_after_close = host->list_node_ids(session);
  EXPECT_FALSE(ids_after_close.status.ok);
  EXPECT_EQ(ids_after_close.status.code, GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ComputeReturnsNotFoundForMissingSession) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_compute_missing_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  HostComputeRequest missing_request =
      make_compute_request(GraphSessionId{"missing_compute_graph"});
  auto missing = host->compute(missing_request);
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(missing.status.code, GraphErrc::NotFound);
  auto missing_image = host->compute_and_get_image(missing_request);
  EXPECT_FALSE(missing_image.status.ok);
  EXPECT_EQ(missing_image.status.code, GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_compute_graph");
  HostComputeRequest closed_request = make_compute_request(session);
  auto initial = host->compute(closed_request);
  ASSERT_TRUE(initial.status.ok) << initial.status.message;

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->compute(closed_request);
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(closed.status.code, GraphErrc::NotFound);
  auto closed_image = host->compute_and_get_image(closed_request);
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(closed_image.status.code, GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ComputeImagePreservesBackendFailureStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_image_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "image_status_graph");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};

  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  EXPECT_FALSE(missing_node_image.status.ok);
  EXPECT_EQ(missing_node_image.status.code, GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_image.status.message.empty());

  auto missing_node_error = host->last_error(session);
  EXPECT_FALSE(missing_node_error.ok);
  EXPECT_EQ(missing_node_error.code, GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_error.message.empty());

  auto recovered_image =
      host->compute_and_get_image(make_compute_request(session));
  ASSERT_TRUE(recovered_image.status.ok) << recovered_image.status.message;
  auto cleared_error = host->last_error(session);
  EXPECT_TRUE(cleared_error.ok) << cleared_error.message;

  GraphLoadRequest missing_op_load;
  missing_op_load.session = GraphSessionId{"image_missing_op_graph"};
  missing_op_load.root_dir = (temp.root() / "sessions").string();
  missing_op_load.yaml_path =
      (temp.root() / "source" / "image_missing_op_graph.yaml").string();
  missing_op_load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(missing_op_load.yaml_path);
  auto loaded_missing_op = host->load_graph(missing_op_load);
  ASSERT_TRUE(loaded_missing_op.status.ok) << loaded_missing_op.status.message;

  auto missing_op_image = host->compute_and_get_image(
      make_compute_request(missing_op_load.session));
  EXPECT_FALSE(missing_op_image.status.ok);
  EXPECT_EQ(missing_op_image.status.code, GraphErrc::NoOperation);
  EXPECT_NE(missing_op_image.status.message.find("No op"), std::string::npos);

  auto missing_op_error = host->last_error(missing_op_load.session);
  EXPECT_FALSE(missing_op_error.ok);
  EXPECT_EQ(missing_op_error.code, GraphErrc::NoOperation);
  EXPECT_NE(missing_op_error.message.find("No op"), std::string::npos);
}

TEST(EmbeddedHostAdapter, ReplaceSchedulerReturnsNotFoundForMissingSession) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_scheduler_missing_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  auto missing = host->replace_scheduler(
      GraphSessionId{"missing_scheduler_graph"},
      ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(missing.status.code, GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_scheduler_graph");
  auto invalid_type = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "missing_scheduler_type");
  EXPECT_FALSE(invalid_type.status.ok);
  EXPECT_EQ(invalid_type.status.code, GraphErrc::InvalidParameter);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(closed.status.code, GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ReloadSaveSetNodeAndClearGraphReturnStatuses) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_graph_mutation_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "graph_mutation");

  auto set = host->set_node_yaml(session, NodeId{1},
                                 replacement_node_yaml("host_replaced", 9, 2));
  ASSERT_TRUE(set.status.ok) << set.status.message;

  auto node = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  EXPECT_EQ(node.value.name, "host_replaced");
  EXPECT_EQ(node.value.parameters.at("width"), "9");

  const auto saved_path = temp.root() / "saved" / "saved_graph.yaml";
  std::filesystem::create_directories(saved_path.parent_path());
  auto save = host->save_graph(session, saved_path.string());
  ASSERT_TRUE(save.status.ok) << save.status.message;
  EXPECT_TRUE(std::filesystem::exists(saved_path));

  const auto reload_path = temp.root() / "source" / "reload_graph.yaml";
  write_host_adapter_graph(reload_path, 11, 5);
  auto reload = host->reload_graph(session, reload_path.string());
  ASSERT_TRUE(reload.status.ok) << reload.status.message;

  auto reloaded = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(reloaded.status.ok) << reloaded.status.message;
  EXPECT_EQ(reloaded.value.parameters.at("width"), "11");

  auto missing_reload =
      host->reload_graph(GraphSessionId{"missing_graph"}, reload_path.string());
  EXPECT_FALSE(missing_reload.status.ok);
  EXPECT_EQ(missing_reload.status.code, GraphErrc::NotFound);

  const auto missing_file_path =
      temp.root() / "source" / "missing_reload_graph.yaml";
  auto io_reload = host->reload_graph(session, missing_file_path.string());
  EXPECT_FALSE(io_reload.status.ok);
  EXPECT_EQ(io_reload.status.code, GraphErrc::Io);

  auto after_io_reload = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(after_io_reload.status.ok) << after_io_reload.status.message;
  EXPECT_EQ(after_io_reload.value.parameters.at("width"), "11");

  const auto invalid_yaml_path =
      temp.root() / "source" / "invalid_reload_graph.yaml";
  {
    std::ofstream invalid_yaml(invalid_yaml_path);
    invalid_yaml << "not: a sequence\n";
  }
  auto invalid_reload = host->reload_graph(session, invalid_yaml_path.string());
  EXPECT_FALSE(invalid_reload.status.ok);
  EXPECT_EQ(invalid_reload.status.code, GraphErrc::InvalidYaml);

  auto after_invalid_reload = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(after_invalid_reload.status.ok)
      << after_invalid_reload.status.message;
  EXPECT_EQ(after_invalid_reload.value.parameters.at("width"), "11");

  auto clear = host->clear_graph(session);
  ASSERT_TRUE(clear.status.ok) << clear.status.message;

  auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  EXPECT_TRUE(ids.value.empty());
}

TEST(EmbeddedHostAdapter, RoiProjectionUsesPublicPixelRectValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_roi_projection_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "roi_graph.yaml";
  write_host_adapter_roi_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"roi_projection"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();

  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const PixelRect roi{1, 2, 3, 2};
  auto projected =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{2});
  ASSERT_TRUE(projected.status.ok) << projected.status.message;
  EXPECT_EQ(projected.value.x, roi.x);
  EXPECT_EQ(projected.value.y, roi.y);
  EXPECT_EQ(projected.value.width, roi.width);
  EXPECT_EQ(projected.value.height, roi.height);

  auto back_projected =
      host->project_roi_backward(request.session, NodeId{2}, roi, NodeId{1});
  ASSERT_TRUE(back_projected.status.ok) << back_projected.status.message;
  EXPECT_EQ(back_projected.value.x, roi.x);
  EXPECT_EQ(back_projected.value.y, roi.y);
  EXPECT_EQ(back_projected.value.width, roi.width);
  EXPECT_EQ(back_projected.value.height, roi.height);

  auto missing_target =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{99});
  EXPECT_FALSE(missing_target.status.ok);
  EXPECT_EQ(missing_target.status.code, GraphErrc::InvalidParameter);
}

TEST(EmbeddedHostAdapter, DirtySnapshotPreservesMonolithicAndEdgeDetails) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_snapshot_details_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "dirty_roi_graph.yaml";
  write_host_adapter_offset_roi_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"dirty_snapshot_details"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();

  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest full_request;
  full_request.session = request.session;
  full_request.node = NodeId{2};
  full_request.cache.precision = "fp32";
  auto initial_compute = host->compute(full_request);
  ASSERT_TRUE(initial_compute.status.ok) << initial_compute.status.message;

  HostComputeRequest dirty_request = full_request;
  dirty_request.intent = ComputeIntent::GlobalHighPrecision;
  dirty_request.dirty_roi = PixelRect{70, 10, 20, 20};
  auto dirty_compute = host->compute(dirty_request);
  ASSERT_TRUE(dirty_compute.status.ok) << dirty_compute.status.message;

  auto snapshot = host->dirty_region_snapshot(request.session);
  ASSERT_TRUE(snapshot.status.ok) << snapshot.status.message;
  EXPECT_FALSE(snapshot.value.dirty_monolithic_nodes.empty());
  EXPECT_FALSE(snapshot.value.edge_mappings.empty());

  const auto monolithic_node =
      std::find_if(snapshot.value.dirty_monolithic_nodes.begin(),
                   snapshot.value.dirty_monolithic_nodes.end(),
                   [](const DirtyMonolithicRegionSnapshot& region) {
                     return region.node.value == 2 &&
                            region.domain == DirtyDomain::HighPrecision;
                   });
  ASSERT_NE(monolithic_node, snapshot.value.dirty_monolithic_nodes.end());
  EXPECT_TRUE(monolithic_node->whole_output);
  EXPECT_EQ(monolithic_node->pixel_roi.x, 0);
  EXPECT_EQ(monolithic_node->pixel_roi.y, 0);
  EXPECT_EQ(monolithic_node->pixel_roi.width, 256);
  EXPECT_EQ(monolithic_node->pixel_roi.height, 128);

  const auto edge_mapping = std::find_if(
      snapshot.value.edge_mappings.begin(), snapshot.value.edge_mappings.end(),
      [](const DirtyEdgeMappingSnapshot& mapping) {
        return mapping.from_node.value == 1 && mapping.to_node.value == 2 &&
               mapping.domain == DirtyDomain::HighPrecision;
      });
  ASSERT_NE(edge_mapping, snapshot.value.edge_mappings.end());
  EXPECT_EQ(edge_mapping->direction, DirtyEdgeDirection::BackwardDemand);
  EXPECT_EQ(edge_mapping->from_roi.x, 64);
  EXPECT_EQ(edge_mapping->from_roi.y, 0);
  EXPECT_EQ(edge_mapping->from_roi.width, 128);
  EXPECT_EQ(edge_mapping->from_roi.height, 64);
  EXPECT_EQ(edge_mapping->to_roi.x, 64);
  EXPECT_EQ(edge_mapping->to_roi.y, 0);
  EXPECT_EQ(edge_mapping->to_roi.width, 64);
  EXPECT_EQ(edge_mapping->to_roi.height, 64);
}

TEST(EmbeddedHostAdapter, DirtySourceAndCacheControlsExposeFrontendStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_cache_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "dirty_cache");

  HostComputeRequest request = make_compute_request(session);
  auto compute = host->compute(request);
  ASSERT_TRUE(compute.status.ok) << compute.status.message;

  const PixelRect roi{1, 1, 2, 2};
  auto begin = host->begin_dirty_source(session, NodeId{1},
                                        DirtyDomain::HighPrecision, roi);
  ASSERT_TRUE(begin.status.ok) << begin.status.message;
  ASSERT_FALSE(begin.value.sources.empty());
  EXPECT_EQ(begin.value.sources.front().node.value, 1);
  EXPECT_EQ(begin.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Updating);

  auto update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, PixelRect{2, 2, 1, 1});
  ASSERT_TRUE(update.status.ok) << update.status.message;

  auto end =
      host->end_dirty_source(session, NodeId{1}, DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.status.ok) << end.status.message;
  ASSERT_FALSE(end.value.sources.empty());
  EXPECT_EQ(end.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Settled);

  auto cache_all = host->cache_all_nodes(session, "fp32");
  EXPECT_TRUE(cache_all.status.ok) << cache_all.status.message;

  auto sync = host->synchronize_disk_cache(session, "fp32");
  EXPECT_TRUE(sync.status.ok) << sync.status.message;

  auto clear_memory = host->clear_memory_cache(session);
  EXPECT_TRUE(clear_memory.status.ok) << clear_memory.status.message;

  auto clear_drive = host->clear_drive_cache(session);
  EXPECT_TRUE(clear_drive.status.ok) << clear_drive.status.message;

  auto clear_all = host->clear_cache(session);
  EXPECT_TRUE(clear_all.status.ok) << clear_all.status.message;

  auto free_memory = host->free_transient_memory(session);
  EXPECT_TRUE(free_memory.status.ok) << free_memory.status.message;
}

TEST(EmbeddedHostAdapter, DirtySourceFailuresPreserveStatusCodes) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "dirty_status");
  const PixelRect roi{1, 1, 2, 2};
  const PixelRect empty_roi{1, 1, 0, 2};

  auto missing_session_begin =
      host->begin_dirty_source(GraphSessionId{"missing_dirty_session"},
                               NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_begin.status.ok);
  EXPECT_EQ(missing_session_begin.status.code, GraphErrc::NotFound);

  auto missing_session_update =
      host->update_dirty_source(GraphSessionId{"missing_dirty_session"},
                                NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_update.status.ok);
  EXPECT_EQ(missing_session_update.status.code, GraphErrc::NotFound);

  auto missing_session_end =
      host->end_dirty_source(GraphSessionId{"missing_dirty_session"}, NodeId{1},
                             DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_session_end.status.ok);
  EXPECT_EQ(missing_session_end.status.code, GraphErrc::NotFound);

  auto missing_node_begin = host->begin_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_begin.status.ok);
  EXPECT_EQ(missing_node_begin.status.code, GraphErrc::NotFound);
  EXPECT_NE(missing_node_begin.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_begin_error = host->last_error(session);
  EXPECT_FALSE(missing_node_begin_error.ok);
  EXPECT_EQ(missing_node_begin_error.code, GraphErrc::NotFound);

  auto missing_node_update = host->update_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_update.status.ok);
  EXPECT_EQ(missing_node_update.status.code, GraphErrc::NotFound);
  EXPECT_NE(missing_node_update.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_update_error = host->last_error(session);
  EXPECT_FALSE(missing_node_update_error.ok);
  EXPECT_EQ(missing_node_update_error.code, GraphErrc::NotFound);

  auto missing_node_end =
      host->end_dirty_source(session, NodeId{99}, DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_node_end.status.ok);
  EXPECT_EQ(missing_node_end.status.code, GraphErrc::NotFound);
  EXPECT_NE(missing_node_end.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_end_error = host->last_error(session);
  EXPECT_FALSE(missing_node_end_error.ok);
  EXPECT_EQ(missing_node_end_error.code, GraphErrc::NotFound);

  auto invalid_begin = host->begin_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_begin.status.ok);
  EXPECT_EQ(invalid_begin.status.code, GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_begin.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_begin_error = host->last_error(session);
  EXPECT_FALSE(invalid_begin_error.ok);
  EXPECT_EQ(invalid_begin_error.code, GraphErrc::InvalidParameter);

  auto invalid_update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_update.status.ok);
  EXPECT_EQ(invalid_update.status.code, GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_update.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_update_error = host->last_error(session);
  EXPECT_FALSE(invalid_update_error.ok);
  EXPECT_EQ(invalid_update_error.code, GraphErrc::InvalidParameter);
}

TEST(EmbeddedHostAdapter, SchedulerScanLoadAndPluginUnloadUseStatusValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_scheduler_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto scheduler_dir = temp.root() / "schedulers";
  std::filesystem::create_directories(scheduler_dir);

  auto scan = host->scheduler_scan({scheduler_dir.string()});
  ASSERT_TRUE(scan.status.ok) << scan.status.message;

  auto bad_load =
      host->scheduler_load((temp.root() / "missing_scheduler.so").string());
  EXPECT_FALSE(bad_load.status.ok);
  EXPECT_EQ(bad_load.status.code, GraphErrc::Io);

  auto loaded = host->scheduler_loaded_plugins();
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const auto plugin_dir = lifecycle_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "lifecycle op plugin directory was not built: " << plugin_dir;

  auto plugin_report = host->plugins_load_report({plugin_dir.string()});
  ASSERT_TRUE(plugin_report.status.ok) << plugin_report.status.message;
  EXPECT_EQ(plugin_report.value.loaded, 1);
  EXPECT_TRUE(plugin_report.value.errors.empty());
  EXPECT_TRUE(
      contains_string(plugin_report.value.new_op_keys, "plugin_lifecycle:op"));

  auto plugin_sources = host->ops_combined_sources();
  ASSERT_TRUE(plugin_sources.status.ok) << plugin_sources.status.message;
  ASSERT_NE(plugin_sources.value.find("plugin_lifecycle:op"),
            plugin_sources.value.end());

  auto unload_ops = host->plugins_unload_all();
  ASSERT_TRUE(unload_ops.status.ok) << unload_ops.status.message;
  EXPECT_GE(unload_ops.value, 1);

  auto plugin_sources_after_unload = host->ops_combined_sources();
  ASSERT_TRUE(plugin_sources_after_unload.status.ok)
      << plugin_sources_after_unload.status.message;
  EXPECT_EQ(plugin_sources_after_unload.value.count("plugin_lifecycle:op"), 0u);

  auto status_only_load = host->plugins_load({plugin_dir.string()});
  ASSERT_TRUE(status_only_load.status.ok) << status_only_load.status.message;

  auto unload_status_only = host->plugins_unload_all();
  ASSERT_TRUE(unload_status_only.status.ok)
      << unload_status_only.status.message;
  EXPECT_GE(unload_status_only.value, 1);

  auto empty_scan_plugins =
      host->plugins_load({(temp.root() / "missing_plugins").string()});
  EXPECT_TRUE(empty_scan_plugins.status.ok)
      << empty_scan_plugins.status.message;
}

}  // namespace
}  // namespace ps
