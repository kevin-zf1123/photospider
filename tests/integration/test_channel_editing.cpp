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

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool okay, const std::string& message) {
  if (!okay)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
StridedLayout dense(const ValueDescriptor& d) {
  StridedLayout layout;
  layout.byte_strides.resize(d.shape.size());
  std::int64_t stride = Value::element_size(d.element_type);
  for (std::size_t i = d.shape.size(); i-- > 0;) {
    layout.byte_strides[i] = stride;
    stride *= d.shape[i];
  }
  return layout;
}
std::vector<std::uint8_t> bytes(const ValueDescriptor& d, unsigned seed) {
  const auto size = take(Region::whole(d.shape).element_count()) *
                    Value::element_size(d.element_type);
  std::vector<std::uint8_t> result(size);
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = (i * 73 + seed * 37) & 255;
  return result;
}
struct Fixture final {
  WorkflowDocument document;
  ExecutionBindings bindings;
  std::vector<std::vector<std::uint8_t>> raw;
  std::vector<ValueDescriptor> descriptors;
  WorkflowInput add(ValueDescriptor descriptor,
                    std::vector<ValueFacet> facets = {},
                    ResourceBindings resources = {}) {
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    auto data = bytes(descriptor, id);
    document.inputs.push_back({id, name, descriptor,
                               Region::whole(descriptor.shape),
                               dense(descriptor), facets});
    bindings.inputs.push_back(
        {name,
         take(Value::create(descriptor, Region::whole(descriptor.shape),
                            dense(descriptor), data, facets, resources))});
    raw.push_back(data);
    descriptors.push_back(descriptor);
    return WorkflowInputReference{id};
  }
  WorkflowInput image(ValueDescriptor descriptor, PlanarImageConfig config,
                      const std::vector<Region>& published = {}) {
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    auto data = bytes(descriptor, id);
    auto source = take(PlanarImage::create(descriptor, config));
    if (published.empty()) {
      require(source
                  .publish(Region::whole(descriptor.shape), data.data(),
                           data.size())
                  .ok(),
              "publish");
    } else {
      for (const auto& region : published) {
        auto value =
            take(Value::create(descriptor, Region::whole(descriptor.shape),
                               dense(descriptor), data));
        std::vector<std::uint8_t> packed;
        auto fp = take(Footprint::from_regions(descriptor.shape, {region}));
        const auto width = Value::element_size(descriptor.element_type);
        auto status = fp.visit(
            [&](const auto& at) {
              auto offset = take(value.byte_address(at));
              packed.insert(packed.end(), data.begin() + offset,
                            data.begin() + offset + width);
              return Status::success();
            },
            UINT64_MAX);
        require(status.ok(), "pack fixture");
        require(source.publish(region, packed.data(), packed.size()).ok(),
                "partial publish");
      }
    }
    document.inputs.push_back(
        {id,
         name,
         descriptor,
         Region::whole(descriptor.shape),
         {},
         {},
         PlanarImageLayout{config.order, config.height_axis, config.width_axis,
                           config.channel_axis, config.row_pitch_bytes,
                           config.groups}});
    ExecutionBinding binding;
    binding.name = name;
    binding.image = std::make_shared<const PlanarImage>(source);
    bindings.inputs.push_back(binding);
    raw.push_back(data);
    descriptors.push_back(descriptor);
    return WorkflowInputReference{id};
  }
};
ExecutionResult run(Fixture& f, WorkflowNodeOutput output,
                    const std::optional<Region>& roi = {},
                    std::shared_ptr<OperationRegistry> registry = {}) {
  f.document.outputs = {{"result", output.source_node, output.source_port}};
  if (!registry)
    registry = make_default_operation_registry();
  GraphContext graph(f.document);
  Compiler compiler(registry);
  PlanningOptions options;
  if (roi)
    options.output_regions = {{"result", *roi}};
  ResourceBindings resources;
  for (const auto& binding : f.bindings.inputs)
    if (binding.value.valid())
      resources = take(resources.unite(binding.value.resources()));
  auto compiled = take(compiler.compile(graph, options, resources));
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  return take(context.execute(compiled.plan, f.bindings));
}
void check_oracle(const ExecutionResult& result, const Fixture& f,
                  std::uint32_t output_axis,
                  const std::vector<std::optional<std::uint32_t>>& source_axes,
                  const std::vector<std::pair<unsigned, unsigned>>& mapping,
                  const Region& requested) {
  const bool image = result.images.count("result");
  const auto& descriptor = image ? result.images.at("result").descriptor()
                                 : result.values.at("result").descriptor();
  const auto width = Value::element_size(descriptor.element_type);
  std::vector<std::uint8_t> packed;
  if (image) {
    packed.resize(take(requested.element_count()) * width);
    require(result.images.at("result")
                .read(requested, packed.data(), packed.size())
                .ok(),
            "read image");
  }
  std::uint64_t linear = 0;
  auto footprint = take(Footprint::from_regions(descriptor.shape, {requested}));
  auto status = footprint.visit(
      [&](const auto& coordinate) {
        const auto selected = mapping.at(coordinate[output_axis]);
        auto at = coordinate;
        at.erase(at.begin() + output_axis);
        if (source_axes[selected.first])
          at.insert(at.begin() + *source_axes[selected.first], selected.second);
        if (f.descriptors[selected.first].shape ==
            std::vector<std::uint64_t>{1})
          at = {0};
        std::uint64_t source = 0;
        for (std::size_t i = 0; i < at.size(); ++i)
          source = source * f.descriptors[selected.first].shape[i] + at[i];
        const auto* observed =
            image
                ? packed.data() + linear * width
                : result.values.at("result").bytes().data() +
                      take(result.values.at("result").byte_address(coordinate));
        require(
            std::memcmp(observed, f.raw[selected.first].data() + source * width,
                        width) == 0,
            "independent byte oracle");
        ++linear;
        return Status::success();
      },
      UINT64_MAX);
  require(status.ok(), "oracle traversal");
}
std::vector<format::ChannelEditInput> edit_inputs(
    const Fixture& f,
    const std::vector<format::ChannelEditStructure>& structures) {
  std::vector<format::ChannelEditInput> result;
  for (std::size_t i = 0; i < f.document.inputs.size(); ++i) {
    const auto& d = f.document.inputs[i];
    OperationMetadata m;
    m.descriptor = d.descriptor;
    m.facets = d.facets;
    m.planar_layout = d.planar_layout;
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
  auto d =
      take(decode_tensor_description(result.values.at("result").facets()[0]));
  require(d.channels[0].role == "blue" && d.groups.size() == 1 &&
              d.groups[0].indices == std::vector<std::uint64_t>({2, 1, 0}) &&
              d.groups[0].alpha == 3,
          "A projects group and alpha uniquely");
  edge = take(format::swizzle_channels(f.document, {inputs[0]},
                                       {source(0), source(0), source(0, 2)}));
  result = run(f, edge);
  d = take(decode_tensor_description(result.values.at("result").facets()[0]));
  require(d.groups.empty() && d.channels[0].role == "red",
          "duplicate drops group");
  edge = take(format::replace_channels(
      f.document, inputs,
      {{{"role", "red"}, source(1)}, {{"name", "A"}, source(2)}}));
  result = run(f, edge);
  d = take(decode_tensor_description(result.values.at("result").facets()[0]));
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
    require(result.diagnostics.source_read_bytes == 112,
            "sparse base/external/scalar exact reads");
    const Region alpha({{127, 3}, {127, 3}, {3, 1}});
    for (const std::uint32_t bits :
         {0x80000000U, 0x7fc01234U, 0xff812345U, 0x3f000000U}) {
      std::vector<std::uint8_t> bytes(4);
      std::memcpy(bytes.data(), &bits, 4);
      f.raw[2] = bytes;
      f.bindings.inputs[2].value = take(Value::create(
          {ElementType::Float32, {1}}, Region::whole({1}), {0, {4}}, bytes));
      result = run(f, edge, alpha);
      check_oracle(result, f, 2, {2, {}, {}}, {{1, 0}, {0, 1}, {0, 2}, {2, 0}},
                   alpha);
      require(result.diagnostics.source_read_bytes == 4,
              "scalar only reads no base pixels");
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
    require(result.diagnostics.source_read_bytes == 0,
            "literal provider reads no base");
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
  auto cert = take(DependencyCertificate::create_mapped(
      "oracle", take(Footprint::all({2, 4})), {{2, 4}, {1}},
      *prepared->traits().outputs[0].static_dependency_pieces));
  auto alpha =
      take(Footprint::from_regions({2, 4}, {Region({{1, 1}, {3, 1}})}));
  auto needs = take(cert.backward(alpha));
  require(needs.size() == 1 && needs[0].port == 1 &&
              take(needs[0].samples.element_count()) == 1,
          "scalar backward exact");
  auto dirty =
      take(cert.transpose({1,
                           static_cast<std::uint32_t>(DependencyRole::Data),
                           take(Footprint::all({1})),
                           {}}));
  require(take(dirty.element_count()) == 2,
          "scalar dirty full spatial only selected slot");
  auto overwritten =
      take(Footprint::from_regions({2, 4}, {Region({{0, 2}, {3, 1}})}));
  require(take(take(cert.transpose(
                        {0,
                         static_cast<std::uint32_t>(DependencyRole::Data),
                         overwritten,
                         {}}))
                   .element_count()) == 0,
          "overwritten base not dirty");
  auto wrong = inputs;
  wrong[0].metadata.descriptor.shape[0] = 3;
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
  forwarded[0].metadata.descriptor.shape[0] = 3;
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
  auto output =
      take(decode_tensor_description(result.values.at("result").facets()[0]));
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
  for (std::uint32_t bits : {0x80000000U, 0x7fc01234U, 0x3f000000U}) {
    std::memcpy(f.raw[1].data(), &bits, 4);
    f.bindings.inputs[1].value = take(Value::create(
        {ElementType::Float32, {1}}, Region::whole({1}), {0, {4}}, f.raw[1]));
    auto result = take(context.execute(plan, f.bindings));
    check_oracle(result, f, 1, {1, {}}, {{1, 0}, {1, 0}},
                 Region::whole({2, 2}));
    const auto& value = result.values.at("result");
    require(value.layout().byte_strides == std::vector<std::int64_t>({0, 0}),
            "generic constant view has scalar-sized zero-stride storage");
  }
  Value retained;
  {
    ExecutionContext temporary(registry);
    retained = take(temporary.execute(plan, f.bindings)).values.at("result");
  }
  auto saved_bindings = f.bindings;
  f.bindings.inputs.clear();
  const float half = .5f;
  require(
      std::memcmp(retained.bytes().data() + take(retained.byte_address({1, 1})),
                  &half, sizeof(half)) == 0,
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
  ExecutionContextConfig limited;
  limited.cpu_workers = 1;
  limited.managed_resources = ResourceLimits{};
  limited.managed_resources->maximum_work = 1;
  ExecutionContext short_work(registry, limited);
  require(short_work.execute(plan, f.bindings).status().code ==
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
    auto value = run(f, edge).values.at("result");
    require(std::memcmp(value.bytes().data() + take(value.byte_address({1, 0})),
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
  require(f.bindings.inputs[0].value.facets().empty(),
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
  Value surviving;
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
    surviving = run(temporary, view).values.at("result");
  }
  require(std::memcmp(
              surviving.bytes().data() + take(surviving.byte_address({1, 0})),
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
