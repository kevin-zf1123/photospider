#include <string>
#include <utility>

#include "01-numeric/fixed_curve.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
using numeric_ops::SequenceProfile;
Status register_curve_inverse(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool pchip : {false, true}) {
      auto status = registry->register_operation(fixed_curve::curve_operation(
          std::string(pchip ? "curve.invert_pchip" : "curve.invert_linear") +
              profile.first,
          pchip, false, true, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
