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
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
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
  const ResultProgramPhase& phase;
  numeric_ops::MathTensorReader input;
  std::function<Status(std::uint64_t)> consume;
  UniformState(LowpassParameters p, unsigned dimension, unsigned count,
               std::string extension, SequenceProfile selected,
               const ResultProgramPhase& invocation)
      : parameters(p),
        axis(dimension),
        radius(count),
        boundary(std::move(extension)),
        profile(selected),
        samples(ResourceAllocator<std::uint64_t>(invocation.resources)),
        coefficients(
            ResourceAllocator<numeric_ops::FastInterval>(invocation.resources)),
        phase(invocation),
        input(invocation.tensors->at({0, 0}), invocation.query.cancellation),
        consume(invocation.consume_work) {}
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
  Status execute(const ResultTensorWriteWindow& window) {
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto& descriptor = tensor.descriptor;
    const auto shape = tensor.sample_shape();
    samples.resize(2 * radius + 1);
    if (profile != SequenceProfile::Strict) {
      coefficients.resize(radius + 1);
      auto prepared = arithmetic.prepare(
          parameters, radius, coefficients.data(), &normalizer, consume);
      if (!prepared.ok())
        return prepared.status();
      coefficients_ready = prepared.value();
    }
    numeric_ops::MathTensorWriter output(window);
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    std::uint64_t count = 1;
    for (auto extent : shape)
      count *= extent;
    std::vector<std::uint64_t> coordinate(shape.size()), at(shape.size());
    std::optional<input_internal::Float32Environment> environment;
    if (coefficients_ready)
      environment.emplace();
    for (std::uint64_t row = 0; row < count; ++row) {
      at = coordinate;
      for (int offset = -static_cast<int>(radius);
           offset <= static_cast<int>(radius); ++offset) {
        auto status = consume(shape.size() + 1);
        if (!status.ok())
          return status;
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
        bits = input.bits(at);
      }
      std::optional<std::uint64_t> fast;
      if (coefficients_ready) {
        auto status = consume((radius + 1) * 32);
        if (!status.ok())
          return status;
        fast = arithmetic.fast(parameters, radius, samples.data(), narrow,
                               coefficients.data(), normalizer,
                               environment && environment->active());
      }
      auto value = fast ? Result<std::uint64_t>(*fast)
                        : arithmetic.evaluate(parameters, radius,
                                              samples.data(), narrow, consume);
      if (!value.ok())
        return value.status();
      auto status = consume(1);
      if (!status.ok())
        return status;
      const auto bits = value.value();
      std::memcpy(output.address(coordinate), &bits, width);
      for (auto i = shape.size(); i; --i) {
        if (++coordinate[i - 1] < shape[i - 1])
          break;
        coordinate[i - 1] = 0;
      }
    }
    auto status = consume(1);
    return status;
  }
};
struct UniformPrepared final {
  LowpassParameters parameters;
  unsigned axis;
  unsigned radius;
  std::string boundary;
  SequenceProfile profile;
};
struct UniformKernel final {
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    const auto& prepared =
        *static_cast<const UniformPrepared*>(phase.query.prepared->state());
    auto memory =
        numeric_ops::math_take(phase.allocator.allocate(sizeof(UniformState)));
    static_assert(alignof(UniformState) <= alignof(std::max_align_t));
    std::unique_ptr<UniformState, void (*)(UniformState*)> state(
        new (memory.data())
            UniformState(prepared.parameters, prepared.axis, prepared.radius,
                         prepared.boundary, prepared.profile, phase),
        [](auto* value) { value->~UniformState(); });
    return state->execute(writers[0]);
  }
};
using UniformProgram = numeric_ops::WholeTensorProgram<UniformKernel>;

OperationDefinition operation(const std::string& name, LowpassKernel kernel,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].kind = OperationPortKind::Result;
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
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(UniformProgram));
  traits.workspace_bytes = sizeof(UniformState);
  definition.prepare_static = [kernel, profile](const auto& inputs,
                                                const auto& p) {
    using Answer = Result<OperationPreparation>;
    if (inputs.size() != 1 || !inputs[0].result_schema ||
        !inputs[0].result_schema->fields.empty() ||
        inputs[0].result_schema->tensors.size() != 1 ||
        (inputs[0].result_schema->tensors[0].descriptor.element_type !=
             ElementType::Float32 &&
         inputs[0].result_schema->tensors[0].descriptor.element_type !=
             ElementType::Float64))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "lowpass input requires Float32/64",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& tensor = inputs[0].result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
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
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(tensor.descriptor.element_type,
                                           shape));
    OperationPreparation prepared;
    prepared.outputs.push_back(std::move(resolved));
    prepared.state = std::make_shared<const UniformPrepared>(
        UniformPrepared{parameters(kernel, p), static_cast<unsigned>(axis),
                        static_cast<unsigned>(radius), boundary, profile});
    return Answer(std::move(prepared));
  };
  definition.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<UniformProgram>(allocator);
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
