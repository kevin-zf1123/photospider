#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_calculus.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Result<ValueDescriptor> metadata(bool integral,
                                 const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<ValueDescriptor>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& member = inputs[0].result_schema->tensors[0];
  const ValueDescriptor source{member.descriptor.element_type,
                               member.sample_shape()};
  if ((source.element_type != ElementType::Float32 &&
       source.element_type != ElementType::Float64) ||
      source.shape.size() != 1 || source.shape[0] < (integral ? 1U : 2U) ||
      source.shape[0] > (UINT64_C(1) << 40))
    return mismatch("calculus requires Float32/64 [N] within 2^40");
  for (unsigned port = 1; port < inputs.size(); ++port)
    if (inputs[port].result_schema->tensors[0].descriptor.element_type !=
            source.element_type ||
        inputs[port].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{1})
      return mismatch("calculus controls require matching dtype and shape [1]");
  return Answer(source);
}
struct CalculusKernel final {
  bool integral;
  SequenceProfile profile;
  CalculusKernel(bool integrate, SequenceProfile selected)
      : integral(integrate), profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "calculus requires one packed writer"};
    const auto& work = phase.consume_work;
    auto status = work(1);
    if (!status.ok())
      return status;
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto count = shape[0];
    const auto type =
        phase.query.output.result_schema->tensors[0].descriptor.element_type;
    const auto width = Value::element_size(type);
    std::vector<std::uint64_t> coordinate(1, 0);
    std::array<std::optional<numeric_ops::MathTensorReader>, 3> readers;
    for (const auto& input : *phase.tensors)
      readers[input.first.first].emplace(input.second,
                                         phase.query.cancellation);
    numeric_ops::MathTensorWriter writer(writers[0]);
    const auto read = [&](std::size_t port,
                          std::uint64_t index) -> Result<std::uint64_t> {
      auto charged = work(2);
      if (!charged.ok())
        return Result<std::uint64_t>(charged);
      coordinate[0] = index;
      const auto bits = readers[port]->bits(coordinate);
      return Result<std::uint64_t>(bits);
    };
    std::uint64_t step = 0, initial = 0;
    if (!integral || count > 1) {
      auto bits = read(1, 0);
      if (!bits.ok())
        return bits.status();
      step = bits.value();
      const auto parts = numeric_ops::BinaryParts::decode(step, width == 4);
      if (!parts.magnitude || parts.nan || parts.infinite)
        return Status{ErrorCode::InvalidArgument,
                      "InvalidSampleStep: port=1 bits=" + std::to_string(step),
                      FailureReason::InvalidDomain,
                      {FailureOrigin::Domain, FailureScope::Run}};
    }
    if (integral) {
      auto bits = read(2, 0);
      if (!bits.ok())
        return bits.status();
      initial = bits.value();
    }
    const auto store = [&](std::uint64_t index, std::uint64_t bits) {
      std::array<std::uint64_t, 4> replicas{};
      numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
      coordinate[0] = index;
      std::memcpy(writer.address(coordinate), replicas.data(), width);
      return work(1);
    };
    if (integral) {
      status = store(0, initial);
      if (!status.ok())
        return status;
    }
    if (integral && count == 1)
      return work(1);
    auto scratch = phase.allocator.allocate(sizeof(numeric_ops::ExactCalculus));
    if (!scratch.ok())
      return scratch.status();
    auto buffer = scratch.take_value();
    std::unique_ptr<numeric_ops::ExactCalculus,
                    void (*)(numeric_ops::ExactCalculus*)>
        arithmetic(
            new (buffer.data()) numeric_ops::ExactCalculus(profile, type),
            [](numeric_ops::ExactCalculus* value) { value->~ExactCalculus(); });
    for (std::uint64_t index = 0; index < count; ++index) {
      Result<std::uint64_t> calculated(initial);
      if (integral) {
        auto bits = read(0, index);
        if (!bits.ok())
          return bits.status();
        status = arithmetic->add(bits.value(), work);
        if (!status.ok())
          return status;
        if (!index)
          continue;
        calculated = arithmetic->integral(step, initial, work);
      } else {
        const auto low = index == 0 ? 0 : index - 1,
                   high = index == count - 1 ? count - 1 : index + 1;
        auto a = read(0, low);
        if (!a.ok())
          return a.status();
        auto b = read(0, high);
        if (!b.ok())
          return b.status();
        calculated = arithmetic->derivative(a.value(), b.value(), step,
                                            high - low == 2, work);
      }
      if (!calculated.ok())
        return calculated.status();
      status = store(index, calculated.value());
      if (!status.ok())
        return status;
    }
    return work(1);
  }
};
using CalculusProgram = numeric_ops::WholeTensorProgram<CalculusKernel>;
OperationDefinition calculus_operation(const std::string& key, bool integral,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = integral ? 3 : 2;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(CalculusProgram));
  traits.workspace_bytes = sizeof(numeric_ops::ExactCalculus);
  operation.specialize_metadata =
      [integral, profile](
          const auto& inputs,
          const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(integral, inputs);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    const auto& output = resolved.value();
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(output.element_type, output.shape));
    if (integral && output.shape[0] == 1)
      result.input_indices = std::vector<std::uint32_t>{2};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [integral, profile](const auto&,
                                               const auto& allocator) {
    return ResultContinuation::make<CalculusProgram>(allocator, integral,
                                                     profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_calculus(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(calculus_operation(
          std::string("numeric.") +
              (integral ? "integrate_1d" : "derivative_1d") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
