#pragma once

#include <memory>

#include "photospider/ops/export.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
/**
 * @brief Creates the maintained built-in operation set.
 * @param freeze Whether to freeze immediately (default true). Pass false to
 * register embedding operations, then freeze before compilation/execution.
 * @return Shared registry owning all maintained built-in operations.
 * @throws std::bad_alloc If construction fails.
 * @note Adding custom operations clears the built-in persistent cache identity.
 * Mutation is serialized; freeze must precede concurrent execution.
 */
[[nodiscard]] PHOTOSPIDER_OPS_API std::shared_ptr<OperationRegistry>
make_default_operation_registry(bool freeze = true);

}  // namespace ps
