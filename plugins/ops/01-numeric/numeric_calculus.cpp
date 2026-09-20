#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_calculus.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Result<ValueDescriptor> metadata(bool integral,
                                 const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<ValueDescriptor>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& source = inputs[0].descriptor;
  if ((source.element_type != ElementType::Float32 &&
       source.element_type != ElementType::Float64) ||
      source.shape.size() != 1 || source.shape[0] < (integral ? 1U : 2U) ||
      source.shape[0] > (UINT64_C(1) << 40))
    return mismatch("calculus requires Float32/64 [N] within 2^40");
  for (unsigned port = 1; port < inputs.size(); ++port)
    if (inputs[port].descriptor.element_type != source.element_type ||
        inputs[port].descriptor.shape != std::vector<std::uint64_t>{1})
      return mismatch("calculus controls require matching dtype and shape [1]");
  return Answer(source);
}
struct CalculusPoint {
  std::uint64_t index = 0, fragment = 0, offset = 0;
};
struct CalculusState final {
  bool integral;
  SequenceProfile profile;
  numeric_ops::ExactCalculus arithmetic;
  unsigned stage = 0;
  std::uint64_t step = 0, initial = 0, cursor = 0, end = 0;
  std::size_t current = 0;
  ResourceVector<CalculusPoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  std::array<std::uint64_t, 4> replicas{};
  CalculusState(bool cumulative, SequenceProfile selected, ElementType dtype)
      : integral(cumulative), profile(selected), arithmetic(selected, dtype) {}
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.calculus/1;%s;exact-rational;replica-store%s",
        integral ? "integrate_1d" : "derivative_1d",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return Status{ErrorCode::Internal, "calculus identity too long"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    return phase.report_numeric(result);
  }
  Status initialize(const DependencyPhase& phase) {
    const auto count = phase.query.outputs.element_count().value();
    auto work = phase.consume_work(count * 4 + 1);
    if (!work.ok())
      return work;
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), 1);
    points.reserve(count);
    outputs.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              box, phase.allocator);
      if (!allocated.ok())
        return allocated.status();
      const auto& span = box.dimensions()[0];
      for (std::uint64_t j = 0; j < span.extent; ++j) {
        work = phase.consume_work(1);
        if (!work.ok())
          return work;
        points.push_back({span.offset + j, outputs.size(), j});
      }
      outputs.push_back(allocated.take_value());
    }
    return Status::success();
  }
  std::pair<std::uint64_t, std::uint64_t> stencil(const DependencyPhase& phase,
                                                  std::uint64_t index) const {
    const auto size = phase.query.inputs[0].descriptor.shape[0];
    return index == 0          ? std::make_pair(UINT64_C(0), UINT64_C(1))
           : index == size - 1 ? std::make_pair(size - 2, size - 1)
                               : std::make_pair(index - 1, index + 1);
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, bool controls) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 8192);
    std::vector<AtomCertificate> rows;
    rows.reserve(points.size());
    for (std::size_t j = 0; j < points.size(); ++j) {
      auto work = phase.consume_work(8);
      if (!work.ok())
        return Answer(work);
      const auto index = points[j].index;
      AtomCertificate row{{index}, {}};
      if (controls) {
        auto scalar = Footprint::all({1}, phase.sets);
        if (!scalar.ok())
          return Answer(scalar.status());
        if (!integral || index > 0)
          row.inputs.push_back({1, 6, scalar.value(), {}});
        if (integral) {
          row.inputs.push_back({2, 1, scalar.value(), {}});
          row.inputs.push_back({2, 4, scalar.value(), {}});
        }
      } else if (!integral || (j >= current && index > 0)) {
        std::vector<Region> boxes;
        if (integral) {
          boxes.emplace_back(
              std::vector<RegionDimension>{{cursor, end - cursor}});
        } else {
          const auto pair = stencil(phase, index);
          boxes.emplace_back(std::vector<RegionDimension>{{pair.first, 1}});
          boxes.emplace_back(std::vector<RegionDimension>{{pair.second, 1}});
        }
        auto support =
            Footprint::from_regions(phase.query.inputs[0].descriptor.shape,
                                    std::move(boxes), phase.sets);
        if (!support.ok())
          return Answer(support.status());
        row.inputs.push_back({0, 1, support.value(), {}});
        row.inputs.push_back({0, 4, support.value(), {}});
      }
      rows.push_back(std::move(row));
    }
    return Answer(DependencyNeedBatch{std::move(rows)});
  }
  Status store(const DependencyPhase& phase, std::size_t point,
               std::uint64_t bits) {
    auto counted = report(phase, 0, 1);
    if (!counted.ok())
      return counted;
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
    std::memcpy(
        outputs[points[point].fragment].data() + points[point].offset * width,
        replicas.data(), width);
    return Status::success();
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(32768);
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    if (!stage) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
      stage = 1;
      return need(phase, true);
    }
    request_capacity.reset();
    if (stage == 1) {
      if (!integral || points.back().index > 0) {
        auto read = phase.read(1, {0}, &step, width);
        if (!read.ok())
          return Answer(read);
        const auto parts = numeric_ops::BinaryParts::decode(step, width == 4);
        if (!parts.magnitude || parts.nan || parts.infinite) {
          const auto at = integral && points[0].index == 0 ? points[1].index
                                                           : points[0].index;
          Status failure{
              ErrorCode::InvalidArgument,
              "InvalidSampleStep: port=1 bits=" + std::to_string(step),
              FailureReason::InvalidDomain,
              {FailureOrigin::Domain, FailureScope::Atom}};
          AtomKey atom;
          atom.output_index = phase.query.output_index;
          atom.rank = 1;
          atom.coordinate[0] = at;
          failure.detail.atom = atom;
          return Answer(failure);
        }
      }
      if (integral) {
        auto read = phase.read(2, {0}, &initial, width);
        if (!read.ok())
          return Answer(read);
      }
      stage = 2;
      if (!integral)
        return need(phase, false);
    } else if (integral) {
      for (auto j = cursor; j < end; ++j) {
        auto charged =
            phase.consume_work(phase.inputs[0].fragments().size() + 1);
        if (!charged.ok())
          return Answer(charged);
        std::uint64_t bits = 0;
        auto read = phase.read(0, {j}, &bits, width);
        if (!read.ok())
          return Answer(read);
        charged = report(phase, 1, 0);
        if (!charged.ok())
          return Answer(charged);
        auto added = arithmetic.add(bits, phase.consume_work);
        if (!added.ok())
          return Answer(added);
      }
      cursor = end;
    }
    while (current < points.size()) {
      const auto index = points[current].index;
      if (integral && index > 0 && cursor <= index) {
        end = cursor + std::min(UINT64_C(64), index + 1 - cursor);
        return need(phase, false);
      }
      Result<std::uint64_t> result(initial);
      if (integral && index > 0) {
        result = arithmetic.integral(step, initial, phase.consume_work);
      } else if (!integral) {
        const auto pair = stencil(phase, index);
        std::uint64_t low = 0, high = 0;
        auto charged =
            phase.consume_work(2 * phase.inputs[0].fragments().size() + 2);
        if (!charged.ok())
          return Answer(charged);
        auto read = phase.read(0, {pair.first}, &low, width);
        if (!read.ok())
          return Answer(read);
        read = phase.read(0, {pair.second}, &high, width);
        if (!read.ok())
          return Answer(read);
        charged = report(phase, 1, 0);
        if (!charged.ok())
          return Answer(charged);
        result = arithmetic.derivative(
            low, high, step, pair.second - pair.first == 2, phase.consume_work);
      }
      if (!result.ok())
        return Answer(result.status());
      auto status = store(phase, current, result.value());
      if (!status.ok())
        return Answer(status);
      ++current;
    }
    ResourceVector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto published = std::move(output).publish();
      if (!published.ok())
        return Answer(published.status());
      auto retained = publication->retain(published.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result =
        publication->finish(phase.query.output.descriptor, phase.query.outputs,
                            values.data(), values.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition calculus_operation(const std::string& key, bool integral,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = integral ? 3 : 2;
  traits.input_schema.resize(traits.input_count);
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.continuation_bytes = sizeof(CalculusState);
  output.maximum_dependency_stages = integral ? 1048576 : 3;
  operation.specialize_metadata =
      [integral, profile](
          const auto& inputs,
          const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(integral, inputs);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = resolved.take_value();
    result.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [integral, profile](const auto& query,
                                                   const auto& allocator) {
    return DependencyContinuation::make<CalculusState>(
        allocator, integral, profile, query.inputs[0].descriptor.element_type);
  };
  return operation;
}
}  // namespace
Status register_numeric_calculus(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(calculus_operation(
          std::string("numeric.") +
              (integral ? "integrate_1d" : "derivative_1d") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
