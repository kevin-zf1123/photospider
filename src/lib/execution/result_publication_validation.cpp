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
    const PublicationValidationView& view, const ResultPublication& published,
    bool& retire_actor) {
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
  for (uint32_t slot = 0; slot < published.result.schema().tensors.size();
       ++slot) {
    auto relation = published.result.tensor_relation(slot);
    if (!relation.ok())
      return relation.status();
    for (uint32_t port = 0; port < view.query.inputs.size(); ++port) {
      const auto& schema = view.query.inputs[port].result_schema;
      if (!schema)
        continue;
      for (uint32_t member = 0; member < schema->tensors.size(); ++member) {
        const auto& tensor = schema->tensors[member];
        const auto channel = input_internal::tuple_channel_axis(
            tensor.descriptor, tensor.facets);
        if (!channel)
          continue;
        auto witness = relation.value();
        if (step.traits.outputs[0].region_rule == OperationRegionRule::Whole) {
          ResourceVector<ResultRelation> obligations{
              ResourceAllocator<ResultRelation>(view.resources)};
          obligations.push_back(witness);
          for (const auto& history : view.history) {
            auto charged = view.services.consume(1);
            if (!charged.ok())
              return charged;
            if (std::get<0>(history.first) != port ||
                std::get<1>(history.first) != ResultSupportTarget::Tensor ||
                std::get<2>(history.first) != member ||
                !(std::get<3>(history.first) & 4U))
              continue;
            for (const auto& box : history.second.boxes()) {
              auto scratch = view.resources.reserve(ResourceCapacity::host(
                  8 * sizeof(ResultMappedAxis), 8 * sizeof(ResultMappedAxis)));
              if (!scratch.ok())
                return scratch.status();
              auto lease = scratch.take_value();
              std::vector<ResultMappedAxis> axes;
              axes.reserve(8);
              for (const auto dimension : box.dimensions())
                axes.push_back({-1, dimension.offset, 0, dimension.extent});
              const auto& shape =
                  published.result.schema().tensors[slot].sample_shape();
              auto obligation = ResultRelation::mapped(
                  view.resources, shape, Region::whole(shape),
                  tensor.sample_shape(), axes,
                  {port, 4, 0, 0, ResultSupportTarget::Tensor, member});
              if (!obligation.ok())
                return obligation.status();
              obligations.push_back(obligation.take_value());
            }
          }
          while (obligations.size() > 1) {
            ResourceVector<ResultRelation> next{
                ResourceAllocator<ResultRelation>(view.resources)};
            for (size_t i = 0; i < obligations.size(); i += 2) {
              if (i + 1 == obligations.size()) {
                next.push_back(obligations[i]);
                continue;
              }
              auto joined = ResultRelation::unite(
                  view.resources, {obligations[i], obligations[i + 1]});
              if (!joined.ok())
                return joined.status();
              next.push_back(joined.take_value());
            }
            obligations = std::move(next);
          }
          witness = obligations.front();
        }
        const auto& output_tensor = published.result.schema().tensors[slot];
        const auto output_shape = output_tensor.sample_shape();
        uint32_t grouped_axes = 0;
        for (size_t axis =
                 output_shape.size() - output_tensor.atomic_trailing_axes;
             axis < output_shape.size(); ++axis)
          grouped_axes |= 1U << axis;
        const auto output_channel = input_internal::tuple_channel_axis(
            output_tensor.descriptor, output_tensor.facets);
        if (output_channel)
          grouped_axes |=
              1U << (*output_channel + output_tensor.batch_axes.size());
        auto checked = witness.validate_tuple_closure(
            descriptor.value().tensor_coverage(slot), port, member,
            tensor.sample_shape(), *channel + tensor.batch_axes.size(),
            grouped_axes, view.services.set_limits());
        if (!checked.ok())
          return checked;
      }
    }
  }
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
  const bool request_record =
      step.traits.outputs[0].observation_kind == ObservationKind::RequestRecord;
  if (request_record && !published.complete)
    return protocol_failure("terminal Result requires complete publication");
  if (request_record && view.query.tensor_outputs &&
      descriptor.value().tensor_coverage(view.query.tensor_slot) !=
          *view.query.tensor_outputs) {
    retire_actor = true;
    return protocol_failure(
        "terminal Result coverage differs from captured query");
  }
  if (published.complete && view.query.tensor_outputs) {
    auto outside = view.query.tensor_outputs->subtract(
        descriptor.value().tensor_coverage(view.query.tensor_slot),
        view.services.set_limits());
    if (!outside.ok() || !outside.value().empty())
      return protocol_failure("image publication omitted captured demand");
  }
  return Status::success();
}
}  // namespace ps::execution_internal
