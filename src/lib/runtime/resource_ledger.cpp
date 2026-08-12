/**
 * @file resource_ledger.cpp
 * @brief Implements checked transactional resource admission and RAII grants.
 */

#include "runtime/resource_ledger.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Adds one unsigned dimension without wraparound.
 * @param lhs First dimension.
 * @param rhs Second dimension.
 * @param output Receives the exact sum on success.
 * @return True on exact addition, false on overflow.
 * @throws Nothing.
 */
bool checked_add_dimension(std::uint64_t lhs, std::uint64_t rhs,
                           std::uint64_t* output) noexcept {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return false;
  }
  *output = lhs + rhs;
  return true;
}

/**
 * @brief Multiplies one unsigned dimension without wraparound.
 * @param value Per-unit dimension.
 * @param count Unit count.
 * @param output Receives the exact product on success.
 * @return True on exact multiplication, false on overflow.
 * @throws Nothing.
 */
bool checked_multiply_dimension(std::uint64_t value, std::uint64_t count,
                                std::uint64_t* output) noexcept {
  if (value != 0U &&
      count > std::numeric_limits<std::uint64_t>::max() / value) {
    return false;
  }
  *output = value * count;
  return true;
}

/**
 * @brief Subtracts one known-fitting resource vector.
 * @param lhs Vector containing at least every `rhs` dimension.
 * @param rhs Vector removed exactly.
 * @return Component-wise difference.
 * @throws Nothing.
 * @note Callers validate `resources_fit(rhs, lhs)` before invoking this helper.
 */
ResourceVector subtract_resources(const ResourceVector& lhs,
                                  const ResourceVector& rhs) noexcept {
  return ResourceVector{
      lhs.cpu_slots - rhs.cpu_slots,
      lhs.retained_memory_bytes - rhs.retained_memory_bytes,
      lhs.scratch_bytes - rhs.scratch_bytes,
      lhs.ready_entries - rhs.ready_entries,
      lhs.ready_bytes - rhs.ready_bytes,
  };
}

/**
 * @brief Subtracts one known-fitting device resource vector.
 * @param lhs Vector containing at least every `rhs` dimension.
 * @param rhs Vector removed exactly.
 * @return Component-wise difference.
 * @throws Nothing.
 * @note Callers validate `device_resources_fit(rhs, lhs)` first.
 */
DeviceResourceVector subtract_device_resources(
    const DeviceResourceVector& lhs, const DeviceResourceVector& rhs) noexcept {
  return DeviceResourceVector{
      lhs.device_memory_bytes - rhs.device_memory_bytes,
      lhs.device_scratch_bytes - rhs.device_scratch_bytes,
  };
}

/**
 * @brief Subtracts one known-fitting isolated-plugin resource vector.
 * @param lhs Vector containing every `rhs` dimension.
 * @param rhs Exact vector removed once.
 * @return Component-wise difference.
 * @throws Nothing.
 */
PluginResourceVector subtract_plugin_resources(
    const PluginResourceVector& lhs, const PluginResourceVector& rhs) noexcept {
  return PluginResourceVector{
      lhs.runtime_processes - rhs.runtime_processes,
      lhs.cpu_slots - rhs.cpu_slots,
      lhs.address_space_bytes - rhs.address_space_bytes,
      lhs.shared_memory_bytes - rhs.shared_memory_bytes,
      lhs.descriptor_count - rhs.descriptor_count,
  };
}

/**
 * @brief Selects the component-wise maximum of two Host resource vectors.
 * @param lhs First complete vector.
 * @param rhs Second complete vector.
 * @return Vector containing the larger value in every independent dimension.
 * @throws Nothing.
 * @note The helper performs no arithmetic and therefore cannot overflow.
 */
ResourceVector maximum_resources(const ResourceVector& lhs,
                                 const ResourceVector& rhs) noexcept {
  return ResourceVector{
      std::max(lhs.cpu_slots, rhs.cpu_slots),
      std::max(lhs.retained_memory_bytes, rhs.retained_memory_bytes),
      std::max(lhs.scratch_bytes, rhs.scratch_bytes),
      std::max(lhs.ready_entries, rhs.ready_entries),
      std::max(lhs.ready_bytes, rhs.ready_bytes),
  };
}

/**
 * @brief Selects the component-wise maximum of two device resource vectors.
 * @param lhs First complete device vector.
 * @param rhs Second complete device vector.
 * @return Vector containing both independent byte maxima.
 * @throws Nothing.
 * @note The helper performs no arithmetic and therefore cannot overflow.
 */
DeviceResourceVector maximum_device_resources(
    const DeviceResourceVector& lhs, const DeviceResourceVector& rhs) noexcept {
  return DeviceResourceVector{
      std::max(lhs.device_memory_bytes, rhs.device_memory_bytes),
      std::max(lhs.device_scratch_bytes, rhs.device_scratch_bytes),
  };
}

/**
 * @brief Selects the component-wise maximum of plugin resource vectors.
 * @param lhs First complete vector.
 * @param rhs Second complete vector.
 * @return Independent maximum for every dimension.
 * @throws Nothing.
 */
PluginResourceVector maximum_plugin_resources(
    const PluginResourceVector& lhs, const PluginResourceVector& rhs) noexcept {
  return PluginResourceVector{
      std::max(lhs.runtime_processes, rhs.runtime_processes),
      std::max(lhs.cpu_slots, rhs.cpu_slots),
      std::max(lhs.address_space_bytes, rhs.address_space_bytes),
      std::max(lhs.shared_memory_bytes, rhs.shared_memory_bytes),
      std::max(lhs.descriptor_count, rhs.descriptor_count),
  };
}

}  // namespace

/**
 * @brief Shared root capacity and current commitments.
 *
 * @throws std::system_error when mutex operations fail.
 * @note Limits never change after construction. Only the mutex-protected
 * `reserved` and monotonic `high_water` vectors are mutable.
 */
struct ResourceLedgerRootState final {
  /**
   * @brief Mutable accounting state for one immutable configured device.
   *
   * @throws Nothing for value operations.
   * @note Access is serialized by the enclosing root mutex.
   */
  struct DeviceAccount final {
    /** @brief Immutable-by-convention composition limit. */
    DeviceResourceVector limits;

    /** @brief Current planned or actual byte commitment. */
    DeviceResourceVector reserved;

    /** @brief Component-wise lifetime maximum of `reserved`. */
    DeviceResourceVector high_water;
  };

  /**
   * @brief Stores immutable composition-root limits.
   * @param configured_limits Exact maximum vector.
   * @param configured_device_limits Per-device non-CPU limits.
   * @throws std::invalid_argument for CPU or duplicate device identities.
   * @throws std::bad_alloc when the deterministic account index cannot grow.
   */
  ResourceLedgerRootState(
      ResourceVector configured_limits,
      const std::vector<DeviceResourceLimit>& configured_device_limits,
      PluginResourceVector configured_plugin_limits)
      : limits(configured_limits), plugin_limits(configured_plugin_limits) {
    for (const DeviceResourceLimit& configured : configured_device_limits) {
      if (configured.device.backend() == DeviceBackend::CPU) {
        throw std::invalid_argument(
            "ResourceLedger device limits must not contain CPU.");
      }
      const auto [unused, inserted] = devices.emplace(
          configured.device, DeviceAccount{configured.resources, {}, {}});
      static_cast<void>(unused);
      if (!inserted) {
        throw std::invalid_argument(
            "ResourceLedger device limits contain a duplicate DeviceId.");
      }
    }
  }

  /** @brief Serializes every root commit and release. */
  mutable std::mutex mutex;

  /** @brief Immutable composition-root limits. */
  const ResourceVector limits;

  /** @brief Current vector committed by root reservations. */
  ResourceVector reserved;

  /** @brief Component-wise lifetime maximum of `reserved`. */
  ResourceVector high_water;

  /** @brief Deterministically ordered configured device accounts. */
  std::map<DeviceId, DeviceAccount> devices;

  /** @brief Immutable aggregate isolated-plugin limits. */
  const PluginResourceVector plugin_limits;

  /** @brief Current token-issued or lease-consumed plugin commitment. */
  PluginResourceVector plugin_reserved;

  /** @brief Component-wise lifetime plugin commitment maximum. */
  PluginResourceVector plugin_high_water;

  /** @brief Attempt-lifetime replay tombstones ordered by digest bytes. */
  std::set<PluginInvocationIdentityDigest> plugin_replay_identities;
};

/**
 * @brief Shared child-accounting state for one root reservation.
 *
 * @throws std::system_error when mutex operations fail.
 * @note `root` keeps the host authority state alive through deferred release.
 */
struct ResourceReservationState final {
  /**
   * @brief Creates a live parent state after root commit staging.
   * @param root_state Matching ledger root.
   * @param committed_resources Exact root reservation vector.
   * @param observer Optional non-authoritative exact-release observer.
   * @param settlement_observer Optional non-owning post-release callback.
   * @throws Nothing after shared owner construction.
   */
  ResourceReservationState(
      std::shared_ptr<ResourceLedgerRootState> root_state,
      ResourceVector committed_resources,
      std::shared_ptr<ResourceLedger::ReservationReleaseObserver> observer =
          nullptr,
      ResourceLedger::ReservationSettlementObserver settlement_observer =
          {}) noexcept
      : root(std::move(root_state)),
        committed(committed_resources),
        release_observer(std::move(observer)),
        settlement_observer(settlement_observer) {}

  /** @brief Serializes parent closure and child accounting. */
  mutable std::mutex mutex;

  /** @brief Root authority retained until exact release. */
  std::shared_ptr<ResourceLedgerRootState> root;

  /** @brief Exact root vector owned by this state. */
  const ResourceVector committed;

  /** @brief Exact vector currently held by live child grants. */
  ResourceVector granted;

  /**
   * @brief Optional non-authoritative owner notified after exact root release.
   */
  std::shared_ptr<ResourceLedger::ReservationReleaseObserver> release_observer;

  /**
   * @brief Non-owning callback published after exact root/quota settlement.
   */
  const ResourceLedger::ReservationSettlementObserver settlement_observer;

  /** @brief Number of live child owner values. */
  std::uint64_t child_count = 0U;

  /** @brief True while the `Reservation` value can mint children. */
  bool parent_active = true;

  /** @brief Guards the one root release across parent/child paths. */
  bool root_released = false;
};

namespace {

/**
 * @brief Returns one exact vector to the root ledger.
 * @param root Matching shared root state.
 * @param resources Previously committed vector.
 * @param observer Optional companion-accounting observer.
 * @param settlement_observer Optional non-owning final settlement callback.
 * @return Nothing.
 * @throws Nothing; invariant or synchronization failure terminates.
 * @note With an observer, its transaction mutex spans root release and the
 * callback. This preserves one lock order for admission and release while the
 * ledger remains the only physical-capacity authority.
 */
void release_root_resources(
    const std::shared_ptr<ResourceLedgerRootState>& root,
    const ResourceVector& resources,
    const std::shared_ptr<ResourceLedger::ReservationReleaseObserver>& observer,
    ResourceLedger::ReservationSettlementObserver
        settlement_observer) noexcept {
  try {
    std::unique_lock<std::mutex> transaction_lock;
    if (observer) {
      transaction_lock =
          std::unique_lock<std::mutex>(observer->release_transaction_mutex());
    }
    {
      std::lock_guard<std::mutex> lock(root->mutex);
      if (!resources_fit(resources, root->reserved)) {
        std::terminate();
      }
      root->reserved = subtract_resources(root->reserved, resources);
    }
    if (observer) {
      observer->on_reservation_released(resources);
    }
    if (settlement_observer.valid()) {
      settlement_observer.on_settled(settlement_observer.context);
    }
  } catch (...) {
    std::terminate();
  }
}

/**
 * @brief Returns one exact plugin resource vector to its root account.
 * @param root Matching ledger authority.
 * @param resources Previously issued exact vector.
 * @return Nothing.
 * @throws Nothing; invariant or synchronization failure terminates.
 * @note Replay tombstones are deliberately preserved for the root lifetime.
 */
void release_plugin_resources(
    const std::shared_ptr<ResourceLedgerRootState>& root,
    const PluginResourceVector& resources) noexcept {
  try {
    std::lock_guard<std::mutex> lock(root->mutex);
    if (!plugin_resources_fit(resources, root->plugin_reserved)) {
      std::terminate();
    }
    root->plugin_reserved =
        subtract_plugin_resources(root->plugin_reserved, resources);
  } catch (...) {
    std::terminate();
  }
}

}  // namespace

/** @copydoc ResourceLedger::reservation_state_retained_memory_bytes */
std::uint64_t
ResourceLedger::reservation_state_retained_memory_bytes() noexcept {
  constexpr std::uint64_t kSharedControlBytes =
      2U * static_cast<std::uint64_t>(sizeof(void*));
  return static_cast<std::uint64_t>(sizeof(ResourceReservationState)) +
         kSharedControlBytes;
}

/** @copydoc operator==(const ResourceVector&, const ResourceVector&) */
bool operator==(const ResourceVector& lhs, const ResourceVector& rhs) noexcept {
  return lhs.cpu_slots == rhs.cpu_slots &&
         lhs.retained_memory_bytes == rhs.retained_memory_bytes &&
         lhs.scratch_bytes == rhs.scratch_bytes &&
         lhs.ready_entries == rhs.ready_entries &&
         lhs.ready_bytes == rhs.ready_bytes;
}

/** @copydoc operator!=(const ResourceVector&, const ResourceVector&) */
bool operator!=(const ResourceVector& lhs, const ResourceVector& rhs) noexcept {
  return !(lhs == rhs);
}

/** @copydoc checked_add_resources */
std::optional<ResourceVector> checked_add_resources(
    const ResourceVector& lhs, const ResourceVector& rhs) noexcept {
  ResourceVector sum;
  if (!checked_add_dimension(lhs.cpu_slots, rhs.cpu_slots, &sum.cpu_slots) ||
      !checked_add_dimension(lhs.retained_memory_bytes,
                             rhs.retained_memory_bytes,
                             &sum.retained_memory_bytes) ||
      !checked_add_dimension(lhs.scratch_bytes, rhs.scratch_bytes,
                             &sum.scratch_bytes) ||
      !checked_add_dimension(lhs.ready_entries, rhs.ready_entries,
                             &sum.ready_entries) ||
      !checked_add_dimension(lhs.ready_bytes, rhs.ready_bytes,
                             &sum.ready_bytes)) {
    return std::nullopt;
  }
  return sum;
}

/** @copydoc checked_multiply_resources */
std::optional<ResourceVector> checked_multiply_resources(
    const ResourceVector& resources, std::uint64_t count) noexcept {
  ResourceVector product;
  if (!checked_multiply_dimension(resources.cpu_slots, count,
                                  &product.cpu_slots) ||
      !checked_multiply_dimension(resources.retained_memory_bytes, count,
                                  &product.retained_memory_bytes) ||
      !checked_multiply_dimension(resources.scratch_bytes, count,
                                  &product.scratch_bytes) ||
      !checked_multiply_dimension(resources.ready_entries, count,
                                  &product.ready_entries) ||
      !checked_multiply_dimension(resources.ready_bytes, count,
                                  &product.ready_bytes)) {
    return std::nullopt;
  }
  return product;
}

/** @copydoc resources_fit */
bool resources_fit(const ResourceVector& requested,
                   const ResourceVector& available) noexcept {
  return requested.cpu_slots <= available.cpu_slots &&
         requested.retained_memory_bytes <= available.retained_memory_bytes &&
         requested.scratch_bytes <= available.scratch_bytes &&
         requested.ready_entries <= available.ready_entries &&
         requested.ready_bytes <= available.ready_bytes;
}

/** @copydoc operator==(const DeviceResourceVector&, const
 * DeviceResourceVector&) */
bool operator==(const DeviceResourceVector& lhs,
                const DeviceResourceVector& rhs) noexcept {
  return lhs.device_memory_bytes == rhs.device_memory_bytes &&
         lhs.device_scratch_bytes == rhs.device_scratch_bytes;
}

/** @copydoc operator!=(const DeviceResourceVector&, const
 * DeviceResourceVector&) */
bool operator!=(const DeviceResourceVector& lhs,
                const DeviceResourceVector& rhs) noexcept {
  return !(lhs == rhs);
}

/** @copydoc checked_add_device_resources */
std::optional<DeviceResourceVector> checked_add_device_resources(
    const DeviceResourceVector& lhs, const DeviceResourceVector& rhs) noexcept {
  DeviceResourceVector sum;
  if (!checked_add_dimension(lhs.device_memory_bytes, rhs.device_memory_bytes,
                             &sum.device_memory_bytes) ||
      !checked_add_dimension(lhs.device_scratch_bytes, rhs.device_scratch_bytes,
                             &sum.device_scratch_bytes)) {
    return std::nullopt;
  }
  return sum;
}

/** @copydoc device_resources_fit */
bool device_resources_fit(const DeviceResourceVector& requested,
                          const DeviceResourceVector& available) noexcept {
  return requested.device_memory_bytes <= available.device_memory_bytes &&
         requested.device_scratch_bytes <= available.device_scratch_bytes;
}

/** @copydoc operator==(const PluginResourceVector&, const
 * PluginResourceVector&) */
bool operator==(const PluginResourceVector& lhs,
                const PluginResourceVector& rhs) noexcept {
  return lhs.runtime_processes == rhs.runtime_processes &&
         lhs.cpu_slots == rhs.cpu_slots &&
         lhs.address_space_bytes == rhs.address_space_bytes &&
         lhs.shared_memory_bytes == rhs.shared_memory_bytes &&
         lhs.descriptor_count == rhs.descriptor_count;
}

/** @copydoc operator!=(const PluginResourceVector&, const
 * PluginResourceVector&) */
bool operator!=(const PluginResourceVector& lhs,
                const PluginResourceVector& rhs) noexcept {
  return !(lhs == rhs);
}

/** @copydoc checked_add_plugin_resources */
std::optional<PluginResourceVector> checked_add_plugin_resources(
    const PluginResourceVector& lhs, const PluginResourceVector& rhs) noexcept {
  PluginResourceVector sum;
  if (!checked_add_dimension(lhs.runtime_processes, rhs.runtime_processes,
                             &sum.runtime_processes) ||
      !checked_add_dimension(lhs.cpu_slots, rhs.cpu_slots, &sum.cpu_slots) ||
      !checked_add_dimension(lhs.address_space_bytes, rhs.address_space_bytes,
                             &sum.address_space_bytes) ||
      !checked_add_dimension(lhs.shared_memory_bytes, rhs.shared_memory_bytes,
                             &sum.shared_memory_bytes) ||
      !checked_add_dimension(lhs.descriptor_count, rhs.descriptor_count,
                             &sum.descriptor_count)) {
    return std::nullopt;
  }
  return sum;
}

/** @copydoc plugin_resources_fit */
bool plugin_resources_fit(const PluginResourceVector& requested,
                          const PluginResourceVector& available) noexcept {
  return requested.runtime_processes <= available.runtime_processes &&
         requested.cpu_slots <= available.cpu_slots &&
         requested.address_space_bytes <= available.address_space_bytes &&
         requested.shared_memory_bytes <= available.shared_memory_bytes &&
         requested.descriptor_count <= available.descriptor_count;
}

/** @copydoc PluginResourceAdmissionError::PluginResourceAdmissionError */
PluginResourceAdmissionError::PluginResourceAdmissionError(
    PluginResourceAdmissionErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

/** @copydoc DeviceResourceError::DeviceResourceError */
DeviceResourceError::DeviceResourceError(DeviceResourceErrorCode code,
                                         DeviceId device,
                                         DeviceResourceVector planned,
                                         DeviceResourceVector actual,
                                         std::string message)
    : std::runtime_error(std::move(message)),
      code_(code),
      device_(device),
      planned_(planned),
      actual_(actual) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ResourceLedger::DeviceReservation::DeviceReservation */
ResourceLedger::DeviceReservation::DeviceReservation(
    std::shared_ptr<ResourceLedgerRootState> root, DeviceId device,
    DeviceResourceVector planned) noexcept
    : root_(std::move(root)),  // NOLINT(whitespace/indent_namespace)
      device_(device),         // NOLINT(whitespace/indent_namespace)
      planned_(planned) {}     // NOLINT(whitespace/indent_namespace)

/** @copydoc ResourceLedger::DeviceReservation::DeviceReservation */
ResourceLedger::DeviceReservation::DeviceReservation(
    DeviceReservation&& other) noexcept
    : root_(std::move(other.root_)),  // NOLINT(whitespace/indent_namespace)
      device_(other.device_),         // NOLINT(whitespace/indent_namespace)
      planned_(other.planned_) {      // NOLINT(whitespace/indent_namespace)
  other.device_ = DeviceId(DeviceBackend::CPU);
  other.planned_ = {};
}

/** @copydoc ResourceLedger::DeviceReservation::operator= */
ResourceLedger::DeviceReservation& ResourceLedger::DeviceReservation::operator=(
    DeviceReservation&& other) noexcept {
  if (this != &other) {
    reset();
    root_ = std::move(other.root_);
    device_ = other.device_;
    planned_ = other.planned_;
    other.device_ = DeviceId(DeviceBackend::CPU);
    other.planned_ = {};
  }
  return *this;
}

/** @copydoc ResourceLedger::DeviceReservation::~DeviceReservation */
ResourceLedger::DeviceReservation::~DeviceReservation() noexcept {
  reset();
}

/** @copydoc ResourceLedger::DeviceReservation::active */
bool ResourceLedger::DeviceReservation::active() const noexcept {
  return root_ != nullptr;
}

/** @copydoc ResourceLedger::DeviceReservation::device */
DeviceId ResourceLedger::DeviceReservation::device() const noexcept {
  return device_;
}

/** @copydoc ResourceLedger::DeviceReservation::planned_resources */
DeviceResourceVector ResourceLedger::DeviceReservation::planned_resources()
    const noexcept {
  return planned_;
}

/** @copydoc ResourceLedger::DeviceReservation::commit_actual */
ResourceLedger::DeviceLeasePair
ResourceLedger::DeviceReservation::commit_actual(DeviceResourceVector actual) {
  if (!root_) {
    throw std::logic_error(
        "Cannot commit actual bytes through an inactive device reservation.");
  }
  if (!device_resources_fit(actual, planned_)) {
    throw DeviceResourceError(
        DeviceResourceErrorCode::ActualExceedsReservation, device_, planned_,
        actual, "Native actual device bytes exceed the admitted plan.");
  }

  const DeviceResourceVector persistent_resources{actual.device_memory_bytes,
                                                  0U};
  const DeviceResourceVector scratch_resources{0U, actual.device_scratch_bytes};

  {
    std::lock_guard<std::mutex> lock(root_->mutex);
    const auto account = root_->devices.find(device_);
    if (account == root_->devices.end() ||
        !device_resources_fit(planned_, account->second.reserved)) {
      throw std::logic_error(
          "Device reservation no longer matches its ledger account.");
    }
    const DeviceResourceVector unused =
        subtract_device_resources(planned_, actual);
    account->second.reserved =
        subtract_device_resources(account->second.reserved, unused);
  }

  DeviceLease persistent =
      persistent_resources == DeviceResourceVector{}
          ? DeviceLease()
          : DeviceLease(root_, device_, persistent_resources);
  DeviceLease scratch = scratch_resources == DeviceResourceVector{}
                            ? DeviceLease()
                            : DeviceLease(root_, device_, scratch_resources);
  root_.reset();
  device_ = DeviceId(DeviceBackend::CPU);
  planned_ = {};
  return DeviceLeasePair{std::move(persistent), std::move(scratch)};
}

/** @copydoc ResourceLedger::DeviceReservation::reset */
void ResourceLedger::DeviceReservation::reset() noexcept {
  if (!root_) {
    return;
  }

  std::shared_ptr<ResourceLedgerRootState> root = std::move(root_);
  const DeviceId device = device_;
  const DeviceResourceVector planned = planned_;
  device_ = DeviceId(DeviceBackend::CPU);
  planned_ = {};
  try {
    std::lock_guard<std::mutex> lock(root->mutex);
    const auto account = root->devices.find(device);
    if (account == root->devices.end() ||
        !device_resources_fit(planned, account->second.reserved)) {
      std::terminate();
    }
    account->second.reserved =
        subtract_device_resources(account->second.reserved, planned);
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ResourceLedger::DeviceLease::DeviceLease */
ResourceLedger::DeviceLease::DeviceLease(
    std::shared_ptr<ResourceLedgerRootState> root, DeviceId device,
    DeviceResourceVector resources) noexcept
    : root_(std::move(root)),   // NOLINT(whitespace/indent_namespace)
      device_(device),          // NOLINT(whitespace/indent_namespace)
      resources_(resources) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ResourceLedger::DeviceLease::DeviceLease */
ResourceLedger::DeviceLease::DeviceLease(DeviceLease&& other) noexcept
    : root_(std::move(other.root_)),  // NOLINT(whitespace/indent_namespace)
      device_(other.device_),         // NOLINT(whitespace/indent_namespace)
      resources_(other.resources_) {  // NOLINT(whitespace/indent_namespace)
  other.device_ = DeviceId(DeviceBackend::CPU);
  other.resources_ = {};
}

/** @copydoc ResourceLedger::DeviceLease::operator= */
ResourceLedger::DeviceLease& ResourceLedger::DeviceLease::operator=(
    DeviceLease&& other) noexcept {
  if (this != &other) {
    reset();
    root_ = std::move(other.root_);
    device_ = other.device_;
    resources_ = other.resources_;
    other.device_ = DeviceId(DeviceBackend::CPU);
    other.resources_ = {};
  }
  return *this;
}

/** @copydoc ResourceLedger::DeviceLease::~DeviceLease */
ResourceLedger::DeviceLease::~DeviceLease() noexcept {
  reset();
}

/** @copydoc ResourceLedger::DeviceLease::active */
bool ResourceLedger::DeviceLease::active() const noexcept {
  return root_ != nullptr;
}

/** @copydoc ResourceLedger::DeviceLease::device */
DeviceId ResourceLedger::DeviceLease::device() const noexcept {
  return device_;
}

/** @copydoc ResourceLedger::DeviceLease::resources */
DeviceResourceVector ResourceLedger::DeviceLease::resources() const noexcept {
  return resources_;
}

/** @copydoc ResourceLedger::DeviceLease::reset */
void ResourceLedger::DeviceLease::reset() noexcept {
  if (!root_) {
    return;
  }

  std::shared_ptr<ResourceLedgerRootState> root = std::move(root_);
  const DeviceId device = device_;
  const DeviceResourceVector resources = resources_;
  device_ = DeviceId(DeviceBackend::CPU);
  resources_ = {};
  try {
    std::lock_guard<std::mutex> lock(root->mutex);
    const auto account = root->devices.find(device);
    if (account == root->devices.end() ||
        !device_resources_fit(resources, account->second.reserved)) {
      std::terminate();
    }
    account->second.reserved =
        subtract_device_resources(account->second.reserved, resources);
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ResourceLedger::Reservation::Reservation */
ResourceLedger::Reservation::Reservation(
    std::shared_ptr<ResourceReservationState> state) noexcept
    : state_(std::move(state)) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ResourceLedger::Reservation::Reservation */
ResourceLedger::Reservation::Reservation(Reservation&& other) noexcept
    : state_(std::move(other.state_)) {  // NOLINT(whitespace/indent_namespace)
}

/** @copydoc ResourceLedger::Reservation::operator= */
ResourceLedger::Reservation& ResourceLedger::Reservation::operator=(
    Reservation&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc ResourceLedger::Reservation::~Reservation */
ResourceLedger::Reservation::~Reservation() noexcept {
  reset();
}

/** @copydoc ResourceLedger::Reservation::active */
bool ResourceLedger::Reservation::active() const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->parent_active;
}

/** @copydoc ResourceLedger::Reservation::resources */
ResourceVector ResourceLedger::Reservation::resources() const {
  if (!state_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->committed;
}

/** @copydoc ResourceLedger::Reservation::available */
ResourceVector ResourceLedger::Reservation::available() const {
  if (!state_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return subtract_resources(state_->committed, state_->granted);
}

/** @copydoc ResourceLedger::Reservation::try_grant */
std::optional<ResourceLedger::Grant> ResourceLedger::Reservation::try_grant(
    const ResourceVector& requested) {
  if (!state_) {
    return std::nullopt;
  }

  const std::shared_ptr<ResourceReservationState> staged_state = state_;
  std::lock_guard<std::mutex> lock(staged_state->mutex);
  if (!staged_state->parent_active) {
    return std::nullopt;
  }
  const ResourceVector available =
      subtract_resources(staged_state->committed, staged_state->granted);
  if (!resources_fit(requested, available) ||
      staged_state->child_count == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  const std::optional<ResourceVector> next_granted =
      checked_add_resources(staged_state->granted, requested);
  if (!next_granted.has_value()) {
    return std::nullopt;
  }

  Grant grant(staged_state, requested);
  staged_state->granted = *next_granted;
  ++staged_state->child_count;
  if (staged_state->settlement_observer.observes_children()) {
    staged_state->settlement_observer.on_child_granted(
        staged_state->settlement_observer.context);
  }
  return grant;
}

/** @copydoc ResourceLedger::Reservation::reset */
void ResourceLedger::Reservation::reset() noexcept {
  if (!state_) {
    return;
  }

  std::shared_ptr<ResourceReservationState> state = std::move(state_);
  bool release_root = false;
  try {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->parent_active) {
        std::terminate();
      }
      state->parent_active = false;
      if (state->child_count == 0U) {
        if (state->root_released || state->granted != ResourceVector{}) {
          std::terminate();
        }
        state->root_released = true;
        release_root = true;
      }
    }
  } catch (...) {
    std::terminate();
  }
  if (release_root) {
    release_root_resources(state->root, state->committed,
                           state->release_observer, state->settlement_observer);
  }
}

/** @copydoc ResourceLedger::Grant::Grant */
ResourceLedger::Grant::Grant(std::shared_ptr<ResourceReservationState> state,
                             ResourceVector resources) noexcept
    : state_(std::move(state)),  // NOLINT(whitespace/indent_namespace)
      resources_(resources) {}   // NOLINT(whitespace/indent_namespace)

/** @copydoc ResourceLedger::Grant::Grant */
ResourceLedger::Grant::Grant(Grant&& other) noexcept
    : state_(std::move(other.state_)),  // NOLINT(whitespace/indent_namespace)
      resources_(other.resources_) {    // NOLINT(whitespace/indent_namespace)
  other.resources_ = {};
}

/** @copydoc ResourceLedger::Grant::operator= */
ResourceLedger::Grant& ResourceLedger::Grant::operator=(
    Grant&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
    resources_ = other.resources_;
    other.resources_ = {};
  }
  return *this;
}

/** @copydoc ResourceLedger::Grant::~Grant */
ResourceLedger::Grant::~Grant() noexcept {
  reset();
}

/** @copydoc ResourceLedger::PluginResourceToken::PluginResourceToken */
ResourceLedger::PluginResourceToken::PluginResourceToken(
    std::shared_ptr<ResourceLedgerRootState> root,
    PluginInvocationIdentityDigest identity,
    PluginResourceVector resources) noexcept
    : root_(std::move(root)), identity_(identity), resources_(resources) {}

/** @copydoc ResourceLedger::PluginResourceToken::PluginResourceToken */
ResourceLedger::PluginResourceToken::PluginResourceToken(
    PluginResourceToken&& other) noexcept
    : root_(std::move(other.root_)),
      identity_(other.identity_),
      resources_(other.resources_) {
  other.identity_ = {};
  other.resources_ = {};
}

/** @copydoc ResourceLedger::PluginResourceToken::operator= */
ResourceLedger::PluginResourceToken&
ResourceLedger::PluginResourceToken::operator=(
    PluginResourceToken&& other) noexcept {
  if (this != &other) {
    reset();
    root_ = std::move(other.root_);
    identity_ = other.identity_;
    resources_ = other.resources_;
    other.identity_ = {};
    other.resources_ = {};
  }
  return *this;
}

/** @copydoc ResourceLedger::PluginResourceToken::~PluginResourceToken */
ResourceLedger::PluginResourceToken::~PluginResourceToken() noexcept {
  reset();
}

/** @copydoc ResourceLedger::PluginResourceToken::active */
bool ResourceLedger::PluginResourceToken::active() const noexcept {
  return root_ != nullptr;
}

/** @copydoc ResourceLedger::PluginResourceToken::identity_digest */
PluginInvocationIdentityDigest
ResourceLedger::PluginResourceToken::identity_digest() const noexcept {
  return identity_;
}

/** @copydoc ResourceLedger::PluginResourceToken::resources */
PluginResourceVector ResourceLedger::PluginResourceToken::resources()
    const noexcept {
  return resources_;
}

/** @copydoc ResourceLedger::PluginResourceToken::consume */
ResourceLedger::PluginResourceLease
ResourceLedger::PluginResourceToken::consume(
    const PluginInvocationIdentityDigest& identity,
    const PluginResourceVector& resources) && {
  if (!root_) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::InactiveToken,
        "Cannot consume an inactive plugin resource token.");
  }
  if (identity != identity_) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::IdentityMismatch,
        "Plugin resource token identity does not match the invocation.");
  }
  if (resources != resources_) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::ResourceMismatch,
        "Plugin resource token vector does not match the invocation.");
  }
  std::shared_ptr<ResourceLedgerRootState> root = std::move(root_);
  const PluginResourceVector consumed = resources_;
  identity_ = {};
  resources_ = {};
  return PluginResourceLease(std::move(root), consumed);
}

/** @copydoc ResourceLedger::PluginResourceToken::reset */
void ResourceLedger::PluginResourceToken::reset() noexcept {
  if (!root_) {
    return;
  }
  std::shared_ptr<ResourceLedgerRootState> root = std::move(root_);
  const PluginResourceVector resources = resources_;
  identity_ = {};
  resources_ = {};
  release_plugin_resources(root, resources);
}

/** @copydoc ResourceLedger::PluginResourceLease::PluginResourceLease */
ResourceLedger::PluginResourceLease::PluginResourceLease(
    std::shared_ptr<ResourceLedgerRootState> root,
    PluginResourceVector resources) noexcept
    : root_(std::move(root)), resources_(resources) {}

/** @copydoc ResourceLedger::PluginResourceLease::PluginResourceLease */
ResourceLedger::PluginResourceLease::PluginResourceLease(
    PluginResourceLease&& other) noexcept
    : root_(std::move(other.root_)), resources_(other.resources_) {
  other.resources_ = {};
}

/** @copydoc ResourceLedger::PluginResourceLease::operator= */
ResourceLedger::PluginResourceLease&
ResourceLedger::PluginResourceLease::operator=(
    PluginResourceLease&& other) noexcept {
  if (this != &other) {
    reset();
    root_ = std::move(other.root_);
    resources_ = other.resources_;
    other.resources_ = {};
  }
  return *this;
}

/** @copydoc ResourceLedger::PluginResourceLease::~PluginResourceLease */
ResourceLedger::PluginResourceLease::~PluginResourceLease() noexcept {
  reset();
}

/** @copydoc ResourceLedger::PluginResourceLease::active */
bool ResourceLedger::PluginResourceLease::active() const noexcept {
  return root_ != nullptr;
}

/** @copydoc ResourceLedger::PluginResourceLease::resources */
PluginResourceVector ResourceLedger::PluginResourceLease::resources()
    const noexcept {
  return resources_;
}

/** @copydoc ResourceLedger::PluginResourceLease::reset */
void ResourceLedger::PluginResourceLease::reset() noexcept {
  if (!root_) {
    return;
  }
  std::shared_ptr<ResourceLedgerRootState> root = std::move(root_);
  const PluginResourceVector resources = resources_;
  resources_ = {};
  release_plugin_resources(root, resources);
}

/** @copydoc ResourceLedger::Grant::active */
bool ResourceLedger::Grant::active() const noexcept {
  return state_ != nullptr;
}

/** @copydoc ResourceLedger::Grant::resources */
ResourceVector ResourceLedger::Grant::resources() const noexcept {
  return resources_;
}

/** @copydoc ResourceLedger::Grant::reset */
void ResourceLedger::Grant::reset() noexcept {
  if (!state_) {
    return;
  }

  std::shared_ptr<ResourceReservationState> state = std::move(state_);
  const ResourceVector resources = resources_;
  resources_ = {};
  bool release_root = false;
  try {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->child_count == 0U ||
          !resources_fit(resources, state->granted)) {
        std::terminate();
      }
      state->granted = subtract_resources(state->granted, resources);
      --state->child_count;
      if (state->settlement_observer.observes_children()) {
        state->settlement_observer.on_child_released(
            state->settlement_observer.context);
      }
      if (!state->parent_active && state->child_count == 0U) {
        if (state->root_released || state->granted != ResourceVector{}) {
          std::terminate();
        }
        state->root_released = true;
        release_root = true;
      }
    }
  } catch (...) {
    std::terminate();
  }
  if (release_root) {
    release_root_resources(state->root, state->committed,
                           state->release_observer, state->settlement_observer);
  }
}

/** @copydoc ResourceLedger::ResourceLedger */
ResourceLedger::ResourceLedger(ResourceVector limits,
                               std::vector<DeviceResourceLimit> device_limits,
                               PluginResourceVector plugin_limits)
    : state_(std::make_shared<ResourceLedgerRootState>(
          limits, device_limits, plugin_limits)) {}  // NOLINT

/** @copydoc ResourceLedger::~ResourceLedger */
ResourceLedger::~ResourceLedger() noexcept = default;

/** @copydoc ResourceLedger::try_reserve */
std::optional<ResourceLedger::Reservation> ResourceLedger::try_reserve(
    const ResourceVector& requested,
    std::shared_ptr<ReservationReleaseObserver> release_observer,
    ReservationSettlementObserver settlement_observer) {
  auto reservation_state = std::make_shared<ResourceReservationState>(
      state_, requested, std::move(release_observer), settlement_observer);
  std::lock_guard<std::mutex> lock(state_->mutex);
  const std::optional<ResourceVector> next_reserved =
      checked_add_resources(state_->reserved, requested);
  if (!next_reserved.has_value() ||
      !resources_fit(*next_reserved, state_->limits)) {
    return std::nullopt;
  }
  state_->reserved = *next_reserved;
  state_->high_water = maximum_resources(state_->high_water, state_->reserved);
  return Reservation(std::move(reservation_state));
}

/** @copydoc ResourceLedger::try_reserve_pair */
std::optional<ResourceLedger::ReservationPair> ResourceLedger::try_reserve_pair(
    const ResourceVector& first, const ResourceVector& second) {
  const std::optional<ResourceVector> combined =
      checked_add_resources(first, second);
  if (!combined.has_value()) {
    return std::nullopt;
  }

  auto first_state = std::make_shared<ResourceReservationState>(state_, first);
  auto second_state =
      std::make_shared<ResourceReservationState>(state_, second);
  std::lock_guard<std::mutex> lock(state_->mutex);
  const std::optional<ResourceVector> next_reserved =
      checked_add_resources(state_->reserved, *combined);
  if (!next_reserved.has_value() ||
      !resources_fit(*next_reserved, state_->limits)) {
    return std::nullopt;
  }
  state_->reserved = *next_reserved;
  state_->high_water = maximum_resources(state_->high_water, state_->reserved);
  return ReservationPair{Reservation(std::move(first_state)),
                         Reservation(std::move(second_state))};
}

/** @copydoc ResourceLedger::try_reserve_device */
std::optional<ResourceLedger::DeviceReservation>
ResourceLedger::try_reserve_device(DeviceId device,
                                   const DeviceResourceVector& planned) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto account = state_->devices.find(device);
  if (account == state_->devices.end()) {
    return std::nullopt;
  }
  const std::optional<DeviceResourceVector> next_reserved =
      checked_add_device_resources(account->second.reserved, planned);
  if (!next_reserved.has_value() ||
      !device_resources_fit(*next_reserved, account->second.limits)) {
    return std::nullopt;
  }
  account->second.reserved = *next_reserved;
  account->second.high_water = maximum_device_resources(
      account->second.high_water, account->second.reserved);
  return DeviceReservation(state_, device, planned);
}

/** @copydoc ResourceLedger::issue_plugin_invocation */
ResourceLedger::PluginResourceToken ResourceLedger::issue_plugin_invocation(
    const PluginInvocationIdentityDigest& identity,
    const PluginResourceVector& requested) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->plugin_replay_identities.find(identity) !=
      state_->plugin_replay_identities.end()) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::Replay,
        "Plugin invocation identity has already been issued.");
  }
  const std::optional<PluginResourceVector> next_reserved =
      checked_add_plugin_resources(state_->plugin_reserved, requested);
  if (!next_reserved.has_value() ||
      !plugin_resources_fit(*next_reserved, state_->plugin_limits)) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::QuotaExceeded,
        "Plugin invocation resources exceed ledger capacity.");
  }
  const auto [unused, inserted] =
      state_->plugin_replay_identities.insert(identity);
  static_cast<void>(unused);
  if (!inserted) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::Replay,
        "Plugin invocation identity has already been issued.");
  }
  state_->plugin_reserved = *next_reserved;
  state_->plugin_high_water = maximum_plugin_resources(
      state_->plugin_high_water, state_->plugin_reserved);
  return PluginResourceToken(state_, identity, requested);
}

/** @copydoc ResourceLedger::snapshot */
ResourceLedger::Snapshot ResourceLedger::snapshot() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return Snapshot{state_->limits, state_->reserved, state_->high_water};
}

/** @copydoc ResourceLedger::plugin_snapshot */
ResourceLedger::PluginSnapshot ResourceLedger::plugin_snapshot() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return PluginSnapshot{state_->plugin_limits, state_->plugin_reserved,
                        state_->plugin_high_water};
}

/** @copydoc ResourceLedger::device_snapshot */
std::optional<ResourceLedger::DeviceSnapshot> ResourceLedger::device_snapshot(
    DeviceId device) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto account = state_->devices.find(device);
  if (account == state_->devices.end()) {
    return std::nullopt;
  }
  return DeviceSnapshot{
      device,
      account->second.limits,
      account->second.reserved,
      account->second.high_water,
      subtract_device_resources(account->second.limits,
                                account->second.reserved),
  };
}

/** @copydoc ResourceLedger::device_snapshots */
std::vector<ResourceLedger::DeviceSnapshot> ResourceLedger::device_snapshots()
    const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::vector<DeviceSnapshot> snapshots;
  snapshots.reserve(state_->devices.size());
  for (const auto& [device, account] : state_->devices) {
    snapshots.push_back(DeviceSnapshot{
        device,
        account.limits,
        account.reserved,
        account.high_water,
        subtract_device_resources(account.limits, account.reserved),
    });
  }
  return snapshots;
}

}  // namespace ps
