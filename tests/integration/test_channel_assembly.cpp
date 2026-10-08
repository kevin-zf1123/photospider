#include <algorithm>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/photospider.hpp"
#include "support/channel_result_fixture.hpp"

namespace {
using namespace ps;                // NOLINT(build/namespaces)
using namespace assembly_fixture;  // NOLINT(build/namespaces)
void generic_oracle() {
  for (auto dtype : {ElementType::UInt8, ElementType::UInt16, ElementType::Int8,
                     ElementType::Int16, ElementType::Int64,
                     ElementType::Float32, ElementType::Float64}) {
    for (unsigned rank = 1; rank <= 7; ++rank)
      for (unsigned axis = 0; axis <= rank; ++axis) {
        Fixture f;
        std::vector<std::uint64_t> shape(rank, 2);
        auto x = f.add({dtype, shape}), y = f.add({dtype, shape});
        format::ChannelAssemblyOptions options;
        options.metadata_mode = "raw";
        options.layout = "materialize";
        auto edge =
            take(format::assemble_channels(f.document, {x, y}, axis, options));
        auto output_shape = shape;
        output_shape.insert(output_shape.begin() + axis, 2);
        auto roi = Region::whole(output_shape);
        auto result = run(f, edge);
        check_oracle(result, f, axis, {std::nullopt, std::nullopt},
                     {{0, 0}, {1, 0}}, roi);
      }
    for (unsigned rank = 1; rank <= 4; ++rank)
      for (unsigned axis = 0; axis < rank; ++axis) {
        Fixture f;
        std::vector<std::uint64_t> shape(rank - 1, 2);
        auto a = shape, b = shape;
        a.insert(a.begin(), 2);
        b.push_back(1);
        auto x = f.add({dtype, a}), y = f.add({dtype, b});
        format::ChannelAssemblyOptions options;
        options.metadata_mode = "raw";
        options.layout = "materialize";
        auto edge = take(format::concatenate_channels(f.document, {x, y}, axis,
                                                      {0, rank - 1}, options));
        auto out = shape;
        out.insert(out.begin() + axis, 3);
        auto region = Region::whole(out);
        auto result = run(f, edge);
        check_oracle(result, f, axis, {0, rank - 1}, {{0, 0}, {0, 1}, {1, 0}},
                     region);
      }
  }
}
void mapped_and_metadata() {
  TensorDescription d;
  d.channel_axis = 0;
  d.channels = {{"R", "red", "relative"},
                {"G", "green", "relative"},
                {"B", "blue", "relative"}};
  d.model = "rgb";
  d.primaries = "srgb";
  d.transfer = "linear";
  Fixture f;
  auto x =
      f.add({ElementType::UInt8, {3, 2}}, {take(encode_tensor_description(d))});
  TensorDescription component;
  component.component = TensorChannelDescription{"A", "alpha", "coverage"};
  auto y = f.add({ElementType::UInt8, {2}},
                 {take(encode_tensor_description(component))});
  format::ChannelAssemblyOptions options;
  options.layout = "materialize";
  TensorDescription target;
  target.channel_axis = 1;
  target.channels.resize(3);
  target.channels[0].name = "renamed";
  TensorInterpretation lab;
  lab.model = "cielab";
  lab.white = std::array<double, 2>{.3127, .3290};
  target.channels[2].role = "b";
  target.channels[2].interpretation = lab;
  options.output_description = target;
  std::vector<format::ChannelMapping> rows = {{0, "name", "R", 2, {}},
                                              {1, "role", "alpha", 1, {}},
                                              {0, "index", "0", 0, {}}};
  auto edge = take(format::assemble_mapped_channels(
      f.document, {x, y}, 1, {{false, 0}, {true, {}}}, rows, options));
  auto result = run(f, edge);
  check_oracle(result, f, 1, {0, {}}, {{0, 0}, {1, 0}, {0, 0}},
               Region::whole({2, 3}));
  auto output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(output.channels[2].interpretation->model == "cielab" &&
              output.channels[2].interpretation->primaries.empty(),
          "target removes incompatible RGB fields");
  require(output.channels[0].interpretation->primaries == "srgb",
          "unredefined source fields retained");
  require(
      take(decode_tensor_description(facets(f.bindings.inputs[0].result)[0]))
              .model == "rgb",
      "source immutable");
  // Explicit group reinterprets three Gray components without numeric
  // conversion.
  Fixture gray;
  TensorDescription g;
  g.model = "gray";
  g.transfer = "linear";
  g.component = TensorChannelDescription{"Y", "gray", "relative"};
  std::vector<WorkflowInput> inputs;
  for (int i = 0; i < 3; ++i)
    inputs.push_back(gray.add({ElementType::Float32, {2}},
                              {take(encode_tensor_description(g))}));
  TensorDescription rgb;
  rgb.channel_axis = 1;
  TensorColorGroup group;
  group.name = "color";
  group.indices = {0, 1, 2};
  group.components = {{"R", "red", "relative"},
                      {"G", "green", "relative"},
                      {"B", "blue", "relative"}};
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  group.interpretation.association = "straight";
  rgb.groups = {group};
  options.output_description = rgb;
  edge = take(format::assemble_channels(gray.document, inputs, 1, options));
  result = run(gray, edge);
  check_oracle(result, gray, 1, {{}, {}, {}}, {{0, 0}, {1, 0}, {2, 0}},
               Region::whole({2, 3}));
  auto decoded = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(decoded.groups.size() == 1 &&
              decoded.channels[0].interpretation->model == "rgb",
          "explicit RGB group");
  auto encoded = take(encode_tensor_description(rgb));
  require(encoded.version == 4, "version discriminator");
  encoded.version = 1;
  require(!decode_tensor_description(encoded).ok(), "old version rejection");
}
void auto_fragment_oracle() {
  Fixture fixture;
  auto a = fixture.add({ElementType::UInt8, {257}});
  auto b = fixture.add({ElementType::UInt8, {257}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  auto edge =
      take(format::assemble_channels(fixture.document, {a, b}, 0, options));
  fixture.document.outputs = {{"result", edge.source_node, edge.source_port}};
  auto registry = make_default_operation_registry();
  GraphContext graph(fixture.document);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContext context(registry);
  fixture.bind_to(context);
  auto dense_result = take(context.execute(compiled.plan, fixture.bindings));
  check_oracle(dense_result, fixture, 0, {{}, {}}, {{0, 0}, {1, 0}},
               Region::whole({2, 257}));
  auto frozen = take(context.freeze(compiled.plan, fixture.bindings));
  auto result = take(context.execute_fragments(
      frozen, {{"result", take(Footprint::all({2, 257}))}}));
  check_oracle(result, fixture, 0, {{}, {}}, {{0, 0}, {1, 0}},
               Region::whole({2, 257}));
}
void shared_owner_alias_oracle() {
  Fixture fixture;
  auto input = fixture.add({ElementType::UInt8, {2, 3}});
  OperationMetadata metadata;
  metadata = fixture.metadata(0);
  format::ChannelExtractOptions extraction;
  extraction.axis = 1;
  extraction.metadata_mode = "raw";
  auto handles = take(
      format::split_channels(fixture.document, input, metadata, extraction));
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = "view";
  auto edge = take(format::assemble_channels(
      fixture.document, {handles[2].output, handles[0].output}, 1, options));
  auto result = run(fixture, edge);
  const auto& value = result.results.at("result");
  require(owner(value) == owner(fixture.bindings.inputs[0].result),
          "reordered affine view retains the shared source backing");
  check_oracle(result, fixture, 1, {1}, {{0, 2}, {0, 0}},
               Region::whole({2, 2}));
  fixture.bindings.inputs.clear();
  check_oracle(result, fixture, 1, {1}, {{0, 2}, {0, 0}},
               Region::whole({2, 2}));
}
void errors() {
  auto registry = make_default_operation_registry();
  const auto rejects = [&](const WorkflowDocument& d) {
    GraphContext graph(d);
    Compiler compiler(registry);
    return !compiler.compile(graph).ok();
  };
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  Fixture f;
  auto x = f.add({ElementType::UInt8, {2}}),
       y = f.add({ElementType::UInt8, {3}});
  auto edge = take(format::assemble_channels(f.document, {x, y}, 1, options));
  f.document.outputs = {{"result", edge.source_node, "values"}};
  require(rejects(f.document), "unequal extents rejected");
  f.document.nodes[0].inputs = {x};
  f.document.nodes[0].parameters["axis"] = std::int64_t{2};
  require(rejects(f.document), "axis rejected");
  f.document.nodes[0].parameters["axis"] = std::int64_t{1};
  f.document.nodes[0].inputs.clear();
  require(rejects(f.document), "zero inputs rejected");
  Fixture mapped;
  x = mapped.add({ElementType::UInt8, {2, 2}});
  auto bad = take(format::assemble_mapped_channels(
      mapped.document, {x}, 1, {{false, 0}},
      {{0, "index", "0", 0, {}}, {0, "index", "1", 0, {}}}, options));
  mapped.document.outputs = {{"result", bad.source_node, "values"}};
  require(rejects(mapped.document), "duplicate destination rejected");
  Fixture unrelated;
  x = unrelated.add({ElementType::UInt8, {2}});
  y = unrelated.add({ElementType::UInt8, {2}});
  options.layout = "view";
  edge =
      take(format::assemble_channels(unrelated.document, {x, y}, 1, options));
  bool failed = false;
  try {
    run(unrelated, edge);
  } catch (const std::exception& error) {
    failed =
        std::string(error.what()).find("ViewUnavailable") != std::string::npos;
  }
  require(failed, "forced unrelated view rejected");
}
void planar() {
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    PlanarImageConfig component;
    component.channel_axis.reset();
    component.order = order;
    if (order == ImagePlaneOrder::Continuous)
      component.row_pitch_bytes = 560;
    Fixture f;
    std::vector<WorkflowInput> inputs;
    const Region part({{127, 3}, {127, 3}});
    for (unsigned i = 0; i < 4; ++i)
      inputs.push_back(
          f.image({ElementType::Float32, {131, 133}}, component, {part}));
    format::ChannelAssemblyOptions options;
    options.metadata_mode = "raw";
#if defined(__aarch64__) && defined(__APPLE__)
    const std::string native_profile = "accelerated_apple_silicon";
#else
    const std::string native_profile = "accelerated_x86_64";
#endif
    for (const auto& profile : {std::string("strict"), native_profile}) {
      options.profile = profile;
      auto edge =
          take(format::assemble_channels(f.document, inputs, 2, options));
      const Region roi({{127, 3}, {127, 3}, {2, 1}});
      auto result = run(f, edge, roi);
      check_oracle(result, f, 2, {{}, {}, {}, {}},
                   {{0, 0}, {1, 0}, {2, 0}, {3, 0}}, roi);
      require(source_bytes(result, f) == 36,
              "only one input ROI in sample support");
    }
    Fixture c;
    PlanarImageConfig config;
    config.order = order;
    auto source = c.image({ElementType::UInt8, {131, 133, 3}}, config,
                          {Region({{127, 3}, {127, 3}, {0, 1}}),
                           Region({{127, 3}, {127, 3}, {2, 1}})});
    auto edge = take(
        format::assemble_mapped_channels(c.document, {source}, 2, {{false, 2}},
                                         {{0, "index", "2", 0, {}},
                                          {0, "index", "0", 1, {}},
                                          {0, "index", "2", 2, {}}},
                                         options));
    const Region roi({{127, 3}, {127, 3}, {0, 3}});
    auto result = run(c, edge, roi);
    check_oracle(result, c, 2, {2}, {{0, 2}, {0, 0}, {0, 2}}, roi);
    require(source_bytes(result, c) == 18,
            "gap excluded and reuse deduplicated in sample support");
    // Split/reassemble restores a canonical common owner and survives context.
    Fixture split;
    source = split.image({ElementType::UInt8, {3, 4, 3}}, config);
    OperationMetadata md;
    md = split.metadata(0);
    format::ChannelExtractOptions extract;
    extract.axis = 2;
    extract.metadata_mode = "raw";
    auto handles =
        take(format::split_channels(split.document, source, md, extract));
    inputs.clear();
    for (auto h : handles)
      inputs.push_back(h.output);
    options.layout = "view";
    edge = take(format::assemble_channels(split.document, inputs, 2, options));
    result = run(split, edge);
    require(owner(result.results.at("result")) ==
                owner(split.bindings.inputs[0].result),
            "reassemble common root");
    check_oracle(result, split, 2, {2}, {{0, 0}, {0, 1}, {0, 2}},
                 Region::whole({3, 4, 3}));
    split.bindings.inputs.clear();
    require(
        read(result.results.at("result"), Region::whole({3, 4, 3})).size() ==
            36,
        "view survives input and context");
  }
}
void dependency_and_boundaries() {
  auto registry = make_default_operation_registry();
  Fixture f;
  auto source = f.add({ElementType::Float32, {2, 3}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  auto edge = take(format::assemble_mapped_channels(f.document, {source}, 1,
                                                    {{false, 1}},
                                                    {{0, "index", "2", 0, {}},
                                                     {0, "index", "0", 1, {}},
                                                     {0, "index", "2", 2, {}}},
                                                    options));
  OperationMetadata metadata;
  metadata = f.metadata(0);
  auto preparation = take(
      registry->prepare_operation(f.document.nodes[0].operation, {metadata},
                                  f.document.nodes[0].parameters));
  const auto& traits = preparation->traits();
  require(!traits.cacheable, "sample-only cache disabled");
  auto all = take(Footprint::all({2, 3}));
  auto full = run(f, edge);
  auto sparse = take(Footprint::from_regions(
      {2, 3}, {Region({{0, 1}, {0, 1}}), Region({{1, 1}, {2, 1}})}));
  auto registry_plan = std::make_shared<GraphContext>(f.document);
  auto plan = take(Compiler(registry).compile(*registry_plan));
  ExecutionContext context(registry);
  f.bind_to(context);
  auto frozen = take(context.freeze(plan.plan, f.bindings));
  auto sparse_run =
      take(context.execute_fragments(frozen, {{"result", sparse}}));
  require(take(take(sparse_run.dependencies.source_support())
                   .at("input1")
                   .element_count()) == 2,
          "exact disjoint backward support");
  auto dirty =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {2, 1}})}));
  auto affected =
      take(full.dependencies.potential_dirty("input1", dirty, 1, {},
                                             ResultSupportTarget::Tensor, 0))
          .at("result");
  require(take(affected.element_count()) == 2,
          "dirty repeated source fans out");
  auto ignored =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {1, 1}})}));
  require(take(full.dependencies.potential_dirty(
                   "input1", ignored, 1, {}, ResultSupportTarget::Tensor, 0))
              .at("result")
              .empty(),
          "omitted source never dirty");
  // A singleton inserts an axis and admits a generic common-owner view.
  Fixture single;
  source = single.add({ElementType::UInt8, {2}});
  options.layout = "view";
  edge = take(format::assemble_channels(single.document, {source}, 1, options));
  auto result = run(single, edge);
  check_oracle(result, single, 1, {{}}, {{0, 0}}, Region::whole({2, 1}));
  // Conflicting/incomplete explicit descriptions reject before sample reads.
  TensorDescription group;
  group.channel_axis = 0;
  TensorColorGroup bad;
  bad.name = "bad";
  bad.indices = {0};
  bad.components = {{"R", "red", "relative"}};
  bad.interpretation.model = "rgb";
  group.groups = {bad};
  require(!encode_tensor_description(group).ok(),
          "incomplete RGB group rejected");
  bad.indices = {0, 1, 2};
  bad.components = {{"l", "l", "relative"},
                    {"a", "a", "relative"},
                    {"b", "b", "relative"}};
  bad.interpretation.model = "cielab";
  group.groups = {bad};
  require(!encode_tensor_description(group).ok(), "Lab white required");
  bad.interpretation.white = std::array<double, 2>{.3127, .3290};
  group.groups = {bad};
  group.channels = bad.components;
  group.channels[1].role = "red";
  require(!encode_tensor_description(group).ok(),
          "group-channel role conflict rejected");
}
void mixed_chain_and_limits() {
  Fixture f;
  auto x = f.add({ElementType::UInt8, {2, 2, 2}});
  OperationMetadata metadata;
  metadata = f.metadata(0);
  format::ChannelExtractOptions extraction;
  extraction.axis = 2;
  extraction.metadata_mode = "raw";
  auto handles =
      take(format::split_channels(f.document, x, metadata, extraction));
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  auto generic = take(format::assemble_channels(
      f.document, {handles[1].output, handles[0].output}, 2, options));
  PlanarImageConfig config;
  auto image = f.image({ElementType::UInt8, {2, 2, 1}}, config);
  auto output = take(format::concatenate_channels(f.document, {generic, image},
                                                  2, {2, 2}, options));
  auto result = run(f, output);
  check_oracle(result, f, 2, {2, 2}, {{0, 1}, {0, 0}, {1, 0}},
               Region::whole({2, 2, 3}));
  auto registry = make_default_operation_registry();
  GraphContext graph(f.document);
  Compiler compiler(registry);
  PlanningOptions planning;
  planning.output_regions = {{"result", Region({{0, 1}, {0, 1}, {0, 1}})}};
  auto compiled = take(compiler.compile(graph, planning));
  ExecutionContext context(registry);
  f.bind_to(context);
  ExecutionOptions work;
  work.maximum_dependency_work = 1;
  require(context.execute(compiled.plan, f.bindings, {}, work).status().code ==
              ErrorCode::ResourceExhausted,
          "work budget charged");
  CancellationSource stop;
  stop.cancel();
  require(
      context.execute(compiled.plan, f.bindings, stop.token()).status().code ==
          ErrorCode::Cancelled,
      "cancel before publication");
}
void direct_strides_and_bits() {
  Fixture f;
  f.add({ElementType::Float32, {4}});
  f.add({ElementType::Float32, {4}});
  const std::uint32_t patterns[] = {0x80000000U, 0x7f800000U, 0x7fc12345U,
                                    0xff812345U};
  for (unsigned i = 0; i < 2; ++i) {
    std::memcpy(f.raw[i].data(), patterns, sizeof(patterns));
    f.sources[i].layout = i ? StridedLayout{0, {0}} : StridedLayout{12, {-4}};
    f.rebind(i);
  }
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  auto edge = take(format::assemble_channels(
      f.document, {WorkflowInputReference{1}, WorkflowInputReference{2}}, 1,
      options));
  f.document.outputs = {{"result", edge.source_node, "values"}};
  auto registry = make_default_operation_registry();
  ExecutionContext context(registry);
  f.bind_to(context);
  const auto root = take(context.resource_budget());
  auto buffer = take(root.allocator().allocate(sizeof(patterns)));
  std::memcpy(buffer.data(), patterns, sizeof(patterns));
  auto storage = std::move(buffer).freeze();
  for (unsigned i = 0; i < 2; ++i) {
    auto builder =
        take(ResultBuilder::start(root, f.sources[i].schema, "shared.strides"));
    channel_fixture::require(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
    channel_fixture::require(builder.publish_tensor(
        0, Region::whole({4}), f.sources[i].layout, storage,
        take(ResultRelation::cartesian(root, 4, {0, 1, 0, 0})),
        {true, true, true, true}));
    f.bindings.inputs[i].result = take(builder.seal());
  }
  GraphContext graph(f.document);
  auto compiled = take(Compiler(registry).compile(graph));
  auto result =
      take(context.execute(compiled.plan, f.bindings)).results.at("result");
  auto bytes = read(result, Region::whole({4, 2}));
  for (std::uint64_t i = 0; i < 4; ++i)
    for (std::uint64_t c = 0; c < 2; ++c) {
      const auto expected = c == 0 ? patterns[3 - i] : patterns[0];
      require(!std::memcmp(bytes.data() + (i * 2 + c) * 4, &expected, 4),
              "special bits on negative/zero strides");
    }
  f.document.nodes[0].parameters["layout"] = std::string("view");
  graph.replace(f.document);
  compiled = take(Compiler(registry).compile(graph));
  auto failed = context.execute(compiled.plan, f.bindings);
  const bool rejected =
      !failed.ok() &&
      failed.status().message.find("ViewUnavailable") != std::string::npos;
  require(rejected, "nonaffine forced view fails");
}
void override_and_profile_lifetime() {
  Fixture f;
  TensorDescription left, right;
  left.component = TensorChannelDescription{"v", "value", "unit"};
  left.reference = "left";
  right = left;
  right.reference = "right";
  auto a =
      f.add({ElementType::UInt8, {2}}, {take(encode_tensor_description(left))});
  auto b = f.add({ElementType::UInt8, {2}},
                 {take(encode_tensor_description(right))});
  format::ChannelAssemblyOptions options;
  options.layout = "materialize";
  TensorDescription target;
  target.channel_axis = 1;
  target.channels = {{"renamed", "", ""}, {"renamed", "", ""}};
  options.output_description = target;
  auto edge = take(format::assemble_channels(f.document, {a, b}, 1, options));
  bool rejected = false;
  try {
    run(f, edge);
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected, "display name cannot override reference conflicts");
  f.document.nodes.clear();
  options.metadata_mode = "override";
  options.input_overrides = {{1, left}};
  edge = take(format::assemble_channels(f.document, {a, b}, 1, options));
  auto result = run(f, edge);
  require(
      take(decode_tensor_description(facets(f.bindings.inputs[1].result)[0]))
              .reference == "right",
      "override is local");
  auto output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(output.channels[1].interpretation->reference == "left",
          "override projected");
  options.metadata_mode = "raw";
  options.input_overrides.clear();
  edge = take(format::assemble_channels(f.document, {a, b}, 1, options));
  result = run(f, edge);
  output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(output.channels[0].interpretation->reference == "left" &&
              output.channels[1].interpretation->reference == "right",
          "raw preserves independent applicable interpretations");
  // A real owned ICC resource is referenced only by a component, not globals.
  ResultRef surviving;
  ColorProfileIdentity identity;
  {
    ResourceBudget budget;
    auto profile_bytes = numeric_fixture::fixture();
    auto profile = take(IccProfile::import(
        ByteView(profile_bytes.data(), profile_bytes.size()), budget));
    identity = profile.identity();
    auto resources = take(ResourceBindings::create({profile}, budget));
    TensorDescription described;
    described.component.emplace();
    described.component->interpretation.emplace();
    described.component->interpretation->profile = identity;
    Fixture owned;
    auto input =
        owned.add({ElementType::UInt8, {2}},
                  {take(encode_tensor_description(described))}, resources);
    options = {};
    auto node =
        take(format::assemble_channels(owned.document, {input}, 1, options));
    surviving = run(owned, node).results.at("result");
    require(surviving.resources().icc_profile(identity).ok(),
            "component profile retained");
    bool missing_resource = false;
    try {
      Fixture missing;
      missing.add({ElementType::UInt8, {2}},
                  {take(encode_tensor_description(described))});
    } catch (const std::exception&) {
      missing_resource = true;
    }
    require(missing_resource, "unowned component profile rejected");
  }
  require(surviving.resources().icc_profile(identity).ok(),
          "profile survives context/source teardown");
}
void source_coordinate_complement() {
  Fixture fixture;
  TensorDescription source;
  source.channel_axis = 1;
  source.channels = {{"Y", "gray", "relative"}};
  source.channels[0].interpretation.emplace();
  source.channels[0].interpretation->coordinates =
      TensorModelCoordinates{"relative", "", "", {}};
  TensorColorGroup group;
  group.name = "gray";
  group.indices = {0};
  group.components = {{"Y", "gray", "relative"}};
  group.interpretation.model = "gray";
  group.interpretation.coordinates =
      TensorModelCoordinates{"", "1931-2", "linear_y", {}};
  source.groups = {group};
  auto input = fixture.add({ElementType::Float32, {2, 1}},
                           {take(encode_tensor_description(source))});
  auto edge = take(format::concatenate_channels(fixture.document, {input}, 1));
  const auto result = run(fixture, edge);
  const auto output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  const auto& coordinates = *output.channels[0].interpretation->coordinates;
  require(coordinates.scale == "relative" && coordinates.observer == "1931-2" &&
              coordinates.gray_kind == "linear_y",
          "source group model completion preserves complementary coordinates");
}
void source_group_overrides_defaults() {
  Fixture fixture;
  TensorDescription source;
  source.channel_axis = 1;
  source.coordinates = TensorModelCoordinates{"absolute", "", "", {}};
  source.channels = {{"Y", "gray", "relative"}};
  TensorColorGroup group;
  group.name = "gray";
  group.indices = {0};
  group.components = source.channels;
  group.interpretation.model = "gray";
  group.interpretation.coordinates =
      TensorModelCoordinates{"relative", "", "", {}};
  source.groups = {group};
  auto input = fixture.add({ElementType::Float32, {2, 1}},
                           {take(encode_tensor_description(source))});
  auto edge = take(format::concatenate_channels(fixture.document, {input}, 1));
  const auto result = run(fixture, edge);
  const auto output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(output.channels[0].interpretation->coordinates->scale == "relative",
          "explicit group coordinate overrides tensor default");
}
void partial_coordinate_respect() {
  for (const unsigned assignment : {0u, 1u, 2u}) {
    const bool assign_observer = assignment == 2;
    Fixture fixture;
    TensorDescription source;
    source.component = TensorChannelDescription{"Y", "gray", "relative"};
    source.component->interpretation.emplace();
    source.component->interpretation->model = "gray";
    source.component->interpretation->coordinates =
        TensorModelCoordinates{"relative", "1931-2", "", {}};
    auto a = fixture.add({ElementType::Float32, {2}},
                         {take(encode_tensor_description(source))});
    source.component->interpretation->coordinates->observer = "1964-10";
    auto b = fixture.add({ElementType::Float32, {2}},
                         {take(encode_tensor_description(source))});
    TensorDescription target;
    target.channel_axis = 1;
    target.coordinates = TensorModelCoordinates{assignment ? "relative" : "",
                                                assign_observer ? "1931-2" : "",
                                                "",
                                                {}};
    format::ChannelAssemblyOptions options;
    options.layout = "materialize";
    options.output_description = target;
    auto edge =
        take(format::assemble_channels(fixture.document, {a, b}, 1, options));
    bool rejected = false;
    try {
      run(fixture, edge);
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected != assign_observer,
            "only explicitly assigned coordinate fields override respect");
  }
}
void model_coordinate_overlay() {
  TensorDescription source;
  source.component = TensorChannelDescription{"Y", "gray", "relative"};
  source.component->interpretation.emplace();
  source.component->interpretation->model = "gray";
  source.component->interpretation->coordinates =
      TensorModelCoordinates{"relative", "1931-2", "", {}};
  Fixture fixture;
  auto input = fixture.add({ElementType::Float32, {2}},
                           {take(encode_tensor_description(source))});
  TensorDescription target;
  target.channel_axis = 1;
  target.channels = {{"Y", "gray", "relative"}};
  target.channels[0].interpretation.emplace();
  target.channels[0].interpretation->coordinates =
      TensorModelCoordinates{"relative", "", "", {}};
  TensorColorGroup group;
  group.name = "gray";
  group.indices = {0};
  group.components = {{"Y", "gray", "relative"}};
  group.interpretation.model = "gray";
  group.interpretation.coordinates =
      TensorModelCoordinates{"", "", "linear_y", {}};
  target.groups = {group};
  format::ChannelAssemblyOptions options;
  options.output_description = target;
  auto edge =
      take(format::assemble_channels(fixture.document, {input}, 1, options));
  const auto result = run(fixture, edge);
  const auto output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  const auto& c = *output.channels[0].interpretation->coordinates;
  require(c.scale == "relative" && c.gray_kind == "linear_y" &&
              c.observer == "1931-2",
          "assembly merges complementary channel/group/source coordinates");
  check_oracle(result, fixture, 1, {std::nullopt}, {{0, 0}},
               Region::whole({2, 1}));
  std::cout
      << "TDM5 complementary assembly assertions and byte oracle passed\n";
}
void result_batches_and_admission() {
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    ResultTensorLayout physical;
    physical.spatial = true;
    physical.order = order;
    physical.channel_axis.reset();
    Fixture f;
    auto a = f.add_source(channel_fixture::source(
        {ElementType::UInt8, {2, 260}}, {}, physical, {2, 2}));
    auto b = f.add_source(channel_fixture::source(
        {ElementType::UInt8, {2, 260}}, {}, physical, {2, 2}));
    format::ChannelAssemblyOptions options;
    options.metadata_mode = "raw";
    auto edge = take(format::assemble_channels(f.document, {a, b}, 2, options));
    const Region q({{1, 1}, {0, 2}, {1, 1}, {127, 3}, {0, 2}});
    auto run_result = run(f, edge, q);
    check_oracle(run_result, f, 4, {{}, {}}, {{0, 0}, {1, 0}}, q);
    require(spec(run_result.results.at("result")).batch_axes ==
                f.sources[0].schema.tensors[0].batch_axes,
            "assembly preserves batch prefix");
    auto input = channel_fixture::source({ElementType::UInt8, {2, 260, 2}}, {},
                                         [&] {
                                           auto layout = physical;
                                           layout.channel_axis = 2;
                                           return layout;
                                         }(),
                                         {2, 2});
    Fixture alias;
    auto original = alias.add_source(std::move(input));
    format::ChannelExtractOptions extract;
    extract.axis = 2;
    extract.metadata_mode = "raw";
    auto split = take(format::split_channels(alias.document, original,
                                             alias.metadata(0), extract));
    options.layout = "view";
    edge = take(format::assemble_channels(
        alias.document, {split[0].output, split[1].output}, 2, options));
    auto views = run(alias, edge, q);
    check_oracle(views, alias, 4, {4}, {{0, 0}, {0, 1}}, q);
    for (std::uint64_t layer = 0; layer < 2; ++layer) {
      const Region plane({{1, 1}, {layer, 1}, {1, 1}, {127, 3}, {0, 2}});
      require(
          channel_fixture::owner(views.results.at("result"), plane) ==
              channel_fixture::owner(alias.bindings.inputs[0].result, plane),
          "each reassembled batch plane retains its source owner");
    }
  }
  Fixture one;
  auto source = one.add({ElementType::UInt8, {2}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = "view";
  auto edge =
      take(format::assemble_channels(one.document, {source}, 1, options));
  one.document.outputs = {{"result", edge.source_node, "values"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(one.document);
  auto plan = take(Compiler(registry).compile(graph));
  ExecutionContext context(registry);
  one.bind_to(context);
  auto frozen = take(context.freeze(plan.plan, one.bindings));
  const auto root = take(context.resource_budget());
  const auto peak = root.statistics().peak[ResourceKind::Payload];
  auto empty = take(context.execute_fragments(
      frozen, {{"result", take(Footprint::none({2, 1}))}}));
  require(take(empty.results.at("result").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              root.statistics().peak[ResourceKind::Payload] == peak,
          "assembly Empty has no state/payload");
  auto first = std::async(std::launch::async, [&] {
    return context.execute(plan.plan, one.bindings);
  });
  auto second = std::async(std::launch::async, [&] {
    return context.execute(plan.plan, one.bindings);
  });
  check_oracle(take(first.get()), one, 1, {{}}, {{0, 0}},
               Region::whole({2, 1}));
  check_oracle(take(second.get()), one, 1, {{}}, {{0, 0}},
               Region::whole({2, 1}));
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Payload] = 2;
  ExecutionContext limited(registry, config);
  one.bind_to(limited);
  require(limited.execute(plan.plan, one.bindings).status().code ==
              ErrorCode::ResourceExhausted,
          "source-only Payload cannot admit assembly state");
  require(take(limited.resource_budget())
                  .statistics()
                  .live[ResourceKind::Payload] == 2,
          "failed assembly releases candidate state and payload");
}
void large_input_assertion() {
  Fixture f;
  f.execution_config.managed_resources = ResourceLimits{};
  f.execution_config.managed_resources->capacity[ResourceKind::Metadata] =
      64 * 1024 * 1024;
  f.add({ElementType::UInt8, {2, 3}});
  for (unsigned i = 1; i < 1024; ++i)
    f.add({ElementType::UInt8, {1}});
  std::vector<format::ChannelEditInput> inputs;
  for (unsigned i = 0; i < 1024; ++i)
    inputs.push_back({WorkflowInputReference{i + 1}, f.metadata(i),
                      i == 0 ? format::ChannelEditStructure{false, 1, false}
                             : format::ChannelEditStructure{false, {}, true}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = "view";
  auto edge = take(format::swizzle_channels(
      f.document, inputs,
      {{1023, {"index", "0"}, {}}, {1023, {"index", "0"}, {}}}, options));
  require(std::get<std::string>(
              f.document.nodes.back().parameters.at("expected_inputs"))
                  .size() < 128,
          "all ordered input assertions remain bounded");
  auto result = run(f, edge);
  require(read(result.results.at("result"), Region::whole({2, 2})) ==
              std::vector<std::uint8_t>(4, f.raw[1023][0]),
          "multi-envelope scalar gather");
  require(source_bytes(result, f) == 1, "unused ports carry Descriptor only");
  auto altered = *f.document.inputs[42].result_schema;
  altered.tensors[0].facets = {{"app.unused", 1, {42}}};
  f.document.inputs[42].result_schema =
      std::make_shared<const SchemaTemplate>(std::move(altered));
  GraphContext changed(f.document);
  require(!Compiler(make_default_operation_registry()).compile(changed).ok(),
          "bounded assertion detects unrelated unused-input facet drift");
}
}  // namespace
int main() try {
  result_batches_and_admission();
  large_input_assertion();
  source_group_overrides_defaults();
  source_coordinate_complement();
  partial_coordinate_respect();
  model_coordinate_overlay();
  generic_oracle();
  auto_fragment_oracle();
  shared_owner_alias_oracle();
  std::cout << "generic coordinate and all-dtype oracle passed\n";
  mapped_and_metadata();
  std::cout << "mapped selection and metadata passed\n";
  dependency_and_boundaries();
  mixed_chain_and_limits();
  direct_strides_and_bits();
  override_and_profile_lifetime();
  errors();
  std::cout << "preflight and view errors passed\n";
  planar();
  std::cout << "planar ROI, sparse selection, profiles and views passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
