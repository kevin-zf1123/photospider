#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/array_publication.hpp"
#include "data/dependency_metadata.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class LayoutKind { Reshape, Transpose, Slice };
struct LayoutState final {
  LayoutKind kind;
  SequenceProfile profile;
  unsigned stage = 0;
  std::array<std::uint64_t, 8> source_shape{}, target_shape{}, permutation{};
  std::array<std::int64_t, 8> starts{}, steps{};
  unsigned source_rank = 0, target_rank = 0;
  std::array<std::uint8_t, 32> block{};
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  LayoutState(LayoutKind operation, SequenceProfile selected,
              const DependencyQuery& query,
              const std::vector<std::uint64_t>& axes)
      : kind(operation),
        profile(selected),
        source_rank(query.inputs[0].descriptor.shape.size()),
        target_rank(query.output.descriptor.shape.size()) {
    std::copy(query.inputs[0].descriptor.shape.begin(),
              query.inputs[0].descriptor.shape.end(), source_shape.begin());
    std::copy(query.output.descriptor.shape.begin(),
              query.output.descriptor.shape.end(), target_shape.begin());
    std::copy(axes.begin(), axes.end(), permutation.begin());
  }
  std::vector<std::uint64_t> source(
      const std::vector<std::uint64_t>& output) const {
    std::vector<std::uint64_t> result(source_rank);
    if (kind == LayoutKind::Reshape) {
      std::uint64_t linear = 0;
      for (unsigned j = 0; j < target_rank; ++j)
        linear = linear * target_shape[j] + output[j];
      for (unsigned j = source_rank; j; --j) {
        result[j - 1] = linear % source_shape[j - 1];
        linear /= source_shape[j - 1];
      }
    } else if (kind == LayoutKind::Transpose) {
      for (unsigned j = 0; j < target_rank; ++j)
        result[permutation[j]] = output[j];
    } else {
      for (unsigned j = 0; j < source_rank; ++j)
        result[j] = static_cast<std::uint64_t>(
            static_cast<__int128>(starts[j]) +
            static_cast<__int128>(output[j]) * steps[j]);
    }
    return result;
  }
  Result<Footprint> point(const DependencyPhase& phase,
                          const std::vector<std::uint64_t>& coordinate,
                          bool validation) const {
    std::vector<RegionDimension> dimensions;
    dimensions.reserve(source_rank);
    for (auto value : coordinate)
      dimensions.push_back({value, 1});
    if (validation) {
      for (const auto& facet : phase.query.inputs[0].facets)
        if (facet.key == "photospider.image" ||
            facet.key == "photospider.semantic") {
          auto semantic = decode_semantic(facet);
          if (!semantic.ok())
            return Result<Footprint>(semantic.status());
          if (semantic.value().kind == SemanticKind::Image)
            dimensions[2] = {0, source_shape[2]};
        }
    }
    return Footprint::from_regions(phase.query.inputs[0].descriptor.shape,
                                   {Region(std::move(dimensions))}, phase.sets);
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, bool controls) {
    using Answer = Result<DependencyPoll>;
    const auto count = phase.query.observations.element_count().value();
    if (count > phase.sets.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "layout association capacity",
                           FailureReason::CapacityLimit});
    // Admit bounded boundary-vector construction and simultaneous geometric
    // temporaries before growing them. The resulting batch owns its own ledger.
    const auto per_row =
        sizeof(AtomCertificate) + target_rank * 8 +
        4 * (sizeof(DependencyNeed) + 8 * sizeof(Region) +
             64 * sizeof(RegionDimension) + 8 * sizeof(std::uint64_t));
    if (count > (UINT64_MAX - 4096) / per_row)
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "layout metadata overflow",
                           FailureReason::CapacityLimit});
    request_capacity =
        dependency_internal::metadata_owner(4096 + count * per_row);
    std::vector<DependencyNeed> control_needs;
    if (controls) {
      auto all = Footprint::all({source_rank}, phase.sets);
      if (!all.ok())
        return Answer(all.status());
      control_needs.push_back({1, 6, all.take_value(), {}});
      std::vector<Region> used;
      for (unsigned j = 0; j < source_rank; ++j)
        if (target_shape[j] > 1)
          used.push_back(Region({{j, 1}}));
      if (!used.empty()) {
        auto wanted = Footprint::from_regions({source_rank}, used, phase.sets);
        if (!wanted.ok())
          return Answer(wanted.status());
        control_needs.push_back({2, 6, wanted.take_value(), {}});
      }
    }
    std::vector<AtomCertificate> rows;
    rows.reserve(count);
    auto status = phase.query.observations.visit(
        [&](const auto& coordinate) {
          auto work = phase.consume_work(1 + source_rank + target_rank);
          if (!work.ok())
            return work;
          AtomCertificate row{coordinate, {}};
          if (controls) {
            row.inputs = control_needs;
          } else {
            const auto input = source(coordinate);
            auto data = point(phase, input, false);
            if (!data.ok())
              return data.status();
            auto validation = point(phase, input, true);
            if (!validation.ok())
              return validation.status();
            row.inputs.reserve(2);
            row.inputs.push_back({0, 1, data.take_value(), {}});
            row.inputs.push_back({0, 4, validation.take_value(), {}});
          }
          rows.push_back(std::move(row));
          return Status::success();
        },
        phase.sets.maximum_boxes, phase.query.cancellation);
    if (!status.ok())
      return Answer(status);
    return Answer(DependencyNeedBatch{std::move(rows)});
  }
  Status controls(const DependencyPhase& phase) {
    for (unsigned j = 0; j < source_rank; ++j) {
      auto status = phase.consume_work(8);
      if (!status.ok())
        return status;
      status = phase.read(1, {j}, &starts[j], 8);
      if (!status.ok())
        return status;
      if (target_shape[j] > 1) {
        status = phase.read(2, {j}, &steps[j], 8);
        if (!status.ok())
          return status;
      }
      const auto last = static_cast<__int128>(starts[j]) +
                        static_cast<__int128>(target_shape[j] - 1) * steps[j];
      if (starts[j] < 0 ||
          static_cast<std::uint64_t>(starts[j]) >= source_shape[j] ||
          (target_shape[j] > 1 && !steps[j]) || last < 0 ||
          last >= source_shape[j])
        return Status{ErrorCode::InvalidArgument,
                      "InvalidSlice: axis=" + std::to_string(j) +
                          " start=" + std::to_string(starts[j]) +
                          " step=" + std::to_string(steps[j]),
                      FailureReason::InvalidDomain,
                      {FailureOrigin::Domain, FailureScope::Unspecified}};
    }
    return Status::success();
  }
  struct Address {
    std::shared_ptr<const CpuStorage> owner;
    std::uint64_t offset = 0;
    std::array<std::int64_t, 8> strides{};
  };
  Result<Address> address(const DependencyPhase& phase,
                          const std::vector<std::uint64_t>& output) const {
    const auto coordinate = source(output);
    for (const auto& fragment : phase.inputs[0].fragments()) {
      auto work = phase.consume_work(source_rank + 1);
      if (!work.ok())
        return Result<Address>(work);
      bool contains = true;
      for (unsigned j = 0; j < source_rank; ++j) {
        const auto axis = fragment.region().dimensions()[j];
        contains &= coordinate[j] >= axis.offset &&
                    coordinate[j] - axis.offset < axis.extent;
      }
      if (!contains)
        continue;
      auto offset = fragment.byte_address(coordinate);
      if (!offset.ok())
        return Result<Address>(offset.status());
      Address result{fragment.storage(), offset.value(), {}};
      std::copy(fragment.layout().byte_strides.begin(),
                fragment.layout().byte_strides.end(), result.strides.begin());
      return Result<Address>(std::move(result));
    }
    return Result<Address>(
        Status{ErrorCode::OperationFailed, "layout source coverage missing"});
  }
  Result<std::optional<Value>> view(const DependencyPhase& phase,
                                    const Region& box) const {
    using Answer = Result<std::optional<Value>>;
    std::vector<std::uint64_t> origin;
    for (const auto& dimension : box.dimensions())
      origin.push_back(dimension.offset);
    if (kind == LayoutKind::Transpose) {
      const auto first = source(origin);
      for (const auto& fragment : phase.inputs[0].fragments()) {
        auto status = phase.consume_work(source_rank + target_rank);
        if (!status.ok())
          return Answer(status);
        bool covered = true;
        std::vector<std::int64_t> strides(target_rank);
        for (unsigned j = 0; j < target_rank; ++j) {
          const auto source_axis =
              fragment.region().dimensions()[permutation[j]];
          const auto wanted = box.dimensions()[j];
          covered &=
              wanted.offset >= source_axis.offset &&
              wanted.offset - source_axis.offset <= source_axis.extent &&
              wanted.extent <= source_axis.extent -
                                   std::min(source_axis.extent,
                                            wanted.offset - source_axis.offset);
          strides[j] = fragment.layout().byte_strides[permutation[j]];
        }
        if (!covered)
          continue;
        auto offset = fragment.byte_address(first);
        if (!offset.ok())
          return Answer(offset.status());
        auto candidate = Value::from_storage(
            phase.query.output.descriptor, box,
            {offset.value(), std::move(strides), origin}, fragment.storage());
        return candidate.ok()
                   ? Answer(std::optional<Value>{candidate.take_value()})
                   : Answer(candidate.status());
      }
    }
    auto base = address(phase, origin);
    if (!base.ok())
      return Answer(base.status());
    std::vector<std::int64_t> strides(target_rank, 0);
    for (unsigned j = 0; j < target_rank; ++j) {
      if (box.dimensions()[j].extent == 1) {
        __int128 stride = 0;
        if (kind == LayoutKind::Slice && target_shape[j] > 1)
          stride = static_cast<__int128>(base.value().strides[j]) * steps[j];
        else if (kind == LayoutKind::Transpose)
          stride = base.value().strides[permutation[j]];
        if (stride < INT64_MIN || stride > INT64_MAX)
          return Answer(std::optional<Value>{});
        strides[j] = static_cast<std::int64_t>(stride);
        continue;
      }
      auto next = origin;
      ++next[j];
      auto location = address(phase, next);
      if (!location.ok())
        return Answer(location.status());
      const auto stride =
          static_cast<__int128>(location.value().offset) - base.value().offset;
      if (location.value().owner != base.value().owner || stride < INT64_MIN ||
          stride > INT64_MAX)
        return Answer(std::optional<Value>{});
      strides[j] = static_cast<std::int64_t>(stride);
    }
    auto selected = Footprint::from_regions(phase.query.output.descriptor.shape,
                                            {box}, phase.sets);
    if (!selected.ok())
      return Answer(selected.status());
    bool affine = true;
    auto status = selected.value().visit(
        [&](const auto& coordinate) {
          if (!affine)
            return Status::success();
          auto actual = address(phase, coordinate);
          if (!actual.ok())
            return actual.status();
          __int128 expected = base.value().offset;
          for (unsigned j = 0; j < target_rank; ++j)
            expected +=
                static_cast<__int128>(coordinate[j] - origin[j]) * strides[j];
          affine = actual.value().owner == base.value().owner &&
                   expected >= 0 && expected == actual.value().offset;
          return affine ? Status::success()
                        : Status{ErrorCode::InvalidArgument,
                                 "non-affine view proof"};
        },
        phase.sets.maximum_work, phase.query.cancellation);
    if (!affine)
      return Answer(std::optional<Value>{});
    if (!status.ok())
      return Answer(status);
    auto result = Value::from_storage(phase.query.output.descriptor, box,
                                      {base.value().offset, strides, origin},
                                      base.value().owner);
    if (!result.ok())
      return Answer(result.status());
    return Answer(std::optional<Value>{result.take_value()});
  }
  Status report(const DependencyPhase& phase, std::uint64_t count,
                bool view) const {
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    // A stable attempt identity allows different rectangles to choose a view
    // or copy; the two counters report each actual representation separately.
    const auto* identity = numeric_ops::array_implementation(profile, false);
    std::memcpy(diagnostics.implementation.data(), identity,
                std::strlen(identity) + 1);
    diagnostics.evaluated_values = count;
    diagnostics.view_elements = view ? count : 0;
    diagnostics.copied_elements = view ? 0 : count;
    return phase.report_numeric(diagnostics);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!stage) {
      ++stage;
      if (kind == LayoutKind::Transpose)
        return Answer(DependencyNeedBatch{{}, {}, true});
      return need(phase, kind == LayoutKind::Slice);
    }
    if (kind == LayoutKind::Slice && stage == 1) {
      auto status = controls(phase);
      if (!status.ok())
        return Answer(status);
      ++stage;
      return need(phase, false);
    }
    request_capacity.reset();
    const auto& layout =
        std::get<std::string>(phase.query.parameters.at("layout"));
    const auto& descriptor = phase.query.output.descriptor;
    const auto width = Value::element_size(descriptor.element_type);
    const auto boxes = phase.query.outputs.boxes().size();
    numeric_ops::ArrayPublication publication(boxes, target_rank);
    ResourceVector<Value> fragments;
    for (const auto& box : phase.query.outputs.boxes()) {
      if (layout != "dense") {
        auto candidate = view(phase, box);
        if (!candidate.ok())
          return Answer(candidate.status());
        if (candidate.value()) {
          auto status = report(phase, box.element_count().value(), true);
          if (!status.ok())
            return Answer(status);
          auto retained = publication.retain(std::move(*candidate.value()));
          if (!retained.ok())
            return Answer(retained.status());
          fragments.push_back(retained.take_value());
          continue;
        }
        if (layout == "view")
          return Answer(Status{
              ErrorCode::InvalidArgument,
              "ViewUnavailable: requested rectangle is not one affine owner",
              FailureReason::InvalidDomain,
              {FailureOrigin::Domain, FailureScope::Unspecified}});
      }
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto output = allocated.take_value();
      auto selected =
          Footprint::from_regions(descriptor.shape, {box}, phase.sets);
      if (!selected.ok())
        return Answer(selected.status());
      std::uint64_t offset = 0;
      std::size_t buffered = 0;
      auto status = selected.value().visit(
          [&](const auto& coordinate) {
            auto work = phase.consume_work(source_rank + target_rank + width +
                                           phase.inputs[0].fragments().size());
            if (!work.ok())
              return work;
            auto read = phase.read(0, source(coordinate),
                                   block.data() + buffered, width);
            if (!read.ok())
              return read;
            buffered += width;
            if (buffered == block.size()) {
              numeric_ops::array_copy_block(output.data() + offset,
                                            block.data(), buffered, profile);
              offset += buffered;
              buffered = 0;
            }
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      if (buffered)
        numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                      buffered, profile);
      status = report(phase, box.element_count().value(), false);
      if (!status.ok())
        return Answer(status);
      auto published = std::move(output).publish();
      if (!published.ok())
        return Answer(published.status());
      auto retained = publication.retain(published.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      fragments.push_back(retained.take_value());
    }
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result =
        publication.finish(descriptor, phase.query.outputs, fragments.data(),
                           fragments.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition layout_operation(const std::string& key, LayoutKind kind,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  // Physical viewability depends on source owner/stride partitioning. Current
  // disposable content caches cannot witness that partition, so do not reuse
  // these results across executions. Pure active-Run sharing remains valid.
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == LayoutKind::Slice ? 3 : 1;
  traits.input_schema.resize(traits.input_count);
  for (unsigned port = 1; port < traits.input_count; ++port) {
    traits.input_schema[port].rank = 1;
    traits.input_schema[port].element_type =
        static_cast<std::uint32_t>(ElementType::Int64);
  }
  const std::string parameter = kind == LayoutKind::Reshape     ? "shape"
                                : kind == LayoutKind::Transpose ? "permutation"
                                                                : "counts";
  traits.parameter_schema = {{parameter, OperationParameterType::String},
                             {"layout", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(LayoutState);
  output.maximum_dependency_stages = kind == LayoutKind::Slice ? 3 : 2;
  operation.specialize_metadata =
      [kind, profile, parameter](const auto& inputs, const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    const auto& input = inputs[0].descriptor;
    std::uint64_t source_count = 1;
    for (auto size : input.shape) {
      if (!size || size > (UINT64_C(1) << 40) / source_count)
        return mismatch("layout source exceeds 2^40 elements");
      source_count *= size;
    }
    auto parameters_list = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at(parameter)),
        kind != LayoutKind::Transpose);
    if (!parameters_list.ok())
      return Answer(parameters_list.status());
    auto shape = parameters_list.value();
    if (kind == LayoutKind::Transpose) {
      if (shape.size() != input.shape.size())
        return mismatch("transpose permutation rank differs from input");
      unsigned used = 0;
      for (std::size_t j = 0; j < shape.size(); ++j) {
        const auto axis = parameters_list.value()[j];
        if (axis >= shape.size() || (used & (1U << axis)))
          return Answer(numeric_ops::array_parameter_error(
              "transpose axes must form a permutation"));
        used |= 1U << axis;
        shape[j] = input.shape[axis];
      }
    } else if (kind == LayoutKind::Reshape) {
      std::uint64_t count = 1;
      for (auto size : shape)
        count *= size;
      if (count != source_count)
        return Answer(numeric_ops::array_parameter_error(
            "reshape element products must match"));
    } else {
      if (shape.size() != input.shape.size())
        return mismatch("slice counts rank differs from input");
      for (unsigned port = 1; port < 3; ++port)
        if (inputs[port].descriptor.shape !=
            std::vector<std::uint64_t>{shape.size()})
          return mismatch("slice controls must be Int64[input rank]");
    }
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "auto" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "layout must be auto, view or dense"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {input.element_type, std::move(shape)};
    result.regional_atomic = kind != LayoutKind::Transpose;
    result.preserve_output_views = layout != "dense";
    if (layout == "view")
      result.maximum_output_payload_bytes = 0;
    if (kind == LayoutKind::Transpose) {
      DependencyMappedNeed data;
      data.roles = 1;
      data.axes.resize(input.shape.size());
      for (std::size_t j = 0; j < parameters_list.value().size(); ++j)
        data.axes[parameters_list.value()[j]] = {static_cast<std::int32_t>(j),
                                                 {}};
      auto validation = data;
      validation.roles = 4;
      for (const auto& facet : inputs[0].facets)
        if (facet.key == "photospider.image" ||
            facet.key == "photospider.semantic") {
          auto semantic = decode_semantic(facet);
          if (!semantic.ok())
            return Answer(semantic.status());
          if (semantic.value().kind == SemanticKind::Image)
            validation.axes[2] = {-1, {0, input.shape[2]}};
        }
      result.static_dependency_pieces = std::vector<DependencyMapPiece>{
          {Footprint::all(result.metadata.descriptor.shape).take_value(),
           {std::move(data), std::move(validation)}}};
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [kind, profile, parameter](
                                   const auto& query, const auto& allocator) {
    std::vector<std::uint64_t> axes;
    if (kind == LayoutKind::Transpose) {
      auto parsed = numeric_ops::parse_array_list(
          std::get<std::string>(query.parameters.at(parameter)), false);
      if (!parsed.ok())
        return Result<DependencyContinuation>(parsed.status());
      axes = parsed.take_value();
    }
    return DependencyContinuation::make<LayoutState>(allocator, kind, profile,
                                                     query, axes);
  };
  return operation;
}
}  // namespace
Status register_numeric_layouts(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& item : {std::make_pair("reshape", LayoutKind::Reshape),
                             std::make_pair("transpose", LayoutKind::Transpose),
                             std::make_pair("slice", LayoutKind::Slice)}) {
      auto status = registry->register_operation(
          layout_operation(std::string("array.") + item.first + variant.first,
                           item.second, variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
