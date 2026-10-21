#pragma once

#include <functional>

#include "photospider/core/resources.hpp"
#include "photospider/data/storage.hpp"

namespace ps::color_internal {
// The profile is already frozen. This parser checks supported endpoint profile
// classes, required tags and structures and public envelopes; it neither loads
// files nor runs transforms.
Status validate_icc(ByteView bytes, const ResourceBudget& budget,
                    const std::function<Status(std::uint64_t)>& consume);
// Also checks the optional ICC Profile ID against the frozen bytes. Resource
// identity is independently established by ImmutableResource admission. Neither
// this function nor validate_icc copies the full profile payload.
Status validate_icc_resource(
    ByteView bytes, const ResourceBudget& budget,
    const std::function<Status(std::uint64_t)>& consume);
}  // namespace ps::color_internal
