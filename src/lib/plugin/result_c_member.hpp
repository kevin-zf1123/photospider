#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "plugin/failure_latch.hpp"
#include "plugin/result_c_codec.hpp"

namespace ps::plugin_internal::result_c {
// Opaque owner of C continuation bytes and retained grants/window tombstones.
// A poll borrows phase/services only until return; expired lease addresses stay
// owned until continuation retirement. Joint members borrow their shared C
// payload and never invoke the single-operation destroy callback.
class CResultMemberBridge;
using MemberBorrow =
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    std::function<int(const ps_result_query_v2*, const ps_result_services_v2*)>;
std::uint64_t c_member_storage_bytes() noexcept;
Result<ResultContinuation> start_c_member(std::shared_ptr<const Definition>,
                                          const BufferAllocator&);
std::shared_ptr<CResultMemberBridge> make_c_joint_member(
    std::shared_ptr<const Definition>, const ResourceBudget&,
    std::shared_ptr<std::uint64_t>, std::shared_ptr<FailureLatch>);
Result<ResultProgramPoll> poll_c_member(CResultMemberBridge&,
                                        const ResultProgramPhase&,
                                        const MemberBorrow&);
Status c_member_failure(const CResultMemberBridge&);
bool c_member_terminal_conflicts(const CResultMemberBridge&) noexcept;
}  // namespace ps::plugin_internal::result_c
