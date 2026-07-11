#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
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
 *       GPU availability. The `resource_exhausted` operation deliberately
 *       throws std::bad_alloc from real node execution so the public Host
 *       exception contract is tested through the complete backend chain.
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
              output.space.inverse_matrix = {2.0, 0.0, 5.0, 0.0, 3.0,
                                             7.0, 0.0, 0.0, 1.0};
              output.space.local_inverse_matrix = {1.0,  0.0, 11.0, 0.0, 1.0,
                                                   13.0, 0.0, 0.0,  1.0};
              output.debug.compute_device = "host-adapter-resized-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "no_image",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "resource_exhausted",
        MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&)
                             -> NodeOutput { throw std::bad_alloc{}; }));
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
                              const std::string& subtype = "source",
                              int slow_sleep_ms = 75) {
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
    out << "    sleep_ms: " << slow_sleep_ms << "\n";
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
 * @brief Returns the replacement lifecycle plugin fixture directory.
 *
 * @return Directory containing the platform-specific override plugin library.
 * @throws std::bad_alloc if path construction allocates and fails.
 * @note The fixture replaces the same canonical operation key as the lifecycle
 *       plugin so multiple Host instances can exercise one restoration chain.
 */
std::filesystem::path override_lifecycle_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR) / "override";
}

/**
 * @brief Writes a graph backed by the dynamically loaded lifecycle operation.
 *
 * @param path YAML file path to create.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error if parent directory creation fails.
 * @throws std::ios_base::failure if opening or writing the graph file fails.
 * @note The operation returns debug metadata without requiring image inputs, so
 *       Host inspection can distinguish original and replacement callbacks.
 */
void write_lifecycle_plugin_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: lifecycle_plugin_node\n"
      << "  type: plugin_lifecycle\n"
      << "  subtype: op\n";
}

/**
 * @brief Cleans process-global operation plugins through one public Host.
 *
 * Construction removes stale plugins and converts a non-OK public status into
 * `std::runtime_error`. Destruction retries global cleanup but suppresses all
 * exceptions so assertion unwinding remains visible.
 *
 * @throws std::bad_alloc when construction cannot copy Host status storage.
 * @throws std::runtime_error when initial public cleanup reports failure.
 * @note The referenced Host must outlive this guard. Cleanup deliberately uses
 *       the public global-unload surface exercised by the test; every Host sees
 *       the same process-owner state.
 */
class ScopedHostPluginCleanup final {
 public:
  /**
   * @brief Removes stale process plugins before a multi-Host scenario.
   *
   * @param host Long-lived Host used for cleanup.
   * @throws std::bad_alloc if the Host boundary cannot construct a status.
   * @throws std::runtime_error if the public cleanup status is not OK; the
   *         exception copies that status message for test diagnostics.
   * @note The borrowed Host is retained by reference and must remain alive
   * until this guard's destructor has finished.
   */
  explicit ScopedHostPluginCleanup(Host& host) : host_(host) {
    const auto cleanup = host_.plugins_unload_all();
    if (!cleanup.status.ok) {
      throw std::runtime_error(cleanup.status.message);
    }
  }

  /**
   * @brief Performs best-effort public global cleanup after assertions.
   *
   * @throws Nothing; Host exceptions are caught and suppressed.
   * @note Cleanup runs while `host_` is still alive and does not replace an
   *       exception already unwinding from the test body.
   */
  ~ScopedHostPluginCleanup() noexcept {
    try {
      (void)host_.plugins_unload_all();
    } catch (...) {
      // Test teardown must not hide the assertion that triggered unwinding.
    }
  }

  /**
   * @brief Prevents duplicating cleanup ownership for one borrowed Host.
   *
   * @param other Guard that remains the sole cleanup owner.
   * @note Deletion prevents two destructors from racing global cleanup.
   */
  ScopedHostPluginCleanup(const ScopedHostPluginCleanup& other) = delete;

  /**
   * @brief Prevents retargeting an active cleanup guard.
   *
   * @param other Guard whose borrowed Host must remain unchanged.
   * @return No value because this operation is deleted.
   * @note Lexical lifetime remains paired with the Host supplied at
   *       construction.
   */
  ScopedHostPluginCleanup& operator=(const ScopedHostPluginCleanup& other) =
      delete;

  /**
   * @brief Prevents transferring cleanup ownership away from its lexical Host.
   *
   * @param other Guard that remains paired with its borrowed Host.
   * @note Deletion keeps one deterministic destructor cleanup point.
   */
  ScopedHostPluginCleanup(ScopedHostPluginCleanup&& other) = delete;

  /**
   * @brief Prevents replacing cleanup ownership through move assignment.
   *
   * @param other Guard whose borrowed Host remains unchanged.
   * @return No value because this operation is deleted.
   * @note Neither guard can become responsible for a different Host.
   */
  ScopedHostPluginCleanup& operator=(ScopedHostPluginCleanup&& other) = delete;

 private:
  /**
   * @brief Public Host borrowed for initial and final process-global cleanup.
   * @note The surrounding test owns the Host and keeps it alive past this
   * guard.
   */
  Host& host_;
};

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
 * @param slow_sleep_ms Milliseconds used by the slow_source fixture op.
 * @return Loaded session id.
 * @throws std::bad_alloc if path or diagnostic strings allocate and fail.
 * @note Test assertions fail immediately if loading is rejected.
 */
GraphSessionId load_test_graph(Host& host, const std::filesystem::path& root,
                               const std::string& session,
                               const std::string& subtype = "source",
                               int slow_sleep_ms = 75) {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = (root / "source" / (session + ".yaml")).string();
  request.cache_root_dir = (root / "cache").string();
  write_host_adapter_graph(request.yaml_path, 6, 4, subtype, slow_sleep_ms);
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
  EXPECT_EQ(missing_description.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_description.status),
            GraphErrc::NotFound);
  EXPECT_EQ(missing_description.status.name, "not_found");

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
  EXPECT_EQ(node_view.value.space->inverse_matrix[2], 5.0);
  EXPECT_EQ(node_view.value.space->inverse_matrix[5], 7.0);
  EXPECT_EQ(node_view.value.space->local_inverse_matrix[2], 11.0);
  EXPECT_EQ(node_view.value.space->local_inverse_matrix[5], 13.0);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  ASSERT_TRUE(graph_view.value.nodes.front().space.has_value());
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.width, 6);
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.height, 4);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.width, 12);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.height, 9);
  EXPECT_EQ(graph_view.value.nodes.front().space->local_inverse_matrix[2],
            11.0);
  EXPECT_EQ(graph_view.value.nodes.front().space->local_inverse_matrix[5],
            13.0);

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  ASSERT_EQ(tree.value.entries.size(), 1u);
  ASSERT_TRUE(tree.value.entries.front().node.space.has_value());
  EXPECT_EQ(tree.value.entries.front().node.space->extent.width, 6);
  EXPECT_EQ(tree.value.entries.front().node.space->extent.height, 4);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.width, 12);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.height, 9);
  EXPECT_EQ(tree.value.entries.front().node.space->local_inverse_matrix[2],
            11.0);
  EXPECT_EQ(tree.value.entries.front().node.space->local_inverse_matrix[5],
            13.0);
}

TEST(EmbeddedHostAdapter, ComputePlanningSnapshotsUsePublicValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_planning_snapshot_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "planning_snapshot");

  auto before_compute = host->compute_planning_snapshot(session);
  ASSERT_TRUE(before_compute.status.ok) << before_compute.status.message;
  EXPECT_FALSE(before_compute.value.has_value());

  auto before_history = host->recent_compute_planning_snapshots(session);
  ASSERT_TRUE(before_history.status.ok) << before_history.status.message;
  EXPECT_TRUE(before_history.value.empty());

  HostComputeRequest compute_request = make_compute_request(session);
  compute_request.execution.parallel = true;
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto latest = host->compute_planning_snapshot(session);
  ASSERT_TRUE(latest.status.ok) << latest.status.message;
  ASSERT_TRUE(latest.value.has_value());
  EXPECT_EQ(latest.value->intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(latest.value->target_node.value, 1);
  EXPECT_TRUE(latest.value->parallel);
  EXPECT_EQ(latest.value->planned_node_count, 1u);
  EXPECT_GE(latest.value->task_count, 1u);
  EXPECT_GE(latest.value->active_task_count, 1u);
  ASSERT_FALSE(latest.value->planned_node_sample.empty());
  EXPECT_EQ(latest.value->planned_node_sample.front().value, 1);
  ASSERT_FALSE(latest.value->task_sample.empty());
  EXPECT_EQ(latest.value->task_sample.front().node.value, 1);
  EXPECT_FALSE(latest.value->task_sample.front().kind.empty());

  auto history = host->recent_compute_planning_snapshots(session);
  ASSERT_TRUE(history.status.ok) << history.status.message;
  ASSERT_FALSE(history.value.empty());
  EXPECT_EQ(history.value.back().target_node.value, 1);

  auto missing = host->compute_planning_snapshot(GraphSessionId{"missing"});
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);
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
  EXPECT_EQ(checked_graph_error_code(ids_after_close.status),
            GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, AsyncComputeRejectsNewWorkWhileCloseIsWaiting) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "async_close_gate_graph", "slow_source", 250);
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;

  auto initial_async = host->compute_async(request);
  ASSERT_TRUE(initial_async.status.ok) << initial_async.status.message;

  auto close_future = std::async(std::launch::async, [&host, session]() {
    return host->close_graph(session);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_EQ(close_future.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  auto rejected_async = host->compute_async(request);
  EXPECT_FALSE(rejected_async.status.ok);
  EXPECT_EQ(checked_graph_error_code(rejected_async.status),
            GraphErrc::NotFound);

  OperationStatus initial_status = initial_async.value.get();
  EXPECT_TRUE(initial_status.ok) << initial_status.message;

  auto close = close_future.get();
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

TEST(EmbeddedHostAdapter, AsyncComputeFailureStatusSurvivesCloseGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_failure_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest missing_op_load;
  missing_op_load.session = GraphSessionId{"async_missing_op_graph"};
  missing_op_load.root_dir = (temp.root() / "sessions").string();
  missing_op_load.yaml_path =
      (temp.root() / "source" / "async_missing_op_graph.yaml").string();
  missing_op_load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(missing_op_load.yaml_path);
  auto loaded_missing_op = host->load_graph(missing_op_load);
  ASSERT_TRUE(loaded_missing_op.status.ok) << loaded_missing_op.status.message;

  auto async_compute =
      host->compute_async(make_compute_request(missing_op_load.session));
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;

  auto close = host->close_graph(missing_op_load.session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  OperationStatus async_status = async_compute.value.get();
  EXPECT_FALSE(async_status.ok);
  EXPECT_EQ(checked_graph_error_code(async_status), GraphErrc::NoOperation);
  EXPECT_NE(async_status.message.find("No op"), std::string::npos);

  auto closed_error = host->last_error(missing_op_load.session);
  EXPECT_TRUE(closed_error.ok) << closed_error.message;
}

TEST(EmbeddedHostAdapter, SyncComputePropagatesNodeExecutionBadAlloc) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_bad_alloc_sync");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "bad_alloc_sync", "resource_exhausted");
  const HostComputeRequest request = make_compute_request(session);

  try {
    const VoidResult result = host->compute(request);
    FAIL() << "std::bad_alloc was converted to Host status: code="
           << static_cast<int>(result.status.code)
           << " message=" << result.status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
}

TEST(EmbeddedHostAdapter, AsyncComputeFuturePropagatesNodeExecutionBadAlloc) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_bad_alloc_async");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "bad_alloc_async", "resource_exhausted");
  auto scheduled = host->compute_async(make_compute_request(session));
  ASSERT_TRUE(scheduled.status.ok) << scheduled.status.message;
  ASSERT_TRUE(scheduled.value.valid());

  try {
    const OperationStatus status = scheduled.value.get();
    FAIL() << "std::bad_alloc was converted by async Host path: code="
           << static_cast<int>(status.code) << " message=" << status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
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
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);
  auto missing_image = host->compute_and_get_image(missing_request);
  EXPECT_FALSE(missing_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_image.status),
            GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_compute_graph");
  HostComputeRequest closed_request = make_compute_request(session);
  auto initial = host->compute(closed_request);
  ASSERT_TRUE(initial.status.ok) << initial.status.message;

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->compute(closed_request);
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed.status), GraphErrc::NotFound);
  auto closed_image = host->compute_and_get_image(closed_request);
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, CloseGraphClearsStaleLastErrorBeforeImageCompute) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_clears_error_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_clears_error_graph");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};

  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  ASSERT_FALSE(missing_node_image.status.ok);
  ASSERT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);

  auto stale_error = host->last_error(session);
  ASSERT_FALSE(stale_error.ok);
  ASSERT_EQ(checked_graph_error_code(stale_error), GraphErrc::NotFound);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed_error = host->last_error(session);
  EXPECT_TRUE(closed_error.ok) << closed_error.message;

  auto closed_image =
      host->compute_and_get_image(make_compute_request(session));
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
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
  EXPECT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_image.status.message.empty());

  auto missing_node_error = host->last_error(session);
  EXPECT_FALSE(missing_node_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_error), GraphErrc::NotFound);
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
  EXPECT_EQ(checked_graph_error_code(missing_op_image.status),
            GraphErrc::NoOperation);
  EXPECT_NE(missing_op_image.status.message.find("No op"), std::string::npos);

  auto missing_op_error = host->last_error(missing_op_load.session);
  EXPECT_FALSE(missing_op_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_op_error), GraphErrc::NoOperation);
  EXPECT_NE(missing_op_error.message.find("No op"), std::string::npos);
}

TEST(EmbeddedHostAdapter, ComputeImagePreservesSuccessfulEmptyOutput) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_empty_image_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "empty_image_graph", "no_image");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};
  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  ASSERT_FALSE(missing_node_image.status.ok);
  ASSERT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);

  auto empty_image = host->compute_and_get_image(make_compute_request(session));
  ASSERT_TRUE(empty_image.status.ok) << empty_image.status.message;
  EXPECT_EQ(empty_image.value.width, 0);
  EXPECT_EQ(empty_image.value.height, 0);
  EXPECT_EQ(empty_image.value.data, nullptr);

  auto cleared_error = host->last_error(session);
  EXPECT_TRUE(cleared_error.ok) << cleared_error.message;

  auto closed = host->close_graph(session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;

  auto closed_image =
      host->compute_and_get_image(make_compute_request(session));
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
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
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_scheduler_graph");
  auto invalid_type = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "missing_scheduler_type");
  EXPECT_FALSE(invalid_type.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_type.status),
            GraphErrc::InvalidParameter);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed.status), GraphErrc::NotFound);
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
  EXPECT_EQ(checked_graph_error_code(missing_reload.status),
            GraphErrc::NotFound);

  const auto missing_file_path =
      temp.root() / "source" / "missing_reload_graph.yaml";
  auto io_reload = host->reload_graph(session, missing_file_path.string());
  EXPECT_FALSE(io_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(io_reload.status), GraphErrc::Io);

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
  EXPECT_EQ(checked_graph_error_code(invalid_reload.status),
            GraphErrc::InvalidYaml);

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
  EXPECT_EQ(checked_graph_error_code(missing_target.status),
            GraphErrc::InvalidParameter);
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
  EXPECT_EQ(checked_graph_error_code(missing_session_begin.status),
            GraphErrc::NotFound);

  auto missing_session_update =
      host->update_dirty_source(GraphSessionId{"missing_dirty_session"},
                                NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_update.status),
            GraphErrc::NotFound);

  auto missing_session_end =
      host->end_dirty_source(GraphSessionId{"missing_dirty_session"}, NodeId{1},
                             DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_session_end.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_end.status),
            GraphErrc::NotFound);

  auto missing_node_begin = host->begin_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_begin.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_begin.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_begin_error = host->last_error(session);
  EXPECT_FALSE(missing_node_begin_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_begin_error),
            GraphErrc::NotFound);

  auto missing_node_update = host->update_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_update.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_update.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_update_error = host->last_error(session);
  EXPECT_FALSE(missing_node_update_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_update_error),
            GraphErrc::NotFound);

  auto missing_node_end =
      host->end_dirty_source(session, NodeId{99}, DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_node_end.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_end.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_end.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_end_error = host->last_error(session);
  EXPECT_FALSE(missing_node_end_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_end_error),
            GraphErrc::NotFound);

  auto invalid_begin = host->begin_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_begin.status),
            GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_begin.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_begin_error = host->last_error(session);
  EXPECT_FALSE(invalid_begin_error.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_begin_error),
            GraphErrc::InvalidParameter);

  auto invalid_update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_update.status),
            GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_update.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_update_error = host->last_error(session);
  EXPECT_FALSE(invalid_update_error.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_update_error),
            GraphErrc::InvalidParameter);
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
  EXPECT_EQ(checked_graph_error_code(bad_load.status), GraphErrc::Io);

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

/**
 * @brief Proves every embedded Host shares one process plugin owner.
 *
 * @throws Nothing when public Host status mapping and fixture IO succeed.
 * @note One Host loads P1, another loads P2 and executes it, both loading Hosts
 *       are destroyed, and a third Host performs the global unload. The
 *       surviving and newly created Hosts must observe the same state.
 */
TEST(EmbeddedHostAdapter,
     OperationPluginsAreProcessGlobalAcrossHostDestructionAndUnload) {
  ScopedTempDir temp("photospider_host_process_plugin_owner_test");
  auto observer = create_embedded_host();
  ASSERT_NE(observer, nullptr);
  ScopedHostPluginCleanup cleanup(*observer);

  const auto original_dir = lifecycle_plugin_dir();
  const auto replacement_dir = override_lifecycle_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(original_dir));
  ASSERT_TRUE(std::filesystem::exists(replacement_dir));

  auto original_loader = create_embedded_host();
  ASSERT_NE(original_loader, nullptr);
  const auto original_report =
      original_loader->plugins_load_report({original_dir.string()});
  ASSERT_TRUE(original_report.status.ok) << original_report.status.message;
  ASSERT_EQ(original_report.value.loaded, 1);

  auto observer_sources = observer->ops_sources();
  ASSERT_TRUE(observer_sources.status.ok) << observer_sources.status.message;
  ASSERT_EQ(observer_sources.value.count("plugin_lifecycle:op"), 1u);

  GraphLoadRequest load_request;
  load_request.session = GraphSessionId{"process_plugin_graph"};
  load_request.root_dir = (temp.root() / "sessions").string();
  load_request.yaml_path = (temp.root() / "source" / "plugin.yaml").string();
  load_request.cache_root_dir = (temp.root() / "cache").string();
  write_lifecycle_plugin_graph(load_request.yaml_path);
  const auto loaded = observer->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest request = make_compute_request(load_request.session);
  request.cache.force_recache = true;
  auto computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto original_view = observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(original_view.status.ok) << original_view.status.message;
  ASSERT_TRUE(original_view.value.space.has_value());
  EXPECT_EQ(original_view.value.space->absolute_roi.width, 11);
  EXPECT_EQ(original_view.value.space->absolute_roi.height, 7);

  auto replacement_loader = create_embedded_host();
  ASSERT_NE(replacement_loader, nullptr);
  const auto replacement_report =
      replacement_loader->plugins_load_report({replacement_dir.string()});
  ASSERT_TRUE(replacement_report.status.ok)
      << replacement_report.status.message;
  ASSERT_EQ(replacement_report.value.loaded, 1);

  const auto repeated_seed = observer->seed_builtin_ops();
  ASSERT_TRUE(repeated_seed.status.ok) << repeated_seed.status.message;
  computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto replacement_view =
      observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(replacement_view.status.ok) << replacement_view.status.message;
  ASSERT_TRUE(replacement_view.value.space.has_value());
  EXPECT_EQ(replacement_view.value.space->absolute_roi.width, 22);
  EXPECT_EQ(replacement_view.value.space->absolute_roi.height, 9);

  original_loader.reset();
  replacement_loader.reset();
  computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto after_loader_destruction =
      observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(after_loader_destruction.status.ok)
      << after_loader_destruction.status.message;
  ASSERT_TRUE(after_loader_destruction.value.space.has_value());
  EXPECT_EQ(after_loader_destruction.value.space->absolute_roi.width, 22);
  EXPECT_EQ(after_loader_destruction.value.space->absolute_roi.height, 9);

  auto unloading_host = create_embedded_host();
  ASSERT_NE(unloading_host, nullptr);
  const auto unloaded = unloading_host->plugins_unload_all();
  ASSERT_TRUE(unloaded.status.ok) << unloaded.status.message;
  EXPECT_EQ(unloaded.value, 2);

  observer_sources = observer->ops_sources();
  ASSERT_TRUE(observer_sources.status.ok) << observer_sources.status.message;
  EXPECT_EQ(observer_sources.value.count("plugin_lifecycle:op"), 0u);
  const auto unloading_sources = unloading_host->ops_sources();
  ASSERT_TRUE(unloading_sources.status.ok) << unloading_sources.status.message;
  EXPECT_EQ(unloading_sources.value.count("plugin_lifecycle:op"), 0u);

  auto fresh_host = create_embedded_host();
  ASSERT_NE(fresh_host, nullptr);
  const auto fresh_sources = fresh_host->ops_sources();
  ASSERT_TRUE(fresh_sources.status.ok) << fresh_sources.status.message;
  EXPECT_EQ(fresh_sources.value.count("plugin_lifecycle:op"), 0u);
}

}  // namespace
}  // namespace ps
