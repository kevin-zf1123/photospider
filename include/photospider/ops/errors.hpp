#pragma once

#include <string>
#include <utility>

#include "photospider/core/status.hpp"
namespace ps::ops_error {
/** @brief Ops-owned stable domain codes; transport preserves their exact value.
 */
inline constexpr DomainErrorCode InvalidAssociation{0x50534f5000000001ULL};
inline constexpr DomainErrorCode AssociationUnderflow{0x50534f5000000002ULL};
inline constexpr DomainErrorCode DivideByZero{0x50534f5000000003ULL};
inline constexpr DomainErrorCode InvalidQuality{0x50534f5000000004ULL};
inline Status invalid_quality(
    std::string message = "invalid numerical quality report") {
  return {ErrorCode::InvalidArgument, std::move(message), InvalidQuality};
}
}  // namespace ps::ops_error
