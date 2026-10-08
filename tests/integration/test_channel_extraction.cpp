#include <array>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "channel_extraction_workflow/source.hpp"
#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using channel_fixture::check;
using channel_fixture::owner;
using channel_fixture::read;
using channel_fixture::require;
using channel_fixture::source;
using channel_fixture::Source;
using channel_fixture::take;
using Params = std::map<std::string, ParameterValue>;
Params parameters(std::int64_t axis, std::int64_t index, bool keep,
                  const std::string& layout = "auto") {
  return {{"axis", axis},
          {"index", index},
          {"keepdims", keep},
          {"layout", layout},
          {"metadata_mode", std::string("raw")}};
}
struct Fixture {
  Source input;
  std::shared_ptr<OperationRegistry> registry =
      make_default_operation_registry();
  std::unique_ptr<ExecutionContext> context;
  WorkflowDocument document;
  ExecutionBindings bindings;
  std::unique_ptr<GraphContext> graph;
  ResourceBindings resources;
  explicit Fixture(Source value, std::optional<ResourceLimits> limits = {})
      : input(std::move(value)) {
    ExecutionContextConfig config;
    config.managed_resources = limits;
    context = std::make_unique<ExecutionContext>(registry, config);
    document.inputs = {channel_fixture::declaration(input)};
    resources = input.resources;
    bind();
  }
  void bind() {
    bindings.inputs = {
        {"source",
         channel_fixture::publish(take(context->resource_budget()), input)}};
  }
  void node(Params params, bool named = false) {
    document.nodes = {{1,
                       named ? "channel.extract_named_strict"
                             : "channel.extract_index_strict",
                       {WorkflowInputReference{1}},
                       std::move(params)}};
    document.outputs = {{"result", 1, "values"}};
  }
  auto compile(const PlanningOptions& options = {}) {
    graph = std::make_unique<GraphContext>(document);
    return Compiler(registry).compile(*graph, options, resources);
  }
  Result<ExecutionResult> execute(const PlanningOptions& options = {},
                                  CancellationToken cancellation = {},
                                  ExecutionOptions execution = {}) {
    auto plan = compile(options);
    if (!plan.ok())
      return Result<ExecutionResult>(plan.status());
    execution.dependencies.maximum_work = 1000000000;
    execution.maximum_dependency_work = 1000000000;
    return context->execute(plan.value().plan, bindings, cancellation,
                            execution);
  }
};
std::vector<std::uint64_t> output_shape(const Source& source,
                                        unsigned cell_axis, bool keep) {
  auto shape = source.schema.tensors[0].sample_shape();
  const auto axis = source.schema.tensors[0].batch_axes.size() + cell_axis;
  if (keep)
    shape[axis] = 1;
  else
    shape.erase(shape.begin() + axis);
  return shape;
}
Region mapped(const Source& source, const Region& region, unsigned cell_axis,
              std::uint64_t index, bool keep) {
  auto dims = region.dimensions();
  const auto axis = source.schema.tensors[0].batch_axes.size() + cell_axis;
  if (keep)
    dims[axis] = {index, 1};
  else
    dims.insert(dims.begin() + axis, {index, 1});
  return Region(std::move(dims));
}
void oracle(const Source& source, const ExecutionResult& run, const Region& q,
            unsigned axis, std::uint64_t index, bool keep,
            const char* name = "result") {
  const auto& result = run.results.at(name);
  const auto shape = output_shape(source, axis, keep);
  check(result.schema().tensors[0].sample_shape() == shape,
        "inferred complete sample shape");
  const auto covered = take(result.descriptor()).tensor_coverage(0);
  check(covered == take(Footprint::from_regions(shape, {q})),
        "exact output coverage");
  const auto bytes = read(result, q);
  const auto width =
      Value::element_size(source.schema.tensors[0].descriptor.element_type);
  std::size_t next = 0;
  const auto physical_axis = source.schema.tensors[0].batch_axes.size() + axis;
  require(covered.visit(
      [&](const auto& at) {
        auto from = at;
        if (keep)
          from[physical_axis] = index;
        else
          from.insert(from.begin() + physical_axis, index);
        check(!std::memcmp(
                  bytes.data() + next,
                  source.bytes.data() + channel_fixture::address(source, from),
                  width),
              "independent bit-copy oracle");
        next += width;
        return Status::success();
      },
      UINT64_MAX));
}
TensorDescription bgra() {
  TensorDescription d;
  d.channel_axis = 2;
  d.channels = {{"B", "blue", "relative"},
                {"A", "coverage", "ratio"},
                {"R", "red", "relative"},
                {"G", "green", "relative"}};
  d.model = "rgb";
  d.transfer = "linear";
  return d;
}
void workflow() {
  auto input = source({ElementType::UInt8, {2, 2, 4}},
                      {take(encode_tensor_description(bgra()))});
  input.bytes = {30, 25, 10, 20, 31, 50, 11, 21,
                 32, 75, 12, 22, 33, 99, 13, 23};
  Fixture f(input);
  auto params = parameters(2, 1, false, "materialize");
  params["metadata_mode"] = std::string("respect");
  f.node(params);
  PlanningOptions planning;
  const Region roi({{1, 1}, {0, 2}});
  planning.output_regions = {{"result", roi}};
  auto run = take(f.execute(planning));
  check(read(run.results.at("result"), roi) ==
            std::vector<std::uint8_t>({75, 99}),
        "alpha ROI");
  auto d = take(decode_tensor_description(
      run.results.at("result").schema().tensors[0].facets[0]));
  check(!d.channel_axis && d.component->name == "A", "projected component");
  params.erase("index");
  params["match"] = std::string("role");
  params["selector"] = std::string("red");
  params["layout"] = std::string("view");
  f.node(params, true);
  run = take(f.execute(planning));
  oracle(input, run, roi, 2, 2, false);
  f.document.nodes.clear();
  OperationMetadata metadata;
  metadata.result_schema = f.document.inputs[0].result_schema;
  auto handles = take(
      format::split_channels(f.document, WorkflowInputReference{1}, metadata));
  check(handles.size() == 4 && handles[2].name == "c2", "split handles");
  f.document.outputs = {{"result", handles[2].output.source_node, "values"}};
  run = take(f.execute(planning));
  oracle(input, run, roi, 2, 2, false);
  for (const auto& timing : run.diagnostics.operation_timings)
    check(timing.output.node_id == handles[2].output.source_node,
          "unrequested split sibling");
  // Reuse one static preparation concurrently with independent runtime state.
  auto plan = take(f.compile(planning));
  auto first = std::async(std::launch::async, [&] {
    return f.context->execute(plan.plan, f.bindings);
  });
  auto second = f.context->execute(plan.plan, f.bindings);
  oracle(input, take(first.get()), roi, 2, 2, false);
  oracle(input, take(std::move(second)), roi, 2, 2, false);
}
void arbitrary_axis_oracle() {
  std::size_t cases = 0;
  for (auto type : {ElementType::UInt8, ElementType::UInt16, ElementType::Int8,
                    ElementType::Int16, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64})
    for (const auto& shape :
         {std::vector<std::uint64_t>{3, 2, 4},
          std::vector<std::uint64_t>{2, 1, 2, 1, 2, 1, 2, 2},
          std::vector<std::uint64_t>{4}})
      for (unsigned axis = 0; axis < shape.size(); ++axis)
        for (bool keep : {false, true}) {
          if (!keep && shape.size() == 1)
            continue;
          auto input = source({type, shape});
          if (type == ElementType::Float32) {
            const std::uint32_t bits[] = {0x7f800001, 0x7fc12345, 0x80000000,
                                          0xff800000};
            for (std::size_t i = 0; i < input.bytes.size(); i += 4)
              std::memcpy(input.bytes.data() + i, &bits[(i / 4) % 4], 4);
          }
          if (type == ElementType::Float64) {
            const std::uint64_t bits[] = {
                0x7ff0000000000001, 0x7ff8000000012345, 0x8000000000000000,
                0xfff0000000000000};
            for (std::size_t i = 0; i < input.bytes.size(); i += 8)
              std::memcpy(input.bytes.data() + i, &bits[(i / 8) % 4], 8);
          }
          Fixture f(input);
          const auto out_shape = output_shape(input, axis, keep);
          auto dims = Region::whole(out_shape).dimensions();
          for (auto& d : dims)
            if (d.extent > 1) {
              d.offset = 1;
              --d.extent;
            }
          const Region roi(dims);
          PlanningOptions planning;
          planning.output_regions = {{"result", roi}};
          const auto selected = shape[axis] - 1;
          for (const auto* policy : {"auto", "view", "materialize"}) {
            f.node(parameters(axis, selected, keep, policy));
            auto result = take(f.execute(planning));
            oracle(input, result, roi, axis, selected, keep);
            check((owner(result.results.at("result"), roi) ==
                   owner(f.bindings.inputs[0].result,
                         mapped(input, roi, axis, selected, keep))) ==
                      (std::string(policy) != "materialize"),
                  "generic view/copy owners");
            ++cases;
          }
        }
  std::cout << "arbitrary-axis bit-copy cases=" << cases << '\n';
}
void planar_and_batches() {
  for (bool chw : {false, true})
    for (bool tiled : {false, true})
      for (bool keep : {false, true}) {
        ResultTensorLayout layout;
        layout.spatial = true;
        layout.order =
            tiled ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
        layout.height_axis = chw ? 1 : 0;
        layout.width_axis = chw ? 2 : 1;
        layout.channel_axis = chw ? 0 : 2;
        layout.groups = {{"components", 0, 3}};
        if (!tiled)
          layout.row_pitch_bytes = 160;
        const auto cell = chw ? std::vector<std::uint64_t>{3, 130, 131}
                              : std::vector<std::uint64_t>{130, 131, 3};
        auto input = source({ElementType::UInt8, cell}, {}, layout, {2, 2});
        const auto axis = *layout.channel_axis;
        auto dims = Region::whole(output_shape(input, axis, keep)).dimensions();
        dims[0] = {0, 2};
        dims[1] = {1, 1};
        auto h = 2 + layout.height_axis -
                 (!keep && axis < layout.height_axis ? 1 : 0);
        auto w =
            2 + layout.width_axis - (!keep && axis < layout.width_axis ? 1 : 0);
        dims[h] = {127, 3};
        dims[w] = {126, 4};
        const Region roi(dims);
        input.coverage = {mapped(input, roi, axis, 1, keep)};
        Fixture f(input);
        PlanningOptions planning;
        planning.output_regions = {{"result", roi}};
        for (const auto* policy : {"view", "materialize", "auto"}) {
          f.node(parameters(axis, 1, keep, policy));
          auto run = take(f.execute(planning));
          oracle(input, run, roi, axis, 1, keep);
          const auto& groups =
              run.results.at("result").schema().tensors[0].layout.groups;
          check(keep ? (groups.size() == 1 && groups[0].role == "components" &&
                        groups[0].first_channel == 0 &&
                        groups[0].channel_count == 1)
                     : groups.empty(),
                "physical channel group projection");
          auto plane = roi.dimensions();
          plane[0].extent = 1;
          check((owner(run.results.at("result"), Region(plane)) ==
                 owner(f.bindings.inputs[0].result,
                       mapped(input, Region(plane), axis, 1, keep))) ==
                    (std::string(policy) != "materialize"),
                "planar batch owner");
          const auto support = take(Footprint::from_regions(
              input.schema.tensors[0].sample_shape(), input.coverage));
          check(take(run.dependencies.source_support()).at("source") == support,
                "exact single-plane Need across tiles and batches");
          const auto query = take(
              Footprint::from_regions(output_shape(input, axis, keep), {roi}));
          require(take(run.results.at("result").tensor_relation(0))
                      .project(query, [&](auto s, const Footprint* samples) {
                        check(s.roles == 1 &&
                                  s.target == ResultSupportTarget::Tensor &&
                                  samples && *samples == support,
                              "Data-only exact extraction relation");
                        return Status::success();
                      }));
          auto wrong_channel = input.coverage[0].dimensions();
          wrong_channel[2 + axis].offset = 0;
          auto dirty = take(run.dependencies.potential_dirty(
              "source",
              take(Footprint::from_regions(
                  input.schema.tensors[0].sample_shape(),
                  {Region(wrong_channel)})),
              1, {}, ResultSupportTarget::Tensor, 0));
          check(dirty.at("result").empty(), "unselected channel is clean");
          dirty = take(run.dependencies.potential_dirty(
              "source", support, 1, {}, ResultSupportTarget::Tensor, 0));
          check(dirty.at("result") == query,
                "selected source maps to exact output");
        }
        auto unavailable = roi.dimensions();
        unavailable[w] = {0, 1};
        planning.output_regions = {{"result", Region(unavailable)}};
        check(!f.execute(planning).ok(),
              "missing source coverage cannot become a view");
      }
}
void spatial_axes() {
  for (bool width_slice : {false, true})
    for (bool tiled : {false, true})
      for (bool keep : {false, true}) {
        ResultTensorLayout layout;
        layout.spatial = true;
        layout.order =
            tiled ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
        layout.height_axis = width_slice ? 2 : 0;
        layout.width_axis = width_slice ? 0 : 1;
        layout.channel_axis = width_slice ? 1 : 2;
        layout.groups = {{"components", 0, width_slice ? 3U : 4U}};
        if (!tiled)
          layout.row_pitch_bytes = 32;
        auto input = source({ElementType::UInt16,
                             width_slice ? std::vector<std::uint64_t>{5, 3, 2}
                                         : std::vector<std::uint64_t>{2, 3, 4}},
                            {}, layout);
        Fixture f(input);
        auto q = Region::whole(output_shape(input, 0, keep));
        PlanningOptions planning;
        planning.output_regions = {{"result", q}};
        for (const auto* policy : {"auto", "materialize"}) {
          f.node(parameters(0, 1, keep, policy));
          oracle(input, take(f.execute(planning)), q, 0, 1, keep);
        }
        f.node(parameters(0, 1, keep, "view"));
        auto refused = f.execute(planning);
        check(!refused.ok() &&
                  refused.status().code == ErrorCode::InvalidArgument &&
                  refused.status().reason == FailureReason::InvalidDomain,
              "planar spatial slicing requires copying");
      }
}
void strided_and_disjoint() {
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto input = source({ElementType::UInt8, {2, 3}});
    input.bytes = {0, 1, 2, 3, 4, 5};
    input.layout = kind == 0   ? StridedLayout{2, {3, -1}}
                   : kind == 1 ? StridedLayout{0, {0, 1}}
                               : StridedLayout{2, {3, 1}, {0, 2}};
    Fixture f(input);
    for (const auto* policy : {"view", "materialize"}) {
      f.node(parameters(1, 1, false, policy));
      oracle(input, take(f.execute()), Region::whole({2}), 1, 1, false);
    }
  }
  ResultTensorLayout spatial;
  spatial.spatial = true;
  auto input = source({ElementType::UInt8, {1, 3, 2}}, {}, spatial);
  input.coverage = {Region({{0, 1}, {0, 1}, {1, 1}}),
                    Region({{0, 1}, {2, 1}, {1, 1}})};
  Fixture f(input);
  f.node(parameters(2, 1, false, "view"));
  f.document.outputs = {{"left", 1, "values"}, {"right", 1, "values"}};
  PlanningOptions planning;
  planning.output_regions = {{"left", Region({{0, 1}, {0, 1}})},
                             {"right", Region({{0, 1}, {2, 1}})}};
  auto run = take(f.execute(planning));
  oracle(input, run, planning.output_regions.at("left"), 2, 1, false, "left");
  oracle(input, run, planning.output_regions.at("right"), 2, 1, false, "right");
  check(take(run.dependencies.source_support()).at("source") ==
            take(Footprint::from_regions(input.schema.tensors[0].sample_shape(),
                                         input.coverage)),
        "disjoint gaps remain unread");
}
void fragmented_views() {
  for (bool mixed : {false, true}) {
    ResultTensorLayout layout;
    layout.spatial = mixed;
    auto input =
        source({ElementType::UInt8, mixed ? std::vector<std::uint64_t>{2, 3, 2}
                                          : std::vector<std::uint64_t>{2, 4}},
               {}, layout);
    Fixture f(input);
    const auto root = take(f.context->resource_budget());
    auto builder =
        take(ResultBuilder::start(root, input.schema, "fragmented.source"));
    require(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
    const auto shape = input.schema.tensors[0].sample_shape();
    auto relation = take(ResultRelation::cartesian(
        root, take(input.schema.tensors[0].sample_count()), {0, 1, 0, 0}));
    for (std::uint64_t row : {0, 1}) {
      auto dims = Region::whole(shape).dimensions();
      dims[0] = {row, 1};
      const Region region(dims);
      if (mixed && row == 1) {
        require(builder.publish_tensor_kernel(
            0, region,
            [&](const auto& writers) {
              for (const auto& writer : writers) {
                auto fp =
                    take(Footprint::from_regions(shape, {writer.region()}));
                require(fp.visit(
                    [&](const auto& at) {
                      auto run = take(writer.row_run(at));
                      *run.data =
                          input.bytes[channel_fixture::address(input, at)];
                      return Status::success();
                    },
                    UINT64_MAX));
              }
              return Status::success();
            },
            relation, {true, true, true, true}));
      } else {
        const auto count = take(region.element_count());
        auto storage = take(root.allocator().allocate(count));
        std::memcpy(storage.data(), input.bytes.data() + row * count, count);
        auto physical = input.layout;
        physical.origin.assign(shape.size(), 0);
        physical.origin[0] = row;
        require(builder.publish_tensor(0, region, physical,
                                       std::move(storage).freeze(), relation,
                                       {true, true, true, true}));
      }
    }
    f.bindings.inputs[0].result = take(builder.seal());
    const auto axis = mixed ? 2U : 1U;
    f.node(parameters(axis, 1, false, "view"));
    auto selected = WorkflowNodeOutput{1, "values"};
    format::MetadataOptions edit;
    edit.layout = "view";
    auto assigned = take(format::assign_metadata(f.document, selected, edit));
    f.document.outputs = {{"result", assigned.source_node, "values"}};
    auto run = take(f.execute());
    const auto q = Region::whole(output_shape(input, axis, false));
    oracle(input, run, q, axis, 1, false);
    for (std::uint64_t row : {0, 1}) {
      auto dims = q.dimensions();
      dims[0] = {row, 1};
      check(owner(run.results.at("result"), Region(dims)) ==
                owner(f.bindings.inputs[0].result,
                      mapped(input, Region(dims), axis, 1, false)),
            "fragmented/mixed view chain retains each actual backing");
    }
  }
}
void preflight_and_split() {
  Fixture f(source({ElementType::UInt8, {2, 3}}));
  auto params = parameters(1, 0, false);
  for (const auto& invalid : {std::make_pair("axis", std::int64_t{-1}),
                              {"axis", std::int64_t{2}},
                              {"index", std::int64_t{-1}},
                              {"index", std::int64_t{3}}}) {
    auto bad = params;
    bad[invalid.first] = invalid.second;
    f.node(bad);
    auto result = f.compile();
    check(!result.ok() && result.status().code == ErrorCode::InvalidArgument &&
              result.status().reason == FailureReason::InvalidDomain,
          "selector preflight status");
  }
  f.node(params);
  TensorDescription bad_table;
  bad_table.channel_axis = 1;
  bad_table.channels = {{"A", "a", ""}, {"B", "b", ""}};
  auto schema = f.input.schema;
  schema.tensors[0].facets = {take(encode_tensor_description(bad_table))};
  f.document.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  check(f.compile().status().code == ErrorCode::TypeMismatch,
        "invalid table length");
  Fixture named(source({ElementType::UInt8, {2, 2, 4}},
                       {take(encode_tensor_description(bgra()))}));
  auto named_params = parameters(2, 0, false);
  named_params.erase("index");
  named_params["metadata_mode"] = std::string("respect");
  named_params["match"] = std::string("name");
  named_params["selector"] = std::string("missing");
  named.node(named_params, true);
  check(!named.compile().ok(), "absent named selector");
  named.document.nodes[0].parameters["selector"] = std::string("R");
  named.document.nodes[0].parameters["metadata_mode"] = std::string("raw");
  check(!named.compile().ok(), "raw has no name lookup");
  auto ambiguous = bgra();
  ambiguous.channels[1].name = "R";
  schema = named.input.schema;
  schema.tensors[0].facets = {take(encode_tensor_description(ambiguous))};
  named.document.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  named.document.nodes[0].parameters["metadata_mode"] = std::string("respect");
  check(!named.compile().ok(), "ambiguous named selector");
  named.document.inputs = {channel_fixture::declaration(named.input)};
  named.document.nodes.clear();
  OperationMetadata metadata;
  metadata.result_schema = named.document.inputs[0].result_schema;
  format::ChannelExtractOptions options;
  options.layout = "bad";
  auto count = named.document.nodes.size();
  check(!format::split_channels(named.document, WorkflowInputReference{1},
                                metadata, options)
                .ok() &&
            named.document.nodes.size() == count,
        "split invalid layout is transactional");
  options = {};
  auto wrong = *metadata.result_schema;
  wrong.tensors[0].batch_axes = {2};
  metadata.result_schema = std::make_shared<const SchemaTemplate>(wrong);
  check(!format::split_channels(named.document, WorkflowInputReference{1},
                                metadata, options)
                .ok() &&
            named.document.nodes.empty(),
        "split declared batch metadata mismatch");
  named.document.nodes = {
      {99, "core.identity", {WorkflowInputReference{1}}, {}}};
  auto handles = take(format::split_channels(
      named.document, WorkflowNodeOutput{99, "value"}, metadata, options));
  named.document.outputs = {
      {"result", handles[0].output.source_node, "values"}};
  check(named.compile().status().code == ErrorCode::InvalidArgument,
        "forward producer schema assertion rejects batch drift");
  auto large_input =
      source({ElementType::UInt8, {2, 3}},
             {{"app.first", 1, std::vector<std::uint8_t>(4096, 1)},
              {"app.second", 1, std::vector<std::uint8_t>(4096, 2)},
              {"app.third", 1, std::vector<std::uint8_t>(4096, 3)}});
  check(large_input.schema.canonical_size() > 8192,
        "large static metadata fixture");
  Fixture large(large_input);
  metadata.result_schema = large.document.inputs[0].result_schema;
  options = {};
  options.axis = 1;
  options.metadata_mode = "raw";
  handles = take(format::split_channels(
      large.document, WorkflowInputReference{1}, metadata, options));
  check(std::get<std::string>(
            large.document.nodes[0].parameters.at("expected_source_schema"))
                .size() == 64,
        "complete source schema assertion stays within static string capacity");
  large.document.outputs = {
      {"result", handles[0].output.source_node, "values"}};
  oracle(large_input, take(large.execute()), Region::whole({2}), 1, 0, false);
  auto altered = large_input.schema;
  altered.tensors[0].facets[0].payload[0] ^= 1;
  large.document.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(altered);
  check(large.compile().status().code == ErrorCode::InvalidArgument,
        "bounded digest assertion still detects unrelated metadata drift");
  Fixture rank_one(source({ElementType::UInt8, {4}}));
  rank_one.node(parameters(0, 0, false));
  check(!rank_one.compile().ok(), "rank-one squeeze rejected");
  rank_one.node(parameters(0, 0, true));
  rank_one.document.nodes[0].operation =
      "channel.extract_index_accelerated_apple_silicon";
#if defined(__aarch64__) && defined(__APPLE__)
  oracle(rank_one.input, take(rank_one.execute()), Region::whole({1}), 0, 0,
         true);
  rank_one.document.nodes[0].operation =
      "channel.extract_index_accelerated_x86_64";
  check(!rank_one.compile().ok(), "unavailable CPU profile rejected");
#endif
}
void resource_and_metadata() {
  ResourceBudget budget;
  auto bytes = numeric_fixture::fixture();
  auto profile =
      take(IccProfile::import(ByteView(bytes.data(), bytes.size()), budget));
  const auto identity = profile.identity();
  auto resources = take(ResourceBindings::create({profile}, budget));
  TensorDescription d;
  d.channel_axis = 2;
  d.channels = {{"C", "cyan", "relative"},
                {"M", "magenta", "relative"},
                {"Y", "yellow", "relative"},
                {"K", "black", "relative"}};
  d.model = "cmyk";
  d.profile = identity;
  d.white = std::array<double, 2>{.3127, .3290};
  d.primaries_xy = std::array<double, 6>{.64, .33, .30, .60, .15, .06};
  auto facet = take(encode_tensor_description(d));
  auto plain = source({ElementType::UInt8, {1, 1, 4}});
  plain.bytes = {1, 2, 3, 4};
  auto described = plain;
  described.schema.tensors[0].facets = {facet};
  bool denied = false;
  try {
    (void)channel_fixture::publish(budget, described);
  } catch (const std::runtime_error&) {
    denied = true;
  }
  check(denied, "profile metadata cannot manufacture a resource");
  ResultRef surviving;
  for (bool spatial : {false, true}) {
    for (const auto* policy : {"view", "materialize"}) {
      auto input = plain;
      input.schema.tensors[0].layout.spatial = spatial;
      Fixture f(input);
      f.resources = resources;
      auto p = parameters(2, 3, false, policy);
      p["metadata_mode"] = std::string("override");
      p["metadata_override"] = take(tensor_description_parameter(d));
      f.node(p);
      auto result = take(f.execute());
      surviving = result.results.at("result");
      check(surviving.resources().icc_profile(identity).ok() &&
                read(surviving, Region::whole({1, 1})) ==
                    std::vector<std::uint8_t>{4},
            "override retains profile owner");
      auto out = take(
          decode_tensor_description(surviving.schema().tensors[0].facets[0]));
      check(out.component->name == "K" && out.profile == identity &&
                out.white == d.white && out.primaries_xy == d.primaries_xy,
            "component interpretation projected without conversion");
    }
  }
  resources = {};
  profile = {};
  check(surviving.resources().icc_profile(identity).ok(),
        "profile survives producer/context retirement");
  surviving = {};
  check(budget.statistics().live[ResourceKind::Host] == 0,
        "last Result releases profile budget");

  OcioConfigSnapshot snapshot;
  snapshot.config = {'c'};
  snapshot.spaces = {{"working", "scene"}};
  snapshot.build_identity = "fixture-build";
  snapshot.settings = "reference";
  auto config = take(OcioConfigResource::import(snapshot, budget));
  Fixture f(source({ElementType::Float32, {2, 3, 4}}));
  f.resources = take(ResourceBindings::create({}, {config}, budget));
  TensorDescription described_rgb = bgra();
  described_rgb.model.clear();
  described_rgb.transfer.clear();
  TensorColorGroup group;
  group.name = "rgb";
  group.indices = {2, 3, 0};
  group.components = {described_rgb.channels[2], described_rgb.channels[3],
                      described_rgb.channels[0]};
  group.alpha = 1;
  group.interpretation.model = "rgb";
  group.interpretation.convention = "ocio-native";
  group.interpretation.configured =
      TensorConfiguredSpace{config.identity(), "working", "scene"};
  described_rgb.groups = {group};
  format::MetadataOptions edit;
  edit.mode = "replace";
  edit.description = described_rgb;
  auto assigned = take(
      format::assign_metadata(f.document, WorkflowInputReference{1}, edit));
  auto p = parameters(2, 2, false, "view");
  p["metadata_mode"] = std::string("respect");
  f.document.nodes.push_back(
      {99, "channel.extract_index_strict", {assigned}, p});
  f.document.outputs = {{"result", 99, "values"}};
  auto observed = take(f.execute());
  auto component = take(decode_tensor_description(
      observed.results.at("result").schema().tensors[0].facets[0]));
  check(component.groups.empty() &&
            component.component->interpretation->configured ==
                group.interpretation.configured &&
            observed.results.at("result").resources().config_count() == 1,
        "metadata-to-extraction preserves configured interpretation/resource");
}
void atomic_observations() {
  auto input = source({ElementType::UInt8, {2, 3}});
  input.schema.tensors[0].atomic_trailing_axes = 1;
  Fixture f(input);
  for (unsigned axis : {0, 1}) {
    f.node(parameters(axis, 1, false, "view"));
    auto result = take(f.execute());
    oracle(input, result, Region::whole(output_shape(input, axis, false)), axis,
           1, false);
    check(
        result.results.at("result").schema().tensors[0].atomic_trailing_axes ==
            (axis == 0 ? 1U : 0U),
        "atomic cell axis projection");
  }
}
void sparse_huge_domain() {
  auto registry = make_default_operation_registry();
  ExecutionContext context(registry);
  const auto root = take(context.resource_budget());
  SchemaTemplate schema;
  schema.id = "test.huge-channel";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::UInt8, {UINT64_MAX, 2}};
  schema.tensors.push_back(std::move(tensor));
  auto builder = take(ResultBuilder::start(root, schema, "sparse.source"));
  require(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  auto data = take(root.allocator().allocate(1));
  *data.data() = 73;
  require(builder.publish_tensor(
      0, Region({{UINT64_MAX - 1, 1}, {1, 1}}),
      {0, {0, 0}, {UINT64_MAX - 1, 1}}, std::move(data).freeze(),
      take(ResultRelation::cartesian(root, UINT64_MAX, {0, 1, 0, 0})),
      {true, true, true, true}));
  auto source_result = take(builder.seal());
  WorkflowDocument document;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema = std::make_shared<const SchemaTemplate>(schema);
  document.inputs = {input};
  document.nodes = {{1,
                     "channel.extract_index_strict",
                     {WorkflowInputReference{1}},
                     parameters(1, 1, false, "view")}};
  document.outputs = {{"result", 1, "values"}};
  PlanningOptions planning;
  const Region roi({{UINT64_MAX - 1, 1}});
  planning.output_regions = {{"result", roi}};
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph, planning));
  auto run =
      take(context.execute(compiled.plan, {{{"source", source_result}}}));
  check(read(run.results.at("result"), roi) == std::vector<std::uint8_t>{73},
        "sparse domain need not have representable full sample count");
}
void limits_empty_lifetime() {
  auto input = source({ElementType::UInt8, {2, 3}});
  Fixture f(input);
  f.node(parameters(1, 1, false, "view"));
  auto plan = take(f.compile());
  auto frozen = take(f.context->freeze(plan.plan, f.bindings));
  const auto root = take(f.context->resource_budget());
  auto before = root.statistics().peak[ResourceKind::Payload];
  auto empty = take(f.context->execute_fragments(
      frozen, {{"result", take(Footprint::none({2}))}}));
  check(take(empty.results.at("result").descriptor())
                .tensor_coverage(0)
                .empty() &&
            root.statistics().peak[ResourceKind::Payload] == before,
        "Empty has no sample/state Payload");
  CancellationSource cancelled;
  cancelled.cancel();
  check(f.context->execute(plan.plan, f.bindings, cancelled.token())
                .status()
                .code == ErrorCode::Cancelled,
        "precancelled invocation");
  ExecutionOptions work;
  work.maximum_dependency_work = 1;
  check(f.context->execute(plan.plan, f.bindings, {}, work).status().code ==
            ErrorCode::ResourceExhausted,
        "work exhaustion");
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = 6;
  Fixture small(input, limits);
  small.node(parameters(1, 1, false, "view"));
  check(small.execute().status().code == ErrorCode::ResourceExhausted,
        "source-only capacity cannot admit continuation");
  check(take(small.context->resource_budget())
                .statistics()
                .live[ResourceKind::Payload] == 6,
        "failed continuation leaves only source");
  std::optional<ResultTensorReadWindow> retained;
  ResourceBudget owned;
  {
    ResultTensorLayout layout;
    layout.spatial = true;
    auto source_image = source({ElementType::UInt8, {1, 1, 2}}, {}, layout);
    source_image.bytes = {1, 23};
    Fixture owner_fixture(source_image);
    owned = take(owner_fixture.context->resource_budget());
    owner_fixture.node(parameters(2, 1, false, "view"));
    auto run = take(owner_fixture.execute());
    const auto& result = run.results.at("result");
    retained.emplace(take(result.acquire_tensor(take(result.descriptor()), 0,
                                                Region::whole({1, 1}))));
  }
  check(owned.statistics().live[ResourceKind::Payload] > 0 &&
            *take(retained->row_run({0, 0})).data == 23,
        "planar view window outlives all Result/context owners");
  retained.reset();
  check(owned.statistics().live[ResourceKind::Payload] == 0,
        "last window releases planar backing");
}
}  // namespace
int main() try {
  workflow();
  arbitrary_axis_oracle();
  planar_and_batches();
  spatial_axes();
  strided_and_disjoint();
  fragmented_views();
  preflight_and_split();
  resource_and_metadata();
  atomic_observations();
  sparse_huge_domain();
  limits_empty_lifetime();
  std::cout << "FMT-01 Result extraction, split, exact support, views and "
               "resources passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
