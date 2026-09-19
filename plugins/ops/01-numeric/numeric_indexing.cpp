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
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_aggregate.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct IndexFailure {
  Status status;
};
void checked(Status status) {
  if (!status.ok())
    throw IndexFailure{std::move(status)};
}
template <class T>
T taken(Result<T> value) {
  checked(value.status());
  return value.take_value();
}
void shape_valid(const ValueDescriptor& descriptor) {
  std::uint64_t count = 1;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    throw IndexFailure{
        Status{ErrorCode::TypeMismatch,
               "indexing rank outside 1..8",
               FailureReason::None,
               {FailureOrigin::Schema, FailureScope::Unspecified}}};
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      throw IndexFailure{
          Status{ErrorCode::TypeMismatch,
                 "indexing array exceeds 2^40 elements",
                 FailureReason::None,
                 {FailureOrigin::Schema, FailureScope::Unspecified}}};
    count *= extent;
  }
}
std::uint32_t static_axis(
    const std::map<std::string, ParameterValue>& parameters, std::size_t rank) {
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= rank)
    throw IndexFailure{
        numeric_ops::array_parameter_error("indexing axis outside rank")};
  return static_cast<std::uint32_t>(axis);
}
DependencyMappedNeed typed_map(DependencyMappedNeed data,
                               const OperationMetadata& metadata) {
  data = input_internal::validation_map(std::move(data), metadata);
  return data;
}
Status report_index(const DependencyPhase& phase, SequenceProfile profile,
                    const char* operation, std::uint64_t count, bool view,
                    bool evaluated = true, bool formed = true) {
  NumericDiagnostics report;
  report.profile =
      static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
  const auto length =
      std::snprintf(report.implementation.data(), report.implementation.size(),
                    "photospider.indexing/1;%s;%s", operation,
                    numeric_ops::array_implementation(profile, view));
  if (length < 0 ||
      static_cast<std::size_t>(length) >= report.implementation.size())
    return Status{ErrorCode::OperationFailed,
                  "indexing diagnostics identity too long"};
  report.evaluated_values = evaluated ? count : 0;
  report.view_elements = formed && view ? count : 0;
  report.copied_elements = formed && !view ? count : 0;
  return phase.report_numeric(report);
}
struct ConcatenateState final {
  SequenceProfile profile;
  bool requested = false;
  std::uint32_t axis = 0, ports = 0;
  std::array<std::uint64_t, 257> prefixes{};
  std::array<std::uint8_t, 32> block{};
  ConcatenateState(SequenceProfile selected, const DependencyQuery& query)
      : profile(selected),
        axis(static_axis(query.parameters,
                         query.output.descriptor.shape.size())),
        ports(query.inputs.size()) {
    for (std::uint32_t port = 0; port < ports; ++port)
      prefixes[port + 1] =
          prefixes[port] + query.inputs[port].descriptor.shape[axis];
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    try {
      if (!requested) {
        requested = true;
        return Answer(DependencyNeedBatch{{}, {}, true});
      }
      const bool view =
          std::get<std::string>(phase.query.parameters.at("layout")) == "view";
      const auto& descriptor = phase.query.output.descriptor;
      const auto rank = descriptor.shape.size();
      const auto width = Value::element_size(descriptor.element_type);
      std::uint64_t maximum_fragments = phase.query.outputs.boxes().size();
      if (view) {
        maximum_fragments = 0;
        for (const auto& input : phase.inputs) {
          checked(phase.consume_work(1));
          const auto incoming = input.fragments().size();
          const auto boxes = phase.query.outputs.boxes().size();
          if (incoming > (phase.sets.maximum_boxes - maximum_fragments) / boxes)
            return Answer(Status{ErrorCode::ResourceExhausted,
                                 "concatenate fragment capacity",
                                 FailureReason::CapacityLimit});
          maximum_fragments += incoming * boxes;
        }
      }
      numeric_ops::ArrayPublication publication(maximum_fragments, rank);
      ResourceVector<Value> results;
      results.reserve(maximum_fragments);
      for (const auto& box : phase.query.outputs.boxes()) {
        if (view) {
          for (std::uint32_t port = 0; port < ports; ++port) {
            checked(phase.consume_work(rank + 1));
            const auto requested_axis = box.dimensions()[axis];
            const auto begin = std::max(requested_axis.offset, prefixes[port]);
            const auto end =
                std::min(requested_axis.offset + requested_axis.extent,
                         prefixes[port + 1]);
            if (begin >= end)
              continue;
            auto wanted = box.dimensions();
            wanted[axis] = {begin - prefixes[port], end - begin};
            for (const auto& fragment : phase.inputs[port].fragments()) {
              checked(phase.consume_work(rank + 1));
              auto clipped = wanted;
              bool hit = true;
              for (std::size_t j = 0; j < rank; ++j) {
                const auto source = fragment.region().dimensions()[j];
                const auto low = std::max(clipped[j].offset, source.offset);
                const auto high =
                    std::min(clipped[j].offset + clipped[j].extent,
                             source.offset + source.extent);
                if (low >= high) {
                  hit = false;
                  break;
                }
                clipped[j] = {low, high - low};
              }
              if (!hit)
                continue;
              std::vector<std::uint64_t> origin;
              for (const auto& dimension : clipped)
                origin.push_back(dimension.offset);
              const auto offset = taken(fragment.byte_address(origin));
              origin[axis] += prefixes[port];
              clipped[axis].offset += prefixes[port];
              auto value = taken(Value::from_storage(
                  descriptor, Region(std::move(clipped)),
                  {offset, fragment.layout().byte_strides, std::move(origin)},
                  fragment.storage()));
              results.push_back(taken(publication.retain(std::move(value))));
              checked(report_index(
                  phase, profile, "concatenate",
                  taken(results.back().region().element_count()), true));
            }
          }
        } else {
          auto output =
              taken(MutableValue::allocate(descriptor, box, phase.allocator));
          auto selected = taken(
              Footprint::from_regions(descriptor.shape, {box}, phase.sets));
          std::uint64_t offset = 0;
          std::size_t buffered = 0;
          checked(selected.visit(
              [&](const auto& coordinate) {
                checked(phase.consume_work(rank + 10));
                const auto found = std::upper_bound(
                    prefixes.begin(), prefixes.begin() + ports + 1,
                    coordinate[axis]);
                const auto port =
                    static_cast<std::uint32_t>(found - prefixes.begin() - 1);
                auto source = coordinate;
                source[axis] -= prefixes[port];
                checked(
                    phase.read(port, source, block.data() + buffered, width));
                checked(report_index(phase, profile, "concatenate", 1, false,
                                     true, false));
                buffered += width;
                if (buffered == block.size()) {
                  checked(report_index(phase, profile, "concatenate",
                                       buffered / width, false, false, true));
                  numeric_ops::array_copy_block(
                      output.data() + offset, block.data(), buffered, profile);
                  offset += buffered;
                  buffered = 0;
                }
                return Status::success();
              },
              phase.sets.maximum_work, phase.query.cancellation));
          if (buffered) {
            checked(report_index(phase, profile, "concatenate",
                                 buffered / width, false, false, true));
            numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                          buffered, profile);
          }
          results.push_back(
              taken(publication.retain(taken(std::move(output).publish()))));
        }
      }
      if (phase.query.cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      return Answer(taken(publication.finish(descriptor, phase.query.outputs,
                                             results.data(), results.size(),
                                             phase.sets)));
    } catch (const IndexFailure& failure) {
      return Answer(failure.status);
    }
  }
};
enum class IndexKind { Gather, Replace, Sum, Minimum, Maximum };
struct IndexPair {
  std::uint64_t key = 0, position = 0;
};
struct IndexState final {
  IndexKind kind;
  SequenceProfile profile;
  std::uint32_t axis;
  unsigned stage = 0;
  ResourceVector<IndexPair> plan, sorting;
  std::array<std::uint64_t, 256> bins{};
  std::array<std::uint8_t, 32> block{};
  std::array<std::uint64_t, 4> replicas{};
  numeric_ops::ExactAggregate aggregate;
  std::shared_ptr<const dependency_internal::MetadataOwner> construction;
  IndexState(IndexKind operation, SequenceProfile selected,
             const DependencyQuery& query)
      : kind(operation),
        profile(selected),
        axis(static_axis(query.parameters,
                         query.output.descriptor.shape.size())),
        aggregate(selected,
                  operation == IndexKind::Minimum
                      ? numeric_ops::AggregateKind::Minimum
                  : operation == IndexKind::Maximum
                      ? numeric_ops::AggregateKind::Maximum
                      : numeric_ops::AggregateKind::Sum,
                  query.output.descriptor.element_type) {}
  const char* implementation() const {
    return kind == IndexKind::Gather    ? "gather/replica-store"
           : kind == IndexKind::Replace ? "scatter-replace/replica-store"
           : kind == IndexKind::Sum     ? "scatter-exact-sum/replica-store"
           : kind == IndexKind::Minimum ? "scatter-minimum/replica-store"
                                        : "scatter-maximum/replica-store";
  }
  bool aggregation() const {
    return kind == IndexKind::Sum || kind == IndexKind::Minimum ||
           kind == IndexKind::Maximum;
  }
  std::pair<std::size_t, std::size_t> matches(const DependencyPhase& phase,
                                              std::uint64_t key) const {
    const auto levels = plan.empty() ? 0U : 64U - __builtin_clzll(plan.size());
    checked(phase.consume_work(2 * (levels + 1)));
    const auto first = std::lower_bound(
        plan.begin(), plan.end(), key,
        [](const auto& item, auto target) { return item.key < target; });
    const auto last = std::upper_bound(
        first, plan.end(), key,
        [](auto target, const auto& item) { return target < item.key; });
    return {static_cast<std::size_t>(first - plan.begin()),
            static_cast<std::size_t>(last - plan.begin())};
  }
  Region point(const std::vector<std::uint64_t>& coordinate) const {
    std::vector<RegionDimension> dimensions;
    dimensions.reserve(coordinate.size());
    for (auto value : coordinate)
      dimensions.push_back({value, 1});
    return Region(std::move(dimensions));
  }
  std::vector<std::uint64_t> offending(const DependencyPhase& phase,
                                       std::uint64_t position) const {
    for (const auto& box : phase.query.outputs.boxes()) {
      if (kind == IndexKind::Gather &&
          (position < box.dimensions()[axis].offset ||
           position - box.dimensions()[axis].offset >=
               box.dimensions()[axis].extent))
        continue;
      std::vector<std::uint64_t> result;
      for (const auto& dimension : box.dimensions())
        result.push_back(dimension.offset);
      if (kind == IndexKind::Gather)
        result[axis] = position;
      return result;
    }
    throw IndexFailure{
        Status{ErrorCode::Internal, "missing invalid index observation"}};
  }
  Status attributed(const DependencyPhase& phase, Status status,
                    const std::vector<std::uint64_t>& coordinate) const {
    status.detail.origin = FailureOrigin::Domain;
    status.detail.scope = FailureScope::Atom;
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = coordinate.size();
    std::copy(coordinate.begin(), coordinate.end(), atom.coordinate.begin());
    status.detail.atom = atom;
    return status;
  }
  void validate_index(const DependencyPhase& phase, std::uint64_t position,
                      std::int64_t value) const {
    const auto extent = phase.query.inputs[0].descriptor.shape[axis];
    if (value < 0 || static_cast<std::uint64_t>(value) >= extent)
      throw IndexFailure{attributed(
          phase,
          Status{ErrorCode::InvalidArgument,
                 "IndexOutOfBounds: position=" + std::to_string(position) +
                     " index=" + std::to_string(value) +
                     " extent=" + std::to_string(extent),
                 FailureReason::InvalidDomain},
          offending(phase, position))};
  }
  Footprint used_indices(const DependencyPhase& phase) const {
    const auto& shape = phase.query.inputs[1].descriptor.shape;
    if (kind != IndexKind::Gather)
      return taken(Footprint::all(shape, phase.sets));
    std::vector<Region> ranges;
    ranges.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      checked(phase.consume_work(1));
      ranges.emplace_back(std::vector<RegionDimension>{box.dimensions()[axis]});
    }
    return taken(Footprint::from_regions(shape, ranges, phase.sets));
  }
  void resolve_indices(const DependencyPhase& phase) {
    const auto requested = used_indices(phase);
    const auto count = taken(requested.element_count());
    checked(phase.consume_work(count));
    plan.reserve(count);
    checked(requested.visit(
        [&](const auto& coordinate) {
          std::int64_t value = 0;
          checked(phase.read(1, coordinate, &value, 8));
          validate_index(phase, coordinate[0], value);
          plan.push_back(
              kind == IndexKind::Gather
                  ? IndexPair{coordinate[0], static_cast<std::uint64_t>(value)}
                  : IndexPair{static_cast<std::uint64_t>(value),
                              coordinate[0]});
          return Status::success();
        },
        phase.sets.maximum_work, phase.query.cancellation));
    if (kind == IndexKind::Gather)
      return;
    // Stable LSD radix buckets retain increasing update j within each target.
    // Eight bounded passes give O(M) index grouping, independent of duplicates.
    sorting.resize(plan.size());
    for (unsigned byte = 0; byte < 8; ++byte) {
      checked(phase.consume_work(bins.size()));
      bins.fill(0);
      for (const auto& item : plan) {
        checked(phase.consume_work(1));
        ++bins[(item.key >> (8 * byte)) & 255];
      }
      std::uint64_t prefix = 0;
      for (auto& bin : bins) {
        const auto size = bin;
        bin = prefix;
        prefix += size;
      }
      for (const auto& item : plan) {
        checked(phase.consume_work(1));
        sorting[bins[(item.key >> (8 * byte)) & 255]++] = item;
      }
      plan.swap(sorting);
    }
    ResourceVector<IndexPair>{}.swap(sorting);
  }
  void declare(const DependencyPhase& phase, std::uint32_t port,
               const std::vector<Region>& points,
               std::vector<DependencyNeed>* needs) const {
    if (points.empty())
      return;
    const auto& metadata = phase.query.inputs[port];
    auto data = taken(
        Footprint::from_regions(metadata.descriptor.shape, points, phase.sets));
    auto validation = taken(input_internal::validation_closure(
        metadata, data, phase.sets, phase.consume_work));
    needs->push_back({port, 1, std::move(data), {}});
    needs->push_back({port, 4, std::move(validation), {}});
  }
  DependencyNeedBatch need(const DependencyPhase& phase, bool controls) {
    const auto rows_count = taken(phase.query.observations.element_count());
    if (rows_count > phase.sets.maximum_boxes)
      throw IndexFailure{Status{ErrorCode::ResourceExhausted,
                                "index association capacity",
                                FailureReason::CapacityLimit}};
    std::uint64_t points_count = rows_count;
    if (!controls && kind != IndexKind::Gather) {
      points_count = 0;
      checked(phase.query.observations.visit(
          [&](const auto& coordinate) {
            const auto range = matches(phase, coordinate[axis]);
            const auto count = range.second - range.first;
            const auto incoming =
                count && kind == IndexKind::Replace ? 1 : 1 + count;
            if (incoming > phase.sets.maximum_boxes - points_count)
              return Status{ErrorCode::ResourceExhausted,
                            "contributor association capacity",
                            FailureReason::CapacityLimit};
            points_count += incoming;
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation));
    }
    dependency_internal::MetadataBytes capacity;
    capacity.add(4096);
    capacity.add(rows_count, sizeof(AtomCertificate) + 8 * 8);
    capacity.add(points_count,
                 1024 + phase.query.output.descriptor.shape.size() * 256);
    construction = dependency_internal::metadata_owner(capacity.bytes);
    std::vector<AtomCertificate> rows;
    rows.reserve(rows_count);
    Footprint all_indices;
    if (controls && kind != IndexKind::Gather)
      all_indices = used_indices(phase);
    checked(phase.query.observations.visit(
        [&](const auto& coordinate) {
          checked(phase.consume_work(coordinate.size() + 1));
          AtomCertificate row{coordinate, {}};
          row.inputs.reserve(4);
          if (controls) {
            auto indices =
                kind == IndexKind::Gather
                    ? taken(Footprint::from_regions(
                          phase.query.inputs[1].descriptor.shape,
                          {Region({{coordinate[axis], 1}})}, phase.sets))
                    : all_indices;
            row.inputs.push_back({1, 6, std::move(indices), {}});
          } else {
            const auto range = matches(phase, coordinate[axis]);
            std::vector<Region> source, updates;
            if (kind == IndexKind::Gather) {
              if (range.second != range.first + 1)
                throw IndexFailure{
                    Status{ErrorCode::Internal, "gather index plan hole"}};
              auto mapped = coordinate;
              mapped[axis] = plan[range.first].position;
              source.push_back(point(mapped));
            } else {
              if (range.first == range.second || aggregation())
                source.push_back(point(coordinate));
              const auto begin =
                  kind == IndexKind::Replace && range.first != range.second
                      ? range.second - 1
                      : range.first;
              updates.reserve(range.second - begin);
              for (auto j = begin; j < range.second; ++j) {
                checked(phase.consume_work(coordinate.size() + 1));
                auto mapped = coordinate;
                mapped[axis] = plan[j].position;
                updates.push_back(point(mapped));
              }
            }
            declare(phase, 0, source, &row.inputs);
            if (!updates.empty())
              declare(phase, 2, updates, &row.inputs);
          }
          rows.push_back(std::move(row));
          return Status::success();
        },
        phase.sets.maximum_work, phase.query.cancellation));
    return DependencyNeedBatch{std::move(rows)};
  }
  std::uint64_t evaluate(const DependencyPhase& phase,
                         const std::vector<std::uint64_t>& coordinate) {
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    const auto read = [&](std::uint32_t port, const auto& sample) {
      std::uint64_t bits = 0;
      checked(phase.read(port, sample, &bits, width));
      return bits;
    };
    const auto range = matches(phase, coordinate[axis]);
    if (kind == IndexKind::Gather) {
      auto source = coordinate;
      source[axis] = plan[range.first].position;
      return read(0, source);
    }
    if (range.first == range.second)
      return read(0, coordinate);
    if (kind == IndexKind::Replace) {
      auto source = coordinate;
      source[axis] = plan[range.second - 1].position;
      return read(2, source);
    }
    aggregate.reset();
    checked(aggregate.add(read(0, coordinate), phase.consume_work));
    for (auto j = range.first; j < range.second; ++j) {
      auto source = coordinate;
      source[axis] = plan[j].position;
      checked(aggregate.add(read(2, source), phase.consume_work));
    }
    auto result = aggregate.finish(phase.consume_work);
    if (!result.ok() &&
        result.status().reason == FailureReason::ArithmeticOverflow)
      throw IndexFailure{attributed(phase, result.status(), coordinate)};
    return taken(std::move(result));
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    try {
      if (!stage) {
        ++stage;
        return Answer(need(phase, true));
      }
      if (stage == 1) {
        resolve_indices(phase);
        ++stage;
        return Answer(need(phase, false));
      }
      construction.reset();
      numeric_ops::ArrayPublication publication(
          phase.query.outputs.boxes().size(),
          phase.query.output.descriptor.shape.size());
      ResourceVector<Value> results;
      results.reserve(phase.query.outputs.boxes().size());
      const auto& descriptor = phase.query.output.descriptor;
      const auto width = Value::element_size(descriptor.element_type);
      for (const auto& box : phase.query.outputs.boxes()) {
        auto output =
            taken(MutableValue::allocate(descriptor, box, phase.allocator));
        auto selected =
            taken(Footprint::from_regions(descriptor.shape, {box}, phase.sets));
        std::uint64_t offset = 0;
        std::size_t buffered = 0;
        checked(selected.visit(
            [&](const auto& coordinate) {
              checked(phase.consume_work(coordinate.size() + 16));
              checked(report_index(phase, profile, implementation(), 1, false,
                                   true, false));
              const auto bits = evaluate(phase, coordinate);
              numeric_ops::select_words(replicas.data(), bits, bits, 1,
                                        profile);
              std::memcpy(block.data() + buffered, replicas.data(), width);
              buffered += width;
              if (buffered == block.size()) {
                checked(report_index(phase, profile, implementation(),
                                     buffered / width, false, false, true));
                numeric_ops::array_copy_block(output.data() + offset,
                                              block.data(), buffered, profile);
                offset += buffered;
                buffered = 0;
              }
              return Status::success();
            },
            phase.sets.maximum_work, phase.query.cancellation));
        if (buffered) {
          checked(report_index(phase, profile, implementation(),
                               buffered / width, false, false, true));
          numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                        buffered, profile);
        }
        results.push_back(
            taken(publication.retain(taken(std::move(output).publish()))));
      }
      if (phase.query.cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      return Answer(taken(publication.finish(descriptor, phase.query.outputs,
                                             results.data(), results.size(),
                                             phase.sets)));
    } catch (const IndexFailure& failure) {
      return Answer(failure.status);
    }
  }
};
OperationDefinition index_operation(const std::string& key, IndexKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = kind == IndexKind::Gather ? 2 : 3;
  traits.input_schema.resize(traits.input_count);
  traits.input_schema[1].rank = 1;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.continuation_bytes = sizeof(IndexState);
  output.maximum_dependency_stages = 3;
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      for (const auto& input : inputs)
        shape_valid(input.descriptor);
      const auto& source = inputs[0].descriptor;
      const auto axis = static_axis(parameters, source.shape.size());
      auto output_shape = source.shape;
      output_shape[axis] = inputs[1].descriptor.shape[0];
      if (kind != IndexKind::Gather &&
          (inputs[2].descriptor.element_type != source.element_type ||
           inputs[2].descriptor.shape != output_shape))
        return Answer(Status{
            ErrorCode::TypeMismatch,
            "scatter update dtype/non-axis extents or index count mismatch",
            FailureReason::None,
            {FailureOrigin::Schema, FailureScope::Unspecified}});
      OperationOutputSpecialization result;
      result.metadata.descriptor = {
          source.element_type,
          kind == IndexKind::Gather ? output_shape : source.shape};
      shape_valid(result.metadata.descriptor);
      result.regional_atomic = true;
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const IndexFailure& failure) {
      return Answer(failure.status);
    }
  };
  operation.start_dependency = [kind, profile](const auto& query,
                                               const auto& allocator) {
    return DependencyContinuation::make<IndexState>(allocator, kind, profile,
                                                    query);
  };
  return operation;
}
OperationDefinition concatenate_operation(const std::string& key,
                                          SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 0;
  traits.repeated_minimum = 2;
  traits.repeated_maximum = 256;
  traits.repeated_match = false;
  traits.requires_metadata_specialization = true;
  traits.input_schema.resize(1);
  traits.parameter_schema = {{"axis", OperationParameterType::Int64},
                             {"layout", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ConcatenateState);
  output.maximum_dependency_stages = 2;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    try {
      checked(numeric_ops::sequence_profile_available(profile));
      const auto& first = inputs[0].descriptor;
      const auto axis = static_axis(parameters, first.shape.size());
      auto descriptor = first;
      descriptor.shape[axis] = 0;
      for (const auto& input : inputs) {
        shape_valid(input.descriptor);
        bool match = input.descriptor.element_type == first.element_type &&
                     input.descriptor.shape.size() == first.shape.size();
        for (std::size_t j = 0; match && j < first.shape.size(); ++j)
          if (j != axis)
            match &= input.descriptor.shape[j] == first.shape[j];
        if (!match)
          return Answer(
              Status{ErrorCode::TypeMismatch,
                     "concatenate dtype/rank/non-axis extents mismatch",
                     FailureReason::None,
                     {FailureOrigin::Schema, FailureScope::Unspecified}});
        descriptor.shape[axis] += input.descriptor.shape[axis];
      }
      shape_valid(descriptor);
      const auto& layout = std::get<std::string>(parameters.at("layout"));
      if (layout != "view" && layout != "dense")
        return Answer(numeric_ops::array_parameter_error(
            "concatenate layout must be view or dense"));
      OperationOutputSpecialization result;
      result.metadata.descriptor = descriptor;
      result.preserve_output_views = layout == "view";
      if (layout == "view")
        result.maximum_output_payload_bytes = 0;
      std::vector<DependencyMapPiece> pieces;
      std::uint64_t prefix = 0;
      for (std::uint32_t port = 0; port < inputs.size(); ++port) {
        auto dimensions = Region::whole(descriptor.shape).dimensions();
        dimensions[axis] = {prefix, inputs[port].descriptor.shape[axis]};
        DependencyMappedNeed data;
        data.port = port;
        data.roles = 1;
        for (std::size_t j = 0; j < descriptor.shape.size(); ++j)
          data.axes.push_back(
              {static_cast<std::int32_t>(j),
               {},
               j == axis ? -static_cast<std::int64_t>(prefix) : 0});
        auto validation = typed_map(data, inputs[port]);
        pieces.push_back(
            {taken(Footprint::from_regions(descriptor.shape,
                                           {Region(std::move(dimensions))})),
             {std::move(data), std::move(validation)}});
        prefix += inputs[port].descriptor.shape[axis];
      }
      result.static_dependency_pieces = std::move(pieces);
      return Answer(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    } catch (const IndexFailure& failure) {
      return Answer(failure.status);
    }
  };
  operation.start_dependency = [profile](const auto& query,
                                         const auto& allocator) {
    return DependencyContinuation::make<ConcatenateState>(allocator, profile,
                                                          query);
  };
  return operation;
}
}  // namespace
Status register_numeric_indexing(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(concatenate_operation(
        std::string("array.concatenate") + variant.first, variant.second));
    if (!status.ok())
      return status;
    for (const auto& item :
         {std::make_pair("gather", IndexKind::Gather),
          std::make_pair("scatter_replace", IndexKind::Replace),
          std::make_pair("scatter_sum", IndexKind::Sum),
          std::make_pair("scatter_minimum", IndexKind::Minimum),
          std::make_pair("scatter_maximum", IndexKind::Maximum)}) {
      status = registry->register_operation(
          index_operation(std::string("array.") + item.first + variant.first,
                          item.second, variant.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
