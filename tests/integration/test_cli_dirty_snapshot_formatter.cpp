#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "graph_cli/dependency_tree_formatter.hpp"

namespace ps::cli {
namespace {

/**
 * @brief Registers deterministic operations used by CLI command tests.
 *
 * @throws std::bad_alloc if registry storage allocation fails.
 * @note The test operations are monolithic and CPU-only. Dirty planning emits
 *       a monolithic dirty-region record for inspect tests, while the
 *       empty-output op lets save-command tests exercise successful computes
 *       with no image.
 */
void register_cli_command_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "cli_dirty_test", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const int width = node.parameters["width"].as<int>(256);
              const int height = node.parameters["height"].as<int>(128);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              toCvMat(output.image_buffer).setTo(3.0f);
              output.space.absolute_roi = cv::Rect(0, 0, width, height);
              output.debug.compute_device = "cli-dirty-test-source";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "cli_dirty_test", "offset_identity",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty() || inputs.front() == nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "cli dirty inspect requires one input");
              }
              const NodeOutput& input = *inputs.front();
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  input.image_buffer.width, input.image_buffer.height,
                  input.image_buffer.channels, input.image_buffer.type);
              toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
              output.space.absolute_roi = input.space.absolute_roi;
              output.debug.compute_device = "cli-dirty-test-offset-identity";
              return output;
            }));
    OpRegistry::instance().register_dirty_propagator(
        "cli_dirty_test", "offset_identity",
        DirtyRoiPropFunc([](const Node&, const cv::Rect& roi, const GraphModel&,
                            const cv::Size&, const std::vector<cv::Size>&,
                            const plugin::ParameterMap&) {
          return cv::Rect(roi.x + 64, roi.y, roi.width, roi.height);
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "cli_dirty_test", "empty_output",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }));
  });
}

/**
 * @brief Owns a unique temporary directory for one dirty inspect CLI test.
 *
 * @throws std::filesystem::filesystem_error if setup cleanup or directory
 *         creation fails.
 * @note Cleanup is best-effort so test failures are not masked by filesystem
 *       teardown errors.
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
 * @brief Temporarily changes the process working directory for one test scope.
 *
 * @throws std::filesystem::filesystem_error if the current path cannot be read
 *         or changed during construction.
 * @note Destruction restores the original directory with an error-code
 *       overload so cleanup never hides the test's primary failure.
 */
class ScopedCurrentPath {
 public:
  /**
   * @brief Saves the current directory and enters the requested directory.
   *
   * @param next Directory that should become current for the scope.
   * @throws std::filesystem::filesystem_error if either filesystem operation
   *         fails.
   */
  explicit ScopedCurrentPath(const std::filesystem::path& next)
      : original_(std::filesystem::current_path()) {
    std::filesystem::current_path(next);
  }

  /**
   * @brief Prevents duplicate ownership of one process-global path restore.
   *
   * @param other Source scope that cannot be copied.
   * @throws Nothing; this operation is deleted.
   */
  ScopedCurrentPath(const ScopedCurrentPath& other) = delete;

  /**
   * @brief Prevents replacing one process-global path restore obligation.
   *
   * @param other Source scope that cannot be assigned.
   * @return This object is never returned because the operation is deleted.
   * @throws Nothing; this operation is deleted.
   */
  ScopedCurrentPath& operator=(const ScopedCurrentPath& other) = delete;

  /**
   * @brief Restores the process working directory captured at construction.
   *
   * @throws Nothing.
   * @note Restore errors are intentionally ignored during test cleanup.
   */
  ~ScopedCurrentPath() noexcept {
    std::error_code error;
    std::filesystem::current_path(original_, error);
  }

 private:
  /** @brief Original directory restored when this scope ends. */
  std::filesystem::path original_;
};

/**
 * @brief Writes a two-node graph that emits non-empty dirty diagnostics.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 is monolithic and shifts backward dirty ROI demand, producing
 *       both monolithic dirty-region and edge-mapping diagnostics.
 */
void write_dirty_inspect_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: dirty_source\n"
      << "  type: cli_dirty_test\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 256\n"
      << "    height: 128\n"
      << "- id: 2\n"
      << "  name: dirty_offset_identity\n"
      << "  type: cli_dirty_test\n"
      << "  subtype: offset_identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Writes a single-node graph whose operation succeeds with no image.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note The graph exercises the Host image-compute path where Kernel returns
 *       nullopt without LastError to represent successful no-image output.
 */
void write_empty_output_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: empty_output\n"
      << "  type: cli_dirty_test\n"
      << "  subtype: empty_output\n";
}

TEST(CliDirtySnapshotFormatter, RendersMonolithicAndEdgeMappings) {
  DirtyRegionInspectionSnapshot snapshot;
  snapshot.graph_generation = 7;
  snapshot.dirty_monolithic_nodes.push_back(DirtyMonolithicRegionSnapshot{
      NodeId{2}, DirtyDomain::HighPrecision, PixelRect{0, 0, 8, 6}, true});
  snapshot.actual_dirty_rois[2].push_back(PixelRect{0, 0, 8, 6});
  snapshot.edge_mappings.push_back(DirtyEdgeMappingSnapshot{
      NodeId{1}, NodeId{2}, DirtyDomain::HighPrecision, PixelRect{0, 0, 8, 6},
      PixelRect{1, 1, 2, 2}, DirtyEdgeDirection::BackwardDemand});

  const std::string text = format_dirty_snapshot(snapshot);

  EXPECT_EQ(text.find("(No dirty snapshot recorded.)"), std::string::npos);
  EXPECT_NE(text.find("Monolithic dirty regions: 1"), std::string::npos);
  EXPECT_NE(text.find("node 2 hp whole=true roi=0,0 8x6"), std::string::npos);
  EXPECT_NE(text.find("Edge mappings: 1"), std::string::npos);
  EXPECT_NE(text.find("node 1 -> 2 hp backward-demand "
                      "from=[0,0 8x6] to=[1,1 2x2]"),
            std::string::npos);
}

TEST(CliHelpResources, LoadsConfiguredHelpOutsideRepositoryCwd) {
  ScopedTempDir temp("photospider_cli_help_resource_test");
  ScopedCurrentPath current_path(temp.root());

  testing::internal::CaptureStdout();
  ::print_help_from_file("help_compute.txt");
  const std::string output = testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("compute <id|all> [flags]"), std::string::npos);
  EXPECT_EQ(output.find("Help not available"), std::string::npos);
}

TEST(CliNodeInspectionFormatter, RendersLocalInverseMatrix) {
  NodeInspectionView node;
  node.id = NodeId{3};
  node.name = "spatial_node";
  node.type = "image_process";
  node.subtype = "crop";
  node.has_cached_output = true;
  node.source_label = std::string("computed");
  node.debug = DebugMetadataSnapshot{};
  node.space = SpatialSnapshot{};
  node.space->absolute_roi = PixelRect{4, 5, 6, 7};
  node.space->local_inverse_matrix[2] = 11.0;
  node.space->local_inverse_matrix[5] = 13.0;

  const std::string text = format_node_inspection(node);

  EXPECT_NE(text.find("Inverse (Global)"), std::string::npos);
  EXPECT_NE(text.find("Inverse (Local)"), std::string::npos);
  EXPECT_NE(text.find("11"), std::string::npos);
  EXPECT_NE(text.find("13"), std::string::npos);
}

TEST(CliDirtySnapshotFormatter,
     InspectDirtyCommandRendersNonEmptyHostSnapshot) {
  register_cli_command_ops();
  ScopedTempDir temp("photospider_cli_inspect_dirty_non_empty_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "dirty_inspect_graph.yaml";
  write_dirty_inspect_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_dirty_inspect"};
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

  std::istringstream args("dirty");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  auto* original_buffer = std::cout.rdbuf(captured.rdbuf());
  const bool handled =
      ::handle_inspect(args, *host, current_graph, modified, config);
  std::cout.rdbuf(original_buffer);

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_EQ(text.find("(No dirty snapshot recorded.)"), std::string::npos);
  EXPECT_NE(text.find("Monolithic dirty regions:"), std::string::npos);
  EXPECT_NE(text.find("node 2 hp whole=true roi=0,0 256x128"),
            std::string::npos);
  EXPECT_NE(text.find("Edge mappings: 1"), std::string::npos);
  EXPECT_NE(text.find("node 1 -> 2 hp backward-demand "
                      "from=[64,0 128x64] to=[64,0 64x64]"),
            std::string::npos);
}

TEST(CliDirtySnapshotFormatter, InspectDirtyReportsHostFailures) {
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  std::istringstream args("dirty");
  std::string current_graph = "missing_cli_graph";
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  auto* original_buffer = std::cout.rdbuf(captured.rdbuf());
  const bool handled =
      ::handle_inspect(args, *host, current_graph, modified, config);
  std::cout.rdbuf(original_buffer);

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Unable to inspect dirty regions for graph "
                      "'missing_cli_graph'."),
            std::string::npos);
  EXPECT_NE(text.find("Reason:"), std::string::npos);
  EXPECT_EQ(text.find("(No dirty snapshot recorded.)"), std::string::npos);
}

TEST(CliSaveCommand, ReportsSuccessfulEmptyImageOutputs) {
  register_cli_command_ops();
  ScopedTempDir temp("photospider_cli_save_empty_output_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "empty_output_graph.yaml";
  write_empty_output_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_save_empty_output"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  std::istringstream args("1 /tmp/photospider_empty_output.png");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  auto* original_buffer = std::cout.rdbuf(captured.rdbuf());
  const bool handled =
      ::handle_save(args, *host, current_graph, modified, config);
  std::cout.rdbuf(original_buffer);

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("No image to save (node produced no CPU image)."),
            std::string::npos);
  EXPECT_EQ(text.find("Failed to compute image for node 1."),
            std::string::npos);
  EXPECT_EQ(text.find("Reason:"), std::string::npos);
}

TEST(CliSaveCommand, ReportsImageComputeFailures) {
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  std::istringstream args("1 /tmp/photospider_missing_output.png");
  std::string current_graph = "missing_cli_graph";
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  auto* original_buffer = std::cout.rdbuf(captured.rdbuf());
  const bool handled =
      ::handle_save(args, *host, current_graph, modified, config);
  std::cout.rdbuf(original_buffer);

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Failed to compute image for node 1."),
            std::string::npos);
  EXPECT_NE(text.find("Reason:"), std::string::npos);
  EXPECT_EQ(text.find("No image to save (node produced no image)."),
            std::string::npos);
}

}  // namespace
}  // namespace ps::cli
