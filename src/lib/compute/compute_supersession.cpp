#include "compute/compute_supersession.hpp"

#include <limits>
#include <stdexcept>
#include <tuple>

namespace ps::compute {

/** @copydoc SupersessionKey::SupersessionKey */
SupersessionKey::SupersessionKey(int target_node_id,
                                 ComputeIntent request_intent)
    : target_node_id_(target_node_id), request_intent_(request_intent) {
  if (target_node_id_ < 0) {
    throw std::invalid_argument(
        "SupersessionKey requires a nonnegative target node id.");
  }
  if (request_intent_ != ComputeIntent::GlobalHighPrecision &&
      request_intent_ != ComputeIntent::RealTimeUpdate) {
    throw std::invalid_argument(
        "SupersessionKey requires HP or realtime request intent.");
  }
}

/** @copydoc SupersessionKey::operator== */
bool SupersessionKey::operator==(const SupersessionKey& other) const noexcept {
  return target_node_id_ == other.target_node_id_ &&
         request_intent_ == other.request_intent_;
}

/** @copydoc SupersessionKey::operator< */
bool SupersessionKey::operator<(const SupersessionKey& other) const noexcept {
  return std::tie(target_node_id_, request_intent_) <
         std::tie(other.target_node_id_, other.request_intent_);
}

/** @copydoc SupersessionGeneration::SupersessionGeneration */
SupersessionGeneration::SupersessionGeneration(std::uint64_t value)
    : value_(value) {
  if (value_ == 0) {
    throw std::invalid_argument(
        "SupersessionGeneration value must be nonzero.");
  }
}

/** @copydoc SupersessionGenerationAllocator::SupersessionGenerationAllocator */
SupersessionGenerationAllocator::SupersessionGenerationAllocator(
    std::uint64_t next_value)
    : next_value_(next_value) {
  if (next_value_ == 0) {
    throw std::invalid_argument(
        "Supersession generation allocator requires a nonzero start.");
  }
}

/** @copydoc SupersessionGenerationAllocator::allocate */
SupersessionGeneration SupersessionGenerationAllocator::allocate() {
  if (exhausted_) {
    throw std::overflow_error("Supersession generation is exhausted.");
  }
  const SupersessionGeneration result(next_value_);
  if (next_value_ == std::numeric_limits<std::uint64_t>::max()) {
    exhausted_ = true;
  } else {
    ++next_value_;
  }
  return result;
}

/** @copydoc normalize_supersession_intent */
ComputeIntent normalize_supersession_intent(
    std::optional<ComputeIntent> intent) {
  const ComputeIntent normalized =
      intent.value_or(ComputeIntent::GlobalHighPrecision);
  if (normalized != ComputeIntent::GlobalHighPrecision &&
      normalized != ComputeIntent::RealTimeUpdate) {
    throw std::invalid_argument(
        "Supersession intent requires HP or realtime request intent.");
  }
  return normalized;
}

}  // namespace ps::compute
