#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_interpolation.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
enum class InterpolationKind { Mix, Smoothstep };
struct InterpolationMath final {
  InterpolationKind kind;
  numeric_ops::InterpolationWorkspace exact;
  std::array<std::uint64_t, 3> bits{};
  std::array<BinaryParts, 3> parts{};
  InterpolationMath(InterpolationKind operation, SequenceProfile profile)
      : kind(operation), exact(profile) {}
  Result<std::uint64_t> invalid(unsigned port) const {
    return Result<std::uint64_t>(
        Status{ErrorCode::InvalidArgument,
               std::string(kind == InterpolationKind::Mix ? "InvalidMixFactor"
                                                          : "InvalidEdges") +
                   ": port=" + std::to_string(port) +
                   " bits=" + std::to_string(bits[port]),
               FailureReason::InvalidDomain,
               {FailureOrigin::Domain, FailureScope::Run}});
  }
  Result<std::uint64_t> evaluate(
      ElementType type, const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto status = consume(128);
    if (!status.ok())
      return Answer(status);
    const bool narrow = type == ElementType::Float32;
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    for (unsigned port = 0; port < 3; ++port)
      parts[port] = BinaryParts::decode(bits[port], narrow);
    if (kind == InterpolationKind::Mix) {
      if (parts[2].nan || parts[2].infinite ||
          parts[2].order_key() < BinaryParts::decode(0, narrow).order_key() ||
          parts[2].order_key() > BinaryParts::decode(one, narrow).order_key())
        return invalid(2);
      if (!parts[2].magnitude || bits[2] == one)
        return Answer(bits[bits[2] == one ? 1 : 0]);
      if (parts[0].nan || parts[1].nan)
        return Answer(bits[parts[0].nan ? 0 : 1] | quiet);
      if (parts[0].infinite || parts[1].infinite)
        return Answer(parts[0].infinite && parts[1].infinite &&
                              parts[0].negative != parts[1].negative
                          ? infinity | quiet
                          : bits[parts[0].infinite ? 0 : 1]);
      return exact.mix(parts[0], parts[1], parts[2], narrow, consume);
    }
    for (unsigned port = 1; port < 3; ++port)
      if (parts[port].nan || parts[port].infinite)
        return invalid(port);
    if (parts[1].order_key() >= parts[2].order_key())
      return invalid(1);
    if (parts[0].nan)
      return Answer(bits[0] | quiet);
    if (parts[0].order_key() <= parts[1].order_key())
      return Answer(UINT64_C(0));
    if (parts[0].order_key() >= parts[2].order_key())
      return Answer(one);
    return exact.smoothstep(parts[0], parts[1], parts[2], narrow, consume);
  }
};
struct InterpolationKernel final {
  InterpolationKind kind;
  SequenceProfile profile;
  InterpolationKernel(InterpolationKind operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "interpolation requires one packed writer"};
    const auto& consume = phase.consume_work;
    auto status = consume(1);
    if (!status.ok())
      return status;
    const auto& input = phase.tensors->at({0, 0});
    const auto shape = input.spec().sample_shape();
    const auto type = input.spec().descriptor.element_type;
    const auto width = Value::element_size(type);
    auto scratch =
        math_take(phase.allocator.allocate(sizeof(InterpolationMath)));
    static_assert(alignof(InterpolationMath) <= alignof(std::max_align_t));
    std::unique_ptr<InterpolationMath, void (*)(InterpolationMath*)> math(
        new (scratch.data()) InterpolationMath(kind, profile),
        [](InterpolationMath* value) { value->~InterpolationMath(); });
    std::array<MathTensorReader, 3> readers{
        MathTensorReader(input, phase.query.cancellation),
        MathTensorReader(phase.tensors->at({1, 0}), phase.query.cancellation),
        MathTensorReader(phase.tensors->at({2, 0}), phase.query.cancellation)};
    MathTensorWriter writer(writers[0]);
    std::vector<uint64_t> coordinate(shape.size(), 0);
    const auto count =
        math_take(phase.query.output.result_schema->tensors[0].sample_count());
    for (uint64_t i = 0; i < count; ++i) {
      status = consume(3 * shape.size() + 1);
      if (!status.ok())
        return status;
      for (size_t port = 0; port < readers.size(); ++port)
        math->bits[port] = readers[port].bits(coordinate);
      auto result = math->evaluate(type, consume);
      if (!result.ok()) {
        auto failure = result.status();
        if (failure.detail.origin == FailureOrigin::Domain) {
          failure.message += " coordinate=[";
          for (size_t axis = 0; axis < coordinate.size(); ++axis) {
            if (axis)
              failure.message += ',';
            failure.message += std::to_string(coordinate[axis]);
          }
          failure.message += ']';
        }
        return failure;
      }
      const auto bits = result.value();
      std::memcpy(writer.address(coordinate), &bits, width);
      math_next(coordinate, shape);
    }
    return consume(1);
  }
};
using numeric_ops::WholeTensorProgram;
using InterpolationProgram = WholeTensorProgram<InterpolationKernel>;
OperationDefinition interpolation_operation(const std::string& key,
                                            InterpolationKind kind,
                                            SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 3;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 12;
  }
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(InterpolationProgram));
  traits.workspace_bytes = sizeof(InterpolationMath);
  operation.specialize_metadata = [profile](const auto& inputs, const auto&)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    const auto& member = inputs[0].result_schema->tensors[0];
    const ValueDescriptor first{member.descriptor.element_type,
                                member.sample_shape()};
    if (first.shape.empty() || first.shape.size() > 8)
      return mismatch("interpolation requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("interpolation input exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.result_schema->tensors[0].sample_shape() != first.shape ||
          input.result_schema->tensors[0].descriptor.element_type !=
              first.element_type)
        return mismatch(
            "interpolation operands require identical shape and dtype");
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(first.element_type, first.shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [kind, profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<InterpolationProgram>(allocator, kind,
                                                          profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_interpolation(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& item :
         {std::make_pair("mix", InterpolationKind::Mix),
          std::make_pair("smoothstep", InterpolationKind::Smoothstep)}) {
      auto status = registry->register_operation(interpolation_operation(
          std::string("numeric.") + item.first + variant.first, item.second,
          variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
