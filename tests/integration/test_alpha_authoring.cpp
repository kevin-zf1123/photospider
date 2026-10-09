#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../support/channel_result_fixture.hpp"
#include "photospider/ops/format/alpha.hpp"
#include "photospider/ops/format/metadata.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using assembly_fixture::Fixture;
using assembly_fixture::require;
using channel_fixture::take;
TensorDescription gray(std::optional<unsigned> axis, bool alpha = true) {
  TensorDescription d;
  d.association = "straight";
  if (!axis) {
    d.component = TensorChannelDescription{"Y", "gray", "relative"};
    d.model = "gray";
    return d;
  }
  d.channel_axis = *axis;
  d.channels = {{"Y", "gray", "relative"}};
  if (alpha)
    d.channels.push_back({"A", "alpha", "coverage"});
  TensorColorGroup group;
  group.name = "gray";
  group.indices = {0};
  group.components = {d.channels[0]};
  group.interpretation.model = "gray";
  group.interpretation.association = "straight";
  if (alpha)
    group.alpha = 1;
  d.groups = {group};
  return d;
}
WorkflowInput add(Fixture* f, ElementType type,
                  std::vector<std::uint64_t> shape, TensorDescription d,
                  const std::vector<std::uint64_t>& bits,
                  ResultTensorLayout layout = {},
                  std::vector<std::uint64_t> batches = {},
                  bool descriptor_only = false) {
  if (type != ElementType::Float32 && type != ElementType::Float64) {
    d.encoding = TensorEncoding{};
    for (auto& group : d.groups)
      for (auto& channel : group.components)
        channel.encoding = TensorEncoding{};
  }
  auto source = channel_fixture::source({type, std::move(shape)},
                                        {take(encode_tensor_description(d))},
                                        std::move(layout), std::move(batches));
  const auto width = Value::element_size(type);
  require(bits.size() * width == source.bytes.size(), "fixture bits count");
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(source.bytes.data() + i * width, &bits[i], width);
  if (descriptor_only)
    source.coverage.clear();
  return f->add_source(std::move(source));
}
Result<ExecutionResult> execute(Fixture& f, WorkflowNodeOutput output,
                                const std::optional<Region>& region = {}) {
  auto registry = make_default_operation_registry();
  auto config = f.execution_config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  f.bind_to(context);
  f.document.outputs = {{"result", output.source_node, output.source_port}};
  GraphContext graph(f.document);
  PlanningOptions planning;
  if (region)
    planning.output_regions = {{"result", *region}};
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  return context.execute(compiled.value().plan, f.bindings);
}
const ResultTensorSpec& spec(const ExecutionResult& run) {
  return run.results.at("result").schema().tensors[0];
}
TensorDescription description(const ExecutionResult& run) {
  for (const auto& facet : spec(run).facets)
    if (facet.key == "photospider.tensor-description")
      return take(decode_tensor_description(facet));
  throw std::runtime_error("missing tensor description");
}
std::vector<std::uint64_t> observed(const ExecutionResult& run,
                                    const std::optional<Region>& region = {}) {
  const auto bytes = channel_fixture::read(
      run.results.at("result"),
      region.value_or(Region::whole(spec(run).sample_shape())));
  const auto width = Value::element_size(spec(run).descriptor.element_type);
  std::vector<std::uint64_t> result(bytes.size() / width);
  for (std::size_t i = 0; i < result.size(); ++i)
    std::memcpy(&result[i], bytes.data() + i * width, width);
  return result;
}
void exact_copy_and_remove() {
  for (auto type : {ElementType::UInt8, ElementType::UInt16, ElementType::Int8,
                    ElementType::Int16, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    const auto mask =
        width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
    const auto sentinel =
        (type == ElementType::Float32   ? UINT64_C(0xff812345)
         : type == ElementType::Float64 ? UINT64_C(0xfff0123456789abc)
                                        : UINT64_C(0xfedcba9876543210)) &
        mask;
    for (unsigned rank = 1; rank <= 8; ++rank)
      for (unsigned axis = 0; axis < rank; ++axis) {
        std::vector<std::uint64_t> shape(rank, 1);
        shape[axis] = 2;
        Fixture f;
        auto input = add(&f, type, shape, gray(axis), {17, sentinel});
        format::ExtractAlphaOptions e;
        e.group = "gray";
        e.keepdims = rank == 1;
        auto output =
            take(format::extract_alpha(f.document, input, f.metadata(0), e));
        const auto run = take(execute(f, output));
        require(observed(run) == std::vector<std::uint64_t>{sentinel},
                "alpha copy preserves bits");
        require(
            description(run).groups.empty() && description(run).model.empty(),
            "alpha output has no color group");
        require(assembly_fixture::source_bytes(run, f) == width,
                "extraction exact support");
        format::RemoveAlphaOptions r;
        r.group = "gray";
        output =
            take(format::remove_alpha(f.document, input, f.metadata(0), r));
        const auto removed = take(execute(f, output));
        require(observed(removed) == std::vector<std::uint64_t>{17},
                "removal exact color bits");
        require(spec(removed).descriptor.shape.size() == rank &&
                    description(removed).channel_axis == axis &&
                    !description(removed).groups[0].alpha,
                "removal preserves singleton axis");
      }
  }
}
void opaque_encoding() {
  const std::vector<std::pair<ElementType, std::uint64_t>> values{
      {ElementType::UInt8, 255},
      {ElementType::UInt16, 65535},
      {ElementType::Int8, 127},
      {ElementType::Int16, 32767},
      {ElementType::Int64, UINT64_C(0x7fffffffffffffff)},
      {ElementType::Float32, UINT64_C(0x3f800000)},
      {ElementType::Float64, UINT64_C(0x3ff0000000000000)}};
  for (const auto& [type, expected] : values)
    for (bool component : {false, true})
      for (bool keep : {false, true}) {
        Fixture f;
        auto d = gray(
            component ? std::optional<unsigned>{} : std::optional<unsigned>{2},
            false);
        const auto shape = component ? std::vector<std::uint64_t>{2, 3}
                                     : std::vector<std::uint64_t>{2, 3, 1};
        ResultTensorLayout layout;
        layout.spatial = true;
        layout.height_axis = 0;
        layout.width_axis = 1;
        layout.channel_axis = component ? std::optional<std::uint32_t>{}
                                        : std::optional<std::uint32_t>{2};
        auto input = add(&f, type, shape, d, std::vector<std::uint64_t>(24),
                         layout, {2, 2}, true);
        format::ExtractAlphaOptions e;
        e.group = component ? "Y" : "gray";
        e.keepdims = keep;
        e.missing_alpha = "opaque";
        auto output =
            take(format::extract_alpha(f.document, input, f.metadata(0), e));
        auto run = take(execute(f, output));
        require(observed(run) == std::vector<std::uint64_t>(24, expected),
                "opaque dtype exact inverse");
        require(
            spec(run).batch_axes == f.sources[0].schema.tensors[0].batch_axes,
            "opaque preserves batches");
        require(
            spec(run).descriptor.shape.size() == (component || !keep ? 2U : 3U),
            "component keepdims creates no axis");
        require(assembly_fixture::source_bytes(run, f) == 0,
                "opaque descriptor-only source");
        e.layout = "view";
        output =
            take(format::extract_alpha(f.document, input, f.metadata(0), e));
        auto failed = execute(f, output);
        require(!failed.ok() && failed.status().message.find(
                                    "ViewUnavailable") != std::string::npos,
                "opaque view rejected");
      }
  Fixture f;
  auto input = add(&f, ElementType::UInt16, {1}, gray({}), {0});
  format::ExtractAlphaOptions e;
  e.group = "Y";
  e.missing_alpha = "opaque";
  TensorEncoding encoding;
  encoding.stored = {std::int64_t{0}, std::int64_t{1023}};
  e.alpha_encoding = encoding;
  auto output =
      take(format::extract_alpha(f.document, input, f.metadata(0), e));
  require(
      observed(take(execute(f, output))) == std::vector<std::uint64_t>{1023},
      "ten-bit opaque");
  encoding.decoded = {std::int64_t{1}, std::int64_t{0}};
  e.alpha_encoding = encoding;
  output = take(format::extract_alpha(f.document, input, f.metadata(0), e));
  require(observed(take(execute(f, output))) == std::vector<std::uint64_t>{0},
          "descending encoding");
  encoding.stored = {std::int64_t{0}, std::int64_t{1}};
  encoding.decoded = {std::int64_t{0}, std::int64_t{2}};
  e.alpha_encoding = encoding;
  const auto count = f.document.nodes.size();
  require(!format::extract_alpha(f.document, input, f.metadata(0), e).ok() &&
              f.document.nodes.size() == count,
          "fractional integer code rejected transactionally");
  for (auto type : {ElementType::Float32, ElementType::Float64}) {
    Fixture floating;
    auto gray_input = add(&floating, type, {1}, gray({}), {0});
    encoding.stored = {std::int64_t{0}, std::int64_t{1}};
    encoding.decoded = {std::int64_t{0}, std::int64_t{3}};
    e.alpha_encoding = encoding;
    require(!format::extract_alpha(floating.document, gray_input,
                                   floating.metadata(0), e)
                    .ok() &&
                floating.document.nodes.empty(),
            "one third opaque is not exactly representable in binary floating "
            "dtype");
    encoding.decoded = {std::int64_t{0}, 0.5};
    e.alpha_encoding = encoding;
    require(!format::extract_alpha(floating.document, gray_input,
                                   floating.metadata(0), e)
                    .ok() &&
                floating.document.nodes.empty(),
            "opaque code outside stored legal interval rejected");
    Fixture present;
    const auto snan = type == ElementType::Float32
                          ? UINT64_C(0x7f812345)
                          : UINT64_C(0x7ff0123456789abc);
    auto existing = add(&present, type, {1, 2}, gray(1), {0, snan});
    e.group = "gray";
    auto copy = take(format::extract_alpha(present.document, existing,
                                           present.metadata(0), e));
    require(observed(take(execute(present, copy))) ==
                std::vector<std::uint64_t>{snan},
            "fallback encoding never re-encodes existing alpha");
    e.group = "Y";
  }
}
void identity_and_shared_alpha() {
  for (bool component : {false, true}) {
    Fixture f;
    auto input = add(
        &f, ElementType::Float32,
        component ? std::vector<std::uint64_t>{2}
                  : std::vector<std::uint64_t>{2, 1},
        gray(component ? std::optional<unsigned>{} : std::optional<unsigned>{1},
             false),
        {0x80000000, 0x7f812345});
    format::RemoveAlphaOptions r;
    r.group = component ? "Y" : "gray";
    r.missing_alpha = "identity";
    for (const auto* policy : {"auto", "view", "materialize"}) {
      r.layout = policy;
      auto output =
          take(format::remove_alpha(f.document, input, f.metadata(0), r));
      auto run = take(execute(f, output));
      require(
          observed(run) == std::vector<std::uint64_t>{0x80000000, 0x7f812345},
          "identity exact bits");
      require(assembly_fixture::source_bytes(run, f) == 8,
              "identity reads Data");
      require((assembly_fixture::owner(run.results.at("result")) ==
               assembly_fixture::owner(f.bindings.inputs[0].result)) ==
                  (r.layout != "materialize"),
              "identity layout selects retained or independent owner");
    }
  }
  Fixture f;
  auto d = gray(1);
  d.channels.push_back({"other", "gray", "relative"});
  auto other = d.groups[0];
  other.name = "other";
  other.indices = {2};
  other.components = {d.channels[2]};
  d.groups.push_back(other);
  auto input = add(&f, ElementType::UInt8, {1, 3}, d, {7, 99, 11});
  format::RemoveAlphaOptions r;
  r.group = "gray";
  auto output = take(format::remove_alpha(f.document, input, f.metadata(0), r));
  auto run = take(execute(f, output));
  require(observed(run) == std::vector<std::uint64_t>{7, 99, 11},
          "shared alpha retained");
  const auto after = description(run);
  require(!after.groups[0].alpha && after.groups[1].alpha == 1,
          "only selected group detaches alpha");
}
void roi_and_lifetime() {
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    Fixture f;
    ResultTensorLayout layout;
    layout.spatial = true;
    layout.height_axis = 0;
    layout.width_axis = 1;
    layout.channel_axis = 2;
    layout.order = order;
    if (order == ImagePlaneOrder::Continuous)
      layout.row_pitch_bytes = 320;
    std::vector<std::uint64_t> bits(2 * 130 * 131 * 2);
    for (std::size_t i = 0; i < bits.size(); ++i)
      bits[i] = i & 65535;
    auto input =
        add(&f, ElementType::UInt16, {130, 131, 2}, gray(2), bits, layout, {2});
    const Region source_roi({{1, 1}, {127, 3}, {127, 4}, {1, 1}});
    const Region output_roi({{1, 1}, {127, 3}, {127, 4}});
    f.sources[0].coverage = {source_roi};
    format::ExtractAlphaOptions e;
    e.group = "gray";
    e.layout = "view";
    auto output =
        take(format::extract_alpha(f.document, input, f.metadata(0), e));
    auto run = take(execute(f, output, output_roi));
    const auto expected =
        channel_fixture::read(f.bindings.inputs[0].result, source_roi);
    require(
        channel_fixture::read(run.results.at("result"), output_roi) == expected,
        "batched cross-tile ROI exact bytes");
    require(assembly_fixture::source_bytes(run, f) == 24,
            "ROI source support exact");
    auto retained = run.results.at("result");
    f.bindings.inputs.clear();
    run = {};
    require(channel_fixture::read(retained, output_roi) == expected,
            "view survives context and binding retirement");
  }
}
void assertions() {
  for (int route = 0; route < 4; ++route) {
    const bool component = route == 2;
    const bool opaque = route == 3;
    Fixture f;
    auto d =
        gray(component ? std::optional<unsigned>{} : std::optional<unsigned>{1},
             !opaque);
    if (opaque)
      d.channels.push_back({"unused", "", ""});
    auto input = add(&f, ElementType::Float32,
                     component ? std::vector<std::uint64_t>{2}
                               : std::vector<std::uint64_t>{1, 2},
                     d, {1, 2});
    auto source_metadata = f.metadata(0);
    auto expand = [&](WorkflowInput edge, const OperationMetadata& metadata) {
      if (route == 0 || opaque) {
        format::ExtractAlphaOptions e;
        e.group = "gray";
        e.missing_alpha = opaque ? "opaque" : "error";
        return format::extract_alpha(f.document, edge, metadata, e);
      }
      format::RemoveAlphaOptions r;
      r.group = component ? "Y" : "gray";
      r.missing_alpha = component ? "identity" : "error";
      return format::remove_alpha(f.document, edge, metadata, r);
    };
    auto wrong =
        std::make_shared<SchemaTemplate>(*source_metadata.result_schema);
    wrong->tensors[0].facets.push_back({"z.opaque", 1, {7}});
    OperationMetadata wrong_metadata;
    wrong_metadata.result_schema = wrong;
    require(!expand(input, wrong_metadata).ok() && f.document.nodes.empty(),
            "declaration rejects full-schema drift transactionally");
    const WorkflowNodeOutput future{100, "values"};
    const auto output = take(expand(future, source_metadata));
    format::MetadataOptions options;
    options.mode = "replace";
    options.description =
        gray(component ? std::optional<unsigned>{} : std::optional<unsigned>{1},
             !opaque);
    options.description->encoding = TensorEncoding{};
    if (!component && opaque)
      options.description->channels.push_back({"unused", "", ""});
    if (component) {
      options.description->component->name = "changed";
    } else {
      options.description->channels[0].name = "changed";
      options.description->groups[0].components[0].name = "changed";
    }
    take(format::assign_metadata(f.document, input, options));
    f.document.nodes.back().id = future.source_node;
    const auto stale = execute(f, output);
    require(!stale.ok() && stale.status().message.find(
                               opaque ? "descriptor assertion" : "inference") !=
                               std::string::npos,
            "forward-source assertion rejects producer drift route " +
                std::to_string(route) + ": " + stale.status().message);
  }
}
void identity_assertions_and_empty() {
  Fixture f;
  auto input = add(&f, ElementType::Float32, {2, 3}, gray({}),
                   {0, 0x80000000, 0x7f812345, 1, 2, 3});
  auto schema = std::make_shared<SchemaTemplate>(*f.metadata(0).result_schema);
  schema->tensors[0].layout.spatial = true;
  schema->tensors[0].layout.height_axis = 0;
  schema->tensors[0].layout.width_axis = 1;
  schema->tensors[0].layout.channel_axis.reset();
  schema->tensors[0].layout.order = ImagePlaneOrder::Continuous;
  schema->tensors[0].facets.push_back(
      {"z.large-opaque", 1, std::vector<std::uint8_t>(20000, 42)});
  f.sources[0].schema = *schema;
  f.document.inputs[0].result_schema = schema;
  format::RemoveAlphaOptions r;
  r.group = "Y";
  r.missing_alpha = "identity";
  r.layout = "view";
  const WorkflowNodeOutput future{100, "values"};
  const auto output =
      take(format::remove_alpha(f.document, future, f.metadata(0), r));
  require(std::get<std::string>(
              f.document.nodes.back().parameters.at("expected_source"))
                  .size() < 256,
          "large opaque metadata uses bounded complete assertion");
  const auto producer = take(format::assign_metadata(f.document, input));
  (void)producer;
  f.document.nodes.back().id = future.source_node;
  auto run = take(execute(f, output));
  require(observed(run) ==
              std::vector<std::uint64_t>({0, 0x80000000, 0x7f812345, 1, 2, 3}),
          "component identity through unchanged producer");
  auto changed = std::make_shared<SchemaTemplate>(*schema);
  changed->tensors[0].layout.order = ImagePlaneOrder::Tiled;
  // Physical storage order is outside SchemaTemplate::canonical().
  require(changed->canonical() == schema->canonical(),
          "fixture isolates physical layout drift");
  f.document.inputs[0].result_schema = changed;
  GraphContext drifted(f.document);
  const auto stale_layout =
      Compiler(make_default_operation_registry()).compile(drifted);
  require(!stale_layout.ok() &&
              stale_layout.status().message.find("source metadata disagrees") !=
                  std::string::npos,
          "forward component identity rejects layout-only drift");
  f.document.inputs[0].result_schema = schema;
  auto registry = make_default_operation_registry();
  ExecutionContext context(registry);
  f.bind_to(context);
  GraphContext graph(f.document);
  auto compiled = take(Compiler(registry).compile(graph));
  auto frozen = take(context.freeze(compiled.plan, f.bindings));
  const auto root = take(context.resource_budget());
  const auto before = root.statistics().peak[ResourceKind::Payload];
  auto empty = take(context.execute_fragments(
      frozen, {{"result", take(Footprint::none({2, 3}))}}));
  require(take(empty.results.at("result").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              root.statistics().peak[ResourceKind::Payload] == before,
          "Empty composition has no sample or continuation payload");
  auto a = std::async(std::launch::async, [&] {
    return context.execute(compiled.plan, f.bindings);
  });
  auto b = std::async(std::launch::async, [&] {
    return context.execute(compiled.plan, f.bindings);
  });
  require(observed(take(a.get())) == observed(take(b.get())),
          "compiled helper reuses immutable preparation concurrently");
}
void rejection_and_dirty() {
  Fixture f;
  auto input = add(&f, ElementType::Float32, {2, 2}, gray(1),
                   {0x80000000, 0x7f812345, 0x3f800000, 0x7fc12345});
  format::RemoveAlphaOptions r;
  r.group = "gray";
  const auto output =
      take(format::remove_alpha(f.document, input, f.metadata(0), r));
  f.sources[0].coverage = {Region({{0, 2}, {0, 1}})};
  const auto run = take(execute(f, output));
  require(observed(run) == std::vector<std::uint64_t>({0x80000000, 0x3f800000}),
          "remove never reads missing alpha coverage");
  const auto discarded =
      take(Footprint::from_regions({2, 2}, {Region({{0, 2}, {1, 1}})}));
  require(take(run.dependencies.potential_dirty("input1", discarded, 1, {},
                                                ResultSupportTarget::Tensor, 0))
              .at("result")
              .empty(),
          "discarded alpha has no Data dirty output");
  format::ExtractAlphaOptions e;
  e.group = "gray";
  e.missing_alpha = "opaque";
  const auto extract =
      take(format::extract_alpha(f.document, input, f.metadata(0), e));
  require(!execute(f, extract).ok(),
          "existing uncovered alpha cannot become opaque fallback");
  const auto nodes = f.document.nodes.size();
  e.metadata_mode = "raw";
  require(!format::extract_alpha(f.document, input, f.metadata(0), e).ok() &&
              f.document.nodes.size() == nodes,
          "semantic extraction rejects raw transactionally");
  auto bad = std::make_shared<SchemaTemplate>(*f.metadata(0).result_schema);
  auto d = gray(1);
  d.groups[0].interpretation.association = "premultiplied";
  d.association = "premultiplied";
  bad->tensors[0].facets = {take(encode_tensor_description(d))};
  f.document.inputs[0].result_schema = bad;
  require(!format::remove_alpha(f.document, input, f.metadata(0), r).ok() &&
              f.document.nodes.size() == nodes,
          "editing requires straight boundary");
}

}  // namespace
int main() {
  try {
    exact_copy_and_remove();
    opaque_encoding();
    identity_and_shared_alpha();
    roi_and_lifetime();
    assertions();
    identity_assertions_and_empty();
    rejection_and_dirty();
    std::cout << "alpha authoring Result checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
