#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "adapters/yaml/parameter_value_yaml.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/dirty/dirty_region_planner.hpp"
#include "compute/dirty/dirty_region_snapshot_builder.hpp"
#include "compute/dirty/tiled_input_normalizer.hpp"
#include "compute/dispatch/task_graph_planning.hpp"
#include "compute/image_buffer.hpp"
#include "core/parameter_value_text.hpp"
#include "core/pending_value.hpp"
#include "core/value_image_adapter.hpp"
#include "graph/graph_model.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "photospider/core/inspection_types.hpp"
#include "photospider/host/compute_request.hpp"
#include "photospider/plugin/opencv_adapter.hpp"
#include "providers/configured_operation_providers.hpp"

namespace ps {
namespace {

static_assert(sizeof(plugin::ParameterKind) == sizeof(std::uint32_t));
static_assert(sizeof(DataType) == sizeof(std::uint32_t));
static_assert(static_cast<std::uint32_t>(DataType::UINT8) == 0U);
static_assert(static_cast<std::uint32_t>(DataType::FLOAT64) == 5U);
static_assert(static_cast<std::uint32_t>(plugin::ParameterKind::Null) == 0U);
static_assert(static_cast<std::uint32_t>(plugin::ParameterKind::Object) == 6U);

static_assert(std::is_same_v<decltype(InputTileView::roi), PixelRect>);
static_assert(std::is_same_v<decltype(OutputTileView::roi), PixelRect>);
static_assert(std::is_same_v<decltype(InputTile::roi), PixelRect>);
static_assert(std::is_same_v<decltype(OutputTile::roi), PixelRect>);
static_assert(
    std::is_same_v<decltype(Node::hp_region), std::optional<RegionSet>>);
static_assert(std::is_same_v<decltype(Node::last_input_size_hp),
                             std::optional<PixelSize>>);
static_assert(std::is_same_v<typename decltype(DependencyLutCacheIdentity::
                                                   input_extents)::value_type,
                             PixelSize>);
static_assert(
    std::is_same_v<decltype(UpstreamRoiProjection::shared_roi), PixelRect>);
static_assert(
    std::is_same_v<decltype(UpstreamRoiProjection::dependency_roi), PixelRect>);
static_assert(
    std::is_same_v<decltype(compute::HpPlanEntry::roi_hp), PixelRect>);
static_assert(
    std::is_same_v<decltype(compute::HpPlanEntry::region_hp), RegionSet>);
static_assert(
    std::is_same_v<decltype(compute::HpPlanEntry::hp_size), PixelSize>);
static_assert(
    std::is_same_v<decltype(compute::RtPlanEntry::roi_rt), PixelRect>);
static_assert(
    std::is_same_v<decltype(compute::RtPlanEntry::region_hp), RegionSet>);
static_assert(
    std::is_same_v<decltype(compute::RtPlanEntry::rt_size), PixelSize>);
static_assert(std::is_same_v<
              decltype(compute::DirtySourceRoiRecord::source_roi), PixelRect>);
static_assert(std::is_same_v<typename decltype(compute::DirtySourceNodeState::
                                                   source_rois)::value_type,
                             PixelRect>);
static_assert(std::is_same_v<typename decltype(compute::DirtySourceNodeState::
                                                   source_regions)::value_type,
                             RegionSet>);
static_assert(
    std::is_same_v<decltype(compute::DirtyTileKey::pixel_roi), PixelRect>);
static_assert(
    std::is_same_v<decltype(compute::DirtyTileKey::region), RegionSet>);
static_assert(std::is_same_v<
              decltype(compute::DirtyMonolithicRegion::pixel_roi), PixelRect>);
static_assert(std::is_same_v<decltype(compute::DirtyMonolithicRegion::region),
                             RegionSet>);
static_assert(
    std::is_same_v<decltype(compute::DirtyEdgeMapping::from_roi), PixelRect>);
static_assert(
    std::is_same_v<decltype(compute::DirtyEdgeMapping::to_roi), PixelRect>);
static_assert(std::is_same_v<decltype(compute::DirtyEdgeMapping::from_region),
                             RegionSet>);
static_assert(
    std::is_same_v<decltype(compute::DirtyEdgeMapping::to_region), RegionSet>);
static_assert(
    std::is_same_v<
        typename decltype(compute::DirtyRegionSnapshot::per_node_dirty_rois)::
            mapped_type::value_type,
        PixelRect>);
static_assert(std::is_same_v<
              typename decltype(compute::DirtyRegionSnapshot::
                                    actual_dirty_rois)::mapped_type::value_type,
              PixelRect>);
static_assert(
    std::is_same_v<
        typename decltype(compute::DirtyRegionSnapshot::actual_dirty_regions)::
            mapped_type::value_type,
        RegionSet>);
static_assert(
    std::is_same_v<decltype(compute::PlannedDependency::to_roi), PixelRect>);
static_assert(
    std::is_same_v<decltype(compute::PlannedTask::output_roi), PixelRect>);
static_assert(
    std::is_same_v<
        typename decltype(compute::PlannedNodeWork::dirty_rois)::value_type,
        PixelRect>);
static_assert(std::is_same_v<
              decltype(compute::DirtyNodeSelection::execution_roi), PixelRect>);
static_assert(std::is_same_v<decltype(compute::ComputeRequest::dirty_roi),
                             std::optional<PixelRect>>);
static_assert(std::is_same_v<decltype(HostComputeRequest::dirty_roi),
                             std::optional<PixelRect>>);
static_assert(std::is_same_v<decltype(SpatialSnapshot::extent), PixelSize>);
static_assert(
    std::is_same_v<decltype(DirtyTileSnapshot::pixel_roi), PixelRect>);
static_assert(std::is_same_v<
              typename decltype(DirtyRegionInspectionSnapshot::
                                    actual_dirty_rois)::mapped_type::value_type,
              PixelRect>);

Node make_source_node(int id, const std::string& name, int width, int height) {
  Node node;
  node.id = id;
  node.name = name;
  node.type = "image_generator";
  node.subtype = "constant";
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
  return node;
}

Node make_blur_node(int id, int parent_id, int ksize) {
  Node node;
  node.id = id;
  node.name = "blur";
  node.type = "image_process";
  node.subtype = "gaussian_blur";
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
    const ImageBuffer buffer =
        make_aligned_cpu_image_buffer(width, height, 1, DataType::FLOAT32);
    state.cached_output_high_precision->publish_image_value(
        value_image_adapter::snapshot_cpu_image_value(buffer));
    state.hp_region = value_image_adapter::full_node_output_region(
        *state.cached_output_high_precision);
  });
}

/**
 * @brief Creates one dependency-LUT child with image and dynamic parameter
 * inputs.
 * @param id Child node id.
 * @param image_parent_id Image source node id.
 * @param parameter_parent_id Named-value source node id.
 * @return Node configured with an 8x8 output and `radius` parameter input.
 * @throws std::bad_alloc during value construction.
 * @note The operation key is test-local and registered explicitly by its test.
 */
Node make_public_dependency_node(int id, int image_parent_id,
                                 int parameter_parent_id) {
  Node node;
  node.id = id;
  node.name = "public_dependency";
  node.type = "operation_contract_test";
  node.subtype = "dependency";
  node.parameters["width"] = 8;
  node.parameters["height"] = 8;
  node.parameters["radius"] = 1;
  node.image_inputs.push_back(ImageInput{image_parent_id, "image"});
  node.parameter_inputs.push_back(
      ParameterInput{parameter_parent_id, "value", "radius"});
  return node;
}

/**
 * @brief Publishes one HP named integer output and revision on a graph node.
 * @param graph Graph containing node_id.
 * @param node_id Source node whose cache is replaced.
 * @param value Integer named output value.
 * @param revision Positive HP revision paired with the value.
 * @return Nothing.
 * @throws std::bad_alloc during output construction.
 * @note Mutation uses the graph runtime-state boundary so topology is
 * unchanged.
 */
void seed_parameter_output(GraphModel& graph, int node_id, int value,
                           int revision) {
  graph.mutate_node_runtime_state(node_id, [&](auto& state) {
    state.cached_output_high_precision = NodeOutput{};
    state.cached_output_high_precision->data["value"] = value;
    state.hp_version = revision;
  });
}

}  // namespace

TEST(PropagationContracts, BackwardLinearChainPropagatesPreciseDirtyRoi) {
  providers::register_configured_operation_providers();
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
  std::optional<PixelRect> propagated = propagation.project_roi_backward(
      graph, 200, (PixelRect{10, 20, 30, 40}), 1);

  ASSERT_TRUE(propagated.has_value());
  // curve_transform: identity.
  // resize 512->1024 with linear padding 1 yields [19,39,62x82].
  // The ROI propagation boundary also merges single-input local_inverse_matrix
  // propagation; with the default identity matrix, the resize step therefore
  // carries the union of [19,39,62x82] and [10,20,30x40] = [10,20,71x101].
  // gaussian_blur ksize=21 expands by radius 10, producing [0,10,91x121].
  EXPECT_EQ(*propagated, (PixelRect{0, 10, 91, 121}));
}

TEST(PropagationContracts, BackwardResizeRequiresHpParentExtent) {
  providers::register_configured_operation_providers();
  GraphModel graph = make_graph();
  graph.add_node(make_unparameterized_source_node(1, "source_without_hp"));
  graph.add_node(make_resize_node(2, 1, 4, 4, "nearest"));
  graph.validate_topology();

  RoiPropagationService propagation;
  EXPECT_FALSE(
      propagation.project_roi_backward(graph, 2, (PixelRect{1, 1, 1, 1}), 1)
          .has_value())
      << "Missing HP state must not provide propagation extent.";

  seed_hp_extent(graph, 1, 8, 8);

  std::optional<PixelRect> propagated =
      propagation.project_roi_backward(graph, 2, (PixelRect{1, 1, 1, 1}), 1);
  ASSERT_TRUE(propagated.has_value());
  EXPECT_EQ(*propagated, (PixelRect{2, 2, 2, 2}));
}

TEST(OperationParameterAdapter,
     PreservesOwnedRecursiveKindsAndRejectsNormalizedKeyCollisions) {
  YAML::Node source = YAML::Load(
      "plain: 12\n"
      "quoted: \"12\"\n"
      "enabled: true\n"
      "items: [null, 2.5, {name: original}]\n");
  plugin::ParameterMap parameters =
      adapters::yaml::internal::parameter_map_from_yaml(source);

  ASSERT_TRUE(parameters.at("plain").is_int64());
  EXPECT_EQ(parameters.at("plain").as_int64(), 12);
  ASSERT_TRUE(parameters.at("quoted").is_string());
  EXPECT_EQ(parameters.at("quoted").as_string(), "12");
  ASSERT_TRUE(parameters.at("enabled").is_bool());
  ASSERT_TRUE(parameters.at("items").is_array());
  ASSERT_EQ(parameters.at("items").as_array().size(), 3u);
  EXPECT_TRUE(parameters.at("items").as_array()[0].is_null());
  EXPECT_DOUBLE_EQ(parameters.at("items").as_array()[1].as_double(), 2.5);

  source["items"][2]["name"] = "mutated";
  EXPECT_EQ(
      parameters.at("items").as_array()[2].as_object().at("name").as_string(),
      "original");
  EXPECT_THROW(parameters.at("quoted").as_double(), plugin::ParameterTypeError);

  const YAML::Node collision = YAML::Load("{1: first, 1.0: second}");
  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(collision),
               std::invalid_argument);

  GraphModel graph = make_graph();
  Node disconnected = make_public_dependency_node(7, -1, -1);
  disconnected.parameters["radius"] = 6;
  const plugin::ParameterMap fallback =
      resolve_effective_parameter_snapshot(disconnected, graph);
  EXPECT_EQ(fallback.at("radius").as_int64(), 6)
      << "a disconnected parameter input preserves the static value like "
         "execution-time resolution";
}

TEST(OperationParameterValue,
     NormalizesIntegralInputsWithoutBoolOrDoubleAmbiguity) {
  const plugin::ParameterValue from_int(1);
  const plugin::ParameterValue from_long_long(
      std::numeric_limits<long long>::min());  // NOLINT(runtime/int)
  const std::uint64_t signed_max =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const plugin::ParameterValue from_unsigned_boundary(signed_max);
  const plugin::ParameterValue from_bool(true);
  const plugin::ParameterValue from_double(1.0);
  const plugin::ParameterValue from_float(1.0F);
  const plugin::ParameterValue from_long_double(1.0L);

  EXPECT_EQ(from_int.as_int64(), 1);
  EXPECT_EQ(from_long_long.as_int64(),
            static_cast<std::int64_t>(
                std::numeric_limits<long long>::min()));  // NOLINT(runtime/int)
  EXPECT_EQ(from_unsigned_boundary.as_int64(),
            std::numeric_limits<std::int64_t>::max());
  EXPECT_TRUE(from_bool.is_bool());
  EXPECT_TRUE(from_double.is_double());
  EXPECT_TRUE(from_float.is_double());
  EXPECT_TRUE(from_long_double.is_double());
  try {
    (void)from_double.as_int64();
    FAIL() << "Double-to-Int64 access must reject the alternative mismatch";
  } catch (const plugin::ParameterTypeError& error) {
    EXPECT_EQ(error.expected_kind(), plugin::ParameterKind::Int64);
    EXPECT_EQ(error.kind(), plugin::ParameterKind::Double);
  }
  try {
    (void)from_int.as_double();
    FAIL() << "Int64-to-Double access must reject the alternative mismatch";
  } catch (const plugin::ParameterTypeError& error) {
    EXPECT_EQ(error.expected_kind(), plugin::ParameterKind::Double);
    EXPECT_EQ(error.kind(), plugin::ParameterKind::Int64);
  }
  EXPECT_THROW(static_cast<void>(plugin::ParameterValue{
                   std::numeric_limits<std::uint64_t>::max()}),
               std::overflow_error);
  if constexpr (std::numeric_limits<long double>::max_exponent >
                std::numeric_limits<double>::max_exponent) {
    EXPECT_THROW(static_cast<void>(plugin::ParameterValue{
                     std::numeric_limits<long double>::max()}),
                 std::overflow_error);
  }
}

/**
 * @brief Verifies provider-independent scalar and recursive inspection text.
 */
TEST(OperationParameterValue,
     InspectionTextPreservesScalarsAndEscapesRecursiveValues) {
  EXPECT_EQ(core::format_parameter_value_for_inspection(
                plugin::ParameterValue("007")),
            "007");
  EXPECT_EQ(
      core::format_parameter_value_for_inspection(plugin::ParameterValue(1.25)),
      "1.25");

  plugin::ParameterValue::Object object;
  object.emplace("z", plugin::ParameterValue(true));
  object.emplace("a\n",
                 plugin::ParameterValue(std::string("quote\" slash\\\x01")));
  plugin::ParameterValue::Array array;
  array.emplace_back("plain");
  array.emplace_back(std::move(object));
  array.emplace_back(nullptr);
  array.emplace_back(1.25);

  EXPECT_EQ(
      core::format_parameter_value_for_inspection(
          plugin::ParameterValue(std::move(array))),
      R"(["plain", {"a\n": "quote\" slash\\\u0001", "z": true}, null, 1.25])");
}

TEST(OperationParameterAdapter, PreservesTaggedRoundTripAndYamlNumericKinds) {
  plugin::ParameterValue::Object object;
  object.emplace("numeric_text", plugin::ParameterValue("123"));
  object.emplace("boolean_text", plugin::ParameterValue("true"));
  object.emplace("null_text", plugin::ParameterValue("null"));
  object.emplace("numeric_key", plugin::ParameterValue::Object{
                                    {"1", plugin::ParameterValue("value")}});
  object.emplace("integral_double", plugin::ParameterValue(1.0));
  object.emplace(
      "nan", plugin::ParameterValue(std::numeric_limits<double>::quiet_NaN()));
  object.emplace("infinity", plugin::ParameterValue(
                                 std::numeric_limits<double>::infinity()));

  const YAML::Node yaml = adapters::yaml::internal::parameter_value_to_yaml(
      plugin::ParameterValue(std::move(object)));
  const plugin::ParameterValue round_trip =
      adapters::yaml::internal::parameter_value_from_yaml(yaml);
  const auto& values = round_trip.as_object();
  EXPECT_EQ(values.at("numeric_text").as_string(), "123");
  EXPECT_EQ(values.at("boolean_text").as_string(), "true");
  EXPECT_EQ(values.at("null_text").as_string(), "null");
  EXPECT_EQ(values.at("numeric_key").as_object().at("1").as_string(), "value");
  EXPECT_TRUE(values.at("integral_double").is_double());
  EXPECT_DOUBLE_EQ(values.at("integral_double").as_double(), 1.0);
  EXPECT_TRUE(values.at("nan").is_double());
  EXPECT_TRUE(std::isnan(values.at("nan").as_double()));
  EXPECT_TRUE(values.at("infinity").is_double());
  EXPECT_TRUE(std::isinf(values.at("infinity").as_double()));

  const plugin::ParameterMap plain =
      adapters::yaml::internal::parameter_map_from_yaml(
          YAML::Load("{nan: .nan, infinity: .inf, hex: 0x10, octal: 012}"));
  EXPECT_TRUE(plain.at("nan").is_double());
  EXPECT_TRUE(std::isnan(plain.at("nan").as_double()));
  EXPECT_TRUE(plain.at("infinity").is_double());
  EXPECT_TRUE(std::isinf(plain.at("infinity").as_double()));
  EXPECT_EQ(plain.at("hex").as_int64(), 16);
  EXPECT_EQ(plain.at("octal").as_int64(), 10);
}

TEST(OperationParameterAdapter,
     RejectsOutOfRangePlainIntegersBeforeDoubleInference) {
  const plugin::ParameterMap boundaries =
      adapters::yaml::internal::parameter_map_from_yaml(
          YAML::Load("{maximum: 9223372036854775807, "
                     "minimum: -9223372036854775808, scientific: 9.25e2}"));
  EXPECT_EQ(boundaries.at("maximum").as_int64(),
            std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(boundaries.at("minimum").as_int64(),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_DOUBLE_EQ(boundaries.at("scientific").as_double(), 925.0);

  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(
                   YAML::Load("{value: 9223372036854775808}")),
               YAML::Exception);
  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(
                   YAML::Load("{value: 9223372036854775809}")),
               YAML::Exception);
  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(
                   YAML::Load("{value: -9223372036854775809}")),
               YAML::Exception);
  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(
                   YAML::Load("{value: 1e9999}")),
               YAML::Exception);
}

TEST(OperationParameterAdapter,
     HonorsEveryExplicitScalarTagAndRejectsUnknownTags) {
  const plugin::ParameterMap tagged =
      adapters::yaml::internal::parameter_map_from_yaml(
          YAML::Load("{null_value: !!null null, bool_value: !!bool true, "
                     "int_value: !!int 1, float_value: !!float 1, "
                     "string_value: !!str 1}"));
  EXPECT_TRUE(tagged.at("null_value").is_null());
  EXPECT_TRUE(tagged.at("bool_value").is_bool());
  EXPECT_TRUE(tagged.at("bool_value").as_bool());
  EXPECT_TRUE(tagged.at("int_value").is_int64());
  EXPECT_EQ(tagged.at("int_value").as_int64(), 1);
  EXPECT_TRUE(tagged.at("float_value").is_double());
  EXPECT_DOUBLE_EQ(tagged.at("float_value").as_double(), 1.0);
  EXPECT_TRUE(tagged.at("string_value").is_string());
  EXPECT_EQ(tagged.at("string_value").as_string(), "1");

  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(
                   YAML::Load("{value: !photospider/custom opaque}")),
               YAML::Exception);
  EXPECT_THROW(adapters::yaml::internal::parameter_map_from_yaml(
                   YAML::Load("{value: !!timestamp 2026-07-13}")),
               YAML::Exception);
}

TEST(SpatialDependencyMapContract,
     RejectsFullyOutsideQueriesAndClipsPartialIntersection) {
  SpatialDependencyMap lut;
  lut.grid_size_x = 10;
  lut.grid_size_y = 10;
  lut.cols = 2;
  lut.rows = 2;
  lut.output_extent = (PixelSize{20, 20});
  lut.cell_to_upstream_roi = {(PixelRect{1, 1, 1, 1}), (PixelRect{2, 2, 1, 1}),
                              (PixelRect{3, 3, 1, 1}), (PixelRect{4, 4, 1, 1})};
  ASSERT_TRUE(lut.is_valid());

  EXPECT_EQ(lut.lookup((PixelRect{-20, 5, 5, 5})), (PixelRect{}));
  EXPECT_EQ(lut.lookup((PixelRect{20, 5, 5, 5})), (PixelRect{}));
  EXPECT_EQ(lut.lookup((PixelRect{5, -20, 5, 5})), (PixelRect{}));
  EXPECT_EQ(lut.lookup((PixelRect{5, 20, 5, 5})), (PixelRect{}));
  EXPECT_EQ(lut.lookup(PixelRect{std::numeric_limits<int>::min(), 0,
                                 std::numeric_limits<int>::max(), 1}),
            (PixelRect{}));
  EXPECT_EQ(lut.lookup(PixelRect{std::numeric_limits<int>::max(), 0,
                                 std::numeric_limits<int>::max(), 1}),
            (PixelRect{}));

  EXPECT_EQ(lut.lookup((PixelRect{-5, 0, 10, 10})), (PixelRect{1, 1, 1, 1}));
  EXPECT_EQ(lut.lookup((PixelRect{15, 0, 10, 10})), (PixelRect{2, 2, 1, 1}));
  EXPECT_EQ(lut.lookup((PixelRect{0, -5, 10, 10})), (PixelRect{1, 1, 1, 1}));
  EXPECT_EQ(lut.lookup((PixelRect{0, 15, 10, 10})), (PixelRect{3, 3, 1, 1}));
}

TEST(OperationParameterAdapter,
     RejectsMalformedTaggedParametersBeforeGraphStorage) {
  Node node;
  node.id = 42;
  node.name = "malformed_parameter";
  node.type = "operation_contract_test";
  node.subtype = "malformed_parameter";
  const std::vector<std::string> malformed_documents{
      "{value: !!null nope}", "{value: !!bool nope}", "{value: !!int nope}",
      "{value: !!float nope}", "{value: 9223372036854775808}"};
  for (const std::string& document : malformed_documents) {
    SCOPED_TRACE(document);
    EXPECT_THROW(
        adapters::yaml::internal::parameter_map_from_yaml(YAML::Load(document)),
        YAML::Exception);
    EXPECT_TRUE(node.parameters.empty());
  }

  node.parameters = adapters::yaml::internal::parameter_map_from_yaml(
      YAML::Load("{value: !!float 1}"));
  EXPECT_TRUE(node.parameters.at("value").is_double());
  EXPECT_DOUBLE_EQ(node.parameters.at("value").as_double(), 1.0);
}

TEST(PropagationContracts, DependencyLutRoutesOnlyToItsSelectedImageInputEdge) {
  const std::string type = "operation_contract_test";
  const std::string subtype = "dependency_route";
  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      type, subtype,
      [](const Node&, const PixelRect&, const GraphModel&, const PixelSize&,
         const std::vector<PixelSize>&, const plugin::ParameterMap&,
         const std::vector<const NodeOutput*>*) {
        return PixelRect{0, 0, 1, 1};
      });
  registry.register_dependency_builder(
      type, subtype,
      [](const Node&, const GraphModel&, const std::vector<PixelSize>&,
         const PixelSize& output_extent, const plugin::ParameterMap&) {
        SpatialDependencyMap result;
        result.grid_size_x = output_extent.width;
        result.grid_size_y = output_extent.height;
        result.cols = 1;
        result.rows = 1;
        result.output_extent = output_extent;
        result.upstream_input_index = 1;
        result.cell_to_upstream_roi.push_back((PixelRect{5, 0, 1, 1}));
        return result;
      },
      false);

  GraphModel graph = make_graph();
  graph.add_node(make_source_node(1, "left", 8, 8));
  graph.add_node(make_source_node(4, "right", 8, 8));
  Node child;
  child.id = 3;
  child.name = "dependency_route";
  child.type = type;
  child.subtype = subtype;
  child.parameters["width"] = 8;
  child.parameters["height"] = 8;
  child.image_inputs = {ImageInput{1, "image"}, ImageInput{4, "image"}};
  graph.add_node(child);
  graph.validate_topology();
  seed_hp_extent(graph, 1, 8, 8);
  seed_hp_extent(graph, 4, 8, 8);

  RoiPropagationService propagation;
  const auto left =
      propagation.project_roi_backward(graph, 3, (PixelRect{0, 0, 1, 1}), 1);
  const auto right =
      propagation.project_roi_backward(graph, 3, (PixelRect{0, 0, 1, 1}), 4);

  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(*left, (PixelRect{0, 0, 1, 1}));
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(*right, (PixelRect{0, 0, 6, 1}));
  ASSERT_TRUE(graph.node(3).dependency_lut_cache.has_value());
  EXPECT_EQ(graph.node(3).dependency_lut_cache->lut.upstream_input_index, 1u);
}

TEST(PropagationContracts, BoundsSharedAndLutContributionsBeforeBackwardUnion) {
  const std::string type = "operation_contract_test";
  const std::string subtype = "bounded_dependency_union";
  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      type, subtype,
      [](const Node&, const PixelRect&, const GraphModel&, const PixelSize&,
         const std::vector<PixelSize>&, const plugin::ParameterMap&,
         const std::vector<const NodeOutput*>*) {
        return PixelRect{std::numeric_limits<int>::min(), 0,
                         std::numeric_limits<int>::max(), 1};
      });
  registry.register_dependency_builder(
      type, subtype,
      [](const Node&, const GraphModel&, const std::vector<PixelSize>&,
         const PixelSize& output_extent, const plugin::ParameterMap&) {
        SpatialDependencyMap result;
        result.grid_size_x = output_extent.width;
        result.grid_size_y = output_extent.height;
        result.cols = 1;
        result.rows = 1;
        result.output_extent = output_extent;
        result.upstream_input_index = 0;
        result.cell_to_upstream_roi.push_back((PixelRect{70, 0, 1, 1}));
        return result;
      },
      false);

  GraphModel graph = make_graph();
  Node source = make_source_node(1, "bounded_source", 128, 128);
  source.subtype = "bounded_source";
  graph.add_node(source);
  Node child;
  child.id = 2;
  child.name = "bounded_dependency_union";
  child.type = type;
  child.subtype = subtype;
  child.parameters["width"] = 8;
  child.parameters["height"] = 8;
  child.image_inputs.push_back(ImageInput{1, "image"});
  graph.add_node(child);
  graph.validate_topology();
  seed_hp_extent(graph, 1, 128, 128);

  RoiPropagationService propagation;
  const auto projected =
      propagation.project_roi_backward(graph, 2, (PixelRect{0, 0, 1, 1}), 1);
  ASSERT_TRUE(projected.has_value());
  EXPECT_EQ(*projected, (PixelRect{70, 0, 1, 1}));

  GraphTraversalService traversal;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 2, (PixelRect{0, 0, 1, 1}));
  ASSERT_EQ(plan.entries.size(), 2u);
  EXPECT_EQ(plan.entries.at(1).roi_hp, (PixelRect{64, 0, 64, 64}));
  EXPECT_EQ(plan.entries.at(2).roi_hp, (PixelRect{0, 0, 8, 8}));

  ASSERT_EQ(plan.snapshot.actual_dirty_rois.size(), 2u);
  EXPECT_EQ(plan.snapshot.actual_dirty_rois.at(1),
            (std::vector<PixelRect>{{64, 0, 64, 64}}));
  EXPECT_EQ(plan.snapshot.actual_dirty_rois.at(2),
            (std::vector<PixelRect>{{0, 0, 8, 8}}));

  ASSERT_EQ(plan.snapshot.edge_mappings.size(), 1u);
  const compute::DirtyEdgeMapping& mapping =
      plan.snapshot.edge_mappings.front();
  EXPECT_EQ(mapping.from_node_id, 1);
  EXPECT_EQ(mapping.to_node_id, 2);
  EXPECT_EQ(mapping.domain, compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(mapping.from_roi, (PixelRect{64, 0, 64, 64}));
  EXPECT_EQ(mapping.to_roi, (PixelRect{0, 0, 8, 8}));
  EXPECT_EQ(mapping.direction, compute::DirtyEdgeDirection::BackwardDemand);
}

/**
 * @brief Verifies aligned storage tile identity stays distinct from bounded
 * logical Region metadata at an image edge.
 *
 * @return Nothing; GoogleTest reports storage-key or Region mismatches.
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc when
 * the test fixture violates the checked logical/storage boundary.
 * @note A boundary tile key may extend beyond the allocation so task-grid
 * identity remains stable. Its retained Region must still be clipped to the
 * exact signed data window before origin translation.
 */
TEST(DirtyRegionSnapshotBuilderContract,
     ClipsLogicalBoundaryRegionWithoutShrinkingAlignedStorageKey) {
  compute::DirtyRegionSnapshot snapshot;
  compute::DirtyRegionSnapshotBuilder builder;
  const ImageBounds data_window{-10, -20, -2, -12};

  builder.enumerate_tiles(
      snapshot, compute::DirtyTileEnumeration{
                    7, compute::DirtyDomain::HighPrecision,
                    compute::DirtyTileLevel::Micro, PixelRect{0, 0, 8, 8},
                    data_window, compute::kHpMicroTileSize});

  ASSERT_EQ(snapshot.dirty_tiles.size(), 1U);
  const compute::DirtyTileKey& tile = snapshot.dirty_tiles.front();
  EXPECT_EQ(tile.pixel_roi, (PixelRect{0, 0, 64, 64}));
  EXPECT_EQ(tile.region, RegionSet::from_image_rect(
                             {image_region_domain(), -10, -2, -20, -12}));
}

/**
 * @brief Verifies tile enumeration rejects a request ROI that crosses the
 * storage allocation before publishing any tile key.
 *
 * @return Nothing; GoogleTest reports exception or publication mismatches.
 * @throws std::bad_alloc if test-owned storage allocation fails.
 * @note A one-pixel tile forces incremental implementations to publish the
 * contained leading tile before reaching the out-of-window tile. Boundary
 * tile-key overhang is permitted only after a contained request ROI is
 * aligned; callers cannot use alignment clipping to hide invalid input.
 */
TEST(DirtyRegionSnapshotBuilderContract,
     RejectsRequestRoiOutsideStorageWithoutPublishingTiles) {
  compute::DirtyRegionSnapshot snapshot;
  compute::DirtyRegionSnapshotBuilder builder;
  const ImageBounds data_window{-10, -20, -2, -12};
  constexpr int kSinglePixelTileSize = 1;

  EXPECT_THROW(builder.enumerate_tiles(
                   snapshot,
                   compute::DirtyTileEnumeration{
                       7, compute::DirtyDomain::HighPrecision,
                       compute::DirtyTileLevel::Micro, PixelRect{7, 0, 2, 1},
                       data_window, kSinglePixelTileSize}),
               std::invalid_argument);
  EXPECT_TRUE(snapshot.dirty_tiles.empty());
}

TEST(PropagationContracts,
     DependencyLutReusesExactSnapshotAndPublishesWithStrongGuarantee) {
  const std::string type = "operation_contract_test";
  const std::string subtype = "dependency";
  auto builder_calls = std::make_shared<int>(0);
  auto last_source_id = std::make_shared<int>(-1);
  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      type, subtype,
      [](const Node&, const PixelRect&, const GraphModel&, const PixelSize&,
         const std::vector<PixelSize>&, const plugin::ParameterMap&,
         const std::vector<const NodeOutput*>*) { return PixelRect{}; });
  registry.register_dependency_builder(
      type, subtype,
      [builder_calls, last_source_id](
          const Node& node, const GraphModel&, const std::vector<PixelSize>&,
          const PixelSize& output_extent,
          const plugin::ParameterMap& effective_parameters) {
        ++*builder_calls;
        *last_source_id = node.image_inputs[0].from_node_id;
        const int value =
            static_cast<int>(effective_parameters.at("radius").as_int64());
        SpatialDependencyMap result;
        result.grid_size_x = output_extent.width;
        result.grid_size_y = output_extent.height;
        result.cols = 1;
        result.rows = 1;
        result.output_extent = output_extent;
        result.upstream_input_index = value == 99 ? 9 : 0;
        result.cell_to_upstream_roi.push_back((PixelRect{value, 0, 1, 1}));
        return result;
      },
      false);

  GraphModel graph = make_graph();
  graph.add_node(make_source_node(1, "image", 8, 8));
  graph.add_node(make_source_node(4, "replacement_image", 8, 8));
  graph.add_node(make_unparameterized_source_node(2, "parameter"));
  graph.add_node(make_public_dependency_node(3, 1, 2));
  graph.validate_topology();
  seed_hp_extent(graph, 1, 8, 8);
  seed_hp_extent(graph, 4, 8, 8);
  seed_parameter_output(graph, 2, 2, 1);

  RoiPropagationService propagation;
  auto project = [&] {
    const int current_image_source = graph.node(3).image_inputs[0].from_node_id;
    return propagation.project_roi_backward(graph, 3, (PixelRect{0, 0, 1, 1}),
                                            current_image_source);
  };
  ASSERT_EQ(project(), std::optional<PixelRect>((PixelRect{2, 0, 1, 1})));
  EXPECT_EQ(*builder_calls, 1);
  EXPECT_EQ(graph.node(3).dependency_lut_version, 1u);
  ASSERT_EQ(project(), std::optional<PixelRect>((PixelRect{2, 0, 1, 1})));
  EXPECT_EQ(*builder_calls, 1);

  graph.mutate_node_runtime_state(
      3, [](auto& state) { state.parameters_version = 1; });
  ASSERT_EQ(project(), std::optional<PixelRect>((PixelRect{2, 0, 1, 1})));
  EXPECT_EQ(*builder_calls, 2);
  EXPECT_EQ(graph.node(3).dependency_lut_version, 2u);

  seed_parameter_output(graph, 2, 2, 2);
  ASSERT_EQ(project(), std::optional<PixelRect>((PixelRect{2, 0, 1, 1})));
  EXPECT_EQ(*builder_calls, 3);
  EXPECT_EQ(graph.node(3).dependency_lut_version, 3u);
  ASSERT_TRUE(graph.node(3).dependency_lut_cache.has_value());
  EXPECT_FALSE(graph.node(3).dependency_lut_cache->identity.data_dependent);
  EXPECT_TRUE(
      graph.node(3)
          .dependency_lut_cache->identity.upstream_content_revisions.empty());

  seed_parameter_output(graph, 2, 4, 3);
  ASSERT_EQ(project(), std::optional<PixelRect>((PixelRect{4, 0, 1, 1})));
  EXPECT_EQ(*builder_calls, 4);
  EXPECT_EQ(graph.node(3).dependency_lut_version, 4u);
  ASSERT_TRUE(graph.node(3).dependency_lut_cache.has_value());
  EXPECT_EQ(
      graph.node(3)
          .dependency_lut_cache->identity.effective_parameters.at("radius")
          .as_int64(),
      4);

  graph.rewire_image_input(3, 0, 4, "image");
  ASSERT_EQ(project(), std::optional<PixelRect>((PixelRect{4, 0, 1, 1})));
  EXPECT_EQ(*builder_calls, 5);
  EXPECT_EQ(*last_source_id, 4);
  EXPECT_EQ(graph.node(3).dependency_lut_version, 5u);
  ASSERT_TRUE(graph.node(3).dependency_lut_cache.has_value());
  ASSERT_EQ(
      graph.node(3).dependency_lut_cache->identity.image_input_sources.size(),
      1u);
  EXPECT_EQ(graph.node(3)
                .dependency_lut_cache->identity.image_input_sources.front()
                .source_node_id,
            4);

  seed_parameter_output(graph, 2, 99, 4);
  EXPECT_THROW((void)project(), GraphError);
  EXPECT_EQ(*builder_calls, 6);
  EXPECT_EQ(graph.node(3).dependency_lut_version, 5u);
  ASSERT_TRUE(graph.node(3).dependency_lut_cache.has_value());
  EXPECT_EQ(
      graph.node(3)
          .dependency_lut_cache->identity.effective_parameters.at("radius")
          .as_int64(),
      4);
  EXPECT_EQ(
      graph.node(3).dependency_lut_cache->identity.static_parameter_revision,
      1u);
  ASSERT_EQ(graph.node(3)
                .dependency_lut_cache->identity
                .parameter_input_content_revisions.size(),
            1u);
  EXPECT_EQ(graph.node(3)
                .dependency_lut_cache->identity
                .parameter_input_content_revisions.front(),
            3u);
  EXPECT_EQ(graph.node(3)
                .dependency_lut_cache->identity.image_input_sources.front()
                .source_node_id,
            4);
}

TEST(PropagationContracts,
     DirectBuilderReplacementInvalidatesTheExistingDependencyCache) {
  const std::string type = "operation_contract_test";
  const std::string subtype = "builder_revision";
  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      type, subtype,
      [](const Node&, const PixelRect&, const GraphModel&, const PixelSize&,
         const std::vector<PixelSize>&, const plugin::ParameterMap&,
         const std::vector<const NodeOutput*>*) { return PixelRect{}; });
  auto register_builder = [&](int marker, const std::shared_ptr<int>& calls) {
    registry.register_dependency_builder(
        type, subtype,
        [marker, calls](
            const Node&, const GraphModel&, const std::vector<PixelSize>&,
            const PixelSize& output_extent, const plugin::ParameterMap&) {
          ++*calls;
          SpatialDependencyMap result;
          result.grid_size_x = output_extent.width;
          result.grid_size_y = output_extent.height;
          result.cols = 1;
          result.rows = 1;
          result.output_extent = output_extent;
          result.upstream_input_index = 0;
          result.cell_to_upstream_roi.push_back((PixelRect{marker, 0, 1, 1}));
          return result;
        },
        false);
  };

  const auto first_calls = std::make_shared<int>(0);
  const auto second_calls = std::make_shared<int>(0);
  register_builder(1, first_calls);
  GraphModel graph = make_graph();
  graph.add_node(make_source_node(1, "image", 8, 8));
  Node child;
  child.id = 2;
  child.name = "builder_revision";
  child.type = type;
  child.subtype = subtype;
  child.parameters["width"] = 8;
  child.parameters["height"] = 8;
  child.image_inputs.push_back(ImageInput{1, "image"});
  graph.add_node(child);
  graph.validate_topology();
  seed_hp_extent(graph, 1, 8, 8);
  RoiPropagationService propagation;

  (void)propagation.project_roi_backward(graph, 2, (PixelRect{0, 0, 1, 1}), 1);
  EXPECT_EQ(*first_calls, 1);
  EXPECT_EQ(*second_calls, 0);
  ASSERT_TRUE(graph.node(2).dependency_lut_cache.has_value());
  const std::uint64_t first_revision =
      graph.node(2).dependency_lut_cache->identity.dependency_builder_revision;
  EXPECT_EQ(graph.node(2).dependency_lut_cache->lut.cell_to_upstream_roi[0].x,
            1);

  register_builder(2, second_calls);
  (void)propagation.project_roi_backward(graph, 2, (PixelRect{0, 0, 1, 1}), 1);
  EXPECT_EQ(*first_calls, 1);
  EXPECT_EQ(*second_calls, 1);
  EXPECT_NE(
      graph.node(2).dependency_lut_cache->identity.dependency_builder_revision,
      first_revision);
  EXPECT_EQ(graph.node(2).dependency_lut_cache->lut.cell_to_upstream_roi[0].x,
            2);
}

TEST(OperationRegistryContract,
     DependencyBuilderSnapshotNeverMixesConcurrentCallbackAndFlagState) {
  const std::string type = "operation_contract_test";
  const std::string subtype = "concurrent_builder_snapshot";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(type, subtype));
  const auto builder = [](std::size_t marker) {
    return DependencyLutBuilder(
        [marker](const Node&, const GraphModel&, const std::vector<PixelSize>&,
                 const PixelSize&, const plugin::ParameterMap&) {
          SpatialDependencyMap result;
          result.upstream_input_index = marker;
          return result;
        });
  };
  registry.register_dependency_builder(type, subtype, builder(0), false);

  GraphModel graph = make_graph();
  Node node;
  const auto initial_snapshot =
      registry.get_dependency_builder_snapshot(type, subtype);
  ASSERT_TRUE(initial_snapshot.has_value());
  EXPECT_FALSE(initial_snapshot->data_dependent);
  EXPECT_EQ(initial_snapshot->data_dependent_revision, 0u);
  EXPECT_EQ(initial_snapshot->callback(node, graph, {}, (PixelSize{}), {})
                .upstream_input_index,
            0u);
  std::atomic<bool> reader_ready{false};
  std::atomic<bool> writer_finished{false};
  std::atomic<bool> saw_replacement{false};
  std::atomic<int> inconsistent_snapshots{0};
  std::thread reader([&] {
    bool first_snapshot = true;
    for (;;) {
      const bool writer_was_finished =
          writer_finished.load(std::memory_order_acquire);
      const auto snapshot =
          registry.get_dependency_builder_snapshot(type, subtype);
      if (!snapshot) {
        inconsistent_snapshots.fetch_add(1, std::memory_order_relaxed);
      } else {
        const std::size_t marker =
            snapshot->callback(node, graph, {}, (PixelSize{}), {})
                .upstream_input_index;
        if (marker == 0) {
          if (snapshot->data_dependent ||
              snapshot->data_dependent_revision != 0) {
            inconsistent_snapshots.fetch_add(1, std::memory_order_relaxed);
          }
        } else if (marker == 1) {
          saw_replacement.store(true, std::memory_order_release);
          if (!snapshot->data_dependent ||
              snapshot->data_dependent_revision !=
                  snapshot->dependency_builder_revision) {
            inconsistent_snapshots.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          inconsistent_snapshots.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (first_snapshot) {
        reader_ready.store(true, std::memory_order_release);
        first_snapshot = false;
      }
      if (writer_was_finished) {
        break;
      }
      std::this_thread::yield();
    }
  });
  while (!reader_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  registry.register_dependency_builder(type, subtype, builder(1), true);
  writer_finished.store(true, std::memory_order_release);
  reader.join();

  EXPECT_TRUE(saw_replacement.load(std::memory_order_acquire));
  EXPECT_EQ(inconsistent_snapshots.load(std::memory_order_acquire), 0);
  registry.unregister_key(make_key(type, subtype));
}

TEST(OperationRegistryContract,
     DependencyFlagAggregatesBuilderMetadataAndDeviceSourcesIndependently) {
  const std::string type = "operation_contract_test";
  const std::string subtype = "dependency_flag_sources";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(type, subtype));
  const DependencyLutBuilder builder =
      [](const Node&, const GraphModel&, const std::vector<PixelSize>&,
         const PixelSize&,
         const plugin::ParameterMap&) { return SpatialDependencyMap{}; };
  const MonolithicOpFunc operation = [](const Node&,
                                        const std::vector<const NodeOutput*>&) {
    NodeOutput output;
    output.data["marker"] = 1;
    return output;
  };
  registry.register_dependency_builder(type, subtype, builder, false);

  OpMetadata metadata;
  metadata.data_dependent = true;
  registry.register_op_hp_monolithic(type, subtype, operation, metadata);
  auto snapshot = registry.get_dependency_builder_snapshot(type, subtype);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->data_dependent);
  EXPECT_GT(snapshot->data_dependent_revision, 0u);

  metadata.data_dependent = false;
  registry.register_op_hp_monolithic(type, subtype, operation, metadata);
  snapshot = registry.get_dependency_builder_snapshot(type, subtype);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_FALSE(snapshot->data_dependent)
      << "replacing HP metadata true with false must not leave sticky state";
  EXPECT_EQ(snapshot->data_dependent_revision, 0u);

  registry.register_dependency_builder(type, subtype, builder, true);
  snapshot = registry.get_dependency_builder_snapshot(type, subtype);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->data_dependent)
      << "the explicit builder flag remains independent of false metadata";
  EXPECT_EQ(snapshot->data_dependent_revision,
            snapshot->dependency_builder_revision);

  registry.register_dependency_builder(type, subtype, builder, false);
  OpMetadata device_metadata;
  device_metadata.data_dependent = true;
  registry.register_impl(type, subtype, Device::GPU_METAL, operation,
                         device_metadata);
  snapshot = registry.get_dependency_builder_snapshot(type, subtype);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->data_dependent)
      << "a GPU-only implementation metadata flag survives builder=false";
  EXPECT_GT(snapshot->data_dependent_revision,
            snapshot->dependency_builder_revision);

  registry.unregister_key(make_key(type, subtype));
}

}  // namespace ps
