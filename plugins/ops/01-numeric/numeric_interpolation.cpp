#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_interpolation.hpp"
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
Result<Value> execute_interpolation(const OperationInvocation& call,
                                    InterpolationKind kind,
                                    SequenceProfile profile) {
  using Answer = Result<Value>;
  const auto* budget = resource_internal::metadata_budget();
  const std::function<Status(std::uint64_t)> consume = [&](std::uint64_t work) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return budget ? budget->consume({work}) : Status::success();
  };
  auto status = consume(1);
  if (!status.ok())
    return Answer(status);
  const auto& descriptor = call.inputs[0].descriptor();
  const auto& shape = descriptor.shape;
  const auto width = Value::element_size(descriptor.element_type);
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  auto storage = call.allocator.allocate(sizeof(InterpolationMath));
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  static_assert(alignof(InterpolationMath) <= alignof(std::max_align_t));
  std::unique_ptr<InterpolationMath, void (*)(InterpolationMath*)> math(
      new (scratch.data()) InterpolationMath(kind, profile),
      [](InterpolationMath* value) { value->~InterpolationMath(); });
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  std::array<const std::uint8_t*, 3> packed{};
  for (std::size_t port = 0; port < call.inputs.size(); ++port) {
    const auto& input = call.inputs[port];
    std::uint64_t stride = width;
    bool dense = true;
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (shape[axis - 1] > 1 && input.layout().byte_strides[axis - 1] !=
                                     static_cast<std::int64_t>(stride))
        dense = false;
      stride *= shape[axis - 1];
    }
    if (dense) {
      auto address = input.byte_address(coordinate);
      if (!address.ok())
        return Answer(address.status());
      packed[port] = input.bytes().data() + address.value();
    }
  }
  const auto count = call.output_region.element_count().value();
  for (std::uint64_t i = 0; i < count; ++i) {
    status = consume(call.inputs.size() * shape.size() + 1);
    if (!status.ok())
      return Answer(status);
    for (std::size_t port = 0; port < call.inputs.size(); ++port) {
      const auto& input = call.inputs[port];
      const auto* data = packed[port];
      if (data) {
        data += i * width;
      } else {
        auto address = input.byte_address(coordinate);
        if (!address.ok())
          return Answer(address.status());
        data = input.bytes().data() + address.value();
      }
      math->bits[port] = 0;
      if (width == 8)
        std::memcpy(&math->bits[port], data, 8);
      else if (width == 4)
        std::memcpy(&math->bits[port], data, 4);
      else
        math->bits[port] = *data;
    }
    auto result = math->evaluate(descriptor.element_type, consume);
    if (!result.ok()) {
      auto failure = result.status();
      if (failure.detail.origin == FailureOrigin::Domain) {
        failure.message += " coordinate=[";
        for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
          if (axis)
            failure.message += ',';
          failure.message += std::to_string(coordinate[axis]);
        }
        failure.message += ']';
      }
      return Answer(failure);
    }
    const auto bits = result.value();
    auto* destination = output.data() + i * width;
    if (width == 8)
      std::memcpy(destination, &bits, 8);
    else if (width == 4)
      std::memcpy(destination, &bits, 4);
    else
      *destination = static_cast<std::uint8_t>(bits);
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (++coordinate[axis - 1] < shape[axis - 1])
        break;
      coordinate[axis - 1] = 0;
    }
  }
  status = consume(1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
OperationDefinition interpolation_operation(const std::string& key,
                                            InterpolationKind kind,
                                            SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 3;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
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
    const auto& first = inputs[0].descriptor;
    if (first.shape.empty() || first.shape.size() > 8)
      return mismatch("interpolation requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("interpolation input exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.descriptor.shape != first.shape ||
          input.descriptor.element_type != first.element_type)
        return mismatch(
            "interpolation operands require identical shape and dtype");
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = first;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_interpolation(call, kind, profile);
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
