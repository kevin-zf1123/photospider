#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11): C++17 floating-environment regression.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../support/alpha_golden.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces): local test/example helpers.
void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
template <class T>
T take(Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(
        "code=" + std::to_string(static_cast<int>(result.status().code)) +
        ": " + result.status().message);
  }
  return result.take_value();
}
std::uint64_t bits(double x, ElementType type = ElementType::Float32) {
  std::uint64_t value = 0;
  if (type == ElementType::Float32) {
    const float f = static_cast<float>(x);
    std::memcpy(&value, &f, 4);
  } else {
    std::memcpy(&value, &x, 8);
  }
  return value;
}
StridedLayout dense(const ValueDescriptor& descriptor) {
  StridedLayout layout;
  layout.byte_strides.resize(descriptor.shape.size());
  std::int64_t stride = Value::element_size(descriptor.element_type);
  for (std::size_t i = descriptor.shape.size(); i-- > 0;) {
    layout.byte_strides[i] = stride;
    stride *= descriptor.shape[i];
  }
  return layout;
}
TensorDescription rgb(unsigned axis, unsigned count = 4,
                      std::optional<std::uint64_t> alpha = 3) {
  TensorDescription d;
  d.channel_axis = axis;
  d.channels = {{"R", "red", "relative"},
                {"G", "green", "relative"},
                {"B", "blue", "relative"},
                {"A", "alpha", "coverage"}};
  d.channels.resize(count);
  TensorColorGroup group;
  group.name = "rgb";
  group.indices = {0, 1, 2};
  group.components = {d.channels[0], d.channels[1], d.channels[2]};
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  group.interpretation.association = "straight";
  group.alpha = alpha;
  d.groups = {group};
  return d;
}
TensorDescription gray(unsigned axis, bool has_alpha = true) {
  TensorDescription d;
  d.channel_axis = axis;
  d.channels = {{"Y", "gray", "relative"}};
  if (has_alpha) {
    d.channels.push_back({"A", "alpha", "coverage"});
  }
  TensorColorGroup g;
  g.name = "gray";
  g.indices = {0};
  g.components = {d.channels[0]};
  g.interpretation.model = "gray";
  g.interpretation.association = "straight";
  if (has_alpha) {
    g.alpha = 1;
  }
  d.groups = {g};
  return d;
}
TensorDescription component_gray() {
  TensorDescription d;
  d.component = TensorChannelDescription{"Y", "gray", "relative"};
  d.model = "gray";
  d.association = "straight";
  return d;
}
struct Fixture {
  WorkflowDocument document;
  ExecutionBindings bindings;
  WorkflowInput add(const ValueDescriptor& descriptor,
                    const std::vector<std::uint64_t>& samples,
                    const std::optional<TensorDescription>& description = {},
                    const std::optional<PlanarImageConfig>& image_config = {},
                    const std::optional<std::vector<Region>>& published = {}) {
    const auto count = take(Region::whole(descriptor.shape).element_count());
    require(count == samples.size(), "fixture sample count");
    const auto width = Value::element_size(descriptor.element_type);
    std::vector<std::uint8_t> bytes(count * width);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      std::memcpy(bytes.data() + i * width, &samples[i], width);
    }
    std::vector<ValueFacet> facets;
    if (description) {
      facets.push_back(take(encode_tensor_description(*description)));
    }
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    WorkflowInputDeclaration declaration{id,
                                         name,
                                         descriptor,
                                         Region::whole(descriptor.shape),
                                         dense(descriptor),
                                         facets};
    ExecutionBinding binding;
    binding.name = name;
    auto value = take(Value::create(descriptor, Region::whole(descriptor.shape),
                                    dense(descriptor), bytes, facets));
    if (image_config) {
      const auto& c = *image_config;
      declaration.layout = {};
      declaration.planar_layout =
          PlanarImageLayout{c.order,        c.height_axis,     c.width_axis,
                            c.channel_axis, c.row_pitch_bytes, c.groups};
      auto image = take(PlanarImage::create(descriptor, c, facets));
      for (const auto& region : published.value_or(
               std::vector<Region>{Region::whole(descriptor.shape)})) {
        std::vector<std::uint8_t> packed;
        auto fp = take(Footprint::from_regions(descriptor.shape, {region}));
        const auto status = fp.visit(
            [&](const auto& at) {
              const auto offset = take(value.byte_address(at));
              packed.insert(packed.end(), bytes.begin() + offset,
                            bytes.begin() + offset + width);
              return Status::success();
            },
            UINT64_MAX);
        require(status.ok(), "fixture pack");
        require(image.publish(region, packed.data(), packed.size()).ok(),
                "fixture publish");
      }
      binding.image = std::make_shared<const PlanarImage>(std::move(image));
    } else {
      binding.value = value;
    }
    document.inputs.push_back(declaration);
    bindings.inputs.push_back(binding);
    return WorkflowInputReference{id};
  }
  OperationMetadata metadata(unsigned port = 0) const {
    const auto& d = document.inputs[port];
    OperationMetadata m;
    m.descriptor = d.descriptor;
    m.facets = d.facets;
    m.planar_layout = d.planar_layout;
    return m;
  }
};
Result<ExecutionResult> execute(Fixture& f, WorkflowNodeOutput node,
                                const std::optional<Region>& roi = {},
                                const CancellationToken& cancellation = {},
                                std::uint64_t fuel = 1048576) {
  f.document.outputs = {{"result", node.source_node, node.source_port}};
  auto registry = make_default_operation_registry();
  GraphContext graph(f.document);
  PlanningOptions options;
  if (roi) {
    options.output_regions = {{"result", *roi}};
  }
  auto compiled = Compiler(registry).compile(graph, options);
  if (!compiled.ok()) {
    return Result<ExecutionResult>(compiled.status());
  }
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  ExecutionOptions execution;
  execution.dependencies.maximum_work = fuel;
  execution.maximum_dependency_work = fuel * 2;
  return context.execute(compiled.value().plan, f.bindings, cancellation,
                         execution);
}
std::vector<std::uint64_t> observed(const ExecutionResult& result,
                                    const std::optional<Region>& region = {}) {
  const bool image = result.images.count("result");
  const auto& d = image ? result.images.at("result").descriptor()
                        : result.values.at("result").descriptor();
  const auto area = region.value_or(Region::whole(d.shape));
  const auto width = Value::element_size(d.element_type);
  std::vector<std::uint64_t> out;
  std::vector<std::uint8_t> packed;
  if (image) {
    packed.resize(take(area.element_count()) * width);
    require(result.images.at("result")
                .read(area, packed.data(), packed.size())
                .ok(),
            "result planar read");
  }
  auto fp = take(Footprint::from_regions(d.shape, {area}));
  const auto status = fp.visit(
      [&](const auto& at) {
        std::uint64_t sample = 0;
        const auto* data =
            image ? packed.data() + out.size() * width
                  : result.values.at("result").bytes().data() +
                        take(result.values.at("result").byte_address(at));
        std::memcpy(&sample, data, width);
        out.push_back(sample);
        return Status::success();
      },
      UINT64_MAX);
  require(status.ok(), "result traversal");
  return out;
}
TensorDescription description(const ExecutionResult& result) {
  const auto& facets = result.images.count("result")
                           ? result.images.at("result").facets()
                           : result.values.at("result").facets();
  for (const auto& f : facets) {
    if (f.key == "photospider.tensor-description") {
      return take(decode_tensor_description(f));
    }
  }
  throw std::runtime_error("missing tensor description");
}
void basic_association() {
  for (auto type : {ElementType::Float32, ElementType::Float64}) {
    const auto b = [type](double x) { return bits(x, type); };
    for (bool planar : {false, true}) {
      Fixture f;
      std::vector<std::uint64_t> data{b(-2), b(.5),  b(4),  b(.5),
                                      b(2),  b(-0.), b(-4), b(-0.)};
      auto input =
          f.add({type, {1, 2, 4}}, data, rgb(2),
                planar ? std::optional<PlanarImageConfig>(PlanarImageConfig{})
                       : std::nullopt);
      format::AlphaAssociationOptions o;
      o.group = "rgb";
      auto a = take(format::associate_alpha(f.document, input, o));
      auto result = take(execute(f, a));
      require(observed(result) ==
                  std::vector<std::uint64_t>(
                      {b(-1), b(.25), b(2), b(.5), 0, 0, 0, b(-0.)}),
              "semantic associate bits");
      auto d = description(result);
      require(d.association == "premultiplied" &&
                  d.groups[0].interpretation.association == "premultiplied",
              "explicit boundary label");
      auto inverse = take(format::unassociate_alpha(f.document, a, o));
      result = take(execute(f, inverse));
      require(
          observed(result) == std::vector<std::uint64_t>(
                                  {b(-2), b(.5), b(4), b(.5), 0, 0, 0, b(-0.)}),
          "associate/unassociate composition");
      require(description(result).association == "straight",
              "restored straight description");
      format::SetAlphaOptions set;
      set.group = "rgb";
      set.alpha_source.channel.value = "3";
      auto rejected = take(format::set_alpha(f.document, a, set));
      require(!execute(f, rejected).ok(),
              "normal straight consumer rejects boundary");
      auto twice = take(format::associate_alpha(f.document, a, o));
      require(!execute(f, twice).ok(), "already-associated state rejected");
    }
    const std::uint64_t sign = UINT64_C(1)
                               << (type == ElementType::Float32 ? 31 : 63);
    Fixture tiny;
    auto input =
        tiny.add({type, {3, 2}}, {b(1), 1, b(.5), 1, b(-.5), 1}, gray(1));
    format::AlphaAssociationOptions o;
    o.group = "gray";
    auto a = take(format::associate_alpha(tiny.document, input, o));
    require(observed(take(execute(tiny, a))) ==
                std::vector<std::uint64_t>({1, 1, 0, 1, sign, 1}),
            "subnormal half ties and signed underflow");
    Fixture overflow;
    auto d = gray(1);
    d.association = "premultiplied";
    d.groups[0].interpretation.association = "premultiplied";
    const auto maximum = type == ElementType::Float32
                             ? UINT64_C(0x7f7fffff)
                             : UINT64_C(0x7fefffffffffffff);
    auto p = overflow.add({type, {1, 2}}, {maximum, 1}, d);
    auto u = take(format::unassociate_alpha(overflow.document, p, o));
    const auto failed = execute(overflow, u);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::ArithmeticOverflow,
            "division overflow classification");
  }
}
void partial_domains() {
  const auto nan = UINT64_C(0x7f801234);
  Fixture f;
  const Region red({{0, 1}, {0, 2}, {0, 1}}), alpha({{0, 1}, {0, 2}, {3, 1}});
  auto input =
      f.add({ElementType::Float32, {1, 2, 4}},
            {bits(2), nan, nan, bits(.5), bits(4), nan, nan, bits(.25)}, rgb(2),
            PlanarImageConfig{}, std::vector<Region>{red, alpha});
  format::AlphaAssociationOptions o;
  o.group = "rgb";
  auto a = take(format::associate_alpha(f.document, input, o));
  auto r = take(execute(f, a, red));
  require(observed(r, red) == std::vector<std::uint64_t>({bits(1), bits(1)}),
          "R-only ignores missing G/B");
  require(r.diagnostics.source_read_bytes == 16, "R-only exact read union");
  require(!r.images.at("result").acquire(alpha).ok(),
          "R-only does not publish alpha");
  auto b = take(format::unassociate_alpha(f.document, a, o));
  require(observed(take(execute(f, b, red)), red) ==
              std::vector<std::uint64_t>({bits(2), bits(4)}),
          "B explicitly demands A's output alpha");
  Fixture bad;
  auto edge = bad.add({ElementType::Float32, {1, 1, 4}},
                      {bits(2), nan, nan, nan}, rgb(2));
  a = take(format::associate_alpha(bad.document, edge, o));
  const Region only_a({{0, 1}, {0, 1}, {3, 1}}),
      only_r({{0, 1}, {0, 1}, {0, 1}});
  require(observed(take(execute(bad, a, only_a)), only_a) ==
              std::vector<std::uint64_t>{nan},
          "A-only sNaN copies without validation");
  require(!execute(bad, a, only_r).ok(), "color request validates alpha");
  Fixture hidden;
  edge = hidden.add({ElementType::Float32, {1, 2}}, {nan, 0}, gray(1));
  o.group = "gray";
  a = take(format::associate_alpha(hidden.document, edge, o));
  require(!execute(hidden, a).ok(),
          "zero alpha does not suppress color validation");
}
void raw_table() {
  for (auto type : {ElementType::Float32, ElementType::Float64}) {
    const bool narrow = type == ElementType::Float32;
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63),
               quiet = UINT64_C(1) << (narrow ? 22 : 51);
    const auto one = bits(1, type);
    const std::vector<std::uint64_t> x{inf, sign, inf | 17, one,
                                       0,   inf,  one,      one};
    const std::vector<std::uint64_t> a{0, one, inf | 19, inf | 23,
                                       0, inf, 0,        bits(-2, type)};
    for (const auto& algorithm : {"scalar", "simd", "reference"}) {
      Fixture f;
      auto data = x;
      data.insert(data.end(), a.begin(), a.end());
      auto input = f.add({type, {2, x.size()}}, data);
      format::AlphaAssociationOptions o;
      o.metadata_mode = "raw";
      o.input_structure = "channels";
      o.axis = 0;
      o.components = {0};
      o.alpha_source = format::AlphaSource{"internal", {"index", "1"}};
      o.algorithm = algorithm;
      auto product = take(format::associate_alpha(f.document, input, o));
      auto result = execute(f, product);
      if (!result.ok() &&
          result.status().code == ErrorCode::BackendUnavailable) {
        continue;
      }
      auto got = observed(take(std::move(result)));
      const std::vector<std::uint64_t> expected{
          inf | quiet, sign, inf | quiet | 17, inf | quiet | 23, 0,
          inf,         0,    bits(-2, type)};
      require(std::equal(expected.begin(), expected.end(), got.begin()),
              "raw multiply NaN priority/sign table");
      auto quotient = take(format::unassociate_alpha(f.document, input, o));
      got = observed(take(execute(f, quotient)));
      const std::vector<std::uint64_t> divided{
          inf,         sign, inf | quiet | 17, inf | quiet | 23, inf | quiet,
          inf | quiet, inf,  bits(-.5, type)};
      require(std::equal(divided.begin(), divided.end(), got.begin()),
              "raw divide special table");
      require(std::equal(a.begin(), a.end(), got.begin() + x.size()),
              "raw untouched alpha bits");
    }
  }
}
void set_and_views() {
  for (bool planar : {false, true}) {
    Fixture f;
    auto input =
        f.add({ElementType::Float32, {1, 2, 4}},
              {bits(-2), bits(-0.), bits(4), bits(.5), bits(3), bits(2),
               bits(1), bits(0)},
              rgb(2),
              planar ? std::optional<PlanarImageConfig>(PlanarImageConfig{})
                     : std::nullopt);
    format::SetAlphaOptions o;
    o.group = "rgb";
    o.alpha_source.channel.value = "3";
    o.layout = "view";
    auto set = take(format::set_alpha(f.document, input, o));
    auto result = take(execute(f, set));
    require(observed(result)[0] == bits(-2) && observed(result)[1] == bits(-0.),
            "set preserves hidden/signed color");
    if (planar) {
      require(result.images.at("result").owner_token() ==
                  f.bindings.inputs[0].image->owner_token(),
              "validated planar alias");
    } else {
      require(result.values.at("result").bytes().data() ==
                  f.bindings.inputs[0].value.bytes().data(),
              "validated generic alias");
    }
    o.layout = "materialize";
    set = take(format::set_alpha(f.document, input, o));
    result = take(execute(f, set));
    if (planar) {
      require(result.images.at("result").owner_token() !=
                  f.bindings.inputs[0].image->owner_token(),
              "forced planar copy");
    } else {
      require(result.values.at("result").bytes().data() !=
                  f.bindings.inputs[0].value.bytes().data(),
              "forced generic copy");
    }
    auto scalar = f.add({ElementType::Float32, {1}}, {bits(.25)});
    o.alpha_source.kind = "scalar";
    o.placement = "channel";
    o.channel_index = 1;
    set = take(format::set_alpha(f.document, input, o, scalar));
    result = take(execute(f, set));
    require(observed(result) == std::vector<std::uint64_t>(
                                    {bits(-2), bits(.25), bits(-0.), bits(4),
                                     bits(3), bits(.25), bits(2), bits(1)}),
            "private removal then final insertion uses original coordinates");
    auto d = description(result);
    require(d.groups[0].indices == std::vector<std::uint64_t>({0, 2, 3}) &&
                d.groups[0].alpha == 1,
            "set remaps group");
  }
  Fixture bad;
  auto input = bad.add({ElementType::Float32, {1, 1, 4}},
                       {bits(1), bits(2), bits(3), bits(2)}, rgb(2));
  format::SetAlphaOptions o;
  o.group = "rgb";
  o.alpha_source.channel.value = "3";
  o.layout = "view";
  auto set = take(format::set_alpha(bad.document, input, o));
  require(!execute(bad, set).ok(), "view cannot bypass alpha range validation");
  Fixture shared;
  auto d = rgb(2, 5);
  d.channels[4] = {"Y", "gray", "relative"};
  TensorColorGroup other;
  other.name = "gray";
  other.indices = {4};
  other.components = {d.channels[4]};
  other.interpretation.model = "gray";
  other.interpretation.association = "straight";
  other.alpha = 3;
  d.groups.push_back(other);
  input = shared.add({ElementType::Float32, {1, 1, 5}},
                     {bits(1), bits(2), bits(3), bits(.5), bits(4)}, d);
  o.layout = "auto";
  set = take(format::set_alpha(shared.document, input, o));
  require(!execute(shared, set).ok(), "preserve rejects shared alpha");
  shared.document.nodes.pop_back();
  o.placement = "channel";
  o.channel_index = 2;
  set = take(format::set_alpha(shared.document, input, o));
  auto result = take(execute(shared, set));
  require(observed(result) ==
              std::vector<std::uint64_t>(
                  {bits(1), bits(2), bits(.5), bits(3), bits(.5), bits(4)}),
          "shared old alpha retained");
  d = description(result);
  require(d.groups[0].alpha == 2 && d.groups[1].alpha == 4,
          "shared alpha relation remap");
}
void extraction_removal() {
  for (auto type : {ElementType::UInt8, ElementType::UInt16, ElementType::Int8,
                    ElementType::Int16, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    auto d = gray(1);
    if (type != ElementType::Float32 && type != ElementType::Float64) {
      d.encoding = TensorEncoding{};
      d.groups[0].components[0].encoding = TensorEncoding{};
    }
    const auto width = Value::element_size(type);
    const std::uint64_t mask =
        width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
    const auto sentinel =
        (type == ElementType::Float32   ? UINT64_C(0xff812345)
         : type == ElementType::Float64 ? UINT64_C(0xfff0123456789abc)
                                        : UINT64_C(0xfedcba9876543210)) &
        mask;
    Fixture f;
    auto input = f.add({type, {2, 2}}, {17, sentinel, 31, mask}, d);
    format::ExtractAlphaOptions e;
    e.group = "gray";
    auto node = take(format::extract_alpha(f.document, input, f.metadata(), e));
    require(
        f.document.nodes.back().operation.find("channel.extract_index_") == 0,
        "B is a channel extraction lowering");
    auto result = take(execute(f, node));
    require(observed(result) == std::vector<std::uint64_t>({sentinel, mask}),
            "B all-dtype exact bit copy");
    require(description(result).component &&
                description(result).groups.empty() &&
                description(result).model.empty(),
            "standalone alpha metadata");
    format::RemoveAlphaOptions r;
    r.group = "gray";
    node = take(format::remove_alpha(f.document, input, f.metadata(), r));
    result = take(execute(f, node));
    require(observed(result) == std::vector<std::uint64_t>({17, 31}),
            "C keeps exact colors");
    auto remaining = description(result);
    require(remaining.channel_axis == 1 && !remaining.groups[0].alpha,
            "C keeps singleton axis and detaches alpha");
  }
}
void opaque_and_gray_insertion() {
  const std::vector<std::pair<ElementType, std::uint64_t>> defaults{
      {ElementType::UInt8, 255},
      {ElementType::UInt16, 65535},
      {ElementType::Int8, 127},
      {ElementType::Int16, 32767},
      {ElementType::Int64, UINT64_C(0x7fffffffffffffff)},
      {ElementType::Float32, UINT64_C(0x3f800000)},
      {ElementType::Float64, UINT64_C(0x3ff0000000000000)}};
  for (const auto& entry : defaults) {
    Fixture f;
    auto d = component_gray();
    if (entry.first != ElementType::Float32 &&
        entry.first != ElementType::Float64) {
      d.encoding = TensorEncoding{};
    }
    auto input =
        f.add({entry.first, {2, 3}}, std::vector<std::uint64_t>(6, 0), d);
    format::ExtractAlphaOptions o;
    o.group = "Y";
    o.missing_alpha = "opaque";
    for (bool keep : {false, true}) {
      o.keepdims = keep;
      auto node =
          take(format::extract_alpha(f.document, input, f.metadata(), o));
      auto result = take(execute(f, node));
      require(observed(result) == std::vector<std::uint64_t>(6, entry.second),
              "opaque exact dtype maximum/one");
      require(result.values.at("result").descriptor().shape ==
                  std::vector<std::uint64_t>({2, 3}),
              "component keepdims invents no axis");
    }
    o.layout = "view";
    auto node = take(format::extract_alpha(f.document, input, f.metadata(), o));
    require(!execute(f, node).ok(), "opaque forced view fails at observation");
  }
  Fixture f;
  auto d = component_gray();
  d.encoding = TensorEncoding{};
  auto input = f.add({ElementType::UInt16, {1}}, {0}, d);
  format::ExtractAlphaOptions e;
  e.group = "Y";
  e.missing_alpha = "opaque";
  TensorEncoding encoding;
  encoding.stored = {std::int64_t{0}, std::int64_t{1023}};
  e.alpha_encoding = encoding;
  auto node = take(format::extract_alpha(f.document, input, f.metadata(), e));
  require(observed(take(execute(f, node))) == std::vector<std::uint64_t>{1023},
          "10-bit opaque code");
  encoding.decoded = {std::int64_t{1}, std::int64_t{0}};
  e.alpha_encoding = encoding;
  node = take(format::extract_alpha(f.document, input, f.metadata(), e));
  require(observed(take(execute(f, node))) == std::vector<std::uint64_t>{0},
          "descending opaque encoding");
  encoding.stored = {std::int64_t{0}, std::int64_t{1}};
  encoding.decoded = {std::int64_t{0}, std::int64_t{2}};
  e.alpha_encoding = encoding;
  const auto before = f.document.nodes.size();
  require(!format::extract_alpha(f.document, input, f.metadata(), e).ok() &&
              f.document.nodes.size() == before,
          "fractional integer opaque rejected transactionally");
  Fixture image;
  PlanarImageConfig c;
  c.channel_axis.reset();
  input = image.add({ElementType::Float32, {2, 3}},
                    std::vector<std::uint64_t>(6, bits(2)), component_gray(), c,
                    std::vector<Region>{});
  e.alpha_encoding.reset();
  e.group = "Y";
  node =
      take(format::extract_alpha(image.document, input, image.metadata(), e));
  auto result = take(execute(image, node));
  require(observed(result) == std::vector<std::uint64_t>(6, bits(1)) &&
              result.diagnostics.source_read_bytes == 0,
          "opaque planar source is descriptor-only");
  // A separate fully published fixture: Gray insertion requires its source
  // colors.
  Fixture insertion;
  auto gray = component_gray();
  gray.reference = "scene";
  gray.white = std::array<double, 2>{.3127, .3290};
  gray.coordinates = TensorModelCoordinates{};
  gray.coordinates->scale = "relative";
  gray.coordinates->gray_kind = "linear_y";
  gray.coordinates->observer = "cie1931_2deg";
  input = insertion.add({ElementType::Float32, {2, 3}},
                        std::vector<std::uint64_t>(6, bits(2)), gray, c);
  auto weight = insertion.add({ElementType::Float32, {1}}, {bits(.25)});
  format::SetAlphaOptions set;
  set.group = "Y";
  set.alpha_source.kind = "scalar";
  set.placement = "channel";
  set.channel_index = 1;
  set.output_axis = 2;
  node = take(format::set_alpha(insertion.document, input, set, weight));
  result = take(execute(insertion, node));
  require(result.images.at("result").descriptor().shape ==
              std::vector<std::uint64_t>({2, 3, 2}),
          "component Gray axis insertion");
  require(description(result).groups[0].alpha == 1,
          "inserted Gray internal alpha");
  require(description(result).groups[0].interpretation.coordinates ==
              gray.coordinates,
          "Gray alpha insertion lost model coordinates");
  format::ModelConversionOptions expand;
  expand.group = "Y";
  auto converted =
      take(format::gray_to_color(insertion.document, node, expand));
  auto color = take(execute(insertion, converted));
  require(color.images.at("result").descriptor().shape ==
              std::vector<std::uint64_t>({2, 3, 4}),
          "Gray alpha insertion cannot feed native model conversion");
  require(description(color).groups[0].interpretation.model == "xyz",
          "Gray conversion published the wrong native model");
}
void independent_golden() {
  for (auto type : {ElementType::Float32, ElementType::Float64}) {
    const auto& cases = type == ElementType::Float32 ? alpha_golden::binary32
                                                     : alpha_golden::binary64;
    std::vector<std::uint64_t> data;
    for (const auto& c : cases) {
      data.push_back(c.x);
      data.push_back(c.alpha);
    }
    for (const auto& algorithm :
         {"auto", "scalar", "simd", "reference", "named"}) {
      for (bool planar : {false, true}) {
        Fixture f;
        auto input =
            f.add({type, {1, cases.size(), 2}}, data, {},
                  planar ? std::optional<PlanarImageConfig>{PlanarImageConfig{}}
                         : std::nullopt);
        format::AlphaAssociationOptions o;
        o.metadata_mode = "raw";
        o.axis = 2;
        o.input_structure = "channels";
        o.components = {0};
        o.alpha_source = format::AlphaSource{"internal", {"index", "1"}};
        o.algorithm = std::string(algorithm) == "named" ? "auto" : algorithm;
        if (std::string(algorithm) == "named") {
#if defined(__aarch64__)
          o.profile = "accelerated_apple_silicon";
#else
          o.profile = "accelerated_x86_64";
#endif
        }
        for (bool divide : {false, true}) {
          auto operation =
              take(divide ? format::unassociate_alpha(f.document, input, o)
                          : format::associate_alpha(f.document, input, o));
          auto run = execute(f, operation, {}, {}, UINT64_C(1) << 28);
          if (!run.ok() && run.status().code == ErrorCode::BackendUnavailable) {
            f.document.nodes.pop_back();
            continue;
          }
          auto got = observed(take(std::move(run)));
          for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto expected = divide ? cases[i].divide : cases[i].multiply;
            require(got[2 * i] == expected,
                    "independent golden " + std::string(algorithm) +
                        (divide ? " divide" : " multiply") + " case " +
                        std::to_string(i) + " dtype " +
                        std::to_string(Value::element_size(type)) +
                        " expected " + std::to_string(expected) + " got " +
                        std::to_string(got[2 * i]));
            require(got[2 * i + 1] == cases[i].alpha,
                    "golden passthrough alpha");
          }
        }
      }
    }
  }
}
void metadata_conflicts() {
  Fixture f;
  auto d = gray(1);
  TensorEncoding encoded;
  encoded.stored = {std::int64_t{0}, std::int64_t{255}};
  d.channels[1].encoding = encoded;
  auto input = f.add({ElementType::Float32, {1, 2}}, {bits(.5), bits(.5)}, d);
  format::SetAlphaOptions set;
  set.group = "gray";
  set.alpha_source.channel.value = "1";
  auto node = take(format::set_alpha(f.document, input, set));
  require(!execute(f, node).ok(), "set requires normalized alpha encoding");
  f.document.nodes.clear();
  format::AlphaAssociationOptions association;
  association.group = "gray";
  node = take(format::associate_alpha(f.document, input, association));
  require(!execute(f, node).ok(),
          "association requires normalized alpha encoding");
  Fixture identity;
  d.channels[1].encoding->stored = {std::int64_t{-1}, std::int64_t{2}};
  d.channels[1].encoding->decoded = {-1.0, 2.0};
  input = identity.add({ElementType::Float32, {1, 2}}, {bits(.5), bits(.5)}, d);
  node = take(format::set_alpha(identity.document, input, set));
  require(execute(identity, node).ok(),
          "mathematically identity encoding accepted");
  Fixture grid;
  d = gray(1);
  d.channels[0].sampling = TensorSampling{"color-grid"};
  d.groups[0].components[0].sampling = d.channels[0].sampling;
  d.channels[1].sampling = TensorSampling{"different-grid"};
  input = grid.add({ElementType::Float32, {1, 2}}, {bits(.5), bits(.5)}, d);
  node = take(format::set_alpha(grid.document, input, set));
  require(!execute(grid, node).ok(),
          "alpha and selected color must be co-sited");
}
void ranks_tiles_and_views() {
  // Channel-first/last/middle, rank one through eight, with exact small values.
  for (unsigned rank : {1U, 2U, 3U, 8U}) {
    for (unsigned axis = 0; axis < rank; ++axis) {
      Fixture f;
      std::vector<std::uint64_t> shape(rank, 1);
      shape[axis] = 2;
      if (rank > 1) {
        shape[axis ? 0 : 1] = 3;
      }
      std::vector<std::uint64_t> data, expected;
      auto fp = take(Footprint::all(shape));
      require(fp.visit(
                    [&](const auto& at) {
                      data.push_back(bits(at[axis] ? .5 : 2));
                      expected.push_back(bits(at[axis] ? .5 : 1));
                      return Status::success();
                    },
                    UINT64_MAX)
                  .ok(),
              "rank fixture");
      auto input = f.add({ElementType::Float32, shape}, data, gray(axis));
      format::AlphaAssociationOptions a;
      a.group = "gray";
      auto op = take(format::associate_alpha(f.document, input, a));
      require(observed(take(execute(f, op))) == expected,
              "arbitrary channel axis/rank");
      format::ExtractAlphaOptions e;
      e.group = "gray";
      if (rank == 1) {
        require(!format::extract_alpha(f.document, input, f.metadata(), e).ok(),
                "no rank-zero alpha result");
        e.keepdims = true;
      }
      op = take(format::extract_alpha(f.document, input, f.metadata(), e));
      require(observed(take(execute(f, op))) ==
                  std::vector<std::uint64_t>(rank == 1 ? 1 : 3, bits(.5)),
              "alpha shape squeeze");
    }
  }
  for (auto order : {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
    Fixture f;
    constexpr std::uint64_t height = 129, width = 131;
    PlanarImageConfig config;
    config.order = order;
    if (order == ImagePlaneOrder::Continuous) {
      config.row_pitch_bytes = 576;
    }
    std::vector<std::uint64_t> data(height * width * 4);
    for (std::size_t i = 0; i < data.size(); i += 4) {
      data[i] = bits(2);
      data[i + 1] = data[i + 2] = 0x7f801234;
      data[i + 3] = bits(.5);
    }
    const Region red({{127, 2}, {127, 4}, {0, 1}}),
        alpha({{127, 2}, {127, 4}, {3, 1}});
    auto input = f.add({ElementType::Float32, {height, width, 4}}, data, rgb(2),
                       config, std::vector<Region>{red, alpha});
    format::AlphaAssociationOptions a;
    a.group = "rgb";
    a.algorithm = "simd";
    auto op = take(format::associate_alpha(f.document, input, a));
    auto run = execute(f, op, red);
    if (!run.ok() && run.status().code == ErrorCode::BackendUnavailable) {
      f.document.nodes.clear();
      a.algorithm = "scalar";
      op = take(format::associate_alpha(f.document, input, a));
      run = execute(f, op, red);
    }
    auto result = take(std::move(run));
    require(observed(result, red) == std::vector<std::uint64_t>(8, bits(1)),
            "cross-tile/row pitch ROI");
    require(result.diagnostics.source_read_bytes == 64,
            "tile edges preserve exact source support");
    format::SetAlphaOptions identity_set;
    identity_set.group = "rgb";
    identity_set.alpha_source.channel.value = "3";
    identity_set.layout = "view";
    auto viewed = take(format::set_alpha(f.document, input, identity_set));
    auto identity_result = take(execute(f, viewed, red));
    require(identity_result.images.at("result").owner_token() ==
                f.bindings.inputs[0].image->owner_token(),
            "R-only planar set returns validated identity view");
    require(!identity_result.images.at("result").acquire(alpha).ok(),
            "view does not publish validation-only alpha");
    format::SetAlphaOptions set;
    set.group = "rgb";
    set.alpha_source.kind = "external_plane";
    auto alpha_config = config;
    alpha_config.channel_axis.reset();
    const Region weight_roi({{127, 2}, {127, 4}});
    auto weight = f.add({ElementType::Float32, {height, width}},
                        std::vector<std::uint64_t>(height * width, bits(.25)),
                        {}, alpha_config, std::vector<Region>{weight_roi});
    op = take(format::set_alpha(f.document, input, set, weight));
    result = take(execute(f, op, red));
    require(observed(result, red) == std::vector<std::uint64_t>(8, bits(2)),
            "external planar alpha validates without multiplying");
    require(result.diagnostics.source_read_bytes == 64,
            "set R-only does not fetch old alpha");
  }
  Fixture reversed;
  auto input = reversed.add({ElementType::Float32, {2, 2}},
                            {bits(2), bits(.5), bits(4), bits(.25)}, gray(1));
  format::SetAlphaOptions set;
  set.group = "gray";
  set.alpha_source.channel.value = "1";
  set.placement = "channel";
  set.channel_index = 0;
  set.layout = "view";
  auto op = take(format::set_alpha(reversed.document, input, set));
  auto result = take(execute(reversed, op));
  require(observed(result) == std::vector<std::uint64_t>(
                                  {bits(.5), bits(2), bits(.25), bits(4)}),
          "negative channel stride set view");
  require(result.values.at("result").storage().get() ==
              reversed.bindings.inputs[0].value.storage().get(),
          "reverse view retains owner");
  require(result.values.at("result").layout().byte_strides[1] == -4,
          "reverse view affine stride");
  // An extracted alpha may alias the primary generic storage, with a different
  // rank. Complete physical-address proof permits it without weakening checks.
  Fixture common;
  input = common.add({ElementType::Float32, {2, 2}},
                     {bits(2), bits(.5), bits(4), bits(.25)}, gray(1));
  format::ExtractAlphaOptions e;
  e.group = "gray";
  e.layout = "view";
  auto extracted =
      take(format::extract_alpha(common.document, input, common.metadata(), e));
  set = {};
  set.group = "gray";
  set.alpha_source.kind = "external_plane";
  set.layout = "view";
  op = take(format::set_alpha(common.document, input, set, extracted));
  result = take(execute(common, op));
  require(result.values.at("result").storage().get() ==
              common.bindings.inputs[0].value.storage().get(),
          "same-owner external alpha view");
  Fixture identity;
  input = identity.add(
      {ElementType::Float64, {2, 3}},
      std::vector<std::uint64_t>(6, bits(-0., ElementType::Float64)),
      component_gray());
  format::RemoveAlphaOptions remove;
  remove.group = "Y";
  remove.missing_alpha = "identity";
  for (const auto& layout : {"view", "materialize"}) {
    remove.layout = layout;
    op = take(format::remove_alpha(identity.document, input,
                                   identity.metadata(), remove));
    result = take(execute(identity, op));
    require(observed(result) ==
                std::vector<std::uint64_t>(6, bits(-0., ElementType::Float64)),
            "component missing identity bits");
    require((result.values.at("result").storage().get() ==
             identity.bindings.inputs[0].value.storage().get()) ==
                (std::string(layout) == "view"),
            "identity respects forced layout");
  }
  Fixture exhausted;
  input = exhausted.add({ElementType::Float32, {1, 2}}, {bits(2), bits(.5)},
                        gray(1));
  format::AlphaAssociationOptions a;
  a.group = "gray";
  a.algorithm = "reference";
  op = take(format::associate_alpha(exhausted.document, input, a));
  require(execute(exhausted, op, {}, {}, 16).status().code ==
              ErrorCode::ResourceExhausted,
          "bounded work cannot be bypassed");
}
void assertions_and_environment() {
  Fixture f;
  auto input = f.add({ElementType::Float32, {1, 4}},
                     {bits(1), bits(2), bits(3), bits(.5)}, rgb(1));
  format::ExtractAlphaOptions e;
  e.group = "rgb";
  auto wrong = f.metadata();
  wrong.descriptor.shape = {2, 4};
  require(!format::extract_alpha(f.document, input, wrong, e).ok() &&
              f.document.nodes.empty(),
          "declared-source assertion rollback");
  const WorkflowNodeOutput future{100, "values"};
  auto helper =
      take(format::extract_alpha(f.document, future, f.metadata(), e));
  auto changed = rgb(1);
  changed.channels[3].name = "changed";
  format::MetadataOptions m;
  m.mode = "replace";
  m.description = changed;
  auto producer = take(format::assign_metadata(f.document, input, m));
  f.document.nodes.back().id = future.source_node;
  (void)producer;
  require(!execute(f, helper).ok(),
          "forward-source stale metadata is not trusted");
  Fixture env;
  input = env.add({ElementType::Float32, {1, 2}}, {bits(1), bits(.5)}, gray(1));
  format::AlphaAssociationOptions a;
  a.group = "gray";
  auto node = take(format::associate_alpha(env.document, input, a));
  const auto old_round = std::fegetround();
  std::fesetround(FE_UPWARD);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::feraiseexcept(FE_DIVBYZERO);
  auto run = execute(env, node);
  const auto after_round = std::fegetround(),
             after_flags = std::fetestexcept(FE_DIVBYZERO);
  std::fesetround(old_round);
  std::feclearexcept(FE_ALL_EXCEPT);
  require(run.ok() && after_round == FE_UPWARD && after_flags != 0,
          "caller fenv survives execution");
  CancellationSource cancellation;
  cancellation.cancel();
  require(execute(env, node, {}, cancellation.token()).status().code ==
              ErrorCode::Cancelled,
          "pre-cancelled execution");
}
void review_contract_regressions() {
  // Preserve BOTH affine encoding and its represented interval, including
  // inheritance. FMT-06 source_range is an observable downstream consumer.
  for (bool inherited : {false, true}) {
    for (bool moved : {false, true}) {
      Fixture f;
      auto d = gray(1);
      TensorEncoding encoding;
      encoding.stored = {std::int64_t{-1}, std::int64_t{2}};
      encoding.decoded = {-1.0, 2.0};
      if (inherited) {
        d.encoding = encoding;
        d.channels[0].encoding = TensorEncoding{};
        d.groups[0].components[0].encoding = TensorEncoding{};
      } else {
        d.channels[1].encoding = encoding;
      }
      auto input =
          f.add({ElementType::Float32, {1, 2}}, {bits(.5), bits(.5)}, d);
      format::SetAlphaOptions options;
      options.group = "gray";
      options.alpha_source.channel.value = "1";
      if (moved) {
        options.placement = "channel";
        options.channel_index = 0;
      }
      const auto node = take(format::set_alpha(f.document, input, options));
      const auto out = description(take(execute(f, node)));
      const auto alpha = moved ? 0U : 1U;
      require(out.channels[alpha].encoding == d.channels[1].encoding,
              "SetAlpha must preserve local encoding absence/endpoints");
      require(out.encoding == d.encoding,
              "SetAlpha must preserve inherited encoding");
      Fixture conversion;
      const auto source = conversion.add({ElementType::Float32, {1, 2}},
                                         {bits(.5), bits(.5)}, out);
      conversion.document.nodes = {
          {1,
           "numeric.convert_format_strict",
           {source},
           {{"dtype", std::string("float32")},
            {"axis", std::int64_t{1}},
            {"source_range",
             std::string(moved ? "i:-1,i:2;i:0,i:1" : "i:0,i:1;i:-1,i:2")}}}};
      require(execute(conversion, {1, "values"}).ok(),
              "preserved alpha interval must remain acceptable to FMT-06");
    }
  }
  // A new alpha must override a non-identity tensor-wide color encoding.
  {
    Fixture f;
    auto d = gray(1, false);
    d.encoding = TensorEncoding{};
    d.encoding->stored = {std::int64_t{0}, std::int64_t{255}};
    const auto input = f.add({ElementType::Float32, {1, 1}}, {bits(.5)}, d);
    const auto weight = f.add({ElementType::Float32, {1}}, {bits(.5)});
    format::SetAlphaOptions o;
    o.group = "gray";
    o.placement = "channel";
    o.channel_index = 1;
    o.alpha_source.kind = "scalar";
    const auto node = take(format::set_alpha(f.document, input, o, weight));
    const auto out = description(take(execute(f, node)));
    require(out.channels[1].encoding ==
                std::optional<TensorEncoding>{TensorEncoding{}},
            "new alpha must have normalized coverage encoding");
  }
  // Retaining a shared alpha creates a new slot, not a move of the old slot.
  for (bool inherited : {false, true}) {
    Fixture f;
    auto d = gray(1);
    TensorEncoding old_encoding;
    old_encoding.stored = {std::int64_t{-1}, std::int64_t{2}};
    old_encoding.decoded = {-1.0, 2.0};
    if (inherited) {
      d.encoding = old_encoding;
    } else {
      d.channels[1].encoding = old_encoding;
    }
    d.channels[1].name = "shared alpha";
    auto other = d.groups[0];
    other.name = "other";
    d.groups.push_back(other);
    const auto input =
        f.add({ElementType::Float32, {1, 2}}, {bits(.5), bits(.5)}, d);
    const auto weight = f.add({ElementType::Float32, {1}}, {bits(.25)});
    format::SetAlphaOptions o;
    o.group = "gray";
    o.placement = "channel";
    o.channel_index = 2;
    o.alpha_source.kind = "scalar";
    const auto node = take(format::set_alpha(f.document, input, o, weight));
    const auto out = description(take(execute(f, node)));
    require(out.channels[1].encoding == d.channels[1].encoding &&
                out.channels[1].name == d.channels[1].name &&
                out.groups[1].alpha == 1,
            "shared old alpha meaning must survive insertion");
    require(out.channels[2].encoding ==
                    std::optional<TensorEncoding>{TensorEncoding{}} &&
                out.channels[2].name.empty() && out.groups[0].alpha == 2,
            "new shared-case alpha must not inherit old alpha meaning");
  }
  // Static representation errors are public TypeMismatch/None branches, not
  // sample-domain failures and never contain an invented pixel coordinate.
  for (unsigned kind = 0; kind < 6; ++kind) {
    Fixture f;
    auto d = kind == 0   ? gray(1, false)
             : kind == 1 ? component_gray()
                         : gray(1);
    std::vector<std::uint64_t> shape = kind == 0 || kind == 1
                                           ? std::vector<std::uint64_t>{1, 1}
                                           : std::vector<std::uint64_t>{1, 2};
    if (kind == 2) {
      d.association = "premultiplied";
      d.groups[0].interpretation.association = "premultiplied";
    }
    if (kind == 5) {
      d.groups[0].interpretation.model = "black_white";
    }
    const auto input = f.add({ElementType::Float32, shape},
                             std::vector<std::uint64_t>(shape[1], bits(.5)), d);
    format::AlphaAssociationOptions o;
    o.group = kind == 1 ? "Y" : "gray";
    std::optional<WorkflowInput> weight;
    if (kind == 4) {
      o.alpha_source = format::AlphaSource{"scalar", {"index", "1"}};
      weight = f.add({ElementType::Float32, {1}}, {bits(.5)});
    }
    auto node =
        take(kind == 3 ? format::unassociate_alpha(f.document, input, o)
                       : format::associate_alpha(f.document, input, o, weight));
    const auto run = execute(f, node);
    require(!run.ok() && run.status().code == ErrorCode::TypeMismatch &&
                run.status().reason == FailureReason::None,
            "wrong association/missing alpha/component/external semantic alpha "
            "classification kind=" +
                std::to_string(kind));
    require(run.status().message.find(" at [") == std::string::npos,
            "static errors must not invent a sample coordinate");
  }
  for (bool planar : {false, true}) {
    for (const auto& source_kind : {"internal", "external_plane", "scalar"}) {
      Fixture f;
      const auto input =
          f.add({ElementType::Float32, {1, 2, 2}},
                {bits(.5), bits(.5), bits(.5), bits(2)}, gray(2),
                planar ? std::optional<PlanarImageConfig>{PlanarImageConfig{}}
                       : std::nullopt);
      format::SetAlphaOptions o;
      o.group = "gray";
      o.alpha_source.kind = source_kind;
      o.alpha_source.channel.value = "1";
      std::optional<WorkflowInput> weight;
      if (o.alpha_source.kind == "scalar") {
        weight = f.add({ElementType::Float32, {1}}, {bits(2)});
      } else if (o.alpha_source.kind == "external_plane") {
        weight = f.add({ElementType::Float32, {1, 2}}, {bits(.5), bits(2)});
      }
      const auto node = take(format::set_alpha(f.document, input, o, weight));
      const auto run = execute(f, node, Region({{0, 1}, {1, 1}, {0, 1}}));
      require(!run.ok() && run.status().reason == FailureReason::InvalidDomain,
              "invalid supplied alpha fails");
      const auto& message = run.status().message;
      require(
          message.find("group=gray") != std::string::npos &&
              message.find("at [0,1,0]") != std::string::npos &&
              message.find("alpha_source=" + o.alpha_source.kind) !=
                  std::string::npos,
          "diagnostic must identify group, output coordinate and alpha source");
      const auto suffix = o.alpha_source.kind == "internal"
                              ? "alpha_port=0 alpha_channel=1 alpha_at=[0,1,1]"
                          : o.alpha_source.kind == "scalar"
                              ? "alpha_port=1 alpha_at=[0]"
                              : "alpha_port=1 alpha_at=[0,1]";
      require(message.find(suffix) != std::string::npos,
              "diagnostic must report source coordinate in its own rank");
    }
  }
}
void managed_alpha_budget_regressions() {
  for (const auto& storage : {"generic", "continuous", "tiled"}) {
    for (unsigned size : {17U, 256U}) {
      for (const auto& algorithm : {"scalar", "simd", "reference"}) {
        if (size == 256 && std::string(algorithm) == "reference") {
          continue;
        }
        Fixture f;
        std::optional<PlanarImageConfig> layout;
        if (std::string(storage) != "generic") {
          layout.emplace();
          layout->order = std::string(storage) == "tiled"
                              ? ImagePlaneOrder::Tiled
                              : ImagePlaneOrder::Continuous;
        }
        const auto input =
            f.add({ElementType::Float32, {size, size, 2}},
                  std::vector<std::uint64_t>(size * size * 2, bits(.5)),
                  gray(2), layout);
        format::AlphaAssociationOptions o;
        o.group = "gray";
        o.algorithm = algorithm;
        const auto node = take(format::associate_alpha(f.document, input, o));
        f.document.outputs = {{"result", node.source_node, node.source_port}};
        auto registry = make_default_operation_registry();
        GraphContext graph(f.document);
        auto compiled = Compiler(registry).compile(graph);
        if (!compiled.ok() &&
            compiled.status().code == ErrorCode::BackendUnavailable) {
          continue;
        }
        require(compiled.ok(), "budget fixture compile");
        for (unsigned workers : {1U, 2U}) {
          ExecutionContextConfig c;
          c.cpu_workers = workers;
          c.managed_resources = ResourceLimits{};
          c.managed_resources->maximum_work = 10000;
          ExecutionContext context(registry, c);
          ExecutionOptions e;
          e.dependencies.maximum_work = UINT64_C(1) << 42;
          e.maximum_dependency_work = UINT64_C(1) << 42;
          e.maximum_dependency_cache_work = 0;
          const auto run =
              context.execute(compiled.value().plan, f.bindings, {}, e);
          require(
              !run.ok() && run.status().code == ErrorCode::ResourceExhausted,
              "managed work must fail for " + std::string(storage) + "/" +
                  algorithm);
          const auto issued =
              take(context.resource_budget()).statistics().issued.work;
          require(issued <= 10000, "work must be precharged without overshoot");
        }
      }
    }
  }
  // Success must charge actual sample work on the callback thread too. A
  // validate-only identity view cannot bypass this work admission either.
  for (bool viewed : {false, true}) {
    for (const auto& algorithm : {"scalar", "reference"}) {
      Fixture f;
      const auto input =
          f.add({ElementType::Float32, {17, 17, 2}},
                std::vector<std::uint64_t>(17 * 17 * 2, bits(.5)), gray(2),
                PlanarImageConfig{});
      WorkflowNodeOutput node;
      if (viewed) {
        format::SetAlphaOptions o;
        o.group = "gray";
        o.alpha_source.channel.value = "1";
        o.layout = "view";
        node = take(format::set_alpha(f.document, input, o));
      } else {
        format::AlphaAssociationOptions o;
        o.group = "gray";
        o.algorithm = algorithm;
        node = take(format::associate_alpha(f.document, input, o));
      }
      f.document.outputs = {{"result", node.source_node, node.source_port}};
      auto registry = make_default_operation_registry();
      GraphContext graph(f.document);
      auto compiled = take(Compiler(registry).compile(graph));
      for (std::uint64_t limit : {UINT64_C(10000), UINT64_C(1) << 42}) {
        ExecutionContextConfig c;
        c.cpu_workers = 1;
        c.managed_resources = ResourceLimits{};
        c.managed_resources->maximum_work = limit;
        ExecutionContext context(registry, c);
        auto run = context.execute(compiled.plan, f.bindings);
        const auto work =
            take(context.resource_budget()).statistics().issued.work;
        if (limit == 10000) {
          require(
              !run.ok() && run.status().code == ErrorCode::ResourceExhausted,
              "view work cannot bypass budget");
        } else {
          require(run.ok(), "sufficient managed work must succeed");
          require(work >= 17 * 17 * 2 * 64,
                  "successful planar callback sample work is charged");
          if (viewed) {
            require(run.value().images.at("result").owner_token() ==
                        f.bindings.inputs[0].image->owner_token(),
                    "managed view retains its authorized owner");
          }
        }
      }
    }
  }
}
}  // namespace
int main() {
  try {
    for (const auto& test :
         {std::make_pair("association", basic_association),
          std::make_pair("partial", partial_domains),
          std::make_pair("raw", raw_table),
          std::make_pair("set", set_and_views),
          std::make_pair("copy", extraction_removal),
          std::make_pair("opaque", opaque_and_gray_insertion),
          std::make_pair("assertions", assertions_and_environment),
          std::make_pair("golden", independent_golden),
          std::make_pair("metadata", metadata_conflicts),
          std::make_pair("layout", ranks_tiles_and_views),
          std::make_pair("review", review_contract_regressions),
          std::make_pair("managed_budget", managed_alpha_budget_regressions)}) {
      std::cout << "[alpha] " << test.first << std::endl;
      test.second();
    }
    std::cout << "FMT-04/FMT-05 integration checks passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "alpha test: " << e.what() << '\n';
    return 1;
  }
}
