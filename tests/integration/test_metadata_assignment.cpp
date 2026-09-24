#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool value, const std::string& message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
template <class T>
T take(Result<T> value) {
  if (!value.ok()) {
    throw std::runtime_error(value.status().message);
  }
  return value.take_value();
}
void take(Status value) {
  if (!value.ok()) {
    throw std::runtime_error(value.message);
  }
}
StridedLayout dense(const ValueDescriptor& d) {
  StridedLayout l;
  l.byte_strides.resize(d.shape.size());
  std::uint64_t stride = Value::element_size(d.element_type);
  for (std::size_t a = d.shape.size(); a-- > 0;) {
    l.byte_strides[a] = stride;
    stride *= d.shape[a];
  }
  return l;
}
TensorDescription rgb() {
  TensorDescription d;
  d.channel_axis = 2;
  d.channels = {{"R", "red", "relative"},
                {"G", "green", "relative"},
                {"B", "blue", "relative"},
                {"A", "coverage", "ratio"}};
  TensorColorGroup g;
  g.name = "color";
  g.indices = {0, 1, 2};
  g.components = {d.channels[0], d.channels[1], d.channels[2]};
  g.alpha = 3;
  g.interpretation.model = "rgb";
  g.interpretation.primaries = "srgb";
  g.interpretation.transfer = "linear";
  g.interpretation.association = "straight";
  d.groups = {g};
  return d;
}
TensorDescription decoded(const std::vector<ValueFacet>& facets) {
  for (const auto& f : facets) {
    if (f.key == "photospider.tensor-description") {
      return take(decode_tensor_description(f));
    }
  }
  return {};
}
struct Fixture final {
  WorkflowDocument document;
  ExecutionBindings bindings;
  ResourceBindings resources;
  std::shared_ptr<OperationRegistry> registry =
      make_default_operation_registry();
  std::unique_ptr<GraphContext> graph;
  std::vector<std::uint8_t> raw;
  ValueDescriptor descriptor;
  std::vector<ValueFacet> original;
  WorkflowInput input = WorkflowInputReference{1};
  explicit Fixture(ValueDescriptor d, std::vector<ValueFacet> facets = {},
                   bool image = false, bool tiled = false,
                   std::optional<Region> published = {})
      : descriptor(std::move(d)), original(std::move(facets)) {
    auto count = take(Region::whole(descriptor.shape).element_count());
    raw.resize(count * Value::element_size(descriptor.element_type));
    for (std::size_t i = 0; i < raw.size(); ++i) {
      raw[i] = (i * 73 + 29) & 255;
    }
    // Explicit signaling/quiet NaN payloads, -0, +/-Inf, coverage outside
    // [0,1].
    if (descriptor.element_type == ElementType::Float32 && raw.size() >= 24) {
      const std::array<std::uint32_t, 6> bits{0x7f800001, 0x7fc12345,
                                              0x80000000, 0xff800000,
                                              0x7f800000, 0x3fcccccd};
      std::memcpy(raw.data(), bits.data(), sizeof(bits));
    }
    if (descriptor.element_type == ElementType::Int64) {
      const std::array<std::int64_t, 4> extremes{INT64_MIN, INT64_MAX, -1, 0};
      std::memcpy(raw.data(), extremes.data(),
                  std::min(raw.size(), sizeof(extremes)));
    }
    const auto region = Region::whole(descriptor.shape);
    document.inputs.push_back(
        {1, "source", descriptor, region, dense(descriptor), original});
    if (image) {
      PlanarImageConfig config;
      if (descriptor.shape.size() == 2) {
        config.channel_axis.reset();
      }
      config.order =
          tiled ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
      if (!tiled) {
        config.row_pitch_bytes =
            descriptor.shape[1] * Value::element_size(descriptor.element_type) +
            64;
      }
      auto source = take(PlanarImage::create(descriptor, config, original));
      if (published) {
        auto value =
            take(Value::create(descriptor, region, dense(descriptor), raw));
        std::vector<std::uint8_t> bytes;
        auto fp = take(Footprint::from_regions(descriptor.shape, {*published}));
        const auto width = Value::element_size(descriptor.element_type);
        take(fp.visit(
            [&](const auto& at) {
              auto offset = take(value.byte_address(at));
              bytes.insert(bytes.end(), raw.begin() + offset,
                           raw.begin() + offset + width);
              return Status::success();
            },
            UINT64_MAX));
        take(source.publish(*published, bytes.data(), bytes.size()));
      } else {
        take(source.publish(region, raw.data(), raw.size()));
      }
      document.inputs[0].layout = {};
      document.inputs[0].planar_layout = PlanarImageLayout{
          config.order, 0, 1, config.channel_axis, config.row_pitch_bytes, {}};
      ExecutionBinding binding;
      binding.name = "source";
      binding.image = std::make_shared<const PlanarImage>(std::move(source));
      bindings.inputs.push_back(std::move(binding));
    } else {
      bindings.inputs.push_back(
          {"source", take(Value::create(descriptor, region, dense(descriptor),
                                        raw, original))});
    }
  }
  auto build(WorkflowNodeOutput edge,
             const std::optional<Region>& region = {}) {
    document.outputs = {{"result", edge.source_node, edge.source_port}};
    graph = std::make_unique<GraphContext>(document);
    Compiler compiler(registry);
    PlanningOptions options;
    if (region) {
      options.output_regions = {{"result", *region}};
    }
    return compiler.compile(*graph, options, resources);
  }
  ExecutionResult run(WorkflowNodeOutput edge,
                      const std::optional<Region>& region = {}) {
    auto compiled = take(build(edge, region));
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    ExecutionContext context(registry, config);
    return take(context.execute(compiled.plan, bindings));
  }
  void oracle(const ExecutionResult& result, const Region& region) const {
    const auto width = Value::element_size(descriptor.element_type);
    std::vector<std::uint8_t> bytes;
    if (result.images.count("result")) {
      bytes.resize(take(region.element_count()) * width);
      take(result.images.at("result").read(region, bytes.data(), bytes.size()));
      require(result.images.at("result").valid_samples() ==
                  take(region.element_count()),
              "exact image coverage");
    }
    std::uint64_t n = 0;
    auto fp = take(Footprint::from_regions(descriptor.shape, {region}));
    take(fp.visit(
        [&](const auto& at) {
          std::uint64_t source = 0;
          for (std::size_t a = 0; a < at.size(); ++a) {
            source = source * descriptor.shape[a] + at[a];
          }
          const auto* actual =
              bytes.empty()
                  ? result.values.at("result").bytes().data() +
                        take(result.values.at("result").byte_address(at))
                  : bytes.data() + n * width;
          require(!std::memcmp(actual, raw.data() + source * width, width),
                  "independent exact-byte oracle");
          ++n;
          return Status::success();
        },
        UINT64_MAX));
  }
};
const std::vector<ValueFacet>& facets(const ExecutionResult& r) {
  return r.images.count("result") ? r.images.at("result").facets()
                                  : r.values.at("result").facets();
}
void edits() {
  auto d = rgb();
  auto f = take(encode_tensor_description(d));
  Fixture fixture({ElementType::Float32, {2, 3, 4}},
                  {f, {"app.note", 1, {'o', 'l', 'd'}}});
  format::MetadataOptions options;
  options.set = {{"/semantic/groups/color/interpretation/primaries",
                  std::string("display-p3")}};
  auto edge =
      take(format::assign_metadata(fixture.document, fixture.input, options));
  auto result = fixture.run(edge);
  require(decoded(facets(result)).groups[0].interpretation.primaries ==
              "display-p3",
          "patch primaries");
  require(decoded(fixture.bindings.inputs[0].value.facets())
                  .groups[0]
                  .interpretation.primaries == "srgb",
          "source immutable");
  require(result.values.at("result").storage().get() ==
              fixture.bindings.inputs[0].value.storage().get(),
          "generic view owner");
  fixture.oracle(result, Region::whole(fixture.descriptor.shape));
  options = {};
  options.mode = "replace";
  options.description = TensorDescription{};
  edge =
      take(format::assign_metadata(fixture.document, fixture.input, options));
  result = fixture.run(edge);
  require(facets(result).size() == 1 && facets(result)[0].key == "app.note",
          "empty replace keeps annotations");
  options = {};
  options.set = {{"/annotations/app.new~1key",
                  ValueFacet{"app.new/key", 1, {1, 2, 0, 255}}}};
  edge =
      take(format::assign_metadata(fixture.document, fixture.input, options));
  result = fixture.run(edge);
  require(facets(result).size() == 3, "opaque annotation set");
  options = {};
  edge =
      take(format::remove_metadata(fixture.document, fixture.input,
                                   {"/semantic/groups/color/alpha"}, options));
  result = fixture.run(edge);
  require(!decoded(facets(result)).groups[0].alpha, "remove relationship only");
  fixture.oracle(result, Region::whole(fixture.descriptor.shape));
  edge = take(format::remove_metadata(
      fixture.document, fixture.input,
      {"/semantic/groups/color/interpretation/primaries"}));
  require(!fixture.build(edge).ok(), "required field deletion fails");
  fixture.document.nodes.pop_back();
  options.dependencies = "cascade";
  edge = take(format::remove_metadata(
      fixture.document, fixture.input,
      {"/semantic/groups/color/interpretation/primaries"}, options));
  result = fixture.run(edge);
  auto after = decoded(facets(result));
  require(after.groups.empty() && after.channels[0].name == "R",
          "cascade keeps independent channels");
  options.set = {{"/semantic/groups/color/alpha", std::uint64_t{3}}};
  options.remove = {"/semantic/groups/color/interpretation/primaries"};
  edge =
      take(format::assign_metadata(fixture.document, fixture.input, options));
  require(!fixture.build(edge).ok(), "cascade protects explicit group content");
  fixture.document.nodes.pop_back();
  options = {};
  options.set = {{"/semantic/channels/name:R/unit", std::string("new")},
                 {"/semantic/channels/index:0/unit", std::string("new")}};
  edge =
      take(format::assign_metadata(fixture.document, fixture.input, options));
  require(!fixture.build(edge).ok(), "resolved selector overlap");
  fixture.document.nodes.pop_back();
  options = {};
  options.missing = "ignore";
  edge = take(format::remove_metadata(fixture.document, fixture.input,
                                      {"/annotations/missing"}, options));
  fixture.oracle(fixture.run(edge), Region::whole(fixture.descriptor.shape));
  auto count = fixture.document.nodes.size();
  require(!format::remove_metadata(fixture.document, fixture.input,
                                   {"/semantic/dtype"}, options)
               .ok(),
          "unknown field cannot be ignored");
  require(!format::remove_metadata(fixture.document, fixture.input,
                                   {"/semantic", "/semantic/model"})
               .ok(),
          "ancestor overlap");
  require(fixture.document.nodes.size() == count, "helper graph rollback");
  edge = take(format::remove_metadata(fixture.document, fixture.input,
                                      {"/semantic", "/annotations/app.note"}));
  result = fixture.run(edge);
  require(facets(result).empty(), "clear explicit roots");
  // Atomic whole-group replacement changes interpretation with no RGB->Lab
  // math.
  auto lab = d.groups[0];
  lab.interpretation = {};
  lab.interpretation.model = "cielab";
  lab.interpretation.white = std::array<double, 2>{.3127, .3290};
  lab.components = {{"", "l", ""}, {"", "a", ""}, {"", "b", ""}};
  options = {};
  options.set = {{"/semantic/groups/color", lab}};
  options.remove = {"/semantic/channels"};
  edge =
      take(format::assign_metadata(fixture.document, fixture.input, options));
  result = fixture.run(edge);
  fixture.oracle(result, Region::whole(fixture.descriptor.shape));
  require(decoded(facets(result)).groups[0].interpretation.model == "cielab",
          "atomic group reinterpretation");
}
void dtypes_and_layouts() {
  for (auto type : {ElementType::UInt8, ElementType::UInt16, ElementType::Int8,
                    ElementType::Int16, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    for (const auto* policy : {"auto", "view", "materialize"}) {
      for (unsigned rank = 1; rank <= 8; ++rank) {
        ValueDescriptor d{type, std::vector<std::uint64_t>(rank, 2)};
        Fixture f(d);
        format::MetadataOptions options;
        options.layout = policy;
        options.set = {
            {"/semantic/component",
             TensorChannelDescription{"coverage", "coverage", "ratio"}}};
        auto edge = take(format::assign_metadata(f.document, f.input, options));
        auto dims = Region::whole(d.shape).dimensions();
        dims.back() = {1, 1};
        Region roi(dims);
        auto result = f.run(edge, roi);
        f.oracle(result, roi);
        const bool shared = result.values.at("result").storage().get() ==
                            f.bindings.inputs[0].value.storage().get();
        require(shared == (std::string(policy) != "materialize"),
                "layout owner contract");
      }
    }
  }
  for (bool tiled : {false, true}) {
    for (bool channel_axis : {false, true}) {
      ValueDescriptor d{ElementType::Float32, {130, 131}};
      if (channel_axis) {
        d.shape.push_back(4);
      }
      auto dims = Region::whole(d.shape).dimensions();
      dims[0] = {127, 3};
      dims[1] = {126, 4};
      if (channel_axis) {
        dims[2] = {2, 1};
      }
      Region roi(dims);
      Fixture f(d, {}, true, tiled, roi);
      for (const auto* policy : {"auto", "view", "materialize"}) {
        format::MetadataOptions o;
        o.layout = policy;
        o.set = {{"/semantic/component",
                  TensorChannelDescription{"sample", "coverage", "ratio"}}};
        auto edge = take(format::assign_metadata(f.document, f.input, o));
        auto result = f.run(edge, roi);
        f.oracle(result, roi);
        require((result.images.at("result").owner_token() ==
                 f.bindings.inputs[0].image->owner_token()) ==
                    (std::string(policy) != "materialize"),
                "planar owner contract");
        require(result.diagnostics.result_copy_bytes ==
                    (std::string(policy) == "materialize" ? 48 : 0),
                "planar copy accounting");
        auto wrong = dims;
        wrong[1] = {0, 1};
        auto compiled = take(f.build(edge, Region(wrong)));
        ExecutionContext context(f.registry);
        auto missing = context.execute(compiled.plan, f.bindings);
        require(!missing.ok(), "view cannot invent missing coverage");
        auto live = take(f.build(edge, roi));
        CancellationSource stop;
        stop.cancel();
        require(!context.execute(live.plan, f.bindings, stop.token()).ok(),
                "cancelled execution");
      }
    }
  }
}
void strided() {
  for (auto stride : {std::int64_t{-4}, std::int64_t{0}}) {
    Fixture f({ElementType::Float32, {8}});
    auto layout = StridedLayout{stride < 0 ? 28U : 0U, {stride}};
    auto source =
        take(Value::create(f.descriptor, Region::whole({8}), layout, f.raw));
    f.registry = make_default_operation_registry(false);
    OperationDefinition producer;
    producer.key = "test.strided_metadata";
    producer.traits.outputs[0].output_element_type = ElementType::Float32;
    producer.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
    producer.traits.outputs[0].fixed_output_shape = {8};
    producer.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    producer.callback = [source](const OperationInvocation&) {
      return Result<Value>(source);
    };
    take(f.registry->register_operation(std::move(producer)));
    take(f.registry->freeze());
    f.document.inputs.clear();
    f.bindings.inputs.clear();
    f.document.nodes = {{1, "test.strided_metadata", {}, {}}};
    f.input = WorkflowNodeOutput{1, "value"};
    for (const auto* policy : {"view", "materialize"}) {
      format::MetadataOptions o;
      o.layout = policy;
      auto result =
          f.run(take(format::assign_metadata(f.document, f.input, o)));
      for (std::uint64_t i = 0; i < 8; ++i) {
        auto expected = stride < 0 ? 28 - i * 4 : 0;
        auto at = take(result.values.at("result").byte_address({i}));
        require(!std::memcmp(result.values.at("result").bytes().data() + at,
                             f.raw.data() + expected, 4),
                "signed/zero stride bits");
      }
    }
  }
}
std::vector<std::uint8_t> rgb_profile(bool lut = false, bool backward = true) {
  const auto base = numeric_fixture::fixture();
  const auto word = [&](std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) {
      n = (n << 8) | base[at + i];
    }
    return n;
  };
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> tags;
  for (unsigned i = 0; i < 3; ++i) {
    const auto offset = word(136 + 12 * i), size = word(140 + 12 * i);
    tags.push_back(
        {std::string(reinterpret_cast<const char*>(base.data() + 132 + 12 * i),
                     4),
         {base.begin() + offset, base.begin() + offset + size}});
  }
  if (lut) {
    tags.push_back({"A2B0", numeric_fixture::table(3, 3)});
    if (backward) {
      tags.push_back({"B2A0", numeric_fixture::table(3, 3)});
    }
  } else {
    for (const auto* key : {"rXYZ", "gXYZ", "bXYZ"}) {
      tags.push_back({key, tags[2].second});
    }
    std::vector<std::uint8_t> curve(12);
    numeric_fixture::key(&curve, 0, "curv");
    for (const auto* key : {"rTRC", "gTRC", "bTRC"}) {
      tags.push_back({key, curve});
    }
  }
  std::vector<std::uint8_t> out(base.begin(), base.begin() + 128);
  out.resize(132 + 12 * tags.size());
  numeric_fixture::word(&out, 128, tags.size());
  numeric_fixture::key(&out, 12, "mntr");
  numeric_fixture::key(&out, 16, "RGB ");
  numeric_fixture::key(&out, 20, "XYZ ");
  for (std::size_t i = 0; i < tags.size(); ++i) {
    numeric_fixture::key(&out, 132 + 12 * i, tags[i].first.c_str());
    numeric_fixture::word(&out, 136 + 12 * i, out.size());
    numeric_fixture::word(&out, 140 + 12 * i, tags[i].second.size());
    out.insert(out.end(), tags[i].second.begin(), tags[i].second.end());
    while (out.size() % 4) {
      out.push_back(0);
    }
  }
  numeric_fixture::word(&out, 0, out.size());
  return out;
}
void icc_resources() {
  ResourceBudget budget;
  auto bytes = rgb_profile();
  auto profile =
      take(IccProfile::import(ByteView(bytes.data(), bytes.size()), budget));
  require(profile.model() == "rgb", "admitted RGB ICC header model");
  auto resources = take(ResourceBindings::create({profile}, budget));
  ColorArrayDescriptor legacy;
  legacy.model = ColorModel::Cmyk;
  legacy.reference = ColorReference::ProfileRelative;
  legacy.white.reset();
  legacy.profile = profile.identity();
  require(!resources.select({take(encode_color_array(legacy))}).ok(),
          "RGB profile cannot satisfy legacy CMYK");
  for (const auto& entry :
       {std::make_pair(20U, "Lab "), std::make_pair(12U, "prtr"),
        std::make_pair(12U, "abst")}) {
    auto bad = bytes;
    numeric_fixture::key(&bad, entry.first, entry.second);
    require(!IccProfile::import(ByteView(bad.data(), bad.size()), budget).ok(),
            "ICC class/PCS constraints");
  }
  auto bad = rgb_profile(true, false);
  require(!IccProfile::import(ByteView(bad.data(), bad.size()), budget).ok(),
          "LUT display needs both directions");
  auto incomplete_output = rgb_profile(true, true);
  numeric_fixture::key(&incomplete_output, 12, "prtr");
  for (const auto* model : {"XYZ ", "Lab "}) {
    numeric_fixture::key(&incomplete_output, 16, model);
    require(!IccProfile::import(
                 ByteView(incomplete_output.data(), incomplete_output.size()),
                 budget)
                 .ok(),
            "all non-Gray output models require intent/gamut tags");
  }
  auto valid = rgb_profile(true, true);
  require(IccProfile::import(ByteView(valid.data(), valid.size()), budget).ok(),
          "explicit bidirectional LUT profile");
  auto d = rgb();
  d.groups[0].interpretation.primaries.clear();
  d.groups[0].interpretation.transfer.clear();
  d.groups[0].interpretation.profile = profile.identity();
  d.groups[0].interpretation.convention = "icc-native";
  require(resources.select({take(encode_tensor_description(d))}).ok(),
          "profile-defined RGB needs no invented primaries");
  TensorAnalyticBinding binding;
  binding.model = "rgb";
  binding.primaries = "srgb";
  binding.transfer = "linear";
  binding.roles = {"red", "green", "blue"};
  binding.units = {"relative", "relative", "relative"};
  d.groups[0].interpretation.analytic_binding = binding;
  require(encode_tensor_description(d).ok(),
          "explicit analytic binding declaration");
  d.groups[0].interpretation.analytic_binding->roles[0] = "blue";
  require(!encode_tensor_description(d).ok(),
          "analytic binding order mismatch");
}
void schema_and_resources() {
  TensorDescription d;
  d.component = TensorChannelDescription{"code", "sample", "relative"};
  TensorEncoding e;
  e.stored = {INT64_MIN, INT64_MAX};
  e.decoded = {-0.0, 1.0};
  d.component->encoding = e;
  auto facet = take(encode_tensor_description(d));
  require(facet.version == 3, "v3 discriminator");
  auto roundtrip = take(decode_tensor_description(facet));
  require(*roundtrip.component->encoding == e,
          "exact Int64 endpoint and signed zero roundtrip");
  require(validate_tensor_description(d, {ElementType::Int64, {2}}).ok(),
          "exact int64 range");
  require(!validate_tensor_description(d, {ElementType::Float32, {2}}).ok() ==
              false,
          "finite FP32 interval");
  auto old = facet;
  old.version = 2;
  require(!decode_tensor_description(old).ok(), "old runtime version rejected");
  old = facet;
  old.payload.push_back(0);
  require(!decode_tensor_description(old).ok(),
          "noncanonical trailing bytes rejected");
  e.stored = {INT64_MAX, 0x1p63};
  d.component->encoding = e;
  require(encode_tensor_description(d).ok(),
          "mixed exact endpoint ordering near 2^63");
  require(!validate_tensor_description(d, {ElementType::Int64, {2}}).ok(),
          "2^63 exceeds int64 dtype");
  std::swap(e.stored[0], e.stored[1]);
  d.component->encoding = e;
  require(!encode_tensor_description(d).ok(),
          "reversed stored interval rejected exactly");
  auto groups = rgb();
  groups.channels.clear();
  groups.groups.push_back(groups.groups[0]);
  groups.groups[1].name = "second";
  TensorEncoding a;
  a.stored = {std::int64_t{0}, std::int64_t{255}};
  auto b = a;
  b.stored[1] = std::int64_t{127};
  groups.groups[0].components[0].encoding = a;
  groups.groups[1].components[0].encoding = b;
  require(!encode_tensor_description(groups).ok(),
          "overlapping encoding conflict");
  groups.groups[1].components[0].encoding = a;
  for (auto& c : groups.groups[0].components) {
    c.sampling = TensorSampling{"grid-a"};
  }
  for (auto& c : groups.groups[1].components) {
    c.sampling = TensorSampling{"grid-b"};
  }
  require(!encode_tensor_description(groups).ok(),
          "overlapping sampling conflict");
  d = {};
  d.sampling = TensorSampling{"grid", {2, 1}, {0, 0}};
  require(!encode_tensor_description(d).ok(), "external subsampling refused");
  ResourceBudget budget;
  OcioConfigSnapshot snapshot;
  snapshot.config = {'c', 'o', 'n', 'f', 'i', 'g'};
  snapshot.files = {{"lut.spi1d", {0, 1, 2, 3}}};
  snapshot.context = {{"SHOT", "one"}};
  snapshot.spaces = {{"linear", "scene"}};
  snapshot.build_identity = "test-pinned-build";
  snapshot.settings = "reference";
  auto config = take(OcioConfigResource::import(snapshot, budget));
  auto same = take(OcioConfigResource::import(snapshot, budget));
  require(config.identity() == same.identity(),
          "same snapshot content identity");
  snapshot.context["SHOT"] = "two";
  auto changed = take(OcioConfigResource::import(snapshot, budget));
  require(!(config.identity() == changed.identity()),
          "context enters identity");
  require(!config.lookup("files", "missing.spi1d").ok(),
          "closed resource lookup");
  require(take(config.lookup("files", "lut.spi1d")).size() == 4,
          "owned file bytes");
  auto bindings = take(ResourceBindings::create({}, {config, same}, budget));
  require(bindings.size() == 1 && bindings.config_count() == 1,
          "config deduplication");
  TensorInterpretation native;
  native.model = "rgb";
  native.convention = "ocio-native";
  native.configured =
      TensorConfiguredSpace{config.identity(), "linear", "scene"};
  auto source = rgb();
  source.groups[0].interpretation = native;
  auto native_facet = take(encode_tensor_description(source));
  require(!ResourceBindings{}.select({native_facet}).ok(),
          "resource references fail closed");
  require(take(bindings.select({native_facet})).config_count() == 1,
          "select retains referenced resource");
  require(take(bindings.select({})).size() == 0,
          "select releases removed resource");
  Fixture f({ElementType::Float32, {2, 3, 4}});
  f.resources = bindings;
  format::MetadataOptions o;
  o.mode = "replace";
  o.description = source;
  auto result = f.run(take(format::assign_metadata(f.document, f.input, o)));
  f.oracle(result, Region::whole(f.descriptor.shape));
  require(result.values.at("result")
              .resources()
              .ocio_config(config.identity())
              .ok(),
          "runtime config owner survives context");
  o.description->groups[0].interpretation.configured->space = "missing";
  auto edge = take(format::assign_metadata(f.document, f.input, o));
  require(!f.build(edge).ok(), "unknown configured space fails");
  f.document.nodes.pop_back();
  // Existing channel consumers must carry v3 configured interpretation without
  // treating an empty local accumulator as a relative-coordinate assertion.
  f.document.nodes.push_back(
      {999,
       "channel.extract_index_strict",
       {WorkflowNodeOutput{f.document.nodes[0].id, "values"}},
       {{"axis", std::int64_t{2}},
        {"index", std::int64_t{0}},
        {"keepdims", false},
        {"layout", std::string("view")},
        {"metadata_mode", std::string("respect")}}});
  auto assembled = take(format::assemble_channels(
      f.document, {WorkflowNodeOutput{999, "values"}}, 2));
  auto assembled_result = f.run(assembled);
  auto assembled_description = decoded(facets(assembled_result));
  require(assembled_description.channels.size() == 1 &&
              assembled_description.channels[0]
                  .interpretation->configured.has_value(),
          "v3 extraction and assembly preserve configured coordinates");
  require(assembled_result.values.at("result").resources().config_count() == 1,
          "v3 consumer chain retains config resource");
  // Root and independent-channel interpretation dependency units cascade
  // without deleting samples or independent labels.
  for (bool root : {false, true}) {
    TensorDescription desc;
    if (root) {
      desc.convention = "ocio-native";
      desc.configured = native.configured;
    } else {
      desc.channel_axis = 2;
      desc.channels.resize(4);
      desc.channels[0].name = "retained";
      desc.channels[0].interpretation = native;
    }
    Fixture c({ElementType::Float32, {2, 3, 4}});
    c.resources = bindings;
    format::MetadataOptions set;
    set.mode = "replace";
    set.description = desc;
    auto assigned = take(format::assign_metadata(c.document, c.input, set));
    format::MetadataOptions remove;
    remove.dependencies = "cascade";
    auto cleared = take(format::remove_metadata(
        c.document, assigned,
        {root ? "/semantic/configured"
              : "/semantic/channels/index:0/interpretation/configured"},
        remove));
    auto observed = c.run(cleared);
    auto metadata = decoded(facets(observed));
    require(root ? !metadata.configured
                 : (!metadata.channels[0].interpretation &&
                    metadata.channels[0].name == "retained"),
            "minimal interpretation dependency cleanup");
    require(observed.values.at("result").resources().size() == 0,
            "removed config is not owned by result header");
    c.oracle(observed, Region::whole(c.descriptor.shape));
  }
  auto cancelled = CancellationSource{};
  cancelled.cancel();
  require(OcioConfigResource::import(snapshot, budget, cancelled.token())
                  .status()
                  .code == ErrorCode::Cancelled,
          "resource import cancellation");
  require(OcioConfigResource::import(snapshot, budget, {}, 1).status().code ==
              ErrorCode::ResourceExhausted,
          "resource work limit");
  ResourceLimits limits;
  limits.capacity = ResourceCapacity::host(64, 64);
  limits.cleanup = {};
  ResourceBudget tiny(limits);
  require(!OcioConfigResource::import(snapshot, tiny).ok(),
          "resource capacity rejection");
  require(tiny.statistics().live[ResourceKind::Host] == 0,
          "failed resource import releases capacity");
  Fixture g({ElementType::Float32, {1, 1, 4}},
            {take(encode_tensor_description(rgb()))});
  format::MetadataOptions ignore;
  ignore.missing = "ignore";
  edge = take(format::remove_metadata(
      g.document, g.input, {"/semantic/channels/name:absent"}, ignore));
  g.oracle(g.run(edge), Region::whole(g.descriptor.shape));
  auto count = g.document.nodes.size();
  o = {};
  o.set = {{"/semantic/model", std::uint64_t{1}}};
  require(!format::assign_metadata(g.document, g.input, o).ok() &&
              count == g.document.nodes.size(),
          "typed authoring rollback");
}
void dependency_boundaries() {
  auto d = rgb();
  TensorEncoding e;
  e.stored = {std::int64_t{0}, std::int64_t{255}};
  e.decoded = {0.0, 1.0};
  d.encoding = e;
  Fixture integer({ElementType::UInt8, {2, 3, 4}},
                  {take(encode_tensor_description(d))});
  auto edge = take(format::remove_metadata(integer.document, integer.input,
                                           {"/semantic/encoding"}));
  require(!integer.build(edge).ok(), "required integer decoder deletion fails");
  integer.document.nodes.pop_back();
  format::MetadataOptions cascade;
  cascade.dependencies = "cascade";
  edge = take(format::remove_metadata(integer.document, integer.input,
                                      {"/semantic/encoding"}, cascade));
  auto result = integer.run(edge);
  require(decoded(facets(result)).groups.empty(),
          "integer decoder cascade removes complete claim");
  integer.oracle(result, Region::whole(integer.descriptor.shape));
  TensorDescription grids;
  grids.channel_axis = 0;
  auto a = rgb().groups[0];
  a.name = "grid-a";
  a.alpha.reset();
  for (auto& c : a.components) {
    c.sampling = TensorSampling{"a"};
  }
  auto b = a;
  b.name = "grid-b";
  b.indices = {3, 4, 5};
  for (auto& c : b.components) {
    c.sampling = TensorSampling{"b"};
  }
  grids.groups = {a, b};
  Fixture f({ElementType::Float32, {6}},
            {take(encode_tensor_description(grids))});
  cascade.set = {{"/semantic/sampling", TensorSampling{"a"}}};
  edge = take(format::assign_metadata(f.document, f.input, cascade));
  result = f.run(edge);
  require(decoded(facets(result)).groups.size() == 1 &&
              decoded(facets(result)).groups[0].name == "grid-a",
          "cascade only affected sampling group");
  auto compiled = take(f.build(edge));
  ExecutionContextConfig limited;
  limited.cpu_workers = 1;
  limited.managed_resources = ResourceLimits{};
  limited.managed_resources->capacity[ResourceKind::Metadata] = 1;
  ExecutionContext low(f.registry, limited);
  auto failure = low.execute(compiled.plan, f.bindings);
  require(
      !failure.ok() && failure.status().code == ErrorCode::ResourceExhausted,
      "metadata capacity admission");
  ExecutionOptions work;
  work.maximum_dependency_work = 1;
  ExecutionContext normal(f.registry);
  failure = normal.execute(compiled.plan, f.bindings, {}, work);
  require(
      !failure.ok() && failure.status().code == ErrorCode::ResourceExhausted,
      "work admission");
  f.oracle(take(normal.execute(compiled.plan, f.bindings)),
           Region::whole(f.descriptor.shape));
  auto source = f.document.nodes[0].parameters;
  OperationMetadata metadata;
  metadata.descriptor = f.descriptor;
  metadata.facets = f.original;
  auto traits = take(
      f.registry->resolve_traits("metadata.assign_strict", {metadata}, source));
  require(!traits.cacheable &&
              traits.outputs[0].static_dependency_pieces->size() == 1,
          "no sample-only cache and exact dependency map");
  const auto& axes =
      traits.outputs[0].static_dependency_pieces->front().inputs[0].axes;
  require(axes[0].observation_axis == 0 && axes[0].translation == 0,
          "identity data and dirty relation");
#if defined(__aarch64__) && defined(__APPLE__)
  cascade.profile = "accelerated_apple_silicon";
  f.oracle(f.run(take(format::assign_metadata(f.document, f.input, cascade))),
           Region::whole(f.descriptor.shape));
  cascade.profile = "accelerated_x86_64";
  edge = take(format::assign_metadata(f.document, f.input, cascade));
  require(!f.build(edge).ok(),
          "unsupported CPU profile does not silently fallback");
#endif
  // Retained config has no source, graph, compiler or execution-context owner.
  ResourceBudget budget;
  Value surviving;
  ColorProfileIdentity id;
  {
    OcioConfigSnapshot snapshot;
    snapshot.config = {'x'};
    snapshot.spaces = {{"working", "scene"}};
    snapshot.build_identity = "fixture-build";
    snapshot.settings = "reference";
    auto config = take(OcioConfigResource::import(snapshot, budget));
    id = config.identity();
    Fixture owner({ElementType::Float32, {4}});
    owner.resources = take(ResourceBindings::create({}, {config}, budget));
    TensorInterpretation space;
    space.model = "rgb";
    space.convention = "ocio-native";
    space.configured = TensorConfiguredSpace{id, "working", "scene"};
    format::MetadataOptions edit;
    TensorChannelDescription component{"channel", "red", "relative"};
    component.interpretation = space;
    edit.set = {{"/semantic/component", component}};
    surviving = owner
                    .run(take(format::assign_metadata(owner.document,
                                                      owner.input, edit)))
                    .values.at("result");
  }
  require(surviving.resources().ocio_config(id).ok() &&
              budget.statistics().live[ResourceKind::Host] > 0,
          "retained immutable config after context teardown");
  surviving = {};
  require(budget.statistics().live[ResourceKind::Host] == 0,
          "last result retires config capacity");
  // Exercise full physical tiles and a short edge, with an independent
  // full-byte oracle.
  for (bool tiled : {false, true}) {
    Fixture image({ElementType::UInt16, {130, 131, 4}}, {}, true, tiled);
    format::MetadataOptions copy;
    copy.layout = "materialize";
    auto out = image.run(
        take(format::assign_metadata(image.document, image.input, copy)));
    image.oracle(out, Region::whole(image.descriptor.shape));
    copy.set = {{"/semantic/channel_axis", std::uint64_t{0}}};
    auto invalid =
        take(format::assign_metadata(image.document, image.input, copy));
    require(image.build(invalid).status().code == ErrorCode::TypeMismatch,
            "metadata cannot relabel a different physical image axis");
  }
}
}  // namespace
int main() try {
  edits();
  dtypes_and_layouts();
  strided();
  schema_and_resources();
  icc_resources();
  dependency_boundaries();
  std::cout << "FMT-08 metadata transactions, byte oracle, exact ROI, "
               "ownership and errors passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
