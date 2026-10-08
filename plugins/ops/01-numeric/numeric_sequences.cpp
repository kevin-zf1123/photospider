#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_sequence.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/port_validation.hpp"
namespace ps::plugin_internal {
namespace {
using numeric_ops::ExactSequence;
using numeric_ops::SequenceProfile;
enum class Sequence { Linspace, Arange };
struct SequenceMath {
  SequenceProfile profile;
  ExactSequence first, second;
  std::array<std::uint64_t, 68> products{};
  explicit SequenceMath(SequenceProfile selected) : profile(selected) {}
  std::uint64_t rounded(double a, double b, std::uint32_t wa, std::uint32_t wb,
                        std::uint32_t divisor, bool binary32,
                        bool subtract = false) {
    first.set(a);
    second.set(b);
    numeric_ops::sequence_multiply(&first, wa, profile, products.data());
    numeric_ops::sequence_multiply(&second, wb, profile, products.data());
    if (subtract)
      second.negative = !second.negative;
    first.add(second);
    const bool zero_sign =
        !subtract && std::signbit(a) && (!wb || std::signbit(b));
    return first.rounded_bits(divisor, binary32, zero_sign);
  }

  static bool overflow(std::uint64_t bits, bool binary32) {
    return binary32 ? (bits & UINT64_C(0x7f800000)) == UINT64_C(0x7f800000)
                    : (bits & UINT64_C(0x7ff0000000000000)) ==
                          UINT64_C(0x7ff0000000000000);
  }
};
struct SequenceKernel final {
  Sequence kind;
  SequenceProfile profile;
  SequenceKernel(Sequence operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "sequence requires one packed writer"};
    const auto& consume = phase.consume_work;
    const auto failure = [](FailureReason reason, const std::string& message) {
      return Status{ErrorCode::OperationFailed,
                    message,
                    reason,
                    {FailureOrigin::Domain, FailureScope::Run}};
    };
    auto status = consume(1);
    if (!status.ok())
      return status;
    const auto count = static_cast<std::uint32_t>(
        std::get<std::int64_t>(phase.query.parameters.at("count")));
    const auto& dtype =
        std::get<std::string>(phase.query.parameters.at("dtype"));
    const bool integer = dtype == "int64", axis = phase.query.output_index == 1;
    const auto type = integer                      ? ElementType::Int64
                      : axis || dtype == "float64" ? ElementType::Float64
                                                   : ElementType::Float32;
    numeric_ops::MathTensorWriter writer(writers[0]);
    auto storage = phase.allocator.allocate(sizeof(SequenceMath));
    if (!storage.ok())
      return storage.status();
    auto scratch = storage.take_value();
    std::unique_ptr<SequenceMath, void (*)(SequenceMath*)> math(
        new (scratch.data()) SequenceMath(profile),
        [](SequenceMath* p) { p->~SequenceMath(); });
    input_internal::Float32Environment environment;
    if (!integer && !environment.active())
      return Status{ErrorCode::BackendUnavailable,
                    "numeric environment unavailable"};
    std::array<std::uint64_t, 2> words{};
    double start = 0, other = 0;
    for (unsigned port = 0; port < (count == 1 ? 1U : 2U); ++port) {
      const auto& input = phase.tensors->at({port, 0});
      numeric_ops::MathTensorReader reader(input, phase.query.cancellation);
      const auto width =
          Value::element_size(input.spec().descriptor.element_type);
      words[port] = reader.bits({0});
      if (!integer) {
        double number = 0;
        if (width == 4) {
          float small = 0;
          std::memcpy(&small, &words[port], 4);
          number = small;
        } else {
          std::memcpy(&number, &words[port], 8);
        }
        if (!std::isfinite(number))
          return failure(FailureReason::InvalidDomain,
                         port ? "nonfinite end/step" : "nonfinite start");
        (port ? other : start) = number;
      }
    }
    const auto width = Value::element_size(type);
    for (std::uint32_t i = 0; i < (axis ? 1U : count); ++i) {
      status = consume(axis ? 24576 : 8192);
      if (!status.ok())
        return status;
      const auto index = axis ? count - 1 : i;
      std::array<std::uint64_t, 3> result{};
      if (integer) {
        std::int64_t base = 0, step = 0;
        std::memcpy(&base, &words[0], 8);
        std::memcpy(&step, &words[1], 8);
        math->first.set_integer(base);
        math->second.set_integer(step);
        numeric_ops::sequence_multiply(&math->second, index, profile,
                                       math->products.data());
        math->first.add(math->second);
        const auto magnitude =
            static_cast<std::uint64_t>(math->first.words[0]) |
            (static_cast<std::uint64_t>(math->first.words[1]) << 32);
        bool fits =
            magnitude <= (math->first.negative ? UINT64_C(1) << 63 : INT64_MAX);
        for (unsigned j = 2; j < math->first.words.size(); ++j)
          fits &= math->first.words[j] == 0;
        if (!fits)
          return failure(
              FailureReason::ArithmeticOverflow,
              "integer sequence overflow index=" + std::to_string(index));
        result[0] = math->first.negative ? UINT64_C(0) - magnitude : magnitude;
        if (axis)
          result = {words[0], result[0], words[1]};
      } else if (axis) {
        result[0] = math->rounded(start, 0, 1, 0, 1, false);
        result[1] = result[0];
        if (count > 1 && kind == Sequence::Linspace) {
          result[1] = math->rounded(other, 0, 1, 0, 1, false);
          result[2] = math->rounded(other, start, 1, 1, count - 1, false, true);
          if (SequenceMath::overflow(result[2], false))
            return failure(FailureReason::ArithmeticOverflow,
                           "axis step overflow");
        } else if (count > 1) {
          result[1] = math->rounded(start, other, 1, count - 1, 1, false);
          result[2] = math->rounded(other, 0, 1, 0, 1, false);
          if (SequenceMath::overflow(result[1], false))
            return failure(FailureReason::ArithmeticOverflow,
                           "axis last overflow");
        }
      } else {
        const bool narrow = type == ElementType::Float32;
        if (kind == Sequence::Linspace && count > 1 && index > 0 &&
            index + 1 < count)
          result[0] = math->rounded(start, other, count - 1 - index, index,
                                    count - 1, narrow);
        else if (kind == Sequence::Arange && index > 0)
          result[0] = math->rounded(start, other, 1, index, 1, narrow);
        else
          result[0] = math->rounded(
              kind == Sequence::Linspace && count > 1 && index + 1 == count
                  ? other
                  : start,
              0, 1, 0, 1, narrow);
        if (SequenceMath::overflow(result[0], narrow))
          return failure(
              FailureReason::ArithmeticOverflow,
              "sequence value overflow index=" + std::to_string(index));
      }
      if (axis) {
        for (uint64_t component = 0; component < 3; ++component)
          std::memcpy(writer.address({component}), &result[component], 8);
      } else {
        std::memcpy(writer.address({i}), result.data(), width);
      }
    }
    return consume(1);
  }
};
using SequenceProgram = numeric_ops::WholeTensorProgram<SequenceKernel>;

OperationDefinition sequence(const std::string& key, Sequence kind,
                             SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.rank = 1;
    port.element_type_mask = kind == Sequence::Linspace ? 12 : 14;
  }
  traits.parameter_schema = {
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  traits.outputs.resize(2);
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(SequenceProgram), 0);
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(SequenceProgram), 1);
  traits.outputs[1].key = "axis";
  traits.outputs[1].result_schema->tensors[0].descriptor.shape = {3};
  traits.outputs[1].result_schema->tensors[0].atomic_trailing_axes = 1;
  traits.workspace_bytes = sizeof(SequenceMath);
  traits.requires_metadata_specialization = true;
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const bool integer = kind == Sequence::Arange && dtype == "int64";
    if (!integer && dtype != "float32" && dtype != "float64")
      return Answer(Status{dtype == "uint8" || dtype == "int64"
                               ? ErrorCode::TypeMismatch
                               : ErrorCode::InvalidArgument,
                           "invalid sequence dtype",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    for (const auto& input : inputs) {
      const auto& member = input.result_schema->tensors[0];
      if (member.sample_shape() != std::vector<std::uint64_t>{1} ||
          (integer
               ? member.descriptor.element_type != ElementType::Int64
               : member.descriptor.element_type != ElementType::Float32 &&
                     member.descriptor.element_type != ElementType::Float64))
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "sequence requires matching numeric-kind scalars",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    const auto count = static_cast<std::uint64_t>(
        std::get<std::int64_t>(parameters.at("count")));
    std::vector<OperationOutputSpecialization> result(2);
    result[0].metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(integer ? ElementType::Int64
                                           : dtype == "float32"
                                               ? ElementType::Float32
                                               : ElementType::Float64,
                                           {count}));
    auto axis_schema = numeric_ops::numeric_tensor_schema(
        integer ? ElementType::Int64 : ElementType::Float64, {3});
    axis_schema.tensors[0].atomic_trailing_axes = 1;
    result[1].metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(axis_schema));
    for (auto& output : result)
      output.input_indices = count == 1 ? std::vector<std::uint32_t>{0}
                                        : std::vector<std::uint32_t>{0, 1};
    return Answer(std::move(result));
  };
  operation.start_result = [kind, profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<SequenceProgram>(allocator, kind, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_sequences(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("numeric.linspace", Sequence::Linspace),
        std::make_pair("numeric.arange", Sequence::Arange)}) {
    for (const auto& variant :
         {std::make_pair("_strict", SequenceProfile::Strict),
          std::make_pair("_accelerated_apple_silicon",
                         SequenceProfile::AppleSilicon),
          std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
      auto status = registry->register_operation(
          sequence(std::string(entry.first) + variant.first, entry.second,
                   variant.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
