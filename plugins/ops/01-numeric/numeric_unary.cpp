#include <string>
#include <utility>

#include "01-numeric/numeric_math_operation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_unary(OperationRegistry* registry) {
  using namespace numeric_ops;  // NOLINT(build/namespaces)
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& entry :
         {std::make_pair("abs", ElementaryKind::Abs),
          std::make_pair("neg", ElementaryKind::Neg),
          std::make_pair("sqrt", ElementaryKind::Sqrt),
          std::make_pair("floor", ElementaryKind::Floor),
          std::make_pair("ceil", ElementaryKind::Ceil),
          std::make_pair("round", ElementaryKind::Round),
          std::make_pair("sign", ElementaryKind::Sign),
          std::make_pair("reciprocal", ElementaryKind::Reciprocal)}) {
      const auto mask = entry.second == ElementaryKind::Neg ? 14U
                        : entry.second == ElementaryKind::Sqrt ||
                                entry.second == ElementaryKind::Reciprocal
                            ? 12U
                            : 15U;
      auto status =
          registry->register_operation(point_math_operation<ExactElementary>(
              std::string("numeric.") + entry.first + profile.first,
              entry.second, profile.second, 1, mask));
      if (!status.ok())
        return status;
    }
    for (const auto& entry :
         {std::make_pair("exp", CertifiedKind::Exp),
          std::make_pair("ln", CertifiedKind::Ln),
          std::make_pair("sin", CertifiedKind::Sin),
          std::make_pair("cos", CertifiedKind::Cos),
          std::make_pair("tan", CertifiedKind::Tan),
          std::make_pair("sinpi", CertifiedKind::Sinpi),
          std::make_pair("cospi", CertifiedKind::Cospi),
          std::make_pair("tanpi", CertifiedKind::Tanpi),
          std::make_pair("sinc", CertifiedKind::Sinc),
          std::make_pair("sincpi", CertifiedKind::Sincpi),
          std::make_pair("sinpi_rational", CertifiedKind::SinpiRational),
          std::make_pair("cospi_rational", CertifiedKind::CospiRational),
          std::make_pair("tanpi_rational", CertifiedKind::TanpiRational),
          std::make_pair("sincpi_rational", CertifiedKind::SincpiRational)}) {
      const bool rational = CertifiedMath::rational(entry.second);
      auto status =
          registry->register_operation(point_math_operation<CertifiedMath>(
              std::string("numeric.") + entry.first + profile.first,
              entry.second, profile.second, rational ? 2 : 1, rational ? 2 : 12,
              rational));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
