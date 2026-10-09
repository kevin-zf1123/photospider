#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>

#include "core/checked_math.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::plugin_internal {
inline Status invalid_result_need(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::None,
          {FailureOrigin::Protocol, FailureScope::Group}};
}
inline Status validate_result_io(const ResultIoRequest& request,
                                 const ResourceBudget& resources,
                                 std::uint64_t maximum_window_bytes) {
  if (const auto* read = std::get_if<ResultReadPlan>(&request))
    return read->owned_by(resources) &&
                   read->byte_size() <= maximum_window_bytes
               ? Status::success()
               : invalid_result_need("invalid result read window");
  if (const auto* write = std::get_if<ResultWritePlan>(&request))
    return write->owned_by(resources) &&
                   write->byte_size() <= maximum_window_bytes
               ? Status::success()
               : invalid_result_need("invalid result write plan");
  if (const auto* read = std::get_if<ResultReadTemporary>(&request))
    return read->storage.owned_by(resources) && read->bytes &&
                   read->bytes <= maximum_window_bytes &&
                   read->offset <= read->storage.size() &&
                   read->bytes <= read->storage.size() - read->offset
               ? Status::success()
               : invalid_result_need("invalid temporary read window");
  if (const auto* write = std::get_if<ResultWriteTemporary>(&request))
    return write->storage.owned_by(resources) && write->bytes &&
                   write->bytes->capacity() <= maximum_window_bytes &&
                   write->offset <= write->storage.size() &&
                   write->bytes->capacity() <=
                       write->storage.size() - write->offset
               ? Status::success()
               : invalid_result_need("invalid temporary write window");
  if (const auto* extend = std::get_if<ResultExtendTemporary>(&request))
    return extend->storage.owned_by(resources) && extend->bytes &&
                   core_internal::can_add(extend->storage.size(), extend->bytes,
                                          INT64_MAX)
               ? Status::success()
               : invalid_result_need("invalid temporary extension");
  return Status::success();
}
inline Status validate_result_need(
    const ResultProgramNeed& need, const ResultProgramQuery& query,
    const OperationOutputTraits& output, const ResourceBudget& resources,
    std::uint64_t maximum, std::uint64_t maximum_window_bytes,
    bool allow_empty = false,
    const std::function<Status(std::uint64_t)>& consume_work = {}) {
  const auto count = need.results.size() + need.tensors.size() + need.io.size();
  if ((count == 0 && !allow_empty) || count > maximum ||
      (!need.results.empty() &&
       !need.results.get_allocator().owned_by(resources)) ||
      (!need.io.empty() && !need.io.get_allocator().owned_by(resources)) ||
      (!need.tensors.empty() &&
       !need.tensors.get_allocator().owned_by(resources)))
    return invalid_result_need("invalid structured Need envelope");
  if (consume_work) {
    auto charged = consume_work(count);
    if (!charged.ok())
      return charged;
  }
  std::array<bool, 1024> requested_results{};
  if (query.inputs.size() > 1024)
    return invalid_result_need("invalid Result Need input domain");
  const auto selected_port = [&](std::uint32_t port) {
    const auto& selection = output.input_indices;
    return !selection || std::find(selection->begin(), selection->end(),
                                   port) != selection->end();
  };
  // Validate the complete envelope before executing a source read or I/O.
  for (const auto& input : need.results)
    if (input.input >= query.inputs.size() || !selected_port(input.input) ||
        !query.inputs[input.input].result_schema ||
        (!input.complete &&
         input.field >=
             query.inputs[input.input].result_schema->fields.size()) ||
        std::exchange(requested_results[input.input], true))
      return invalid_result_need("invalid structured Result request");
  for (const auto& input : need.tensors) {
    if (input.input >= query.inputs.size() || !selected_port(input.input) ||
        !input.roles || (input.roles & ~15U) ||
        !query.inputs[input.input].result_schema ||
        input.slot >= query.inputs[input.input].result_schema->tensors.size() ||
        !input.samples.valid() ||
        input.samples.shape() != query.inputs[input.input]
                                     .result_schema->tensors[input.slot]
                                     .sample_shape())
      return invalid_result_need("invalid typed image Need");
  }
  for (const auto& action : need.io) {
    auto valid = validate_result_io(action, resources, maximum_window_bytes);
    if (!valid.ok())
      return valid;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
