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
#include "01-numeric/numeric_tensor_program.hpp"
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
struct RangeKernel final {
  RangeKind kind;
  SequenceProfile profile;
  RangeKernel(RangeKind kind, SequenceProfile profile)
      : kind(kind), profile(profile) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed, "range requires one packed writer"};
    const auto& target = phase.query.output.result_schema->tensors[0];
    const auto shape = target.sample_shape();
    const auto width = Value::element_size(target.descriptor.element_type);
    auto scratch = math_take(phase.allocator.allocate(sizeof(RangeMath)));
    static_assert(alignof(RangeMath) <= alignof(std::max_align_t));
    std::unique_ptr<RangeMath, void (*)(RangeMath*)> math(
        new (scratch.data()) RangeMath(kind, profile),
        [](RangeMath* value) { value->~RangeMath(); });
    std::array<std::optional<MathTensorReader>, 5> inputs;
    const auto ports = kind == RangeKind::Clamp ? 3U : 5U;
    for (unsigned port = 0; port < ports; ++port)
      inputs[port].emplace(phase.tensors->at({port, 0}),
                           phase.query.cancellation);
    MathTensorWriter writer(writers[0]);
    std::vector<uint64_t> coordinate(shape.size(), 0);
    for (uint64_t i = 0, count = math_take(target.sample_count()); i < count;
         ++i) {
      math_require(phase.consume_work(ports * shape.size() + 1));
      for (unsigned port = 0; port < ports; ++port)
        math->bits[port] = inputs[port]->bits(coordinate);
      auto result =
          math->evaluate(target.descriptor.element_type, phase.consume_work);
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
    return phase.consume_work(1);
  }
};
using RangeProgram = numeric_ops::WholeTensorProgram<RangeKernel>;
OperationDefinition range_operation(const std::string& key, RangeKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == RangeKind::Clamp ? 3 : 5;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = kind == RangeKind::Clamp ? 15 : 12;
  }
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(RangeProgram));
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
    const auto& selected = inputs[0].result_schema->tensors[0];
    const ValueDescriptor first{selected.descriptor.element_type,
                                selected.sample_shape()};
    if (first.shape.empty() || first.shape.size() > 8)
      return mismatch("range requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("range input exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.result_schema->tensors[0].sample_shape() != first.shape ||
          input.result_schema->tensors[0].descriptor.element_type !=
              first.element_type)
        return mismatch("range operands require identical shape and dtype");
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
    return ResultContinuation::make<RangeProgram>(allocator, kind, profile);
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
