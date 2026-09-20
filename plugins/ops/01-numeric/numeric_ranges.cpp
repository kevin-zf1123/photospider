#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/exact_ratio.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
enum class RangeKind { Clamp, Remap };
struct RangeState final {
  RangeKind kind;
  SequenceProfile profile;
  bool ready = false;
  numeric_ops::RatioWorkspace ratio;
  std::array<std::uint64_t, 5> bits{};
  std::array<BinaryParts, 5> parts{};
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  RangeState(RangeKind operation, SequenceProfile selected)
      : kind(operation), profile(selected), ratio(selected) {}
  Status report(const DependencyPhase& phase) const {
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto* isa = profile == SequenceProfile::Strict         ? "scalar-u64"
                      : profile == SequenceProfile::AppleSilicon ? "NEON-u64x2"
                                                                 : "AVX2-u64x4";
    const auto length = std::snprintf(
        diagnostics.implementation.data(), diagnostics.implementation.size(),
        "photospider.range/1;%s;%s%s",
        kind == RangeKind::Clamp ? "bounds-bitselect" : "exact-rational-round",
        isa, numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostics.implementation.size())
      return Status{ErrorCode::OperationFailed,
                    "numeric implementation identity too long"};
    diagnostics.evaluated_values = 1;
    return phase.report_numeric(diagnostics);
  }
  Result<DependencyPoll> invalid(const DependencyPhase& phase,
                                 std::size_t port) const {
    Status status{ErrorCode::InvalidArgument,
                  "InvalidBounds: port=" + std::to_string(port) +
                      " bits=" + std::to_string(bits[port]),
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    auto atom = dependency_atom_key(phase.query);
    if (atom.ok())
      status.detail.atom = atom.take_value();
    return Result<DependencyPoll>(status);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    const auto count = kind == RangeKind::Clamp ? 3U : 5U;
    if (!ready) {
      ready = true;
      std::vector<DependencyNeed> needs;
      for (std::uint32_t port = 0; port < count; ++port) {
        auto closure = input_internal::validation_closure(
            phase.query.inputs[port], phase.query.outputs, phase.sets,
            phase.consume_work);
        if (!closure.ok())
          return Answer(closure.status());
        auto validation = closure.take_value();
        needs.push_back({port, 1, phase.query.outputs, {}});
        needs.push_back({port, 4, std::move(validation), {}});
      }
      return multi_output::need(phase, std::move(needs));
    }
    auto status = phase.consume_work(128);
    if (!status.ok())
      return Answer(status);
    const auto type = phase.query.output.descriptor.element_type;
    const bool narrow = type == ElementType::Float32;
    const bool floating = narrow || type == ElementType::Float64;
    const auto width = Value::element_size(type);
    const auto coordinate = multi_output::coordinate(phase);
    for (std::uint32_t port = 0; port < count; ++port) {
      status = phase.read(port, coordinate, &bits[port], width);
      if (!status.ok())
        return Answer(status);
      if (floating)
        parts[port] = BinaryParts::decode(bits[port], narrow);
    }
    status = report(phase);
    if (!status.ok())
      return Answer(status);
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
        return invalid(phase, 1);
      if (floating && parts[2].nan)
        return invalid(phase, 2);
      if (greater[2])
        return invalid(phase, 1);
      output = floating && parts[0].nan
                   ? bits[0] | (UINT64_C(1) << (narrow ? 22 : 51))
               : less[0]    ? bits[1]
               : greater[1] ? bits[2]
                            : bits[0];
    } else {
      for (unsigned port = 1; port < count; ++port)
        if (parts[port].nan || parts[port].infinite)
          return invalid(phase, port);
      if (!less[2])
        return invalid(phase, 1);
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
        status = phase.consume_work(2048);
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
        auto rounded = ratio.round(narrow, phase.consume_work);
        if (!rounded.ok())
          return Answer(rounded.status());
        output = rounded.take_value();
      }
    }
    return multi_output::finish(phase, &output, width);
  }
};
OperationDefinition range_operation(const std::string& key, RangeKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = kind == RangeKind::Clamp ? 3 : 5;
  traits.input_schema.resize(traits.input_count);
  if (kind == RangeKind::Remap)
    for (auto& input : traits.input_schema)
      input.element_type_mask = 12;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(RangeState);
  output.maximum_dependency_stages = 2;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  operation.validate_dependency = [profile](const auto& inputs, const auto&) {
    for (const auto& input : inputs)
      if (input.descriptor.element_type != inputs[0].descriptor.element_type)
        return Status{ErrorCode::TypeMismatch,
                      "range operand dtypes must match",
                      FailureReason::None,
                      {FailureOrigin::Schema, FailureScope::Unspecified}};
    std::uint64_t count = 1;
    for (auto extent : inputs[0].descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Status{ErrorCode::TypeMismatch,
                      "range input exceeds 2^40 elements",
                      FailureReason::None,
                      {FailureOrigin::Schema, FailureScope::Unspecified}};
      count *= extent;
    }
    return numeric_ops::sequence_profile_available(profile);
  };
  operation.start_dependency = [kind, profile](const auto&,
                                               const auto& allocator) {
    return DependencyContinuation::make<RangeState>(allocator, kind, profile);
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
