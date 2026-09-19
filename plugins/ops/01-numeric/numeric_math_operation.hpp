#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/certified_math.hpp"
#include "01-numeric/exact_elementary.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::numeric_ops {
template <class Arithmetic>
struct PointMathState final {
  unsigned kind;
  const char* name;
  SequenceProfile profile;
  Arithmetic arithmetic;
  bool requested = false;
  std::array<std::uint64_t, 4> replicas{};
  PointMathState(unsigned operation, const char* function,
                 SequenceProfile selected)
      : kind(operation),
        name(function),
        profile(selected),
        arithmetic(selected) {}
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied, std::uint64_t fallback = 0) const {
    NumericDiagnostics report;
    report.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        report.implementation.data(), report.implementation.size(),
        "photospider.math/1;%s;%s;replica-store%s", name,
        std::is_same_v<Arithmetic, CertifiedMath> ? "certified-Q128..4096"
                                                  : "exact-bits-ratio-root",
        numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= report.implementation.size())
      return Status{ErrorCode::Internal, "math identity too long"};
    report.evaluated_values = evaluated;
    report.copied_elements = copied;
    report.strict_fallbacks = fallback;
    report.fallback_reasons[static_cast<unsigned>(
        NumericFallbackReason::FunctionUnsupported)] = fallback;
    return phase.report_numeric(report);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!requested) {
      requested = true;
      return Answer(DependencyNeedBatch{{}, {}, true});
    }
    auto construction = dependency_internal::metadata_owner(32768);
    const auto& descriptor = phase.query.output.descriptor;
    const auto source_width =
        Value::element_size(phase.query.inputs[0].descriptor.element_type);
    const auto output_width = Value::element_size(descriptor.element_type);
    ArrayPublication publication(phase.query.outputs.boxes().size(),
                                 descriptor.shape.size());
    ResourceVector<Value> values;
    values.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      auto selected =
          Footprint::from_regions(descriptor.shape, {box}, phase.sets);
      if (!selected.ok())
        return Answer(selected.status());
      std::uint64_t offset = 0;
      auto status = selected.value().visit(
          [&](const auto& coordinate) {
            std::array<std::uint64_t, 2> bits{};
            for (unsigned port = 0; port < phase.query.inputs.size(); ++port) {
              auto work = phase.consume_work(
                  (phase.inputs[port].fragments().size() + 1) *
                      coordinate.size() +
                  1);
              if (!work.ok())
                return work;
              auto read =
                  phase.read(port, coordinate, &bits[port], source_width);
              if (!read.ok())
                return read;
            }
            auto admitted = report(phase, 1, 0);
            if (!admitted.ok())
              return admitted;
            Result<std::uint64_t> calculated(
                Status{ErrorCode::Internal, "uninitialized math result"});
            if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
              calculated = arithmetic.evaluate(
                  static_cast<CertifiedKind>(kind), descriptor.element_type,
                  bits[0], bits[1], phase.consume_work, [&] {
                    // No certified approximate transcendental backend is
                    // selected. Ordinary accelerated requests enter the strict
                    // interval engine; exact special/algebraic paths return
                    // before this callback.
                    return profile == SequenceProfile::Strict
                               ? Status::success()
                               : report(phase, 0, 0, 1);
                  });
            } else {
              calculated = arithmetic.evaluate(
                  static_cast<ElementaryKind>(kind), descriptor.element_type,
                  bits[0], bits[1], phase.consume_work);
            }
            if (!calculated.ok()) {
              auto failure = calculated.status();
              if (failure.detail.scope == FailureScope::Atom) {
                AtomKey atom;
                atom.output_index = phase.query.output_index;
                atom.rank = static_cast<std::uint32_t>(coordinate.size());
                std::copy(coordinate.begin(), coordinate.end(),
                          atom.coordinate.begin());
                failure.detail.atom = atom;
              }
              return failure;
            }
            admitted = report(phase, 0, 1);
            if (!admitted.ok())
              return admitted;
            select_words(replicas.data(), calculated.value(),
                         calculated.value(), 1, profile);
            std::memcpy(writer.data() + offset * output_width, replicas.data(),
                        output_width);
            ++offset;
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
      values.push_back(retained.take_value());
    }
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result = publication.finish(descriptor, phase.query.outputs,
                                     values.data(), values.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
template <class Arithmetic, class Kind>
OperationDefinition point_math_operation(const std::string& key,
                                         const char* name, Kind kind,
                                         SequenceProfile profile,
                                         unsigned ports, unsigned dtype_mask,
                                         bool rational = false) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = ports;
  traits.input_schema.resize(ports);
  for (auto& input : traits.input_schema)
    input.element_type_mask = dtype_mask;
  traits.requires_metadata_specialization = true;
  if (rational)
    traits.parameter_schema = {{"dtype", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(PointMathState<Arithmetic>);
  output.maximum_dependency_stages = 2;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  operation.specialize_metadata = [profile, rational](const auto& inputs,
                                                      const auto& parameters)
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
      return mismatch("numeric math requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("numeric math exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.descriptor.shape != first.shape ||
          input.descriptor.element_type != first.element_type)
        return mismatch("numeric math inputs must match shape and dtype");
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = first;
    if (rational) {
      const auto& dtype = std::get<std::string>(parameters.at("dtype"));
      if (dtype != "float32" && dtype != "float64")
        return Answer(
            array_parameter_error("rational pi dtype must be float32/float64"));
      result.metadata.descriptor.element_type =
          dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
    }
    std::vector<DependencyMappedNeed> maps;
    for (std::uint32_t port = 0; port < inputs.size(); ++port) {
      DependencyMappedNeed data;
      data.port = port;
      data.roles = 1;
      for (std::size_t axis = 0; axis < first.shape.size(); ++axis)
        data.axes.push_back({static_cast<std::int32_t>(axis), {}});
      auto validation = input_internal::validation_map(data, inputs[port]);
      maps.push_back(std::move(data));
      maps.push_back(std::move(validation));
    }
    auto all = Footprint::all(first.shape);
    if (!all.ok())
      return Answer(all.status());
    result.static_dependency_pieces =
        std::vector<DependencyMapPiece>{{all.take_value(), std::move(maps)}};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [kind, name, profile](const auto&,
                                                     const auto& allocator) {
    return DependencyContinuation::make<PointMathState<Arithmetic>>(
        allocator, static_cast<unsigned>(kind), name, profile);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
