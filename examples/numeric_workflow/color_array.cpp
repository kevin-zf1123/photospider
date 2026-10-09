#include "photospider/data/color_array.hpp"

#include <cfenv>  // NOLINT(build/c++11)
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/unary.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

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
  std::vector<std::uint8_t> bytes(samples.size() * 8);
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  return take(ps::Value::create({ps::ElementType::Float64, {2, 2, 4}},
                                ps::Region::whole({2, 2, 4}), {0, {64, 32, 8}},
                                std::move(bytes)));
}
namespace rf = numeric_result_fixture;
void public_workflow() {
  const std::vector<double> values{-2,  3,  4,  1, -8,  9,  10, 1,
                                   -12, 13, 14, 1, -16, 17, 18, 1};
  const auto input = colors(values);
  const auto roi = take(ps::Footprint::from_regions(
      {2, 2, 4}, {ps::Region({{1, 1}, {0, 1}, {0, 1}})}));
  const auto complete = take(ps::Footprint::from_regions(
      {2, 2, 4}, {ps::Region({{1, 1}, {0, 1}, {0, 4}})}));
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  rf::declare_sources(&document, {input});
  document.inputs[0].name = "colors";
  auto input_schema = *document.inputs[0].result_schema;
  auto color = description(ps::ColorModel::Rgb);
  color.association = ps::ColorAssociation::Straight;
  input_schema.tensors[0].facets = {take(ps::encode_color_array(color))};
  input_schema.tensors[0].atomic_trailing_axes = 1;
  document.inputs[0].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(input_schema));
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
  auto root = take(context.resource_budget());
  ps::ExecutionOptions options;
  options.maximum_dependency_work = 10000000;
  auto bind = [&](const ps::Value& backing) {
    return point_math_checks::bindings(root, {backing}, document);
  };
  auto snapshot = take(context.freeze(compiled.plan, bind(input)));
  for (bool joint : {false, true}) {
    options.enable_joint = joint;
    auto result = take(
        context.execute_fragments(snapshot, {{"absolute", roi}}, {}, options));
    double actual = 0;
    require(
        rf::read(result.results.at("absolute"), {1, 0, 0}, &actual, 8).ok() &&
            actual == 12,
        "public generic NUM consumes rank-three ColorArray Result");
    require(result.results.at("absolute").schema().tensors[0].facets.empty(),
            "generic NUM drops color facet");
    require(take(result.dependencies.source_support()).at("colors") ==
                take(ps::Footprint::all({2, 2, 4})),
            "Whole source support includes all color tuples");
    const auto alpha = take(ps::Footprint::from_regions(
        {2, 2, 4}, {ps::Region({{1, 1}, {0, 1}, {3, 1}})}));
    auto dirty =
        take(result.dependencies.potential_dirty(
                 "colors", alpha, 4, {}, ps::ResultSupportTarget::Tensor, 0))
            .at("absolute");
    require(dirty == roi, "unread alpha validation participates in dirty");
  }
  for (unsigned alpha : {3U, 11U}) {
    auto invalid = values;
    invalid[alpha] = 2;
    snapshot = take(context.freeze(compiled.plan, bind(colors(invalid))));
    require(
        !context.execute_fragments(snapshot, {{"absolute", roi}}, {}, options)
             .ok(),
        "Whole consumer validates both selected and unselected colors");
  }
  auto source = point_math_checks::source(
      root, input, document.inputs[0].result_schema.get());
  auto facts = take(source.descriptor());
  auto window = take(source.acquire_tensor(facts, 0, complete.boxes()[0]));
  auto partial = take(source.acquire_tensor(facts, 0, roi.boxes()[0]));
  double selected = 0;
  std::memcpy(&selected, take(partial.row_run({1, 0, 0})).data, 8);
  require(selected == -12,
          "Result window retains an exact authorized component");
  auto rejected =
      take(ps::ResultBuilder::start(root, source.schema(), "partial.color"));
  require(rejected
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "partial source basis");
  require(!rejected
               .publish_tensor(
                   0, roi.boxes()[0],
                   {reinterpret_cast<const std::uint8_t*>(&selected), 8},
                   take(ps::ResultRelation::cartesian(root, 16, {})),
                   {true, true, true, true})
               .ok(),
          "ColorArray publication rejects an incomplete tuple");
  source = {};
  std::array<double, 4> pixel{};
  auto row = take(window.row_run({1, 0, 0}));
  for (unsigned channel = 0; channel < 4; ++channel)
    std::memcpy(&pixel[channel], row.data + channel * row.sample_stride_bytes,
                8);
  require(pixel == std::array<double, 4>{-12, 13, 14, 1},
          "owned Result window keeps complete tuple after source retirement");
  std::cout << "color workflow: abs(-12)=12, Whole typed validation, "
               "dirty/cache, joint, owned windows PASS\n";
}
struct ColorProbe {
  unsigned stage = 0;
  bool wrong = false, history = false;
  ColorProbe(bool mismatch, bool staged_history)
      : wrong(mismatch), history(staged_history) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto query = phase.query.tensor_outputs
                           ? *phase.query.tensor_outputs
                           : take(ps::Footprint::all(shape));
    if (stage < (history ? 2U : 1U)) {
      ps::ResultProgramNeed need;
      for (const auto& box : query.boxes()) {
        const auto row = box.dimensions()[0];
        const auto validation = history || !wrong ? row : box.dimensions()[1];
        if (!history || !stage)
          for (unsigned channel : {0U, 1U})
            need.tensors.push_back(
                {0, 0,
                 take(ps::Footprint::from_regions(
                     {2, 3},
                     {ps::Region({validation, {channel, channel ? 2U : 1U}})})),
                 4});
        if (!history || stage)
          need.tensors.push_back(
              {0, 0,
               take(ps::Footprint::from_regions(
                   {2, 3},
                   {ps::Region(
                       {history && wrong ? ps::RegionDimension{0, 1} : row,
                        {0, 1}})})),
               1});
      }
      ++stage;
      return ps::Result<ps::ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "color probe descriptor");
    for (const auto& box : query.boxes()) {
      const auto data_axis = history && wrong ? -1 : 0;
      const auto validation_axis = history || !wrong ? 0 : 1;
      std::vector<ps::ResultRelation> relations;
      relations.push_back(take(ps::ResultRelation::mapped(
          phase.resources, shape, box, {2, 3},
          {{data_axis, 0, 1, 1}, {-1, 0, 0, 1}},
          {0, 1, 0, 0, ps::ResultSupportTarget::Tensor, 0})));
      for (unsigned channel : {0U, 1U})
        relations.push_back(take(ps::ResultRelation::mapped(
            phase.resources, shape, box, {2, 3},
            {{validation_axis, 0, 1, 1}, {-1, channel, 0, channel ? 2U : 1U}},
            {0, 4, 0, 0, ps::ResultSupportTarget::Tensor, 0})));
      auto relation =
          take(ps::ResultRelation::unite(phase.resources, relations));
      std::vector<double> numbers(take(box.element_count()));
      auto samples = take(ps::Footprint::from_regions(shape, {box}));
      unsigned next = 0;
      require(samples
                  .visit(
                      [&](const auto& at) {
                        return phase.tensors->at({0, 0}).read(
                            {history && wrong ? 0 : at[0], 0}, &numbers[next++],
                            8);
                      },
                      16)
                  .ok(),
              "color probe reads only granted samples");
      require(builder
                  .publish_tensor(
                      0, box,
                      {reinterpret_cast<const std::uint8_t*>(numbers.data()),
                       numbers.size() * 8},
                      relation, {true, true, true, true})
                  .ok(),
              "color probe publication");
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
ps::SchemaTemplate probe_schema(
    const std::vector<std::uint64_t>& shape,
    const std::vector<ps::ValueFacet>& facets = {}) {
  ps::SchemaTemplate schema;
  schema.id = "manual.color_probe";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::Float64, shape};
  tensor.facets = facets;
  tensor.atomic_trailing_axes = facets.empty() ? 0 : 1;
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
ps::OperationDefinition probe_definition(const ps::SchemaTemplate& schema,
                                         bool wrong, bool history) {
  ps::OperationDefinition operation;
  operation.key = "manual.color_probe";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  operation.traits.input_schema[0].result_schema_id = "manual.color_probe";
  operation.traits.input_schema[0].result_schema_version = 1;
  auto& output = operation.traits.outputs[0];
  output.output_schema = operation.traits.input_schema[0];
  output.result_schema = schema;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(ColorProbe);
  output.maximum_dependency_stages = 3;
  operation.start_result = [wrong, history](const auto&,
                                            const auto& allocator) {
    return ps::ResultContinuation::make<ColorProbe>(allocator, wrong, history);
  };
  return operation;
}
ps::Result<ps::DemandResult> run_probe(bool wrong, bool history,
                                       const ps::Footprint& demand) {
  const auto facet =
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)));
  const auto input_schema = probe_schema({2, 3}, {facet});
  const auto output_schema =
      probe_schema(history ? std::vector<std::uint64_t>{2}
                           : std::vector<std::uint64_t>{2, 2});
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(
                      probe_definition(output_schema, wrong, history))
                  .ok() &&
              registry->freeze().ok(),
          "register color Result proof");
  ps::WorkflowDocument document;
  ps::WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "colors";
  input.result_schema = std::make_shared<ps::SchemaTemplate>(input_schema);
  document.inputs = {input};
  document.nodes = {
      {1, "manual.color_probe", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"values", 1, "value"}};
  ps::GraphContext graph(document);
  auto plan = ps::Compiler(registry).compile(graph);
  if (!plan.ok())
    return ps::Result<ps::DemandResult>(plan.status());
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  const auto backing = take(ps::Value::create(
      {ps::ElementType::Float64, {2, 3}}, ps::Region::whole({2, 3}),
      {0, {24, 8}}, std::vector<std::uint8_t>(48), {facet}));
  auto frozen = take(context.freeze(
      plan.value().plan,
      point_math_checks::bindings(take(context.resource_budget()), {backing},
                                  document)));
  return context.execute_fragments(frozen, {{"values", demand}});
}
struct TupleView {
  bool requested = false;
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    auto samples = phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                              : take(ps::Footprint::all(shape));
    if (!requested) {
      requested = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, samples, 5});
      return ps::Result<ps::ResultProgramPoll>(std::move(need));
    }
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key));
    require(builder
                .bind_descriptor_relation(take(ps::ResultRelation::cartesian(
                    phase.resources, 1,
                    {0, 8, 0, 1, ps::ResultSupportTarget::Descriptor, 0})))
                .ok(),
            "tuple view basis");
    ps::ResultTensorViewTransform transform;
    transform.source_axes = {{0, 0, 1, 1}, {1, 0, 1, 1}};
    for (const auto& box : samples.boxes()) {
      auto window = take(phase.tensors->at({0, 0}).acquire(box));
      auto relation = take(ps::ResultRelation::mapped(
          phase.resources, shape, box, shape, transform.source_axes,
          {0, 5, 0, 0, ps::ResultSupportTarget::Tensor, 0}));
      require(builder
                  .publish_tensor_view(0, box, window, transform, relation,
                                       {true, true, true, true})
                  .ok(),
              "typed tuple view publication");
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void tuple_output_view() {
  const auto facet =
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)));
  auto schema = probe_schema({2, 3}, {facet});
  auto operation = probe_definition(schema, false, false);
  operation.traits.outputs[0].continuation_bytes = sizeof(TupleView);
  operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<TupleView>(allocator);
  };
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(std::move(operation)).ok() &&
              registry->freeze().ok(),
          "register typed tuple view");
  ps::ResultRef held;
  ps::ResultTensorReadWindow original;
  {
    ps::WorkflowDocument document;
    ps::WorkflowInputDeclaration input;
    input.id = 1;
    input.name = "colors";
    input.result_schema = std::make_shared<ps::SchemaTemplate>(schema);
    document.inputs = {input};
    document.nodes = {
        {1, "manual.color_probe", {ps::WorkflowInputReference{1}}, {}}};
    document.outputs = {{"colors", 1, "value"}};
    ps::GraphContext graph(document);
    auto plan = take(ps::Compiler(registry).compile(graph)).plan;
    ps::ExecutionContext context(registry);
    auto root = take(context.resource_budget());
    const std::vector<double> values{1, 2, 3, 4, 5, 6};
    std::vector<std::uint8_t> bytes(48);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    auto backing = take(ps::Value::create({ps::ElementType::Float64, {2, 3}},
                                          ps::Region::whole({2, 3}),
                                          {0, {24, 8}}, std::move(bytes)));
    auto bindings = point_math_checks::bindings(root, {backing}, document);
    original = take(bindings.inputs[0].result.acquire_tensor(
        take(bindings.inputs[0].result.descriptor()), 0,
        ps::Region({{1, 1}, {0, 3}})));
    auto frozen = take(context.freeze(plan, std::move(bindings)));
    auto query = take(
        ps::Footprint::from_regions({2, 3}, {ps::Region({{1, 1}, {2, 1}})}));
    auto result = take(context.execute_fragments(frozen, {{"colors", query}}));
    held = result.results.at("colors");
  }
  auto alias = take(held.acquire_tensor(take(held.descriptor()), 0,
                                        ps::Region({{1, 1}, {0, 3}})));
  require(
      take(alias.row_run({1, 0})).data == take(original.row_run({1, 0})).data &&
          alias.storage_owner_token() == original.storage_owner_token(),
      "tuple observation retains zero-copy mapped backing after context "
      "retirement");
  double last = 0;
  require(rf::read(held, {1, 2}, &last, 8).ok() && last == 6,
          "typed mapped tuple remains readable");
  std::cout
      << "color tuple output: grouped Validation, owned zero-copy view PASS\n";
}
struct JointPartialProbe {
  unsigned mode;
  explicit JointPartialProbe(unsigned mode) : mode(mode) {}
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    ps::ResourceVector<ps::ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      const auto key = take(ps::result_atom_key(member->query));
      ps::ResultProgramNeed need;
      const auto row = key.coordinate[0];
      if (mode == 2) {
        for (unsigned channel : {0U, 1U})
          need.tensors.push_back(
              {0, 0,
               take(ps::Footprint::from_regions(
                   {2, 3},
                   {ps::Region({{row, 1}, {channel, channel ? 2U : 1U}})})),
               4});
      } else {
        need.tensors.push_back(
            {0, 0,
             take(ps::Footprint::from_regions(
                 {2, 3},
                 {ps::Region({{row, 1}, {0, mode == 0 && row ? 1U : 3U}})})),
             4});
      }
      outcomes.push_back(
          {key, ps::Result<ps::ResultProgramPoll>(std::move(need))});
    }
    return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
        std::move(outcomes));
  }
};
void joint_partial_validation(const ps::ValueFacet& facet, unsigned mode = 0) {
  ps::ResourceBudget root;
  ps::ResourceAllocationScope scope(root);
  ps::SchemaTemplate input_schema;
  input_schema.id = "manual.color_input";
  ps::ResultTensorSpec color;
  color.key = "color";
  color.descriptor = {ps::ElementType::Float64, {2, 3}};
  color.facets = {facet};
  input_schema.tensors.push_back(std::move(color));
  ps::SchemaTemplate output_schema;
  output_schema.id = "manual.color_output";
  ps::ResultTensorSpec number;
  number.key = "number";
  number.descriptor = {ps::ElementType::Float64, {2}};
  output_schema.tensors.push_back(std::move(number));
  ps::OperationDefinition operation;
  operation.key = "manual.color_joint";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  operation.traits.input_schema[0].result_schema_id = "manual.color_input";
  operation.traits.input_schema[0].result_schema_version = 1;
  auto& output = operation.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "manual.color_output";
  output.output_schema.result_schema_version = 1;
  output.result_schema = output_schema;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = 256;
  output.maximum_dependency_stages = 3;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  operation.traits.joint_contract = 2;
  operation.traits.joint_continuation_bytes = sizeof(JointPartialProbe);
  operation.start_result = [](const auto&, const auto&) {
    return ps::Result<ps::ResultContinuation>(
        ps::Status{ps::ErrorCode::Internal, "unexpected singleton"});
  };
  operation.start_result_joint = [mode](const auto&, const auto& allocator) {
    return ps::ResultJointContinuation::make<JointPartialProbe>(allocator,
                                                                mode);
  };
  ps::OperationRegistry registry;
  require(registry.register_operation(std::move(operation)).ok() &&
              registry.freeze().ok(),
          "register Result joint validation probe");
  ps::OperationMetadata input;
  input.result_schema =
      std::make_shared<const ps::SchemaTemplate>(input_schema);
  auto inferred = take(ps::infer_operation_outputs(
      take(registry.find_traits("manual.color_joint")), {input}, {}));
  ps::ResultProgramMetadata metadata{{input}, inferred[0]};
  std::map<std::string, ps::ParameterValue> parameters;
  ps::ResourceVector<ps::ResultProgramQuery> queries;
  for (unsigned i = 0; i < 2; ++i) {
    queries.emplace_back(metadata, parameters);
    queries.back().semantic_key = "color-joint";
    queries.back().snapshot_identity = "color-history";
    queries.back().tensor_outputs =
        take(ps::Footprint::from_regions({2}, {ps::Region({{i, 1}})}));
  }
  auto joint =
      take(registry.start_result_joint("manual.color_joint", queries, root));
  auto allocator = root.allocator();
  auto work = [&](std::uint64_t n) { return root.consume({n}); };
  ps::ResultObjectInputs inputs;
  ps::ResourceVector<ps::ResultIoReply> io;
  ps::ResourceVector<ps::ResultProgramPhase> phases;
  for (const auto& query : queries)
    phases.push_back({query, inputs, io, allocator, root, work, {}});
  ps::ResourceVector<const ps::ResultProgramPhase*> ready;
  for (const auto& phase : phases)
    ready.push_back(&phase);
  const auto payload_before = root.statistics().live[ps::ResourceKind::Payload];
  auto replies = joint.poll({ready, allocator, work});
  if (!mode)
    require(
        !replies.ok() &&
            replies.status().detail.origin == ps::FailureOrigin::Protocol &&
            replies.status().detail.scope == ps::FailureScope::Group,
        "joint partial transport fails entire group before any member commit");
  else
    require(replies.ok() && replies.value().size() == 2,
            "joint complete or split tuple transport accepted");
  require(root.statistics().live[ps::ResourceKind::Payload] == payload_before,
          "joint partial Validation publishes no payload");
}
void history_and_joint() {
  for (bool wrong : {false, true}) {
    auto batch = run_probe(wrong, true, take(ps::Footprint::all({2})));
    require(batch.ok() != wrong,
            "historical Validation remains per observation in a batch");
    if (wrong)
      require(batch.status().detail.origin == ps::FailureOrigin::Protocol,
              "batch cross-observation Validation rejection");
    for (unsigned row = 0; row < 2; ++row) {
      auto next = run_probe(
          wrong, true,
          take(ps::Footprint::from_regions({2}, {ps::Region({{row, 1}})})));
      require(next.ok() == (!wrong || !row),
              "same-observation prior Validation only");
      if (!next.ok())
        require(next.status().detail.origin == ps::FailureOrigin::Protocol,
                "cross-observation history cannot satisfy closure");
    }
  }
  const auto facet =
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)));
  for (unsigned mode = 0; mode < 3; ++mode)
    joint_partial_validation(facet, mode);
  std::cout << "color protocol: historical Validation, cross-observation "
               "isolation, joint transport preflight PASS\n";
}
void grouping_schema() {
  const auto facet =
      take(ps::encode_color_array(description(ps::ColorModel::Xyz)));
  for (unsigned grouping : {1U, 2U}) {
    auto registry = std::make_shared<ps::OperationRegistry>();
    const auto schema = probe_schema({2, 2, 3}, {facet});
    auto operation = probe_definition(schema, false, false);
    operation.traits.input_count = 0;
    operation.traits.input_schema.clear();
    operation.traits.requires_metadata_specialization = true;
    operation.specialize_metadata = [schema, grouping](const auto&,
                                                       const auto&) {
      ps::OperationOutputSpecialization specialization;
      auto result = schema;
      result.tensors[0].atomic_trailing_axes = grouping;
      specialization.metadata.result_schema =
          std::make_shared<ps::SchemaTemplate>(std::move(result));
      return ps::Result<std::vector<ps::OperationOutputSpecialization>>(
          std::vector<ps::OperationOutputSpecialization>{specialization});
    };
    require(registry->register_operation(std::move(operation)).ok() &&
                registry->freeze().ok(),
            "register grouping specializer");
    ps::WorkflowDocument document;
    document.nodes = {{1, "manual.color_probe", {}, {}}};
    document.outputs = {{"colors", 1, "value"}};
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph);
    auto direct = registry->prepare_operation("manual.color_probe", {}, {});
    require(compiled.ok() == (grouping == 1) && direct.ok() == compiled.ok(),
            "compiler/direct Result grouping parity");
    if (grouping == 2)
      require(compiled.status().code == ps::ErrorCode::TypeMismatch &&
                  direct.status().code == ps::ErrorCode::TypeMismatch,
              "ColorArray observation groups exactly one channel axis");
  }
  std::cout << "color grouping: compiler/direct schema parity PASS\n";
}
void mapping_proof() {
  const auto all = take(ps::Footprint::all({2, 2}));
  for (bool wrong : {false, true}) {
    auto result = run_probe(wrong, false, all);
    require(result.ok() != wrong,
            "per-observation proof distinguishes cross-observation Validation");
    if (wrong) {
      require(result.status().detail.origin == ps::FailureOrigin::Protocol,
              "missing closure protocol origin");
      require(run_probe(true, false,
                        take(ps::Footprint::from_regions(
                            {2, 2}, {ps::Region({{0, 1}, {0, 1}})})))
                  .ok(),
              "exact singleton proof accepts different mapping skeleton");
    }
  }
  std::cout << "color dependency proof: compact split Validation, "
               "cross-observation rejection, exact fallback PASS\n";
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
    if (argc == 2 && std::string(argv[1]) == "--joint-validation") {
      for (unsigned mode = 0; mode < 3; ++mode)
        joint_partial_validation(
            take(ps::encode_color_array(description(ps::ColorModel::Xyz))),
            mode);
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--dependency-proof") {
      mapping_proof();
      history_and_joint();
      grouping_schema();
      return 0;
    }
    codec();
    sample_domains();
    environment();
    public_workflow();
    tuple_output_view();
    mapping_proof();
    history_and_joint();
    grouping_schema();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
