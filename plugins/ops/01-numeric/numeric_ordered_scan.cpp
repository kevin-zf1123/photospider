#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "01-numeric/ordered_result.hpp"
#include "common/registry.hpp"
#include "common/typed_need_validation.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "plugin/port_validation.hpp"

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
  std::uint64_t target = 0;
  std::size_t box = 0;
  std::optional<ResultBuilder> builder;
  explicit ScanState(std::uint64_t block) : block(block) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto checked_inputs = ops_validation::typed_inputs(phase);
    if (!checked_inputs.ok())
      return Result<ResultProgramPoll>(checked_inputs);
    if (phase.query.tensor_outputs && phase.query.tensor_outputs->empty())
      return numeric_ops::ordered_empty_output(phase);
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<ResultProgramPoll>(Status{
          ErrorCode::InvalidArgument, "numeric environment unavailable"});
    if (!started) {
      started = true;
      if (!phase.query.tensor_outputs || phase.query.tensor_outputs->empty())
        return Result<ResultProgramPoll>(
            Status{ErrorCode::InvalidArgument, "empty ordered scan demand"});
      target = phase.query.tensor_outputs->boxes()[0].dimensions()[0].offset;
      auto made = ResultBuilder::start(phase.resources,
                                       *phase.query.output.result_schema,
                                       phase.query.semantic_key);
      if (!made.ok())
        return Result<ResultProgramPoll>(made.status());
      builder = made.take_value();
      auto prior = phase.checkpoint_before(1, target);
      if (!prior.ok())
        return Result<ResultProgramPoll>(prior.status());
      if (prior.value()) {
        const auto& checkpoint = *prior.value();
        auto value = numeric_ops::ordered_read_state(checkpoint.state(), 0);
        if (!value.ok() || !std::isfinite(value.value()))
          return Result<ResultProgramPoll>(Status{
              ErrorCode::OperationFailed, "invalid ordered scan checkpoint"});
        carry = value.value();
        cursor = checkpoint.sequence() + 1;
      }
    }
    if (ready) {
      auto outgoing = advance(phase);
      if (!outgoing.ok())
        return Result<ResultProgramPoll>(outgoing.status());
      carry = numeric_ops::ordered_read_state(outgoing.value(), 0).value();
      cursor = end;
      ready = false;
      auto status = phase.checkpoint_publish(1, cursor - 1, outgoing.value());
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    while (cursor > target) {
      auto status = builder->publish_tensor(
          0, Region({{target, 1}}),
          {reinterpret_cast<const std::uint8_t*>(&carry), 8},
          phase.query.cancellation);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      const auto& boxes = phase.query.tensor_outputs->boxes();
      const auto& range = boxes[box].dimensions()[0];
      if (++target == range.offset + range.extent) {
        if (++box == boxes.size()) {
          auto sealed = builder->seal();
          if (!sealed.ok())
            return Result<ResultProgramPoll>(sealed.status());
          return Result<ResultProgramPoll>(
              ResultPublication{sealed.take_value(), true});
        }
        target = boxes[box].dimensions()[0].offset;
      }
    }

    end = cursor + std::min(block, target + 1 - cursor);
    auto samples = Footprint::from_regions(
        phase.query.inputs[0].result_schema->tensors[0].sample_shape(),
        {Region({{cursor, end - cursor}})}, numeric_ops::ordered_limits(phase));
    if (!samples.ok())
      return Result<ResultProgramPoll>(samples.status());
    ready = true;
    ResultProgramNeed need;
    {
      auto tile_need_0 = sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, samples.take_value());
      if (!tile_need_0.ok())
        return Result<ResultProgramPoll>(tile_need_0.status());
      need.tiles.push_back(tile_need_0.take_value());
    }
    return Result<ResultProgramPoll>(std::move(need));
  }
  Result<ResultRef> advance(const ResultProgramPhase& phase) const {
    const auto state = [&](double value) {
      return numeric_ops::ordered_state(phase, {value});
    };
    auto incoming = state(carry);
    if (!incoming.ok())
      return incoming;
    return phase.block(
        1, cursor, end, 1, incoming.value(), [&]() -> Result<ResultRef> {
          double outgoing = carry;
          for (auto i = cursor; i < end; ++i) {
            double value = 0;
            auto status = phase.consume_work(1);
            if (!status.ok())
              return Result<ResultRef>(status);
            status = phase.read_tensor(0, 0, {i}, &value, 8);
            if (!status.ok())
              return Result<ResultRef>(status);
            if (!std::isfinite(value))
              return Result<ResultRef>(
                  Status{ErrorCode::OperationFailed,
                         "nonfinite scan input " + std::to_string(i)});
            outgoing += value;
            if (!std::isfinite(outgoing))
              return Result<ResultRef>(
                  Status{ErrorCode::OperationFailed,
                         "scan overflow " + std::to_string(i)});
          }
          return state(outgoing);
        });
  }
};
}  // namespace
Status register_numeric_ordered_scan(OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "numeric.ordered_scan";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.input_schema[0].rank = 1;
  traits.requires_metadata_specialization = true;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(ScanState));
  traits.outputs[0].key = "value";
  traits.outputs[0].region_rule = OperationRegionRule::Tiled;
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.workspace_bytes = 16;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    const auto shape = inputs[0].result_schema->tensors[0].sample_shape();
    if (!inputs[0].result_schema->fields.empty() || shape.size() != 1)
      return Result<std::vector<OperationOutputSpecialization>>(
          Status{ErrorCode::TypeMismatch, "ordered scan requires rank one"});
    OperationOutputSpecialization output;
    output.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(ElementType::Float64, shape));
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  operation.start_result = [](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    const auto found = query.parameters.find("block_size");
    const auto block =
        found == query.parameters.end()
            ? 64U
            : static_cast<std::uint64_t>(std::get<std::int64_t>(found->second));
    return ResultContinuation::make<ScanState>(allocator, block);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
