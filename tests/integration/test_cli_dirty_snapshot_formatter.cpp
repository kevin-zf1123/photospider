#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/dense_image_processing.hpp"
#include "core/param_utils.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"
#include "graph_cli/dependency_tree_formatter.hpp"
#include "graph_cli/print_repl_help.hpp"
#include "photospider/data/image_view.hpp"
#include "providers/configured_image_artifact_codec.hpp"  // NOLINT(build/include_subdir)
#include "providers/configured_operation_providers.hpp"  // NOLINT(build/include_subdir)
#include "support/scoped_test_resources.hpp"

namespace ps::cli {
namespace {

using test_support::ScopedStreamBufferRedirect;
using test_support::ScopedTempDir;

/**
 * @brief Publishes one constant tightly packed FP32 ordinary image Value.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param sample Scalar copied to every channel element.
 * @param sample_domain Explicit uniform sample declaration.
 * @return Fresh immutable single-channel CPU image Value.
 * @throws std::invalid_argument or std::overflow_error when the requested
 * image cannot be represented.
 * @throws std::bad_alloc when metadata or payload allocation fails.
 * @note The helper performs no implicit sample-domain conversion.
 */
Value make_cli_float_image(std::size_t width, std::size_t height, float sample,
                           SampleDomain sample_domain = SampleDomain{
                               SampleDomainKind::Normalized, 0.0, 1.0}) {
  std::vector<float> samples(width * height, sample);
  std::vector<std::byte> storage(samples.size() * sizeof(float));
  std::memcpy(storage.data(), samples.data(), storage.size());
  DenseTensorDescriptor descriptor{{height, width, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.display_window = facet.data_window;
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::Normalized},
                        sample_domain,
                        {}};
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(width * sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage));
}

/**
 * @brief Registers deterministic operations used by CLI command tests.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry storage allocation fails.
 * @throws Any registry exception unchanged; std::call_once retries a later
 *         invocation when registration does not complete.
 * @note Registration is process-wide and thread-safe through std::call_once;
 *       the callbacks remain in the process-global registry until shutdown.
 * @note The test operations are monolithic and CPU-only. Dirty planning emits
 *       a monolithic dirty-region record for inspect tests, while the
 *       empty-output op lets save-command tests exercise successful computes
 *       with an explicitly declared empty output schema. The fixed-offset
 *       dirty propagator ignores optional execution-time inputs because its
 *       mapping depends only on the requested ROI and remains identical during
 *       planning and execution.
 */
void register_cli_command_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "cli_dirty_test", "source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const int width = as_int_flexible(node.parameters, "width", 256);
          const int height = as_int_flexible(node.parameters, "height", 128);
          NodeOutput output;
          output.publish_image_value(
              make_cli_float_image(static_cast<std::size_t>(width),
                                   static_cast<std::size_t>(height), 0.25F));
          output.space.absolute_roi = PixelRect{0, 0, width, height};
          output.debug.compute_device = "cli-dirty-test-source";
          return output;
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "cli_dirty_test", "legal_source",
        MonolithicOpFunc([](const Node&,
                            const std::vector<const NodeOutput*>&) {
          NodeOutput output;
          output.publish_image_value(make_cli_float_image(
              2U, 1U, 0.75F, SampleDomain{SampleDomainKind::Legal, -1.0, 1.0}));
          return output;
        }));
    const DirtyRoiPropFunc offset_dirty(
        [](const Node&, const PixelRect& roi, const GraphModel&,
           const PixelSize&, const std::vector<PixelSize>&,
           const plugin::ParameterMap&,
           const std::vector<const NodeOutput*>* available_inputs) {
          (void)available_inputs;
          return PixelRect{roi.x + 64, roi.y, roi.width, roi.height};
        });
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
              output.publish_image_value(
                  dense_image_processing::clone(input.image_value()));
              output.space.absolute_roi = input.space.absolute_roi;
              output.debug.compute_device = "cli-dirty-test-offset-identity";
              return output;
            }),
        {}, OpPlanningCallbacks{offset_dirty, {}, {}});
    OpRegistry::instance().register_dirty_propagator(
        "cli_dirty_test", "offset_identity", offset_dirty);
    OpMetadata empty_output_metadata;
    empty_output_metadata.produces_image = false;
    OpRegistry::instance().register_op_hp_monolithic(
        "cli_dirty_test", "empty_output",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }),
        empty_output_metadata);
  });
}

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
 * @note The graph exercises the Host named-Value compute path where Kernel
 *       returns an empty output set without LastError.
 */
void write_empty_output_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: empty_output\n"
      << "  type: cli_dirty_test\n"
      << "  subtype: empty_output\n";
}

/**
 * @brief Writes constants at and beyond the normalized legal-domain endpoints.
 *
 * @param path YAML file path to create.
 * @return Nothing after the complete graph fixture has been written.
 * @throws std::filesystem::filesystem_error if parent-directory creation
 *         fails.
 * @throws std::ios_base::failure if the destination cannot be opened or a
 *         YAML write fails.
 * @throws std::bad_alloc if path, stream, or YAML text construction exhausts
 *         memory.
 * @note Nodes 1 and 2 produce the normalized endpoints through integer values
 *       0 and 255. Nodes 3 and 4 use adjacent integers -1 and 256, which the
 *       maintained producer accepts as out-of-domain payload samples. Every
 *       case is integral, so non-finite floating input is outside this
 *       compatibility regression.
 */
void write_constant_boundary_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: constant_zero\n"
      << "  type: image_generator\n"
      << "  subtype: constant\n"
      << "  parameters: {width: 2, height: 1, channels: 1, value: 0}\n"
      << "- id: 2\n"
      << "  name: constant_255\n"
      << "  type: image_generator\n"
      << "  subtype: constant\n"
      << "  parameters: {width: 2, height: 1, channels: 1, value: 255}\n"
      << "- id: 3\n"
      << "  name: constant_negative\n"
      << "  type: image_generator\n"
      << "  subtype: constant\n"
      << "  parameters: {width: 2, height: 1, channels: 1, value: -1}\n"
      << "- id: 4\n"
      << "  name: constant_overflow\n"
      << "  type: image_generator\n"
      << "  subtype: constant\n"
      << "  parameters: {width: 2, height: 1, channels: 1, value: 256}\n";
}

/**
 * @brief Writes a source-only graph using the real coordinate-pattern op.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error if parent-directory creation
 *         fails.
 * @throws std::ios_base::failure if the destination cannot be opened or a
 *         YAML write fails.
 * @note The `3x2x3` FP32 output contains exact normalized byte fractions and
 *       reaches the production OpenCV provider through embedded Host compute.
 * @note Stream exceptions are enabled before `open` so an invalid destination
 *       cannot be mistaken for a successfully written fixture.
 */
void write_coordinate_pattern_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: coordinate_pattern\n"
      << "  type: image_generator\n"
      << "  subtype: coordinate_pattern\n"
      << "  parameters:\n"
      << "    width: 3\n"
      << "    height: 2\n"
      << "    channels: 3\n"
      << "    seed: 0\n";
}

/**
 * @brief Writes a source-only graph using the real CPU Perlin operation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error if parent-directory creation
 *         fails.
 * @throws std::ios_base::failure if the destination cannot be opened or a
 *         YAML write fails.
 * @note The deterministic `8x8` FP32 output reaches the configured OpenCV
 *       provider and has producer-declared normalized `[0,1]` sample meaning.
 */
void write_perlin_noise_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: perlin_noise\n"
      << "  type: image_generator\n"
      << "  subtype: perlin_noise\n"
      << "  parameters:\n"
      << "    width: 8\n"
      << "    height: 8\n"
      << "    grid_size: 3.0\n"
      << "    seed: 84\n";
}

/**
 * @brief Writes a same-channel blend whose two sources disagree on samples.
 *
 * @param path YAML file path to create.
 * @return Nothing after the complete graph fixture has been written.
 * @throws std::filesystem::filesystem_error if parent-directory creation
 *         fails.
 * @throws std::ios_base::failure if the destination cannot be opened or a
 *         YAML write fails.
 * @throws std::bad_alloc if path, stream, or YAML text construction exhausts
 *         memory.
 * @note Node 1 is the configured normalized constant producer, node 2 is a
 *       deterministic Legal `[-1,1]` test source, and node 3 reaches the real
 *       configured OpenCV weighted-blend candidate without channel mapping.
 */
void write_incompatible_weighted_blend_graph(
    const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: normalized_source\n"
      << "  type: image_generator\n"
      << "  subtype: constant\n"
      << "  parameters: {width: 2, height: 1, channels: 1, value: 64}\n"
      << "- id: 2\n"
      << "  name: legal_source\n"
      << "  type: cli_dirty_test\n"
      << "  subtype: legal_source\n"
      << "- id: 3\n"
      << "  name: incompatible_weighted_blend\n"
      << "  type: image_mixing\n"
      << "  subtype: add_weighted\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n"
      << "    - from_node_id: 2\n"
      << "  parameters: {alpha: 0.5, beta: 0.5, gamma: 0.0, "
         "merge_strategy: resize}\n";
}

/**
 * @brief Rejects a directory destination for coordinate-pattern graph YAML.
 *
 * @return Nothing; GoogleTest reports a missing I/O exception or a destination
 *         that masquerades as a regular output file.
 * @throws std::filesystem::filesystem_error if temporary-directory setup or
 *         inspection fails unexpectedly.
 * @note An existing directory is a deterministic invalid `std::ofstream`
 *       target on Darwin and Linux and avoids permission-bit assumptions.
 */
TEST(CliSaveCommand, CoordinatePatternGraphWriterReportsFileCreationFailures) {
  ScopedTempDir temp("photospider_cli_coordinate_graph_io_failure_test");

  EXPECT_THROW(write_coordinate_pattern_graph(temp.root()),
               std::ios_base::failure);
  EXPECT_TRUE(std::filesystem::is_directory(temp.root()));
  EXPECT_FALSE(std::filesystem::is_regular_file(temp.root()));
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

/**
 * @brief Proves the main REPL help lists every supported save storage token.
 * @return Nothing; GoogleTest reports a missing canonical save signature.
 * @throws Nothing; the default configuration and stdout capture remain local
 *         to this test body.
 * @note This locks the overview syntax only. The detailed backend/format
 *       matrix remains authoritative in the dedicated `help save` resource.
 */
TEST(CliHelpResources, MainReplHelpListsEverySaveStorageChoice) {
  CliConfig config;

  testing::internal::CaptureStdout();
  ::print_repl_help(config);
  const std::string output = testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("save <id> <output> <file> "
                        "<uint8|uint16|uint32|fp32>"),
            std::string::npos);
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
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_inspect(args, *host, current_graph, modified, config);
  }

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
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_inspect(args, *host, current_graph, modified, config);
  }

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

  std::istringstream args(
      "1 image /tmp/photospider_empty_output.png uint8 code code 0 255 clamp "
      "nearest-even reject allow");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Named output 'image' is absent or is not an ordinary "
                      "image."),
            std::string::npos);
  EXPECT_EQ(text.find("Failed to compute node 1."), std::string::npos);
  EXPECT_EQ(text.find("Reason:"), std::string::npos);
}

TEST(CliSaveCommand, SavesWithExplicitDestinationSampleSemantics) {
  register_cli_command_ops();
  ScopedTempDir temp("photospider_cli_save_explicit_sample_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "save_graph.yaml";
  const auto output_path = temp.root() / "saved.png";
  write_dirty_inspect_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_save_explicit_sample"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  std::istringstream args(
      "1 image " + output_path.string() +
      " uint8 code code 0 255 clamp nearest-even reject allow");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Saved named output 'image'"), std::string::npos) << text;
  ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << text;
  EXPECT_GT(std::filesystem::file_size(output_path), 0U);
}

/**
 * @brief Saves both inclusive constant byte-code boundaries through the real
 * configured provider, Host, CLI, and PNG codec.
 *
 * @return Nothing; GoogleTest reports metadata, conversion, file, or decoded
 *         code-value disagreement.
 * @throws Host, filesystem, codec, and allocation exceptions unchanged to the
 *         test runner outside the command's maintained diagnostic boundary.
 * @note `NamedValueResult::inspect()` verifies the source SampleDomain without
 *       acquiring payload authority. The subsequent save and decode prove the
 *       same configured result reaches the real codec at both endpoints.
 */
TEST(CliSaveCommand, SavesConstantByteBoundariesThroughConfiguredCodec) {
  providers::register_configured_operation_providers();
  ScopedTempDir temp("photospider_cli_save_constant_boundaries_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "constant_graph.yaml";
  write_constant_boundary_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_save_constant_boundaries"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  const auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const SampleEndpoint code_samples{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}};
  const std::array<std::pair<int, std::uint8_t>, 2U> cases{
      {{1, 0U}, {2, 255U}}};
  for (const auto& [node_id, expected_code] : cases) {
    HostComputeRequest compute_request;
    compute_request.session = request.session;
    compute_request.node = NodeId{node_id};
    compute_request.cache.precision = "fp32";
    const Result<NamedValueResult> computed =
        host->compute_and_get_values(compute_request);
    ASSERT_TRUE(computed.status.ok) << computed.status.message;

    const std::vector<NamedValueInspection> inspections =
        computed.value.inspect();
    ASSERT_EQ(inspections.size(), 1U);
    EXPECT_EQ(inspections.front().name, "image");
    ASSERT_TRUE(inspections.front().dense_descriptor.has_value());
    EXPECT_EQ(inspections.front().dense_descriptor->element_semantics,
              ElementSemantics::FloatingPoint);
    EXPECT_EQ(inspections.front().dense_descriptor->storage_encoding,
              (StorageEncoding{32U}));
    ASSERT_TRUE(inspections.front().image_facet.has_value());
    const auto& sample_domain = inspections.front().image_facet->sample_domain;
    EXPECT_TRUE(sample_domain.has_value());
    if (sample_domain.has_value()) {
      EXPECT_TRUE(sample_domain->per_channel.empty());
      EXPECT_EQ(sample_domain->encoding,
                (SampleEncoding{1U, SampleEncodingKind::Normalized}));
      EXPECT_EQ(sample_domain->default_domain,
                (SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}));
    }

    const auto output_path =
        temp.root() / ("constant-" + std::to_string(expected_code) + ".png");
    std::istringstream args(
        std::to_string(node_id) + " image " + output_path.string() +
        " uint8 code code 0 255 reject nearest-even reject allow");
    std::string current_graph = request.session.value;
    bool modified = false;
    CliConfig config;
    std::ostringstream captured;
    bool handled = false;
    {
      ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
      handled = ::handle_save(args, *host, current_graph, modified, config);
    }

    const std::string text = captured.str();
    EXPECT_TRUE(handled);
    EXPECT_NE(text.find("Saved named output 'image'"), std::string::npos)
        << text;
    ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << text;

    const Value decoded =
        providers::make_configured_image_artifact_codec()->decode(
            output_path,
            {{ImageArtifactDecodeRule{ElementSemantics::UnsignedInteger,
                                      StorageEncoding{8U}, code_samples,
                                      std::nullopt}}});
    const ImageView view(decoded);
    ASSERT_EQ(view.width(), 2U);
    ASSERT_EQ(view.height(), 1U);
    ASSERT_EQ(view.channels(), 1U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(0U, 0U, 0U)),
              expected_code);
    EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(1U, 0U, 0U)),
              expected_code);
  }
}

/**
 * @brief Preserves out-of-domain constants and requires explicit save policy.
 *
 * @return Nothing; GoogleTest reports compute, descriptor, payload, conversion,
 *         file, or decoded endpoint disagreement.
 * @throws Host, filesystem, codec, stream, and allocation exceptions unchanged
 *         to the test runner outside the command's maintained diagnostic
 *         boundary.
 * @note The producer remains compatible with integer values -1 and 256. Their
 *       payloads lie beyond the declared Normalized `[0,1]` legal interval,
 *       proving the facet is not inferred from actual minima/maxima. CLI
 *       `reject` fails closed without creating a file, while an explicit
 *       `clamp` request writes the corresponding code-value endpoint.
 */
TEST(CliSaveCommand, ConstantOutOfDomainValuesRequireExplicitSavePolicy) {
  providers::register_configured_operation_providers();
  ScopedTempDir temp("photospider_cli_constant_out_of_range_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "constant_graph.yaml";
  write_constant_boundary_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_constant_out_of_range"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  const auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const SampleEndpoint code_samples{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}};
  const std::array<std::pair<int, int>, 2U> cases{{{3, -1}, {4, 256}}};
  for (const auto& [node_id, value_int] : cases) {
    HostComputeRequest compute_request;
    compute_request.session = request.session;
    compute_request.node = NodeId{node_id};
    compute_request.cache.precision = "fp32";
    const Result<NamedValueResult> computed =
        host->compute_and_get_values(compute_request);
    ASSERT_TRUE(computed.status.ok) << computed.status.message;

    const std::vector<NamedValueInspection> inspections =
        computed.value.inspect();
    ASSERT_EQ(inspections.size(), 1U);
    ASSERT_TRUE(inspections.front().dense_descriptor.has_value());
    EXPECT_EQ(inspections.front().dense_descriptor->shape,
              (std::vector<std::size_t>{1U, 2U, 1U}));
    EXPECT_EQ(inspections.front().dense_descriptor->element_semantics,
              ElementSemantics::FloatingPoint);
    EXPECT_EQ(inspections.front().dense_descriptor->storage_encoding,
              (StorageEncoding{32U}));
    ASSERT_TRUE(inspections.front().image_facet.has_value());
    ASSERT_TRUE(inspections.front().image_facet->sample_domain.has_value());
    const SampleDomainFacet& source_samples =
        *inspections.front().image_facet->sample_domain;
    EXPECT_TRUE(source_samples.per_channel.empty());
    EXPECT_EQ(source_samples.encoding,
              (SampleEncoding{1U, SampleEncodingKind::Normalized}));
    EXPECT_EQ(source_samples.default_domain,
              (SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}));

    const Value* source = computed.value.find("image");
    ASSERT_NE(source, nullptr);
    const ImageView source_view(*source);
    ASSERT_EQ(source_view.width(), 2U);
    ASSERT_EQ(source_view.height(), 1U);
    ASSERT_EQ(source_view.channels(), 1U);
    float source_sample = 0.0F;
    std::memcpy(&source_sample, source_view.channel_data(0U, 0U, 0U),
                sizeof(source_sample));
    EXPECT_FLOAT_EQ(source_sample, static_cast<float>(value_int) / 255.0F);

    const auto reject_path =
        temp.root() / ("rejected-constant-" + std::to_string(node_id) + ".png");
    EXPECT_FALSE(std::filesystem::exists(reject_path));
    std::istringstream reject_args(
        std::to_string(node_id) + " image " + reject_path.string() +
        " uint8 code code 0 255 reject nearest-even reject allow");
    std::string current_graph = request.session.value;
    bool modified = false;
    CliConfig config;
    std::ostringstream captured;
    bool handled = false;
    {
      ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
      handled =
          ::handle_save(reject_args, *host, current_graph, modified, config);
    }

    const std::string text = captured.str();
    EXPECT_TRUE(handled);
    EXPECT_NE(text.find("Failed to save image:"), std::string::npos) << text;
    EXPECT_NE(text.find("out-of-domain"), std::string::npos) << text;
    EXPECT_FALSE(std::filesystem::exists(reject_path));

    const auto clamp_path =
        temp.root() / ("clamped-constant-" + std::to_string(node_id) + ".png");
    std::istringstream clamp_args(
        std::to_string(node_id) + " image " + clamp_path.string() +
        " uint8 code code 0 255 clamp nearest-even reject allow");
    std::ostringstream clamp_captured;
    bool clamp_handled = false;
    {
      ScopedStreamBufferRedirect redirect(std::cout, clamp_captured.rdbuf());
      clamp_handled =
          ::handle_save(clamp_args, *host, current_graph, modified, config);
    }
    const std::string clamp_text = clamp_captured.str();
    EXPECT_TRUE(clamp_handled);
    EXPECT_NE(clamp_text.find("Saved named output 'image'"), std::string::npos)
        << clamp_text;
    ASSERT_TRUE(std::filesystem::is_regular_file(clamp_path)) << clamp_text;

    const Value decoded =
        providers::make_configured_image_artifact_codec()->decode(
            clamp_path, {{ImageArtifactDecodeRule{
                            ElementSemantics::UnsignedInteger,
                            StorageEncoding{8U}, code_samples, std::nullopt}}});
    const ImageView decoded_view(decoded);
    ASSERT_EQ(decoded_view.width(), 2U);
    ASSERT_EQ(decoded_view.height(), 1U);
    ASSERT_EQ(decoded_view.channels(), 1U);
    const std::uint8_t expected_code = value_int < 0 ? 0U : 255U;
    EXPECT_EQ(
        std::to_integer<std::uint8_t>(*decoded_view.channel_data(0U, 0U, 0U)),
        expected_code);
    EXPECT_EQ(
        std::to_integer<std::uint8_t>(*decoded_view.channel_data(1U, 0U, 0U)),
        expected_code);
  }
}

/**
 * @brief Saves the real coordinate-pattern Value through CLI and codec seams.
 *
 * @return Nothing; GoogleTest reports compute, conversion, file, or decode
 *         failures.
 * @throws Host, filesystem, codec, and allocation exceptions unchanged to the
 *         test runner outside the command's maintained diagnostic boundary.
 * @note Success proves `make_encode_request` consumed the source's explicit
 *       normalized `[0,1]` endpoint. Decoding the emitted PNG then proves the
 *       conversion reached the real codec instead of stopping at metadata
 *       inspection or a mock encoder.
 */
TEST(CliSaveCommand, SavesCoordinatePatternThroughConfiguredCodec) {
  providers::register_configured_operation_providers();
  ScopedTempDir temp("photospider_cli_save_coordinate_pattern_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path =
      temp.root() / "source" / "coordinate_pattern_graph.yaml";
  const auto output_path = temp.root() / "coordinate-pattern.png";
  write_coordinate_pattern_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_save_coordinate_pattern"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  std::istringstream args(
      "1 image " + output_path.string() +
      " uint8 code code 0 255 reject nearest-even reject allow");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Saved named output 'image'"), std::string::npos) << text;
  ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << text;

  const SampleEndpoint code_samples{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}};
  const Value decoded =
      providers::make_configured_image_artifact_codec()->decode(
          output_path, {{ImageArtifactDecodeRule{
                           ElementSemantics::UnsignedInteger,
                           StorageEncoding{8U}, code_samples, std::nullopt}}});
  const ImageView view(decoded);
  ASSERT_EQ(view.width(), 3U);
  ASSERT_EQ(view.height(), 2U);
  ASSERT_EQ(view.channels(), 3U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(0U, 0U, 0U)), 0U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(0U, 0U, 1U)), 47U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(1U, 0U, 0U)), 17U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(0U, 1U, 0U)), 31U);
}

/**
 * @brief Saves the real CPU Perlin Value through CLI and configured codec.
 *
 * @return Nothing; GoogleTest reports provider, metadata, conversion, file, or
 *         decode failures.
 * @throws Host, filesystem, codec, and allocation exceptions unchanged to the
 *         test runner outside the command's maintained diagnostic boundary.
 * @note The first compute inspects the exact configured-provider Value. The
 *       save then proves `make_encode_request` consumes that same explicit
 *       normalized `[0,1]` endpoint before the real PNG codec round trip.
 */
TEST(CliSaveCommand, SavesPerlinNoiseThroughConfiguredCodec) {
  providers::register_configured_operation_providers();
  ScopedTempDir temp("photospider_cli_save_perlin_noise_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "perlin_noise_graph.yaml";
  const auto output_path = temp.root() / "perlin-noise.png";
  write_perlin_noise_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_save_perlin_noise"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  const auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest compute_request;
  compute_request.session = request.session;
  compute_request.node = NodeId{1};
  compute_request.cache.precision = "fp32";
  const Result<NamedValueResult> computed =
      host->compute_and_get_values(compute_request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  const Value* source = computed.value.find("image");
  ASSERT_NE(source, nullptr);
  ASSERT_TRUE(source->image_facet().has_value());
  ASSERT_TRUE(source->image_facet()->sample_domain.has_value());
  const SampleDomainFacet& samples = *source->image_facet()->sample_domain;
  EXPECT_TRUE(samples.per_channel.empty());
  EXPECT_EQ(samples.encoding,
            (SampleEncoding{1U, SampleEncodingKind::Normalized}));
  EXPECT_EQ(samples.default_domain,
            (SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}));

  std::istringstream args(
      "1 image " + output_path.string() +
      " uint8 code code 0 255 reject nearest-even reject allow");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Saved named output 'image'"), std::string::npos) << text;
  ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << text;

  const SampleEndpoint code_samples{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}};
  const Value decoded =
      providers::make_configured_image_artifact_codec()->decode(
          output_path, {{ImageArtifactDecodeRule{
                           ElementSemantics::UnsignedInteger,
                           StorageEncoding{8U}, code_samples, std::nullopt}}});
  const ImageView view(decoded);
  ASSERT_EQ(view.width(), 8U);
  ASSERT_EQ(view.height(), 8U);
  ASSERT_EQ(view.channels(), 1U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*view.channel_data(0U, 0U, 0U)),
            128U);
}

/**
 * @brief Proves an incompatible same-channel blend fails closed at CLI save.
 *
 * @return Nothing; GoogleTest reports provider, metadata, diagnostic, or
 *         filesystem disagreement.
 * @throws Host, filesystem, provider, stream, and allocation exceptions
 *         unchanged outside the command's maintained diagnostic boundary.
 * @note The configured weighted-blend candidate publishes raw FP32 pixels but
 *       no false common SampleDomain. Metadata-only Host inspection observes
 *       that absence, and the real save command refuses to invent a source
 *       endpoint or create a PNG despite an explicit destination policy.
 */
TEST(CliSaveCommand,
     IncompatibleWeightedBlendSampleDomainsFailClosedBeforeCodecWrite) {
  register_cli_command_ops();
  providers::register_configured_operation_providers();
  ScopedTempDir temp("photospider_cli_weighted_blend_sample_mismatch_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path =
      temp.root() / "source" / "weighted_blend_mismatch.yaml";
  const auto output_path = temp.root() / "weighted-blend-mismatch.png";
  write_incompatible_weighted_blend_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_weighted_blend_sample_mismatch"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  const auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest compute_request;
  compute_request.session = request.session;
  compute_request.node = NodeId{3};
  compute_request.cache.precision = "fp32";
  const Result<NamedValueResult> computed =
      host->compute_and_get_values(compute_request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  const Value* blend = computed.value.find("image");
  ASSERT_NE(blend, nullptr);
  ASSERT_TRUE(blend->image_facet().has_value());
  EXPECT_FALSE(blend->image_facet()->sample_domain.has_value());
  const ImageView blend_view(*blend);
  ASSERT_EQ(blend_view.width(), 2U);
  ASSERT_EQ(blend_view.height(), 1U);
  ASSERT_EQ(blend_view.channels(), 1U);

  EXPECT_FALSE(std::filesystem::exists(output_path));
  std::istringstream args(
      "3 image " + output_path.string() +
      " uint8 code code 0 255 reject nearest-even reject allow");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Failed to save image:"), std::string::npos) << text;
  EXPECT_NE(text.find("explicit default sample domain"), std::string::npos)
      << text;
  EXPECT_FALSE(std::filesystem::exists(output_path));
}

#if defined(PHOTOSPIDER_TEST_HAS_OPENEXR_DENSE)
TEST(CliSaveCommand, SavesOrdinaryOpenExrWithExplicitUint32AndFp32Policies) {
  register_cli_command_ops();
  ScopedTempDir temp("photospider_cli_save_openexr_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "save_openexr_graph.yaml";
  const auto output_path = temp.root() / "saved.exr";
  const auto uint_output_path = temp.root() / "saved-uint.exr";
  write_dirty_inspect_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"cli_save_openexr"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();
  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  std::istringstream args(
      "1 image " + output_path.string() +
      " fp32 normalized normalized 0 1 reject nearest-even reject reject");
  std::string current_graph = request.session.value;
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Saved named output 'image'"), std::string::npos) << text;
  ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << text;

  const SampleEndpoint samples{
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}};
  const Value decoded =
      providers::make_configured_image_artifact_codec()->decode(
          output_path, {{ImageArtifactDecodeRule{
                           ElementSemantics::FloatingPoint,
                           StorageEncoding{32U}, samples, std::nullopt}}});
  const ImageView view(decoded);
  float sample = 0.0F;
  std::memcpy(&sample, view.channel_data(0U, 0U, 0U), sizeof(sample));
  EXPECT_FLOAT_EQ(sample, 0.25F);
  EXPECT_EQ(decoded.image_facet()->data_window,
            *decoded.image_facet()->display_window);

  std::istringstream uint_args(
      "1 image " + uint_output_path.string() +
      " uint32 code code 0 4294967295 reject nearest-even reject allow");
  std::ostringstream uint_captured;
  bool uint_handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, uint_captured.rdbuf());
    uint_handled =
        ::handle_save(uint_args, *host, current_graph, modified, config);
  }
  EXPECT_TRUE(uint_handled);
  EXPECT_NE(uint_captured.str().find("Saved named output 'image'"),
            std::string::npos)
      << uint_captured.str();
  ASSERT_TRUE(std::filesystem::is_regular_file(uint_output_path));

  const SampleEndpoint uint_samples{
      SampleEncoding{1U, SampleEncodingKind::CodeValue},
      SampleDomain{SampleDomainKind::CodeValue, 0.0, 4294967295.0}};
  const Value uint_decoded =
      providers::make_configured_image_artifact_codec()->decode(
          uint_output_path,
          {{ImageArtifactDecodeRule{ElementSemantics::UnsignedInteger,
                                    StorageEncoding{32U}, uint_samples,
                                    std::nullopt}}});
  const ImageView uint_view(uint_decoded);
  std::uint32_t uint_sample = 0U;
  std::memcpy(&uint_sample, uint_view.channel_data(0U, 0U, 0U),
              sizeof(uint_sample));
  EXPECT_EQ(uint_sample, 1073741824U);
}
#endif

TEST(CliSaveCommand, ReportsImageComputeFailures) {
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  std::istringstream args(
      "1 image /tmp/photospider_missing_output.png uint8 code code 0 255 "
      "clamp nearest-even reject allow");
  std::string current_graph = "missing_cli_graph";
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Failed to compute node 1."), std::string::npos);
  EXPECT_NE(text.find("Reason:"), std::string::npos);
  EXPECT_EQ(text.find("Named output 'image' is absent or is not an ordinary "
                      "image."),
            std::string::npos);
}

TEST(CliSaveCommand, RejectsImplicitDestinationSampleSemantics) {
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  std::istringstream args(
      "1 image /tmp/photospider_implicit_output.png uint8 clamp nearest-even "
      "reject allow");
  std::string current_graph = "missing_cli_graph";
  bool modified = false;
  CliConfig config;
  std::ostringstream captured;
  bool handled = false;
  {
    ScopedStreamBufferRedirect redirect(std::cout, captured.rdbuf());
    handled = ::handle_save(args, *host, current_graph, modified, config);
  }

  const std::string text = captured.str();
  EXPECT_TRUE(handled);
  EXPECT_NE(text.find("Usage: save <id> <output> <file> "
                      "<uint8|uint16|uint32|fp32>"),
            std::string::npos);
  EXPECT_EQ(text.find("Failed to compute node 1."), std::string::npos);
}

}  // namespace
}  // namespace ps::cli
