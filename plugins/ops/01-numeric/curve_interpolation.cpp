#include <string>
#include <utility>

#include "01-numeric/fixed_curve.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
using numeric_ops::SequenceProfile;
Status register_curve_interpolation(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool pchip : {false, true})
      for (bool multi : {false, true}) {
        auto status = registry->register_operation(fixed_curve::curve_operation(
            std::string("curve.interpolate_") + (pchip ? "pchip" : "linear") +
                (multi ? "_multi" : "") + entry.first,
            pchip, multi, false, entry.second));
        if (!status.ok())
          return status;
      }
  return Status::success();
}
}  // namespace ps::plugin_internal
