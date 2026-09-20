#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "data/input_validation.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Status array_report(const DependencyPhase& phase, SequenceProfile profile,
                    std::uint64_t values) {
  NumericDiagnostics report;
  report.profile =
      static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
  const bool view =
      std::get<std::string>(phase.query.parameters.at("layout")) == "view";
  const auto* identity = numeric_ops::array_implementation(profile, view);
  std::memcpy(report.implementation.data(), identity,
              std::strlen(identity) + 1);
  report.evaluated_values = values;
  return phase.report_numeric(report);
}
struct ConstantState final {
  SequenceProfile profile;
  bool ready = false;
  std::array<std::uint8_t, 32> block{};
  explicit ConstantState(SequenceProfile selected) : profile(selected) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const bool view =
        std::get<std::string>(phase.query.parameters.at("layout")) == "view";
    if (!ready) {
      ready = true;
      if (!view)
        return Result<DependencyPoll>(DependencyNeedBatch{{}, {}, true});
      auto input = Footprint::all({1}, phase.sets);
      if (!input.ok())
        return Result<DependencyPoll>(input.status());
      return multi_output::need(phase, {{0, 5, input.take_value(), {}}});
    }
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    auto work =
        phase.consume_work(width + phase.query.output.descriptor.shape.size());
    if (!work.ok())
      return Result<DependencyPoll>(work);
    if (!view) {
      auto read = phase.read(0, {0}, block.data(), width);
      if (!read.ok())
        return Result<DependencyPoll>(read);
      for (std::size_t i = width; i < block.size(); i += width)
        std::memcpy(block.data() + i, block.data(), width);
      ResourceVector<Value> fragments;
      for (const auto& region : phase.query.outputs.boxes()) {
        auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                                region, phase.allocator);
        if (!allocated.ok())
          return Result<DependencyPoll>(allocated.status());
        auto output = allocated.take_value();
        for (std::uint64_t offset = 0; offset < output.size();) {
          const auto count =
              std::min<std::uint64_t>(32, output.size() - offset);
          auto status = phase.consume_work(count);
          if (!status.ok())
            return Result<DependencyPoll>(status);
          numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                        count, profile);
          status = array_report(phase, profile, count / width);
          if (!status.ok())
            return Result<DependencyPoll>(status);
          offset += count;
        }
        auto published = std::move(output).publish();
        if (!published.ok())
          return Result<DependencyPoll>(published.status());
        fragments.push_back(published.take_value());
      }
      auto result = ValueFragments::create_view(
          phase.query.output.descriptor, {}, phase.query.outputs,
          fragments.data(), fragments.size(), phase.sets);
      return result.ok() ? Result<DependencyPoll>(result.take_value())
                         : Result<DependencyPoll>(result.status());
    }
    auto buffer = phase.allocator.allocate(width);
    if (!buffer.ok())
      return Result<DependencyPoll>(buffer.status());
    auto bytes = buffer.take_value();
    auto read = phase.read(0, {0}, bytes.data(), width);
    if (!read.ok())
      return Result<DependencyPoll>(read);
    auto reported = array_report(phase, profile, 1);
    if (!reported.ok())
      return Result<DependencyPoll>(reported);
    if (phase.query.cancellation.cancelled())
      return Result<DependencyPoll>(Status{ErrorCode::Cancelled, {}});
    auto region = phase.query.outputs.boxes()[0];
    std::vector<std::uint64_t> origin;
    for (const auto& dimension : region.dimensions())
      origin.push_back(dimension.offset);
    auto output = Value::from_storage(
        phase.query.output.descriptor, region,
        StridedLayout{
            0, std::vector<std::int64_t>(origin.size(), view ? 0 : width),
            origin},
        std::move(bytes).freeze());
    if (!output.ok())
      return Result<DependencyPoll>(output.status());
    return multi_output::finish(phase, output.take_value());
  }
};
OperationDefinition constant(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].rank = 1;
  traits.parameter_schema = {{"layout", OperationParameterType::String},
                             {"shape", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ConstantState);
  output.maximum_dependency_stages = 2;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    if (inputs[0].descriptor.shape != std::vector<std::uint64_t>{1})
      return Answer(Status{ErrorCode::TypeMismatch,
                           "constant requires one scalar",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    auto shape = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("shape")), true);
    if (!shape.ok())
      return Answer(shape.status());
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "constant layout must be view or dense"));
    auto supported = numeric_ops::sequence_profile_available(profile);
    if (!supported.ok())
      return Answer(supported);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {inputs[0].descriptor.element_type,
                                  shape.take_value()};
    if (layout == "view") {
      result.metadata.atomic_trailing_axes =
          result.metadata.descriptor.shape.size();
      result.maximum_output_payload_bytes =
          Value::element_size(inputs[0].descriptor.element_type);
    } else {
      result.static_dependency_pieces = std::vector<DependencyMapPiece>{
          {Footprint::all(result.metadata.descriptor.shape).take_value(),
           {{0, 5, {{-1, {0, 1}}}, {}}}}};
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [profile](const auto&, const auto& allocator) {
    return DependencyContinuation::make<ConstantState>(allocator, profile);
  };
  return operation;
}
struct BroadcastState final {
  SequenceProfile profile;
  std::array<std::uint32_t, 8> axes{};
  std::uint32_t rank = 0;
  bool requested = false;
  std::array<std::uint8_t, 32> block{};
  BroadcastState(SequenceProfile selected,
                 const std::vector<std::uint64_t>& map)
      : profile(selected), rank(static_cast<std::uint32_t>(map.size())) {
    for (std::size_t i = 0; i < map.size(); ++i)
      axes[i] = static_cast<std::uint32_t>(map[i]);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!requested) {
      requested = true;
      return Answer(DependencyNeedBatch{{}, {}, true});
    }
    const auto& source_shape = phase.query.inputs[0].descriptor.shape;
    const auto& descriptor = phase.query.output.descriptor;
    const auto width = Value::element_size(descriptor.element_type);
    const bool view =
        std::get<std::string>(phase.query.parameters.at("layout")) == "view";
    ResourceVector<Value> fragments;
    auto reported = array_report(phase, profile, 0);
    if (!reported.ok())
      return Answer(reported);
    for (const auto& box : phase.query.outputs.boxes()) {
      if (view) {
        for (const auto& input : phase.inputs[0].fragments()) {
          auto work = phase.consume_work(1 + rank + descriptor.shape.size());
          if (!work.ok())
            return Answer(work);
          auto dimensions = box.dimensions();
          bool hit = true;
          for (std::size_t j = 0; j < rank; ++j) {
            const auto source = input.region().dimensions()[j];
            if (source_shape[j] == 1) {
              hit &= source.offset == 0 && source.extent != 0;
            } else {
              auto& target = dimensions[axes[j]];
              const auto begin = std::max(target.offset, source.offset);
              const auto end = std::min(target.offset + target.extent,
                                        source.offset + source.extent);
              if (begin >= end) {
                hit = false;
                break;
              }
              target = {begin, end - begin};
            }
          }
          if (!hit)
            continue;
          if (fragments.size() >= phase.sets.maximum_boxes)
            return Answer(Status{ErrorCode::ResourceExhausted,
                                 "broadcast fragment limit",
                                 FailureReason::CapacityLimit});
          std::vector<std::uint64_t> origin, coordinate(rank);
          std::vector<std::int64_t> strides(descriptor.shape.size(), 0);
          for (const auto& dimension : dimensions)
            origin.push_back(dimension.offset);
          for (std::size_t j = 0; j < rank; ++j) {
            coordinate[j] = source_shape[j] == 1 ? 0 : origin[axes[j]];
            if (source_shape[j] != 1)
              strides[axes[j]] = input.layout().byte_strides[j];
          }
          auto address = input.byte_address(coordinate);
          if (!address.ok())
            return Answer(address.status());
          auto output = Value::from_storage(
              descriptor, Region(std::move(dimensions)),
              {address.value(), std::move(strides), std::move(origin)},
              input.storage());
          if (!output.ok())
            return Answer(output.status());
          fragments.push_back(output.take_value());
        }
      } else {
        auto allocation =
            MutableValue::allocate(descriptor, box, phase.allocator);
        if (!allocation.ok())
          return Answer(allocation.status());
        auto output = allocation.take_value();
        auto selected =
            Footprint::from_regions(descriptor.shape, {box}, phase.sets);
        if (!selected.ok())
          return Answer(selected.status());
        std::vector<std::uint64_t> source(rank);
        std::uint64_t offset = 0;
        std::size_t buffered = 0;
        auto copied = selected.value().visit(
            [&](const auto& coordinate) {
              auto work = phase.consume_work(rank + width);
              if (!work.ok())
                return work;
              for (std::size_t j = 0; j < rank; ++j)
                source[j] = source_shape[j] == 1 ? 0 : coordinate[axes[j]];
              std::uint64_t bits = 0;
              auto read = phase.read(0, source, &bits, width);
              if (!read.ok())
                return read;
              auto report = array_report(phase, profile, 1);
              if (!report.ok())
                return report;
              std::memcpy(block.data() + buffered, &bits, width);
              buffered += width;
              if (buffered == block.size()) {
                numeric_ops::array_copy_block(output.data() + offset,
                                              block.data(), buffered, profile);
                offset += buffered;
                buffered = 0;
              }
              return Status::success();
            },
            selected.value().element_count().value(), phase.query.cancellation);
        if (!copied.ok())
          return Answer(copied);
        if (buffered)
          numeric_ops::array_copy_block(output.data() + offset, block.data(),
                                        buffered, profile);
        auto published = std::move(output).publish();
        if (!published.ok())
          return Answer(published.status());
        fragments.push_back(published.take_value());
      }
    }
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result = ValueFragments::create_view(
        descriptor, {}, phase.query.outputs, fragments.data(), fragments.size(),
        phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition broadcast(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.parameter_schema = {{"axis_map", OperationParameterType::String},
                             {"layout", OperationParameterType::String},
                             {"shape", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(BroadcastState);
  output.maximum_dependency_stages = 2;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    auto shape = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("shape")), true);
    if (!shape.ok())
      return Answer(shape.status());
    auto axes = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("axis_map")), false);
    if (!axes.ok())
      return Answer(axes.status());
    const auto& source = inputs[0].descriptor;
    if (axes.value().size() != source.shape.size() ||
        shape.value().size() < source.shape.size())
      return mismatch(
          "broadcast must map every source axis without rank reduction");
    std::uint32_t used = 0;
    DependencyMappedNeed data;
    data.roles = 1;
    for (std::size_t j = 0; j < source.shape.size(); ++j) {
      const auto axis = axes.value()[j];
      if (axis >= shape.value().size() || (used & (1U << axis)))
        return Answer(numeric_ops::array_parameter_error(
            "broadcast axis map must be injective and in range"));
      used |= 1U << axis;
      if (source.shape[j] != 1 && source.shape[j] != shape.value()[axis])
        return mismatch("broadcast mapped extent must match or be one");
      data.axes.push_back(
          source.shape[j] == 1
              ? DependencyAxis{-1, {0, 1}}
              : DependencyAxis{static_cast<std::int32_t>(axis), {0, 0}});
    }
    auto validation = input_internal::validation_map(data, inputs[0]);
    const auto& layout = std::get<std::string>(parameters.at("layout"));
    if (layout != "view" && layout != "dense")
      return Answer(numeric_ops::array_parameter_error(
          "broadcast layout must be view or dense"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {source.element_type, shape.take_value()};
    result.static_dependency_pieces = std::vector<DependencyMapPiece>{
        {Footprint::all(result.metadata.descriptor.shape).take_value(),
         {std::move(data), std::move(validation)}}};
    if (layout == "view")
      result.maximum_output_payload_bytes = 0;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [profile](const auto& query,
                                         const auto& allocator) {
    auto axes = numeric_ops::parse_array_list(
        std::get<std::string>(query.parameters.at("axis_map")), false);
    if (!axes.ok())
      return Result<DependencyContinuation>(axes.status());
    return DependencyContinuation::make<BroadcastState>(allocator, profile,
                                                        axes.value());
  };
  return operation;
}
}  // namespace
Status register_numeric_arrays(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(constant(
        std::string("numeric.constant") + variant.first, variant.second));
    if (!status.ok())
      return status;
    status = registry->register_operation(broadcast(
        std::string("numeric.broadcast") + variant.first, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
