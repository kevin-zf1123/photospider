#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_moments.hpp"
#include "01-numeric/ordered_reduction.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class ReductionKind { Sum, Minimum, Maximum, Mean, Count, Variance, Std };
struct ReductionMetadata {
  unsigned mask = 0;
  std::uint64_t count = 1, ddof = 0;
  ValueDescriptor output;
};
Result<ReductionMetadata> metadata(
    ReductionKind kind, const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ReductionMetadata>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& descriptor = input.descriptor;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    return mismatch("reduction requires rank 1..8");
  std::uint64_t elements = 1;
  for (auto size : descriptor.shape) {
    if (!size || size > (UINT64_C(1) << 40) / elements)
      return mismatch("reduction input exceeds 2^40 elements");
    elements *= size;
  }
  auto axes = numeric_ops::parse_array_list(
      std::get<std::string>(parameters.at("axes")), false);
  if (!axes.ok())
    return Answer(axes.status());
  ReductionMetadata result;
  result.output = descriptor;
  for (auto axis : axes.value()) {
    if (axis >= descriptor.shape.size() || (result.mask & (1U << axis)))
      return Answer(numeric_ops::array_parameter_error(
          "invalid or duplicate reduction axis"));
    result.mask |= 1U << axis;
    result.count *= descriptor.shape[axis];
    result.output.shape[axis] = 1;
  }
  if (kind == ReductionKind::Count)
    result.output.element_type = ElementType::Int64;
  if (kind == ReductionKind::Sum || kind == ReductionKind::Mean ||
      kind == ReductionKind::Variance || kind == ReductionKind::Std) {
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype == "uint8")
      result.output.element_type = ElementType::UInt8;
    else if (dtype == "int64")
      result.output.element_type = ElementType::Int64;
    else if (dtype == "float32")
      result.output.element_type = ElementType::Float32;
    else if (dtype == "float64")
      result.output.element_type = ElementType::Float64;
    else
      return Answer(
          numeric_ops::array_parameter_error("unsupported reduction dtype"));
    const bool source_float = descriptor.element_type == ElementType::Float32 ||
                              descriptor.element_type == ElementType::Float64;
    const bool target_float =
        result.output.element_type == ElementType::Float32 ||
        result.output.element_type == ElementType::Float64;
    if (kind == ReductionKind::Sum ? source_float != target_float
                                   : !target_float)
      return mismatch("reduction source/destination numeric domains differ");
  }
  if (kind == ReductionKind::Variance || kind == ReductionKind::Std) {
    const auto ddof = std::get<std::int64_t>(parameters.at("ddof"));
    if (ddof < 0 || static_cast<std::uint64_t>(ddof) >= result.count)
      return Answer(numeric_ops::array_parameter_error(
          "InvalidDegreesOfFreedom: require 0<=ddof<N"));
    result.ddof = static_cast<std::uint64_t>(ddof);
  }
  return Answer(std::move(result));
}
const char* operation_name(ReductionKind kind) {
  switch (kind) {
    case ReductionKind::Sum:
      return "reduce_sum";
    case ReductionKind::Minimum:
      return "reduce_minimum";
    case ReductionKind::Maximum:
      return "reduce_maximum";
    case ReductionKind::Mean:
      return "reduce_mean";
    case ReductionKind::Count:
      return "reduce_count";
    case ReductionKind::Variance:
      return "reduce_variance";
    case ReductionKind::Std:
      return "reduce_std";
  }
  return "invalid";
}
Status report(const DependencyPhase& phase, SequenceProfile profile,
              ReductionKind kind, std::uint64_t processed,
              std::uint64_t copied = 0) {
  NumericDiagnostics result;
  result.profile =
      static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
  const auto length = std::snprintf(
      result.implementation.data(), result.implementation.size(),
      "photospider.reduction/1;%s;exact-limbs;replica-store%s",
      operation_name(kind), numeric_ops::numeric_build_identity());
  if (length < 0 ||
      static_cast<std::size_t>(length) >= result.implementation.size())
    return Status{ErrorCode::OperationFailed, "reduction identity too long"};
  // Reducers count consumed accumulator inputs here; OperationTiming separately
  // records computed output elements. Count has no numeric input evaluations.
  result.evaluated_values = processed;
  result.copied_elements = copied;
  return phase.report_numeric(result);
}
struct ReductionState final {
  ReductionKind kind;
  SequenceProfile profile;
  ReductionMetadata metadata;
  bool requested = false;
  std::uint64_t cursor = 0, end = 0;
  std::variant<numeric_ops::ExactAggregate, numeric_ops::ExactMoments>
      arithmetic;
  std::array<std::uint64_t, 4> replicas{};
  ReductionState(ReductionKind operation, SequenceProfile selected,
                 ReductionMetadata description, ElementType input_type)
      : kind(operation),
        profile(selected),
        metadata(std::move(description)),
        arithmetic(std::in_place_type<numeric_ops::ExactAggregate>, selected,
                   operation == ReductionKind::Minimum
                       ? numeric_ops::AggregateKind::Minimum
                   : operation == ReductionKind::Maximum
                       ? numeric_ops::AggregateKind::Maximum
                       : numeric_ops::AggregateKind::Sum,
                   input_type) {
    if (kind == ReductionKind::Variance || kind == ReductionKind::Std)
      arithmetic.emplace<numeric_ops::ExactMoments>(selected, input_type);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    // A bounded logical window is transported at each stage; accumulated
    // exact arithmetic retains no source payload owners between windows.
    auto construction = dependency_internal::metadata_owner(32768);
    const auto& input = phase.query.inputs[0];
    const auto window = [&](std::uint64_t begin,
                            std::uint64_t stop) -> Result<Footprint> {
      auto shape = input.descriptor.shape;
      for (std::size_t j = 0; j < shape.size(); ++j)
        if (!(metadata.mask & (1U << j)))
          shape[j] = 1;
      auto local = numeric_ops::ordered_range(shape, begin, stop, phase.sets);
      if (!local.ok())
        return Result<Footprint>(local.status());
      std::vector<Region> boxes;
      for (const auto& box : local.value().boxes()) {
        auto dimensions = box.dimensions();
        for (std::size_t j = 0; j < shape.size(); ++j)
          if (!(metadata.mask & (1U << j)))
            dimensions[j].offset =
                phase.query.outputs.boxes()[0].dimensions()[j].offset;
        boxes.emplace_back(std::move(dimensions));
      }
      return Footprint::from_regions(input.descriptor.shape, std::move(boxes),
                                     phase.sets);
    };
    Status status;
    if (requested) {
      auto group = window(cursor, end);
      if (!group.ok())
        return Answer(group.status());
      const auto width = Value::element_size(input.descriptor.element_type);
      status = group.value().visit(
          [&](const auto& coordinate) {
            auto work = phase.consume_work(
                (phase.inputs[0].fragments().size() + 1) * coordinate.size() +
                1);
            if (!work.ok())
              return work;
            std::uint64_t bits = 0;
            auto read = phase.read(0, coordinate, &bits, width);
            if (!read.ok())
              return read;
            // Admission precedes the next numeric evaluation; later failures
            // retain the already admitted input-attempt count.
            auto counted = report(phase, profile, kind, 1);
            if (!counted.ok())
              return counted;
            return std::visit(
                [&](auto& state) {
                  return state.add(bits, phase.consume_work);
                },
                arithmetic);
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      cursor = end;
    }
    if (cursor < metadata.count) {
      end = cursor + std::min(UINT64_C(64), metadata.count - cursor);
      auto group = window(cursor, end);
      if (!group.ok())
        return Answer(group.status());
      auto validation = group.value();
      for (const auto& facet : input.facets) {
        if (facet.key != "photospider.image" &&
            facet.key != "photospider.semantic")
          continue;
        auto semantic = decode_semantic(facet);
        if (!semantic.ok())
          return Answer(semantic.status());
        if (semantic.value().kind == SemanticKind::Image) {
          std::vector<Region> boxes;
          for (const auto& box : group.value().boxes()) {
            auto dimensions = box.dimensions();
            dimensions[2] = {0, input.descriptor.shape[2]};
            boxes.emplace_back(std::move(dimensions));
          }
          auto closure = Footprint::from_regions(input.descriptor.shape,
                                                 std::move(boxes), phase.sets);
          if (!closure.ok())
            return Answer(closure.status());
          validation = closure.take_value();
        }
      }
      requested = true;
      return multi_output::need(phase, {{0, 1, group.take_value(), {}},
                                        {0, 4, std::move(validation), {}}});
    }
    Result<std::uint64_t> calculated(
        Status{ErrorCode::Internal, "uninitialized reduction"});
    if (auto* moments = std::get_if<numeric_ops::ExactMoments>(&arithmetic))
      calculated = moments->finish(metadata.output.element_type, metadata.count,
                                   metadata.ddof, kind == ReductionKind::Std,
                                   phase.consume_work);
    else
      calculated =
          std::get<numeric_ops::ExactAggregate>(arithmetic)
              .finish_as(metadata.output.element_type,
                         kind == ReductionKind::Mean ? metadata.count : 1,
                         phase.consume_work);
    if (!calculated.ok()) {
      auto failure = calculated.status();
      if (failure.reason == FailureReason::ArithmeticOverflow) {
        failure.detail.origin = FailureOrigin::Domain;
        failure.detail.scope = FailureScope::Atom;
        auto atom = dependency_atom_key(phase.query);
        if (!atom.ok())
          return Answer(atom.status());
        failure.detail.atom = atom.take_value();
      }
      return Answer(failure);
    }
    numeric_ops::ArrayPublication publication(1, metadata.output.shape.size());
    auto allocation = MutableValue::allocate(
        metadata.output, phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocation.ok())
      return Answer(allocation.status());
    auto output = allocation.take_value();
    const auto output_width = Value::element_size(metadata.output.element_type);
    if (output.size() != output_width)
      return Answer(Status{ErrorCode::Internal,
                           "reducer requires one output observation"});
    status = report(phase, profile, kind, 0, 1);
    if (!status.ok())
      return Answer(status);
    numeric_ops::select_words(replicas.data(), calculated.value(),
                              calculated.value(), 1, profile);
    std::memcpy(output.data(), replicas.data(), output_width);
    auto value = std::move(output).publish();
    if (!value.ok())
      return Answer(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Answer(retained.status());
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result = publication.finish(metadata.output, phase.query.outputs,
                                     &retained.value(), 1, phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
struct CountState final {
  SequenceProfile profile;
  std::uint64_t count;
  bool requested = false;
  std::array<std::uint64_t, 4> replicas{};
  CountState(SequenceProfile selected, std::uint64_t size)
      : profile(selected), count(size) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!requested) {
      requested = true;
      return Answer(DependencyNeedBatch{{}, {}, true});
    }
    auto status =
        phase.consume_work(phase.query.inputs[0].descriptor.shape.size() + 16);
    if (!status.ok())
      return Answer(status);
    status = report(phase, profile, ReductionKind::Count, 0);
    if (!status.ok())
      return Answer(status);
    auto allocation = phase.allocator.allocate(8);
    if (!allocation.ok())
      return Answer(allocation.status());
    auto buffer = allocation.take_value();
    numeric_ops::select_words(replicas.data(), count, count, 1, profile);
    std::memcpy(buffer.data(), replicas.data(), 8);
    auto owner = std::move(buffer).freeze();
    const auto& descriptor = phase.query.output.descriptor;
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), descriptor.shape.size());
    ResourceVector<Value> values;
    values.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      status = phase.consume_work(descriptor.shape.size() + 1);
      if (!status.ok())
        return Answer(status);
      std::vector<std::uint64_t> origin;
      origin.reserve(descriptor.shape.size());
      for (const auto& dimension : box.dimensions())
        origin.push_back(dimension.offset);
      auto value = Value::from_storage(
          descriptor, box,
          {0, std::vector<std::int64_t>(origin.size(), 0), origin}, owner);
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
OperationDefinition reduction_operation(const std::string& key,
                                        ReductionKind kind,
                                        SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axes", OperationParameterType::String}};
  if (kind == ReductionKind::Sum || kind == ReductionKind::Mean ||
      kind == ReductionKind::Variance || kind == ReductionKind::Std)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  if (kind == ReductionKind::Variance || kind == ReductionKind::Std)
    traits.parameter_schema.insert(traits.parameter_schema.begin() + 1,
                                   {"ddof", OperationParameterType::Int64});
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = kind == ReductionKind::Count
                                  ? sizeof(CountState)
                                  : sizeof(ReductionState);
  output.maximum_dependency_stages = kind == ReductionKind::Count ? 2 : 1048576;
  if (kind != ReductionKind::Count)
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(kind, inputs[0], parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = resolved.value().output;
    if (kind == ReductionKind::Count) {
      result.maximum_output_payload_bytes = 8;
      result.preserve_output_views = true;
      auto all = Footprint::all(result.metadata.descriptor.shape);
      if (!all.ok())
        return Answer(all.status());
      result.static_dependency_pieces =
          std::vector<DependencyMapPiece>{{all.take_value(), {}}};
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [kind, profile](const auto& query,
                                               const auto& allocator) {
    auto resolved = metadata(kind, query.inputs[0], query.parameters);
    if (!resolved.ok())
      return Result<DependencyContinuation>(resolved.status());
    if (kind == ReductionKind::Count)
      return DependencyContinuation::make<CountState>(allocator, profile,
                                                      resolved.value().count);
    return DependencyContinuation::make<ReductionState>(
        allocator, kind, profile, resolved.take_value(),
        query.inputs[0].descriptor.element_type);
  };
  return operation;
}
}  // namespace
Status register_numeric_reductions(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (auto kind :
         {ReductionKind::Sum, ReductionKind::Minimum, ReductionKind::Maximum,
          ReductionKind::Mean, ReductionKind::Count, ReductionKind::Variance,
          ReductionKind::Std}) {
      auto status = registry->register_operation(reduction_operation(
          std::string("numeric.") + operation_name(kind) + profile.first, kind,
          profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
