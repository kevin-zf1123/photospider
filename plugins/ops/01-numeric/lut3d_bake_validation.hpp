#pragma once

#include <cstdint>
#include <functional>

#include "photospider/ops/numeric/lut3d_bake.hpp"

namespace ps::input_internal {
Status validate_lut3d_bake_report_values(
    const Lut3dBakeDescription& description, Lut3dBakeReport* report,
    std::uint8_t passed,
    const std::function<Status(std::uint64_t)>& consume_work);
}  // namespace ps::input_internal
