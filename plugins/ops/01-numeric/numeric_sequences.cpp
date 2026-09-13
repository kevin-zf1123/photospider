#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/exact_sequence.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::ExactSequence;
using numeric_ops::SequenceProfile;

enum class Sequence { Linspace, Arange };

struct SequenceState final {
  Sequence kind;
  SequenceProfile profile;
  bool ready = false;
  ExactSequence first, second;
  std::array<std::uint64_t, 68> products{};

  SequenceState(Sequence operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}

  Status report(const DependencyPhase& phase, std::size_t size) const {
    NumericDiagnostics report;
    report.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const char* identity = numeric_ops::sequence_implementation();
    const auto length = std::strlen(identity);
    std::memcpy(report.implementation.data(), identity, length + 1);
    report.evaluated_values =
        size / Value::element_size(phase.query.output.descriptor.element_type);
    if (!phase.report_numeric)
      return Status{ErrorCode::BackendUnavailable,
                    "numeric diagnostics service unavailable"};
    return phase.report_numeric(report);
  }

  Result<DependencyPoll> fail(const DependencyPhase& phase,
                              FailureReason reason, const char* message) const {
    auto key = dependency_atom_key(phase.query);
    Status status{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    if (key.ok())
      status.detail.atom = key.take_value();
    return Result<DependencyPoll>(std::move(status));
  }

  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto count = static_cast<std::uint32_t>(
        std::get<std::int64_t>(phase.query.parameters.at("count")));
    const auto coordinate = multi_output::coordinate(phase)[0];
    const bool axis = phase.query.output_index == 1;
    const auto index = axis ? count - 1 : coordinate;
    const bool start_needed =
        axis || kind == Sequence::Arange || index == 0 || index + 1 < count;
    const bool other_needed = count > 1 && (axis || index > 0);
    if (!ready) {
      std::vector<DependencyNeed> needs;
      for (std::uint32_t port = 0; port < 2; ++port) {
        if (!(port == 0 ? start_needed : other_needed))
          continue;
        auto samples =
            Footprint::from_regions({1}, {Region::whole({1})}, phase.sets);
        if (!samples.ok())
          return Result<DependencyPoll>(samples.status());
        needs.push_back({port, 5, samples.take_value(), {}});
      }
      ready = true;
      return multi_output::need(phase, std::move(needs));
    }
    auto charged = phase.consume_work(axis ? 24576 : 8192);
    if (!charged.ok())
      return Result<DependencyPoll>(charged);
    if (phase.query.cancellation.cancelled())
      return Result<DependencyPoll>(Status{ErrorCode::Cancelled, {}});
    const auto dtype = phase.query.output.descriptor.element_type;
    if (dtype == ElementType::Int64)
      return integer(phase, index, axis, other_needed);
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<DependencyPoll>(Status{ErrorCode::BackendUnavailable,
                                           "numeric environment unavailable"});
    double start = 0, other = 0;
    for (std::uint32_t port = 0; port < 2; ++port) {
      if (!(port == 0 ? start_needed : other_needed))
        continue;
      double value = 0;
      Status read;
      if (phase.query.inputs[port].descriptor.element_type ==
          ElementType::Float32) {
        float narrow = 0;
        read = phase.read(port, {0}, &narrow, 4);
        value = narrow;
      } else {
        read = phase.read(port, {0}, &value, 8);
      }
      if (!read.ok())
        return Result<DependencyPoll>(read);
      if (!std::isfinite(value))
        return fail(phase, FailureReason::InvalidDomain,
                    port ? "nonfinite end/step" : "nonfinite start");
      (port == 0 ? start : other) = value;
    }
    std::uint64_t bits = 0;
    const bool binary32 = dtype == ElementType::Float32;
    auto reported = report(phase, axis ? 24 : binary32 ? 4 : 8);
    if (!reported.ok())
      return Result<DependencyPoll>(reported);
    if (axis) {
      std::array<std::uint64_t, 3> tuple{};
      tuple[0] = rounded(start, 0, 1, 0, 1, false);
      if (count == 1) {
        tuple[1] = tuple[0];
      } else if (kind == Sequence::Linspace) {
        const auto step = rounded(other, start, 1, 1, count - 1, false, true);
        if (overflow(step, false))
          return fail(phase, FailureReason::ArithmeticOverflow,
                      "axis step overflow");
        tuple[1] = rounded(other, 0, 1, 0, 1, false);
        tuple[2] = step;
      } else {
        const auto last = rounded(start, other, 1, count - 1, 1, false);
        if (overflow(last, false))
          return fail(phase, FailureReason::ArithmeticOverflow,
                      "axis last overflow");
        tuple[1] = last;
        tuple[2] = rounded(other, 0, 1, 0, 1, false);
      }
      return multi_output::finish(phase, tuple.data(), sizeof(tuple));
    } else if (kind == Sequence::Linspace && count > 1 && index > 0 &&
               index + 1 < count) {
      bits =
          rounded(start, other, count - 1 - index, index, count - 1, binary32);
    } else if (kind == Sequence::Arange && index > 0) {
      bits = rounded(start, other, 1, index, 1, binary32);
    } else {
      bits = rounded(start_needed ? start : other, 0, 1, 0, 1, binary32);
    }
    if (overflow(bits, binary32))
      return fail(phase, FailureReason::ArithmeticOverflow,
                  "sequence value overflow");
    if (binary32) {
      const auto narrow = static_cast<std::uint32_t>(bits);
      return multi_output::finish(phase, &narrow, 4);
    }
    return multi_output::finish(phase, &bits, 8);
  }

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

  Result<DependencyPoll> integer(const DependencyPhase& phase,
                                 std::uint64_t index, bool axis,
                                 bool other_needed) {
    std::int64_t start = 0, step = 0;
    auto read = phase.read(0, {0}, &start, 8);
    if (!read.ok())
      return Result<DependencyPoll>(read);
    if (other_needed) {
      read = phase.read(1, {0}, &step, 8);
      if (!read.ok())
        return Result<DependencyPoll>(read);
    }
    auto reported = report(phase, axis ? 24 : 8);
    if (!reported.ok())
      return Result<DependencyPoll>(reported);
    first.set_integer(start);
    second.set_integer(step);
    numeric_ops::sequence_multiply(&second, static_cast<std::uint32_t>(index),
                                   profile, products.data());
    first.add(second);
    const auto magnitude = static_cast<std::uint64_t>(first.words[0]) |
                           (static_cast<std::uint64_t>(first.words[1]) << 32);
    bool fits = magnitude <= (first.negative ? UINT64_C(1) << 63 : INT64_MAX);
    for (unsigned i = 2; i < first.words.size(); ++i)
      fits = fits && first.words[i] == 0;
    if (!fits)
      return fail(phase, FailureReason::ArithmeticOverflow,
                  axis ? "axis last overflow" : "integer sequence overflow");
    const auto bits = first.negative ? UINT64_C(0) - magnitude : magnitude;
    std::int64_t output = 0;
    std::memcpy(&output, &bits, 8);
    if (axis) {
      const std::array<std::int64_t, 3> tuple{start, output, step};
      return multi_output::finish(phase, tuple.data(), sizeof(tuple));
    }
    return multi_output::finish(phase, &output, 8);
  }
};

OperationDefinition sequence(const std::string& key, Sequence kind,
                             SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.rank = 1;
    port.element_type_mask = kind == Sequence::Linspace ? 12 : 14;
  }
  traits.parameter_schema = {
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  auto& values = traits.outputs[0];
  values.key = "values";
  values.shape_rule = OperationShapeRule::Axes;
  values.output_axes = {{OperationExtentSource::Parameter, 1, "count"}};
  values.output_dtype_rule = OperationDtypeRule::Parameter;
  values.output_dtype_parameter = "dtype";
  values.region_rule = OperationRegionRule::Dependency;
  values.dependency_version = 1;
  values.continuation_bytes = sizeof(SequenceState);
  values.maximum_dependency_stages = 2;
  traits.outputs.push_back(values);
  auto& axis = traits.outputs[1];
  axis.key = "axis";
  axis.atomic_trailing_axes = 1;
  axis.shape_rule = OperationShapeRule::Fixed;
  axis.fixed_output_shape = {3};
  axis.output_axes.clear();
  axis.output_dtype_rule = kind == Sequence::Arange
                               ? OperationDtypeRule::WidenNumericInput
                               : OperationDtypeRule::Declared;
  axis.output_dtype_parameter.clear();
  operation.validate_dependency = [kind, profile](const auto& inputs,
                                                  const auto& parameters) {
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const bool integer = kind == Sequence::Arange && dtype == "int64";
    if (!integer && dtype != "float32" && dtype != "float64")
      return Status{
          dtype == "uint8" || dtype == "int64" ? ErrorCode::TypeMismatch
                                               : ErrorCode::InvalidArgument,
          "sequence dtype must be float32/float64, or int64 for arange",
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
    for (const auto& input : inputs)
      if (input.descriptor.shape != std::vector<std::uint64_t>{1} ||
          (integer ? input.descriptor.element_type != ElementType::Int64
                   : (input.descriptor.element_type != ElementType::Float32 &&
                      input.descriptor.element_type != ElementType::Float64)))
        return Status{ErrorCode::TypeMismatch,
                      "sequence requires matching numeric-kind scalars",
                      FailureReason::None,
                      {FailureOrigin::Schema, FailureScope::Unspecified}};
    return numeric_ops::sequence_profile_available(profile);
  };
  operation.start_dependency = [kind, profile](const auto&,
                                               const auto& allocator) {
    return DependencyContinuation::make<SequenceState>(allocator, kind,
                                                       profile);
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
