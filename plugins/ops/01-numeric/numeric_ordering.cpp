#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_quantile.hpp"
#include "01-numeric/pointwise_execution.hpp"
#include "01-numeric/stable_order.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Status shape_error(const char* message) {
  return Status{ErrorCode::TypeMismatch,
                message,
                FailureReason::None,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}

Result<std::uint32_t> ordering_axis(
    const ValueDescriptor& descriptor,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<std::uint32_t>;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    return Answer(shape_error("ordering requires rank 1..8"));
  std::uint64_t count = 1;
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      return Answer(shape_error("ordering exceeds 2^40 elements"));
    count *= extent;
  }
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= descriptor.shape.size())
    return Answer(
        numeric_ops::array_parameter_error("ordering axis outside rank"));
  return Answer(static_cast<std::uint32_t>(axis));
}
Status report(const DependencyPhase& phase, SequenceProfile profile,
              bool quantile, std::uint64_t evaluated,
              std::uint64_t copied = 0) {
  NumericDiagnostics result;
  result.profile =
      static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
  const auto length = std::snprintf(
      result.implementation.data(), result.implementation.size(),
      "photospider.ordering/"
      "2;%s;stable-tuple-heapsort;bounded-final-rational%s",
      quantile ? "quantile" : "sort", numeric_ops::numeric_build_identity());
  if (length < 0 ||
      static_cast<std::size_t>(length) >= result.implementation.size())
    return Status{ErrorCode::OperationFailed, "ordering identity too long"};
  result.evaluated_values = evaluated;
  result.copied_elements = copied;
  return phase.report_numeric(result);
}
struct OrderingState final {
  bool quantile;
  std::uint32_t axis, stage = 0;
  SequenceProfile profile;
  numeric_ops::QuantilePosition position;
  numeric_ops::StableOrderWorkspace ordering;
  numeric_ops::ExactQuantile arithmetic;
  std::optional<Value> cached_permutation;
  std::uint64_t cached_line = 0;
  std::array<std::uint64_t, 4> replicas{};
  OrderingState(bool probability, std::uint32_t selected_axis,
                SequenceProfile selected)
      : quantile(probability),
        axis(selected_axis),
        profile(selected),
        arithmetic(selected) {}
  Result<DependencyPoll> sort_region(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!stage) {
      ++stage;
      return Answer(DependencyNeedBatch{{}, {}, true});
    }
    const auto& input = phase.query.inputs[0].descriptor;
    const auto count = input.shape[axis];
    return numeric_ops::publish_numeric_lines(
        phase, axis,
        [&](const std::vector<std::uint64_t>& at) -> Result<std::uint64_t> {
          using Bits = Result<std::uint64_t>;
          auto status = report(phase, profile, false, 1);
          if (!status.ok())
            return Bits(status);
          std::uint64_t line = 0;
          for (std::size_t j = 0; j < at.size(); ++j)
            if (j != axis)
              line = line * input.shape[j] + at[j];
          auto coordinate = at;
          const auto read = [&](std::uint64_t index, std::uint64_t* bits) {
            auto charged = phase.consume_work(
                (phase.inputs[0].fragments().size() + 1) * coordinate.size() +
                1);
            if (!charged.ok())
              return charged;
            coordinate[axis] = index;
            *bits = 0;
            return phase.read(0, coordinate, bits,
                              Value::element_size(input.element_type));
          };
          if (!cached_permutation || cached_line != line) {
            cached_permutation.reset();
            auto value =
                ordering.build(count, input.element_type, profile,
                               phase.allocator, phase.consume_work, read);
            if (!value.ok())
              return Bits(value.status());
            cached_permutation = value.take_value();
            cached_line = line;
          }
          std::uint64_t index = 0, bits = 0;
          std::memcpy(&index, cached_permutation->bytes().data() + at[axis] * 8,
                      8);
          if (phase.query.output_index == 1) {
            bits = index;
          } else {
            status = read(index, &bits);
            if (!status.ok())
              return Bits(status);
          }
          status = report(phase, profile, false, 0, 1);
          return status.ok() ? Bits(bits) : Bits(status);
        });
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!quantile)
      return sort_region(phase);
    auto construction = dependency_internal::metadata_owner(8192);
    const auto& input = phase.query.inputs[0];
    const auto count = input.descriptor.shape[axis];
    const auto type = input.descriptor.element_type;
    if (stage == 0 && quantile && count > 1) {
      stage = 1;
      auto scalar = Footprint::all({1}, phase.sets);
      if (!scalar.ok())
        return Answer(scalar.status());
      return multi_output::need(phase, {{1, 6, scalar.take_value(), {}}});
    }
    if (stage == 1) {
      std::uint64_t bits = 0;
      auto status = phase.read(
          1, {0}, &bits,
          Value::element_size(phase.query.inputs[1].descriptor.element_type));
      if (!status.ok())
        return Answer(status);
      auto resolved = numeric_ops::quantile_position(
          bits, phase.query.inputs[1].descriptor.element_type, count);
      if (!resolved.ok()) {
        auto failure = resolved.status();
        std::array<char, 128> message{};
        std::snprintf(message.data(), message.size(),
                      "InvalidQuantileProbability: port=1 bits=0x%016" PRIx64
                      "; "
                      "require finite q in [0,1]",
                      bits);
        failure.message = message.data();
        failure.detail.scope = FailureScope::Atom;
        auto atom = dependency_atom_key(phase.query);
        if (!atom.ok())
          return Answer(atom.status());
        failure.detail.atom = atom.take_value();
        return Answer(failure);
      }
      position = resolved.value();
    }
    if (stage < 2) {
      stage = 2;
      auto dimensions = phase.query.outputs.boxes()[0].dimensions();
      dimensions[axis] = {0, count};
      auto data = Footprint::from_regions(input.descriptor.shape,
                                          {Region(dimensions)}, phase.sets);
      if (!data.ok())
        return Answer(data.status());
      auto closure = input_internal::validation_closure(
          input, data.value(), phase.sets, phase.consume_work);
      if (!closure.ok())
        return Answer(closure.status());
      auto validation = closure.take_value();
      return multi_output::need(phase, {{0, 1, data.take_value(), {}},
                                        {0, 4, std::move(validation), {}}});
    }
    const auto at = multi_output::coordinate(phase);
    std::uint64_t line = 0;
    for (std::size_t j = 0; j < at.size(); ++j)
      if (j != axis)
        line = line * input.descriptor.shape[j] + at[j];
    // The transition's explicit line id determines source coordinates. It
    // never depends on the requested sorted position or selected public output.
    std::vector<std::uint64_t> coordinate(at.size());
    auto remainder = line;
    for (std::size_t j = at.size(); j; --j)
      if (j - 1 != axis) {
        coordinate[j - 1] = remainder % input.descriptor.shape[j - 1];
        remainder /= input.descriptor.shape[j - 1];
      }
    const auto read = [&](std::uint64_t index, std::uint64_t* bits) {
      auto status = phase.consume_work(
          (phase.inputs[0].fragments().size() + 1) * coordinate.size() + 1);
      if (!status.ok())
        return status;
      coordinate[axis] = index;
      *bits = 0;
      return phase.read(0, coordinate, bits, Value::element_size(type));
    };
    numeric_ops::ArrayPublication state_metadata(1, 1);
    auto zero = phase.allocator.allocate(8);
    if (!zero.ok())
      return Answer(zero.status());
    auto storage = zero.take_value();
    std::memset(storage.data(), 0, 8);
    auto incoming = Value::from_storage({ElementType::Int64, {count}},
                                        Region::whole({count}), {0, {0}},
                                        std::move(storage).freeze());
    if (!incoming.ok())
      return Answer(incoming.status());
    auto retained = state_metadata.retain(incoming.take_value());
    if (!retained.ok())
      return Answer(retained.status());
    auto status = report(phase, profile, quantile, 1);
    if (!status.ok())
      return Answer(status);
    auto permutation = phase.block(1, line, line + 1, 1, retained.value(), [&] {
      return ordering.build(count, type, profile, phase.allocator,
                            phase.consume_work, read);
    });
    if (!permutation.ok())
      return Answer(permutation.status());
    const auto index_at = [&](std::uint64_t rank) {
      std::uint64_t index = 0;
      std::memcpy(&index, permutation.value().bytes().data() + rank * 8, 8);
      return index;
    };
    std::uint64_t bits = 0;
    if (!quantile) {
      const auto original = index_at(at[axis]);
      if (phase.query.output_index == 1)
        bits = original;
      else
        status = read(original, &bits);
    } else {
      // Stable ordering places NaNs last in original index order. Locate the
      // first NaN before choosing endpoints, so endpoint q never omits one.
      const bool floating =
          type == ElementType::Float32 || type == ElementType::Float64;
      std::uint64_t nan_begin = count;
      if (floating) {
        std::uint64_t low = 0, high = count;
        while (low < high) {
          const auto middle = low + (high - low) / 2;
          std::uint64_t value = 0;
          status = read(index_at(middle), &value);
          if (!status.ok())
            return Answer(status);
          if (numeric_ops::BinaryParts::decode(value,
                                               type == ElementType::Float32)
                  .nan)
            high = middle;
          else
            low = middle + 1;
        }
        nan_begin = low;
      }
      if (nan_begin < count) {
        status = read(index_at(nan_begin), &bits);
        if (status.ok())
          bits = numeric_ops::converted_nan(
              bits, type, phase.query.output.descriptor.element_type);
      } else {
        std::uint64_t a = 0, b = 0;
        status = read(index_at(position.index), &a);
        if (status.ok() && position.fractional())
          status = read(index_at(position.index + 1), &b);
        if (!status.ok())
          return Answer(status);
        auto calculated = arithmetic.finish(
            a, b, type, phase.query.output.descriptor.element_type, position,
            phase.consume_work);
        if (!calculated.ok())
          return Answer(calculated.status());
        bits = calculated.value();
      }
    }
    if (!status.ok())
      return Answer(status);
    numeric_ops::ArrayPublication publication(1, at.size());
    auto allocated =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    status = report(phase, profile, quantile, 0, 1);
    if (!status.ok())
      return Answer(status);
    numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
    std::memcpy(
        output.data(), replicas.data(),
        Value::element_size(phase.query.output.descriptor.element_type));
    auto published = std::move(output).publish();
    if (!published.ok())
      return Answer(published.status());
    auto result = publication.retain(published.take_value());
    if (!result.ok())
      return Answer(result.status());
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto fragments =
        publication.finish(phase.query.output.descriptor, phase.query.outputs,
                           &result.value(), 1, phase.sets);
    return fragments.ok() ? Answer(fragments.take_value())
                          : Answer(fragments.status());
  }
};
OperationDefinition ordering_operation(const std::string& key, bool quantile,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = quantile ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  if (quantile) {
    traits.input_schema[1].rank = 1;
    traits.input_schema[1].element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.share_blocks_across_outputs = quantile;
  traits.workspace_input_multiplier = 16;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  if (quantile)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  traits.outputs.resize(quantile ? 1 : 2);
  for (std::size_t j = 0; j < traits.outputs.size(); ++j) {
    auto& output = traits.outputs[j];
    output.key = j ? "indices" : "values";
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {1};
    output.region_rule = OperationRegionRule::Dependency;
    output.dependency_version = 1;
    output.continuation_bytes = sizeof(OrderingState);
    output.maximum_dependency_stages = quantile ? 3 : 2;
    output.failure_delivery = FailureDelivery::PerAtomOutcome;
  }
  operation.specialize_metadata = [quantile, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto axis = ordering_axis(inputs[0].descriptor, parameters);
    if (!axis.ok())
      return Answer(axis.status());
    auto shape = inputs[0].descriptor.shape;
    auto type = inputs[0].descriptor.element_type;
    if (quantile) {
      if (inputs[1].descriptor.shape != std::vector<std::uint64_t>{1})
        return Answer(shape_error("quantile q requires scalar [1]"));
      const auto& dtype = std::get<std::string>(parameters.at("dtype"));
      if (dtype != "float32" && dtype != "float64")
        return Answer(numeric_ops::array_parameter_error(
            "quantile requires floating dtype"));
      type = dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
      shape[axis.value()] = 1;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> results(quantile ? 1 : 2);
    results[0].metadata.descriptor = {type, shape};
    if (!quantile)
      results[1].metadata.descriptor = {ElementType::Int64, shape};
    if (!quantile) {
      DependencyMappedNeed data;
      data.port = 0;
      data.roles = 1;
      for (std::size_t j = 0; j < shape.size(); ++j)
        data.axes.push_back(
            j == axis.value()
                ? DependencyAxis{-1, {0, shape[j]}}
                : DependencyAxis{static_cast<std::int32_t>(j), {}});
      auto validation = input_internal::validation_map(data, inputs[0]);
      auto all = Footprint::all(shape);
      if (!all.ok())
        return Answer(all.status());
      for (auto& result : results)
        result.static_dependency_pieces =
            std::vector<DependencyMapPiece>{{all.value(), {data, validation}}};
    }
    return Answer(std::move(results));
  };
  operation.start_dependency = [quantile, profile](const auto& query,
                                                   const auto& allocator) {
    auto axis = ordering_axis(query.inputs[0].descriptor, query.parameters);
    if (!axis.ok())
      return Result<DependencyContinuation>(axis.status());
    return DependencyContinuation::make<OrderingState>(allocator, quantile,
                                                       axis.value(), profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_ordering(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool quantile : {false, true}) {
      auto status = registry->register_operation(ordering_operation(
          std::string(quantile ? "numeric.quantile" : "array.sort") +
              profile.first,
          quantile, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
