#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "data/input_validation.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::numeric_ops {
template <class Inputs>
Result<std::vector<OperationOutputSpecialization>> pointwise_specialization(
    const Inputs& inputs) {
  using Answer = Result<std::vector<OperationOutputSpecialization>>;
  OperationOutputSpecialization result;
  result.metadata.descriptor = inputs[0].descriptor;
  std::vector<DependencyMappedNeed> maps;
  for (std::uint32_t port = 0; port < inputs.size(); ++port) {
    if (inputs[port].descriptor.shape != inputs[0].descriptor.shape ||
        inputs[port].descriptor.element_type !=
            inputs[0].descriptor.element_type)
      return Answer(Status{ErrorCode::TypeMismatch,
                           "pointwise operands require matching shape/dtype"});
    DependencyMappedNeed data;
    data.port = port;
    data.roles = 1;
    for (std::size_t axis = 0; axis < inputs[0].descriptor.shape.size(); ++axis)
      data.axes.push_back({static_cast<std::int32_t>(axis), {}});
    auto validation = input_internal::validation_map(data, inputs[port]);
    maps.push_back(std::move(data));
    maps.push_back(std::move(validation));
  }
  auto all = Footprint::all(inputs[0].descriptor.shape);
  if (!all.ok())
    return Answer(all.status());
  result.static_dependency_pieces =
      std::vector<DependencyMapPiece>{{all.take_value(), std::move(maps)}};
  return Answer(std::vector<OperationOutputSpecialization>{std::move(result)});
}

template <class Evaluate>
Result<DependencyPoll> publish_numeric_points(const DependencyPhase& phase,
                                              Evaluate evaluate) {
  using Answer = Result<DependencyPoll>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto width = Value::element_size(descriptor.element_type);
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
        [&](const auto& at) {
          auto calculated = evaluate(at);
          if (!calculated.ok()) {
            auto failure = calculated.status();
            if (failure.detail.scope == FailureScope::Atom) {
              AtomKey atom;
              atom.output_index = phase.query.output_index;
              atom.rank = static_cast<std::uint32_t>(at.size());
              std::copy(at.begin(), at.end(), atom.coordinate.begin());
              failure.detail.atom = atom;
            }
            return failure;
          }
          auto bits = calculated.value();
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
    values.push_back(retained.take_value());
  }
  auto result = publication.finish(descriptor, phase.query.outputs,
                                   values.data(), values.size(), phase.sets);
  return result.ok() ? Answer(result.take_value()) : Answer(result.status());
}
// Visit each requested logical line once, including across disjoint boxes.
// Dense publication offsets remain row-major even when the line axis is not.
template <class Evaluate>
Result<DependencyPoll> publish_numeric_lines(const DependencyPhase& phase,
                                             std::size_t axis,
                                             Evaluate evaluate) {
  using Answer = Result<DependencyPoll>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& boxes = phase.query.outputs.boxes();
  const auto width = Value::element_size(descriptor.element_type);
  ArrayPublication publication(boxes.size(), descriptor.shape.size());
  dependency_internal::MetadataBytes temporary;
  temporary.add(4096);
  temporary.add(boxes.size(), sizeof(Region) + descriptor.shape.size() *
                                                   sizeof(RegionDimension));
  auto projection_owner = dependency_internal::metadata_owner(temporary.bytes);
  ResourceVector<MutableValue> writers;
  writers.reserve(boxes.size());
  std::vector<Region> projected;
  for (const auto& box : boxes) {
    auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    writers.push_back(allocated.take_value());
    auto dimensions = box.dimensions();
    dimensions[axis] = {0, 1};
    projected.emplace_back(std::move(dimensions));
  }
  auto lines = Footprint::from_regions(descriptor.shape, std::move(projected),
                                       phase.sets);
  if (!lines.ok())
    return Answer(lines.status());
  auto status = lines.value().visit(
      [&](const auto& line) {
        auto at = line;
        for (std::size_t b = 0; b < boxes.size(); ++b) {
          auto charged = phase.consume_work(at.size() + 1);
          if (!charged.ok())
            return charged;
          const auto& dimensions = boxes[b].dimensions();
          bool contains = true;
          for (std::size_t j = 0; j < at.size(); ++j)
            if (j != axis &&
                (at[j] < dimensions[j].offset ||
                 at[j] - dimensions[j].offset >= dimensions[j].extent))
              contains = false;
          if (!contains)
            continue;
          const auto end = dimensions[axis].offset + dimensions[axis].extent;
          for (at[axis] = dimensions[axis].offset; at[axis] < end; ++at[axis]) {
            auto calculated = evaluate(at);
            if (!calculated.ok()) {
              auto failure = calculated.status();
              if (failure.detail.scope == FailureScope::Atom) {
                AtomKey atom;
                atom.output_index = phase.query.output_index;
                atom.rank = static_cast<std::uint32_t>(at.size());
                std::copy(at.begin(), at.end(), atom.coordinate.begin());
                failure.detail.atom = atom;
              }
              return failure;
            }
            std::uint64_t offset = 0;
            for (std::size_t j = 0; j < at.size(); ++j)
              offset =
                  offset * dimensions[j].extent + at[j] - dimensions[j].offset;
            const auto bits = calculated.value();
            std::memcpy(writers[b].data() + offset * width, &bits, width);
          }
        }
        return Status::success();
      },
      phase.sets.maximum_work, phase.query.cancellation);
  if (!status.ok())
    return Answer(status);
  ResourceVector<Value> values;
  values.reserve(boxes.size());
  for (auto& writer : writers) {
    auto value = std::move(writer).publish();
    if (!value.ok())
      return Answer(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Answer(retained.status());
    values.push_back(retained.take_value());
  }
  auto result = publication.finish(descriptor, phase.query.outputs,
                                   values.data(), values.size(), phase.sets);
  return result.ok() ? Answer(result.take_value()) : Answer(result.status());
}
}  // namespace ps::plugin_internal::numeric_ops
