#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_publication.hpp"
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
        "photospider.range/2;%s;%s%s",
        kind == RangeKind::Clamp ? "bounds-bitselect" : "exact-rational-round",
        isa, numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostics.implementation.size())
      return Status{ErrorCode::OperationFailed,
                    "numeric implementation identity too long"};
    diagnostics.evaluated_values = 1;
    return phase.report_numeric(diagnostics);
  }
  Result<std::uint64_t> invalid(
      const DependencyPhase& phase, std::size_t port,
      const std::vector<std::uint64_t>& coordinate) const {
    Status status{ErrorCode::InvalidArgument,
                  "InvalidBounds: port=" + std::to_string(port) +
                      " bits=" + std::to_string(bits[port]),
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    (void)phase;
    AtomKey atom;
    atom.rank = static_cast<std::uint8_t>(coordinate.size());
    std::copy(coordinate.begin(), coordinate.end(), atom.coordinate.begin());
    status.detail.atom = atom;
    return Result<std::uint64_t>(status);
  }
  Result<std::uint64_t> evaluate(const DependencyPhase& phase,
                                 const std::vector<std::uint64_t>& coordinate) {
    using Answer = Result<std::uint64_t>;
    const auto count = kind == RangeKind::Clamp ? 3U : 5U;
    auto status = phase.consume_work(128);
    if (!status.ok())
      return Answer(status);
    const auto type = phase.query.output.descriptor.element_type;
    const bool narrow = type == ElementType::Float32;
    const bool floating = narrow || type == ElementType::Float64;
    const auto width = Value::element_size(type);
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
        return invalid(phase, 1, coordinate);
      if (floating && parts[2].nan)
        return invalid(phase, 2, coordinate);
      if (greater[2])
        return invalid(phase, 1, coordinate);
      output = floating && parts[0].nan
                   ? bits[0] | (UINT64_C(1) << (narrow ? 22 : 51))
               : less[0]    ? bits[1]
               : greater[1] ? bits[2]
                            : bits[0];
    } else {
      for (unsigned port = 1; port < count; ++port)
        if (parts[port].nan || parts[port].infinite)
          return invalid(phase, port, coordinate);
      if (!less[2])
        return invalid(phase, 1, coordinate);
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
        auto rounded = ratio.round(narrow, phase.consume_work, -1074, true);
        if (!rounded.ok())
          return Answer(rounded.status());
        output = rounded.take_value();
      }
    }
    return Answer(output);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!ready) {
      ready = true;
      return Answer(DependencyNeedBatch{{}, {}, true});
    }
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(),
        phase.query.output.descriptor.shape.size());
    ResourceVector<Value> outputs;
    outputs.reserve(phase.query.outputs.boxes().size());
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      auto selected = Footprint::from_regions(
          phase.query.output.descriptor.shape, {box}, phase.sets);
      if (!selected.ok())
        return Answer(selected.status());
      std::uint64_t offset = 0;
      auto status = selected.value().visit(
          [&](const auto& coordinate) {
            auto value = evaluate(phase, coordinate);
            if (!value.ok())
              return value.status();
            const auto bits = value.value();
            std::memcpy(writer.data() + offset, &bits, width);
            offset += width;
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      auto value = std::move(writer).publish();
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication.retain(value.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      outputs.push_back(retained.take_value());
    }
    auto result =
        publication.finish(phase.query.output.descriptor, phase.query.outputs,
                           outputs.data(), outputs.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
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
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    for (const auto& input : inputs)
      if (input.descriptor.shape != inputs[0].descriptor.shape ||
          input.descriptor.element_type != inputs[0].descriptor.element_type)
        return Result<std::vector<OperationOutputSpecialization>>(
            Status{ErrorCode::TypeMismatch,
                   "range operands require identical shape and dtype"});
    OperationOutputSpecialization result;
    result.metadata.descriptor = inputs[0].descriptor;
    std::vector<DependencyMappedNeed> maps;
    for (std::uint32_t port = 0; port < inputs.size(); ++port) {
      DependencyMappedNeed data;
      data.port = port;
      data.roles = 1;
      for (std::size_t axis = 0; axis < inputs[0].descriptor.shape.size();
           ++axis)
        data.axes.push_back({static_cast<std::int32_t>(axis), {}});
      auto validation = input_internal::validation_map(data, inputs[port]);
      maps.push_back(std::move(data));
      maps.push_back(std::move(validation));
    }
    auto all = Footprint::all(inputs[0].descriptor.shape);
    if (!all.ok())
      return Result<std::vector<OperationOutputSpecialization>>(all.status());
    result.static_dependency_pieces =
        std::vector<DependencyMapPiece>{{all.take_value(), std::move(maps)}};
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(result)});
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
