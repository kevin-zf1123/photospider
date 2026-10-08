#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#include "../support/test_support.hpp"
#include "channel_extraction_workflow/source.hpp"
#include "numeric_workflow/icc_fixture.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)

template <class T>
std::vector<std::uint8_t> pack(const std::vector<T>& values) {
  std::vector<std::uint8_t> bytes(values.size() * sizeof(T));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}
using channel_fixture::take;
const ResultTensorSpec& tensor(const ResultRef& result) {
  return result.schema().tensors[0];
}
Status read_bytes(const ResultRef& result, const Region& region,
                  std::uint8_t* out, std::size_t bytes) {
  const auto packed = channel_fixture::read(result, region);
  if (packed.size() != bytes)
    return {ErrorCode::Internal, "test read size"};
  std::memcpy(out, packed.data(), bytes);
  return Status::success();
}
template <class T>
T read(const ResultRef& result, const std::vector<std::uint64_t>& at) {
  std::vector<RegionDimension> dims;
  for (auto x : at)
    dims.push_back({x, 1});
  auto window =
      take(result.acquire_tensor(take(result.descriptor()), 0, Region(dims)));
  T sample;
  std::memcpy(&sample, take(window.row_run(at)).data, sizeof(sample));
  return sample;
}
struct Probe {
  std::uint64_t limit = UINT64_MAX, work = 0;
  unsigned charges = 0;
  std::function<void()> before, after;
  std::function<void(std::uint64_t)> charge;
  CancellationToken cancellation;
};
struct PreparedProbe {
  std::shared_ptr<const PreparedOperation> inner;
};
struct ProbePhase {
  ResultContinuation inner;
  Probe* probe;
  ProbePhase(ResultContinuation value, Probe* p)
      : inner(std::move(value)), probe(p) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!phase.tensors || phase.tensors->empty())
      return inner.poll(phase);
    auto forwarded = phase;
    forwarded.consume_work = [&](std::uint64_t amount) {
      if (amount > probe->limit - probe->work)
        return Status{ErrorCode::ResourceExhausted, "test numeric work limit"};
      probe->work += amount;
      ++probe->charges;
      if (probe->charge)
        probe->charge(amount);
      return phase.consume_work(amount);
    };
    if (probe->before)
      probe->before();
    auto result = inner.poll(forwarded);
    if (probe->after)
      probe->after();
    return result;
  }
};
std::shared_ptr<OperationRegistry> probe_registry(Probe* probe) {
  auto base = make_default_operation_registry();
  if (!probe)
    return base;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition spy;
  spy.key = "numeric.convert_format_strict";
  spy.traits = take(base->find_traits(spy.key));
  spy.traits.outputs[0].continuation_bytes += sizeof(ProbePhase);
  spy.prepare_static = [base](const auto& inputs, const auto& params) {
    auto inner = base->prepare_operation("numeric.convert_format_strict",
                                         inputs, params);
    if (!inner.ok())
      return Result<OperationPreparation>(inner.status());
    OperationPreparation result;
    result.outputs.resize(1);
    result.outputs[0].metadata.result_schema = std::make_shared<SchemaTemplate>(
        *inner.value()->traits().outputs[0].result_schema);
    result.state =
        std::make_shared<PreparedProbe>(PreparedProbe{inner.take_value()});
    return Result<OperationPreparation>(std::move(result));
  };
  spy.start_result = [base, probe](const auto& query, const auto& allocator) {
    auto forwarded = query;
    forwarded.prepared =
        static_cast<const PreparedProbe*>(query.prepared->state())->inner;
    auto inner = base->start_result("numeric.convert_format_strict", forwarded,
                                    allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<ProbePhase>(allocator, inner.take_value(),
                                                probe);
  };
  channel_fixture::require(registry->register_operation(std::move(spy)));
  channel_fixture::require(registry->freeze());
  return registry;
}
Result<ExecutionResult> run_source(
    const channel_fixture::Source& source,
    std::map<std::string, ParameterValue> parameters,
    std::optional<Region> roi = {}, Probe* probe = nullptr,
    ResultRef* published = nullptr, bool empty = false) {
  WorkflowDocument document;
  document.inputs = {channel_fixture::declaration(source)};
  document.nodes = {{1,
                     "numeric.convert_format_strict",
                     {WorkflowInputReference{1}},
                     std::move(parameters)}};
  document.outputs = {{"converted", 1, "values"}};
  auto registry = probe_registry(probe);
  PlanningOptions planning;
  planning.tile_height = source.tile_height;
  planning.tile_width = source.tile_width;
  if (roi)
    planning.output_regions = {{"converted", *roi}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph, planning, source.resources);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  ExecutionBindings bindings;
  bindings.inputs.push_back(
      {"source",
       channel_fixture::publish(take(context.resource_budget()), source)});
  if (published)
    *published = bindings.inputs[0].result;
  if (empty) {
    auto frozen = context.freeze(compiled.value().plan, bindings);
    if (!frozen.ok())
      return Result<ExecutionResult>(frozen.status());
    auto demand = context.execute_fragments(
        frozen.value(),
        {{"converted",
          take(Footprint::none(source.schema.tensors[0].sample_shape()))}});
    if (!demand.ok())
      return Result<ExecutionResult>(demand.status());
    ExecutionResult result;
    result.results = std::move(demand.value().results);
    return Result<ExecutionResult>(std::move(result));
  }
  ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(1) << 40;
  options.maximum_dependency_work = UINT64_C(1) << 40;
  return context.execute(compiled.value().plan, bindings,
                         probe ? probe->cancellation : CancellationToken{},
                         options);
}
Result<ExecutionResult> run(ElementType source_type,
                            const std::vector<std::uint64_t>& shape,
                            const std::vector<std::uint8_t>& bytes,
                            std::map<std::string, ParameterValue> parameters,
                            std::optional<Region> roi = {},
                            std::optional<TensorDescription> description = {},
                            Probe* probe = nullptr) {
  std::vector<ValueFacet> facets;
  if (description)
    facets.push_back(take(encode_tensor_description(*description)));
  auto source = channel_fixture::source({source_type, shape}, facets);
  source.bytes = bytes;
  return run_source(source, std::move(parameters), roi, probe);
}
Result<ExecutionResult> run_planar(
    ElementType type, std::uint64_t samples,
    const std::vector<std::uint8_t>& bytes,
    std::map<std::string, ParameterValue> parameters, std::uint64_t rows = 1,
    std::optional<Region> roi = {}, std::uint64_t tile_extent = 128) {
  ResultTensorLayout layout;
  layout.spatial = true;
  layout.order = ImagePlaneOrder::Tiled;
  auto source = channel_fixture::source({type, {rows, samples, 1}}, {}, layout);
  source.bytes = bytes;
  source.tile_height = source.tile_width = tile_extent;
  return run_source(source, std::move(parameters), roi);
}

int numeric_cases() {
  auto converted = run(ElementType::UInt8, {3}, {0, 128, 255},
                       {{"dtype", std::string("float32")}});
  if (!converted.ok())
    std::cerr << "default conversion: " << converted.status().message << '\n';
  PS_CHECK(converted.ok());
  const auto& first = converted.value().results.at("converted");
  PS_CHECK(read<float>(first, {0}) == 0);
  PS_CHECK(read<float>(first, {1}) == 128.0f / 255.0f);
  PS_CHECK(read<float>(first, {2}) == 1);
  auto registry = make_default_operation_registry();
  PS_CHECK(registry->find_traits("numeric.convert_format_strict").ok());
  PS_CHECK(
      !registry->find_traits("numeric.convert_format_accelerated_apple_silicon")
           .ok());
  PS_CHECK(
      !registry->find_traits("numeric.convert_format_accelerated_x86_64").ok());
  converted = run(ElementType::UInt8, {3}, {0, 128, 255},
                  {{"dtype", std::string("float32")}, {"rescale", false}});
  PS_CHECK(converted.ok());
  PS_CHECK(read<float>(converted.value().results.at("converted"), {1}) == 128);
  auto signed_map = run(ElementType::Int8, {4}, {128, 255, 0, 127},
                        {{"dtype", std::string("uint8")}});
  PS_CHECK(signed_map.ok());
  const auto& mapped = signed_map.value().results.at("converted");
  PS_CHECK(read<std::uint8_t>(mapped, {0}) == 0);
  PS_CHECK(read<std::uint8_t>(mapped, {1}) == 127);
  PS_CHECK(read<std::uint8_t>(mapped, {2}) == 128);
  PS_CHECK(read<std::uint8_t>(mapped, {3}) == 255);
  auto big = run(ElementType::Int64, {4},
                 pack<std::int64_t>({INT64_MIN, -1, 0, INT64_MAX}),
                 {{"dtype", std::string("uint8")}});
  if (!big.ok())
    std::cerr << "int64: " << big.status().message << '\n';
  PS_CHECK(big.ok());
  const auto& big_result = big.value().results.at("converted");
  PS_CHECK(read<std::uint8_t>(big_result, {1}) == 127);
  PS_CHECK(read<std::uint8_t>(big_result, {2}) == 128);
  auto partial = run(ElementType::Float64, {3}, pack<double>({0, 1, 1e100}),
                     {{"dtype", std::string("uint8")}, {"rescale", false}},
                     Region({{0, 2}}));
  PS_CHECK(partial.ok());
  PS_CHECK(read<std::uint8_t>(partial.value().results.at("converted"), {1}) ==
           1);
  auto rejected = run(ElementType::Float64, {3}, pack<double>({0, 1, 1e100}),
                      {{"dtype", std::string("uint8")}, {"rescale", false}});
  PS_CHECK(!rejected.ok());
  const auto nan_bits = UINT64_C(0xfff0000000000001);
  double signaling;
  std::memcpy(&signaling, &nan_bits, 8);
  auto identity = run(
      ElementType::Float64, {3},
      pack<double>({-0.0, signaling, std::numeric_limits<double>::infinity()}),
      {{"dtype", std::string("float64")},
       {"rescale", false},
       {"layout", std::string("view")}});
  PS_CHECK(identity.ok());
  std::uint64_t preserved;
  auto value = read<double>(identity.value().results.at("converted"), {1});
  std::memcpy(&preserved, &value, 8);
  PS_CHECK(preserved == nan_bits);
  const double max32 = static_cast<double>(std::numeric_limits<float>::max());
  const double adjacent =
      std::nextafter(max32, std::numeric_limits<double>::infinity());
  auto near_limit =
      run(ElementType::Float64, {2}, pack<double>({adjacent, max32 * 2}),
          {{"dtype", std::string("float32")}, {"rescale", false}},
          Region({{0, 1}}));
  PS_CHECK(near_limit.ok());
  PS_CHECK(read<float>(near_limit.value().results.at("converted"), {0}) ==
           std::numeric_limits<float>::max());
  auto far_limit =
      run(ElementType::Float64, {2}, pack<double>({adjacent, max32 * 2}),
          {{"dtype", std::string("float32")}, {"rescale", false}});
  PS_CHECK(!far_limit.ok());
  auto clipped_limit =
      run(ElementType::Float64, {2}, pack<double>({adjacent, max32 * 2}),
          {{"dtype", std::string("float32")},
           {"rescale", false},
           {"overflow", std::string("clip")}});
  PS_CHECK(clipped_limit.ok());
  PS_CHECK(read<float>(clipped_limit.value().results.at("converted"), {1}) ==
           std::numeric_limits<float>::max());
  auto nan_cast = run(ElementType::Float64, {1}, pack<double>({signaling}),
                      {{"dtype", std::string("float32")}, {"rescale", false}});
  PS_CHECK(nan_cast.ok());
  std::uint32_t quiet;
  const auto converted_nan =
      read<float>(nan_cast.value().results.at("converted"), {0});
  std::memcpy(&quiet, &converted_nan, 4);
  PS_CHECK(quiet == UINT32_C(0xffc00000));
  auto reversed =
      run(ElementType::Float64, {4}, pack<double>({0, .25, 1, 2}),
          {{"dtype", std::string("float32")},
           {"target_range",
            std::string("f64:3ff0000000000000,f64:0000000000000000")}});
  PS_CHECK(reversed.ok());
  const auto& reverse_value = reversed.value().results.at("converted");
  PS_CHECK(read<float>(reverse_value, {0}) == 1);
  PS_CHECK(read<float>(reverse_value, {1}) == .75f);
  PS_CHECK(read<float>(reverse_value, {2}) == 0);
  PS_CHECK(read<float>(reverse_value, {3}) == -1);
  TensorDescription described;
  described.channel_axis = 1;
  described.axes = {{"pixel", "px", 4, 2}, {"component", "", 0, 1}};
  described.encoding = TensorEncoding{{std::int64_t{0}, std::int64_t{255}},
                                      {std::int64_t{0}, std::int64_t{255}}};
  auto conflict = run(ElementType::Float32, {1, 2}, pack<float>({0, 1}),
                      {{"dtype", std::string("uint8")},
                       {"axis", std::int64_t{1}},
                       {"source_range", std::string("i:0,i:1;i:0,i:1")}},
                      {}, described);
  PS_CHECK(!conflict.ok());
  auto raw = run(
      ElementType::Float32, {1, 2}, pack<float>({0, 1}),
      {{"dtype", std::string("uint8")}, {"metadata_mode", std::string("raw")}},
      {}, described);
  PS_CHECK(raw.ok());
  auto raw_description = decode_tensor_description(
      tensor(raw.value().results.at("converted")).facets.at(0));
  PS_CHECK(raw_description.ok());
  PS_CHECK(raw_description.value().axes[0].name == "pixel");
  PS_CHECK(raw_description.value().axes[0].origin == 4);
  PS_CHECK(!raw_description.value().encoding);
  auto table =
      run(ElementType::Float32, {1, 4}, pack<float>({.5f, 0, 0, .5f}),
          {{"dtype", std::string("uint8")},
           {"axis", std::int64_t{1}},
           {"source_range",
            std::string("i:0,i:1;i:-128,i:127;i:-128,i:127;i:0,i:1")}});
  PS_CHECK(table.ok());
  const auto& table_value = table.value().results.at("converted");
  for (std::uint64_t c = 0; c < 4; ++c)
    PS_CHECK(read<std::uint8_t>(table_value, {0, c}) == 128);
  auto signed_zero =
      run(ElementType::UInt8, {1}, {0},
          {{"dtype", std::string("float32")},
           {"target_range", std::string("f64:8000000000000000,i:1")}});
  PS_CHECK(signed_zero.ok());
  float zero_sample =
      read<float>(signed_zero.value().results.at("converted"), {0});
  std::uint32_t zero_bits;
  std::memcpy(&zero_bits, &zero_sample, 4);
  PS_CHECK(zero_bits == UINT32_C(0x80000000));
  auto ordinary_zero = run(ElementType::Float64, {1}, pack<double>({0}),
                           {{"dtype", std::string("float32")},
                            {"source_range", std::string("i:-1,i:1")},
                            {"target_range", std::string("i:-1,i:1")}});
  PS_CHECK(ordinary_zero.ok());
  zero_sample = read<float>(ordinary_zero.value().results.at("converted"), {0});
  std::memcpy(&zero_bits, &zero_sample, 4);
  PS_CHECK(zero_bits == 0);
  auto mixed_identity =
      run(ElementType::Float64, {1}, pack<double>({signaling}),
          {{"dtype", std::string("float64")},
           {"source_range", std::string("i:-1,i:1")},
           {"target_range", std::string("f64:bff0000000000000,i:1")},
           {"layout", std::string("view")}});
  PS_CHECK(mixed_identity.ok());
  auto identity_nan =
      read<double>(mixed_identity.value().results.at("converted"), {0});
  std::memcpy(&preserved, &identity_nan, 8);
  PS_CHECK(preserved == nan_bits);
  TensorDescription grouped;
  grouped.channel_axis = 2;
  grouped.channels = {{"R", "red", "relative"},
                      {"G", "green", "relative"},
                      {"B", "blue", "relative"},
                      {"A", "coverage", "ratio"}};
  TensorColorGroup group;
  group.name = "color";
  group.indices = {0, 1, 2};
  group.components = {grouped.channels[0], grouped.channels[1],
                      grouped.channels[2]};
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  group.interpretation.association = "straight";
  group.alpha = 3;
  for (auto& component : group.components)
    component.encoding = TensorEncoding{{std::int64_t{0}, std::int64_t{255}},
                                        {std::int64_t{0}, std::int64_t{1}}};
  grouped.groups.push_back(group);
  grouped.channels.clear();
  auto inherited = run(ElementType::UInt8, {1, 1, 4}, {0, 128, 255, 255},
                       {{"dtype", std::string("float32")}}, {}, grouped);
  if (!inherited.ok())
    std::cerr << inherited.status().message << '\n';
  PS_CHECK(inherited.ok());
  auto inherited_description = decode_tensor_description(
      tensor(inherited.value().results.at("converted")).facets.at(0));
  PS_CHECK(inherited_description.ok());
  const auto& encoding = *inherited_description.value().channels[0].encoding;
  PS_CHECK((std::get_if<std::int64_t>(&encoding.decoded[1]) &&
            *std::get_if<std::int64_t>(&encoding.decoded[1]) == 1) ||
           (std::get_if<double>(&encoding.decoded[1]) &&
            *std::get_if<double>(&encoding.decoded[1]) == 1));
  auto raw_group = run(ElementType::UInt8, {1, 1, 4}, {0, 128, 255, 255},
                       {{"dtype", std::string("float32")},
                        {"metadata_mode", std::string("raw")}},
                       {}, grouped);
  PS_CHECK(raw_group.ok());
  auto raw_group_description = decode_tensor_description(
      tensor(raw_group.value().results.at("converted")).facets.at(0));
  PS_CHECK(raw_group_description.ok());
  PS_CHECK(raw_group_description.value().groups.empty());
  PS_CHECK(raw_group_description.value().channels[0].name == "R");
  PS_CHECK(!raw_group_description.value().channels[0].encoding);
  TensorDescription fractional;
  fractional.encoding = TensorEncoding{{std::int64_t{0}, std::int64_t{3}},
                                       {std::int64_t{0}, std::int64_t{1}}};
  auto replacement = tensor_description_parameter(fractional);
  PS_CHECK(replacement.ok());
  auto current_facet = encode_tensor_description(fractional);
  PS_CHECK(current_facet.ok());
  PS_CHECK(current_facet.value().version == 4);
  auto retired_facet = current_facet.value();
  retired_facet.version = 3;
  retired_facet.payload[3] = '3';
  PS_CHECK(!decode_tensor_description(retired_facet).ok());
  auto rational = run(ElementType::Float64, {1}, pack<double>({1}),
                      {{"dtype", std::string("float64")},
                       {"source_range", std::string("i:1,i:2")},
                       {"metadata_mode", std::string("override")},
                       {"metadata_override", replacement.take_value()}});
  PS_CHECK(rational.ok());
  auto rational_description = decode_tensor_description(
      tensor(rational.value().results.at("converted")).facets.at(0));
  PS_CHECK(rational_description.ok());
  const auto& rational_encoding = *rational_description.value().encoding;
  const auto* third =
      std::get_if<TensorRationalEndpoint>(&rational_encoding.decoded[0]);
  PS_CHECK(third != nullptr);
  PS_CHECK(third->numerator == std::vector<std::uint32_t>({1}));
  PS_CHECK(third->denominator == std::vector<std::uint32_t>({3}));
  return 0;
}
int pair_endpoints() {
  const std::array<std::pair<ElementType, std::string>, 7> types{
      {{ElementType::UInt8, "uint8"},
       {ElementType::UInt16, "uint16"},
       {ElementType::Int8, "int8"},
       {ElementType::Int16, "int16"},
       {ElementType::Int64, "int64"},
       {ElementType::Float32, "float32"},
       {ElementType::Float64, "float64"}}};
  const auto endpoints = [](ElementType type) {
    switch (type) {
      case ElementType::UInt8:
        return pack<std::uint8_t>({0, UINT8_MAX});
      case ElementType::UInt16:
        return pack<std::uint16_t>({0, UINT16_MAX});
      case ElementType::Int8:
        return pack<std::int8_t>({INT8_MIN, INT8_MAX});
      case ElementType::Int16:
        return pack<std::int16_t>({INT16_MIN, INT16_MAX});
      case ElementType::Int64:
        return pack<std::int64_t>({INT64_MIN, INT64_MAX});
      case ElementType::Float32:
        return pack<float>({0, 1});
      case ElementType::Float64:
        return pack<double>({0, 1});
    }
    return std::vector<std::uint8_t>{};
  };
  for (const auto& source : types)
    for (const auto& target : types) {
      auto converted = run(source.first, {2}, endpoints(source.first),
                           {{"dtype", target.second}});
      if (!converted.ok())
        std::cerr << source.second << "->" << target.second << ": "
                  << converted.status().message << '\n';
      PS_CHECK(converted.ok());
      const auto& value = converted.value().results.at("converted");
      PS_CHECK(tensor(value).descriptor.element_type == target.first);
      const auto expected = endpoints(target.first);
      for (std::uint64_t i = 0; i < 2; ++i) {
        const auto observed = channel_fixture::read(value, Region({{i, 1}}));
        const auto width = Value::element_size(target.first);
        PS_CHECK(std::memcmp(observed.data(), expected.data() + i * width,
                             width) == 0);
      }
    }
  return 0;
}
int planar_cross_tile() {
  ResultTensorLayout layout;
  layout.spatial = true;
  layout.order = ImagePlaneOrder::Tiled;
  auto source =
      channel_fixture::source({ElementType::UInt8, {131, 133, 4}}, {}, layout);
  for (std::uint64_t y = 0; y < 131; ++y)
    for (std::uint64_t x = 0; x < 133; ++x)
      for (std::uint64_t c = 0; c < 4; ++c)
        source.bytes[(y * 133 + x) * 4 + c] = (y * 17 + x * 13 + c * 31) % 256;
  const Region roi({{127, 3}, {127, 3}, {1, 1}});
  auto result = run_source(source,
                           {{"dtype", std::string("float32")},
                            {"metadata_mode", std::string("raw")}},
                           roi);
  PS_CHECK(result.ok());
  std::array<float, 9> observed{};
  PS_CHECK(read_bytes(result.value().results.at("converted"), roi,
                      reinterpret_cast<std::uint8_t*>(observed.data()),
                      sizeof(observed))
               .ok());
  for (std::uint64_t y = 0; y < 3; ++y)
    for (std::uint64_t x = 0; x < 3; ++x) {
      const auto code = ((127 + y) * 17 + (127 + x) * 13 + 31) % 256;
      PS_CHECK(observed[y * 3 + x] == static_cast<float>(code) / 255);
    }
  return 0;
}
int planar_vector_oracles() {
  constexpr std::uint64_t count = 259;
  std::vector<std::uint8_t> codes(count);
  for (std::uint64_t i = 0; i < count; ++i)
    codes[i] = static_cast<std::uint8_t>(i);
  auto expanded = run_planar(ElementType::UInt8, count, codes,
                             {{"dtype", std::string("float32")},
                              {"metadata_mode", std::string("raw")}});
  PS_CHECK(expanded.ok());
  std::vector<float> floats(count);
  PS_CHECK(read_bytes(expanded.value().results.at("converted"),
                      Region::whole({1, count, 1}),
                      reinterpret_cast<std::uint8_t*>(floats.data()),
                      floats.size() * 4)
               .ok());
  for (std::uint64_t i = 0; i < count; ++i)
    PS_CHECK(floats[i] ==
             static_cast<float>(static_cast<double>(codes[i]) / 255.0));
  auto compressed = run_planar(
      ElementType::Float32, count, pack(floats),
      {{"dtype", std::string("uint8")}, {"metadata_mode", std::string("raw")}});
  PS_CHECK(compressed.ok());
  std::vector<std::uint8_t> round_trip(count);
  PS_CHECK(read_bytes(compressed.value().results.at("converted"),
                      Region::whole({1, count, 1}), round_trip.data(),
                      round_trip.size())
               .ok());
  PS_CHECK(round_trip == codes);
  auto invalid_float = floats;
  invalid_float[19] = std::numeric_limits<float>::quiet_NaN();
  auto rejected = run_planar(ElementType::Float32, count, pack(invalid_float),
                             {{"dtype", std::string("uint8")},
                              {"overflow", std::string("clip")},
                              {"metadata_mode", std::string("raw")}});
  PS_CHECK(!rejected.ok());
  PS_CHECK(rejected.status().reason == FailureReason::InvalidDomain);
  std::vector<double> doubles(count);
  for (std::uint64_t i = 0; i < count; ++i)
    doubles[i] = static_cast<double>(i) / 255.0;
  auto narrowed = run_planar(ElementType::Float64, count, pack(doubles),
                             {{"dtype", std::string("float32")},
                              {"rescale", false},
                              {"metadata_mode", std::string("raw")}});
  PS_CHECK(narrowed.ok());
  std::vector<float> narrowed_samples(count);
  PS_CHECK(read_bytes(narrowed.value().results.at("converted"),
                      Region::whole({1, count, 1}),
                      reinterpret_cast<std::uint8_t*>(narrowed_samples.data()),
                      narrowed_samples.size() * 4)
               .ok());
  for (std::uint64_t i = 0; i < count; ++i)
    PS_CHECK(narrowed_samples[i] == static_cast<float>(doubles[i]));
  doubles[1] = -0.0;
  doubles[2] = std::numeric_limits<double>::infinity();
  const std::uint64_t nan_bits = UINT64_C(0xfff0000000000001);
  std::memcpy(&doubles[3], &nan_bits, 8);
  const auto maximum = static_cast<double>(std::numeric_limits<float>::max());
  doubles[4] = std::nextafter(maximum, std::numeric_limits<double>::infinity());
  doubles[5] = maximum * 2;
  doubles[6] = std::ldexp(1.0, -149);
  doubles[7] = std::ldexp(1.0, -150);
  doubles[8] = std::ldexp(3.0, -150);
  doubles[9] = -std::ldexp(1.0, -149);
  auto exceptional = run_planar(ElementType::Float64, count, pack(doubles),
                                {{"dtype", std::string("float32")},
                                 {"rescale", false},
                                 {"overflow", std::string("clip")},
                                 {"metadata_mode", std::string("raw")}});
  PS_CHECK(exceptional.ok());
  std::vector<std::uint32_t> bits(count);
  PS_CHECK(read_bytes(exceptional.value().results.at("converted"),
                      Region::whole({1, count, 1}),
                      reinterpret_cast<std::uint8_t*>(bits.data()),
                      bits.size() * 4)
               .ok());
  PS_CHECK(bits[1] == UINT32_C(0x80000000));
  PS_CHECK(bits[2] == UINT32_C(0x7f800000));
  PS_CHECK(bits[3] == UINT32_C(0xffc00000));
  PS_CHECK(bits[4] == UINT32_C(0x7f7fffff));
  PS_CHECK(bits[5] == UINT32_C(0x7f7fffff));
  PS_CHECK(bits[6] == 1);
  PS_CHECK(bits[7] == 0);
  PS_CHECK(bits[8] == 2);
  PS_CHECK(bits[9] == UINT32_C(0x80000001));
  return 0;
}
int planar_tile_oracle(std::uint64_t side) {
  const auto count = side * side;
  std::vector<float> samples(count);
  std::vector<std::uint8_t> expected(count), actual(count);
  std::uint32_t random = UINT32_C(0x243f6a88);
  for (std::uint64_t i = 0; i < count; ++i) {
    random = random * UINT32_C(1664525) + UINT32_C(1013904223);
    const auto bits = random % UINT32_C(0x3f800001);
    std::memcpy(&samples[i], &bits, 4);
    const double exact = static_cast<double>(samples[i]) * 255;
    const auto floor = static_cast<unsigned>(exact);
    const auto fraction = exact - floor;
    expected[i] = static_cast<std::uint8_t>(
        floor + (fraction > 0.5 || (fraction == 0.5 && (floor & 1))));
  }
  // Include each quantization threshold and its adjacent binary32 values.
  for (unsigned i = 0; i < 255; ++i) {
    const auto value = static_cast<float>((i + 0.5) / 255.0);
    const float values[] = {std::nextafter(value, 0.0f), value,
                            std::nextafter(value, 1.0f)};
    for (unsigned j = 0; j < 3; ++j) {
      samples[i * 3 + j] = values[j];
      const double exact = static_cast<double>(values[j]) * 255;
      const auto floor = static_cast<unsigned>(exact);
      const double fraction = exact - floor;
      expected[i * 3 + j] = static_cast<std::uint8_t>(
          floor + (fraction > 0.5 || (fraction == 0.5 && (floor & 1))));
    }
  }
  const std::map<std::string, ParameterValue> parameters{
      {"dtype", std::string("uint8")},
      {"metadata_mode", std::string("raw")}};
  auto converted = run_planar(ElementType::Float32, side, pack(samples),
                              parameters, side, {}, side);
  PS_CHECK(converted.ok());
  PS_CHECK(read_bytes(converted.value().results.at("converted"),
                      Region::whole({side, side, 1}), actual.data(),
                      actual.size())
               .ok());
  PS_CHECK(actual == expected);
  samples.back() = std::numeric_limits<float>::quiet_NaN();
  auto rejected = run_planar(ElementType::Float32, side, pack(samples),
                             parameters, side, {}, side);
  PS_CHECK(!rejected.ok());
  PS_CHECK(rejected.status().reason == FailureReason::InvalidDomain);
  samples.back() = 1.5f;
  auto clipping = parameters;
  clipping["overflow"] = std::string("clip");
  auto clipped = run_planar(ElementType::Float32, side, pack(samples), clipping,
                            side, {}, side);
  PS_CHECK(clipped.ok());
  PS_CHECK(read_bytes(clipped.value().results.at("converted"),
                      Region::whole({side, side, 1}), actual.data(),
                      actual.size())
               .ok());
  expected.back() = 255;
  PS_CHECK(actual == expected);
  samples.back() = std::numeric_limits<float>::quiet_NaN();
  const Region roi({{32, 64}, {32, 64}, {0, 1}});
  auto partial = run_planar(ElementType::Float32, side, pack(samples),
                            parameters, side, roi, side);
  PS_CHECK(partial.ok());
  std::vector<std::uint8_t> region_bytes(64 * 64);
  PS_CHECK(read_bytes(partial.value().results.at("converted"), roi,
                      region_bytes.data(), region_bytes.size())
               .ok());
  for (unsigned y = 0; y < 64; ++y)
    for (unsigned x = 0; x < 64; ++x)
      PS_CHECK(region_bytes[y * 64 + x] == expected[(y + 32) * side + x + 32]);
  return 0;
}
int randomized_i64_oracle() {
  constexpr std::uint64_t count = 1024;
  std::vector<std::int64_t> samples(count);
  std::uint64_t state = UINT64_C(0x243f6a8885a308d3);
  for (auto& sample : samples) {
    state = state * UINT64_C(6364136223846793005) + 1;
    std::memcpy(&sample, &state, 8);
  }
  auto converted = run_planar(
      ElementType::Int64, count, pack(samples),
      {{"dtype", std::string("uint8")}, {"metadata_mode", std::string("raw")}});
  PS_CHECK(converted.ok());
  std::vector<std::uint8_t> actual(count);
  PS_CHECK(read_bytes(converted.value().results.at("converted"),
                      Region::whole({1, count, 1}), actual.data(),
                      actual.size())
               .ok());
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto offset =
        static_cast<std::uint64_t>(samples[i]) - (UINT64_C(1) << 63);
    const auto numerator = static_cast<unsigned __int128>(offset) * 255;
    const auto quotient = numerator / UINT64_MAX;
    const auto remainder = numerator % UINT64_MAX;
    const auto expected =
        quotient + (remainder * 2 > UINT64_MAX ||
                    (remainder * 2 == UINT64_MAX && (quotient & 1)));
    PS_CHECK(actual[i] == static_cast<std::uint8_t>(expected));
  }
  return 0;
}
int randomized_float_narrowing() {
  std::vector<double> samples;
  std::uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  for (unsigned i = 0; i < 4096; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    const std::uint64_t exponent = 1 + ((state >> 16) % 2046);
    const std::uint64_t bits =
        (state & UINT64_C(0x800fffffffffffff)) | (exponent << 52);
    double sample;
    std::memcpy(&sample, &bits, 8);
    samples.push_back(sample);
  }
  auto converted = run(ElementType::Float64, {samples.size()}, pack(samples),
                       {{"dtype", std::string("float32")},
                        {"rescale", false},
                        {"overflow", std::string("clip")}});
  PS_CHECK(converted.ok());
  auto planar = run_planar(ElementType::Float64, samples.size(), pack(samples),
                           {{"dtype", std::string("float32")},
                            {"rescale", false},
                            {"overflow", std::string("clip")},
                            {"metadata_mode", std::string("raw")}});
  PS_CHECK(planar.ok());
  std::vector<std::uint32_t> planar_bits(samples.size());
  PS_CHECK(read_bytes(planar.value().results.at("converted"),
                      Region::whole({1, samples.size(), 1}),
                      reinterpret_cast<std::uint8_t*>(planar_bits.data()),
                      planar_bits.size() * 4)
               .ok());
  const auto& output = converted.value().results.at("converted");
  for (std::size_t i = 0; i < samples.size(); ++i) {
    float expected = static_cast<float>(samples[i]);
    if (std::isinf(expected))
      expected = std::copysign(std::numeric_limits<float>::max(), expected);
    const auto actual = read<float>(output, {i});
    std::uint32_t actual_bits, expected_bits;
    std::memcpy(&actual_bits, &actual, 4);
    std::memcpy(&expected_bits, &expected, 4);
    PS_CHECK(actual_bits == expected_bits);
    PS_CHECK(planar_bits[i] == expected_bits);
  }
  return 0;
}
int randomized_float_widening() {
  std::vector<float> samples;
  std::uint32_t state = UINT32_C(0x12345678);
  for (unsigned i = 0; i < 2048; ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    const std::uint32_t exponent = 1 + ((state >> 16) % 254);
    const std::uint32_t bits =
        (state & UINT32_C(0x807fffff)) | (exponent << 23);
    float sample;
    std::memcpy(&sample, &bits, 4);
    samples.push_back(sample);
  }
  auto converted = run(ElementType::Float32, {samples.size()}, pack(samples),
                       {{"dtype", std::string("float64")}, {"rescale", false}});
  PS_CHECK(converted.ok());
  const auto& output = converted.value().results.at("converted");
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double expected = static_cast<double>(samples[i]);
    const auto actual = read<double>(output, {i});
    std::uint64_t actual_bits, expected_bits;
    std::memcpy(&actual_bits, &actual, 8);
    std::memcpy(&expected_bits, &expected, 8);
    PS_CHECK(actual_bits == expected_bits);
  }
  return 0;
}
int direct_fenv() {
  Probe probe;
  fenv_t original;
  int before = 0, after = 0, rounding = 0;
  probe.before = [&] {
    fegetenv(&original);
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
    before = fetestexcept(FE_ALL_EXCEPT);
  };
  probe.after = [&] {
    after = fetestexcept(FE_ALL_EXCEPT);
    rounding = fegetround();
    fesetenv(&original);
  };
  auto result = run(ElementType::Float32, {1},
                    pack<std::uint32_t>({UINT32_C(0x80000001)}),
                    {{"dtype", std::string("uint8")},
                     {"metadata_mode", std::string("raw")},
                     {"overflow", std::string("clip")}},
                    {}, {}, &probe);
  PS_CHECK(result.ok());
  PS_CHECK(before == after && rounding == FE_DOWNWARD);
  return 0;
}
int static_shape_limit() {
  auto registry = make_default_operation_registry();
  OperationMetadata input;
  SchemaTemplate schema;
  schema.id = "test.numeric";
  ResultTensorSpec spec;
  spec.key = "samples";
  spec.descriptor = {ElementType::UInt8, {(UINT64_C(1) << 40) + 1}};
  schema.tensors.push_back(spec);
  input.result_schema = std::make_shared<SchemaTemplate>(schema);
  auto prepared = registry->prepare_operation(
      "numeric.convert_format_strict", {input},
      {{"dtype", std::string("uint8")}, {"metadata_mode", std::string("raw")}});
  PS_CHECK(!prepared.ok());
  PS_CHECK(prepared.status().code == ErrorCode::TypeMismatch);
  return 0;
}
int rational_codec_rejections() {
  TensorDescription description;
  description.encoding =
      TensorEncoding{{std::int64_t{0}, std::int64_t{1}},
                     {TensorRationalEndpoint{{1}, {3}, false},
                      TensorRationalEndpoint{{2}, {3}, false}}};
  auto valid = encode_tensor_description(description);
  PS_CHECK(valid.ok());
  PS_CHECK(decode_tensor_description(valid.value()).ok());
  auto broken = valid.value();
  broken.payload.pop_back();
  PS_CHECK(!decode_tensor_description(broken).ok());
  broken = valid.value();
  broken.payload[3] = '3';
  PS_CHECK(!decode_tensor_description(broken).ok());
  const std::array<std::uint8_t, 14> marker{2, 0, 1, 0, 1, 0, 0,
                                            0, 1, 0, 3, 0, 0, 0};
  const auto found =
      std::search(valid.value().payload.begin(), valid.value().payload.end(),
                  marker.begin(), marker.end());
  PS_CHECK(found != valid.value().payload.end());
  const auto offset =
      static_cast<std::size_t>(found - valid.value().payload.begin());
  broken = valid.value();
  broken.payload[offset] = 3;
  PS_CHECK(!decode_tensor_description(broken).ok());
  broken = valid.value();
  broken.payload[offset + 1] = 2;
  PS_CHECK(!decode_tensor_description(broken).ok());
  broken = valid.value();
  broken.payload[offset + 2] = 0;
  PS_CHECK(!decode_tensor_description(broken).ok());
  broken = valid.value();
  broken.payload.resize(offset + 6);
  PS_CHECK(!decode_tensor_description(broken).ok());
  const auto invalid = [&](TensorRationalEndpoint endpoint) {
    description.encoding->decoded[0] = std::move(endpoint);
    return !encode_tensor_description(description).ok();
  };
  PS_CHECK(invalid({{1}, {0}, false}));
  PS_CHECK(invalid({{1, 0}, {3}, false}));
  PS_CHECK(invalid({{2}, {6}, false}));
  PS_CHECK(invalid({{0}, {1}, true}));
  PS_CHECK(invalid({std::vector<std::uint32_t>(129, 1), {3}, false}));
  return 0;
}
int cold_u8_budget() {
  for (std::uint64_t count : {1, 3, 16, 19, 64, 259}) {
    Probe probe;
    probe.limit = 2048;
    auto result =
        run(ElementType::UInt8, {count}, std::vector<std::uint8_t>(count, 128),
            {{"dtype", std::string("float32")}}, {}, {}, &probe);
    PS_CHECK(!result.ok());
    PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(probe.work > 95 && probe.work <= probe.limit);
  }
  return 0;
}
int budget_failure_order() {
  std::uint64_t first_failure_work = 0;
  for (std::uint64_t bad_at : {0, 1, 17, 63}) {
    std::vector<float> samples(64, .5f);
    samples[bad_at] = std::numeric_limits<float>::quiet_NaN();
    Probe probe;
    // One window checkpoint plus exactly 65 units per visited sample.
    probe.limit = 1 + (bad_at + 1) * 65;
    auto result = run(ElementType::Float32, {64}, pack(samples),
                      {{"dtype", std::string("uint8")},
                       {"metadata_mode", std::string("raw")}},
                      {}, {}, &probe);
    PS_CHECK(!result.ok());
    PS_CHECK(result.status().code == ErrorCode::OperationFailed);
    PS_CHECK(result.status().reason == FailureReason::InvalidDomain);
    PS_CHECK(result.status().message.find("coordinate=[" +
                                          std::to_string(bad_at) + "]") !=
             std::string::npos);
    PS_CHECK(result.status().message.find(
                 "source_dtype=float32 target_dtype=uint8 range_index=0") !=
             std::string::npos);
    if (!bad_at)
      first_failure_work = probe.work;
    PS_CHECK(probe.work == first_failure_work + bad_at * 65);
    PS_CHECK(probe.work <= probe.limit);
  }
  return 0;
}
int generic_vector_oracles() {
  std::vector<std::uint8_t> codes(259);
  for (unsigned i = 0; i < codes.size(); ++i)
    codes[i] = static_cast<std::uint8_t>(i);
  auto expanded = run(ElementType::UInt8, {codes.size()}, codes,
                      {{"dtype", std::string("float32")}});
  PS_CHECK(expanded.ok());
  for (unsigned i = 0; i < codes.size(); ++i) {
    const float expected =
        static_cast<float>(static_cast<double>(codes[i]) / 255.0);
    const auto actual =
        read<float>(expanded.value().results.at("converted"), {i});
    PS_CHECK(std::memcmp(&actual, &expected, 4) == 0);
  }
  // Every rounding boundary, on both sides, plus exact halves and short tails.
  std::vector<float> samples{-0.0f, 0.0f,
                             std::numeric_limits<float>::denorm_min(),
                             -std::numeric_limits<float>::denorm_min()};
  for (unsigned i = 0; i < 255; ++i) {
    const float boundary = static_cast<float>((i + .5) / 255.0);
    samples.push_back(std::nextafter(boundary, -INFINITY));
    samples.push_back(boundary);
    samples.push_back(std::nextafter(boundary, INFINITY));
  }
  auto compressed = run(ElementType::Float32, {samples.size()}, pack(samples),
                        {{"dtype", std::string("uint8")}});
  PS_CHECK(compressed.ok());
  for (unsigned i = 0; i < samples.size(); ++i) {
    const double mapped = static_cast<double>(samples[i]) * 255;
    const auto lower = static_cast<std::int64_t>(std::floor(mapped));
    const auto expected =
        lower + (mapped - lower > .5 || (mapped - lower == .5 && (lower & 1)));
    PS_CHECK(read<std::uint8_t>(compressed.value().results.at("converted"),
                                {i}) == expected);
  }
  return 0;
}
int generic_special_bits() {
  const std::vector<std::pair<std::uint64_t, std::uint32_t>> cases{
      {0, 0},
      {UINT64_C(0x8000000000000000), UINT32_C(0x80000000)},
      {UINT64_C(0x3ff0000000000000), UINT32_C(0x3f800000)},
      {UINT64_C(0x4000000000000000), UINT32_C(0x40000000)},
      {UINT64_C(0x4008000000000000), UINT32_C(0x40400000)},
      {UINT64_C(0x4010000000000000), UINT32_C(0x40800000)},
      {UINT64_C(0x4014000000000000), UINT32_C(0x40a00000)},
      {UINT64_C(0x4018000000000000), UINT32_C(0x40c00000)},
      {UINT64_C(0x36a0000000000000), 1},
      {UINT64_C(0x3690000000000000), 0},
      {UINT64_C(0x36a8000000000000), 2},
      {UINT64_C(0x3810000000000000), UINT32_C(0x00800000)},
      {UINT64_C(0xfff0000000000001), UINT32_C(0xffc00000)},
      {UINT64_C(0x7ff8000020000000), UINT32_C(0x7fc00001)},
      {UINT64_C(0x7ff0000000000000), UINT32_C(0x7f800000)},
      {UINT64_C(0xfff0000000000000), UINT32_C(0xff800000)},
      {UINT64_C(0x47f0000000000000), UINT32_C(0x7f7fffff)},
      {UINT64_C(0x3ff0000010000000), UINT32_C(0x3f800000)},
      {UINT64_C(0x3ff0000010000001), UINT32_C(0x3f800001)}};
  std::vector<std::uint64_t> bits(259);
  for (std::size_t i = 0; i < bits.size(); ++i)
    bits[i] = cases[i % cases.size()].first;
  for (unsigned mode = 0; mode < 4; ++mode) {
    Probe probe;
    bool restored_ok = false;
#if defined(__x86_64__)
    unsigned saved = 0;
#elif defined(__aarch64__)
    std::uint64_t saved = 0, selected = 0;
#endif
    probe.before = [&] {
#if defined(__x86_64__)
      saved = _mm_getcsr();
      _mm_setcsr((saved & ~UINT32_C(0x8040)) | ((mode & 1) ? 0x8000 : 0) |
                 ((mode & 2) ? 0x40 : 0));
#elif defined(__aarch64__)
      __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
      selected = (saved & ~(UINT64_C(1) << 24)) |
                 ((mode & 1) ? (UINT64_C(1) << 24) : 0);
      __asm__ volatile("msr fpcr, %0" : : "r"(selected));
#endif
    };
    probe.after = [&] {
#if defined(__x86_64__)
      const bool restored =
          _mm_getcsr() == ((saved & ~UINT32_C(0x8040)) |
                           ((mode & 1) ? 0x8000 : 0) | ((mode & 2) ? 0x40 : 0));
      _mm_setcsr(saved);
      restored_ok = restored;
#elif defined(__aarch64__)
      std::uint64_t restored;
      __asm__ volatile("mrs %0, fpcr" : "=r"(restored));
      __asm__ volatile("msr fpcr, %0" : : "r"(saved));
      restored_ok = restored == selected;
#else
      restored_ok = true;
#endif
    };
    auto result = run(ElementType::Float64, {bits.size()}, pack(bits),
                      {{"dtype", std::string("float32")},
                       {"rescale", false},
                       {"overflow", std::string("clip")}},
                      {}, {}, &probe);
    PS_CHECK(result.ok());
    PS_CHECK(restored_ok);
    const auto& output = result.value().results.at("converted");
    for (std::size_t i = 0; i < bits.size(); ++i) {
      std::uint32_t actual = 0;
      actual = read<std::uint32_t>(output, {i});
      PS_CHECK(actual == cases[i % cases.size()].second);
    }
  }
  return 0;
}
int result_batches_and_views() {
  for (unsigned storage = 0; storage < 3; ++storage) {
    ResultTensorLayout layout;
    layout.spatial = storage != 0;
    layout.order =
        storage == 2 ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
    auto source = channel_fixture::source({ElementType::UInt8, {2, 3, 2}}, {},
                                          layout, {2, 2});
    const auto shape = source.schema.tensors[0].sample_shape();
    const Region roi({{1, 1}, {0, 1}, {1, 1}, {0, 3}, {1, 1}});
    ResultRef original;
    const std::map<std::string, ParameterValue> identity{
        {"dtype", std::string("uint8")},
        {"rescale", false},
        {"layout", std::string("view")}};
    auto viewed = run_source(source, identity, roi, nullptr, &original);
    PS_CHECK(viewed.ok());
    auto retained = viewed.value().results.at("converted");
    PS_CHECK(channel_fixture::owner(retained, roi) ==
             channel_fixture::owner(original, roi));
    PS_CHECK(tensor(retained).batch_axes ==
             source.schema.tensors[0].batch_axes);
    PS_CHECK(retained.association().size() == 1 &&
             retained.association()[0] == original.object_id());
    const auto expected = channel_fixture::read(original, roi);
    original = {};
    viewed = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(channel_fixture::read(retained, roi) == expected);
    PS_CHECK(!retained
                  .acquire_tensor(take(retained.descriptor()), 0,
                                  Region::whole(shape))
                  .ok());
    auto empty = run_source(source, identity, {}, nullptr, nullptr, true);
    PS_CHECK(empty.ok());
    PS_CHECK(take(empty.value().results.at("converted").descriptor())
                 .tensor_coverage(0)
                 .empty());
    auto converted =
        run_source(source,
                   {{"dtype", std::string("float32")},
                    {"axis", std::int64_t{2}},
                    {"target_range", std::string("i:0,i:1;i:0,i:2")}},
                   roi);
    PS_CHECK(converted.ok());
    const auto& output = converted.value().results.at("converted");
    for (std::uint64_t x = 0; x < 3; ++x) {
      const std::vector<std::uint64_t> at{1, 0, 1, x, 1};
      const auto code = source.bytes[channel_fixture::address(source, at)];
      PS_CHECK(read<float>(output, at) ==
               static_cast<float>(static_cast<double>(code) * 2 / 255));
    }
    const auto q = take(Footprint::from_regions(shape, {roi}));
    PS_CHECK(
        take(converted.value().dependencies.source_support()).at("source") ==
        q);
    for (unsigned roles : {1, 2, 4}) {
      auto dirty =
          take(converted.value().dependencies.potential_dirty(
                   "source", q, roles, {}, ResultSupportTarget::Tensor, 0))
              .at("converted");
      PS_CHECK(roles == 1 ? dirty == q : dirty.empty());
    }
    auto invalid_source = channel_fixture::source(
        {ElementType::Float64, {2, 3, 2}}, {}, layout, {2, 2});
    invalid_source.bytes = pack<double>(std::vector<double>(48, 1e100));
    auto invalid =
        run_source(invalid_source,
                   {{"dtype", std::string("uint8")},
                    {"axis", std::int64_t{2}},
                    {"source_range", std::string("i:0,i:1;i:0,i:1")}},
                   roi);
    PS_CHECK(!invalid.ok() &&
             invalid.status().reason == FailureReason::ArithmeticOverflow);
    PS_CHECK(invalid.status().message.find("coordinate=[1,0,1,0,1]") !=
             std::string::npos);
    PS_CHECK(invalid.status().message.find(
                 "source_dtype=float64 target_dtype=uint8 range_index=1 "
                 "channel_axis=2 channel=1") != std::string::npos);
    auto forced = run_source(
        source,
        {{"dtype", std::string("float32")}, {"layout", std::string("view")}},
        roi);
    PS_CHECK(!forced.ok() && forced.status().message.find("ViewUnavailable") !=
                                 std::string::npos);
  }
  // A generic identity preserves legal negative and zero strides.
  for (const auto stride : {std::int64_t{-4}, std::int64_t{0}}) {
    auto source = channel_fixture::source({ElementType::Float32, {3}});
    source.bytes = pack<std::uint32_t>(
        {UINT32_C(0x7f800001), UINT32_C(0x80000000), UINT32_C(0xffc00123)});
    source.layout.byte_strides = {stride};
    source.layout.byte_offset = stride < 0 ? 8 : 0;
    ResultRef original;
    auto run = run_source(source,
                          {{"dtype", std::string("float32")},
                           {"rescale", false},
                           {"layout", std::string("view")}},
                          {}, nullptr, &original);
    PS_CHECK(run.ok());
    const auto& result = run.value().results.at("converted");
    auto window = take(result.acquire_tensor(take(result.descriptor()), 0,
                                             Region::whole({3})));
    PS_CHECK(take(window.row_run({0})).sample_stride_bytes == stride);
    PS_CHECK(channel_fixture::read(result, Region::whole({3})) ==
             channel_fixture::read(original, Region::whole({3})));
  }
  for (auto stride : {std::int64_t{-2}, std::int64_t{0}}) {
    auto source = channel_fixture::source({ElementType::Int16, {3}});
    source.bytes = pack<std::int16_t>({1, 2, 3});
    source.layout.byte_strides = {stride};
    source.layout.byte_offset = stride < 0 ? 4 : 0;
    auto converted = run_source(
        source, {{"dtype", std::string("float32")}, {"rescale", false}});
    PS_CHECK(converted.ok());
    for (std::uint64_t i = 0; i < 3; ++i)
      PS_CHECK(read<float>(converted.value().results.at("converted"), {i}) ==
               (stride < 0 ? 3 - i : 1));
  }
  return 0;
}
int result_resource_lifetime_and_reuse() {
  ResultRef retained;
  ColorProfileIdentity identity;
  ResourceBudget source_root;
  {
    ResourceBudget profiles;
    const auto bytes = numeric_fixture::fixture();
    auto profile = take(
        IccProfile::import(ByteView(bytes.data(), bytes.size()), profiles));
    identity = profile.identity();
    TensorDescription d;
    d.component = TensorChannelDescription{"x", "", ""};
    d.component->interpretation.emplace();
    d.component->interpretation->profile = identity;
    auto source = channel_fixture::source({ElementType::Float64, {3}},
                                          {take(encode_tensor_description(d))});
    source.bytes = pack<double>({.25, .5, 1});
    source.resources = take(ResourceBindings::create({profile}, profiles));
    WorkflowDocument document;
    document.inputs = {channel_fixture::declaration(source)};
    document.nodes = {
        {1,
         "numeric.convert_format_strict",
         {WorkflowInputReference{1}},
         {{"dtype", std::string("float32")}, {"rescale", false}}}};
    document.outputs = {{"converted", 1, "values"}};
    auto registry = make_default_operation_registry();
    GraphContext graph(document);
    const auto compiled =
        take(Compiler(registry).compile(graph, {}, source.resources));
    ExecutionContext context(registry);
    source_root = take(context.resource_budget());
    ExecutionBindings bindings;
    bindings.inputs.push_back(
        {"source", channel_fixture::publish(source_root, source)});
    auto first = std::async(std::launch::async, [&] {
      return context.execute(compiled.plan, bindings);
    });
    auto second = std::async(std::launch::async, [&] {
      return context.execute(compiled.plan, bindings);
    });
    auto a = take(first.get());
    auto b = take(second.get());
    retained = a.results.at("converted");
    PS_CHECK(
        channel_fixture::read(retained, Region::whole({3})) ==
        channel_fixture::read(b.results.at("converted"), Region::whole({3})));
    PS_CHECK(retained.resources().icc_profile(identity).ok());
    PS_CHECK(retained.association()[0] ==
             bindings.inputs[0].result.object_id());
  }
  PS_CHECK(retained.resources().icc_profile(identity).ok());
  PS_CHECK(read<float>(retained, {0}) == .25F &&
           read<float>(retained, {2}) == 1);
  retained = {};
  PS_CHECK(source_root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
int direct_limits() {
  const std::map<std::string, ParameterValue> parameters{
      {"dtype", std::string("float32")},
      {"source_range", std::string("i:0,i:3")},
      {"target_range", std::string("i:0,i:1")},
      {"metadata_mode", std::string("raw")}};
  auto execute = [&](Probe* probe) {
    return run(ElementType::Float64, {1}, pack<double>({.25}), parameters, {},
               {}, probe);
  };
  Probe measured;
  PS_CHECK(execute(&measured).ok());
  PS_CHECK(measured.work > 1);
  Probe limited;
  limited.limit = measured.work - 1;
  auto failed = execute(&limited);
  PS_CHECK(!failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(limited.work > 0 && limited.work < measured.work);
  CancellationSource cancelled;
  cancelled.cancel();
  Probe before;
  before.cancellation = cancelled.token();
  auto stopped = execute(&before);
  PS_CHECK(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled);
  CancellationSource midflight;
  Probe during;
  during.cancellation = midflight.token();
  unsigned checks = 0;
  during.charge = [&](std::uint64_t amount) {
    if (amount && ++checks == 2)
      midflight.cancel();
  };
  auto interrupted = execute(&during);
  PS_CHECK(!interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled);
  PS_CHECK(during.work > 0 && checks == 2);
  return 0;
}
}  // namespace

int main() {
  if (cold_u8_budget())
    return 1;
  if (budget_failure_order() || generic_vector_oracles() ||
      generic_special_bits())
    return 1;
  if (numeric_cases())
    return 1;
  if (pair_endpoints())
    return 1;
  if (planar_cross_tile())
    return 1;
  if (planar_vector_oracles())
    return 1;
  if (planar_tile_oracle(128) || planar_tile_oracle(256))
    return 1;
  if (randomized_i64_oracle())
    return 1;
  if (randomized_float_narrowing())
    return 1;
  if (randomized_float_widening())
    return 1;
  if (direct_fenv())
    return 1;
  if (static_shape_limit())
    return 1;
  if (rational_codec_rejections())
    return 1;
  if (result_batches_and_views() || result_resource_lifetime_and_reuse())
    return 1;
  return direct_limits();
}
