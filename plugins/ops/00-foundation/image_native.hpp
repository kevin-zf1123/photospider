#pragma once

#include <cstdint>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::image_ops {
Result<MutableBuffer> native_image(const ResultProgramPhase& phase,
                                   std::uint32_t kind, const Region& output,
                                   const std::vector<Region>& sources,
                                   unsigned radius, const double* weights,
                                   unsigned factor);
}  // namespace ps::plugin_internal::image_ops
