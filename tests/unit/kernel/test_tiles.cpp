#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
SchemaTemplate schema(std::vector<std::uint64_t> shape,
                      std::vector<std::uint64_t> batch = {}) {
  SchemaTemplate result;
  result.id = "test.tiles";
  ResultTensorSpec tensor;
  tensor.key = "pixels";
  tensor.descriptor = {ElementType::UInt8, std::move(shape)};
  tensor.batch_axes.assign(batch.begin(), batch.end());
  tensor.layout.spatial = true;
  tensor.atomic_trailing_axes = tensor.descriptor.shape.size() == 3 ? 1 : 0;
  if (tensor.descriptor.shape.size() == 2)
    tensor.layout.channel_axis.reset();
  result.tensors.push_back(std::move(tensor));
  return result;
}
bool same_region(const Region& actual, const Region& expected) {
  if (actual.rank() != expected.rank())
    return false;
  for (std::size_t i = 0; i < actual.rank(); ++i)
    if (actual.dimensions()[i].offset != expected.dimensions()[i].offset ||
        actual.dimensions()[i].extent != expected.dimensions()[i].extent)
      return false;
  return true;
}
int geometry() {
  auto grid = TileGrid::from_schema(schema({129, 257, 4}, {2}));
  PS_REQUIRE_OK(grid);
  PS_CHECK(grid.value().tile_extent() == 128 &&
           grid.value().batch_count() == 2 && grid.value().tile_rows() == 2 &&
           grid.value().tile_columns() == 3 && grid.value().tile_count() == 12);
  PS_CHECK(same_region(grid.value().region(11).value(),
                       Region({{1, 1}, {128, 1}, {256, 1}, {0, 4}})));
  PS_CHECK(grid.value().region(12).status().code == ErrorCode::InvalidArgument);
  auto chw = schema({3, 5, 7}, {2, 3});
  chw.tensors[0].atomic_trailing_axes = 0;
  chw.tensors[0].layout.height_axis = 1;
  chw.tensors[0].layout.width_axis = 2;
  chw.tensors[0].layout.channel_axis = 0;
  auto transposed = TileGrid::from_schema(chw, 4);
  PS_REQUIRE_OK(transposed);
  PS_CHECK(transposed.value().tile_count() == 24 &&
           same_region(transposed.value().region(23).value(),
                       Region({{1, 1}, {2, 1}, {0, 3}, {4, 1}, {4, 3}})));
  const std::uint64_t last[] = {1, 2, 2, 4, 6};
  PS_CHECK(sdk::tile_for_coordinate(transposed.value(), last, 5).value() == 23);
  PS_CHECK(
      sdk::tile_for_coordinate(transposed.value(), last, 4).status().code ==
      ErrorCode::InvalidArgument);
  PS_CHECK(!sdk::tile_for_coordinate(transposed.value(), nullptr, 5).ok());
  auto bad_channel = std::array<std::uint64_t, 5>{1, 2, 3, 4, 6};
  PS_CHECK(!sdk::tile_for_coordinate(transposed.value(), bad_channel.data(), 5)
                .ok());
  auto one = TileGrid::from_schema(chw, 1);
  PS_REQUIRE_OK(one);
  PS_CHECK(one.value().tile_count() == 210);
  PS_CHECK(TileGrid::from_schema(chw, 0).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(TileGrid::from_schema(chw, 3).status().code ==
           ErrorCode::InvalidArgument);
  auto near = TileGrid::from_schema(schema({UINT64_MAX, 1}), UINT64_C(1) << 63);
  PS_REQUIRE_OK(near);
  PS_CHECK(near.value().tile_count() == 2 &&
           same_region(
               near.value().region(1).value(),
               Region({{UINT64_C(1) << 63, (UINT64_C(1) << 63) - 1}, {0, 1}})));
  auto overflow = TileGrid::from_schema(schema({UINT64_MAX, 2}), 1);
  PS_CHECK(overflow.status().code == ErrorCode::ResourceExhausted);
  auto batches = TileGrid::from_schema(schema({1, 1}, {UINT64_MAX, 2}), 128);
  PS_CHECK(batches.status().code == ErrorCode::ResourceExhausted);
  auto ordinary = schema({3, 5});
  ordinary.tensors[0].layout.spatial = false;
  PS_CHECK(TileGrid::from_schema(ordinary).value().tile_count() == 1);
  auto multiple = schema({3, 5});
  multiple.tensors.push_back(multiple.tensors[0]);
  multiple.tensors[1].key = "second";
  multiple.tensors[1].descriptor.shape = {129, 257};
  auto members = TileGrid::members(multiple).take_value();
  PS_CHECK(members.size() == 2 && members[0].tile_count() == 1 &&
           members[1].tile_count() == 6);
  return 0;
}
int storage_tiles() {
  using multi_result::take;
  ResourceBudget root;
  for (bool chw : {false, true}) {
    auto image = schema(chw ? std::vector<std::uint64_t>{2, 3, 5}
                            : std::vector<std::uint64_t>{3, 5, 2});
    auto& layout = image.tensors[0].layout;
    if (chw) {
      image.tensors[0].atomic_trailing_axes = 0;
      layout.height_axis = 1;
      layout.width_axis = 2;
      layout.channel_axis = 0;
    }
    for (std::uint64_t extent : {1, 2, 4, 128}) {
      std::vector<std::uint8_t> bytes(30);
      for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i + 1);
      auto builder =
          take(ResultBuilder::start(root, image, "storage.tiles", {}, extent));
      const auto region = Region::whole(image.tensors[0].sample_shape());
      PS_CHECK(
          builder
              .publish_tensor(0, region, ByteView(bytes.data(), bytes.size()))
              .ok());
      auto result = take(builder.seal());
      auto window =
          take(result.acquire_tensor(take(result.descriptor()), 0, region));
      for (std::uint64_t c = 0; c < 2; ++c)
        for (std::uint64_t y = 0; y < 3; ++y)
          for (std::uint64_t x = 0; x < 5; ++x) {
            const auto at = chw ? std::vector<std::uint64_t>{c, y, x}
                                : std::vector<std::uint64_t>{y, x, c};
            const auto rectangle = take(window.rectangle_run(at));
            PS_CHECK(rectangle.rows == std::min(3 - y, extent - y % extent));
            PS_CHECK(rectangle.row.samples ==
                     std::min(5 - x, extent - x % extent));
            PS_CHECK(rectangle.row.sample_stride_bytes == 1 &&
                     rectangle.row_stride_bytes ==
                         static_cast<std::int64_t>(
                             std::min(extent, 5 - (x / extent) * extent)));
            const auto index = chw ? (c * 3 + y) * 5 + x : (y * 5 + x) * 2 + c;
            PS_CHECK(*rectangle.row.data == bytes[index]);
          }
    }
    for (std::uint64_t invalid : {0, 3})
      PS_CHECK(ResultBuilder::start(root, image, "invalid.tiles", {}, invalid)
                   .status()
                   .code == ErrorCode::InvalidArgument);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
int sets_and_needs() {
  ResourceBudget root;
  {
    auto first = TileSet::from_indices(root, {UINT64_MAX - 1, 4, 1, 4});
    PS_REQUIRE_OK(first);
    ResourceBudget other_root;
    PS_CHECK(first.value().owned_by(root) &&
             !first.value().owned_by(other_root));
    PS_CHECK(TileSet{}.owned_by(root) && TileSet{}.owned_by(other_root));
    PS_CHECK(first.value().size() == 3 && first.value().contains(4) &&
             !first.value().contains(3));
    auto second = TileSet::from_indices(root, {2, 4}).take_value();
    auto joined = first.value().unite(second, root);
    PS_REQUIRE_OK(joined);
    PS_CHECK(
        std::vector<TileIndex>(joined.value().begin(), joined.value().end()) ==
        std::vector<TileIndex>({1, 2, 4, UINT64_MAX - 1}));
    auto copy = joined.value().copy_to(root);
    PS_REQUIRE_OK(copy);
    PS_CHECK(copy.value() == joined.value());
    for (bool descriptor : {false, true})
      for (bool payload : {false, true}) {
        TileNeed need{0, 0, descriptor, payload ? second : TileSet{}};
        PS_CHECK(need.validate().ok() == (!descriptor || !payload));
      }
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
    auto grids = TileGrid::members(schema({8, 8}), 4).take_value();
    TileNeed need{2, 0, false,
                  TileSet::from_indices(root, {1, 3}).take_value()};
    PS_CHECK(need.resolve(grids, root).value().tiles == need.tiles);
    PS_CHECK(need.resolve({}, root).status().code ==
             ErrorCode::InvalidArgument);
    TileRequest request{{{0, need.tiles}}};
    PS_CHECK(request.resolve(grids).value() == request);
    PS_CHECK(TileRequest{}.resolve({}).value().empty());
    PS_CHECK(request.resolve({}).status().code == ErrorCode::InvalidArgument);
    PS_CHECK(request.contains({}) && request.contains(request) &&
             !TileRequest{}.contains(request));
    auto all = TileRequest::all(grids, root).take_value();
    PS_CHECK(all.contains(request) && all.size() == 4);
    need.tiles = TileSet::from_indices(root, {4}).take_value();
    PS_CHECK(need.resolve(grids, root).status().code ==
             ErrorCode::InvalidArgument);
    PS_CHECK(TileSet::from_indices(root, nullptr, 1).status().code ==
             ErrorCode::InvalidArgument);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  ResourceLimits limits;
  limits.capacity[ResourceKind::Metadata] = 0;
  ResourceBudget bounded(limits);
  PS_CHECK(TileSet::from_indices(bounded, {1}).status().code ==
           ErrorCode::ResourceExhausted);
  limits = {};
  limits.maximum_work = 0;
  ResourceBudget no_work(limits);
  PS_CHECK(TileSet::from_indices(no_work, {1}).status().reason ==
           FailureReason::WorkLimit);
  PS_CHECK(TileSet::from_indices(no_work, {}).value().empty());
  return 0;
}
int sdk_conversion() {
  ResourceBudget root;
  {
    auto layout = schema({3, 5, 7}, {2, 3});
    layout.tensors[0].atomic_trailing_axes = 0;
    layout.tensors[0].layout.height_axis = 1;
    layout.tensors[0].layout.width_axis = 2;
    layout.tensors[0].layout.channel_axis = 0;
    auto grid = TileGrid::from_schema(layout, 4).take_value();
    auto set = sdk::tiles_for_region(
        root, grid, Region({{0, 2}, {1, 1}, {2, 1}, {3, 2}, {3, 2}}));
    PS_REQUIRE_OK(set);
    PS_CHECK(std::vector<TileIndex>(set.value().begin(), set.value().end()) ==
             std::vector<TileIndex>({4, 5, 6, 7, 16, 17, 18, 19}));
    auto samples = Footprint::from_regions(
                       layout.tensors[0].sample_shape(),
                       {Region({{1, 1}, {2, 1}, {0, 1}, {4, 1}, {4, 1}}),
                        Region({{1, 1}, {2, 1}, {2, 1}, {4, 1}, {6, 1}})})
                       .take_value();
    auto single = sdk::tiles_for_samples(root, grid, samples);
    PS_REQUIRE_OK(single);
    PS_CHECK(single.value().size() == 1 && single.value().contains(23));
    auto empty = sdk::tiles_for_region(
        root, grid, Region({{0, 1}, {0, 1}, {0, 3}, {5, 0}, {0, 7}}));
    PS_REQUIRE_OK(empty);
    PS_CHECK(empty.value().empty());
    PS_CHECK(sdk::tiles_for_region(root, grid, Region({{0, 1}, {0, 1}}))
                 .status()
                 .code == ErrorCode::InvalidArgument);
    PS_CHECK(sdk::tiles_for_region(
                 root, grid, Region({{0, 1}, {0, 1}, {0, 3}, {5, 1}, {0, 7}}))
                 .status()
                 .code == ErrorCode::InvalidArgument);
    auto enormous =
        TileGrid::from_schema(schema({UINT64_MAX, 1}), 1).take_value();
    auto sparse = sdk::tiles_for_region(root, enormous,
                                        Region({{UINT64_MAX - 1, 1}, {0, 1}}));
    PS_REQUIRE_OK(sparse);
    PS_CHECK(sparse.value().size() == 1 &&
             sparse.value().contains(UINT64_MAX - 1));
    auto too_many = sdk::tiles_for_region(
        root, enormous, Region({{0, UINT64_C(1) << 63}, {0, 1}}));
    PS_REQUIRE_OK(too_many);
    PS_CHECK(too_many.value().size() == (UINT64_C(1) << 63) &&
             too_many.value().intervals().size() == 1);
    ResourceLimits limits;
    limits.maximum_work = 1;
    ResourceBudget bounded(limits);
    PS_CHECK(sdk::tiles_for_samples(bounded, grid, samples).status().reason ==
             FailureReason::WorkLimit);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int sdk_needs() {
  ResourceBudget root;
  ResultProgramMetadata metadata;
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema({129, 257, 4}, {2}));
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  auto samples = Footprint::from_regions(
      {2, 129, 257, 4}, {Region({{1, 1}, {127, 2}, {255, 2}, {2, 1}})});
  PS_REQUIRE_OK(samples);
  auto selected =
      sdk::tile_need_for_samples(root, query, 0, 0, samples.value());
  PS_REQUIRE_OK(selected);
  PS_CHECK(selected.value().tensor_slot == 0 &&
           !selected.value().descriptor_only);
  PS_CHECK(std::vector<TileIndex>(selected.value().tiles.begin(),
                                  selected.value().tiles.end()) ==
           std::vector<TileIndex>({7, 8, 10, 11}));
  metadata.input_tile_grids.push_back(
      TileGrid::members(*metadata.inputs[0].result_schema, 64).take_value());
  selected = sdk::tile_need_for_samples(root, query, 0, 0, samples.value());
  PS_REQUIRE_OK(selected);
  PS_CHECK(std::vector<TileIndex>(selected.value().tiles.begin(),
                                  selected.value().tiles.end()) ==
           std::vector<TileIndex>({23, 24, 28, 29}));
  auto empty = Footprint::none({2, 129, 257, 4}).take_value();
  auto empty_need = sdk::tile_need_for_samples(root, query, 0, 0, empty);
  PS_REQUIRE_OK(empty_need);
  PS_CHECK(empty_need.value().tiles.empty() &&
           !empty_need.value().descriptor_only);
  auto descriptor = sdk::tile_need_for_samples(root, query, 0, 0, empty, true);
  PS_REQUIRE_OK(descriptor);
  PS_CHECK(descriptor.value().descriptor_only &&
           descriptor.value().tiles.empty());
  PS_CHECK(sdk::tile_need_for_samples(root, query, 1, 0, empty, true)
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(sdk::tile_need_for_samples(root, query, 0, 1, empty, true)
               .status()
               .code == ErrorCode::InvalidArgument);
  auto ordinary = schema({8});
  ordinary.tensors[0].layout.spatial = false;
  ordinary.tensors[0].layout.channel_axis.reset();
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(ordinary);
  metadata.input_tile_grids.clear();
  auto point = Footprint::from_regions({8}, {Region({{3, 1}})}).take_value();
  auto selected_point = sdk::tile_need_for_samples(root, query, 0, 0, point);
  PS_REQUIRE_OK(selected_point);
  PS_CHECK(selected_point.value().tiles.size() == 1 &&
           selected_point.value().tiles.contains(0));
  ResourceLimits exhausted;
  exhausted.maximum_work = 0;
  ResourceBudget no_work(exhausted);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema({129, 257, 4}, {2}));
  PS_CHECK(sdk::tile_need_for_samples(no_work, query, 0, 0, samples.value())
               .status()
               .reason == FailureReason::WorkLimit);
  return 0;
}
int planning() {
  auto operations = std::make_shared<OperationRegistry>();
  auto spatial = schema({129, 257, 4}, {2});
  auto ordinary = schema({3, 5});
  ordinary.tensors[0].layout.spatial = false;
  for (const auto* key : {"test.tiled", "test.second", "test.ordinary"}) {
    OperationDefinition definition;
    definition.key = key;
    definition.traits.input_count = 1;
    OperationPortConstraint port;

    port.result_schema_id = std::string(spatial.id);
    port.result_schema_version = spatial.version;
    definition.traits.input_schema = {port};
    definition.traits.outputs = {multi_result::output(
        "value", definition.key == "test.ordinary" ? ordinary : spatial)};
    definition.start_result = [](const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
      return ResultContinuation::make<multi_result::Program>(allocator, -1,
                                                             0.0);
    };
    const auto registered =
        operations->register_operation(std::move(definition));
    if (!registered.ok())
      std::cerr << registered.message << "\n";
    PS_CHECK(registered.ok());
  }
  PS_CHECK(operations->freeze().ok());
  Compiler compiler(operations);
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "source", spatial)};
  document.nodes = {{1, "test.tiled", {WorkflowInputReference{1}}, {}},
                    {2, "test.second", {WorkflowNodeOutput{1, "value"}}, {}},
                    {3, "test.ordinary", {WorkflowNodeOutput{2, "value"}}, {}}};
  document.outputs = {{"value", 3, "value"},
                      {"spatial", 1, "value"},
                      {"second", 2, "value"}};
  GraphContext graph(std::move(document));
  auto compiled = compiler.compile(graph);
  PS_REQUIRE_OK(compiled);
  const auto& plan = compiled.value().plan.value();
  PS_CHECK(plan.tile_extent() == 128 && plan.input_tile_grids().size() == 1 &&
           plan.input_tile_grids()[0][0].tile_count() == 12);
  PS_CHECK(plan.steps()[0].tile_grids[0].tile_count() == 12);
  PS_CHECK(plan.steps()[1].tile_grids[0].tile_count() == 12);
  PS_CHECK(plan.steps()[2].tile_grids[0].tile_count() == 1);
  const auto& metadata = *plan.steps()[1].structured_metadata;
  ResultProgramQuery query(metadata, plan.steps()[1].parameters);
  PS_CHECK(query.input_tile_grids.size() == 1 &&
           query.input_tile_grids[0][0].tile_count() == 12 &&
           query.output_tile_grids[0].tile_count() == 12);
  PlanningOptions options;
  options.tile_extent = 64;
  auto smaller =
      compiler.plan(compiled.value().ir_versions.back().ir.value(), options);
  PS_REQUIRE_OK(smaller);
  PS_CHECK(smaller.value().input_tile_grids()[0][0].tile_count() == 30 &&
           smaller.value().digest().value != plan.digest().value);
  PS_CHECK(smaller.value().structured_templates() !=
           plan.structured_templates());
  ResourceBudget root;
  const TileRequest selected{
      {{0, TileSet::from_indices(root, {0, 11}).take_value()}}};
  options.tile_extent = 128;
  options.output_requests = {{"spatial", selected}, {"second", selected}};
  auto narrowed =
      compiler.plan(compiled.value().ir_versions.back().ir.value(), options);
  PS_REQUIRE_OK(narrowed);
  PS_CHECK(narrowed.value().output_requests().at("spatial") == selected &&
           narrowed.value().output_requests().at("second") == selected &&
           narrowed.value().output_requests().at("value").size() == 1);
  const TileRequest last{{{0, TileSet::from_indices(root, {11}).take_value()}}};
  auto subset = narrowed.value().tile_plan("spatial", last);
  PS_REQUIRE_OK(subset);
  PS_CHECK(subset.value().outputs().size() == 1 &&
           subset.value().output_requests().at("spatial") == last &&
           subset.value().digest().value != narrowed.value().digest().value);
  PS_CHECK(narrowed.value().tile_plan("spatial", {}).ok());
  PS_CHECK(
      narrowed.value()
          .tile_plan("spatial",
                     {{{0, TileSet::from_indices(root, {1}).take_value()}}})
          .status()
          .code == ErrorCode::InvalidArgument);
  PS_CHECK(narrowed.value()
               .tile_plan("second", last)
               .value()
               .output_requests()
               .at("second") == last);
  PS_CHECK(narrowed.value().tile_plan("value", last).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(narrowed.value().tile_plan("missing", {}).status().code ==
           ErrorCode::InvalidArgument);
  for (const auto& invalid : std::vector<std::map<std::string, TileRequest>>{
           {{"spatial",
             {{{0, TileSet::from_indices(root, {12}).take_value()}}}}},
           {{"value", selected}},
           {{"spatial", {{{1, selected.members.at(0)}}}}},
           {{"missing", {}}}}) {
    options.output_requests = invalid;
    PS_CHECK(
        compiler.plan(compiled.value().ir_versions.back().ir.value(), options)
            .status()
            .code == ErrorCode::InvalidArgument);
  }
  options.output_requests = {{"spatial", {}}};
  auto empty =
      compiler.plan(compiled.value().ir_versions.back().ir.value(), options);
  PS_REQUIRE_OK(empty);
  PS_CHECK(empty.value().output_requests().at("spatial").empty());
  options.tile_extent = 3;
  PS_CHECK(
      compiler.plan(compiled.value().ir_versions.back().ir.value(), options)
          .status()
          .code == ErrorCode::InvalidArgument);
  return 0;
}
}  // namespace
int main() {
  return geometry() || storage_tiles() || sets_and_needs() ||
         sdk_conversion() || sdk_needs() || planning();
}
