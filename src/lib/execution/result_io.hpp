#pragma once

#include <cstdint>
#include <functional>

#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
// The validated request and services are one-call borrows. Returned payload or
// temporary capabilities own their leases; source plans retire after physical
// work. Charge occurs before I/O, and issued work is never refunded on failure.
Result<ResultIoReply> execute_result_io(
    const ResultIoRequest&, const ResourceBudget&, std::uint64_t maximum_window,
    const std::function<Status(std::uint64_t)>& consume,
    const std::function<CancellationToken()>& cancellation);
}  // namespace ps::execution_internal
