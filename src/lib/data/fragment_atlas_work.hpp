#pragma once

#include <cstdint>
#include <functional>

#include "photospider/data/footprint.hpp"

namespace ps::data_internal {
inline Status invalid_fragment_atlas() {
  return Status{ErrorCode::InvalidArgument, "invalid fragment atlas"};
}
struct FragmentAtlasWork {
  std::uint64_t left;
  CancellationToken cancel;
  std::function<Status(std::uint64_t)> charge = {};
  Status consume(std::uint64_t cost) {
    if (cancel.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (cost > left)
      return Status{ErrorCode::ResourceExhausted, {}};
    auto status = charge ? charge(cost) : Status::success();
    if (status.ok())
      left -= cost;
    return status;
  }
};
}  // namespace ps::data_internal
