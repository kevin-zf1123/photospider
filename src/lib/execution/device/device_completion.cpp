#include "execution/device/device_completion.hpp"

#include <utility>

namespace ps::execution {

/** @copydoc DeviceCompletionSeed::DeviceCompletionSeed */
DeviceCompletionSeed::DeviceCompletionSeed(
    std::uint64_t graph_instance_id, int target_node_id,
    ComputeIntent request_intent, std::uint64_t supersession_generation,
    std::uint64_t run_id, std::uint64_t local_task_id,
    DeviceCompletionUse completion_use)
    : graph_instance_id_(graph_instance_id),
      target_node_id_(target_node_id),
      request_intent_(request_intent),
      supersession_generation_(supersession_generation),
      run_id_(run_id),
      local_task_id_(local_task_id),
      completion_use_(completion_use) {
  if (graph_instance_id_ == 0U || target_node_id_ < 0 ||
      supersession_generation_ == 0U || run_id_ == 0U) {
    throw std::invalid_argument(
        "Device completion seed requires nonzero identities and a valid "
        "target.");
  }
  switch (request_intent_) {
    case ComputeIntent::GlobalHighPrecision:
    case ComputeIntent::RealTimeUpdate:
      break;
    default:
      throw std::invalid_argument(
          "Device completion seed requires a supported request intent.");
  }
  switch (completion_use_) {
    case DeviceCompletionUse::CurrentRunSubmission:
    case DeviceCompletionUse::PublishedValueAcquisition:
      return;
  }
  throw std::invalid_argument(
      "Device completion seed requires a supported completion use.");
}

/** @copydoc DeviceCompletionSeed::operator== */
bool DeviceCompletionSeed::operator==(
    const DeviceCompletionSeed& other) const noexcept {
  return graph_instance_id_ == other.graph_instance_id_ &&
         target_node_id_ == other.target_node_id_ &&
         request_intent_ == other.request_intent_ &&
         supersession_generation_ == other.supersession_generation_ &&
         run_id_ == other.run_id_ && local_task_id_ == other.local_task_id_ &&
         completion_use_ == other.completion_use_;
}

/** @copydoc DeviceCompletionIdentity::DeviceCompletionIdentity */
DeviceCompletionIdentity::DeviceCompletionIdentity(DeviceCompletionSeed seed,
                                                   const Value& source,
                                                   const Value& destination)
    : seed_(std::move(seed)) {
  if (!source.valid() || !destination.valid()) {
    throw std::invalid_argument(
        "Device completion identity requires valid Values.");
  }
  source_revision_ = source.revision_id();
  destination_revision_ = destination.revision_id();
  source_producer_ = source.producer_identity();
  destination_producer_ = destination.producer_identity();
  source_binding_ = source.storage_binding();
  destination_binding_ = destination.storage_binding();
  if (source_revision_ != destination_revision_) {
    throw std::invalid_argument(
        "Device completion identity requires a revision-preserving replica.");
  }
  if (source_binding_ == destination_binding_) {
    throw std::invalid_argument(
        "Device completion identity requires distinct physical bindings.");
  }
}

/** @copydoc DeviceCompletionIdentity::operator== */
bool DeviceCompletionIdentity::operator==(
    const DeviceCompletionIdentity& other) const noexcept {
  return seed_ == other.seed_ && source_revision_ == other.source_revision_ &&
         destination_revision_ == other.destination_revision_ &&
         source_producer_ == other.source_producer_ &&
         destination_producer_ == other.destination_producer_ &&
         source_binding_ == other.source_binding_ &&
         destination_binding_ == other.destination_binding_;
}

}  // namespace ps::execution
