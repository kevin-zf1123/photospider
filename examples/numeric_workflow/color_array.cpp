#include "photospider/data/color_array.hpp"

#include <cfenv>  // NOLINT(build/c++11)
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/numeric/unary.hpp"
#include "photospider/photospider.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ps::ColorArrayDescriptor description(ps::ColorModel model) {
  ps::ColorArrayDescriptor s;
  s.model = model;
  if (model == ps::ColorModel::Rgb || model == ps::ColorModel::Hsl ||
      model == ps::ColorModel::Ycbcr) {
    s.primaries =
        take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
            .primaries;
    s.transfer = ps::ColorTransfer{};
  }
  if (model == ps::ColorModel::Cielab || model == ps::ColorModel::Cielch)
    s.white = ps::color_white_d50();
  if (model == ps::ColorModel::Cielch || model == ps::ColorModel::Oklch ||
      model == ps::ColorModel::Hsl)
    s.hue = ps::ColorHueUnit::PiMultiple;
  if (model == ps::ColorModel::Ycbcr)
    s.ncl_coefficients =
        take(ps::color_ncl_coefficients(ps::ColorNclPreset::Bt709));
  if (model == ps::ColorModel::Cmyk) {
    s.white.reset();
    s.reference = ps::ColorReference::ProfileRelative;
    s.profile = ps::ColorProfileIdentity{1024, {}};
  }
  return s;
}
void codec() {
  const ps::ColorArrayDescriptor xyz;
  require(take(ps::color_array_parameter(xyz)) ==
              "0202000088635ddc4603d43f75931804560ed53f",
          "independent XYZ byte fixture");
  for (unsigned model = 1; model <= 9; ++model) {
    const auto s = description(static_cast<ps::ColorModel>(model));
    const auto encoded = take(ps::encode_color_array(s));
    const auto parameter = take(ps::color_array_parameter(s));
    require(take(ps::encode_color_array(take(ps::decode_color_array(encoded))))
                    .payload == encoded.payload,
            "model facet round trip");
    require(take(ps::color_array_parameter(
                take(ps::color_array_from_parameter(parameter)))) == parameter,
            "model static round trip");
    for (std::size_t size = 0; size < encoded.payload.size(); ++size) {
      auto truncated = encoded;
      truncated.payload.resize(size);
      require(!ps::decode_color_array(truncated).ok(),
              "every truncation rejected");
    }
    auto bad = encoded;
    bad.payload.push_back(0);
    require(!ps::decode_color_array(bad).ok(), "trailing bytes rejected");
    bad = encoded;
    bad.version = 2;
    require(!ps::decode_color_array(bad).ok(), "unknown version rejected");
    bad = encoded;
    bad.key = "photospider.semantic";
    require(!ps::decode_color_array(bad).ok(), "legacy key rejected");
    for (std::size_t tag = 0; tag < 4; ++tag) {
      bad = encoded;
      bad.payload[tag] = 255;
      require(!ps::decode_color_array(bad).ok(), "unknown prefix rejected");
    }
    for (auto type : {ps::ElementType::Float32, ps::ElementType::Float64}) {
      const std::uint64_t c = model == 9 ? 4 : 3;
      for (unsigned rank = 2; rank <= 8; ++rank) {
        std::vector<std::uint64_t> shape(rank, 1);
        shape.back() = c;
        require(ps::validate_color_array_descriptor(s, {type, shape}).ok(),
                "all color ranks");
      }
      require(!ps::validate_color_array_descriptor(s, {type, {c}}).ok(),
              "one color uses rank two");
      require(!ps::validate_color_array_descriptor(s, {type, {1, c + 1}}).ok(),
              "channel mismatch");
      require(
          !ps::validate_color_array_descriptor(s, {type, {1ULL << 40, c}}).ok(),
          "logical product limit");
    }
    require(!ps::validate_color_array_descriptor(
                 s, {ps::ElementType::Int64, {1, model == 9 ? 4U : 3U}})
                 .ok(),
            "integer colors rejected");
  }
  for (auto preset :
       {ps::ColorPrimaryPreset::Srgb, ps::ColorPrimaryPreset::DisplayP3,
        ps::ColorPrimaryPreset::Rec2020, ps::ColorPrimaryPreset::AdobeRgb1998,
        ps::ColorPrimaryPreset::ProphotoRgb, ps::ColorPrimaryPreset::AcesAp0,
        ps::ColorPrimaryPreset::AcesAp1}) {
    auto s = description(ps::ColorModel::Rgb);
    const auto p = take(ps::color_primary_coordinates(preset));
    s.primaries = p.primaries;
    s.white = p.white;
    require(ps::encode_color_array(s).ok(), "all primary/white presets");
  }
  auto s = description(ps::ColorModel::Rgb);
  s.primaries = std::array<double, 6>{1, 0, 0, 1, 0, 0};
  auto negative_zero = s;
  (*negative_zero.primaries)[1] = -0.0;
  require(take(ps::encode_color_array(s)).payload ==
              take(ps::encode_color_array(negative_zero)).payload,
          "metadata signed zero canonicalized");
  auto bad = take(ps::encode_color_array(s));
  bad.payload[35] |= 128;
  require(!ps::decode_color_array(bad).ok(), "stored minus zero rejected");
  s.primaries = std::array<double, 6>{1, 0, 1, 0, 0, 0};
  require(!ps::encode_color_array(s).ok(), "singular primaries rejected");
  s = description(ps::ColorModel::Rgb);
  s.transfer->kind = ps::ColorTransferKind::Gamma;
  require(!ps::encode_color_array(s).ok(), "gamma exponent required");
  s.transfer->gamma = -1;
  require(!ps::encode_color_array(s).ok(), "gamma positive");
  s.transfer->gamma = std::numeric_limits<double>::denorm_min();
  require(ps::encode_color_array(s).ok(), "positive subnormal gamma accepted");
  s.transfer->kind = ps::ColorTransferKind::Srgb;
  require(!ps::encode_color_array(s).ok(), "irrelevant gamma rejected");
  s = description(ps::ColorModel::Oklab);
  s.white = ps::color_white_d50();
  require(!ps::encode_color_array(s).ok(), "OKLab fixed D65");
  for (auto model :
       {ps::ColorModel::Cielch, ps::ColorModel::Oklch, ps::ColorModel::Hsl}) {
    s = description(model);
    s.source_layout = ps::ColorSourceLayout::RationalHueSplit;
    s.hue = ps::ColorHueUnit::RationalPi;
    auto parameter = take(ps::color_array_parameter(s));
    require(take(ps::color_array_from_parameter(parameter)).source_layout ==
                s.source_layout,
            "static rational source round trip");
    require(!ps::encode_color_array(s).ok() &&
                !ps::validate_color_array_descriptor(
                     s, {ps::ElementType::Float64, {1, 3}})
                     .ok(),
            "rational source is not runtime ColorArray");
    s.source_layout = ps::ColorSourceLayout::Interleaved;
    require(!ps::color_array_parameter(s).ok(), "layout/hue contradiction");
  }
  require(!ps::color_array_from_parameter("AA").ok() &&
              !ps::color_array_from_parameter("0").ok() &&
              !ps::color_array_from_parameter(std::string(8194, '0')).ok(),
          "static hex limits");
  std::cout << "color codec: nine models, seven primary presets, exact XYZ "
               "bytes, static/runtime separation PASS\n";
}
ps::Value samples(const std::vector<double>& values, bool narrow = false,
                  bool reverse = false, bool broadcast = false) {
  const auto c = values.size();
  const auto width = narrow ? 4U : 8U;
  std::vector<std::uint8_t> bytes(1 + width * c);
  for (std::size_t i = 0; i < c; ++i) {
    if (narrow) {
      const float value = values[i];
      std::memcpy(bytes.data() + 1 + i * width, &value, width);
    } else {
      std::memcpy(bytes.data() + 1 + i * width, &values[i], width);
    }
  }
  return take(ps::Value::create(
      {narrow ? ps::ElementType::Float32 : ps::ElementType::Float64, {1, c}},
      ps::Region::whole({1, c}),
      {1 + (reverse ? (c - 1) * width : 0),
       {0, broadcast ? 0
           : reverse ? -static_cast<std::int64_t>(width)
                     : width}},
      std::move(bytes)));
}
void sample_domains() {
  for (bool narrow : {false, true}) {
    for (auto model :
         {ps::ColorModel::Rgb, ps::ColorModel::Xyz, ps::ColorModel::Cielab,
          ps::ColorModel::Cielch, ps::ColorModel::Oklab, ps::ColorModel::Oklch,
          ps::ColorModel::Hsl, ps::ColorModel::Ycbcr, ps::ColorModel::Cmyk}) {
      auto s = description(model);
      std::vector<double> valid = model == ps::ColorModel::Cmyk
                                      ? std::vector<double>{0, .25, 1, -0.0}
                                      : std::vector<double>{-12, 2, 42};
      require(ps::validate_color_array_value(s, samples(valid, narrow)).ok(),
              "extended model samples");
      for (std::size_t c = 0; c < valid.size(); ++c) {
        auto invalid = valid;
        invalid[c] = std::numeric_limits<double>::infinity();
        require(
            !ps::validate_color_array_value(s, samples(invalid, narrow)).ok(),
            "all channels finite");
      }
      if (model == ps::ColorModel::Cielch || model == ps::ColorModel::Oklch) {
        valid[1] = -1;
        require(!ps::validate_color_array_value(s, samples(valid, narrow)).ok(),
                "chroma nonnegative");
        valid[1] = -0.0;
        require(ps::validate_color_array_value(s, samples(valid, narrow)).ok(),
                "zero chroma keeps original hue");
      }
    }
    auto rgb = description(ps::ColorModel::Rgb);
    rgb.association = ps::ColorAssociation::Straight;
    require(ps::validate_color_array_value(rgb, samples({-5, 2, 1, 0}, narrow))
                .ok(),
            "straight hidden color legal");
    rgb.association = ps::ColorAssociation::Premultiplied;
    auto failure = ps::validate_color_array_value(
        rgb, samples({-5, 2, 1, 0}, narrow), ps::ErrorCode::OperationFailed);
    require(failure.code == ps::ErrorCode::OperationFailed &&
                failure.reason == ps::FailureReason::InvalidAssociation,
            "premultiplied alpha-zero association category");
    require(
        ps::validate_color_array_value(rgb, samples({-0.0, 0, 0, -0.0}, narrow))
            .ok(),
        "premultiplied signed zero legal");
    require(!ps::validate_color_array_value(rgb, samples({0, 0, 0, 2}, narrow))
                 .ok(),
            "alpha bounded");
    auto generic = description(ps::ColorModel::Xyz);
    require(ps::validate_color_array_value(generic,
                                           samples({1, -2, 3}, narrow, true))
                .ok(),
            "negative unaligned stride");
    require(ps::validate_color_array_value(
                generic, samples({-2, 3, 4}, narrow, false, true))
                .ok(),
            "zero strides");
    auto partial =
        take(samples({1, 2, 3}, narrow).view(ps::Region({{0, 1}, {1, 1}})));
    require(ps::validate_color_array_value(generic, partial).code ==
                ps::ErrorCode::TypeMismatch,
            "partial colors rejected");
  }
  std::cout << "color samples: full tuples, arbitrary strides, extended "
               "ranges, alpha/chroma domains PASS\n";
}
void environment() {
  const auto value =
      samples({std::numeric_limits<double>::denorm_min(), -0.0, 1});
  const auto s = description(ps::ColorModel::Rgb);
  const auto expected = take(ps::color_array_parameter(s));
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    std::fesetround(mode);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_INEXACT);
    const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
    require(take(ps::color_array_parameter(s)) == expected,
            "metadata fenv independent");
    require(ps::validate_color_array_value(s, value).ok(),
            "sample subnormal survives");
    require(
        std::fegetround() == mode && std::fetestexcept(FE_ALL_EXCEPT) == flags,
        "fenv restored");
    auto cancelled =
        ps::validate_color_array_value(s, value, ps::ErrorCode::OperationFailed,
                                       [] { return ps::ErrorCode::Cancelled; });
    require(cancelled.code == ps::ErrorCode::Cancelled &&
                std::fegetround() == mode &&
                std::fetestexcept(FE_ALL_EXCEPT) == flags,
            "cancel/fenv preserved");
  }
  std::fesetenv(FE_DFL_ENV);
  std::cout << "color environment: four rounding modes, subnormals, flags, "
               "cancellation PASS\n";
}
ps::Value colors(const std::vector<double>& samples) {
  auto s = description(ps::ColorModel::Rgb);
  s.association = ps::ColorAssociation::Straight;
  std::vector<std::uint8_t> bytes(samples.size() * 8);
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  return take(ps::Value::create(
      {ps::ElementType::Float64, {2, 2, 4}}, ps::Region::whole({2, 2, 4}),
      {0, {64, 32, 8}}, std::move(bytes), {take(ps::encode_color_array(s))}));
}
void public_workflow() {
  const std::vector<double> samples{-2,  3,  4,  1, -8,  9,  10, 1,
                                    -12, 13, 14, 1, -16, 17, 18, 1};
  const auto input = colors(samples);
  const auto roi = take(ps::Footprint::from_regions(
      {2, 2, 4}, {ps::Region({{1, 1}, {0, 1}, {0, 1}})}));
  const auto complete = take(ps::Footprint::from_regions(
      {2, 2, 4}, {ps::Region({{1, 1}, {0, 1}, {0, 4}})}));
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  document.inputs = {{1, "colors", input.descriptor(), input.region(),
                      input.layout(), input.facets()}};
  document.nodes = {
      take(ps::numeric::abs_node(1, ps::WorkflowInputReference{1}))};
  document.outputs = {{"absolute", 1, "values"}};
  ps::GraphContext graph(document);
  const auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 10000000;
  auto snapshot = take(context.freeze(compiled.plan, {{{"colors", input}}}));
  for (bool joint : {false, true}) {
    options.enable_joint = joint;
    auto result = take(
        context.execute_fragments(snapshot, {{"absolute", roi}}, {}, options));
    double actual = 0;
    require(result.values.at("absolute").read({1, 0, 0}, &actual, 8).ok() &&
                actual == 12,
            "public generic NUM consumes rank-three ColorArray");
    require(result.values.at("absolute").facets().empty(),
            "generic NUM drops color facet");
    require(take(result.dependencies.source_support()).at("colors") == complete,
            "source support includes full color only");
    const auto alpha = take(ps::Footprint::from_regions(
        {2, 2, 4}, {ps::Region({{1, 1}, {0, 1}, {3, 1}})}));
    require(take(result.dependencies.potential_dirty("colors", alpha))
                    .at("absolute") == roi,
            "unread alpha validation participates in dirty");
  }
  auto invalid = samples;
  invalid[3] = 2;  // Undemanded color stays unread.
  snapshot =
      take(context.freeze(compiled.plan, {{{"colors", colors(invalid)}}}));
  require(context.execute_fragments(snapshot, {{"absolute", roi}}, {}, options)
              .ok(),
          "unrelated invalid color remains unread");
  invalid = samples;
  invalid[11] = 2;
  snapshot =
      take(context.freeze(compiled.plan, {{{"colors", colors(invalid)}}}));
  require(!context.execute_fragments(snapshot, {{"absolute", roi}}, {}, options)
               .ok(),
          "selected color alpha rejected through generic consumer");
  ps::InputSnapshotStoreConfig store_config;
  store_config.block_size = 1;
  ps::InputSnapshotStore store(store_config);
  const auto imported = take(store.import_value(input));
  std::array<double, 4> pixel{};
  require(imported.read(complete.boxes()[0],
                        reinterpret_cast<std::uint8_t*>(pixel.data()), 32)
                  .ok() &&
              pixel == std::array<double, 4>{-12, 13, 14, 1},
          "snapshot keeps complete channel blocks");
  require(!imported
               .read(roi.boxes()[0],
                     reinterpret_cast<std::uint8_t*>(pixel.data()), 8)
               .ok(),
          "snapshot rejects partial color read");
  auto all = take(
      ps::ValueFragments::create(input.descriptor(), input.facets(),
                                 take(ps::Footprint::all({2, 2, 4})), {input}));
  require(!all.restrict(roi).ok() && all.restrict(complete).ok(),
          "fragment boundary keeps complete colors");
  std::cout << "color workflow: abs(-12)=12, local Data/full-color Validation, "
               "dirty/cache, joint, snapshot PASS\n";
}
struct ProofProbe {
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase&) {
    ps::DependencyNeedBatch need;
    need.static_mapping = true;
    return ps::Result<ps::DependencyPoll>(std::move(need));
  }
};
struct HistoryProbe {
  unsigned stage = 0;
  bool wrong = false;
  explicit HistoryProbe(bool mismatch) : wrong(mismatch) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    ps::DependencyNeedBatch need;
    ++stage;
    for (unsigned row = 0; row < 2; ++row) {
      auto samples = take(ps::Footprint::from_regions(
          {2, 3}, {ps::Region({{stage == 2 && wrong ? 0 : row, 1}, {0, 3}})}));
      need.associations.push_back(
          {{row}, {{0, stage == 1 ? 4U : 1U, std::move(samples), {}}}});
    }
    static_cast<void>(phase);
    return ps::Result<ps::DependencyPoll>(std::move(need));
  }
};
struct JointPartialProbe {
  ps::Result<std::vector<ps::DependencyAtomOutcome>> poll(
      const ps::DependencyJointPhase& phase) {
    std::vector<ps::DependencyAtomOutcome> outcomes;
    for (const auto* member : phase.members) {
      const auto key = take(ps::dependency_atom_key(member->query));
      ps::DependencyNeedBatch need;
      // First member's envelope is valid; second has only partial Validation.
      auto samples = take(ps::Footprint::from_regions(
          {2, 3}, {ps::Region({{key.coordinate[0], 1},
                               {0, key.coordinate[0] ? 1U : 3U}})}));
      need.associations.push_back(
          {{key.coordinate[0]}, {{0, 4, std::move(samples), {}}}});
      outcomes.push_back(
          {key, ps::Result<ps::DependencyPoll>(std::move(need))});
    }
    return ps::Result<std::vector<ps::DependencyAtomOutcome>>(
        std::move(outcomes));
  }
};
void history_and_joint() {
  const auto facet =
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)));
  for (bool wrong : {false, true}) {
    auto registry = std::make_shared<ps::OperationRegistry>();
    ps::OperationDefinition operation;
    operation.key = "manual.color_history";
    operation.traits.input_count = 1;
    operation.traits.input_schema.resize(1);
    auto& output = operation.traits.outputs[0];
    output.shape_rule = ps::OperationShapeRule::Fixed;
    output.fixed_output_shape = {2};
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.regional_atomic = true;
    output.continuation_bytes = sizeof(HistoryProbe);
    output.maximum_dependency_stages = 3;
    operation.start_dependency = [wrong](const auto&, const auto& allocator) {
      return ps::DependencyContinuation::make<HistoryProbe>(allocator, wrong);
    };
    auto joint_operation = operation;
    joint_operation.key = "manual.color_joint";
    joint_operation.traits.outputs[0].regional_atomic = false;
    joint_operation.traits.outputs[0].failure_delivery =
        ps::FailureDelivery::PerAtomOutcome;
    joint_operation.traits.joint_contract = 2;
    joint_operation.traits.joint_continuation_bytes = sizeof(JointPartialProbe);
    joint_operation.start_joint = [](const auto&, const auto& allocator) {
      return ps::DependencyJointContinuation::make<JointPartialProbe>(
          allocator);
    };
    auto registered = registry->register_operation(std::move(operation));
    if (!registered.ok())
      throw std::runtime_error("register history probe: " + registered.message);
    registered = registry->register_operation(std::move(joint_operation));
    if (!registered.ok())
      throw std::runtime_error("register joint probe: " + registered.message);
    require(registry->freeze().ok(), "freeze history probe");
    ps::DependencyRequest request;
    request.inputs = {{{ps::ElementType::Float64, {2, 3}}, {facet}}};
    request.outputs = take(ps::Footprint::all({2}));
    request.snapshot_identity = "color-history";
    auto session =
        take(registry->start_dependency("manual.color_history", request));
    require(session->poll().ok(), "history first Validation");
    auto input = take(ps::Value::create(
        {ps::ElementType::Float64, {2, 3}}, ps::Region::whole({2, 3}),
        {0, {24, 8}}, std::vector<std::uint8_t>(48), {facet}));
    auto fragments = take(
        ps::ValueFragments::create(input.descriptor(), input.facets(),
                                   take(ps::Footprint::all({2, 3})), {input}));
    require(session->supply({fragments}, request.snapshot_identity).ok(),
            "history Validation supply");
    auto next = session->poll();
    require(next.ok() != wrong, "same-atom prior Validation only");
    if (wrong)
      require(next.status().detail.origin == ps::FailureOrigin::Protocol,
              "cross-atom history cannot satisfy closure");
    auto second = request;
    request.outputs =
        take(ps::Footprint::from_regions({2}, {ps::Region({{0, 1}})}));
    second.outputs =
        take(ps::Footprint::from_regions({2}, {ps::Region({{1, 1}})}));
    auto joint =
        take(registry->start_joint("manual.color_joint", {request, second}));
    auto replies = joint->poll();
    require(
        !replies.ok() &&
            replies.status().detail.origin == ps::FailureOrigin::Protocol &&
            replies.status().detail.scope == ps::FailureScope::Group,
        "joint partial transport fails entire group before any member commit");
  }
  std::cout << "color protocol: historical Validation, cross-atom isolation, "
               "joint transport preflight PASS\n";
}
void grouping_schema() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::OperationDefinition operation;
  operation.key = "manual.color_grouping";
  operation.traits.input_count = 0;
  operation.traits.input_schema.clear();
  auto& output = operation.traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::Fixed;
  output.fixed_output_shape = {2, 2, 3};
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ProofProbe);
  output.maximum_dependency_stages = 2;
  output.atomic_trailing_axes = 2;
  output.output_semantic_rule = ps::OperationSemanticRule::Establish;
  output.output_facets = {
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)))};
  operation.start_dependency = [](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<ProofProbe>(allocator);
  };
  require(registry->register_operation(std::move(operation)).ok() &&
              registry->freeze().ok(),
          "register invalid grouping template");
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.color_grouping", {}, {}}};
  document.outputs = {{"colors", 1, "value"}};
  ps::GraphContext graph(document);
  auto compiled = ps::Compiler(registry).compile(graph);
  require(
      !compiled.ok() && compiled.status().code == ps::ErrorCode::TypeMismatch,
      "compile rejects ColorArray grouping greater than one");
  ps::DependencyRequest request;
  request.outputs = take(ps::Footprint::all({2, 2, 3}));
  request.snapshot_identity = "bad-grouping";
  auto direct = registry->start_dependency("manual.color_grouping", request);
  require(!direct.ok() && direct.status().code == ps::ErrorCode::TypeMismatch,
          "direct grouping rejection agrees with compile");
  std::cout << "color grouping: compiler/direct schema parity PASS\n";
}
void mapping_proof() {
  const auto facet =
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)));
  for (bool wrong_axis : {false, true}) {
    auto registry = std::make_shared<ps::OperationRegistry>();
    ps::OperationDefinition operation;
    operation.key = "manual.color_proof";
    operation.traits.input_count = 1;
    operation.traits.input_schema.resize(1);
    auto& output = operation.traits.outputs[0];
    output.shape_rule = ps::OperationShapeRule::Fixed;
    output.fixed_output_shape = {2, 2};
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(ProofProbe);
    output.maximum_dependency_stages = 2;
    // Split Validation maps must be accepted jointly. Wrong-axis coverage has
    // the same batch fetch union, but cannot validate atom (0,1)'s data row.
    output.static_dependency_pieces = std::vector<ps::DependencyMapPiece>{
        {take(ps::Footprint::all({2, 2})),
         {{0, 1, {{0, {}}, {-1, {0, 1}}}, {}},
          {0, 4, {{wrong_axis ? 1 : 0, {}}, {-1, {0, 1}}}, {}},
          {0, 4, {{wrong_axis ? 1 : 0, {}}, {-1, {1, 2}}}, {}}}}};
    operation.start_dependency = [](const auto&, const auto& allocator) {
      return ps::DependencyContinuation::make<ProofProbe>(allocator);
    };
    require(registry->register_operation(std::move(operation)).ok() &&
                registry->freeze().ok(),
            "register color proof fixture");
    ps::DependencyRequest request;
    request.inputs = {{{ps::ElementType::Float64, {2, 3}}, {facet}}};
    request.outputs = take(ps::Footprint::all({2, 2}));
    request.snapshot_identity = "color-proof";
    auto session =
        take(registry->start_dependency("manual.color_proof", request));
    auto status = session->poll();
    require(status.ok() != wrong_axis,
            "per-observation proof distinguishes cross-atom Validation");
    if (wrong_axis) {
      require(status.status().detail.origin == ps::FailureOrigin::Protocol,
              "missing closure protocol origin");
      request.outputs = take(
          ps::Footprint::from_regions({2, 2}, {ps::Region({{0, 1}, {0, 1}})}));
      session = take(registry->start_dependency("manual.color_proof", request));
      require(session->poll().ok(),
              "bounded row fallback accepts different skeleton on singleton "
              "domain");
    }
  }
  std::cout << "color dependency proof: split Validation, cross-atom "
               "rejection, exact fallback PASS\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--metadata") {
      for (std::string line; std::getline(std::cin, line);)
        std::cout << (ps::color_array_from_parameter(line).ok() ? "OK" : "ERR")
                  << '\n';
      return 0;
    }
    codec();
    sample_domains();
    environment();
    public_workflow();
    mapping_proof();
    history_and_joint();
    grouping_schema();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
