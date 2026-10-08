#pragma once

#include <cstdint>
#include <memory>

#include "plugin/result_c_codec.hpp"

namespace ps::plugin_internal::result_c {
// Returns a continuation owning one C joint payload and its member bridges.
// Query frames and start views are synchronous borrows. State destroy runs
// exactly once, after the final callback, before the retained DSO is released.
std::uint64_t c_joint_storage_bytes() noexcept;
Result<ResultJointContinuation> start_c_joint(
    std::shared_ptr<const Definition>,
    const ResourceVector<ResultProgramQuery>&, const BufferAllocator&);
}  // namespace ps::plugin_internal::result_c
