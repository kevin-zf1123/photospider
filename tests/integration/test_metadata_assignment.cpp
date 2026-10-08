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
  std::unique_ptr<ExecutionContext> context;
  SchemaTemplate schema;
  bool spatial = false;
  std::optional<Region> coverage;
  explicit Fixture(ValueDescriptor d, std::vector<ValueFacet> facets = {},
                   bool image = false, bool tiled = false,
                   std::optional<Region> published = {},
                   std::optional<ResourceLimits> limits = {},
                   std::vector<std::uint64_t> batches = {})
      : descriptor(std::move(d)),
        original(std::move(facets)),
        spatial(image),
        coverage(std::move(published)) {
    ExecutionContextConfig config;
    config.managed_resources = limits;
    context = std::make_unique<ExecutionContext>(registry, config);
    auto count = take(Region::whole(descriptor.shape).element_count());
    for (auto n : batches)
      count *= n;
    raw.resize(count * Value::element_size(descriptor.element_type));
    for (std::size_t i = 0; i < raw.size(); ++i)
      raw[i] = (i * 73 + 29) & 255;
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
    schema.id = "metadata.fixture";
    ResultTensorSpec tensor;
    tensor.key = "samples";
    tensor.batch_axes.assign(batches.begin(), batches.end());
    tensor.descriptor = descriptor;
    tensor.facets = original;
    std::sort(tensor.facets.begin(), tensor.facets.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    tensor.layout.spatial = image;
    tensor.layout.order =
        tiled ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
    if (descriptor.shape.size() == 2)
      tensor.layout.channel_axis.reset();
    if (image && !tiled)
      tensor.layout.row_pitch_bytes =
          descriptor.shape[1] * Value::element_size(descriptor.element_type) +
          64;
    schema.tensors.push_back(std::move(tensor));
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "source";
    declaration.result_schema = std::make_shared<const SchemaTemplate>(schema);
    document.inputs.push_back(std::move(declaration));
    bind();
  }
  void bind(std::optional<StridedLayout> override_layout = {}) {
    const auto root = take(context->resource_budget());
    const auto shape = schema.tensors[0].sample_shape();
    auto result = take(ResultBuilder::start(root, schema, "metadata.source", {},
                                            {}, 128, 128, resources));
    take(result.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
    auto relation = take(ResultRelation::cartesian(
        root, take(schema.tensors[0].sample_count()), {0, 1, 0, 0}));
    const auto region = coverage.value_or(Region::whole(shape));
    if (spatial) {
      take(result.publish_tensor_kernel(
          0, region,
          [&](const auto& writers) {
            const auto width = Value::element_size(descriptor.element_type);
            for (const auto& writer : writers) {
              auto fp = take(Footprint::from_regions(shape, {writer.region()}));
              take(fp.visit(
                  [&](const auto& at) {
                    std::uint64_t offset = 0;
                    for (std::size_t axis = 0; axis < at.size(); ++axis)
                      offset = offset * shape[axis] + at[axis];
                    auto row = take(writer.row_run(at));
                    std::memcpy(row.data, raw.data() + offset * width, width);
                    return Status::success();
                  },
                  UINT64_MAX));
            }
            return Status::success();
          },
          relation, {true, true, true, true}));
    } else {
      auto bytes = take(root.allocator().allocate(raw.size()));
      std::memcpy(bytes.data(), raw.data(), raw.size());
      take(result.publish_tensor(
          0, region,
          override_layout.value_or(dense({descriptor.element_type, shape})),
          std::move(bytes).freeze(), relation, {true, true, true, true}));
    }
    bindings.inputs = {{"source", take(result.seal())}};
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
    ExecutionOptions options;
    options.maximum_dependency_work = 1000000000;
    options.dependencies.maximum_work = 1000000000;
    return take(context->execute(compiled.plan, bindings, {}, options));
  }
  void oracle(const ExecutionResult& result, const Region& region) const {
    const auto& value = result.results.at("result");
    const auto facts = take(value.descriptor());
    auto fp = take(
        Footprint::from_regions(schema.tensors[0].sample_shape(), {region}));
    require(facts.tensor_coverage(0) == fp, "exact Result coverage");
    auto window = take(value.acquire_tensor(facts, 0, region));
    const auto width = Value::element_size(descriptor.element_type);
    take(fp.visit(
        [&](const auto& at) {
          std::uint64_t source = 0;
          const auto shape = schema.tensors[0].sample_shape();
          for (std::size_t a = 0; a < at.size(); ++a)
            source = source * shape[a] + at[a];
          const auto actual = take(window.row_run(at));
          require(!std::memcmp(actual.data, raw.data() + source * width, width),
                  "independent exact-byte oracle");
          return Status::success();
        },
        UINT64_MAX));
  }
};
const std::vector<ValueFacet>& facets(const ExecutionResult& r) {
  return r.results.at("result").schema().tensors[0].facets;
}
const void* owner(const ResultRef& result, const Region& region) {
  auto window =
      take(result.acquire_tensor(take(result.descriptor()), 0, region));
  return window.storage_owner_token();
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
  require(decoded(fixture.bindings.inputs[0].result.schema().tensors[0].facets)
                  .groups[0]
                  .interpretation.primaries == "srgb",
          "source immutable");
  require(owner(result.results.at("result"),
                Region::whole(fixture.descriptor.shape)) ==
              owner(fixture.bindings.inputs[0].result,
                    Region::whole(fixture.descriptor.shape)),
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
        const bool shared = owner(result.results.at("result"), roi) ==
                            owner(f.bindings.inputs[0].result, roi);
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
        require((owner(result.results.at("result"), roi) ==
                 owner(f.bindings.inputs[0].result, roi)) ==
                    (std::string(policy) != "materialize"),
                "planar owner contract");
        auto wrong = dims;
        wrong[1] = {0, 1};
        auto compiled = take(f.build(edge, Region(wrong)));
        auto missing = f.context->execute(compiled.plan, f.bindings);
        require(!missing.ok(), "view cannot invent missing coverage");
        auto live = take(f.build(edge, roi));
        CancellationSource stop;
        stop.cancel();
        require(!f.context->execute(live.plan, f.bindings, stop.token()).ok(),
                "cancelled execution");
      }
    }
  }
}
void strided() {
  for (auto stride : {std::int64_t{-4}, std::int64_t{0}}) {
    Fixture f({ElementType::Float32, {8}});
    auto layout = StridedLayout{stride < 0 ? 28U : 0U, {stride}};
    f.bind(layout);
    for (const auto* policy : {"view", "materialize"}) {
      format::MetadataOptions o;
      o.layout = policy;
      auto result =
          f.run(take(format::assign_metadata(f.document, f.input, o)));
      for (std::uint64_t i = 0; i < 8; ++i) {
        auto expected = stride < 0 ? 28 - i * 4 : 0;
        std::uint32_t actual = 0;
        const auto& value = result.results.at("result");
        take(value.read_tensor(take(value.descriptor()), 0, {i}, &actual, 4));
        require(!std::memcmp(&actual, f.raw.data() + expected, 4),
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
  require(facet.version == 4, "v4 discriminator");
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
  require(result.results.at("result")
              .resources()
              .ocio_config(config.identity())
              .ok(),
          "runtime config owner survives context");
  o.description->groups[0].interpretation.configured->space = "missing";
  auto edge = take(format::assign_metadata(f.document, f.input, o));
  require(!f.build(edge).ok(), "unknown configured space fails");
  f.document.nodes.pop_back();
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
    require(observed.results.at("result").resources().size() == 0,
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
  Fixture baseline(f.descriptor, f.original);
  ResourceLimits limits;
  limits.capacity[ResourceKind::Metadata] =
      take(baseline.context->resource_budget())
          .statistics()
          .peak[ResourceKind::Metadata];
  Fixture low(f.descriptor, f.original, false, false, {}, limits);
  auto low_edge =
      take(format::assign_metadata(low.document, low.input, cascade));
  auto low_plan = take(low.build(low_edge));
  auto failure = low.context->execute(low_plan.plan, low.bindings);
  require(
      !failure.ok() && failure.status().code == ErrorCode::ResourceExhausted,
      "metadata capacity admission");
  ExecutionOptions work;
  work.maximum_dependency_work = 1;
  failure = f.context->execute(compiled.plan, f.bindings, {}, work);
  require(
      !failure.ok() && failure.status().code == ErrorCode::ResourceExhausted,
      "work admission");
  f.oracle(take(f.context->execute(compiled.plan, f.bindings)),
           Region::whole(f.descriptor.shape));
  auto source = f.document.nodes[0].parameters;
  OperationMetadata metadata;
  metadata.result_schema = f.document.inputs[0].result_schema;
  auto traits = take(
      f.registry->resolve_traits("metadata.assign_strict", {metadata}, source));
  require(!traits.cacheable && traits.outputs[0].dependency_version == 2,
          "Result dependency program disables sample-only caching");
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
  ResultRef surviving;
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
                    .results.at("result");
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
void result_contract() {
  for (bool spatial : {false, true}) {
    for (bool tiled : {false, true}) {
      Fixture f({ElementType::Float32, {3, 5, 4}}, {}, spatial, tiled, {}, {},
                {2, 2});
      const auto shape = f.schema.tensors[0].sample_shape();
      const Region roi({{0, 2}, {1, 1}, {1, 2}, {2, 2}, {2, 1}});
      for (const auto* policy : {"auto", "view", "materialize"}) {
        format::MetadataOptions o;
        o.layout = policy;
        o.set = {{"/semantic/component",
                  TensorChannelDescription{"coverage", "coverage", "ratio"}}};
        const auto edge = take(format::assign_metadata(f.document, f.input, o));
        for (const auto& q : {roi, Region::whole(shape)}) {
          const auto output = f.run(edge, q);
          f.oracle(output, q);
          const auto& result = output.results.at("result");
          require(
              result.schema().publication == f.schema.publication &&
                  result.schema().id == f.schema.id &&
                  result.schema().tensors[0].key == f.schema.tensors[0].key &&
                  result.schema().tensors[0].batch_axes ==
                      f.schema.tensors[0].batch_axes,
              "metadata edit retains schema and publication contract");
          const auto query = take(Footprint::from_regions(shape, {q}));
          require(
              take(output.dependencies.source_support()).at("source") == query,
              "Data support is exactly Q including batch axes");
          unsigned visits = 0;
          take(take(result.tensor_relation(0))
                   .project(query, [&](auto support, const auto* samples) {
                     require(
                         support.input == 0 && support.roles == 1 &&
                             support.target == ResultSupportTarget::Tensor &&
                             support.slot == 0 && samples && *samples == query,
                         "metadata has Data only, no Validation/Control "
                         "support");
                     ++visits;
                     return Status::success();
                   }));
          require(visits == 1, "one compact identity support");
          take(take(result.descriptor_relation())
                   .visit(0, 100, [&](auto support) {
                     require(support.roles == 8 &&
                                 support.target ==
                                     ResultSupportTarget::Descriptor &&
                                 support.count == 1,
                             "static descriptor support is independent");
                     return Status::success();
                   }));
          auto one = q.dimensions();
          for (auto& d : one)
            d.extent = 1;
          auto changed = take(Footprint::from_regions(shape, {Region(one)}));
          auto dirty = take(output.dependencies.potential_dirty(
              "source", changed, 1, {}, ResultSupportTarget::Tensor, 0));
          require(dirty.at("result") == changed, "identity dirty projection");
          auto association = result.association();
          require(std::find(association.begin(), association.end(),
                            f.bindings.inputs[0].result.object_id()) !=
                      association.end(),
                  "source Result association retained");
          require((owner(result, Region(one)) ==
                   owner(f.bindings.inputs[0].result, Region(one))) ==
                      (std::string(policy) != "materialize"),
                  "batch plane view owner contract");
        }
      }
      auto edge = take(format::assign_metadata(f.document, f.input));
      auto plan = take(f.build(edge));
      auto frozen = take(f.context->freeze(plan.plan, f.bindings));
      const auto root = take(f.context->resource_budget());
      const auto before = root.statistics().peak[ResourceKind::Payload];
      auto empty = take(f.context->execute_fragments(
          frozen, {{"result", take(Footprint::none(shape))}}));
      require(take(empty.results.at("result").descriptor())
                      .tensor_coverage(0)
                      .empty() &&
                  root.statistics().peak[ResourceKind::Payload] == before,
              "Empty has no Need or continuation/sample Payload");
    }
  }
  Fixture assertion({ElementType::UInt8, {4}});
  auto edge =
      take(format::assign_metadata(assertion.document, assertion.input));
  auto& params = assertion.document.nodes.back().parameters;
  const auto digest = format::detail::schema_assertion(assertion.schema);
  const auto layout =
      format::detail::layout_assertion(assertion.schema.tensors[0].layout);
  params["expected_source"] = "result-v1:" + std::to_string(digest.size()) +
                              ":" + digest + ":" +
                              std::to_string(layout.size()) + ":" + layout;
  assertion.oracle(assertion.run(edge), Region::whole({4}));
  params["expected_source"] = std::string("stale");
  require(!assertion.build(edge).ok(),
          "static source metadata assertion rejects drift");
  // Independent backing fragments remain a zero-copy Result view partition.
  const auto root = take(assertion.context->resource_budget());
  auto source =
      take(ResultBuilder::start(root, assertion.schema, "fragmented"));
  take(source.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  for (std::uint64_t first : {0, 2}) {
    auto storage = take(root.allocator().allocate(2));
    std::memcpy(storage.data(), assertion.raw.data() + first, 2);
    take(source.publish_tensor(
        0, Region({{first, 2}}), {0, {1}, {first}}, std::move(storage).freeze(),
        take(ResultRelation::cartesian(root, 4, {0, 1, 0, 0})),
        {true, true, true, true}));
  }
  assertion.bindings.inputs[0].result = take(source.seal());
  params.erase("expected_source");
  params["layout"] = std::string("view");
  auto plan = take(assertion.build(edge));
  auto fragmented =
      take(assertion.context->execute(plan.plan, assertion.bindings));
  assertion.oracle(fragmented, Region::whole({4}));
  for (std::uint64_t first : {0, 2}) {
    const Region piece({{first, 2}});
    require(owner(fragmented.results.at("result"), piece) ==
                owner(assertion.bindings.inputs[0].result, piece),
            "fragmented metadata view retains each independent backing owner");
  }
  params["layout"] = std::string("auto");
  assertion.oracle(assertion.run(edge), Region::whole({4}));
  // Admit source plus the one-byte continuation, but no materialized output.
  ResourceLimits payload_limit;
  payload_limit.capacity[ResourceKind::Payload] = 5;
  Fixture capacity({ElementType::UInt8, {4}}, {}, false, false, {},
                   payload_limit);
  format::MetadataOptions view;
  view.layout = "view";
  capacity.oracle(capacity.run(take(format::assign_metadata(
                      capacity.document, capacity.input, view))),
                  Region::whole({4}));
  view.layout = "materialize";
  auto copy_edge =
      take(format::assign_metadata(capacity.document, capacity.input, view));
  auto copy_plan = take(capacity.build(copy_edge));
  auto exhausted = capacity.context->execute(copy_plan.plan, capacity.bindings);
  require(!exhausted.ok() &&
              exhausted.status().code == ErrorCode::ResourceExhausted &&
              take(capacity.context->resource_budget())
                      .statistics()
                      .live[ResourceKind::Payload] == 4,
          "materialization capacity failure rolls back to source-only Payload");
  // A window keeps the source backing alive after every Result/context retires.
  std::optional<ResultTensorReadWindow> surviving;
  ResourceBudget retained;
  std::array<std::uint8_t, 4> expected{};
  {
    Fixture f({ElementType::UInt8, {4}});
    retained = take(f.context->resource_budget());
    auto result = f.run(take(format::assign_metadata(f.document, f.input)));
    const auto& value = result.results.at("result");
    surviving.emplace(take(
        value.acquire_tensor(take(value.descriptor()), 0, Region::whole({4}))));
    std::copy_n(f.raw.begin(), 4, expected.begin());
  }
  require(
      retained.statistics().live[ResourceKind::Payload] == 4 &&
          !std::memcmp(take(surviving->row_run({0})).data, expected.data(), 4),
      "owning window survives context and Result teardown");
  surviving.reset();
  require(retained.statistics().live[ResourceKind::Payload] == 0,
          "last view window releases backing");
  std::cout << "Result batches, exact roles/dirty mapping, Empty, views and "
               "lifetime passed\n";
}
void model_coordinate_edits() {
  for (unsigned location = 0; location < 4; ++location) {
    TensorDescription description;
    std::string path = "/semantic/coordinates";
    ValueDescriptor descriptor{ElementType::Float32, {2, 3, 1}};
    if (location == 1 || location == 3) {
      description.channel_axis = 2;
      description.channels = {{"Y", "gray", "relative"}};
      if (location == 1) {
        description.channels[0].interpretation.emplace();
        path = "/semantic/channels/index:0/interpretation/coordinates";
      } else {
        TensorColorGroup group;
        group.name = "gray";
        group.indices = {0};
        group.components = description.channels;
        group.interpretation.model = "gray";
        description.groups = {group};
        path = "/semantic/groups/gray/interpretation/coordinates";
      }
    } else if (location == 2) {
      description.component = TensorChannelDescription{"Y", "gray", "relative"};
      description.component->interpretation.emplace();
      path = "/semantic/component/interpretation/coordinates";
    }
    const auto original = take(encode_tensor_description(description));
    Fixture fixture(descriptor, {original});
    const auto at = [location](const TensorDescription& d)
        -> std::optional<TensorModelCoordinates> {
      if (location == 1)
        return !d.channels.empty() && d.channels[0].interpretation
                   ? d.channels[0].interpretation->coordinates
                   : std::nullopt;
      if (location == 2)
        return d.component && d.component->interpretation
                   ? d.component->interpretation->coordinates
                   : std::nullopt;
      if (location == 3)
        return d.groups.empty() ? std::nullopt
                                : d.groups[0].interpretation.coordinates;
      return d.coordinates;
    };
    const TensorModelCoordinates initial{"relative", "1931-2", "linear_y",
                                         std::array<double, 2>{.25, .25}};
    format::MetadataOptions options;
    options.set = {{path, initial}};
    auto edge =
        take(format::assign_metadata(fixture.document, fixture.input, options));
    auto result = fixture.run(edge);
    require(
        at(decoded(facets(result))) && *at(decoded(facets(result))) == initial,
        "assign native coordinate subtree at every scope");
    fixture.oracle(result, Region::whole(descriptor.shape));
    options.set = {{path + "/scale", std::string("absolute")},
                   {path + "/observer", std::string("1964-10")},
                   {path + "/gray_kind", std::string("oklab_l")},
                   {path + "/ncl_coefficients", std::array<double, 2>{.3, .2}}};
    edge = take(format::assign_metadata(fixture.document, edge, options));
    result = fixture.run(edge);
    const auto changed = *at(decoded(facets(result)));
    require(changed.scale == "absolute" && changed.observer == "1964-10" &&
                changed.gray_kind == "oklab_l" &&
                changed.ncl_coefficients == std::array<double, 2>{.3, .2},
            "patch all native coordinate leaves");
    edge = take(
        format::remove_metadata(fixture.document, edge, {path + "/observer"}));
    result = fixture.run(edge);
    require(at(decoded(facets(result)))->observer.empty() &&
                at(decoded(facets(result)))->gray_kind == "oklab_l",
            "leaf removal keeps independent coordinate assertions");
    edge = take(format::remove_metadata(fixture.document, edge, {path}));
    result = fixture.run(edge);
    require(!at(decoded(facets(result))), "remove coordinate subtree");
    // An entirely empty semantic description may be removed as a facet.
    // Its codec representation is still canonical v4, not a spurious TDM5.
    require(
        take(encode_tensor_description(decoded(facets(result)))).version == 4,
        "removing last coordinate record returns to canonical v4");
    require(
        fixture.bindings.inputs[0].result.schema().tensors[0].facets.size() ==
                1 &&
            fixture.bindings.inputs[0]
                    .result.schema()
                    .tensors[0]
                    .facets[0]
                    .payload == original.payload,
        "coordinate edits preserve source metadata");
    fixture.oracle(result, Region::whole(descriptor.shape));
  }
  std::cout
      << "TDM5 subtree/leaf assignment and removal at all four scopes passed\n";
}
}  // namespace
int main() try {
  result_contract();
  model_coordinate_edits();
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
