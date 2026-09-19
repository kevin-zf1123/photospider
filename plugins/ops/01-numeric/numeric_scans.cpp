#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_aggregate.hpp"
#include "01-numeric/ordered_reduction.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct ScanMetadata {
  unsigned mask = 0, axis = 0;
  ValueDescriptor output;
};
Result<ScanMetadata> metadata(
    bool integral, const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ScanMetadata>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& shape = input.descriptor.shape;
  if (shape.size() < (integral ? 2U : 1U) || shape.size() > 8)
    return mismatch("scan requires rank 1..8 (integral rank >=2)");
  ScanMetadata result;
  result.output = input.descriptor;
  std::vector<std::uint64_t> axes;
  if (integral) {
    auto parsed = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("axes")), false);
    if (!parsed.ok())
      return Answer(parsed.status());
    axes = parsed.take_value();
    if (axes.size() != 2)
      return Answer(numeric_ops::array_parameter_error("require two axes"));
  } else {
    const auto axis = std::get<std::int64_t>(parameters.at("axis"));
    if (axis < 0)
      return Answer(numeric_ops::array_parameter_error("negative scan axis"));
    axes.push_back(static_cast<std::uint64_t>(axis));
  }
  for (auto axis : axes) {
    if (axis >= shape.size() || (result.mask & (1U << axis)))
      return Answer(numeric_ops::array_parameter_error("invalid scan axes"));
    result.mask |= 1U << axis;
    result.axis = static_cast<unsigned>(axis);
  }
  std::uint64_t source_count = 1, output_count = 1;
  for (std::size_t j = 0; j < shape.size(); ++j) {
    if (!shape[j] || shape[j] > (UINT64_C(1) << 40) / source_count)
      return mismatch("scan input exceeds 2^40 elements");
    source_count *= shape[j];
    result.output.shape[j] += (result.mask >> j) & 1;
    if (result.output.shape[j] > (UINT64_C(1) << 40) / output_count)
      return mismatch("scan output exceeds 2^40 elements");
    output_count *= result.output.shape[j];
  }
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
    return Answer(numeric_ops::array_parameter_error("unsupported scan dtype"));
  const auto floating = [](ElementType type) {
    return type == ElementType::Float32 || type == ElementType::Float64;
  };
  if (floating(input.descriptor.element_type) !=
      floating(result.output.element_type))
    return mismatch("scan source/destination numeric domains differ");
  return Answer(std::move(result));
}
struct ScanPoint {
  std::array<std::uint64_t, 8> coordinate{};
  std::uint64_t fragment = 0, offset = 0;
};
struct ScanState final {
  bool integral, initialized = false, requested = false;
  SequenceProfile profile;
  ScanMetadata description;
  numeric_ops::ExactAggregate accumulator, conversion;
  ResourceVector<ScanPoint> points;
  ResourceVector<MutableValue> outputs;
  std::size_t current = 0;
  std::uint64_t cursor = 0, end = 0;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  std::array<std::uint64_t, 4> replicas{};
  ScanState(bool rectangle, SequenceProfile selected, ScanMetadata resolved,
            ElementType source)
      : integral(rectangle),
        profile(selected),
        description(std::move(resolved)),
        accumulator(selected, numeric_ops::AggregateKind::Sum, source),
        conversion(selected, numeric_ops::AggregateKind::Sum, source) {}
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.scan/1;%s;exact-limbs;replica-store%s",
        integral ? "integral_image" : "prefix_sum",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return Status{ErrorCode::Internal, "scan identity too long"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    return phase.report_numeric(result);
  }
  bool same_line(const ScanPoint& a, const ScanPoint& b) const {
    for (std::size_t j = 0; j < description.output.shape.size(); ++j)
      if (j != description.axis && a.coordinate[j] != b.coordinate[j])
        return false;
    return true;
  }
  std::uint64_t count() const {
    std::uint64_t result = 1;
    for (std::size_t j = 0; j < description.output.shape.size(); ++j)
      if (description.mask & (1U << j))
        result *= points[current].coordinate[j];
    return result;
  }
  Result<Footprint> window(const DependencyPhase& phase) const {
    auto shape = phase.query.inputs[0].descriptor.shape;
    for (std::size_t j = 0; j < shape.size(); ++j)
      shape[j] =
          (description.mask & (1U << j)) ? points[current].coordinate[j] : 1;
    auto local = numeric_ops::ordered_range(shape, cursor, end, phase.sets);
    if (!local.ok())
      return Result<Footprint>(local.status());
    std::vector<Region> boxes;
    for (const auto& box : local.value().boxes()) {
      auto dimensions = box.dimensions();
      for (std::size_t j = 0; j < shape.size(); ++j)
        if (!(description.mask & (1U << j)))
          dimensions[j].offset = points[current].coordinate[j];
      boxes.emplace_back(std::move(dimensions));
    }
    return Footprint::from_regions(phase.query.inputs[0].descriptor.shape,
                                   std::move(boxes), phase.sets);
  }
  Status initialize(const DependencyPhase& phase) {
    const auto total = phase.query.outputs.element_count().value();
    const auto rank = description.output.shape.size();
    auto charged = phase.consume_work(total * 2 * rank);
    if (!charged.ok())
      return charged;
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), rank);
    points.reserve(total);
    outputs.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated =
          MutableValue::allocate(description.output, box, phase.allocator);
      if (!allocated.ok())
        return allocated.status();
      auto region =
          Footprint::from_regions(description.output.shape, {box}, phase.sets);
      if (!region.ok())
        return region.status();
      std::uint64_t offset = 0;
      auto visited = region.value().visit(
          [&](const auto& coordinate) {
            ScanPoint point;
            std::copy(coordinate.begin(), coordinate.end(),
                      point.coordinate.begin());
            point.fragment = outputs.size();
            point.offset = offset++;
            points.push_back(point);
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!visited.ok())
        return visited;
      outputs.push_back(allocated.take_value());
    }
    if (!integral) {
      // Comparator failure unwinds the private, unpublished point plan. This
      // keeps cancellation/work admission responsive during metadata sorting.
      try {
        std::sort(
            points.begin(), points.end(), [&](const auto& a, const auto& b) {
              auto admitted = phase.consume_work(rank + 1);
              if (!admitted.ok())
                throw admitted;
              for (std::size_t j = 0; j < rank; ++j)
                if (j != description.axis && a.coordinate[j] != b.coordinate[j])
                  return a.coordinate[j] < b.coordinate[j];
              return a.coordinate[description.axis] <
                     b.coordinate[description.axis];
            });
      } catch (const Status& failure) {
        return failure;
      }
    }
    initialized = true;
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, Footprint data) {
    using Answer = Result<DependencyPoll>;
    auto validation = data;
    for (const auto& facet : phase.query.inputs[0].facets) {
      if (facet.key != "photospider.image" &&
          facet.key != "photospider.semantic")
        continue;
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return Answer(semantic.status());
      if (semantic.value().kind == SemanticKind::Image) {
        std::vector<Region> boxes;
        for (const auto& box : data.boxes()) {
          auto dimensions = box.dimensions();
          dimensions[2] = {0, phase.query.inputs[0].descriptor.shape[2]};
          boxes.emplace_back(std::move(dimensions));
        }
        auto closure =
            Footprint::from_regions(phase.query.inputs[0].descriptor.shape,
                                    std::move(boxes), phase.sets);
        if (!closure.ok())
          return Answer(closure.status());
        validation = closure.take_value();
      }
    }
    request_capacity =
        dependency_internal::metadata_owner(32768 + points.size() * 8192);
    std::vector<AtomCertificate> rows;
    rows.reserve(points.size());
    const auto rank = description.output.shape.size();
    for (std::size_t j = 0; j < points.size(); ++j) {
      auto charged = phase.consume_work(rank + 1);
      if (!charged.ok())
        return Answer(charged);
      AtomCertificate row{
          std::vector<std::uint64_t>(points[j].coordinate.begin(),
                                     points[j].coordinate.begin() + rank),
          {}};
      if (j == current ||
          (!integral && j > current && same_line(points[current], points[j])))
        row.inputs = {{0, 1, data, {}}, {0, 4, validation, {}}};
      rows.push_back(std::move(row));
    }
    requested = true;
    return Answer(DependencyNeedBatch{std::move(rows)});
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(32768);
    if (!initialized) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
    }
    request_capacity.reset();
    if (requested) {
      auto source = window(phase);
      if (!source.ok())
        return Answer(source.status());
      auto status = source.value().visit(
          [&](const auto& coordinate) {
            auto charged = phase.consume_work(
                (phase.inputs[0].fragments().size() + 1) * coordinate.size() +
                1);
            if (!charged.ok())
              return charged;
            std::uint64_t bits = 0;
            auto read =
                phase.read(0, coordinate, &bits,
                           Value::element_size(
                               phase.query.inputs[0].descriptor.element_type));
            if (!read.ok())
              return read;
            charged = report(phase, 1, 0);
            return charged.ok() ? accumulator.add(bits, phase.consume_work)
                                : charged;
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      cursor = end;
      requested = false;
    }
    while (current < points.size()) {
      const auto required = count();
      if (cursor < required) {
        end = cursor + std::min(UINT64_C(64), required - cursor);
        auto source = window(phase);
        return source.ok() ? need(phase, source.take_value())
                           : Answer(source.status());
      }
      // Final conversion may destroy its workspace. Continue only from the
      // original exact carry, including source NaN and infinity
      // classifications.
      auto status = phase.consume_work(sizeof(accumulator) / 8 + 1);
      if (!status.ok())
        return Answer(status);
      conversion = accumulator;
      auto calculated =
          required ? conversion.finish_as(description.output.element_type, 1,
                                          phase.consume_work)
                   : Result<std::uint64_t>(UINT64_C(0));
      if (!calculated.ok()) {
        auto failure = calculated.status();
        if (failure.reason == FailureReason::ArithmeticOverflow) {
          failure.detail.origin = FailureOrigin::Domain;
          failure.detail.scope = FailureScope::Atom;
          failure.detail.atom = AtomKey{
              phase.query.output_index,
              static_cast<std::uint32_t>(description.output.shape.size()),
              points[current].coordinate};
        }
        return Answer(failure);
      }
      status = report(phase, 0, 1);
      if (!status.ok())
        return Answer(status);
      numeric_ops::select_words(replicas.data(), calculated.value(),
                                calculated.value(), 1, profile);
      const auto width = Value::element_size(description.output.element_type);
      std::memcpy(outputs[points[current].fragment].data() +
                      points[current].offset * width,
                  replicas.data(), width);
      ++current;
      if (integral || (current < points.size() &&
                       !same_line(points[current - 1], points[current]))) {
        accumulator.reset();
        cursor = 0;
      }
    }
    ResourceVector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto value = std::move(output).publish();
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication->retain(value.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result = publication->finish(description.output, phase.query.outputs,
                                      values.data(), values.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition scan_operation(const std::string& key, bool integral,
                                   SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {integral ? "axes" : "axis", integral ? OperationParameterType::String
                                            : OperationParameterType::Int64},
      {"dtype", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.continuation_bytes = sizeof(ScanState);
  output.maximum_dependency_stages = 1048576;
  operation.specialize_metadata = [integral, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(integral, inputs[0], parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = resolved.value().output;
    result.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [integral, profile](const auto& query,
                                                   const auto& allocator) {
    auto resolved = metadata(integral, query.inputs[0], query.parameters);
    if (!resolved.ok())
      return Result<DependencyContinuation>(resolved.status());
    return DependencyContinuation::make<ScanState>(
        allocator, integral, profile, resolved.take_value(),
        query.inputs[0].descriptor.element_type);
  };
  return operation;
}
}  // namespace
Status register_numeric_scans(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(scan_operation(
          std::string("numeric.") +
              (integral ? "integral_image" : "prefix_sum") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
