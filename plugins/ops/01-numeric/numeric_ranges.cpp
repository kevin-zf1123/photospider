#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_ratio.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
enum class RangeKind { Clamp, Remap };
struct RangeMath final {
  RangeKind kind;
  SequenceProfile profile;
  numeric_ops::RatioWorkspace ratio;
  std::array<std::uint64_t, 5> bits{};
  std::array<BinaryParts, 5> parts{};
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  RangeMath(RangeKind operation, SequenceProfile selected)
      : kind(operation), profile(selected), ratio(selected) {}
  Result<std::uint64_t> invalid(std::size_t port) const {
    return Result<std::uint64_t>(
        Status{ErrorCode::InvalidArgument,
               "InvalidBounds: port=" + std::to_string(port) +
                   " bits=" + std::to_string(bits[port]),
               FailureReason::InvalidDomain,
               {FailureOrigin::Domain, FailureScope::Run}});
  }
  Result<std::uint64_t> evaluate(
      ElementType type, const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    const auto count = kind == RangeKind::Clamp ? 3U : 5U;
    auto status = consume(128);
    if (!status.ok())
      return Answer(status);
    const bool narrow = type == ElementType::Float32;
    const bool floating = narrow || type == ElementType::Float64;
    if (floating) {
      for (unsigned port = 0; port < count; ++port)
        parts[port] = BinaryParts::decode(bits[port], narrow);
    }
    const auto key = [&](std::uint32_t port) {
      return floating                     ? parts[port].order_key()
             : type == ElementType::Int64 ? bits[port] ^ (UINT64_C(1) << 63)
                                          : bits[port];
    };
    left = {key(0), key(0), key(1), 0};
    right = {key(1), key(2), key(2), 0};
    numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                              less.data(), profile);
    std::uint64_t output = 0;
    if (kind == RangeKind::Clamp) {
      if (floating && parts[1].nan)
        return invalid(1);
      if (floating && parts[2].nan)
        return invalid(2);
      if (greater[2])
        return invalid(1);
      output = floating && parts[0].nan
                   ? bits[0] | (UINT64_C(1) << (narrow ? 22 : 51))
               : less[0]    ? bits[1]
               : greater[1] ? bits[2]
                            : bits[0];
    } else {
      for (unsigned port = 1; port < count; ++port)
        if (parts[port].nan || parts[port].infinite)
          return invalid(port);
      if (!less[2])
        return invalid(1);
      if (parts[0].nan) {
        output = bits[0] | (UINT64_C(1) << (narrow ? 22 : 51));
      } else if (key(3) == key(4)) {
        output = bits[3];
      } else if (key(0) == key(1)) {
        output = bits[3];
      } else if (key(0) == key(2)) {
        output = bits[4];
      } else if (parts[0].infinite) {
        const bool negative = parts[0].negative != (key(4) < key(3));
        output = (static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63)) |
                 (narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
      } else {
        status = consume(2048);
        if (!status.ok())
          return Answer(status);
        // Form a strictly positive denominator in units 2^-1074.
        ratio.numerator.set(parts[2], 1074);
        ratio.negative = parts[2].negative;
        ratio.term.set(parts[1], 1074);
        ratio.add_term(!parts[1].negative);
        ratio.denominator = ratio.numerator;
        ratio.numerator.words.fill(0);
        ratio.negative = false;
        // Algebraically expand the whole formula before its single rounding.
        ratio.product_term(parts[3], parts[2]);
        ratio.product_term(parts[0], parts[4]);
        ratio.product_term(parts[0], parts[3], true);
        ratio.product_term(parts[1], parts[4], true);
        auto rounded = ratio.round(narrow, consume, -1074, true);
        if (!rounded.ok())
          return Answer(rounded.status());
        output = rounded.take_value();
      }
    }
    return Answer(output);
  }
};
Result<Value> execute_range(const OperationInvocation& call, RangeKind kind,
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
  auto storage = call.allocator.allocate(sizeof(RangeMath));
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  static_assert(alignof(RangeMath) <= alignof(std::max_align_t));
  std::unique_ptr<RangeMath, void (*)(RangeMath*)> math(
      new (scratch.data()) RangeMath(kind, profile),
      [](RangeMath* value) { value->~RangeMath(); });
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  std::array<const std::uint8_t*, 5> packed{};
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
OperationDefinition range_operation(const std::string& key, RangeKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == RangeKind::Clamp ? 3 : 5;
  traits.input_schema.resize(traits.input_count);
  if (kind == RangeKind::Remap)
    for (auto& input : traits.input_schema)
      input.element_type_mask = 12;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(RangeMath);
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
      return mismatch("range requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("range input exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.descriptor.shape != first.shape ||
          input.descriptor.element_type != first.element_type)
        return mismatch("range operands require identical shape and dtype");
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = first;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [kind, profile](const OperationInvocation& call) {
    return execute_range(call, kind, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_ranges(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(
        range_operation(std::string("numeric.clamp") + variant.first,
                        RangeKind::Clamp, variant.second));
    if (!status.ok())
      return status;
    status = registry->register_operation(
        range_operation(std::string("numeric.remap_range") + variant.first,
                        RangeKind::Remap, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
