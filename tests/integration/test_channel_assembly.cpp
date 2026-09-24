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

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
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
  auto output =
      take(decode_tensor_description(result.values.at("result").facets()[0]));
  require(output.channels[2].interpretation->model == "cielab" &&
              output.channels[2].interpretation->primaries.empty(),
          "target removes incompatible RGB fields");
  require(output.channels[0].interpretation->primaries == "srgb",
          "unredefined source fields retained");
  require(
      take(decode_tensor_description(f.bindings.inputs[0].value.facets()[0]))
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
  auto decoded =
      take(decode_tensor_description(result.values.at("result").facets()[0]));
  require(decoded.groups.size() == 1 &&
              decoded.channels[0].interpretation->model == "rgb",
          "explicit RGB group");
  auto encoded = take(encode_tensor_description(rgb));
  require(encoded.version == 3, "version discriminator");
  encoded.version = 1;
  require(!decode_tensor_description(encoded).ok(), "old version rejection");
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
      require(result.diagnostics.source_read_bytes == 36,
              "only one input ROI read");
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
    require(result.diagnostics.source_read_bytes == 18,
            "gap not read and reuse deduplicated");
    // Split/reassemble restores a canonical common owner and survives context.
    Fixture split;
    source = split.image({ElementType::UInt8, {3, 4, 3}}, config);
    OperationMetadata md;
    md.descriptor = split.document.inputs[0].descriptor;
    md.planar_layout = split.document.inputs[0].planar_layout;
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
    require(result.images.at("result").owner_token() ==
                split.bindings.inputs[0].image->owner_token(),
            "reassemble common root");
    check_oracle(result, split, 2, {2}, {{0, 0}, {0, 1}, {0, 2}},
                 Region::whole({3, 4, 3}));
    split.bindings.inputs.clear();
    std::vector<std::uint8_t> restored(36);
    require(
        result.images.at("result")
            .read(Region::whole({3, 4, 3}), restored.data(), restored.size())
            .ok(),
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
  metadata.descriptor = f.descriptors[0];
  auto preparation = take(
      registry->prepare_operation(f.document.nodes[0].operation, {metadata},
                                  f.document.nodes[0].parameters));
  const auto& traits = preparation->traits();
  require(!traits.cacheable, "sample-only cache disabled");
  auto all = take(Footprint::all({2, 3}));
  auto certificate = take(DependencyCertificate::create_mapped(
      "oracle", all, {{2, 3}}, *traits.outputs[0].static_dependency_pieces));
  auto sparse = take(Footprint::from_regions(
      {2, 3}, {Region({{0, 1}, {0, 1}}), Region({{1, 1}, {2, 1}})}));
  auto needs = take(certificate.backward(sparse));
  require(needs.size() == 1 && take(needs[0].samples.element_count()) == 2,
          "exact disjoint backward support");
  auto dirty =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {2, 1}})}));
  auto affected = take(certificate.transpose(
      {0, static_cast<std::uint32_t>(DependencyRole::Data), dirty, {}}));
  require(take(affected.element_count()) == 2,
          "dirty repeated source fans out");
  auto ignored =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {1, 1}})}));
  require(take(take(certificate.transpose(
                        {0,
                         static_cast<std::uint32_t>(DependencyRole::Data),
                         ignored,
                         {}}))
                   .element_count()) == 0,
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
  metadata.descriptor = f.descriptors[0];
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
  ExecutionContextConfig limited;
  limited.cpu_workers = 1;
  limited.managed_resources = ResourceLimits{};
  limited.managed_resources->capacity[ResourceKind::Referenced] = 1;
  ExecutionContext short_reference(registry, limited);
  require(short_reference.execute(compiled.plan, f.bindings).status().code ==
              ErrorCode::ResourceExhausted,
          "mixed generic referenced owner admitted");
  limited.managed_resources = ResourceLimits{};
  limited.managed_resources->maximum_work = 1;
  ExecutionContext short_work(registry, limited);
  require(short_work.execute(compiled.plan, f.bindings).status().code ==
              ErrorCode::ResourceExhausted,
          "work budget charged");
  limited.managed_resources.reset();
  limited.maximum_live_bytes = 1024;
  ExecutionContext short_memory(registry, limited);
  require(short_memory.execute(compiled.plan, f.bindings).status().code ==
              ErrorCode::ResourceExhausted,
          "page/metadata budget charged");
  CancellationSource stop;
  stop.cancel();
  ExecutionContext context(registry);
  require(
      context.execute(compiled.plan, f.bindings, stop.token()).status().code ==
          ErrorCode::Cancelled,
      "cancel before publication");
}
void direct_strides_and_bits() {
  auto registry = make_default_operation_registry();
  const ValueDescriptor descriptor{ElementType::Float32, {4}};
  const std::uint32_t patterns[] = {0x80000000U, 0x7f800000U, 0x7fc12345U,
                                    0xff812345U};
  std::vector<std::uint8_t> data(sizeof(patterns));
  std::memcpy(data.data(), patterns, sizeof(patterns));
  auto base =
      take(Value::create(descriptor, Region::whole({4}), {0, {4}}, data));
  auto negative = take(Value::from_storage(descriptor, Region::whole({4}),
                                           {12, {-4}}, base.storage()));
  auto zero = take(Value::from_storage(descriptor, Region::whole({4}), {0, {0}},
                                       base.storage()));
  std::vector<Value> inputs{negative, zero};
  std::vector<Region> demands(2, Region::whole({4}));
  std::map<std::string, ParameterValue> params{
      {"axis", std::int64_t{1}},
      {"layout", std::string("auto")},
      {"metadata_mode", std::string("raw")}};
  OperationInvocation call(inputs, demands, params, Backend::Cpu, {},
                           Region::whole({4, 2}));
  auto result = take(registry->invoke("channel.assemble_strict", call));
  for (std::uint64_t i = 0; i < 4; ++i)
    for (std::uint64_t c = 0; c < 2; ++c) {
      const auto expected = c == 0 ? patterns[3 - i] : patterns[0];
      require(
          std::memcmp(result.bytes().data() + take(result.byte_address({i, c})),
                      &expected, 4) == 0,
          "signed zero, Inf and NaN payload preserved on negative/zero "
          "strides");
    }
  params["layout"] = std::string("view");
  require(!registry->invoke("channel.assemble_strict", call).ok(),
          "nonaffine forced view fails");
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
      take(decode_tensor_description(f.bindings.inputs[1].value.facets()[0]))
              .reference == "right",
      "override is local");
  auto output =
      take(decode_tensor_description(result.values.at("result").facets()[0]));
  require(output.channels[1].interpretation->reference == "left",
          "override projected");
  options.metadata_mode = "raw";
  options.input_overrides.clear();
  edge = take(format::assemble_channels(f.document, {a, b}, 1, options));
  result = run(f, edge);
  output =
      take(decode_tensor_description(result.values.at("result").facets()[0]));
  require(output.channels[0].interpretation->reference == "left" &&
              output.channels[1].interpretation->reference == "right",
          "raw preserves independent applicable interpretations");
  // A real owned ICC resource is referenced only by a component, not globals.
  Value surviving;
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
    surviving = run(owned, node).values.at("result");
    require(surviving.resources().icc_profile(identity).ok(),
            "component profile retained");
    require(
        !Value::create({ElementType::UInt8, {2}}, Region::whole({2}), {0, {1}},
                       {1, 2}, {take(encode_tensor_description(described))})
             .ok(),
        "unowned component profile rejected");
  }
  require(surviving.resources().icc_profile(identity).ok(),
          "profile survives context/source teardown");
}
}  // namespace
int main() try {
  generic_oracle();
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
