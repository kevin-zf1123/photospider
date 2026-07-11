#include "ps_types.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "kernel/op_registry_test_access.hpp"
#endif

namespace ps {

namespace {

/**
 * @brief Thread-local capture currently observing OpRegistry registrations.
 *
 * @note Plugin loading invokes registration on the calling thread, so a
 * thread-local pointer avoids adding process-global registry state while still
 * allowing nested callers to restore the previous capture.
 */
thread_local OpRegistry::RegistrationCapture* active_registration_capture =
    nullptr;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Per-thread identity used by the allocation-free recursive state lock.
 *
 * @note Its address is unique for each thread and requires no dynamic
 *       allocation, hashing, or platform mutex state.
 */
thread_local const unsigned char registry_state_lock_token = 0;

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Optional counter observing genuine cross-thread registry contention.
 *
 * @note The pointer is installed only by deterministic tests. A null value is
 *       the production-equivalent state and adds no callback or allocation to
 *       the lock path.
 */
std::atomic<std::atomic<std::uint64_t>*> registry_contention_counter{nullptr};

/** @brief Short internal name for the precise registration failpoint enum. */
using RegistrationFailpoint = testing::OpRegistryDeviceRegistrationFailpoint;

/** @brief Thread-local precise device-registration boundary selected by tests.
 */
thread_local RegistrationFailpoint device_registration_failpoint{};

/** @brief Visits to the currently armed device-registration failpoint. */
thread_local std::size_t device_registration_failpoint_hit_count = 0;

/** @brief Short internal name for device-wrapper retirement counters. */
using RetirementProbe = testing::OpRegistryDeviceCallbackRetirementInspection;

/** @brief Optional counters observing final host device-wrapper retirement. */
std::atomic<RetirementProbe*> device_callback_retirement_inspection{};

/**
 * @brief Throws at one exact device-registration construction boundary.
 *
 * @param failpoint Boundary entered by the real registration implementation.
 * @return Nothing.
 * @throws std::bad_alloc when `failpoint` is currently armed.
 * @note The hit count advances before throwing so tests cannot mistake an
 *       unrelated allocator failure for the requested boundary.
 */
void maybe_fail_device_registration_for_testing(
    testing::OpRegistryDeviceRegistrationFailpoint failpoint) {
  if (device_registration_failpoint != failpoint) {
    return;
  }
  ++device_registration_failpoint_hit_count;
  throw std::bad_alloc{};
}

/**
 * @brief Reports one lock attempt entering the genuine contention slow path.
 *
 * @return Nothing.
 * @throws Nothing; both pointer lookup and counter increment are atomic.
 * @note Callers invoke this at most once per lock acquisition and only after a
 *       failed compare-exchange observed a non-null owner belonging to another
 *       thread.
 */
void report_registry_contention_for_testing() noexcept {
  auto* counter = registry_contention_counter.load(std::memory_order_acquire);
  if (counter) {
    counter->fetch_add(1, std::memory_order_release);
  }
}
#endif

/**
 * @brief Sorts and deduplicates captured canonical operation keys.
 *
 * @param keys Mutable list of keys captured during one registration call.
 * @return Nothing.
 * @throws Nothing; sorting, deduplication, and vector erasure reuse existing
 *         storage.
 * @note Stable sorted output keeps plugin unload metadata deterministic.
 */
void sort_unique_keys(std::vector<std::string>& keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

/**
 * @brief Maps one implementation to an intent-specific device priority.
 *
 * @param intent Compute path selecting the HP or RT priority policy.
 * @param device Candidate implementation device.
 * @param is_tiled Whether the candidate uses the tiled callback shape.
 * @return Lower values represent higher registry selection priority.
 * @throws Nothing.
 * @note GlobalHighPrecision prefers GPU backends, then accelerator backends,
 * then CPU. RealTimeUpdate prefers tiled CPU callbacks before GPU backends,
 * preserving the existing low-latency RT policy.
 */
int implementation_device_priority(ComputeIntent intent, Device device,
                                   bool is_tiled) {
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      if (device == Device::GPU_METAL || device == Device::GPU_CUDA) {
        return 0;
      }
      if (device == Device::ASIC_NPU) {
        return 1;
      }
      return 2;
    case ComputeIntent::RealTimeUpdate:
      if (device == Device::CPU && is_tiled) {
        return 0;
      }
      if (device == Device::GPU_METAL || device == Device::GPU_CUDA) {
        return 1;
      }
      return 2;
    default:
      return 99;
  }
}

/**
 * @brief Orders two implementation candidates by registry policy.
 *
 * @param lhs Left candidate borrowed from an OpRegistry implementation vector.
 * @param rhs Right candidate borrowed from an OpRegistry implementation vector.
 * @param intent Compute path selecting the HP or RT priority policy.
 * @return True when `lhs` should sort before `rhs`.
 * @throws Nothing.
 * @note Device priority is compared before `cost_score`; equal priorities and
 * equal costs preserve `std::min_element`'s first-seen candidate selection.
 */
bool implementation_less_for_intent(const OpImplementation* lhs,
                                    const OpImplementation* rhs,
                                    ComputeIntent intent) {
  const Device lhs_device = lhs->metadata.device_preference;
  const Device rhs_device = rhs->metadata.device_preference;
  const int lhs_priority =
      implementation_device_priority(intent, lhs_device, lhs->is_tiled());
  const int rhs_priority =
      implementation_device_priority(intent, rhs_device, rhs->is_tiled());
  if (lhs_priority != rhs_priority) {
    return lhs_priority < rhs_priority;
  }
  return lhs->metadata.cost_score < rhs->metadata.cost_score;
}

/**
 * @brief Reports whether an implementation group contains no active slot.
 *
 * @param implementations Group to inspect after slot-wise retirement.
 * @return True when every callback/metadata slot is empty, the dependency flag
 *         is false, and both device snapshot/storage lists are empty.
 * @throws Nothing.
 * @note This helper lets unload erase an empty map node without destroying a
 *       plugin callback under the registry lock.
 */
bool implementation_group_is_empty(
    const OpRegistry::OpImplementations& implementations) noexcept {
  return !implementations.monolithic_hp && !implementations.tiled_hp &&
         !implementations.tiled_rt && !implementations.meta_hp &&
         !implementations.meta_rt && !implementations.dirty_propagator &&
         !implementations.forward_propagator &&
         !implementations.dependency_builder &&
         !implementations.data_dependent &&
         implementations.device_impls.empty() &&
         implementations.device_impl_slots.empty();
}

/**
 * @brief Materializes immutable device slots into independent callback values.
 *
 * @param slots Stable implementation owners retained from one coherent
 *        registry snapshot.
 * @return Device implementation values in registration order.
 * @throws std::bad_alloc if result or callback-target copying allocates.
 * @throws Any exception raised while copying a callback target.
 * @note Callers invoke this only after releasing the registry state lock. The
 *       retained owners also keep plugin libraries mapped through every copy
 *       and exceptional cleanup path.
 */
std::vector<OpImplementation> materialize_device_implementations(
    const std::vector<std::shared_ptr<const OpImplementation>>& slots) {
  std::vector<OpImplementation> result;
  result.reserve(slots.size());
  for (const auto& slot : slots) {
    if (slot) {
      result.push_back(*slot);
    }
  }
  return result;
}

/**
 * @brief Builds the legacy HP bridge for one stable monolithic CPU device slot.
 *
 * @param slot Immutable device implementation owner retained by the bridge.
 * @return Monolithic callback forwarding to the stable device value.
 * @throws std::bad_alloc if `std::function` target storage cannot allocate.
 * @note Construction occurs before registry-lock acquisition. Copying the
 *       bridge later copies only a shared owner; it never copies or moves the
 *       original stateful device callback target under the lock. Invoking the
 *       returned callback propagates the original target's exception or
 *       `std::bad_variant_access` if `slot` violates its shape precondition.
 *       The bridge does not serialize invocation against device snapshots; the
 *       callback provider owns shared-target synchronization.
 */
MonolithicOpFunc make_monolithic_device_compatibility_bridge(
    std::shared_ptr<const OpImplementation> slot) {
  return [slot = std::move(slot)](
             const Node& node,
             const std::vector<const NodeOutput*>& inputs) -> NodeOutput {
    return std::get<MonolithicOpFunc>(slot->func)(node, inputs);
  };
}

/**
 * @brief Builds the legacy HP bridge for one stable tiled CPU device slot.
 *
 * @param slot Immutable device implementation owner retained by the bridge.
 * @return Tiled callback forwarding to the stable device value.
 * @throws std::bad_alloc if `std::function` target storage cannot allocate.
 * @note Construction occurs before registry-lock acquisition. Copying the
 *       bridge later copies only a shared owner; it never copies or moves the
 *       original stateful device callback target under the lock. Invoking the
 *       returned callback propagates the original target's exception or
 *       `std::bad_variant_access` if `slot` violates its shape precondition.
 *       The bridge does not serialize invocation against device snapshots; the
 *       callback provider owns shared-target synchronization.
 */
TileOpFunc make_tiled_device_compatibility_bridge(
    std::shared_ptr<const OpImplementation> slot) {
  return [slot = std::move(slot)](const Node& node, const OutputTile& output,
                                  const std::vector<InputTile>& inputs) {
    std::get<TileOpFunc>(slot->func)(node, output, inputs);
  };
}

/**
 * @brief Checks whether a plugin-owned device token set contains one revision.
 *
 * @param owned Plugin publication revisions for appended device entries.
 * @param revision Active element revision to classify.
 * @return True when the active element was appended by that plugin.
 * @throws Nothing.
 * @note Revision comparison replaces unsafe callable-target comparison.
 */
bool owns_device_revision(const OpRegistry::RegistryEntryOwnership& owned,
                          std::uint64_t revision) noexcept {
  return revision != 0 &&
         std::find(owned.device_impls.begin(), owned.device_impls.end(),
                   revision) != owned.device_impls.end();
}

}  // namespace

/** @copydoc OpRegistry::StateLockGuard::StateLockGuard */
OpRegistry::StateLockGuard::
    StateLockGuard(  // NOLINT(whitespace/indent_namespace)
        const OpRegistry& registry) noexcept
    : registry_(registry) {  // NOLINT(whitespace/indent_namespace)
  registry_.lock_state();
}

/** @copydoc OpRegistry::StateLockGuard::~StateLockGuard */
OpRegistry::StateLockGuard::~StateLockGuard() {
  registry_.unlock_state();
}

/** @copydoc OpRegistry::lock_state */
void OpRegistry::lock_state() const noexcept {
  const void* token = &registry_state_lock_token;
  if (state_lock_owner_.load(std::memory_order_relaxed) == token) {
    ++state_lock_depth_;
    return;
  }

  const void* expected = nullptr;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  bool contention_reported = false;
#endif
  while (!state_lock_owner_.compare_exchange_weak(
      expected, token, std::memory_order_acquire, std::memory_order_relaxed)) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    if (!contention_reported && expected != nullptr && expected != token) {
      report_registry_contention_for_testing();
      contention_reported = true;
    }
#endif
    expected = nullptr;
    std::this_thread::yield();
  }
  state_lock_depth_ = 1;
}

/** @copydoc OpRegistry::unlock_state */
void OpRegistry::unlock_state() const noexcept {
  if (--state_lock_depth_ == 0) {
    state_lock_owner_.store(nullptr, std::memory_order_release);
  }
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
namespace testing {

/** @copydoc set_op_registry_device_registration_failpoint */
void set_op_registry_device_registration_failpoint(
    OpRegistryDeviceRegistrationFailpoint failpoint) noexcept {
  device_registration_failpoint = failpoint;
  device_registration_failpoint_hit_count = 0;
}

/** @copydoc op_registry_device_registration_failpoint_hits */
std::size_t op_registry_device_registration_failpoint_hits() noexcept {
  return device_registration_failpoint_hit_count;
}

/** @copydoc set_op_registry_device_callback_retirement_inspection */
void set_op_registry_device_callback_retirement_inspection(
    OpRegistryDeviceCallbackRetirementInspection* inspection) noexcept {
  device_callback_retirement_inspection.store(inspection,
                                              std::memory_order_release);
}

/** @copydoc set_op_registry_contention_counter */
void set_op_registry_contention_counter(
    std::atomic<std::uint64_t>* counter) noexcept {
  registry_contention_counter.store(counter, std::memory_order_release);
}

/** @copydoc op_registry_lock_held_by_current_thread_for_testing */
bool op_registry_lock_held_by_current_thread_for_testing(
    const OpRegistry& registry) noexcept {
  return registry.state_lock_owner_.load(std::memory_order_relaxed) ==
         &registry_state_lock_token;
}

/** @copydoc report_op_registry_device_callback_retirement_for_testing */
void report_op_registry_device_callback_retirement_for_testing(
    const OpRegistry& registry) noexcept {
  auto* inspection =
      device_callback_retirement_inspection.load(std::memory_order_acquire);
  if (!inspection) {
    return;
  }
  inspection->destructions.fetch_add(1, std::memory_order_relaxed);
  if (op_registry_lock_held_by_current_thread_for_testing(registry)) {
    inspection->destructions_under_lock.fetch_add(1, std::memory_order_release);
  }
}

/** @copydoc inspect_op_registry_device_ownership_for_testing */
OpRegistryDeviceOwnershipInspection
inspect_op_registry_device_ownership_for_testing(
    const OpRegistry& registry, const std::string& key) noexcept {
  OpRegistryDeviceOwnershipInspection inspection;
  OpRegistry::StateLockGuard lock(registry);
  const auto implementations = registry.impl_table_.find(key);
  if (implementations != registry.impl_table_.end()) {
    inspection.implementation_entry_present = true;
    inspection.implementation_count =
        implementations->second.device_impl_slots.size();
  }
  const auto ownership = registry.ownership_table_.find(key);
  if (ownership != registry.ownership_table_.end()) {
    inspection.ownership_entry_present = true;
    inspection.revision_count = ownership->second.device_impls.size();
    inspection.all_revisions_nonzero =
        std::all_of(ownership->second.device_impls.begin(),
                    ownership->second.device_impls.end(),
                    [](std::uint64_t revision) { return revision != 0; });
    if (!ownership->second.device_impls.empty()) {
      inspection.first_device_revision = ownership->second.device_impls.front();
    }
    inspection.monolithic_hp_revision = ownership->second.monolithic_hp;
    inspection.tiled_hp_revision = ownership->second.tiled_hp;
    inspection.meta_hp_revision = ownership->second.meta_hp;
  }
  return inspection;
}

}  // namespace testing
#endif

/** @copydoc OpRegistry::OpRegistry(const OpRegistry&) */
OpRegistry::OpRegistry(const OpRegistry& other) {
  StateLockGuard lock(other);
  table_ = other.table_;
  metadata_table_ = other.metadata_table_;
  impl_table_ = other.impl_table_;
  ownership_table_ = other.ownership_table_;
  next_ownership_revision_ = other.next_ownership_revision_;
}

/** @copydoc OpRegistry::instance */
OpRegistry& OpRegistry::instance() {
  static OpRegistry inst;
  return inst;
}

/** @copydoc OpRegistry::next_ownership_revision */
std::uint64_t OpRegistry::next_ownership_revision() noexcept {
  const std::uint64_t revision = next_ownership_revision_;
  ++next_ownership_revision_;
  if (next_ownership_revision_ == 0) {
    next_ownership_revision_ = 1;
  }
  return revision;
}

/** @copydoc OpRegistry::record_scalar_ownership */
void OpRegistry::record_scalar_ownership(const std::string& key,
                                         RegistryEntryOwnership& ownership,
                                         OwnershipSlot slot,
                                         std::uint64_t revision) {
  auto assign = [slot, revision](RegistryEntryOwnership& target) noexcept {
    switch (slot) {
      case OwnershipSlot::LegacyOp:
        target.legacy_op = revision;
        break;
      case OwnershipSlot::Metadata:
        target.metadata = revision;
        break;
      case OwnershipSlot::MonolithicHp:
        target.monolithic_hp = revision;
        break;
      case OwnershipSlot::TiledHp:
        target.tiled_hp = revision;
        break;
      case OwnershipSlot::TiledRt:
        target.tiled_rt = revision;
        break;
      case OwnershipSlot::MetaHp:
        target.meta_hp = revision;
        break;
      case OwnershipSlot::MetaRt:
        target.meta_rt = revision;
        break;
      case OwnershipSlot::DirtyPropagator:
        target.dirty_propagator = revision;
        break;
      case OwnershipSlot::ForwardPropagator:
        target.forward_propagator = revision;
        break;
      case OwnershipSlot::DependencyBuilder:
        target.dependency_builder = revision;
        break;
      case OwnershipSlot::DataDependent:
        target.data_dependent = revision;
        break;
    }
  };

  assign(ownership);
  if (active_registration_capture) {
    assign(active_registration_capture->owned_entries[key]);
  }
}

/** @copydoc OpRegistry::record_device_ownership */
void OpRegistry::record_device_ownership(const std::string& key,
                                         RegistryEntryOwnership& ownership,
                                         std::uint64_t revision) {
  ownership.device_impls.push_back(revision);
  if (active_registration_capture) {
    active_registration_capture->owned_entries[key].device_impls.push_back(
        revision);
  }
}

/** @copydoc OpRegistry::snapshot_entry */
OpRegistry::RegistryEntrySnapshot OpRegistry::snapshot_entry(
    const std::string& key) const {
  RegistryEntrySnapshot snapshot;
  if (auto it = table_.find(key); it != table_.end()) {
    snapshot.legacy_op = it->second;
  }
  if (auto it = metadata_table_.find(key); it != metadata_table_.end()) {
    snapshot.metadata = it->second;
  }
  if (auto it = impl_table_.find(key); it != impl_table_.end()) {
    snapshot.implementations = it->second;
  }
  if (auto it = ownership_table_.find(key); it != ownership_table_.end()) {
    snapshot.ownership = it->second;
  }
  return snapshot;
}

/** @copydoc OpRegistry::restore_entry */
void OpRegistry::restore_entry(const std::string& key,
                               const RegistryEntrySnapshot& snapshot) {
  RegistryEntrySnapshot replacement = snapshot;
  std::optional<decltype(table_)::node_type> retired_legacy;
  std::optional<decltype(metadata_table_)::node_type> retired_metadata;
  std::optional<decltype(impl_table_)::node_type> retired_implementations;
  std::optional<decltype(ownership_table_)::node_type> retired_ownership;
  const bool restore_ownership =
      snapshot.legacy_op || snapshot.metadata || snapshot.implementations;

  {
    StateLockGuard lock(*this);
    const auto restore_table = [&key](auto& table, auto& previous,
                                      auto& retired) {
      auto active = table.find(key);
      if (previous) {
        if (active != table.end()) {
          using std::swap;
          swap(active->second, *previous);
        } else {
          table.emplace(key, std::move(*previous));
        }
      } else if (active != table.end()) {
        retired.emplace(table.extract(active));
      }
    };

    restore_table(table_, replacement.legacy_op, retired_legacy);
    restore_table(metadata_table_, replacement.metadata, retired_metadata);
    restore_table(impl_table_, replacement.implementations,
                  retired_implementations);

    auto active_ownership = ownership_table_.find(key);
    if (restore_ownership) {
      if (active_ownership != ownership_table_.end()) {
        using std::swap;
        swap(active_ownership->second, replacement.ownership);
      } else {
        ownership_table_.emplace(key, std::move(replacement.ownership));
      }
    } else if (active_ownership != ownership_table_.end()) {
      retired_ownership.emplace(ownership_table_.extract(active_ownership));
    }
  }
}

/** @copydoc OpRegistry::capture_key_before_mutation */
void OpRegistry::capture_key_before_mutation(const std::string& key) {
  if (!active_registration_capture) {
    return;
  }
  if (active_registration_capture->previous_entries.count(key) == 0) {
    active_registration_capture->previous_entries.emplace(key,
                                                          snapshot_entry(key));
  }
  active_registration_capture->registered_keys.push_back(key);
}

/** @copydoc OpRegistry::capture_registration */
void OpRegistry::capture_registration(const std::function<void()>& registration,
                                      RegistrationCapture& capture) {
  capture.registered_keys.clear();
  capture.previous_entries.clear();
  capture.owned_entries.clear();

  auto* previous_capture = active_registration_capture;
  active_registration_capture = &capture;
  auto finish_capture = [&]() {
    active_registration_capture = previous_capture;
    sort_unique_keys(capture.registered_keys);
    prune_registration_capture(capture);
  };

  try {
    registration();
  } catch (...) {
    finish_capture();
    throw;
  }
  finish_capture();
}

/** @copydoc OpRegistry::prune_registration_capture */
void OpRegistry::prune_registration_capture(
    RegistrationCapture& capture) noexcept {
  for (auto& [key, previous] : capture.previous_entries) {
    const auto owned_it = capture.owned_entries.find(key);
    if (owned_it == capture.owned_entries.end()) {
      previous = RegistryEntrySnapshot{};
      continue;
    }
    const RegistryEntryOwnership& owned = owned_it->second;

    if (owned.legacy_op == 0) {
      previous.legacy_op.reset();
      previous.ownership.legacy_op = 0;
    }
    if (owned.metadata == 0) {
      previous.metadata.reset();
      previous.ownership.metadata = 0;
    }

    if (previous.implementations) {
      auto& implementations = *previous.implementations;
      if (owned.monolithic_hp == 0) {
        implementations.monolithic_hp.reset();
        previous.ownership.monolithic_hp = 0;
      }
      if (owned.tiled_hp == 0) {
        implementations.tiled_hp.reset();
        previous.ownership.tiled_hp = 0;
      }
      if (owned.tiled_rt == 0) {
        implementations.tiled_rt.reset();
        previous.ownership.tiled_rt = 0;
      }
      if (owned.meta_hp == 0) {
        implementations.meta_hp.reset();
        previous.ownership.meta_hp = 0;
      }
      if (owned.meta_rt == 0) {
        implementations.meta_rt.reset();
        previous.ownership.meta_rt = 0;
      }
      if (owned.dirty_propagator == 0) {
        implementations.dirty_propagator.reset();
        previous.ownership.dirty_propagator = 0;
      }
      if (owned.forward_propagator == 0) {
        implementations.forward_propagator.reset();
        previous.ownership.forward_propagator = 0;
      }
      if (owned.dependency_builder == 0) {
        implementations.dependency_builder.reset();
        previous.ownership.dependency_builder = 0;
      }
      if (owned.data_dependent == 0) {
        implementations.data_dependent = false;
        previous.ownership.data_dependent = 0;
      }

      // Device registration appends instead of replacing. The live vector
      // already retains every predecessor element, so no callback copy is
      // needed in the restoration snapshot.
      implementations.device_impl_slots.clear();
      implementations.device_impls.clear();
      previous.ownership.device_impls.clear();
      if (implementation_group_is_empty(implementations)) {
        previous.implementations.reset();
      }
    }
  }
}

/** @copydoc OpRegistry::restore_registration_capture */
void OpRegistry::restore_registration_capture(
    const RegistrationCapture& capture) {
  for (const auto& key : capture.registered_keys) {
    auto snapshot_it = capture.previous_entries.find(key);
    if (snapshot_it != capture.previous_entries.end()) {
      restore_entry(key, snapshot_it->second);
    }
  }
}

/** @copydoc OpRegistry::restore_entry_noexcept */
bool OpRegistry::restore_entry_noexcept(
    const std::string& key, RegistryEntrySnapshot& snapshot) noexcept {
  std::optional<decltype(ownership_table_)::node_type> retired_ownership;
  StateLockGuard lock(*this);
  static_assert(std::is_nothrow_swappable_v<OpVariant>);
  static_assert(std::is_nothrow_swappable_v<OpMetadata>);
  static_assert(std::is_nothrow_swappable_v<OpImplementations>);
  static_assert(std::is_nothrow_default_constructible_v<OpVariant>);
  static_assert(std::is_nothrow_default_constructible_v<OpMetadata>);
  static_assert(std::is_nothrow_default_constructible_v<OpImplementations>);

  bool changed = false;
  const bool restores_previous_state =
      snapshot.legacy_op || snapshot.metadata || snapshot.implementations;
  const auto restore_table = [&key, &changed](auto& table,
                                              auto& previous) noexcept {
    auto active = table.find(key);
    if (previous) {
      if (active != table.end()) {
        using std::swap;
        swap(active->second, *previous);
        changed = true;
      }
      return;
    }
    if (active != table.end()) {
      previous.emplace();
      using std::swap;
      swap(active->second, *previous);
      table.erase(active);
      changed = true;
    }
  };

  restore_table(table_, snapshot.legacy_op);
  restore_table(metadata_table_, snapshot.metadata);
  restore_table(impl_table_, snapshot.implementations);
  auto active_ownership = ownership_table_.find(key);
  if (active_ownership != ownership_table_.end()) {
    if (restores_previous_state) {
      static_assert(std::is_nothrow_swappable_v<RegistryEntryOwnership>);
      using std::swap;
      swap(active_ownership->second, snapshot.ownership);
    } else {
      retired_ownership.emplace(ownership_table_.extract(active_ownership));
    }
  }
  return changed;
}

/** @copydoc OpRegistry::classify_active_ownership */
OpRegistry::OwnershipMatch OpRegistry::classify_active_ownership(
    const std::string& key,
    const RegistryEntryOwnership& owned) const noexcept {
  StateLockGuard lock(*this);
  const auto ownership_it = ownership_table_.find(key);
  if (ownership_it == ownership_table_.end()) {
    return OwnershipMatch::None;
  }
  const RegistryEntryOwnership& active_ownership = ownership_it->second;
  bool plugin_owned = false;
  bool foreign_owned = false;
  const auto classify = [&](bool present, std::uint64_t active,
                            std::uint64_t plugin) noexcept {
    if (!present) {
      return;
    }
    if (plugin != 0 && active == plugin) {
      plugin_owned = true;
    } else {
      foreign_owned = true;
    }
  };

  classify(table_.find(key) != table_.end(), active_ownership.legacy_op,
           owned.legacy_op);
  classify(metadata_table_.find(key) != metadata_table_.end(),
           active_ownership.metadata, owned.metadata);

  const auto implementations_it = impl_table_.find(key);
  if (implementations_it != impl_table_.end()) {
    const OpImplementations& implementations = implementations_it->second;
    classify(implementations.monolithic_hp.has_value(),
             active_ownership.monolithic_hp, owned.monolithic_hp);
    classify(implementations.tiled_hp.has_value(), active_ownership.tiled_hp,
             owned.tiled_hp);
    classify(implementations.tiled_rt.has_value(), active_ownership.tiled_rt,
             owned.tiled_rt);
    classify(implementations.meta_hp.has_value(), active_ownership.meta_hp,
             owned.meta_hp);
    classify(implementations.meta_rt.has_value(), active_ownership.meta_rt,
             owned.meta_rt);
    classify(implementations.dirty_propagator.has_value(),
             active_ownership.dirty_propagator, owned.dirty_propagator);
    classify(implementations.forward_propagator.has_value(),
             active_ownership.forward_propagator, owned.forward_propagator);
    classify(implementations.dependency_builder.has_value(),
             active_ownership.dependency_builder, owned.dependency_builder);
    classify(implementations.data_dependent, active_ownership.data_dependent,
             owned.data_dependent);

    for (std::size_t index = 0;
         index < implementations.device_impl_slots.size(); ++index) {
      const std::uint64_t active = index < active_ownership.device_impls.size()
                                       ? active_ownership.device_impls[index]
                                       : 0;
      if (owns_device_revision(owned, active)) {
        plugin_owned = true;
      } else {
        foreign_owned = true;
      }
    }
  }

  if (!plugin_owned) {
    return OwnershipMatch::None;
  }
  return foreign_owned ? OwnershipMatch::Partial : OwnershipMatch::Complete;
}

/** @copydoc OpRegistry::retire_owned_entry_noexcept */
bool OpRegistry::retire_owned_entry_noexcept(
    const std::string& key, const RegistryEntryOwnership& owned,
    RegistryEntrySnapshot& previous,
    RegistryEntrySnapshot& retirement) noexcept {
  std::optional<decltype(ownership_table_)::node_type> retired_ownership;
  StateLockGuard lock(*this);
  const auto ownership_it = ownership_table_.find(key);
  if (ownership_it == ownership_table_.end()) {
    return false;
  }
  RegistryEntryOwnership& active_ownership = ownership_it->second;
  bool changed = false;

  auto active_legacy = table_.find(key);
  if (active_legacy != table_.end() && owned.legacy_op != 0 &&
      active_ownership.legacy_op == owned.legacy_op) {
    if (previous.legacy_op) {
      using std::swap;
      swap(active_legacy->second, *previous.legacy_op);
    } else {
      using std::swap;
      swap(active_legacy->second, *retirement.legacy_op);
      table_.erase(active_legacy);
    }
    active_ownership.legacy_op = previous.ownership.legacy_op;
    changed = true;
  }

  auto active_metadata = metadata_table_.find(key);
  if (active_metadata != metadata_table_.end() && owned.metadata != 0 &&
      active_ownership.metadata == owned.metadata) {
    if (previous.metadata) {
      using std::swap;
      swap(active_metadata->second, *previous.metadata);
    } else {
      using std::swap;
      swap(active_metadata->second, *retirement.metadata);
      metadata_table_.erase(active_metadata);
    }
    active_ownership.metadata = previous.ownership.metadata;
    changed = true;
  }

  auto active_implementations = impl_table_.find(key);
  if (active_implementations == impl_table_.end()) {
    if (table_.find(key) == table_.end() &&
        metadata_table_.find(key) == metadata_table_.end()) {
      retired_ownership.emplace(ownership_table_.extract(ownership_it));
    }
    return changed;
  }
  OpImplementations& active = active_implementations->second;
  OpImplementations* predecessor =
      previous.implementations ? &*previous.implementations : nullptr;
  OpImplementations& retired = *retirement.implementations;

  const auto retire_optional =
      [&](auto& active_slot, auto* previous_slot, auto& retirement_slot,
          std::uint64_t& active_revision, std::uint64_t owned_revision,
          std::uint64_t previous_revision) noexcept {
        if (!active_slot || owned_revision == 0 ||
            active_revision != owned_revision) {
          return;
        }
        if (previous_slot && *previous_slot) {
          active_slot.swap(*previous_slot);
        } else {
          active_slot.swap(retirement_slot);
        }
        active_revision = previous_revision;
        changed = true;
      };

  retire_optional(active.monolithic_hp,
                  predecessor ? &predecessor->monolithic_hp : nullptr,
                  retired.monolithic_hp, active_ownership.monolithic_hp,
                  owned.monolithic_hp, previous.ownership.monolithic_hp);
  retire_optional(active.tiled_hp,
                  predecessor ? &predecessor->tiled_hp : nullptr,
                  retired.tiled_hp, active_ownership.tiled_hp, owned.tiled_hp,
                  previous.ownership.tiled_hp);
  retire_optional(active.tiled_rt,
                  predecessor ? &predecessor->tiled_rt : nullptr,
                  retired.tiled_rt, active_ownership.tiled_rt, owned.tiled_rt,
                  previous.ownership.tiled_rt);
  retire_optional(active.meta_hp, predecessor ? &predecessor->meta_hp : nullptr,
                  retired.meta_hp, active_ownership.meta_hp, owned.meta_hp,
                  previous.ownership.meta_hp);
  retire_optional(active.meta_rt, predecessor ? &predecessor->meta_rt : nullptr,
                  retired.meta_rt, active_ownership.meta_rt, owned.meta_rt,
                  previous.ownership.meta_rt);
  retire_optional(active.dirty_propagator,
                  predecessor ? &predecessor->dirty_propagator : nullptr,
                  retired.dirty_propagator, active_ownership.dirty_propagator,
                  owned.dirty_propagator, previous.ownership.dirty_propagator);
  retire_optional(active.forward_propagator,
                  predecessor ? &predecessor->forward_propagator : nullptr,
                  retired.forward_propagator,
                  active_ownership.forward_propagator, owned.forward_propagator,
                  previous.ownership.forward_propagator);
  retire_optional(active.dependency_builder,
                  predecessor ? &predecessor->dependency_builder : nullptr,
                  retired.dependency_builder,
                  active_ownership.dependency_builder, owned.dependency_builder,
                  previous.ownership.dependency_builder);

  if (owned.data_dependent != 0 &&
      active_ownership.data_dependent == owned.data_dependent) {
    active.data_dependent = predecessor && predecessor->data_dependent;
    active_ownership.data_dependent = previous.ownership.data_dependent;
    changed = true;
  }

  static_assert(
      std::is_nothrow_swappable_v<std::shared_ptr<const OpImplementation>>);
  std::size_t write_index = 0;
  for (std::size_t read_index = 0; read_index < active.device_impl_slots.size();
       ++read_index) {
    const std::uint64_t revision =
        read_index < active_ownership.device_impls.size()
            ? active_ownership.device_impls[read_index]
            : 0;
    auto owned_token = std::find(owned.device_impls.begin(),
                                 owned.device_impls.end(), revision);
    if (revision != 0 && owned_token != owned.device_impls.end()) {
      const std::size_t retirement_index = static_cast<std::size_t>(
          std::distance(owned.device_impls.begin(), owned_token));
      using std::swap;
      swap(active.device_impl_slots[read_index],
           retired.device_impl_slots[retirement_index]);
      changed = true;
      continue;
    }
    if (write_index != read_index) {
      // Every gap was created by swapping an owned slot into retirement, so
      // the destination is empty. Swapping transfers only a shared owner and
      // cannot release or relocate either callback target under this lock.
      using std::swap;
      swap(active.device_impl_slots[write_index],
           active.device_impl_slots[read_index]);
    }
    if (write_index < active_ownership.device_impls.size()) {
      active_ownership.device_impls[write_index] = revision;
    }
    ++write_index;
  }
  // Compaction leaves only empty owners in the tail, so resize cannot release
  // a callback target or its final plugin lease while the lock is held.
  active.device_impl_slots.resize(write_index);
  active_ownership.device_impls.resize(write_index);

  if (implementation_group_is_empty(active)) {
    impl_table_.erase(active_implementations);
  }
  if (table_.find(key) == table_.end() &&
      metadata_table_.find(key) == metadata_table_.end() &&
      impl_table_.find(key) == impl_table_.end()) {
    retired_ownership.emplace(ownership_table_.extract(ownership_it));
  }
  return changed;
}

/** @copydoc OpRegistry::splice_owned_snapshot_noexcept */
void OpRegistry::splice_owned_snapshot_noexcept(
    RegistryEntrySnapshot& dependent, const RegistryEntryOwnership& owned,
    RegistryEntrySnapshot& previous,
    RegistryEntrySnapshot& retirement) noexcept {
  const auto splice_optional =
      [](auto& dependent_slot, auto& dependent_revision,
         std::uint64_t owned_revision, auto& previous_slot,
         std::uint64_t previous_revision, auto& retirement_slot) noexcept {
        if (!dependent_slot || owned_revision == 0 ||
            dependent_revision != owned_revision) {
          return;
        }
        if (previous_slot) {
          dependent_slot.swap(previous_slot);
        } else {
          dependent_slot.swap(retirement_slot);
        }
        dependent_revision = previous_revision;
      };

  splice_optional(dependent.legacy_op, dependent.ownership.legacy_op,
                  owned.legacy_op, previous.legacy_op,
                  previous.ownership.legacy_op, retirement.legacy_op);
  splice_optional(dependent.metadata, dependent.ownership.metadata,
                  owned.metadata, previous.metadata,
                  previous.ownership.metadata, retirement.metadata);

  if (!dependent.implementations) {
    return;
  }
  OpImplementations& dependent_impl = *dependent.implementations;
  OpImplementations empty_previous;
  OpImplementations& previous_impl =
      previous.implementations ? *previous.implementations : empty_previous;
  OpImplementations& retired_impl = *retirement.implementations;

  splice_optional(dependent_impl.monolithic_hp,
                  dependent.ownership.monolithic_hp, owned.monolithic_hp,
                  previous_impl.monolithic_hp, previous.ownership.monolithic_hp,
                  retired_impl.monolithic_hp);
  splice_optional(dependent_impl.tiled_hp, dependent.ownership.tiled_hp,
                  owned.tiled_hp, previous_impl.tiled_hp,
                  previous.ownership.tiled_hp, retired_impl.tiled_hp);
  splice_optional(dependent_impl.tiled_rt, dependent.ownership.tiled_rt,
                  owned.tiled_rt, previous_impl.tiled_rt,
                  previous.ownership.tiled_rt, retired_impl.tiled_rt);
  splice_optional(dependent_impl.meta_hp, dependent.ownership.meta_hp,
                  owned.meta_hp, previous_impl.meta_hp,
                  previous.ownership.meta_hp, retired_impl.meta_hp);
  splice_optional(dependent_impl.meta_rt, dependent.ownership.meta_rt,
                  owned.meta_rt, previous_impl.meta_rt,
                  previous.ownership.meta_rt, retired_impl.meta_rt);
  splice_optional(
      dependent_impl.dirty_propagator, dependent.ownership.dirty_propagator,
      owned.dirty_propagator, previous_impl.dirty_propagator,
      previous.ownership.dirty_propagator, retired_impl.dirty_propagator);
  splice_optional(
      dependent_impl.forward_propagator, dependent.ownership.forward_propagator,
      owned.forward_propagator, previous_impl.forward_propagator,
      previous.ownership.forward_propagator, retired_impl.forward_propagator);
  splice_optional(
      dependent_impl.dependency_builder, dependent.ownership.dependency_builder,
      owned.dependency_builder, previous_impl.dependency_builder,
      previous.ownership.dependency_builder, retired_impl.dependency_builder);

  if (owned.data_dependent != 0 &&
      dependent.ownership.data_dependent == owned.data_dependent) {
    dependent_impl.data_dependent =
        previous.implementations && previous_impl.data_dependent;
    dependent.ownership.data_dependent = previous.ownership.data_dependent;
  }

  // Pruned predecessor snapshots never retain device-vector entries: device
  // registration is append-only, so those predecessors remain in live state.
  if (implementation_group_is_empty(dependent_impl)) {
    dependent.implementations.reset();
  }
}

/**
 * @brief Publishes a prepared registry through allocation-free table swaps.
 * @param other Host-owned shadow registry whose complete state is exchanged.
 * @return Nothing.
 * @throws Nothing; all member containers provide noexcept swap here.
 * @note Plugin-load commit retains both relevant library lifetimes until
 * callable objects in the swapped-out registry have been destroyed.
 */
void OpRegistry::swap_state(OpRegistry& other) noexcept {
  if (this == &other) {
    return;
  }
  OpRegistry* first = this;
  OpRegistry* second = &other;
  if (std::less<OpRegistry*>{}(second, first)) {
    std::swap(first, second);
  }
  first->lock_state();
  second->lock_state();
  table_.swap(other.table_);
  metadata_table_.swap(other.metadata_table_);
  impl_table_.swap(other.impl_table_);
  ownership_table_.swap(other.ownership_table_);
  using std::swap;
  swap(next_ownership_revision_, other.next_ownership_revision_);
  second->unlock_state();
  first->unlock_state();
}

// --- 修改: 实现新的 register_op 和 get_metadata ---

/** @copydoc OpRegistry::register_op */
void OpRegistry::register_op(const std::string& type,
                             const std::string& subtype, MonolithicOpFunc fn,
                             OpMetadata meta) {
  auto key = make_key(type, subtype);
  OpVariant legacy_replacement = fn;
  std::optional<MonolithicOpFunc> hp_replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    using std::swap;
    swap(table_[key], legacy_replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::LegacyOp, revision);
    metadata_table_[key] = meta;  // 存储元数据
    record_scalar_ownership(key, ownership, OwnershipSlot::Metadata, revision);
    // Phase 1 bridge: also populate multi-impl table as HP monolithic
    OpImplementations& implementations = impl_table_[key];
    implementations.monolithic_hp.swap(hp_replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::MonolithicHp,
                            revision);
    implementations.meta_hp = meta;
    record_scalar_ownership(key, ownership, OwnershipSlot::MetaHp, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }
  }
}

/** @copydoc OpRegistry::register_op */
void OpRegistry::register_op(const std::string& type,
                             const std::string& subtype, TileOpFunc fn,
                             OpMetadata meta) {
  auto key = make_key(type, subtype);
  OpVariant legacy_replacement = fn;
  std::optional<TileOpFunc> hp_replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    if (meta.tile_preference == TileSizePreference::UNDEFINED) {
      // Tiled 操作可以不指定偏好，默认为 UNDEFINED
    }
    using std::swap;
    swap(table_[key], legacy_replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::LegacyOp, revision);
    metadata_table_[key] = meta;  // 存储元数据
    record_scalar_ownership(key, ownership, OwnershipSlot::Metadata, revision);
    // Phase 1 bridge: also populate multi-impl table as HP tiled
    OpImplementations& implementations = impl_table_[key];
    implementations.tiled_hp.swap(hp_replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::TiledHp, revision);
    implementations.meta_hp = meta;
    record_scalar_ownership(key, ownership, OwnershipSlot::MetaHp, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }
  }
}

/** @copydoc OpRegistry::find */
std::optional<OpRegistry::OpVariant> OpRegistry::find(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto it = table_.find(make_key(type, subtype));
  if (it == table_.end())
    return std::nullopt;
  return it->second;
}

/** @copydoc OpRegistry::get_metadata */
std::optional<OpMetadata> OpRegistry::get_metadata(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = metadata_table_.find(key);
  if (it != metadata_table_.end())
    return it->second;
  // Fallback to new multi-impl table
  auto it2 = impl_table_.find(key);
  if (it2 == impl_table_.end())
    return std::nullopt;
  if (it2->second.meta_hp)
    return *(it2->second.meta_hp);
  if (it2->second.meta_rt)
    return *(it2->second.meta_rt);
  return std::nullopt;
}

/** @copydoc OpRegistry::get_keys */
std::vector<std::string> OpRegistry::get_keys() const {
  StateLockGuard lock(*this);
  std::vector<std::string> keys;
  keys.reserve(table_.size() + impl_table_.size());
  for (const auto& pair : table_)
    keys.push_back(pair.first);
  for (const auto& pair : impl_table_)
    keys.push_back(pair.first);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

/** @copydoc OpRegistry::get_combined_keys */
std::vector<std::string> OpRegistry::get_combined_keys() const {
  StateLockGuard lock(*this);
  // Start with keys that have multi-impl entries (canonical combined ops)
  std::vector<std::string> combined;
  combined.reserve(impl_table_.size() + table_.size());
  for (const auto& kv : impl_table_)
    combined.push_back(kv.first);
  // Add legacy-only keys (not present in impl table)
  for (const auto& kv : table_) {
    const auto& key = kv.first;
    if (impl_table_.count(key) == 0) {
      // If this legacy key looks like an alias ending with "_tiled" and the
      // base exists, skip it
      auto pos = key.rfind(':');
      if (pos != std::string::npos) {
        std::string type = key.substr(0, pos);
        std::string subtype = key.substr(pos + 1);
        if (subtype.size() > 6 &&
            subtype.rfind("_tiled") == subtype.size() - 6) {
          std::string base_key =
              type + ":" + subtype.substr(0, subtype.size() - 6);
          if (impl_table_.count(base_key) || table_.count(base_key)) {
            continue;  // collapse alias under base op
          }
        }
      }
      combined.push_back(key);
    }
  }
  std::sort(combined.begin(), combined.end());
  combined.erase(std::unique(combined.begin(), combined.end()), combined.end());
  return combined;
}

/** @copydoc OpRegistry::unregister_op */
bool OpRegistry::unregister_op(const std::string& type,
                               const std::string& subtype) {
  return unregister_key(make_key(type, subtype));
}

/** @copydoc OpRegistry::unregister_key */
bool OpRegistry::unregister_key(const std::string& key) {
  std::optional<decltype(table_)::node_type> retired_legacy;
  std::optional<decltype(metadata_table_)::node_type> retired_metadata;
  std::optional<decltype(impl_table_)::node_type> retired_implementations;
  std::optional<decltype(ownership_table_)::node_type> retired_ownership;
  static_assert(
      std::is_nothrow_move_constructible_v<decltype(table_)::node_type> &&
      std::is_nothrow_move_constructible_v<
          decltype(metadata_table_)::node_type> &&
      std::is_nothrow_move_constructible_v<decltype(impl_table_)::node_type> &&
      std::is_nothrow_move_constructible_v<
          decltype(ownership_table_)::node_type>);

  {
    StateLockGuard lock(*this);
    if (auto entry = table_.find(key); entry != table_.end()) {
      retired_legacy.emplace(table_.extract(entry));
    }
    if (auto entry = metadata_table_.find(key);
        entry != metadata_table_.end()) {
      retired_metadata.emplace(metadata_table_.extract(entry));
    }
    if (auto entry = impl_table_.find(key); entry != impl_table_.end()) {
      retired_implementations.emplace(impl_table_.extract(entry));
    }
    if (auto entry = ownership_table_.find(key);
        entry != ownership_table_.end()) {
      retired_ownership.emplace(ownership_table_.extract(entry));
    }
  }

  return retired_legacy.has_value() || retired_metadata.has_value() ||
         retired_implementations.has_value() || retired_ownership.has_value();
}

// -------------------------
// Phase 1 scaffolding below
// -------------------------

/** @copydoc OpRegistry::register_op_hp_monolithic */
void OpRegistry::register_op_hp_monolithic(const std::string& type,
                                           const std::string& subtype,
                                           MonolithicOpFunc fn,
                                           OpMetadata meta) {
  auto key = make_key(type, subtype);
  std::optional<MonolithicOpFunc> replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    OpImplementations& implementations = impl_table_[key];
    implementations.monolithic_hp.swap(replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::MonolithicHp,
                            revision);
    implementations.meta_hp = meta;
    record_scalar_ownership(key, ownership, OwnershipSlot::MetaHp, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }
  }
}

/** @copydoc OpRegistry::register_op_hp_tiled */
void OpRegistry::register_op_hp_tiled(const std::string& type,
                                      const std::string& subtype, TileOpFunc fn,
                                      OpMetadata meta) {
  auto key = make_key(type, subtype);
  std::optional<TileOpFunc> replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    OpImplementations& implementations = impl_table_[key];
    implementations.tiled_hp.swap(replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::TiledHp, revision);
    implementations.meta_hp = meta;
    record_scalar_ownership(key, ownership, OwnershipSlot::MetaHp, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }
  }
}

/** @copydoc OpRegistry::register_op_rt_tiled */
void OpRegistry::register_op_rt_tiled(const std::string& type,
                                      const std::string& subtype, TileOpFunc fn,
                                      OpMetadata meta) {
  auto key = make_key(type, subtype);
  std::optional<TileOpFunc> replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    OpImplementations& implementations = impl_table_[key];
    implementations.tiled_rt.swap(replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::TiledRt, revision);
    implementations.meta_rt = meta;
    record_scalar_ownership(key, ownership, OwnershipSlot::MetaRt, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }
  }
}

/** @copydoc OpRegistry::register_dirty_propagator */
void OpRegistry::register_dirty_propagator(const std::string& type,
                                           const std::string& subtype,
                                           DirtyRoiPropFunc fn) {
  auto key = make_key(type, subtype);
  std::optional<DirtyRoiPropFunc> replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    impl_table_[key].dirty_propagator.swap(replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::DirtyPropagator,
                            revision);
  }
}

/** @copydoc OpRegistry::register_forward_propagator */
void OpRegistry::register_forward_propagator(const std::string& type,
                                             const std::string& subtype,
                                             ForwardRoiPropFunc fn) {
  auto key = make_key(type, subtype);
  std::optional<ForwardRoiPropFunc> replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    impl_table_[key].forward_propagator.swap(replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::ForwardPropagator,
                            revision);
  }
}

/** @copydoc OpRegistry::register_dependency_builder */
void OpRegistry::register_dependency_builder(const std::string& type,
                                             const std::string& subtype,
                                             DependencyLutBuilder fn,
                                             bool mark_data_dependent) {
  auto key = make_key(type, subtype);
  std::optional<DependencyLutBuilder> replacement(std::in_place, std::move(fn));
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    const std::uint64_t revision = next_ownership_revision();
    OpImplementations& implementations = impl_table_[key];
    implementations.dependency_builder.swap(replacement);
    record_scalar_ownership(key, ownership, OwnershipSlot::DependencyBuilder,
                            revision);
    if (mark_data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }
  }
}

/** @copydoc OpRegistry::resolve_for_intent */
std::optional<OpRegistry::OpVariant> OpRegistry::resolve_for_intent(
    const std::string& type, const std::string& subtype,
    ComputeIntent intent) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it == impl_table_.end()) {
    const auto legacy = table_.find(key);
    return legacy == table_.end() ? std::nullopt
                                  : std::optional<OpVariant>{legacy->second};
  }
  const auto& impls = it->second;
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      if (impls.monolithic_hp)
        return OpVariant{*impls.monolithic_hp};
      if (impls.tiled_hp)
        return OpVariant{*impls.tiled_hp};
      break;
    case ComputeIntent::RealTimeUpdate:
      if (impls.tiled_rt)
        return OpVariant{*impls.tiled_rt};
      if (impls.tiled_hp)
        return OpVariant{*impls.tiled_hp};
      break;
    default:
      break;
  }
  const auto legacy = table_.find(key);
  return legacy == table_.end() ? std::nullopt
                                : std::optional<OpVariant>{legacy->second};
}

/** @copydoc OpRegistry::get_dirty_propagator */
DirtyRoiPropFunc OpRegistry::get_dirty_propagator(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.dirty_propagator) {
      return *(it->second.dirty_propagator);
    }
  }
  static const DirtyRoiPropFunc kIdentity =
      [](const Node&, const cv::Rect& roi, const GraphModel&) { return roi; };
  return kIdentity;
}

/** @copydoc OpRegistry::get_forward_propagator */
ForwardRoiPropFunc OpRegistry::get_forward_propagator(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.forward_propagator) {
      return *(it->second.forward_propagator);
    }
  }
  static const ForwardRoiPropFunc kIdentity =
      [](const Node&, const cv::Rect& roi, const GraphModel&, const cv::Size&,
         const cv::Size&) { return roi; };
  return kIdentity;
}

/** @copydoc OpRegistry::dirty_propagation_contract_status */
PropagationContractStatus OpRegistry::dirty_propagation_contract_status(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end() && it->second.dirty_propagator) {
    return PropagationContractStatus::Explicit;
  }
  return PropagationContractStatus::LegacyIdentityFallback;
}

/** @copydoc OpRegistry::forward_propagation_contract_status */
PropagationContractStatus OpRegistry::forward_propagation_contract_status(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end() && it->second.forward_propagator) {
    return PropagationContractStatus::Explicit;
  }
  return PropagationContractStatus::LegacyIdentityFallback;
}

/** @copydoc OpRegistry::get_dependency_builder */
std::optional<DependencyLutBuilder> OpRegistry::get_dependency_builder(
    const std::string& type, const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.dependency_builder) {
      return it->second.dependency_builder;
    }
  }
  return std::nullopt;
}

/** @copydoc OpRegistry::is_data_dependent */
bool OpRegistry::is_data_dependent(const std::string& type,
                                   const std::string& subtype) const {
  StateLockGuard lock(*this);
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.data_dependent)
      return true;
    if (it->second.meta_hp && it->second.meta_hp->data_dependent)
      return true;
    if (it->second.meta_rt && it->second.meta_rt->data_dependent)
      return true;
  }
  auto metadata = metadata_table_.find(key);
  return metadata != metadata_table_.end() && metadata->second.data_dependent;
}

/** @copydoc OpRegistry::get_implementations */
std::optional<OpRegistry::OpImplementations> OpRegistry::get_implementations(
    const std::string& type, const std::string& subtype) const {
  std::optional<OpImplementations> result;
  {
    StateLockGuard lock(*this);
    auto key = make_key(type, subtype);
    auto it = impl_table_.find(key);
    if (it == impl_table_.end()) {
      return std::nullopt;
    }
    result = it->second;
  }

  result->device_impls =
      materialize_device_implementations(result->device_impl_slots);
  result->device_impl_slots.clear();
  return result;
}

// =============================================================================
// [M3.1 新增] 多设备实现注册与检索方法
// =============================================================================

/** @copydoc OpRegistry::register_impl */
void OpRegistry::register_impl(const std::string& type,
                               const std::string& subtype, Device device,
                               MonolithicOpFunc fn, OpMetadata meta) {
  // 设置元数据中的设备偏好
  meta.device_preference = device;
  OpImplementation impl;
  impl.func = std::move(fn);
  impl.metadata = meta;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  maybe_fail_device_registration_for_testing(
      testing::OpRegistryDeviceRegistrationFailpoint::StableOwner);
#endif
  std::shared_ptr<const OpImplementation> device_slot =
      std::make_shared<OpImplementation>(std::move(impl));
  std::optional<MonolithicOpFunc> cpu_compatibility;
  if (device == Device::CPU) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    maybe_fail_device_registration_for_testing(
        testing::OpRegistryDeviceRegistrationFailpoint::CpuCompatibilityBridge);
#endif
    cpu_compatibility.emplace(
        make_monolithic_device_compatibility_bridge(device_slot));
  }
  auto key = make_key(type, subtype);
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    OpImplementations& implementations = impl_table_[key];
    implementations.device_impl_slots.reserve(
        implementations.device_impl_slots.size() + 1);
    ownership.device_impls.reserve(ownership.device_impls.size() + 1);
    if (active_registration_capture) {
      auto& captured = active_registration_capture->owned_entries[key];
      captured.device_impls.reserve(captured.device_impls.size() + 1);
    }
    const std::uint64_t revision = next_ownership_revision();

    implementations.device_impl_slots.push_back(std::move(device_slot));
    record_device_ownership(key, ownership, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }

    // 同时更新传统表以保持向后兼容
    if (device == Device::CPU && !implementations.monolithic_hp) {
      implementations.monolithic_hp.swap(cpu_compatibility);
      record_scalar_ownership(key, ownership, OwnershipSlot::MonolithicHp,
                              revision);
      implementations.meta_hp = meta;
      record_scalar_ownership(key, ownership, OwnershipSlot::MetaHp, revision);
    }
  }
}

/** @copydoc OpRegistry::register_impl */
void OpRegistry::register_impl(const std::string& type,
                               const std::string& subtype, Device device,
                               TileOpFunc fn, OpMetadata meta) {
  // 设置元数据中的设备偏好
  meta.device_preference = device;
  OpImplementation impl;
  impl.func = std::move(fn);
  impl.metadata = meta;
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  maybe_fail_device_registration_for_testing(
      testing::OpRegistryDeviceRegistrationFailpoint::StableOwner);
#endif
  std::shared_ptr<const OpImplementation> device_slot =
      std::make_shared<OpImplementation>(std::move(impl));
  std::optional<TileOpFunc> cpu_compatibility;
  if (device == Device::CPU) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    maybe_fail_device_registration_for_testing(
        testing::OpRegistryDeviceRegistrationFailpoint::CpuCompatibilityBridge);
#endif
    cpu_compatibility.emplace(
        make_tiled_device_compatibility_bridge(device_slot));
  }
  auto key = make_key(type, subtype);
  {
    StateLockGuard lock(*this);
    capture_key_before_mutation(key);
    RegistryEntryOwnership& ownership = ownership_table_[key];
    OpImplementations& implementations = impl_table_[key];
    implementations.device_impl_slots.reserve(
        implementations.device_impl_slots.size() + 1);
    ownership.device_impls.reserve(ownership.device_impls.size() + 1);
    if (active_registration_capture) {
      auto& captured = active_registration_capture->owned_entries[key];
      captured.device_impls.reserve(captured.device_impls.size() + 1);
    }
    const std::uint64_t revision = next_ownership_revision();

    implementations.device_impl_slots.push_back(std::move(device_slot));
    record_device_ownership(key, ownership, revision);
    if (meta.data_dependent) {
      implementations.data_dependent = true;
      record_scalar_ownership(key, ownership, OwnershipSlot::DataDependent,
                              revision);
    }

    // 同时更新传统表以保持向后兼容
    if (device == Device::CPU && !implementations.tiled_hp) {
      implementations.tiled_hp.swap(cpu_compatibility);
      record_scalar_ownership(key, ownership, OwnershipSlot::TiledHp, revision);
      implementations.meta_hp = meta;
      record_scalar_ownership(key, ownership, OwnershipSlot::MetaHp, revision);
    }
  }
}

/** @copydoc OpRegistry::get_implementations_by_device */
std::vector<OpImplementation> OpRegistry::get_implementations_by_device(
    const std::string& type, const std::string& subtype, Device device) const {
  std::vector<std::shared_ptr<const OpImplementation>> slots;
  {
    StateLockGuard lock(*this);
    auto key = make_key(type, subtype);
    auto it = impl_table_.find(key);
    if (it == impl_table_.end()) {
      return {};
    }
    slots = it->second.device_impl_slots;
  }

  std::vector<OpImplementation> result;
  result.reserve(slots.size());
  for (const auto& slot : slots) {
    if (!slot) {
      continue;
    }
    const OpImplementation& impl = *slot;
    if (impl.metadata.device_preference == device) {
      result.push_back(impl);
    }
  }
  return result;
}

/** @copydoc OpRegistry::get_all_implementations */
std::vector<OpImplementation> OpRegistry::get_all_implementations(
    const std::string& type, const std::string& subtype) const {
  std::vector<std::shared_ptr<const OpImplementation>> slots;
  {
    StateLockGuard lock(*this);
    auto key = make_key(type, subtype);
    auto it = impl_table_.find(key);
    if (it == impl_table_.end()) {
      return {};
    }
    slots = it->second.device_impl_slots;
  }
  return materialize_device_implementations(slots);
}

/** @copydoc OpRegistry::select_best_implementation */
std::optional<OpImplementation> OpRegistry::select_best_implementation(
    const std::string& type, const std::string& subtype,
    const std::vector<Device>& available_devices, ComputeIntent intent) const {
  return select_best_implementation(
      type, subtype, available_devices, intent,
      std::function<bool(const OpImplementation&)>{});
}

/** @copydoc OpRegistry::select_best_implementation */
std::optional<OpImplementation> OpRegistry::select_best_implementation(
    const std::string& type, const std::string& subtype,
    const std::vector<Device>& available_devices, ComputeIntent intent,
    const std::function<bool(const OpImplementation&)>& candidate_filter)
    const {  // NOLINT(whitespace/indent_namespace)
  const auto implementations = get_all_implementations(type, subtype);

  // Filter a stable copied snapshot outside the registry lock. This lets a
  // caller perform read-only registry inspection without self-deadlocking and
  // ensures concurrent unload cannot invalidate callback storage.
  std::vector<const OpImplementation*> candidates;
  candidates.reserve(implementations.size());
  for (const auto& impl : implementations) {
    Device impl_device = impl.metadata.device_preference;
    // 检查实现的设备是否在可用设备列表中
    if (std::find(available_devices.begin(), available_devices.end(),
                  impl_device) != available_devices.end()) {
      if (candidate_filter && !candidate_filter(impl)) {
        continue;
      }
      candidates.push_back(&impl);
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  // 找出最优实现
  auto best_it = std::min_element(
      candidates.begin(), candidates.end(),
      [intent](const OpImplementation* lhs, const OpImplementation* rhs) {
        return implementation_less_for_intent(lhs, rhs, intent);
      });
  return **best_it;
}

}  // namespace ps
