#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/exact_interpolation.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
enum class InterpolationKind { Mix, Smoothstep };
struct InterpolationState final {
  InterpolationKind kind;
  SequenceProfile profile;
  unsigned stage = 0;
  numeric_ops::InterpolationWorkspace exact;
  std::array<std::uint64_t, 3> bits{};
  std::array<BinaryParts, 3> parts{};
  std::array<std::uint64_t, 4> left{}, right{}, selected{};
  std::array<std::int64_t, 4> greater{}, less{};
  InterpolationState(InterpolationKind operation, SequenceProfile selected)
      : kind(operation), profile(selected), exact(selected) {}
  Status add_need(const DependencyPhase& phase, std::uint32_t port,
                  std::uint32_t role, std::vector<DependencyNeed>* needs) {
    auto closure = input_internal::validation_closure(
        phase.query.inputs[port], phase.query.outputs, phase.sets,
        phase.consume_work);
    if (!closure.ok())
      return closure.status();
    auto validation = closure.take_value();
    needs->push_back({port, role, phase.query.outputs, {}});
    needs->push_back({port, 4, std::move(validation), {}});
    return Status::success();
  }
  Status report(const DependencyPhase& phase) {
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto* isa = profile == SequenceProfile::Strict         ? "scalar-u64"
                      : profile == SequenceProfile::AppleSilicon ? "NEON-u64x2"
                                                                 : "AVX2-u64x4";
    const auto length = std::snprintf(
        diagnostics.implementation.data(), diagnostics.implementation.size(),
        "photospider.interpolation/1;exact-ratio;scalar-u128-product;%s%s", isa,
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostics.implementation.size())
      return Status{ErrorCode::OperationFailed,
                    "numeric implementation identity too long"};
    diagnostics.evaluated_values = 1;
    return phase.report_numeric(diagnostics);
  }
  Result<DependencyPoll> invalid(const DependencyPhase& phase,
                                 std::uint32_t port) {
    Status status{
        ErrorCode::InvalidArgument,
        std::string(kind == InterpolationKind::Mix ? "InvalidMixFactor"
                                                   : "InvalidEdges") +
            ": port=" + std::to_string(port) +
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
    const bool mix = kind == InterpolationKind::Mix;
    if (!stage) {
      ++stage;
      std::vector<DependencyNeed> needs;
      for (std::uint32_t port = mix ? 2 : 0; port < 3; ++port) {
        auto status = add_need(phase, port, mix ? 2 : 1, &needs);
        if (!status.ok())
          return Answer(status);
      }
      return multi_output::need(phase, std::move(needs));
    }
    auto status = phase.consume_work(128);
    if (!status.ok())
      return Answer(status);
    const bool narrow =
        phase.query.output.descriptor.element_type == ElementType::Float32;
    const auto width = narrow ? 4 : 8;
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    const auto read = [&](unsigned port) {
      auto value =
          phase.read(port, multi_output::coordinate(phase), &bits[port], width);
      if (value.ok())
        parts[port] = BinaryParts::decode(bits[port], narrow);
      return value;
    };
    if (mix && stage == 1) {
      status = read(2);
      if (!status.ok())
        return Answer(status);
      status = report(phase);
      if (!status.ok())
        return Answer(status);
      left = {parts[2].order_key(), parts[2].order_key(), 0, 0};
      right = {BinaryParts::decode(0, narrow).order_key(),
               BinaryParts::decode(one, narrow).order_key(), 0, 0};
      numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                                less.data(), profile);
      if (parts[2].nan || parts[2].infinite || less[0] || greater[1])
        return invalid(phase, 2);
      ++stage;
      std::vector<DependencyNeed> needs;
      for (unsigned port = 0; port < 2; ++port)
        if (port ? parts[2].magnitude != 0 : bits[2] != one) {
          status = add_need(phase, port, 1, &needs);
          if (!status.ok())
            return Answer(status);
        }
      return multi_output::need(phase, std::move(needs));
    }
    for (unsigned port = 0; port < (mix ? 2U : 3U); ++port) {
      if (mix && (port ? !parts[2].magnitude : bits[2] == one))
        continue;
      status = read(port);
      if (!status.ok())
        return Answer(status);
    }
    std::uint64_t output = 0;
    if (mix) {
      if (!parts[2].magnitude || bits[2] == one) {
        numeric_ops::select_words(selected.data(), bits[1], bits[0],
                                  bits[2] == one, profile);
        output = selected[0];
      } else if (parts[0].nan || parts[1].nan) {
        output = bits[parts[0].nan ? 0 : 1] | quiet;
      } else if (parts[0].infinite || parts[1].infinite) {
        output = parts[0].infinite && parts[1].infinite &&
                         parts[0].negative != parts[1].negative
                     ? infinity | quiet
                     : bits[parts[0].infinite ? 0 : 1];
      } else {
        auto result =
            exact.mix(parts[0], parts[1], parts[2], narrow, phase.consume_work);
        if (!result.ok())
          return Answer(result.status());
        output = result.take_value();
      }
    } else {
      status = report(phase);
      if (!status.ok())
        return Answer(status);
      for (unsigned port = 1; port < 3; ++port)
        if (parts[port].nan || parts[port].infinite)
          return invalid(phase, port);
      left = {parts[1].order_key(), parts[0].order_key(), parts[0].order_key(),
              0};
      right = {parts[2].order_key(), parts[1].order_key(), parts[2].order_key(),
               0};
      numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                                less.data(), profile);
      if (!less[0])
        return invalid(phase, 1);
      if (parts[0].nan) {
        output = bits[0] | quiet;
      } else if (!greater[1]) {
        output = 0;
      } else if (!less[2]) {
        output = one;
      } else {
        auto result = exact.smoothstep(parts[0], parts[1], parts[2], narrow,
                                       phase.consume_work);
        if (!result.ok())
          return Answer(result.status());
        output = result.take_value();
      }
    }
    return multi_output::finish(phase, &output, width);
  }
};
OperationDefinition interpolation_operation(const std::string& key,
                                            InterpolationKind kind,
                                            SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(InterpolationState);
  output.maximum_dependency_stages = kind == InterpolationKind::Mix ? 3 : 2;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  operation.validate_dependency = [profile](const auto& inputs, const auto&) {
    for (const auto& input : inputs)
      if (input.descriptor.element_type != inputs[0].descriptor.element_type)
        return Status{ErrorCode::TypeMismatch,
                      "interpolation operand dtypes must match",
                      FailureReason::None,
                      {FailureOrigin::Schema, FailureScope::Unspecified}};
    std::uint64_t count = 1;
    for (auto extent : inputs[0].descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Status{ErrorCode::TypeMismatch,
                      "interpolation input exceeds 2^40 elements",
                      FailureReason::None,
                      {FailureOrigin::Schema, FailureScope::Unspecified}};
      count *= extent;
    }
    return numeric_ops::sequence_profile_available(profile);
  };
  operation.start_dependency = [kind, profile](const auto&,
                                               const auto& allocator) {
    return DependencyContinuation::make<InterpolationState>(allocator, kind,
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
