#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/lowpass_uniform_math.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::LowpassKernel;
using numeric_ops::LowpassParameters;
using numeric_ops::SequenceProfile;
std::uint64_t raw(double value) {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, 8);
  return result;
}
LowpassParameters parameters(
    LowpassKernel kernel, const std::map<std::string, ParameterValue>& input) {
  LowpassParameters result{kernel};
  result.radius =
      raw(static_cast<double>(std::get<std::int64_t>(input.at("radius"))));
  if (kernel == LowpassKernel::Gaussian)
    result.sigma = raw(std::get<double>(input.at("sigma")));
  else
    result.cutoff = raw(std::get<double>(input.at("cutoff")));
  if (kernel == LowpassKernel::Kaiser)
    result.beta = raw(std::get<double>(input.at("beta")));
  return result;
}
struct UniformState {
  LowpassParameters parameters;
  unsigned axis, radius;
  std::string boundary;
  SequenceProfile profile;
  numeric_ops::UniformLowpassMath arithmetic;
  ResourceVector<std::uint64_t> samples;
  ResourceVector<numeric_ops::FastInterval> coefficients;
  numeric_ops::FastInterval normalizer;
  bool coefficients_ready = false;
  const OperationInvocation& call;
  const ResourceBudget* budget;
  std::function<Status(std::uint64_t)> consume;
  UniformState(LowpassParameters p, unsigned dimension, unsigned count,
               std::string extension, SequenceProfile selected,
               const OperationInvocation& invocation)
      : parameters(p),
        axis(dimension),
        radius(count),
        boundary(std::move(extension)),
        profile(selected),
        call(invocation),
        budget(resource_internal::metadata_budget()),
        consume([this](auto amount) {
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled, {}};
          return budget ? budget->consume({amount}) : Status::success();
        }) {}
  std::optional<std::uint64_t> mapped(std::uint64_t center, int offset,
                                      std::uint64_t size) const {
    const auto at = static_cast<std::int64_t>(center) + offset;
    if (at >= 0 && static_cast<std::uint64_t>(at) < size)
      return static_cast<std::uint64_t>(at);
    if (boundary == "zero")
      return {};
    if (boundary == "replicate")
      return at < 0 ? 0 : size - 1;
    if (size == 1)
      return 0;
    const auto period =
        static_cast<std::int64_t>(boundary == "wrap" ? size : 2 * (size - 1));
    auto reduced = at % period;
    if (reduced < 0)
      reduced += period;
    return boundary == "reflect" && static_cast<std::uint64_t>(reduced) >= size
               ? period - reduced
               : reduced;
  }
  Result<Value> execute() {
    using Answer = Result<Value>;
    const auto& input = call.inputs[0];
    const auto& descriptor = input.descriptor();
    const auto& shape = descriptor.shape;
    samples.resize(2 * radius + 1);
    if (profile != SequenceProfile::Strict) {
      coefficients.resize(radius + 1);
      auto prepared = arithmetic.prepare(
          parameters, radius, coefficients.data(), &normalizer, consume);
      if (!prepared.ok())
        return Answer(prepared.status());
      coefficients_ready = prepared.value();
    }
    auto allocated =
        MutableValue::allocate(descriptor, call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    std::uint64_t count = 1;
    for (auto extent : shape)
      count *= extent;
    std::vector<std::uint64_t> coordinate(shape.size()), at(shape.size());
    for (std::uint64_t row = 0; row < count; ++row) {
      at = coordinate;
      for (int offset = -static_cast<int>(radius);
           offset <= static_cast<int>(radius); ++offset) {
        auto status = consume(shape.size() + 1);
        if (!status.ok())
          return Answer(status);
        auto& bits = samples[offset + radius];
        bits = 0;
        // Complete collection does not make zero-weight samples numerical
        // operands.
        if (!numeric_ops::uniform_tap_sign(parameters, radius,
                                           offset < 0 ? -offset : offset))
          continue;
        auto source = mapped(coordinate[axis], offset, shape[axis]);
        if (!source)
          continue;
        at[axis] = *source;
        auto address = input.byte_address(at);
        if (!address.ok())
          return Answer(address.status());
        std::memcpy(&bits, input.bytes().data() + address.value(), width);
      }
      std::optional<std::uint64_t> fast;
      if (coefficients_ready) {
        auto status = consume((radius + 1) * 32);
        if (!status.ok())
          return Answer(status);
        fast = arithmetic.fast(parameters, radius, samples.data(), narrow,
                               coefficients.data(), normalizer);
      }
      auto value = fast ? Result<std::uint64_t>(*fast)
                        : arithmetic.evaluate(parameters, radius,
                                              samples.data(), narrow, consume);
      if (!value.ok())
        return Answer(value.status());
      auto status = consume(1);
      if (!status.ok())
        return Answer(status);
      const auto bits = value.value();
      std::memcpy(output.data() + row * width, &bits, width);
      for (auto i = shape.size(); i; --i) {
        if (++coordinate[i - 1] < shape[i - 1])
          break;
        coordinate[i - 1] = 0;
      }
    }
    auto status = consume(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  }
};
Result<Value> execute_uniform(const OperationInvocation& call,
                              LowpassKernel kernel, SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    auto scratch = call.allocator.allocate(sizeof(UniformState));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto buffer = scratch.take_value();
    std::unique_ptr<UniformState, void (*)(UniformState*)> state(
        new (buffer.data()) UniformState(
            parameters(kernel, call.parameters),
            static_cast<unsigned>(
                std::get<std::int64_t>(call.parameters.at("axis"))),
            static_cast<unsigned>(
                std::get<std::int64_t>(call.parameters.at("radius"))),
            std::get<std::string>(call.parameters.at("boundary")), profile,
            call),
        [](auto* value) { value->~UniformState(); });
    return state->execute();
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}

OperationDefinition operation(const std::string& name, LowpassKernel kernel,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type_mask = 12;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64},
                             {"radius", OperationParameterType::Int64},
                             {"boundary", OperationParameterType::String}};
  traits.parameter_schema.push_back(
      {kernel == LowpassKernel::Gaussian ? "sigma" : "cutoff",
       OperationParameterType::Float64});
  if (kernel == LowpassKernel::Kaiser)
    traits.parameter_schema.push_back(
        {"beta", OperationParameterType::Float64});
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(UniformState);
  definition.specialize_metadata = [profile](const auto& inputs,
                                             const auto& p) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    if (inputs.size() != 1 || inputs[0].result_schema ||
        (inputs[0].descriptor.element_type != ElementType::Float32 &&
         inputs[0].descriptor.element_type != ElementType::Float64))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "lowpass input requires Float32/64",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& shape = inputs[0].descriptor.shape;
    std::uint64_t count = 1;
    if (shape.empty() || shape.size() > 8)
      return Answer(numeric_ops::array_parameter_error("lowpass rank 1..8"));
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(numeric_ops::array_parameter_error(
            "lowpass positive shape within 2^40"));
      count *= extent;
    }
    const auto axis = std::get<std::int64_t>(p.at("axis")),
               radius = std::get<std::int64_t>(p.at("radius"));
    const auto& boundary = std::get<std::string>(p.at("boundary"));
    if (axis < 0 || static_cast<std::uint64_t>(axis) >= shape.size() ||
        radius < 1 || radius > 4096 ||
        (boundary != "reflect" && boundary != "replicate" &&
         boundary != "zero" && boundary != "wrap"))
      return Answer(
          numeric_ops::array_parameter_error("lowpass axis/radius/boundary"));
    for (const auto& field : p) {
      if (!std::holds_alternative<double>(field.second))
        continue;
      const auto value =
          BinaryParts::decode(raw(std::get<double>(field.second)), false);
      if (value.nan || value.infinite || (value.negative && value.magnitude) ||
          (field.first != "beta" && !value.magnitude) ||
          (field.first == "cutoff" &&
           value.magnitude >= UINT64_C(0x3fe0000000000000)))
        return Answer(numeric_ops::array_parameter_error(
            "lowpass finite positive kernel parameter (beta>=0, cutoff<0.5)"));
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = inputs[0].descriptor;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  definition.callback = [kernel, profile](const OperationInvocation& call) {
    return execute_uniform(call, kernel, profile);
  };
  return definition;
}
}  // namespace
Status register_uniform_lowpass(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& kernel :
         {std::make_pair("hann_sinc", LowpassKernel::Hann),
          std::make_pair("hamming_sinc", LowpassKernel::Hamming),
          std::make_pair("blackman_sinc", LowpassKernel::Blackman),
          std::make_pair("kaiser_sinc", LowpassKernel::Kaiser),
          std::make_pair("gaussian", LowpassKernel::Gaussian)}) {
      auto status = registry->register_operation(operation(
          std::string("curve.lowpass_uniform_") + kernel.first + profile.first,
          kernel.second, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
