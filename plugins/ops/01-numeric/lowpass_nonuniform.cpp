#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/lowpass_nonuniform_math.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
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
  result.radius = raw(std::get<double>(input.at("support_radius")));
  if (kernel == LowpassKernel::Gaussian)
    result.sigma = raw(std::get<double>(input.at("sigma")));
  else
    result.cutoff = raw(std::get<double>(input.at("cutoff")));
  if (kernel == LowpassKernel::Kaiser)
    result.beta = raw(std::get<double>(input.at("beta")));
  return result;
}
struct NonuniformState {
  LowpassParameters parameters;
  unsigned axis;
  std::string boundary;
  numeric_ops::NonuniformLowpassMath arithmetic;
  ResourceVector<std::uint64_t> positions;
  ResourceVector<numeric_ops::LowpassPiece> pieces;
  const ResultProgramPhase& phase;
  std::array<numeric_ops::MathTensorReader, 2> inputs;
  std::function<Status(std::uint64_t)> consume;
  NonuniformState(LowpassParameters p, unsigned dimension,
                  std::string extension, const ResultProgramPhase& invocation)
      : parameters(p),
        axis(dimension),
        boundary(std::move(extension)),
        positions(ResourceAllocator<std::uint64_t>(invocation.resources)),
        pieces(
            ResourceAllocator<numeric_ops::LowpassPiece>(invocation.resources)),
        phase(invocation),
        inputs{numeric_ops::MathTensorReader(invocation.tensors->at({0, 0}),
                                             invocation.query.cancellation),
               numeric_ops::MathTensorReader(invocation.tensors->at({1, 0}),
                                             invocation.query.cancellation)},
        consume(invocation.consume_work) {}
  Status failure(const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    return {ErrorCode::OperationFailed,
            message,
            reason,
            {FailureOrigin::Domain, FailureScope::Run}};
  }
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& at) {
    auto charged = consume(at.size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const bool narrow = phase.query.inputs[port]
                            .result_schema->tensors[0]
                            .descriptor.element_type == ElementType::Float32;
    std::uint64_t bits = inputs[port].bits(at);
    auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure("nonfinite nonuniform lowpass input"));
    if (!port && narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!value.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(value.significand);
        bits = sign |
               (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
               ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
      }
    }
    return Result<std::uint64_t>(bits);
  }
  Status partition(std::uint64_t center) {
    pieces.clear();
    auto& m = arithmetic.kernel.functions.math;
    m.used = 0;
    m.precision = 1074;
    m.consume = &consume;
    struct End {
      numeric_ops::DirectedInterval& m;
      ~End() {
        m.consume = nullptr;
        m.used = 0;
      }
    } end{m};
    try {
      numeric_ops::LowpassGeometry geometry(m);
      geometry.partition(&pieces, positions.data(), positions.size(), center,
                         parameters.radius, boundary);
      return Status::success();
    } catch (const Status& status) {
      return status;
    }
  }
  Status execute(const ResultTensorWriteWindow& window) {
    positions.resize(
        phase.query.inputs[0].result_schema->tensors[0].sample_shape()[0]);
    for (unsigned i = 0; i < positions.size(); ++i) {
      auto value = read(0, {i});
      if (!value.ok())
        return value.status();
      positions[i] = value.value();
      if (i && BinaryParts::decode(positions[i - 1], false).order_key() >=
                   BinaryParts::decode(positions[i], false).order_key())
        return failure("nonuniform positions require strict increase");
    }
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto& descriptor = tensor.descriptor;
    const auto shape = tensor.sample_shape();
    numeric_ops::MathTensorWriter output(window);
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    std::uint64_t count = 1;
    for (auto extent : shape)
      count *= extent;
    std::vector<std::uint64_t> coordinate(shape.size()), at(shape.size());
    for (std::uint64_t row = 0; row < count; ++row) {
      auto status = partition(coordinate[axis]);
      if (!status.ok())
        return status;
      at = coordinate;
      for (auto& piece : pieces) {
        status = consume(1);
        if (!status.ok())
          return status;
        if (piece.first == UINT64_MAX)
          continue;
        at[axis] = piece.first;
        auto first = read(1, at);
        if (!first.ok())
          return first.status();
        piece.first_value = first.value();
        at[axis] = piece.last;
        auto last = read(1, at);
        if (!last.ok())
          return last.status();
        piece.last_value = last.value();
      }
      auto value = arithmetic.evaluate(pieces, positions[coordinate[axis]],
                                       parameters, narrow, consume);
      if (!value.ok())
        return value.status();
      const auto bits = value.value();
      if (BinaryParts::decode(bits, narrow).infinite)
        return failure("nonuniform lowpass output overflow",
                       FailureReason::ArithmeticOverflow);
      status = consume(1);
      if (!status.ok())
        return status;
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
struct NonuniformPrepared final {
  LowpassParameters parameters;
  unsigned axis;
  std::string boundary;
  SequenceProfile profile;
};
struct NonuniformKernel final {
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    const auto& prepared =
        *static_cast<const NonuniformPrepared*>(phase.query.prepared->state());
    auto memory = numeric_ops::math_take(
        phase.allocator.allocate(sizeof(NonuniformState)));
    static_assert(alignof(NonuniformState) <= alignof(std::max_align_t));
    std::unique_ptr<NonuniformState, void (*)(NonuniformState*)> state(
        new (memory.data()) NonuniformState(prepared.parameters, prepared.axis,
                                            prepared.boundary, phase),
        [](auto* value) { value->~NonuniformState(); });
    return state->execute(writers[0]);
  }
};
using NonuniformProgram = numeric_ops::WholeTensorProgram<NonuniformKernel>;

OperationDefinition operation(const std::string& name, LowpassKernel kernel,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 12;
  }
  traits.parameter_schema = {
      {"axis", OperationParameterType::Int64},
      {"support_radius", OperationParameterType::Float64},
      {"boundary", OperationParameterType::String},
      {kernel == LowpassKernel::Gaussian ? "sigma" : "cutoff",
       OperationParameterType::Float64}};
  if (kernel == LowpassKernel::Kaiser)
    traits.parameter_schema.push_back(
        {"beta", OperationParameterType::Float64});
  traits.requires_metadata_specialization = true;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(NonuniformProgram));
  traits.outputs[0].key = "samples";
  traits.workspace_bytes = sizeof(NonuniformState);
  definition.prepare_static = [kernel, profile](const auto& inputs,
                                                const auto& p) {
    using Answer = Result<OperationPreparation>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 2)
      return Answer(mismatch("nonuniform lowpass ports"));
    for (const auto& input : inputs)
      if (!input.result_schema || !input.result_schema->fields.empty() ||
          input.result_schema->tensors.size() != 1 ||
          (input.result_schema->tensors[0].descriptor.element_type !=
               ElementType::Float32 &&
           input.result_schema->tensors[0].descriptor.element_type !=
               ElementType::Float64))
        return Answer(mismatch("nonuniform lowpass Float32/64 inputs"));
    if (inputs[0].result_schema->tensors[0].sample_shape().size() != 1)
      return Answer(mismatch("nonuniform positions rank one"));
    const auto k = inputs[0].result_schema->tensors[0].sample_shape()[0];
    const auto& tensor = inputs[1].result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
    const auto axis = std::get<std::int64_t>(p.at("axis"));
    const auto& boundary = std::get<std::string>(p.at("boundary"));
    if (k < 2 || k > 1048576 || shape.empty() || shape.size() > 8 || axis < 0 ||
        static_cast<std::uint64_t>(axis) >= shape.size() ||
        (boundary != "reflect" && boundary != "replicate" &&
         boundary != "zero" && boundary != "wrap"))
      return Answer(numeric_ops::array_parameter_error(
          "nonuniform counts/axis/boundary"));
    if (shape[axis] != k)
      return Answer(mismatch("nonuniform positions/value axis mismatch"));
    std::uint64_t count = 1;
    for (auto size : shape) {
      if (!size || size > (UINT64_C(1) << 40) / count)
        return Answer(
            numeric_ops::array_parameter_error("nonuniform shape count"));
      count *= size;
    }
    for (const auto& entry : p)
      if (const auto* real = std::get_if<double>(&entry.second)) {
        const auto value = BinaryParts::decode(raw(*real), false);
        if (value.nan || value.infinite ||
            (value.negative && value.magnitude) ||
            (!value.magnitude && entry.first != "beta"))
          return Answer(numeric_ops::array_parameter_error(
              "nonuniform finite positive parameters (beta>=0)"));
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
    prepared.state = std::make_shared<const NonuniformPrepared>(
        NonuniformPrepared{parameters(kernel, p), static_cast<unsigned>(axis),
                           boundary, profile});
    return Answer(std::move(prepared));
  };
  definition.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<NonuniformProgram>(allocator);
  };
  return definition;
}
}  // namespace
Status register_nonuniform_lowpass(OperationRegistry* registry) {
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
      auto status = registry->register_operation(
          operation(std::string("curve.lowpass_nonuniform_") + kernel.first +
                        profile.first,
                    kernel.second, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
