#include "execution/result_publication_validation.hpp"

#include <tuple>
#include <utility>
#include <vector>

#include "data/value_validation.hpp"
#include "execution/result_protocol.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/validation_dispatch.hpp"

namespace ps::execution_internal {
Status ResultPublicationValidator::validate(
    const PublicationValidationView& view, const ResultPublication& published) {
  const auto& step = view.step;
  if (!step.output_result_schema ||
      !published.result.owned_by(view.resources) ||
      !published.result.schema().same_schema(*step.output_result_schema) ||
      !published.result.matches_scope(view.query.semantic_key) ||
      (view.previous.valid() &&
       view.previous.object_id() != published.result.object_id()))
    return protocol_failure("structured publication identity mismatch");
  published.result.bind_producer(view.node_id);
  auto descriptor = published.result.descriptor(published.complete);
  if (!descriptor.ok() ||
      descriptor.value().revision() <= view.previous_revision ||
      descriptor.value().sealed() != published.complete)
    return protocol_failure("invalid descriptor publication");
  if (step.tiled) {
    if (!view.query.tensor_outputs)
      return protocol_failure("tile publication has no computation unit");
    const auto& coverage = descriptor.value().tensor_coverage(0);
    auto outside = coverage.subtract(*view.query.tensor_outputs,
                                     view.services.set_limits());
    if (!outside.ok())
      return outside.status();
    if (!outside.value().empty() ||
        (published.complete && coverage != *view.query.tensor_outputs))
      return protocol_failure("tile publication differs from computation unit");
  }
  if (published.complete && !step.tiled &&
      (!view.query.tensor_outputs || !view.query.tensor_outputs->empty())) {
    for (std::uint32_t slot = 0;
         slot < published.result.schema().tensors.size(); ++slot) {
      auto full =
          Footprint::all(published.result.schema().tensors[slot].sample_shape(),
                         view.services.set_limits());
      if (!full.ok())
        return full.status();
      auto missing = full.value().subtract(
          descriptor.value().tensor_coverage(slot), view.services.set_limits());
      if (!missing.ok())
        return missing.status();
      if (!missing.value().empty())
        return protocol_failure("Whole publication omitted a tensor member");
    }
  }
  auto facts_work = view.services.consume(view.input_facts.size());
  if (!facts_work.ok())
    return facts_work;
  ResourceVector<std::uint64_t> association{
      ResourceAllocator<std::uint64_t>(view.resources)};
  association.reserve(view.input_facts.size());
  for (const auto& input : view.input_facts)
    association.push_back(input.first.second);
  auto retained = published.result.retain_association(
      association, [&](auto n) { return view.services.consume(n); });
  if (!retained.ok())
    return retained;
  if (view.query.prepared) {
    std::optional<ResultDescriptor> previous;
    if (view.previous.valid()) {
      auto captured = view.previous.descriptor(false);
      if (!captured.ok())
        return captured.status();
      previous = captured.take_value();
    }
    auto checked = plugin_internal::validate_result_contract(
        *view.query.prepared, true, view.query.output_index, published.result,
        descriptor.value(), previous ? &*previous : nullptr, view.resources,
        view.services.cancellation(),
        [&](auto n) { return view.services.operation_work(n); },
        view.maximum_window, ErrorCode::OperationFailed);
    if (!checked.ok())
      return checked;
  }
  if (step.traits.outputs[0].output_schema.scalar_bounds) {
    auto checked = input_internal::validate_port_tensor(
        step.traits.outputs[0].output_schema, published.result,
        descriptor.value(), view.query.output, ErrorCode::OperationFailed,
        view.services.cancellation(), [&] { return view.services.stop(); });
    if (!checked.ok())
      return checked;
  }
  if (published.complete && view.query.tensor_outputs) {
    auto outside = view.query.tensor_outputs->subtract(
        descriptor.value().tensor_coverage(view.query.tensor_slot),
        view.services.set_limits());
    if (!outside.ok())
      return outside.status();
    if (!outside.value().empty())
      return protocol_failure("image publication omitted captured demand");
  }
  return Status::success();
}
}  // namespace ps::execution_internal
