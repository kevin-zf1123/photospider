#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "plugin/failure_latch.hpp"
#include "plugin/result_c_codec.hpp"

namespace ps::plugin_internal::result_c {
// Opaque owner of C continuation bytes and retained grants/window tombstones.
// A poll borrows phase/services only until return; expired lease addresses stay
// owned until continuation retirement.
class CResultMemberBridge;
std::uint64_t c_member_storage_bytes() noexcept;
Result<ResultContinuation> start_c_member(std::shared_ptr<const Definition>,
                                          const BufferAllocator&,
                                          std::uint64_t state_bytes);
}  // namespace ps::plugin_internal::result_c
