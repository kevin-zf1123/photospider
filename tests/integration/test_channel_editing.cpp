#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/channel_result_fixture.hpp"

namespace {
using namespace ps;                // NOLINT(build/namespaces)
using namespace assembly_fixture;  // NOLINT(build/namespaces)
std::vector<format::ChannelEditInput> edit_inputs(
    const Fixture& f,
    const std::vector<format::ChannelEditStructure>& structures) {
  std::vector<format::ChannelEditInput> result;
  for (std::size_t i = 0; i < f.document.inputs.size(); ++i) {
    const auto& d = f.document.inputs[i];
    OperationMetadata m;
    m.result_schema = d.result_schema;
    result.push_back({WorkflowInputReference{d.id}, m, structures[i]});
  }
  return result;
}
format::ChannelEditSource source(unsigned port, unsigned index = 0) {
  return {port, {"index", std::to_string(index)}, {}};
}
format::ChannelReplacement replacement(unsigned dest, unsigned port,
                                       unsigned index = 0) {
  return {{"index", std::to_string(dest)}, source(port, index)};
}
void generic() {
  for (auto dtype : {ElementType::UInt8, ElementType::Int8, ElementType::UInt16,
                     ElementType::Int16, ElementType::Int64,
                     ElementType::Float32, ElementType::Float64})
    for (unsigned rank = 1; rank <= 8; ++rank)
      for (unsigned axis = 0; axis < rank; ++axis) {
        Fixture f;
        std::vector<std::uint64_t> shape(rank, 2);
        shape[axis] = 4;
        f.add({dtype, shape});
        f.add({dtype, {1}});
        auto inputs = edit_inputs(f, {{false, axis}, {false, {}, true}});
        format::ChannelAssemblyOptions options;
        options.metadata_mode = "raw";
        options.layout = "materialize";
        auto out = take(format::swizzle_channels(
            f.document, inputs,
            {source(0, 2), source(0, 0), source(0, 2), source(1)}, options));
        auto result = run(f, out);
        check_oracle(result, f, axis, {axis, {}},
                     {{0, 2}, {0, 0}, {0, 2}, {1, 0}}, Region::whole(shape));
        auto swap = take(format::replace_channels(
            f.document, inputs,
            {replacement(0, 0, 1), replacement(1, 0, 0), replacement(3, 1)},
            options));
        result = run(f, swap);
        check_oracle(result, f, axis, {axis, {}},
                     {{0, 1}, {0, 0}, {0, 2}, {1, 0}}, Region::whole(shape));
      }
}
void metadata() {
  Fixture f;
  TensorDescription desc;
  desc.channel_axis = 2;
  desc.channels = {{"R", "red", "relative"},
                   {"G", "green", "relative"},
                   {"B", "blue", "relative"},
                   {"A", "alpha", "relative"}};
  TensorColorGroup group;
  group.name = "rgb";
  group.indices = {0, 1, 2};
  group.components = {desc.channels[0], desc.channels[1], desc.channels[2]};
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  group.interpretation.association = "straight";
  group.alpha = 3;
  desc.groups = {group};
  f.add({ElementType::Float32, {1, 2, 4}},
        {take(encode_tensor_description(desc))});
  TensorDescription gray;
  gray.component = TensorChannelDescription{"Y", "gray", "relative"};
  gray.model = "gray";
  f.add({ElementType::Float32, {1, 2}},
        {take(encode_tensor_description(gray))});
  f.add({ElementType::Float32, {1}});
  auto inputs = edit_inputs(f, {{}, {true, {}}, {false, {}, true}});
  auto edge = take(format::swizzle_channels(
      f.document, {inputs[0]},
      {source(0, 2), source(0, 1), source(0, 0), source(0, 3)}));
  auto result = run(f, edge);
  auto d = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(d.channels[0].role == "blue" && d.groups.size() == 1 &&
              d.groups[0].indices == std::vector<std::uint64_t>({2, 1, 0}) &&
              d.groups[0].alpha == 3,
          "A projects group and alpha uniquely");
  edge = take(format::swizzle_channels(f.document, {inputs[0]},
                                       {source(0), source(0), source(0, 2)}));
  result = run(f, edge);
  d = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(d.groups.empty() && d.channels[0].role == "red",
          "duplicate drops group");
  edge = take(format::replace_channels(
      f.document, inputs,
      {{{"role", "red"}, source(1)}, {{"name", "A"}, source(2)}}));
  result = run(f, edge);
  d = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(d.channels[0].role == "red" &&
              d.channels[0].interpretation->model == "rgb" &&
              d.channels[3].role == "alpha" && !d.channels[3].interpretation &&
              d.groups.size() == 1,
          "B fully preserves destination semantics without source leakage");
  check_oracle(result, f, 2, {2, {}, {}}, {{1, 0}, {0, 1}, {0, 2}, {2, 0}},
               Region::whole({1, 2, 4}));
  format::ChannelAssemblyOptions options;
  auto target = desc;
  target.groups.clear();
  target.channels[1].name = "changed";
  options.output_description = target;
  const auto before = f.document.nodes.size();
  require(!format::replace_channels(f.document, inputs, {replacement(0, 1)},
                                    options)
                  .ok() &&
              before == f.document.nodes.size(),
          "B cannot relabel unchanged slot, transactional");
}
void planar_and_scalar() {
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    Fixture f;
    PlanarImageConfig config;
    config.order = order;
    const Region part({{127, 3}, {127, 3}, {1, 2}});
    f.image({ElementType::Float32, {131, 133, 4}}, config, {part});
    config.channel_axis.reset();
    f.image({ElementType::Float32, {131, 133}}, config,
            {Region({{127, 3}, {127, 3}})});
    f.add({ElementType::Float32, {1}});
    auto inputs = edit_inputs(f, {{false, 2}, {true, {}}, {false, {}, true}});
    format::ChannelAssemblyOptions options;
    options.metadata_mode = "raw";
    options.layout = "materialize";
    auto edge = take(format::replace_channels(
        f.document, inputs, {replacement(0, 1), replacement(3, 2)}, options));
    const Region roi({{127, 3}, {127, 3}, {0, 4}});
    auto result = run(f, edge, roi);
    check_oracle(result, f, 2, {2, {}, {}}, {{1, 0}, {0, 1}, {0, 2}, {2, 0}},
                 roi);
    require(source_bytes(result, f) == 112,
            "sparse base/external/scalar exact sample support");
    const Region alpha({{127, 3}, {127, 3}, {3, 1}});
    for (const std::uint32_t bits :
         {0x80000000U, 0x7fc01234U, 0xff812345U, 0x3f000000U}) {
      std::vector<std::uint8_t> bytes(4);
      std::memcpy(bytes.data(), &bits, 4);
      f.raw[2] = bytes;
      f.rebind(2);
      result = run(f, edge, alpha);
      check_oracle(result, f, 2, {2, {}, {}}, {{1, 0}, {0, 1}, {0, 2}, {2, 0}},
                   alpha);
      require(source_bytes(result, f) == 4,
              "scalar sample support excludes base pixels");
    }
    options.layout = "view";
    edge = take(format::replace_channels(f.document, inputs,
                                         {replacement(3, 2)}, options));
    bool failed = false;
    try {
      run(f, edge, alpha);
    } catch (const std::exception& e) {
      failed =
          std::string(e.what()).find("ViewUnavailable") != std::string::npos;
    }
    require(failed, "constant cannot alias canonical image");
    options.layout = "materialize";
    auto literal = source(0);
    literal.literal = format::ChannelLiteral{ElementType::Float32, f.raw[2]};
    edge = take(format::swizzle_channels(f.document, {inputs[0]},
                                         {literal, literal}, options));
    result = run(f, edge, Region({{127, 3}, {127, 3}, {0, 2}}));
    check_oracle(result, f, 2, {2, {}, {}}, {{2, 0}, {2, 0}},
                 Region({{127, 3}, {127, 3}, {0, 2}}));
    require(source_bytes(result, f) == 0,
            "literal provider requires no base sample support");
  }
}
void dependencies_and_errors() {
  Fixture f;
  f.add({ElementType::Float32, {2, 4}});
  f.add({ElementType::Float32, {1}});
  auto inputs = edit_inputs(f, {{false, 1}, {false, {}, true}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  auto edge = take(format::replace_channels(
      f.document, inputs,
      {replacement(0, 0, 1), replacement(1, 0, 0), replacement(3, 1)},
      options));
  auto registry = make_default_operation_registry();
  const auto& node = f.document.nodes.back();
  auto prepared = take(registry->prepare_operation(
      node.operation, {inputs[0].metadata, inputs[1].metadata},
      node.parameters));
  auto complete = run(f, edge);
  auto alpha =
      take(Footprint::from_regions({2, 4}, {Region({{1, 1}, {3, 1}})}));
  auto partial = run(f, edge, alpha.boxes()[0]);
  auto needs = take(partial.dependencies.source_support());
  require(take(needs.at("input2").element_count()) == 1 &&
              (!needs.count("input1") || needs.at("input1").empty()),
          "scalar backward exact");
  auto dirty = take(complete.dependencies.potential_dirty(
                        "input2", take(Footprint::all({1})), 1, {},
                        ResultSupportTarget::Tensor, 0))
                   .at("result");
  require(take(dirty.element_count()) == 2,
          "scalar dirty full spatial only selected slot");
  auto overwritten =
      take(Footprint::from_regions({2, 4}, {Region({{0, 2}, {3, 1}})}));
  require(
      take(complete.dependencies.potential_dirty(
               "input1", overwritten, 1, {}, ResultSupportTarget::Tensor, 0))
          .at("result")
          .empty(),
      "overwritten base not dirty");
  auto wrong = inputs;
  auto wrong_schema = *wrong[0].metadata.result_schema;
  wrong_schema.tensors[0].descriptor.shape[0] = 3;
  wrong[0].metadata.result_schema =
      std::make_shared<const SchemaTemplate>(wrong_schema);
  const auto before = f.document.nodes.size();
  require(
      !format::swizzle_channels(f.document, wrong, {source(1)}, options).ok(),
      "declaration assertion");
  require(!format::swizzle_channels(f.document, inputs, {}, options).ok(),
          "empty A");
  require(
      !format::replace_channels(f.document, inputs,
                                {replacement(0, 1), replacement(0, 1)}, options)
           .ok(),
      "duplicate B");
  require(f.document.nodes.size() == before, "failed expansions unchanged");
  // Producer descriptors are asserted again at compilation.
  auto producer =
      take(format::replace_channels(f.document, inputs, {}, options));
  auto forwarded = inputs;
  forwarded[0].input = producer;
  forwarded[0].metadata.result_schema =
      std::make_shared<const SchemaTemplate>(wrong_schema);
  auto stale = take(
      format::swizzle_channels(f.document, forwarded, {source(1)}, options));
  f.document.outputs = {{"result", stale.source_node, "values"}};
  GraphContext graph(f.document);
  Compiler compiler(registry);
  require(!compiler.compile(graph).ok(),
          "stale producer descriptor rejected for all-scalar result");
  f.document.nodes.pop_back();
  f.document.outputs = {{"result", edge.source_node, "values"}};
  GraphContext valid(f.document);
  auto compiled = take(compiler.compile(valid));
  CancellationSource stop;
  stop.cancel();
  ExecutionContext context(registry);
  f.bind_to(context);
  require(
      context.execute(compiled.plan, f.bindings, stop.token()).status().code ==
          ErrorCode::Cancelled,
      "cancellation before publication");
}
void regression_boundaries() {
  // Two independent RGB groups: editing one must retain the other.
  Fixture f;
  TensorDescription desc;
  desc.channel_axis = 1;
  for (unsigned g = 0; g < 2; ++g) {
    TensorColorGroup group;
    group.name = "rgb" + std::to_string(g);
    group.indices = {g * 3U, g * 3U + 1, g * 3U + 2};
    group.components = {{"R" + std::to_string(g), "red", "relative"},
                        {"G" + std::to_string(g), "green", "relative"},
                        {"B" + std::to_string(g), "blue", "relative"}};
    group.interpretation.model = "rgb";
    group.interpretation.primaries = "srgb";
    group.interpretation.transfer = "linear";
    group.interpretation.association = "straight";
    desc.groups.push_back(group);
  }
  f.add({ElementType::Float32, {2, 6}},
        {take(encode_tensor_description(desc))});
  auto inputs = edit_inputs(f, {{false, 1}});
  format::ChannelAssemblyOptions options;
  TensorDescription target;
  target.channel_axis = 1;
  target.groups = {desc.groups[0]};
  target.groups[0].interpretation.transfer = "srgb";
  options.output_description = target;
  auto edge = take(format::replace_channels(
      f.document, inputs,
      {replacement(0, 0), replacement(1, 0, 1), replacement(2, 0, 2)},
      options));
  auto result = run(f, edge);
  auto output = take(decode_tensor_description(
      result.results.at("result").schema().tensors[0].facets[0]));
  require(output.groups.size() == 2, "explicit group retains unrelated group");
  // Actual generated-node count, not repeated literal occurrence count.
  Fixture limit;
  limit.add({ElementType::Float32, {4}});
  auto described = edit_inputs(limit, {{false, 0}});
  for (std::uint64_t i = 1; i <= 65534; ++i)
    limit.document.nodes.push_back({i, "unused", {}, {}});
  auto literal = source(0);
  literal.literal =
      format::ChannelLiteral{ElementType::Float32, {0, 0, 0, 128}};
  options = {};
  options.metadata_mode = "raw";
  auto expanded = format::swizzle_channels(
      limit.document, described, {literal, literal, literal, literal}, options);
  require(expanded.ok() && limit.document.nodes.size() == 65536,
          "literal reuse counts exactly two generated nodes");
  require(
      !format::swizzle_channels(limit.document, described, {source(0)}, options)
              .ok() &&
          limit.document.nodes.size() == 65536,
      "node limit remains transactional");
  // External channels use their own axis; no implicit spatial broadcasting.
  Fixture mixed;
  mixed.add({ElementType::Float32, {1, 2, 4}});
  mixed.add({ElementType::Float32, {2, 1, 2}});
  auto m = edit_inputs(mixed, {{false, 2}, {false, 0}});
  edge = take(format::replace_channels(mixed.document, m,
                                       {replacement(2, 1, 1)}, options));
  result = run(mixed, edge);
  check_oracle(result, mixed, 2, {2, 0}, {{0, 0}, {0, 1}, {1, 1}, {0, 3}},
               Region::whole({1, 2, 4}));
  // Raw mismatching semantic axis cannot be used to index the old channel
  // table.
  Fixture raw;
  TensorDescription old;
  old.channel_axis = 0;
  old.channels = {{"x", "", ""}};
  raw.add({ElementType::Float32, {1, 4}},
          {take(encode_tensor_description(old))});
  edge = take(format::swizzle_channels(
      raw.document, edit_inputs(raw, {{false, 1}}), {source(0, 3)}, options));
  run(raw, edge);
  edge = take(format::replace_channels(
      raw.document, edit_inputs(raw, {{false, 1}}), {}, options));
  run(raw, edge);
  auto registry = make_default_operation_registry();
  for (std::int64_t dtype :
       {std::int64_t{-1}, std::int64_t{999}, std::int64_t{4294967300LL}}) {
    auto prepared = registry->prepare_operation(
        "channel.scalar_literal_strict", {},
        {{"dtype", dtype}, {"bits", std::string("00000000")}});
    require(!prepared.ok(), "invalid literal dtype rejects before narrowing");
  }
  literal.literal->dtype = static_cast<ElementType>(999);
  const auto before = raw.document.nodes.size();
  require(!format::swizzle_channels(
               raw.document, edit_inputs(raw, {{false, 1}}), {literal}, options)
                  .ok() &&
              raw.document.nodes.size() == before,
          "invalid helper literal dtype returns transactional error");
}
void dynamic_and_resource() {
  Fixture f;
  f.add({ElementType::Float32, {2, 4}});
  f.add({ElementType::Float32, {1}});
  auto inputs = edit_inputs(f, {{false, 1}, {false, {}, true}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "raw";
  options.layout = "view";
  auto edge = take(format::swizzle_channels(f.document, inputs,
                                            {source(1), source(1)}, options));
  f.document.outputs = {{"result", edge.source_node, "values"}};
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(f.document);
  auto plan = take(compiler.compile(graph)).plan;
  ExecutionContext context(registry);
  f.bind_to(context);
  for (std::uint32_t bits : {0x80000000U, 0x7fc01234U, 0x3f000000U}) {
    std::memcpy(f.raw[1].data(), &bits, 4);
    f.rebind(1);
    auto result = take(context.execute(plan, f.bindings));
    check_oracle(result, f, 1, {1, {}}, {{1, 0}, {1, 0}},
                 Region::whole({2, 2}));
    const auto& value = result.results.at("result");
    auto window = take(value.acquire_tensor(take(value.descriptor()), 0,
                                            Region::whole({2, 2})));
    require(
        take(window.row_run({0, 0})).data == take(window.row_run({1, 1})).data,
        "generic constant view has scalar-sized zero-stride storage");
  }
  ResultRef retained;
  {
    ExecutionContext temporary(registry);
    f.bind_to(temporary);
    retained = take(temporary.execute(plan, f.bindings)).results.at("result");
  }
  auto saved_bindings = f.bindings;
  f.bindings.inputs.clear();
  const float half = .5f;
  require(std::memcmp(read(retained, Region({{1, 1}, {1, 1}})).data(), &half,
                      sizeof(half)) == 0,
          "scalar view survives execution context");
  f.bindings = saved_bindings;
  Fixture owners;
  owners.add({ElementType::Float32, {2, 4}});
  owners.add({ElementType::Float32, {1}});
  owners.add({ElementType::Float32, {1}});
  auto described =
      edit_inputs(owners, {{false, 1}, {false, {}, true}, {false, {}, true}});
  auto multi = take(format::swizzle_channels(owners.document, described,
                                             {source(1), source(2)}, options));
  bool unavailable = false;
  try {
    run(owners, multi);
  } catch (const std::exception& error) {
    unavailable =
        std::string(error.what()).find("ViewUnavailable") != std::string::npos;
  }
  require(unavailable,
          "independent scalar owners cannot force one affine view");
  options.layout = "auto";
  multi = take(format::swizzle_channels(owners.document, described,
                                        {source(1), source(2)}, options));
  auto copied = run(owners, multi);
  check_oracle(copied, owners, 1, {1, {}, {}}, {{1, 0}, {2, 0}},
               Region::whole({2, 2}));
  f.bind_to(context);
  ExecutionOptions work;
  work.maximum_dependency_work = 1;
  require(context.execute(plan, f.bindings, {}, work).status().code ==
              ErrorCode::ResourceExhausted,
          "work budget enforced");
  graph.replace(f.document);
  require(context.execute(plan, f.bindings).status().code == ErrorCode::Stale,
          "stale plan cannot publish");
}
void literals_and_override() {
  for (auto dtype : {ElementType::UInt8, ElementType::UInt16, ElementType::Int8,
                     ElementType::Int16, ElementType::Int64,
                     ElementType::Float32, ElementType::Float64}) {
    Fixture f;
    f.add({dtype, {2, 4}});
    auto bits = std::vector<std::uint8_t>(Value::element_size(dtype), 255);
    if (dtype == ElementType::Int64) {
      const std::int64_t minimum = INT64_MIN;
      std::memcpy(bits.data(), &minimum, 8);
    }
    auto literal = source(0);
    literal.literal = format::ChannelLiteral{dtype, bits};
    format::ChannelAssemblyOptions options;
    options.metadata_mode = "raw";
    auto edge = take(format::swizzle_channels(
        f.document, edit_inputs(f, {{false, 1}}), {literal}, options));
    auto value = run(f, edge).results.at("result");
    require(std::memcmp(read(value, Region({{1, 1}, {0, 1}})).data(),
                        bits.data(), bits.size()) == 0,
            "typed literal preserves every dtype and Int64 minimum");
  }
  Fixture f;
  f.add({ElementType::Float32, {2, 4}});
  auto inputs = edit_inputs(f, {{false, {}}});
  format::ChannelAssemblyOptions options;
  options.metadata_mode = "override";
  TensorDescription described;
  described.channel_axis = 1;
  described.channels = {{"R", "red", ""},
                        {"G", "green", ""},
                        {"B", "blue", ""},
                        {"A", "alpha", ""}};
  options.input_overrides = {{0, described}};
  auto edge = take(format::swizzle_channels(f.document, inputs,
                                            {{0, {"name", "B"}, {}}}, options));
  auto result = run(f, edge);
  check_oracle(result, f, 1, {1}, {{0, 2}}, Region::whole({2, 1}));
  require(facets(f.bindings.inputs[0].result).empty(),
          "override leaves source unchanged");
  auto unused_plane = inputs;
  unused_plane.push_back(inputs[0]);
  require(
      !format::swizzle_channels(f.document, unused_plane, {source(0)}, options)
           .ok(),
      "A forbids external nonscalar inputs even when unused");
  options.input_overrides[9] = described;
  require(
      !format::swizzle_channels(f.document, inputs, {source(0)}, options).ok(),
      "override cannot name an absent original input");
  // An unrequested malformed scalar still fails its Descriptor checks.
  Fixture bad;
  bad.add({ElementType::Float32, {2, 4}});
  bad.add({ElementType::Float32, {2}});
  options = {};
  options.metadata_mode = "raw";
  require(!format::swizzle_channels(
               bad.document, edit_inputs(bad, {{false, 1}, {false, {}, true}}),
               {source(0)}, options)
                  .ok() &&
              bad.document.nodes.empty(),
          "unused scalar shape must be [1]");
  ResultRef surviving;
  std::vector<std::uint8_t> expected;
  {
    Fixture temporary;
    temporary.add({ElementType::Float32, {2, 4}});
    temporary.add({ElementType::Float32, {1}});
    expected = temporary.raw[1];
    options.layout = "view";
    auto view = take(format::swizzle_channels(
        temporary.document,
        edit_inputs(temporary, {{false, 1}, {false, {}, true}}), {source(1)},
        options));
    surviving = run(temporary, view).results.at("result");
  }
  require(std::memcmp(read(surviving, Region({{1, 1}, {0, 1}})).data(),
                      expected.data(), 4) == 0,
          "scalar view outlives every input and context owner");
}
}  // namespace
int main() try {
  generic();
  std::cerr << "generic passed\n";
  metadata();
  std::cerr << "metadata passed\n";
  planar_and_scalar();
  std::cerr << "planar passed\n";
  dependencies_and_errors();
  regression_boundaries();
  dynamic_and_resource();
  literals_and_override();
  std::cout << "FMT-03 generic/all-dtype, metadata, planar scalar/literal, "
               "dependency/error oracles passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
