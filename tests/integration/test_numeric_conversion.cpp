#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)

template <class T>
std::vector<std::uint8_t> pack(const std::vector<T>& values) {
  std::vector<std::uint8_t> bytes(values.size() * sizeof(T));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}
template <class T>
T read(const Value& value, const std::vector<std::uint64_t>& at) {
  auto address = value.byte_address(at);
  if (!address.ok())
    std::abort();
  T result;
  std::memcpy(&result, value.bytes().data() + address.value(), sizeof(T));
  return result;
}
Result<ExecutionResult> run(ElementType source_type,
                            const std::vector<std::uint64_t>& shape,
                            const std::vector<std::uint8_t>& bytes,
                            std::map<std::string, ParameterValue> parameters,
                            std::optional<Region> roi = {},
                            std::optional<TensorDescription> description = {}) {
  const ValueDescriptor source{source_type, shape};
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t width = Value::element_size(source_type);
  for (std::size_t i = shape.size(); i; --i) {
    strides[i - 1] = width;
    width *= static_cast<std::int64_t>(shape[i - 1]);
  }
  std::vector<ValueFacet> facets;
  if (description) {
    auto encoded = encode_tensor_description(*description);
    if (!encoded.ok())
      return Result<ExecutionResult>(encoded.status());
    facets.push_back(encoded.take_value());
  }
  auto value =
      Value::create(source, Region::whole(shape), {0, strides}, bytes, facets);
  if (!value.ok())
    return Result<ExecutionResult>(value.status());
  WorkflowDocument document;
  document.inputs = {
      {1, "source", source, Region::whole(shape), {0, strides}, facets}};
  document.nodes = {{1,
                     "numeric.convert_format_strict",
                     {WorkflowInputReference{1}},
                     std::move(parameters)}};
  document.outputs = {{"converted", 1, "values"}};
  PlanningOptions options;
  if (roi)
    options.output_regions = {{"converted", *roi}};
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(document);
  auto plan = compiler.compile(graph, options);
  if (!plan.ok())
    return Result<ExecutionResult>(plan.status());
  ExecutionContext executor(registry);
  return executor.execute(plan.value().plan,
                          {{{"source", value.take_value()}}});
}
Result<ExecutionResult> run_planar(
    ElementType source_type, std::uint64_t samples,
    const std::vector<std::uint8_t>& bytes,
    std::map<std::string, ParameterValue> parameters) {
  const ValueDescriptor descriptor{source_type, {1, samples, 1}};
  PlanarImageConfig config;
  config.order = ImagePlaneOrder::Tiled;
  auto created = PlanarImage::create(descriptor, config);
  if (!created.ok())
    return Result<ExecutionResult>(created.status());
  auto image = created.take_value();
  auto published = image.publish(Region::whole(descriptor.shape), bytes.data(),
                                 bytes.size());
  if (!published.ok())
    return Result<ExecutionResult>(published);
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
  document.nodes = {{1,
                     "numeric.convert_format_strict",
                     {WorkflowInputReference{1}},
                     std::move(parameters)}};
  document.outputs = {{"converted", 1, "values"}};
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(document);
  auto plan = compiler.compile(graph);
  if (!plan.ok())
    return Result<ExecutionResult>(plan.status());
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(image);
  ExecutionBindings bindings;
  bindings.inputs.push_back(binding);
  ExecutionContext execution(registry);
  return execution.execute(plan.value().plan, bindings);
}

int numeric_cases() {
  auto converted = run(ElementType::UInt8, {3}, {0, 128, 255},
                       {{"dtype", std::string("float32")}});
  if (!converted.ok())
    std::cerr << "default conversion: " << converted.status().message << '\n';
  PS_CHECK(converted.ok());
  const auto& first = converted.value().values.at("converted");
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
  PS_CHECK(read<float>(converted.value().values.at("converted"), {1}) == 128);
  auto signed_map = run(ElementType::Int8, {4}, {128, 255, 0, 127},
                        {{"dtype", std::string("uint8")}});
  PS_CHECK(signed_map.ok());
  const auto& mapped = signed_map.value().values.at("converted");
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
  const auto& big_result = big.value().values.at("converted");
  PS_CHECK(read<std::uint8_t>(big_result, {1}) == 127);
  PS_CHECK(read<std::uint8_t>(big_result, {2}) == 128);
  auto partial = run(ElementType::Float64, {3}, pack<double>({0, 1, 1e100}),
                     {{"dtype", std::string("uint8")}, {"rescale", false}},
                     Region({{0, 2}}));
  PS_CHECK(partial.ok());
  PS_CHECK(read<std::uint8_t>(partial.value().values.at("converted"), {1}) ==
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
  auto value = read<double>(identity.value().values.at("converted"), {1});
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
  PS_CHECK(read<float>(near_limit.value().values.at("converted"), {0}) ==
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
  PS_CHECK(read<float>(clipped_limit.value().values.at("converted"), {1}) ==
           std::numeric_limits<float>::max());
  auto nan_cast = run(ElementType::Float64, {1}, pack<double>({signaling}),
                      {{"dtype", std::string("float32")}, {"rescale", false}});
  PS_CHECK(nan_cast.ok());
  std::uint32_t quiet;
  const auto converted_nan =
      read<float>(nan_cast.value().values.at("converted"), {0});
  std::memcpy(&quiet, &converted_nan, 4);
  PS_CHECK(quiet == UINT32_C(0xffc00000));
  auto reversed =
      run(ElementType::Float64, {4}, pack<double>({0, .25, 1, 2}),
          {{"dtype", std::string("float32")},
           {"target_range",
            std::string("f64:3ff0000000000000,f64:0000000000000000")}});
  PS_CHECK(reversed.ok());
  const auto& reverse_value = reversed.value().values.at("converted");
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
      raw.value().values.at("converted").facets().at(0));
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
  const auto& table_value = table.value().values.at("converted");
  for (std::uint64_t c = 0; c < 4; ++c)
    PS_CHECK(read<std::uint8_t>(table_value, {0, c}) == 128);
  auto signed_zero =
      run(ElementType::UInt8, {1}, {0},
          {{"dtype", std::string("float32")},
           {"target_range", std::string("f64:8000000000000000,i:1")}});
  PS_CHECK(signed_zero.ok());
  float zero_sample =
      read<float>(signed_zero.value().values.at("converted"), {0});
  std::uint32_t zero_bits;
  std::memcpy(&zero_bits, &zero_sample, 4);
  PS_CHECK(zero_bits == UINT32_C(0x80000000));
  auto ordinary_zero = run(ElementType::Float64, {1}, pack<double>({0}),
                           {{"dtype", std::string("float32")},
                            {"source_range", std::string("i:-1,i:1")},
                            {"target_range", std::string("i:-1,i:1")}});
  PS_CHECK(ordinary_zero.ok());
  zero_sample = read<float>(ordinary_zero.value().values.at("converted"), {0});
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
      read<double>(mixed_identity.value().values.at("converted"), {0});
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
      inherited.value().values.at("converted").facets().at(0));
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
      raw_group.value().values.at("converted").facets().at(0));
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
      rational.value().values.at("converted").facets().at(0));
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
      const auto& value = converted.value().values.at("converted");
      PS_CHECK(value.descriptor().element_type == target.first);
      const auto expected = endpoints(target.first);
      for (std::uint64_t i = 0; i < 2; ++i) {
        auto address = value.byte_address({i});
        PS_CHECK(address.ok());
        const auto width = Value::element_size(target.first);
        PS_CHECK(std::memcmp(value.bytes().data() + address.value(),
                             expected.data() + i * width, width) == 0);
      }
    }
  return 0;
}
int planar_cross_tile() {
  const ValueDescriptor descriptor{ElementType::UInt8, {131, 133, 4}};
  PlanarImageConfig config;
  auto source = PlanarImage::create(descriptor, config);
  PS_CHECK(source.ok());
  auto input_image = source.take_value();
  std::vector<std::uint8_t> plane(131 * 133);
  for (std::uint64_t channel = 0; channel < 4; ++channel) {
    for (std::uint64_t y = 0; y < 131; ++y)
      for (std::uint64_t x = 0; x < 133; ++x)
        plane[y * 133 + x] =
            static_cast<std::uint8_t>((y * 17 + x * 13 + channel * 31) % 256);
    PS_CHECK(input_image
                 .publish(Region({{0, 131}, {0, 133}, {channel, 1}}),
                          plane.data(), plane.size())
                 .ok());
  }
  WorkflowDocument document;
  document.inputs = {{1,
                      "image",
                      descriptor,
                      Region::whole(descriptor.shape),
                      {},
                      {},
                      PlanarImageLayout{config.order, 0, 1, 2, 0, {}}}};
  document.nodes = {{1,
                     "numeric.convert_format_strict",
                     {WorkflowInputReference{1}},
                     {{"dtype", std::string("float32")},
                      {"metadata_mode", std::string("raw")}}}};
  document.outputs = {{"converted", 1, "values"}};
  PlanningOptions options;
  const Region roi({{127, 3}, {127, 3}, {1, 1}});
  options.output_regions = {{"converted", roi}};
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  GraphContext graph(document);
  auto plan = compiler.compile(graph, options);
  PS_CHECK(plan.ok());
  ExecutionBinding binding;
  binding.name = "image";
  binding.image = std::make_shared<const PlanarImage>(input_image);
  ExecutionBindings bindings;
  bindings.inputs.push_back(binding);
  ExecutionContext execution(registry);
  auto result = execution.execute(plan.value().plan, bindings);
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok());
  const auto& image = result.value().images.at("converted");
  std::array<float, 9> observed{};
  PS_CHECK(image
               .read(roi, reinterpret_cast<std::uint8_t*>(observed.data()),
                     sizeof(observed))
               .ok());
  for (std::uint64_t y = 0; y < 3; ++y)
    for (std::uint64_t x = 0; x < 3; ++x) {
      const auto code = (static_cast<std::uint64_t>(127 + y) * 17 +
                         static_cast<std::uint64_t>(127 + x) * 13 + 31) %
                        256;
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
  PS_CHECK(expanded.value()
               .images.at("converted")
               .read(Region::whole({1, count, 1}),
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
  PS_CHECK(compressed.value()
               .images.at("converted")
               .read(Region::whole({1, count, 1}), round_trip.data(),
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
  PS_CHECK(narrowed.value()
               .images.at("converted")
               .read(Region::whole({1, count, 1}),
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
  PS_CHECK(exceptional.value()
               .images.at("converted")
               .read(Region::whole({1, count, 1}),
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
  PS_CHECK(converted.value()
               .images.at("converted")
               .read(Region::whole({1, count, 1}), actual.data(), actual.size())
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
  PS_CHECK(planar.value()
               .images.at("converted")
               .read(Region::whole({1, samples.size(), 1}),
                     reinterpret_cast<std::uint8_t*>(planar_bits.data()),
                     planar_bits.size() * 4)
               .ok());
  const auto& output = converted.value().values.at("converted");
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
  const auto& output = converted.value().values.at("converted");
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
  const std::uint32_t input_bits = UINT32_C(0x80000001);
  auto value = Value::create({ElementType::Float32, {1}}, Region::whole({1}),
                             {0, {4}}, pack<std::uint32_t>({input_bits}));
  PS_CHECK(value.ok());
  auto source = value.take_value();
  DependencyRequest request;
  request.inputs = {{source.descriptor(), source.facets()}};
  request.outputs = Footprint::all({1}).take_value();
  request.parameters = {{"dtype", std::string("uint8")},
                        {"metadata_mode", std::string("raw")},
                        {"overflow", std::string("clip")}};
  request.snapshot_identity = "fmt06-fenv";
  auto registry = make_default_operation_registry();
  auto started =
      registry->start_dependency("numeric.convert_format_strict", request);
  PS_CHECK(started.ok());
  auto session = started.take_value();
  auto first = session->poll();
  PS_CHECK(first.ok());
  auto fragments =
      ValueFragments::create(source.descriptor(), source.facets(),
                             Footprint::all({1}).take_value(), {source});
  PS_CHECK(fragments.ok());
  PS_CHECK(session->supply({fragments.take_value()}, "fmt06-fenv").ok());
  fenv_t original;
  fegetenv(&original);
  PS_CHECK(fesetround(FE_DOWNWARD) == 0);
  feclearexcept(FE_ALL_EXCEPT);
  feraiseexcept(FE_INVALID);
  const auto before = fetestexcept(FE_ALL_EXCEPT);
  auto second = session->poll();
  const auto after = fetestexcept(FE_ALL_EXCEPT);
  const auto rounding = fegetround();
  fesetenv(&original);
  PS_CHECK(second.ok());
  PS_CHECK(std::holds_alternative<DependencyResult>(second.value()));
  PS_CHECK(before == after);
  PS_CHECK(rounding == FE_DOWNWARD);
  return 0;
}
int static_shape_limit() {
  auto registry = make_default_operation_registry();
  OperationMetadata input;
  input.descriptor = {ElementType::UInt8, {(UINT64_C(1) << 40) + 1}};
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
int direct_limits() {
  auto source = Value::create({ElementType::Float64, {1}}, Region::whole({1}),
                              {0, {8}}, pack<double>({0.25}));
  PS_CHECK(source.ok());
  auto input = source.take_value();
  DependencyRequest request;
  request.inputs = {{input.descriptor(), input.facets()}};
  request.outputs = Footprint::all({1}).take_value();
  request.parameters = {{"dtype", std::string("float32")},
                        {"source_range", std::string("i:0,i:3")},
                        {"target_range", std::string("i:0,i:1")},
                        {"metadata_mode", std::string("raw")}};
  request.snapshot_identity = "fmt06-limits";
  request.limits.maximum_work = 150;
  auto registry = make_default_operation_registry();
  auto started =
      registry->start_dependency("numeric.convert_format_strict", request);
  PS_CHECK(started.ok());
  auto session = started.take_value();
  auto first = session->poll();
  PS_CHECK(first.ok());
  auto fragments =
      ValueFragments::create(input.descriptor(), input.facets(),
                             Footprint::all({1}).take_value(), {input});
  PS_CHECK(fragments.ok());
  PS_CHECK(session->supply({fragments.take_value()}, "fmt06-limits").ok());
  auto second = session->poll();
  PS_CHECK(!second.ok());
  PS_CHECK(second.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(session->consumed_work() > 100);
  CancellationSource cancellation;
  request.limits.maximum_work = 1048576;
  request.cancellation = cancellation.token();
  cancellation.cancel();
  auto cancelled =
      registry->start_dependency("numeric.convert_format_strict", request);
  if (cancelled.ok()) {
    auto progress = cancelled.value()->poll();
    PS_CHECK(!progress.ok());
  } else {
    PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
  }
  CancellationSource midflight;
  request.cancellation = midflight.token();
  std::uint64_t work = 0;
  auto charging = [&](std::uint64_t amount) {
    work += amount;
    if (work > 200)
      midflight.cancel();
    return Status::success();
  };
  auto running = registry->start_dependency(
      "numeric.convert_format_strict", request, BufferAllocator{}, charging);
  PS_CHECK(running.ok());
  auto active = running.take_value();
  PS_CHECK(active->poll().ok());
  auto mid_fragments =
      ValueFragments::create(input.descriptor(), input.facets(),
                             Footprint::all({1}).take_value(), {input});
  PS_CHECK(mid_fragments.ok());
  PS_CHECK(active->supply({mid_fragments.take_value()}, "fmt06-limits").ok());
  auto interrupted = active->poll();
  PS_CHECK(!interrupted.ok());
  PS_CHECK(interrupted.status().code == ErrorCode::Cancelled);
  PS_CHECK(work > 200);
  return 0;
}
}  // namespace

int main() {
  if (numeric_cases())
    return 1;
  if (pair_endpoints())
    return 1;
  if (planar_cross_tile())
    return 1;
  if (planar_vector_oracles())
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
  return direct_limits();
}
