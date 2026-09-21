#include <string>
#include <utility>

#include "01-numeric/numeric_math_operation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_binary(OperationRegistry* registry) {
  using namespace numeric_ops;  // NOLINT(build/namespaces)
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& entry :
         {std::make_pair("add", ElementaryKind::Add),
          std::make_pair("subtract", ElementaryKind::Subtract),
          std::make_pair("multiply", ElementaryKind::Multiply),
          std::make_pair("divide", ElementaryKind::Divide),
          std::make_pair("minimum", ElementaryKind::Minimum),
          std::make_pair("maximum", ElementaryKind::Maximum)}) {
      auto status =
          registry->register_operation(point_math_operation<ExactElementary>(
              std::string("numeric.") + entry.first + profile.first,
              entry.second, profile.second, 2,
              entry.second == ElementaryKind::Divide ? 12U : 15U));
      if (!status.ok())
        return status;
    }
    for (const auto& entry :
         {std::make_pair("pow", CertifiedKind::Pow),
          std::make_pair("atan2", CertifiedKind::Atan2),
          std::make_pair("atan2pi", CertifiedKind::Atan2pi)}) {
      auto status =
          registry->register_operation(point_math_operation<CertifiedMath>(
              std::string("numeric.") + entry.first + profile.first,
              entry.second, profile.second, 2, 12U));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
