#pragma once

#include <functional>

#include "photospider/data/storage.hpp"
#include "photospider/execution/resources.hpp"

namespace ps::color_internal {
// The profile is already frozen. This parser checks supported endpoint profile
// classes, required tags and structures and public envelopes; it neither loads
// files nor runs transforms.
Status validate_icc(ByteView bytes, const ResourceBudget& budget,
                    const std::function<Status(std::uint64_t)>& consume);
}  // namespace ps::color_internal
