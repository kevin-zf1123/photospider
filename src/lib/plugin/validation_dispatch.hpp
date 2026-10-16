#pragma once

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
Status validate_tensor_contract(
    const PreparedOperation& prepared, std::uint32_t port,
    const SchemaTemplate& schema, std::uint32_t slot,
    const ResultTensorInput& input, const Footprint& granted,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume,
    ErrorCode domain_failure);
Status validate_tensor_contracts(
    const PreparedOperation& prepared, std::uint32_t port,
    const SchemaTemplate& schema, const ResultTensorInputs& inputs,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume,
    ErrorCode domain_failure);
Status validate_result_contract(
    const PreparedOperation& prepared, bool output, std::uint32_t port,
    const ResultRef& result, const ResultDescriptor& descriptor,
    const ResultDescriptor* previous, const ResourceBudget& resources,
    const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume,
    std::uint64_t maximum_window, ErrorCode domain_failure,
    std::uint32_t shared_stage = UINT32_MAX);
}  // namespace ps::plugin_internal
