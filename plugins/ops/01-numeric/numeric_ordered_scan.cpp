#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
/** @brief One requested scan output; only successful prefix checkpoints share.
 * @note A checkpoint's sequence is its last processed logical index; the
 * algorithm identity fixes positive-zero start, binary64 nearest/gradual mode
 * and strict left-fold arithmetic. A final block never reads beyond this atom.
 */
struct ScanState final {
  std::uint64_t block = 64, cursor = 0, end = 0;
  double carry = 0;
  bool started = false, ready = false;
  explicit ScanState(std::uint64_t block) : block(block) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<DependencyPoll>(Status{ErrorCode::InvalidArgument,
                                           "numeric environment unavailable"});
    const auto target = phase.query.outputs.boxes()[0].dimensions()[0].offset;
    if (!started) {
      started = true;
      auto prior = phase.checkpoint_before(1, target);
      if (!prior.ok())
        return Result<DependencyPoll>(prior.status());
      if (prior.value()) {
        const auto& checkpoint = *prior.value();
        auto value = checkpoint.state().as_float64();
        if (!value.ok() || !std::isfinite(value.value()))
          return Result<DependencyPoll>(Status{
              ErrorCode::OperationFailed, "invalid ordered scan checkpoint"});
        carry = value.value();
        cursor = checkpoint.sequence() + 1;
      }
    }
    if (ready) {
      for (auto i = cursor; i < end; ++i) {
        double value = 0;
        auto status = phase.read(0, {i}, &value, 8);
        if (!status.ok())
          return Result<DependencyPoll>(status);
        if (!std::isfinite(value))
          return Result<DependencyPoll>(
              Status{ErrorCode::OperationFailed,
                     "nonfinite scan input " + std::to_string(i)});
        carry += value;
        if (!std::isfinite(carry))
          return Result<DependencyPoll>(
              Status{ErrorCode::OperationFailed,
                     "scan overflow " + std::to_string(i)});
      }
      cursor = end;
      auto made = MutableValue::allocate({ElementType::Float64, {1}},
                                         Region::whole({1}), phase.allocator);
      if (!made.ok())
        return Result<DependencyPoll>(made.status());
      auto writer = made.take_value();
      std::memcpy(writer.data(), &carry, 8);
      auto state = std::move(writer).publish();
      if (!state.ok())
        return Result<DependencyPoll>(state.status());
      auto status = phase.checkpoint_publish(1, cursor - 1, state.value());
      if (!status.ok())
        return Result<DependencyPoll>(status);
    }
    if (cursor > target) {
      auto made = MutableValue::allocate(phase.query.output.descriptor,
                                         phase.query.outputs.boxes()[0],
                                         phase.allocator);
      if (!made.ok())
        return Result<DependencyPoll>(made.status());
      auto writer = made.take_value();
      std::memcpy(writer.data(), &carry, 8);
      auto value = std::move(writer).publish();
      if (!value.ok())
        return Result<DependencyPoll>(value.status());
      auto output = ValueFragments::create(phase.query.output.descriptor, {},
                                           phase.query.outputs,
                                           {value.take_value()}, phase.sets);
      if (!output.ok())
        return Result<DependencyPoll>(output.status());
      return Result<DependencyPoll>(output.take_value());
    }
    end = cursor + std::min(block, target + 1 - cursor);
    auto samples =
        Footprint::from_regions(phase.query.inputs[0].descriptor.shape,
                                {Region({{cursor, end - cursor}})}, phase.sets);
    if (!samples.ok())
      return Result<DependencyPoll>(samples.status());
    ready = true;
    return Result<DependencyPoll>(
        DependencyNeedBatch{{{{target}, {{0, 5, samples.take_value(), {}}}}},
                            {}});
  }
};
}  // namespace
Status register_numeric_ordered_scan(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "numeric.ordered_scan";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.input_schema[0].rank = 1;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Dependency;
  traits.dependency_version = 1;
  traits.continuation_bytes = sizeof(ScanState);
  traits.workspace_bytes = 8;
  traits.maximum_dependency_stages = 1048576;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.start_dependency = [](const DependencyQuery& query,
                                  const BufferAllocator& allocator) {
    const auto found = query.parameters.find("block_size");
    const auto block =
        found == query.parameters.end()
            ? 64U
            : static_cast<std::uint64_t>(std::get<std::int64_t>(found->second));
    return DependencyContinuation::make<ScanState>(allocator, block);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
