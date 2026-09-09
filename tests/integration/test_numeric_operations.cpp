#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
template <class T>
ElementType type() {
  return std::is_same_v<T, float>          ? ElementType::Float32
         : std::is_same_v<T, double>       ? ElementType::Float64
         : std::is_same_v<T, std::int64_t> ? ElementType::Int64
                                           : ElementType::UInt8;
}
template <class T>
Value array(const std::vector<T>& numbers) {
  std::vector<std::uint8_t> bytes(numbers.size() * sizeof(T));
  std::memcpy(bytes.data(), numbers.data(), bytes.size());
  return Value::create({type<T>(), {numbers.size()}},
                       Region::whole({numbers.size()}), {0, {sizeof(T)}}, bytes)
      .take_value();
}
Parameters cast_parameters(const std::string& dtype,
                           const std::string& overflow = "reject") {
  return {{"dtype", dtype},
          {"rounding", std::string("ties_even")},
          {"overflow", overflow}};
}
Parameters range_parameters(const std::string& dtype, double source_min,
                            double source_max, double target_min,
                            double target_max,
                            const std::string& overflow = "reject") {
  auto result = cast_parameters(dtype, overflow);
  result.insert({{"src_min", source_min},
                 {"src_max", source_max},
                 {"dst_min", target_min},
                 {"dst_max", target_max}});
  return result;
}
/** @brief Public producer -> numeric workflow; producer allows legal strided
 * Values. */
Result<ExecutionResult> run(const std::string& operation,
                            const std::vector<Value>& inputs,
                            const Parameters& parameters = {},
                            unsigned* calls = nullptr,
                            ExecutionContextConfig config = {}) {
  auto base = make_default_operation_registry();
  auto registry = std::make_shared<OperationRegistry>();
  auto traits = base->find_traits(operation).take_value();
  auto registered = registry->register_operation(
      {operation, traits,
       [base, operation, calls](const OperationInvocation& call) {
         if (calls)
           ++*calls;
         return base->invoke(operation, call);
       }});
  if (!registered.ok())
    return Result<ExecutionResult>(registered);
  WorkflowDocument document;
  std::vector<WorkflowInput> references;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    OperationTraits source;
    source.output_element_type = inputs[i].descriptor().element_type;
    source.shape_rule = OperationShapeRule::Fixed;
    source.fixed_output_shape = inputs[i].descriptor().shape;
    source.estimated_bytes = inputs[i].storage()->capacity();
    source.output_semantic_rule = OperationSemanticRule::Establish;
    source.output_facets = inputs[i].facets();
    const auto key = "fixture.input" + std::to_string(i);
    auto status = registry->register_operation(
        {key, source, [value = inputs[i]](const OperationInvocation&) {
           return Result<Value>(value);
         }});
    if (!status.ok())
      return Result<ExecutionResult>(status);
    document.nodes.push_back({i + 1, key, {}, {}});
    references.push_back(WorkflowNodeOutput{i + 1, "value"});
  }
  registry->freeze();
  document.nodes.push_back({100, operation, references, parameters});
  document.outputs = {{"result", 100, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContext execution(registry, config);
  return execution.execute(compiled.value().plan);
}
Value output(const Result<ExecutionResult>& result) {
  return result.value().values.at("result");
}
template <class T>
bool equal(const Result<ExecutionResult>& result,
           const std::vector<T>& expected) {
  if (!result.ok())
    return false;
  const auto value = output(result);
  return value.descriptor().element_type == type<T>() &&
         value.facets().empty() &&
         value.bytes().size() == expected.size() * sizeof(T) &&
         std::memcmp(value.bytes().data(), expected.data(),
                     value.bytes().size()) == 0;
}
int casts() {
  std::vector<std::uint8_t> original(256);
  for (unsigned i = 0; i < 256; ++i)
    original[i] = static_cast<std::uint8_t>(i);
  const auto bytes = array(original);
  for (const auto* dtype : {"uint8", "int64", "float32", "float64"}) {
    auto converted = run("numeric.cast", {bytes}, cast_parameters(dtype));
    PS_CHECK(converted.ok());
    PS_CHECK(equal(
        run("numeric.cast", {output(converted)}, cast_parameters("uint8")),
        original));
    auto encoded = run("numeric.encode_range", {bytes},
                       range_parameters(dtype, 0, 255, 0,
                                        std::string(dtype) == "uint8" ||
                                                std::string(dtype) == "int64"
                                            ? 255
                                            : 1));
    PS_CHECK(encoded.ok());
    const double max =
        std::string(dtype) == "uint8" || std::string(dtype) == "int64" ? 255
                                                                       : 1;
    PS_CHECK(equal(run("numeric.encode_range", {output(encoded)},
                       range_parameters("uint8", 0, max, 0, 255)),
                   original));
  }
  const auto halfway =
      array<double>({-.5, .5, 1.5, 2.5, -1.5, -2.5, 254.5, 255.5});
  PS_CHECK(equal<std::int64_t>(
      run("numeric.cast", {halfway}, cast_parameters("int64")),
      {0, 0, 2, 2, -2, -2, 254, 256}));
  PS_CHECK(
      run("numeric.cast", {halfway}, cast_parameters("uint8")).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(equal<std::uint8_t>(
      run("numeric.cast", {halfway}, cast_parameters("uint8", "clip")),
      {0, 0, 2, 2, 0, 0, 254, 255}));
  const auto minimum = std::numeric_limits<std::int64_t>::min(),
             maximum = std::numeric_limits<std::int64_t>::max();
  const std::vector<std::int64_t> exact{minimum,
                                        minimum + 1,
                                        -INT64_C(9007199254740993),
                                        INT64_C(9007199254740993),
                                        maximum - 1,
                                        maximum};
  PS_CHECK(equal(run("numeric.cast", {array(exact)}, cast_parameters("int64")),
                 exact));
  PS_CHECK(
      equal(run("numeric.encode_range", {array(exact)},
                range_parameters("int64", -0x1p63, 0x1p63, -0x1p63, 0x1p63)),
            exact));
  const auto midpoint_above = (INT64_C(1) << 62) + (INT64_C(1) << 38) + 1;
  auto rounded = run("numeric.cast",
                     {array<std::int64_t>({midpoint_above, -midpoint_above})},
                     cast_parameters("float32"));
  PS_CHECK(rounded.ok());
  std::uint32_t bits[2];
  std::memcpy(bits, output(rounded).bytes().data(), sizeof(bits));
  PS_CHECK(bits[0] == 0x5e800001U && bits[1] == 0xde800001U);
  PS_CHECK(equal<std::int64_t>(
      run("numeric.cast",
          {array<double>({-0x1p63, std::nextafter(0x1p63, 0.)})},
          cast_parameters("int64")),
      {minimum, maximum - 1023}));
  const auto outside = array<double>(
      {0x1p63,
       std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity())});
  PS_CHECK(
      run("numeric.cast", {outside}, cast_parameters("int64")).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(equal<std::int64_t>(
      run("numeric.cast", {outside}, cast_parameters("int64", "clip")),
      {maximum, minimum}));
  const auto huge = array<double>({std::numeric_limits<double>::max()});
  PS_CHECK(
      run("numeric.cast", {huge}, cast_parameters("float32")).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(equal<float>(
      run("numeric.cast", {huge}, cast_parameters("float32", "clip")),
      {std::numeric_limits<float>::max()}));
  PS_CHECK(equal<double>(
      run("numeric.cast", {array<float>({std::numeric_limits<float>::max()})},
          cast_parameters("float64")),
      {static_cast<double>(std::numeric_limits<float>::max())}));
  const double largest = std::numeric_limits<double>::max();
  PS_CHECK(equal<double>(run("numeric.encode_range", {array<double>({2, -2})},
                             range_parameters("float64", -largest, largest,
                                              -largest / 2, largest / 2)),
                         {1, -1}));
  PS_CHECK(equal<double>(
      run("numeric.encode_range", {array<double>({largest, -largest})},
          range_parameters("float64", 0, 1, 0, 2, "clip")),
      {largest, -largest}));
  PS_CHECK(run("numeric.encode_range", {huge},
               range_parameters("float64", 0, 1, 0, 2))
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(equal<double>(run("numeric.encode_range", {array<double>({2})},
                             range_parameters("float64", 0, 0x1p1000, 0, 1)),
                         {0x1p-999}));
  PS_CHECK(
      equal<double>(run("numeric.encode_range", {array<double>({0x1p53 + 2})},
                        range_parameters("float64", 0x1p53, 0x1p53 + 6, 0, 6)),
                    {2}));
  auto nearby = run("numeric.encode_range", {array<double>({0x1p53 + 4})},
                    range_parameters("float64", 0x1p53 + 2, 0x1p53 + 8, 0, 1));
  PS_CHECK(nearby.ok() &&
           std::abs(output(nearby).as_float64().value() - 1. / 3) < 1e-15);
  PS_CHECK(equal<double>(
      run("numeric.encode_range", {array<double>({2})},
          range_parameters("float64", 0, largest, 0, largest / 2)),
      {1}));
  for (double bad : {std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    for (const auto* policy : {"reject", "clip"}) {
      PS_CHECK(run("numeric.cast", {array<double>({bad})},
                   cast_parameters("int64", policy))
                   .status()
                   .code == ErrorCode::OperationFailed);
      PS_CHECK(run("numeric.encode_range", {array<double>({bad})},
                   range_parameters("float32", 0, 1, 0, 1, policy))
                   .status()
                   .code == ErrorCode::OperationFailed);
    }
    auto floating =
        run("numeric.cast", {array<double>({bad})}, cast_parameters("float32"));
    PS_CHECK(floating.ok());
    float actual;
    std::memcpy(&actual, output(floating).bytes().data(), 4);
    PS_CHECK(std::isnan(bad) ? std::isnan(actual)
                             : std::isinf(actual) &&
                                   std::signbit(actual) == std::signbit(bad));
  }
  const std::uint32_t patterns[] = {0x80000000U, 0x7f800000U, 0xff800000U,
                                    0x7fc01234U, 0x7f801234U};
  std::vector<std::uint8_t> raw(sizeof(patterns));
  std::memcpy(raw.data(), patterns, sizeof(patterns));
  auto special = Value::create({ElementType::Float32, {5}}, Region::whole({5}),
                               {0, {4}}, raw)
                     .take_value();
  auto identical = run("numeric.cast", {special}, cast_parameters("float32"));
  PS_CHECK(identical.ok() && output(identical).copy_bytes() == raw);
  const auto old_rounding = std::fegetround();
  std::fesetround(FE_UPWARD);
  auto ties =
      run("numeric.cast", {array<double>({2.5})}, cast_parameters("int64"));
  const auto restored = std::fegetround();
  std::fesetround(old_rounding);
  PS_CHECK(equal<std::int64_t>(ties, {2}) && restored == FE_UPWARD);
  return 0;
}
int math_and_views() {
  PS_CHECK(equal<float>(run("numeric.subtract",
                            {array<float>({3, 2, 1}), array<float>({4, 4, 4})}),
                        {-1, -2, -3}));
  PS_CHECK(equal<double>(
      run("numeric.add", {array<double>({-3, 2, 1}), array<double>({4, 4, 4})}),
      {1, 6, 5}));
  PS_CHECK(equal<float>(run("numeric.divide", {array<float>({-3, 2, 1}),
                                               array<float>({2, 4, .5F})}),
                        {-1.5F, .5F, 2}));
  PS_CHECK(equal<float>(run("numeric.clamp", {array<float>({-2, .5F, 4})},
                            {{"min", 0.}, {"max", 1.}}),
                        {0, .5F, 1}));
  PS_CHECK(equal<float>(run("numeric.clamp", {array<float>({1})},
                            {{"min", -1e100}, {"max", 1e100}}),
                        {1}));
  PS_CHECK(run("numeric.clamp", {array<float>({1})},
               {{"min", 1e100}, {"max", 1e100}})
               .status()
               .code == ErrorCode::OperationFailed);
  for (const auto& samples :
       {array<float>({1, 2, 3}), array<double>({1, 2, 3})}) {
    PS_CHECK(equal<double>(run("numeric.mean", {samples}), {2}));
    auto variance = run("numeric.variance", {samples});
    PS_CHECK(variance.ok() &&
             std::abs(output(variance).as_float64().value() - 2. / 3) < 1e-15);
  }
  auto data = array<double>({1, 2, 3});
  auto reversed = Value::create(data.descriptor(), data.region(),
                                {0, {-8}, {2}}, data.copy_bytes())
                      .take_value();
  auto broadcast = Value::create(data.descriptor(), data.region(), {0, {0}},
                                 Value::from_float64(1).copy_bytes())
                       .take_value();
  PS_CHECK(equal<float>(
      run("numeric.cast", {reversed}, cast_parameters("float32")), {3, 2, 1}));
  PS_CHECK(equal<double>(run("numeric.add", {reversed, broadcast}), {4, 3, 2}));
  PS_CHECK(equal<double>(run("numeric.mean", {broadcast}), {1}));
  std::vector<std::uint8_t> unaligned(13);
  const float values[] = {.5F, 1.5F, 2.5F};
  std::memcpy(unaligned.data() + 1, values, 12);
  auto padded = Value::create({ElementType::Float32, {3}}, Region::whole({3}),
                              {1, {4}}, unaligned)
                    .take_value();
  PS_CHECK(equal<std::int64_t>(
      run("numeric.cast", {padded}, cast_parameters("int64")), {0, 2, 2}));
  auto half = array<float>({.75F});
  auto mask =
      Value::create({ElementType::Float32, {1, 1}}, Region::whole({1, 1}),
                    {0, {4, 4}}, half.copy_bytes(),
                    {encode_semantic(coverage_semantics()).take_value()})
          .take_value();
  auto twice = Value::create(mask.descriptor(), mask.region(), mask.layout(),
                             array<float>({2}).copy_bytes())
                   .take_value();
  PS_CHECK(equal<float>(run("numeric.multiply", {mask, twice}), {1.5F}));
  return 0;
}
int failures_and_resources() {
  unsigned callbacks = 0;
  for (const auto& pair : std::vector<std::vector<Value>>{
           {array<std::int64_t>({1}), array<std::int64_t>({2})},
           {array<float>({1}), array<double>({2})},
           {array<float>({1}), array<float>({1, 2})}}) {
    PS_CHECK(run("numeric.add", pair, {}, &callbacks).status().code ==
             ErrorCode::TypeMismatch);
    PS_CHECK(callbacks == 0);
  }
  PS_CHECK(run("numeric.mean", {array<std::uint8_t>({1})}, {}, &callbacks)
                   .status()
                   .code == ErrorCode::TypeMismatch &&
           callbacks == 0);
  PS_CHECK(run("numeric.cast", {array<float>({1})}, {}).status().code ==
           ErrorCode::InvalidArgument);
  auto bad_rounding = cast_parameters("uint8");
  bad_rounding["rounding"] = std::string("floor");
  PS_CHECK(
      run("numeric.cast", {array<float>({1})}, bad_rounding).status().code ==
      ErrorCode::InvalidArgument);
  PS_CHECK(run("numeric.encode_range", {array<float>({1})},
               range_parameters("uint8", 1, 1, 0, 255))
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(run("numeric.clamp", {array<float>({1})}, {{"min", 2.}, {"max", 1.}})
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(run("numeric.divide", {array<float>({1}), array<float>({-0.F})})
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(run("numeric.multiply",
               {array<float>({std::numeric_limits<float>::max()}),
                array<float>({2})})
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(run("numeric.variance",
               {array<double>({std::numeric_limits<double>::quiet_NaN()})})
               .status()
               .code == ErrorCode::OperationFailed);
  const auto n = UINT64_C(1) << 62;
  auto enormous = Value::create({ElementType::Float64, {n}}, Region::whole({n}),
                                {0, {0}}, Value::from_float64(1).copy_bytes())
                      .take_value();
  PS_CHECK(
      run("numeric.cast", {enormous}, cast_parameters("float64"), &callbacks)
              .status()
              .code == ErrorCode::ResourceExhausted &&
      callbacks == 0);
  ExecutionContextConfig limited;
  limited.maximum_live_bytes = 1;
  PS_CHECK(run("numeric.add", {array<float>({1}), array<float>({2})}, {},
               nullptr, limited)
               .status()
               .code == ErrorCode::ResourceExhausted);
  auto registry = make_default_operation_registry();
  CancellationSource cancelled;
  std::uint64_t live = 0;
  BufferAllocator allocator([&](std::uint64_t bytes) {
    live += bytes;
    cancelled.cancel();
    return Result<std::shared_ptr<void>>(
        std::shared_ptr<void>(new int(0), [&, bytes](void* pointer) {
          delete static_cast<int*>(pointer);
          live -= bytes;
        }));
  });
  const std::vector<Value> inputs{array<float>({1})};
  const std::vector<Region> demands{Region::whole({1})};
  auto parameters = cast_parameters("float32");
  auto result = registry->invoke(
      "numeric.cast", {inputs, demands, parameters, Backend::Cpu,
                       cancelled.token(), Region::whole({1}), allocator});
  PS_CHECK(result.status().code == ErrorCode::Cancelled && live == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(casts() == 0);
  PS_CHECK(math_and_views() == 0);
  PS_CHECK(failures_and_resources() == 0);
  return 0;
}
