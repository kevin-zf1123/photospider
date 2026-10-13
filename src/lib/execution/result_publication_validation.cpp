#include "execution/result_publication_validation.hpp"

#include <tuple>
#include <utility>
#include <vector>

#include "data/value_validation.hpp"
#include "execution/result_protocol.hpp"
#include "photospider/data/representation.hpp"
#include "plugin/port_validation.hpp"

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
  if (step.traits.outputs[0].output_schema.scalar_bounds) {
    auto checked = input_internal::validate_port_tensor(
        step.traits.outputs[0].output_schema, published.result,
        descriptor.value(), view.query.output, ErrorCode::OperationFailed,
        view.services.cancellation(), [&] { return view.services.stop(); });
    if (!checked.ok())
      return checked;
  }
  for (uint32_t slot = 0; slot < published.result.schema().tensors.size();
       ++slot) {
    auto checked = input_internal::validate_tensor_samples(
        published.result, descriptor.value(), slot,
        descriptor.value().tensor_coverage(slot), view.resources,
        ErrorCode::OperationFailed, view.services.cancellation(),
        [&] { return view.services.stop(); },
        [&](uint64_t n) { return view.services.operation_work(n); });
    if (!checked.ok()) {
      if (checked.detail.origin == FailureOrigin::Unspecified &&
          checked.code == ErrorCode::OperationFailed)
        checked.detail.origin = FailureOrigin::Domain;
      checked.detail.node_id = view.node_id;
      return checked;
    }
  }
  auto validated = validate_representation(
      published.result, view.resources, view.maximum_window,
      view.services.cancellation(), [&](std::uint64_t count) {
        const auto stopped = view.services.stop();
        if (stopped != ErrorCode::Ok)
          return Status{stopped, {}};
        return view.services.run_work(count);
      });
  if (!validated.ok()) {
    if ((validated.detail.origin == FailureOrigin::Unspecified ||
         validated.detail.origin == FailureOrigin::Domain ||
         validated.detail.origin == FailureOrigin::Schema) &&
        validated.code != ErrorCode::ResourceExhausted &&
        validated.code != ErrorCode::Cancelled &&
        validated.code != ErrorCode::Stale) {
      if (validated.detail.origin == FailureOrigin::Unspecified)
        validated.detail.origin = FailureOrigin::Schema;
      validated.detail.scope = FailureScope::Association;
      validated.detail.association = published.result.object_id();
    }
    return validated;
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
