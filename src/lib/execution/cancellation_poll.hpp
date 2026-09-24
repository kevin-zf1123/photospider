#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "photospider/execution/cancellation.hpp"

namespace ps::execution_internal {
// Internal allocation-free borrowed view for scalar polling within an ISA
// scope. The source token must remain alive and unchanged until polling ends.
// A combined token retains at most 64 source flags plus its own group flag.
// No pointers escape the synchronous host invocation. Loads use acquire order,
// exactly as CancellationToken::cancelled(). No public layout or ABI changes.
struct CancellationPoll final {
  std::array<const std::atomic<bool>*, 65> flags{};
  std::size_t size = 0;
  static CancellationPoll borrow(const CancellationToken& token) noexcept;
};
}  // namespace ps::execution_internal
