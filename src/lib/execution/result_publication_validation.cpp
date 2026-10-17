#include "execution/result_publication_validation.hpp"

#include <tuple>
#include <utility>
#include <vector>

#include "data/tensor_validation.hpp"
#include "execution/result_protocol.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/publication_visibility.hpp"
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
  if (view.query.shared_stage != UINT32_MAX) {
    auto tiles = plugin_internal::published_tiles(
        published.result.schema(), descriptor.value(), view.query.tile_extent,
        view.resources, view.services.set_limits());
    if (!tiles.ok())
      return tiles.status();
  } else {
    if (!published.complete || !view.query.tensor_outputs)
      return protocol_failure("tile publication must complete one unit");
    for (std::uint32_t slot = 0; slot < descriptor.value().tensor_count();
         ++slot) {
      const auto& coverage = descriptor.value().tensor_coverage(slot);
      if (slot == view.query.tensor_slot) {
        if (coverage != *view.query.tensor_outputs)
          return protocol_failure(
              "tile publication differs from selected member unit");
      } else if (!coverage.empty()) {
        return protocol_failure("tile publication includes another member");
      }
    }
  }
  if (view.query.prepared && !view.defer_contract) {
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
        view.maximum_window, ErrorCode::OperationFailed,
        view.query.shared_stage);
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
