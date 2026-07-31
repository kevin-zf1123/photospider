#include "execution/residency_manager.hpp"

#include <exception>
#include <map>
#include <stdexcept>
#include <tuple>

#include "core/pending_value.hpp"

namespace ps::execution {

/** @copydoc ResidencyManager::ResidencyManager */
ResidencyManager::ResidencyManager(std::size_t resident_capacity)
    : resident_capacity_(resident_capacity) {
  if (resident_capacity_ == 0U) {
    throw std::invalid_argument(
        "Residency replica-entry capacity must be positive.");
  }
}

/** @copydoc ResidencyManager::LineageKey::operator< */
bool ResidencyManager::LineageKey::operator<(
    const LineageKey& other) const noexcept {
  return std::tie(graph_instance_id, target_node_id, request_intent) <
         std::tie(other.graph_instance_id, other.target_node_id,
                  other.request_intent);
}

/** @copydoc ResidencyManager::ReplicaKey::operator< */
bool ResidencyManager::ReplicaKey::operator<(
    const ReplicaKey& other) const noexcept {
  return std::tie(revision, device, memory_domain) <
         std::tie(other.revision, other.device, other.memory_domain);
}

/** @copydoc ResidencyManager::lineage_key */
ResidencyManager::LineageKey ResidencyManager::lineage_key(
    const DeviceCompletionSeed& seed) noexcept {
  return lineage_key(seed.graph_instance_id(), seed.target_node_id(),
                     seed.request_intent());
}

/** @copydoc ResidencyManager::lineage_key */
ResidencyManager::LineageKey ResidencyManager::lineage_key(
    std::uint64_t graph_instance_id, int target_node_id,
    ComputeIntent request_intent) noexcept {
  return LineageKey{graph_instance_id, target_node_id, request_intent};
}

/** @copydoc ResidencyManager::observe_generation */
void ResidencyManager::observe_generation(const DeviceCompletionSeed& seed) {
  std::lock_guard<std::mutex> lock(mutex_);
  const LineageKey key = lineage_key(seed);
  auto current = current_generations_.find(key);
  if (current == current_generations_.end()) {
    current_generations_.emplace(key, seed.supersession_generation());
  } else if (current->second < seed.supersession_generation()) {
    current->second = seed.supersession_generation();
  }
}

/** @copydoc ResidencyManager::track_lineage */
void ResidencyManager::track_lineage(std::uint64_t graph_instance_id,
                                     int target_node_id,
                                     ComputeIntent request_intent) {
  if (graph_instance_id == 0U || target_node_id < 0 ||
      (request_intent != ComputeIntent::GlobalHighPrecision &&
       request_intent != ComputeIntent::RealTimeUpdate)) {
    throw std::invalid_argument(
        "Residency lineage tracking requires canonical request facts.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  current_generations_.try_emplace(
      lineage_key(graph_instance_id, target_node_id, request_intent), 0U);
}

/** @copydoc ResidencyManager::publish_current_generation */
void ResidencyManager::publish_current_generation(
    std::uint64_t graph_instance_id, int target_node_id,
    ComputeIntent request_intent,
    std::uint64_t supersession_generation) noexcept {
  try {
    if (graph_instance_id == 0U || target_node_id < 0 ||
        supersession_generation == 0U ||
        (request_intent != ComputeIntent::GlobalHighPrecision &&
         request_intent != ComputeIntent::RealTimeUpdate)) {
      std::terminate();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const LineageKey key =
        lineage_key(graph_instance_id, target_node_id, request_intent);
    auto current = current_generations_.find(key);
    if (current == current_generations_.end() ||
        current->second > supersession_generation) {
      std::terminate();
    }
    current->second = supersession_generation;
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ResidencyManager::retire_graph_lineages */
std::size_t ResidencyManager::retire_graph_lineages(
    std::uint64_t graph_instance_id) {
  if (graph_instance_id == 0U) {
    throw std::invalid_argument(
        "Residency lineage retirement requires a nonzero Graph identity.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& pending : pending_transfers_) {
    if (pending.second.seed().graph_instance_id() == graph_instance_id) {
      throw std::logic_error(
          "Residency lineage retirement requires drained Graph transfers.");
    }
  }

  std::size_t retired = 0U;
  auto lineage = current_generations_.lower_bound(
      LineageKey{graph_instance_id, -1, ComputeIntent::GlobalHighPrecision});
  while (lineage != current_generations_.end() &&
         lineage->first.graph_instance_id == graph_instance_id) {
    lineage = current_generations_.erase(lineage);
    ++retired;
  }
  return retired;
}

/** @copydoc ResidencyManager::lineage_count_for_graph */
std::size_t ResidencyManager::lineage_count_for_graph(
    std::uint64_t graph_instance_id) const {
  if (graph_instance_id == 0U) {
    throw std::invalid_argument(
        "Residency lineage observation requires a nonzero Graph identity.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0U;
  auto lineage = current_generations_.lower_bound(
      LineageKey{graph_instance_id, -1, ComputeIntent::GlobalHighPrecision});
  while (lineage != current_generations_.end() &&
         lineage->first.graph_instance_id == graph_instance_id) {
    ++lineage;
    ++count;
  }
  return count;
}

/** @copydoc ResidencyManager::register_transfer */
void ResidencyManager::register_transfer(
    const DeviceCompletionIdentity& identity) {
  std::lock_guard<std::mutex> lock(mutex_);
  const LineageKey key = lineage_key(identity.seed());
  const auto current = current_generations_.find(key);
  if (current != current_generations_.end() &&
      current->second > identity.seed().supersession_generation()) {
    throw std::invalid_argument(
        "Cannot register a stale device transfer completion.");
  }
  if (current == current_generations_.end()) {
    current_generations_.emplace(key,
                                 identity.seed().supersession_generation());
  } else if (current->second < identity.seed().supersession_generation()) {
    current->second = identity.seed().supersession_generation();
  }
  const std::uint64_t producer = identity.destination_producer().value();
  const auto pending = pending_transfers_.find(producer);
  if (pending != pending_transfers_.end()) {
    if (!(pending->second == identity)) {
      throw std::invalid_argument(
          "Destination producer is already bound to another transfer.");
    }
    return;
  }
  pending_transfers_.emplace(producer, identity);
}

/** @copydoc ResidencyManager::publish_ready_transfer */
ResidencyCompletionDisposition ResidencyManager::publish_ready_transfer(
    const DeviceCompletionIdentity& identity, const Value& source,
    const Value& destination, PendingDeviceValueProducer* source_producer,
    PendingDeviceValueProducer& destination_producer) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t producer = identity.destination_producer().value();
  const auto pending = pending_transfers_.find(producer);
  if (pending == pending_transfers_.end() || !(pending->second == identity)) {
    return ResidencyCompletionDisposition::Rejected;
  }
  const LineageKey key = lineage_key(identity.seed());
  const auto current = current_generations_.find(key);
  if (current == current_generations_.end() ||
      current->second != identity.seed().supersession_generation()) {
    pending_transfers_.erase(pending);
    return ResidencyCompletionDisposition::Stale;
  }
  if (!source.valid() || source.revision_id() != identity.source_revision() ||
      source.producer_identity() != identity.source_producer() ||
      source.storage_binding() != identity.source_binding() ||
      !destination.valid() ||
      destination.revision_id() != identity.destination_revision() ||
      destination.producer_identity() != identity.destination_producer() ||
      destination.storage_binding() != identity.destination_binding() ||
      destination.ready_fence().poll().state() != ReadyFenceState::Pending ||
      !destination_producer.valid()) {
    pending_transfers_.erase(pending);
    return ResidencyCompletionDisposition::Rejected;
  }
  const ReadyFenceState source_state = source.ready_fence().poll().state();
  if ((source_producer == nullptr && source_state != ReadyFenceState::Ready) ||
      (source_producer != nullptr &&
       (source_state != ReadyFenceState::Pending ||
        !source_producer->valid()))) {
    pending_transfers_.erase(pending);
    return ResidencyCompletionDisposition::Rejected;
  }

  const StorageBinding binding = destination.storage_binding();
  const ReplicaKey replica_key{destination.revision_id().value(),
                               binding.device, binding.memory_domain};
  std::map<ReplicaKey, Value> staged_replica;
  staged_replica.emplace(replica_key, destination);
  if (source_producer != nullptr && !source_producer->complete_ready()) {
    pending_transfers_.erase(pending);
    return ResidencyCompletionDisposition::Rejected;
  }
  if (!destination_producer.complete_ready()) {
    pending_transfers_.erase(pending);
    return ResidencyCompletionDisposition::Rejected;
  }
  const auto resident = resident_values_.find(replica_key);
  if (resident == resident_values_.end()) {
    resident_values_.merge(staged_replica);
    if (resident_values_.size() > resident_capacity_) {
      resident_values_.erase(resident_values_.begin());
    }
  } else {
    resident->second = destination;
  }
  pending_transfers_.erase(pending);
  return ResidencyCompletionDisposition::Published;
}

/** @copydoc ResidencyManager::discard_transfer */
bool ResidencyManager::discard_transfer(
    const DeviceCompletionIdentity& identity) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto pending =
      pending_transfers_.find(identity.destination_producer().value());
  if (pending == pending_transfers_.end() || !(pending->second == identity)) {
    return false;
  }
  pending_transfers_.erase(pending);
  return true;
}

/** @copydoc ResidencyManager::find */
std::optional<Value> ResidencyManager::find(ValueRevisionId revision,
                                            DeviceId device,
                                            MemoryDomain memory_domain) const {
  if (!revision.valid()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto resident = resident_values_.find(
      ReplicaKey{revision.value(), device, memory_domain});
  if (resident == resident_values_.end()) {
    return std::nullopt;
  }
  return resident->second;
}

}  // namespace ps::execution
