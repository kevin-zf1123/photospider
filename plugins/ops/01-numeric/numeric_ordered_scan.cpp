#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "01-numeric/ordered_result.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "photospider/plugin/whole_stage.hpp"

namespace ps::plugin_internal {
namespace {
/** @brief One Run-shared inclusive scan over complete authorized input.
 * The compiler binds complete input; the numerical pass runs once and writes
 * each prefix into host windows. block bounds each cancellation/work
 * checkpoint. Optional completed-result reuse remains a host concern.
 */
struct ScanState final {
  std::uint64_t block;
  explicit ScanState(std::uint64_t block) : block(block) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto acquired = phase.tensors->at({0, 0}).acquire(
        Region::whole(
            phase.query.inputs[0].result_schema->tensors[0].sample_shape()),
        phase.query.cancellation);
    if (!acquired.ok())
      return Result<ResultProgramPoll>(acquired.status());
    const auto& window = acquired.value();
    FloatingEnvironment environment;
    if (!environment.active())
      return Result<ResultProgramPoll>(Status{
          ErrorCode::InvalidArgument, "numeric environment unavailable"});
    auto made = ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent);
    if (!made.ok())
      return Result<ResultProgramPoll>(made.status());
    auto builder = made.take_value();
    const auto region = Region::whole(
        phase.query.output.result_schema->tensors[0].sample_shape());
    double carry = 0;
    auto status = builder.publish_tensor_kernel(
        0, region,
        [&](const auto& outputs) {
          for (const auto& output : outputs) {
            const auto d = output.region().dimensions()[0];
            for (auto cursor = d.offset; cursor < d.offset + d.extent;) {
              if (phase.query.cancellation.cancelled())
                return Status{ErrorCode::Cancelled, {}};
              auto source = window.row_run({cursor});
              auto target = output.row_run({cursor});
              if (!source.ok() || !target.ok())
                return source.ok() ? target.status() : source.status();
              const auto count = std::min(
                  block,
                  std::min(source.value().samples, target.value().samples));
              auto work = phase.consume_work(count);
              if (!work.ok())
                return work;
              for (std::uint64_t i = 0; i < count; ++i) {
                double value;
                std::memcpy(&value,
                            source.value().data +
                                static_cast<std::int64_t>(i) *
                                    source.value().sample_stride_bytes,
                            sizeof(value));
                if (!std::isfinite(value))
                  return Status{
                      ErrorCode::OperationFailed,
                      "nonfinite scan input " + std::to_string(cursor + i)};
                carry += value;
                if (!std::isfinite(carry))
                  return Status{ErrorCode::OperationFailed,
                                "scan overflow " + std::to_string(cursor + i)};
                std::memcpy(target.value().data +
                                static_cast<std::int64_t>(i) *
                                    target.value().sample_stride_bytes,
                            &carry, sizeof(carry));
              }
              cursor += count;
            }
          }
          return Status::success();
        },
        phase.query.cancellation);
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto sealed = builder.seal();
    if (!sealed.ok())
      return Result<ResultProgramPoll>(sealed.status());
    return Result<ResultProgramPoll>(
        ResultPublication{sealed.take_value(), true});
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
  traits.requires_metadata_specialization = true;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(ScanState));
  traits.outputs[0].key = "value";
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.workspace_bytes = 16;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    const auto shape = inputs[0].result_schema->tensors[0].sample_shape();
    if (shape.size() != 1)
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
  return registry->register_operation(
      sdk::with_whole_stage(std::move(operation), UINT64_C(1) << 48));
}
}  // namespace ps::plugin_internal
