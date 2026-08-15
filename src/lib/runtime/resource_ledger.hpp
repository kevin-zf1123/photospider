/**
 * @file resource_ledger.hpp
 * @brief Declares host-authoritative checked resource admission and grants.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "photospider/core/device.hpp"

namespace ps {

/**
 * @brief Exact host-accounted resources committed in one ledger transaction.
 *
 * Every field is an independent unsigned capacity dimension. The issue #70
 * implementation accounts only resources for which trusted host code has an
 * explicit declaration; zero means no amount was declared for that dimension.
 *
 * @throws Nothing for value construction, copying, and comparison.
 * @note This value is a request/snapshot, not an authority-bearing token.
 */
struct ResourceVector final {
  /** @brief Concurrent execution callback slots. */
  std::uint64_t cpu_slots = 0U;

  /** @brief Host bytes retained while admitted callbacks may be in flight. */
  std::uint64_t retained_memory_bytes = 0U;

  /** @brief Host-declared temporary workspace bytes. */
  std::uint64_t scratch_bytes = 0U;

  /** @brief Entries reserved in the owning service ready store. */
  std::uint64_t ready_entries = 0U;

  /** @brief Accounted bytes reserved in the owning service ready store. */
  std::uint64_t ready_bytes = 0U;
};

/**
 * @brief Compares every resource dimension for exact equality.
 * @param lhs First checked resource vector.
 * @param rhs Second checked resource vector.
 * @return True only when every dimension is equal.
 * @throws Nothing.
 */
bool operator==(const ResourceVector& lhs, const ResourceVector& rhs) noexcept;

/**
 * @brief Compares resource vectors for any differing dimension.
 * @param lhs First checked resource vector.
 * @param rhs Second checked resource vector.
 * @return True when at least one dimension differs.
 * @throws Nothing.
 */
bool operator!=(const ResourceVector& lhs, const ResourceVector& rhs) noexcept;

/**
 * @brief Adds two complete resource vectors with checked arithmetic.
 * @param lhs First vector.
 * @param rhs Second vector.
 * @return Complete sum, or `std::nullopt` if any dimension overflows.
 * @throws Nothing.
 * @note No dimension is clamped and no partial sum is returned.
 */
std::optional<ResourceVector> checked_add_resources(
    const ResourceVector& lhs, const ResourceVector& rhs) noexcept;

/**
 * @brief Multiplies every resource dimension by one checked scalar.
 * @param resources Per-unit resources.
 * @param count Number of identical units.
 * @return Complete product, or `std::nullopt` if any dimension overflows.
 * @throws Nothing.
 * @note Zero count returns the zero vector.
 */
std::optional<ResourceVector> checked_multiply_resources(
    const ResourceVector& resources, std::uint64_t count) noexcept;

/**
 * @brief Tests whether one vector fits component-wise within another.
 * @param requested Vector requiring capacity.
 * @param available Vector supplying capacity.
 * @return True only when every requested dimension is within availability.
 * @throws Nothing.
 */
bool resources_fit(const ResourceVector& requested,
                   const ResourceVector& available) noexcept;

/**
 * @brief Exact bytes accounted for one concrete non-CPU device.
 *
 * Persistent allocation bytes and invocation/transfer scratch are independent
 * dimensions. Neither field can borrow from the other or from `ResourceVector`.
 *
 * @throws Nothing for value construction, copying, and comparison.
 * @note This value carries no reservation or release authority.
 */
struct DeviceResourceVector final {
  /** @brief Native bytes retained by persistent device-resident allocations. */
  std::uint64_t device_memory_bytes = 0U;

  /** @brief Native bytes used by invocation or transfer workspace. */
  std::uint64_t device_scratch_bytes = 0U;
};

/**
 * @brief Exact Host-accounted resources for one isolated plugin invocation.
 *
 * Every field is an independent capacity dimension. The vector is copied into
 * the Host-issued token and later compared exactly at consumption, so child or
 * wire declarations can neither enlarge nor retarget admitted authority.
 *
 * @throws Nothing for aggregate value operations.
 * @note This value carries no reservation, execution, or release authority.
 */
struct PluginResourceVector final {
  /** @brief Concurrent freshly execed runtime-process slots. */
  std::uint64_t runtime_processes = 0U;

  /** @brief Concurrent isolated CPU callback slots. */
  std::uint64_t cpu_slots = 0U;

  /** @brief Aggregate admitted per-process address-space ceilings in bytes. */
  std::uint64_t address_space_bytes = 0U;

  /** @brief Invocation-owned shared-memory bytes. */
  std::uint64_t shared_memory_bytes = 0U;

  /** @brief Invocation capability and control descriptor count. */
  std::uint64_t descriptor_count = 0U;
};

/**
 * @brief Fixed domain-separated digest of one complete invocation identity.
 * @note The executor derives this from tenant, Job, attempt, worker lease,
 * package generation, and invocation identity before asking the ledger to
 * mint authority. The digest itself never crosses the plugin wire.
 */
using PluginInvocationIdentityDigest = std::array<std::uint8_t, 32U>;

/**
 * @brief Compares every plugin resource dimension for exact equality.
 * @param lhs First complete vector.
 * @param rhs Second complete vector.
 * @return True only when every dimension is equal.
 * @throws Nothing.
 */
bool operator==(const PluginResourceVector& lhs,
                const PluginResourceVector& rhs) noexcept;

/**
 * @brief Reports any differing plugin resource dimension.
 * @param lhs First complete vector.
 * @param rhs Second complete vector.
 * @return True when at least one dimension differs.
 * @throws Nothing.
 */
bool operator!=(const PluginResourceVector& lhs,
                const PluginResourceVector& rhs) noexcept;

/**
 * @brief Adds complete plugin resource vectors without wraparound.
 * @param lhs First vector.
 * @param rhs Second vector.
 * @return Exact sum, or no value if any dimension overflows.
 * @throws Nothing.
 */
std::optional<PluginResourceVector> checked_add_plugin_resources(
    const PluginResourceVector& lhs, const PluginResourceVector& rhs) noexcept;

/**
 * @brief Tests component-wise plugin resource capacity.
 * @param requested Vector requiring capacity.
 * @param available Vector supplying capacity.
 * @return True only when every requested dimension fits.
 * @throws Nothing.
 */
bool plugin_resources_fit(const PluginResourceVector& requested,
                          const PluginResourceVector& available) noexcept;

/**
 * @brief Stable reason one Host plugin-resource authority operation failed.
 */
enum class PluginResourceAdmissionErrorCode : std::uint8_t {
  /** @brief One or more requested dimensions exceed remaining capacity. */
  QuotaExceeded = 0U,

  /** @brief The complete invocation identity was already issued once. */
  Replay = 1U,

  /** @brief Token consumption supplied a different identity digest. */
  IdentityMismatch = 2U,

  /** @brief Token consumption supplied a different resource vector. */
  ResourceMismatch = 3U,

  /** @brief Consumption was attempted through an inactive token. */
  InactiveToken = 4U,
};

/**
 * @brief Typed fail-closed plugin resource admission or consumption failure.
 *
 * @throws std::bad_alloc when retaining the diagnostic fails.
 * @note The exception carries observation only and never carries capacity.
 */
class PluginResourceAdmissionError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one stable typed resource-authority failure.
   * @param code Closed failure reason.
   * @param message Host-owned human-readable diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  PluginResourceAdmissionError(PluginResourceAdmissionErrorCode code,
                               std::string message);

  /**
   * @brief Returns the closed failure reason.
   * @return Code supplied at construction.
   * @throws Nothing.
   */
  PluginResourceAdmissionErrorCode code() const noexcept { return code_; }

 private:
  /** @brief Stable failure category. */
  PluginResourceAdmissionErrorCode code_;
};

/**
 * @brief Compares every device resource dimension for equality.
 * @param lhs First device vector.
 * @param rhs Second device vector.
 * @return True only when both byte dimensions are equal.
 * @throws Nothing.
 */
bool operator==(const DeviceResourceVector& lhs,
                const DeviceResourceVector& rhs) noexcept;

/**
 * @brief Compares device resource vectors for any difference.
 * @param lhs First device vector.
 * @param rhs Second device vector.
 * @return True when either byte dimension differs.
 * @throws Nothing.
 */
bool operator!=(const DeviceResourceVector& lhs,
                const DeviceResourceVector& rhs) noexcept;

/**
 * @brief Adds two device resource vectors without wraparound.
 * @param lhs First vector.
 * @param rhs Second vector.
 * @return Exact sum, or `std::nullopt` if either dimension overflows.
 * @throws Nothing.
 * @note No dimension is clamped and no partial sum is returned.
 */
std::optional<DeviceResourceVector> checked_add_device_resources(
    const DeviceResourceVector& lhs, const DeviceResourceVector& rhs) noexcept;

/**
 * @brief Tests whether one device vector fits within another.
 * @param requested Vector requiring capacity.
 * @param available Vector supplying capacity.
 * @return True only when both requested dimensions fit.
 * @throws Nothing.
 */
bool device_resources_fit(const DeviceResourceVector& requested,
                          const DeviceResourceVector& available) noexcept;

/**
 * @brief Immutable composition limit for one complete concrete device.
 *
 * @throws Nothing for aggregate value operations.
 * @note CPU identities are invalid configuration because the existing Host
 * vector remains the sole CPU capacity authority.
 */
struct DeviceResourceLimit final {
  /** @brief Complete process-local non-CPU device identity. */
  DeviceId device{DeviceBackend::Metal};

  /** @brief Maximum persistent-memory and scratch commitments. */
  DeviceResourceVector resources;
};

/**
 * @brief Stable category for allocator-facing device accounting failures.
 *
 * @throws Nothing for value operations.
 */
enum class DeviceResourceErrorCode : std::uint32_t {
  /** @brief The configured device account could not admit the complete plan. */
  AdmissionRejected = 0U,

  /** @brief Native reported actual bytes exceeded the admitted plan. */
  ActualExceedsReservation = 1U,

  /** @brief A native size/alignment or allocated-size fact was invalid. */
  InvalidNativeSize = 2U,
};

/**
 * @brief Typed device resource planning or reconciliation failure.
 *
 * @throws std::bad_alloc when diagnostic ownership cannot allocate.
 * @note This private source-tree exception carries observation only and mints
 * no resource authority.
 */
class DeviceResourceError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one owned typed device accounting failure.
   * @param code Stable failure category.
   * @param device Exact device whose plan failed.
   * @param planned Complete admitted or requested plan.
   * @param actual Native actual bytes, or zero before native allocation.
   * @param message Stable human-readable diagnostic.
   * @throws std::bad_alloc when the diagnostic string cannot allocate.
   */
  DeviceResourceError(DeviceResourceErrorCode code, DeviceId device,
                      DeviceResourceVector planned, DeviceResourceVector actual,
                      std::string message);

  /**
   * @brief Returns the stable failure category.
   * @return Error classification supplied at construction.
   * @throws Nothing.
   */
  DeviceResourceErrorCode code() const noexcept { return code_; }

  /**
   * @brief Returns the exact affected device.
   * @return Complete process-local device identity.
   * @throws Nothing.
   */
  DeviceId device() const noexcept { return device_; }

  /**
   * @brief Returns the admitted or requested plan.
   * @return Complete planned device resource vector.
   * @throws Nothing.
   */
  DeviceResourceVector planned() const noexcept { return planned_; }

  /**
   * @brief Returns native actual bytes known at failure.
   * @return Complete actual vector, or zero before allocation.
   * @throws Nothing.
   */
  DeviceResourceVector actual() const noexcept { return actual_; }

 private:
  /** @brief Stable failure category. */
  DeviceResourceErrorCode code_;

  /** @brief Exact concrete device involved in the failure. */
  DeviceId device_;

  /** @brief Requested or admitted complete plan. */
  DeviceResourceVector planned_;

  /** @brief Native actual bytes observed before failure. */
  DeviceResourceVector actual_;
};

/** @brief Opaque shared root state retained by outstanding reservations. */
struct ResourceLedgerRootState;

/** @brief Opaque shared reservation state retained by outstanding grants. */
struct ResourceReservationState;

/**
 * @brief Host/device-authoritative reservation, grant, and lease ledger.
 *
 * One ledger owns immutable composition-root limits. Root reservations commit
 * complete checked vectors atomically. A live reservation may suballocate only
 * within its committed vector, and root capacity remains committed until both
 * the reservation owner is gone and every child grant has released. Separate
 * per-device plans reconcile native actual bytes into independently lived
 * persistent-memory and completion-scratch leases.
 *
 * @throws std::bad_alloc when private authority state cannot allocate.
 * @throws std::system_error when internal mutex operations fail.
 * @note The ledger owns no ordering, Graph, Run, callback, or cancellation
 * policy. Its nested owners are the only authority-bearing values.
 */
class ResourceLedger final {
 public:
  /**
   * @brief Non-owning exact root-settlement notification.
   *
   * @throws Nothing for value construction, copying, and invocation.
   * @note The callback carries no capacity authority and allocates nothing. Its
   * context must outlive the root reservation, including every deferred child
   * grant. The ledger invokes it exactly once after physical capacity and any
   * companion quota accounting have both been returned.
   */
  struct ReservationSettlementObserver final {
    /**
     * @brief Non-throwing root- or child-settlement lifecycle callback.
     *
     * @param context Borrowed observer context supplied with this value.
     * @return Nothing.
     * @throws Nothing.
     */
    using Callback = void (*)(void* context) noexcept;

    /**
     * @brief Creates an empty or complete non-owning callback value.
     * @param observer_context Borrowed stable context, or null.
     * @param settled_callback Exact-once root-settlement callback, or null.
     * @param child_granted_callback Callback after one child grant is minted,
     * or null.
     * @param child_released_callback Callback after one child grant is
     * returned, or null.
     * @throws Nothing.
     */
    constexpr ReservationSettlementObserver(
        void* observer_context = nullptr, Callback settled_callback = nullptr,
        Callback child_granted_callback = nullptr,
        Callback child_released_callback = nullptr) noexcept
        : context(observer_context),
          on_settled(settled_callback),
          on_child_granted(child_granted_callback),
          on_child_released(child_released_callback) {}

    /** @brief Borrowed stable context retained only as an opaque address. */
    void* context;

    /**
     * @brief Exact-once post-release callback, or null for no observation.
     * @param context Borrowed stable context supplied above.
     * @return Nothing.
     * @throws Nothing; throwing across this callback terminates.
     */
    Callback on_settled;

    /**
     * @brief Post-mint child-grant observation, or null when unobserved.
     * @param context Borrowed stable context supplied above.
     * @return Nothing.
     * @throws Nothing; throwing across this callback terminates.
     * @note The callback observes ownership only and cannot modify the grant.
     */
    Callback on_child_granted;

    /**
     * @brief Post-release child-grant observation, or null when unobserved.
     * @param context Borrowed stable context supplied above.
     * @return Nothing.
     * @throws Nothing; throwing across this callback terminates.
     * @note The callback runs exactly once for each successfully minted child.
     */
    Callback on_child_released;

    /**
     * @brief Reports whether this value names a complete callback.
     * @return True only when context and function are both non-null.
     * @throws Nothing.
     */
    bool valid() const noexcept {
      return context != nullptr && on_settled != nullptr;
    }

    /**
     * @brief Reports whether both exact child-lifetime callbacks are complete.
     * @return True only when context and both child callbacks are non-null.
     * @throws Nothing.
     */
    bool observes_children() const noexcept {
      return context != nullptr && on_child_granted != nullptr &&
             on_child_released != nullptr;
    }
  };

  /**
   * @brief Observes exact root release under one caller-owned transaction lock.
   *
   * @throws Nothing from destruction or either callback.
   * @note This private source-tree seam carries no resource authority. The
   * ledger retains a shared observer only for a successfully committed root
   * reservation and calls it exactly once after that root vector is physically
   * returned, including when outstanding child grants defer the release.
   */
  class ReservationReleaseObserver {
   public:
    /**
     * @brief Releases one non-authoritative observer owner.
     * @throws Nothing.
     */
    virtual ~ReservationReleaseObserver() noexcept = default;

    /**
     * @brief Returns the external lock serializing admission and release.
     * @return Stable mutex retained for the observer lifetime.
     * @throws Nothing.
     * @note The mutex must be distinct from ledger and reservation mutexes.
     */
    virtual std::mutex& release_transaction_mutex() noexcept = 0;

    /**
     * @brief Updates companion non-authoritative accounting after root release.
     * @param released Exact vector already returned to ledger capacity.
     * @return Nothing.
     * @throws Nothing.
     * @note The ledger holds `release_transaction_mutex()` while calling this
     * method. Implementations must not call back into the ledger or take
     * another lock.
     */
    virtual void on_reservation_released(
        const ResourceVector& released) noexcept = 0;
  };

  /** @brief Move-only exact child authority minted by one reservation. */
  class Grant;

  /** @brief Move-only consumed isolated-plugin resource authority. */
  class PluginResourceLease;

  /**
   * @brief Host-issued one-use plugin invocation resource token.
   *
   * Issuance reserves the exact vector and permanently spends the identity for
   * this ledger lifetime. Exact consumption transfers release ownership to a
   * lease; destruction before consumption rolls reservation back once while
   * preserving the replay tombstone.
   */
  class PluginResourceToken final {
   public:
    /**
     * @brief Transfers one unconsumed token and its rollback obligation.
     * @param other Token made inactive by the transfer.
     * @throws Nothing.
     */
    PluginResourceToken(PluginResourceToken&& other) noexcept;

    /**
     * @brief Replaces this token after returning any prior reservation.
     * @param other Token made inactive by the transfer.
     * @return Reference to this token.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    PluginResourceToken& operator=(PluginResourceToken&& other) noexcept;

    /**
     * @brief Returns an unconsumed exact reservation once.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    ~PluginResourceToken() noexcept;

    /**
     * @brief Prevents copying Host-minted authority.
     * @param other Source token that cannot be copied.
     * @throws Nothing because this overload is deleted.
     */
    PluginResourceToken(const PluginResourceToken& other) = delete;

    /**
     * @brief Prevents copy-assigning Host-minted authority.
     * @param other Source token that cannot be copied.
     * @return No value because this overload is deleted.
     * @throws Nothing because this overload is deleted.
     */
    PluginResourceToken& operator=(const PluginResourceToken& other) = delete;

    /**
     * @brief Reports whether this owner retains an unconsumed reservation.
     * @return True before movement, consumption, or destruction.
     * @throws Nothing.
     */
    bool active() const noexcept;

    /**
     * @brief Copies the bound invocation identity digest.
     * @return Exact digest, or zero after movement/consumption.
     * @throws Nothing.
     */
    PluginInvocationIdentityDigest identity_digest() const noexcept;

    /**
     * @brief Copies the exact admitted resource vector.
     * @return Bound vector, or zero after movement/consumption.
     * @throws Nothing.
     */
    PluginResourceVector resources() const noexcept;

    /**
     * @brief Consumes this token against equal identity and resource facts.
     * @param identity Exact complete invocation identity digest.
     * @param resources Exact resource vector used for issuance.
     * @return Move-only lease retaining the reservation through all effects.
     * @throws PluginResourceAdmissionError for inactive or mismatched facts.
     * @note Mismatch leaves this token active and performs no accounting
     * mutation. Success can occur only once and transfers exact settlement.
     */
    PluginResourceLease consume(const PluginInvocationIdentityDigest& identity,
                                const PluginResourceVector& resources) &&;

   private:
    friend class ResourceLedger;

    /**
     * @brief Wraps one already committed Host reservation.
     * @param root Matching shared ledger state.
     * @param identity Exact replay-spent identity digest.
     * @param resources Exact committed vector.
     * @throws Nothing.
     */
    PluginResourceToken(std::shared_ptr<ResourceLedgerRootState> root,
                        PluginInvocationIdentityDigest identity,
                        PluginResourceVector resources) noexcept;

    /**
     * @brief Returns this unconsumed reservation exactly once.
     * @return Nothing.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    void reset() noexcept;

    /** @brief Shared ledger authority retained until transfer or rollback. */
    std::shared_ptr<ResourceLedgerRootState> root_;

    /** @brief Complete replay-spent identity bound at issuance. */
    PluginInvocationIdentityDigest identity_{};

    /** @brief Exact vector returned or transferred once. */
    PluginResourceVector resources_;
  };

  /**
   * @brief Move-only consumed plugin resource lease.
   *
   * The lease spans materialization, spawn, exec, callback, validation, and
   * publication. Destruction releases its exact vector once while the ledger's
   * replay tombstone remains immutable.
   *
   * @note Move and destruction must not race on the same lease object. Ledger
   * settlement is internally serialized across independent leases.
   */
  class PluginResourceLease final {
   public:
    /**
     * @brief Transfers one consumed reservation and settlement obligation.
     * @param other Lease made inactive by the transfer.
     * @throws Nothing.
     * @note No ledger accounting changes; only unique local ownership moves.
     */
    PluginResourceLease(PluginResourceLease&& other) noexcept;

    /**
     * @brief Replaces this lease after exactly settling prior ownership.
     * @param other Lease made inactive by the transfer.
     * @return Reference to this lease after transfer.
     * @throws Nothing; invariant or synchronization failure terminates.
     * @note Self-move is a no-op. Otherwise this lease settles its old vector
     * before taking the source lease's unique settlement obligation.
     */
    PluginResourceLease& operator=(PluginResourceLease&& other) noexcept;

    /**
     * @brief Settles the exact consumed vector once.
     * @throws Nothing; invariant or synchronization failure terminates.
     * @note The replay tombstone is retained by the ledger after settlement.
     */
    ~PluginResourceLease() noexcept;

    /**
     * @brief Prevents copying consumed authority.
     * @param other Source lease that cannot be copied.
     * @throws Nothing because this overload is deleted.
     */
    PluginResourceLease(const PluginResourceLease& other) = delete;

    /**
     * @brief Prevents copy-assigning consumed authority.
     * @param other Source lease that cannot be copied.
     * @return No value because this overload is deleted.
     * @throws Nothing because this overload is deleted.
     */
    PluginResourceLease& operator=(const PluginResourceLease& other) = delete;

    /**
     * @brief Reports whether this lease owns unsettled resources.
     * @return True before movement or destruction.
     * @throws Nothing.
     */
    bool active() const noexcept;

    /**
     * @brief Copies the exact consumed vector.
     * @return Bound vector, or zero when inactive.
     * @throws Nothing.
     */
    PluginResourceVector resources() const noexcept;

   private:
    friend class PluginResourceToken;

    /**
     * @brief Creates an active lease by ownership transfer from one token.
     * @param root Shared root containing the committed plugin subledger.
     * @param resources Exact consumed vector to settle once.
     * @throws Nothing.
     * @note The caller has already spent the identity tombstone and detached
     * token ownership; construction performs no admission or accounting.
     */
    PluginResourceLease(std::shared_ptr<ResourceLedgerRootState> root,
                        PluginResourceVector resources) noexcept;

    /**
     * @brief Settles this exact vector and makes the lease inactive.
     * @return Nothing.
     * @throws Nothing; invariant or synchronization failure terminates.
     * @note Moves root ownership out before taking the internal ledger lock,
     * so re-entry or repeated destruction cannot settle the vector twice.
     */
    void reset() noexcept;

    /** @brief Shared ledger root retaining settlement authority. */
    std::shared_ptr<ResourceLedgerRootState> root_;

    /** @brief Exact vector returned once. */
    PluginResourceVector resources_;
  };

  /** @brief Move-only committed authority for one device resource component. */
  class DeviceLease;

  /** @brief Completed pair defined after the concrete lease owner. */
  struct DeviceLeasePair;

  /**
   * @brief Move-only planned device allocation reservation.
   *
   * The reservation owns both planned dimensions until `commit_actual()`
   * transfers exact actual components into independent leases. Destruction
   * before commit returns the complete plan once.
   */
  class DeviceReservation final {
   public:
    /**
     * @brief Transfers one planned reservation.
     * @param other Reservation made inactive by the transfer.
     * @throws Nothing.
     */
    DeviceReservation(DeviceReservation&& other) noexcept;

    /**
     * @brief Replaces this plan after returning prior ownership.
     * @param other Reservation made inactive by the transfer.
     * @return Reference to this reservation.
     * @throws Nothing; synchronization failure during release terminates.
     */
    DeviceReservation& operator=(DeviceReservation&& other) noexcept;

    /**
     * @brief Returns an uncommitted complete plan once.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    ~DeviceReservation() noexcept;

    /**
     * @brief Prevents duplicating planned authority.
     * @param other Unused source because copying is forbidden.
     * @throws Nothing because this operation is deleted.
     */
    DeviceReservation(const DeviceReservation& other) = delete;

    /**
     * @brief Prevents assigning duplicate planned authority.
     * @param other Unused source because copying is forbidden.
     * @return No value because this operation is deleted.
     * @throws Nothing because this operation is deleted.
     */
    DeviceReservation& operator=(const DeviceReservation& other) = delete;

    /**
     * @brief Reports whether this owner still holds an uncommitted plan.
     * @return True before movement, commit, or destruction.
     * @throws Nothing.
     */
    bool active() const noexcept;

    /**
     * @brief Returns the exact device of an active plan.
     * @return Configured device identity, or CPU sentinel after movement.
     * @throws Nothing.
     */
    DeviceId device() const noexcept;

    /**
     * @brief Copies the complete planned vector.
     * @return Planned bytes, or zero after movement or commit.
     * @throws Nothing.
     */
    DeviceResourceVector planned_resources() const noexcept;

    /**
     * @brief Reconciles native actual bytes and transfers exact authority.
     * @param actual Complete native-reported memory and scratch totals.
     * @return Independent persistent-memory and scratch leases.
     * @throws DeviceResourceError when actual bytes exceed the plan.
     * @throws std::logic_error when this reservation is inactive.
     * @throws std::system_error when ledger synchronization fails.
     * @note Fitting commit returns unused planned bytes under the root lock.
     * No partial lease is published on failure.
     */
    DeviceLeasePair commit_actual(DeviceResourceVector actual);

   private:
    friend class ResourceLedger;

    /**
     * @brief Wraps one root-committed complete device plan.
     * @param root Matching shared ledger root.
     * @param device Exact configured device.
     * @param planned Complete vector already committed at the root.
     * @throws Nothing.
     */
    DeviceReservation(std::shared_ptr<ResourceLedgerRootState> root,
                      DeviceId device, DeviceResourceVector planned) noexcept;

    /**
     * @brief Returns this uncommitted plan for destruction or assignment.
     * @return Nothing.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    void reset() noexcept;

    /** @brief Root authority retained until commit or rollback. */
    std::shared_ptr<ResourceLedgerRootState> root_;

    /** @brief Exact configured device for this plan. */
    DeviceId device_{DeviceBackend::CPU};

    /** @brief Complete plan returned or transferred once. */
    DeviceResourceVector planned_;
  };

  /**
   * @brief Move-only exact committed device resource authority.
   *
   * One lease owns either persistent memory, scratch, or a checked vector of
   * both. Actual-byte commit returns separate single-component leases. The
   * lease performs no identity lookup; destruction subtracts its exact vector
   * from its exact device account once.
   */
  class DeviceLease final {
   public:
    /**
     * @brief Transfers one committed device authority.
     * @param other Lease made inactive by the transfer.
     * @throws Nothing.
     */
    DeviceLease(DeviceLease&& other) noexcept;

    /**
     * @brief Replaces this lease after returning prior ownership.
     * @param other Lease made inactive by the transfer.
     * @return Reference to this lease.
     * @throws Nothing; synchronization failure during release terminates.
     */
    DeviceLease& operator=(DeviceLease&& other) noexcept;

    /**
     * @brief Returns the exact committed bytes once.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    ~DeviceLease() noexcept;

    /**
     * @brief Prevents duplicating device authority.
     * @param other Unused source because copying is forbidden.
     * @throws Nothing because this operation is deleted.
     */
    DeviceLease(const DeviceLease& other) = delete;

    /**
     * @brief Prevents assigning duplicate device authority.
     * @param other Unused source because copying is forbidden.
     * @return No value because this operation is deleted.
     * @throws Nothing because this operation is deleted.
     */
    DeviceLease& operator=(const DeviceLease& other) = delete;

    /**
     * @brief Reports whether this lease owns a nonzero exact vector.
     * @return True before movement or destruction.
     * @throws Nothing.
     */
    bool active() const noexcept;

    /**
     * @brief Returns the exact device for this lease.
     * @return Configured device identity, or CPU sentinel when inactive.
     * @throws Nothing.
     */
    DeviceId device() const noexcept;

    /**
     * @brief Copies the exact committed vector.
     * @return Owned bytes, or zero when inactive.
     * @throws Nothing.
     */
    DeviceResourceVector resources() const noexcept;

   private:
    friend class DeviceReservation;

    /**
     * @brief Creates an active exact lease.
     * @param root Matching shared root authority.
     * @param device Exact configured device.
     * @param resources Nonzero exact committed vector.
     * @throws Nothing.
     */
    DeviceLease(std::shared_ptr<ResourceLedgerRootState> root, DeviceId device,
                DeviceResourceVector resources) noexcept;

    /**
     * @brief Creates an inactive zero lease for a zero actual component.
     * @throws Nothing.
     */
    DeviceLease() noexcept = default;

    /**
     * @brief Returns this exact vector and makes the lease inactive.
     * @return Nothing.
     * @throws Nothing; invariant or synchronization failure terminates.
     */
    void reset() noexcept;

    /** @brief Shared root retained through native/completion lifetime. */
    std::shared_ptr<ResourceLedgerRootState> root_;

    /** @brief Exact device account debited by this lease. */
    DeviceId device_{DeviceBackend::CPU};

    /** @brief Exact vector returned once. */
    DeviceResourceVector resources_;
  };

  /**
   * @brief Independently owned leases returned by actual-byte commit.
   *
   * A zero actual component produces an inactive lease. Nonzero memory and
   * scratch leases release independently, so completion may return scratch
   * while a persistent native owner retains memory.
   *
   * @throws Nothing after staged lease construction.
   */
  struct DeviceLeasePair final {
    /** @brief Persistent device-memory authority for a native owner. */
    DeviceLease persistent_memory;

    /** @brief Invocation/transfer scratch authority for completion ownership.
     */
    DeviceLease scratch;
  };

  /**
   * @brief Move-only exact root commitment minted only by the ledger.
   *
   * Destroying this owner closes future child creation. When children remain,
   * root release is deferred until the last child releases.
   */
  class Reservation final {
   public:
    /**
     * @brief Transfers one reservation authority.
     * @param other Reservation made inactive by the transfer.
     * @throws Nothing.
     */
    Reservation(Reservation&& other) noexcept;

    /**
     * @brief Replaces this authority after releasing its prior ownership.
     * @param other Reservation made inactive by the transfer.
     * @return Reference to this owner.
     * @throws Nothing; an internal synchronization failure terminates because
     * this operation participates in exact RAII release.
     */
    Reservation& operator=(Reservation&& other) noexcept;

    /**
     * @brief Closes child creation and releases when all children are gone.
     * @throws Nothing; an internal synchronization failure terminates.
     */
    ~Reservation() noexcept;

    /**
     * @brief Prevents duplicating root authority.
     * @param other Reservation whose authority cannot be copied.
     * @throws Nothing because this operation is unavailable.
     */
    Reservation(const Reservation& other) = delete;

    /**
     * @brief Prevents assigning duplicated root authority.
     * @param other Reservation whose authority cannot be copied.
     * @return No value because this operation is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    Reservation& operator=(const Reservation& other) = delete;

    /**
     * @brief Reports whether this owner can still mint child grants.
     * @return True before movement or destruction closes the owner.
     * @throws std::system_error when internal mutex locking fails.
     */
    bool active() const;

    /**
     * @brief Copies the exact root vector committed for this owner.
     * @return Committed vector, or the zero vector after movement.
     * @throws std::system_error when internal mutex locking fails.
     */
    ResourceVector resources() const;

    /**
     * @brief Copies currently ungranted capacity within this reservation.
     * @return Component-wise committed capacity minus live child grants.
     * @throws std::system_error when internal mutex locking fails.
     */
    ResourceVector available() const;

    /**
     * @brief Mints one exact child grant within committed capacity.
     * @param requested Complete child vector.
     * @return Move-only grant, or `std::nullopt` without state change when the
     * owner is closed or any dimension lacks capacity.
     * @throws std::system_error when internal mutex locking fails.
     * @note Authority construction is staged before state mutation, so
     * exceptional exits cannot consume a partial child vector.
     */
    std::optional<Grant> try_grant(const ResourceVector& requested);

   private:
    friend class ResourceLedger;

    /**
     * @brief Wraps one ledger-created reservation state.
     * @param state Opaque state whose parent ownership is initially live.
     * @throws Nothing.
     */
    explicit Reservation(
        std::shared_ptr<ResourceReservationState> state) noexcept;

    /**
     * @brief Performs exact close/release for destruction and move assignment.
     * @return Nothing.
     * @throws Nothing; synchronization failures terminate.
     */
    void reset() noexcept;

    /** @brief Sole shared handle authorizing child grant creation. */
    std::shared_ptr<ResourceReservationState> state_;
  };

  /**
   * @brief Move-only exact child grant held by queued or executing work.
   *
   * A grant cannot create another grant or enlarge its vector. Destruction
   * returns its exact vector to the parent reservation once.
   */
  class Grant final {
   public:
    /**
     * @brief Transfers one child authority.
     * @param other Grant made inactive by the transfer.
     * @throws Nothing.
     */
    Grant(Grant&& other) noexcept;

    /**
     * @brief Replaces this authority after exact prior release.
     * @param other Grant made inactive by the transfer.
     * @return Reference to this grant.
     * @throws Nothing; synchronization failures terminate.
     */
    Grant& operator=(Grant&& other) noexcept;

    /**
     * @brief Returns the exact child vector to its parent once.
     * @throws Nothing; synchronization failures terminate.
     */
    ~Grant() noexcept;

    /**
     * @brief Prevents duplicating child authority.
     * @param other Grant whose authority cannot be copied.
     * @throws Nothing because this operation is unavailable.
     */
    Grant(const Grant& other) = delete;

    /**
     * @brief Prevents assigning duplicated child authority.
     * @param other Grant whose authority cannot be copied.
     * @return No value because this operation is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    Grant& operator=(const Grant& other) = delete;

    /**
     * @brief Reports whether this value still owns a child vector.
     * @return True before movement or destruction.
     * @throws Nothing.
     */
    bool active() const noexcept;

    /**
     * @brief Copies the exact child vector.
     * @return Granted vector, or the zero vector after movement.
     * @throws Nothing.
     */
    ResourceVector resources() const noexcept;

   private:
    friend class Reservation;

    /**
     * @brief Wraps one parent state and exact granted vector.
     * @param state Parent reservation state.
     * @param resources Exact child vector already committed in that state.
     * @throws Nothing.
     */
    Grant(std::shared_ptr<ResourceReservationState> state,
          ResourceVector resources) noexcept;

    /**
     * @brief Returns this child vector and performs deferred root release.
     * @return Nothing.
     * @throws Nothing; synchronization failures terminate.
     */
    void reset() noexcept;

    /** @brief Parent state retaining root authority through child lifetime. */
    std::shared_ptr<ResourceReservationState> state_;

    /** @brief Exact vector returned once by `reset()`. */
    ResourceVector resources_;
  };

  /**
   * @brief Two root owners committed by one all-or-none transaction.
   * @throws Nothing after both reservation states have been allocated.
   */
  struct ReservationPair final {
    /** @brief First independent owner, conventionally HP. */
    Reservation first;

    /** @brief Second independent owner, conventionally RT. */
    Reservation second;
  };

  /**
   * @brief Immutable diagnostic snapshot without minting authority.
   * @throws Nothing for value copying.
   */
  struct Snapshot final {
    /** @brief Immutable configured composition-root limits. */
    ResourceVector limits;

    /** @brief Current complete root commitments. */
    ResourceVector reserved;

    /**
     * @brief Component-wise lifetime maximum of complete root commitments.
     * @note Values advance only in the same transaction that successfully
     * commits `reserved`; release never lowers them and they mint no capacity.
     */
    ResourceVector high_water;
  };

  /**
   * @brief Immutable plugin quota, commitment, and high-water snapshot.
   * @throws Nothing for value copying.
   * @note Replay tombstones are intentionally not exposed or mutable here.
   */
  struct PluginSnapshot final {
    /** @brief Immutable configured isolated-plugin limits. */
    PluginResourceVector limits;

    /** @brief Current issued or consumed exact commitments. */
    PluginResourceVector reserved;

    /** @brief Component-wise lifetime maximum of successful commitments. */
    PluginResourceVector high_water;
  };

  /**
   * @brief Immutable diagnostic snapshot for one exact device.
   * @throws Nothing for value copying.
   */
  struct DeviceSnapshot final {
    /** @brief Complete configured concrete device identity. */
    DeviceId device{DeviceBackend::Metal};

    /** @brief Immutable configured memory and scratch limits. */
    DeviceResourceVector limits;

    /** @brief Current planned or committed byte ownership. */
    DeviceResourceVector reserved;

    /**
     * @brief Component-wise lifetime maximum device byte ownership.
     * @note Values advance only on successful device reservation and grant no
     * allocation authority.
     */
    DeviceResourceVector high_water;

    /** @brief Checked component-wise limit minus reservation. */
    DeviceResourceVector available;
  };

  /**
   * @brief Returns the structural bytes allocated for one reservation state.
   * @return Reservation state object plus shared-allocation control payload.
   * @throws Nothing.
   * @note This is a source-tree accounting aid for `ExecutionService`; it does
   * not expose state layout, authority, or allocator-private metadata.
   */
  static std::uint64_t reservation_state_retained_memory_bytes() noexcept;

  /**
   * @brief Creates one independent ledger with immutable composition limits.
   * @param limits Maximum committed Host vector.
   * @param device_limits Immutable non-CPU per-device byte limits.
   * @param plugin_limits Immutable isolated-plugin aggregate limits.
   * @throws std::invalid_argument for CPU or duplicate device limits.
   * @throws std::bad_alloc when root state allocation fails.
   * @note Zero limits are valid and reject only positive requests.
   */
  explicit ResourceLedger(ResourceVector limits,
                          std::vector<DeviceResourceLimit> device_limits = {},
                          PluginResourceVector plugin_limits = {});

  /**
   * @brief Releases root state after every outstanding owner is gone.
   * @throws Nothing.
   * @note Outstanding reservations/grants retain their shared root state until
   * their own exact releases complete.
   */
  ~ResourceLedger() noexcept;

  /**
   * @brief Prevents duplicating one ledger authority.
   * @param other Ledger whose authority cannot be copied.
   * @throws Nothing because this operation is unavailable.
   */
  ResourceLedger(const ResourceLedger& other) = delete;

  /**
   * @brief Prevents assigning duplicated ledger authority.
   * @param other Ledger whose authority cannot be copied.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ResourceLedger& operator=(const ResourceLedger& other) = delete;

  /**
   * @brief Prevents moving authority away from its composition owner.
   * @param other Ledger whose authority cannot be transferred.
   * @throws Nothing because this operation is unavailable.
   */
  ResourceLedger(ResourceLedger&& other) = delete;

  /**
   * @brief Prevents move-assigning ledger authority.
   * @param other Ledger whose authority cannot be transferred.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ResourceLedger& operator=(ResourceLedger&& other) = delete;

  /**
   * @brief Atomically commits one complete root vector.
   * @param requested Checked resource demand.
   * @param release_observer Optional non-authoritative owner retained only for
   * a successful reservation and notified after its exact physical release.
   * @param settlement_observer Optional non-owning exact-settlement callback.
   * @return Move-only reservation, or `std::nullopt` without state change when
   * any dimension lacks capacity.
   * @throws std::bad_alloc when reservation state allocation fails.
   * @throws std::system_error when internal mutex locking fails.
   * @note A caller coupling admission to companion accounting must hold the
   * observer's transaction mutex across this call and its own successful
   * charge. The settlement observer context must remain alive until callback.
   * The ledger remains the sole capacity authority.
   */
  std::optional<Reservation> try_reserve(
      const ResourceVector& requested,
      std::shared_ptr<ReservationReleaseObserver> release_observer = nullptr,
      ReservationSettlementObserver settlement_observer = {});

  /**
   * @brief Atomically commits two independently owned root vectors.
   * @param first First exact demand.
   * @param second Second exact demand.
   * @return Two move-only owners, or `std::nullopt` without state change when
   * addition overflows or the combined vector lacks capacity.
   * @throws std::bad_alloc when either reservation state allocation fails.
   * @throws std::system_error when internal mutex locking fails.
   */
  std::optional<ReservationPair> try_reserve_pair(const ResourceVector& first,
                                                  const ResourceVector& second);

  /**
   * @brief Atomically commits one complete device allocation plan.
   * @param device Exact configured non-CPU device.
   * @param planned Complete persistent-memory and scratch plan.
   * @return Move-only planned reservation, or `std::nullopt` without mutation
   * for an unknown device, overflow, or exhausted dimension.
   * @throws std::system_error when root synchronization fails.
   * @note Admission is immediate and linearizable; it provides no FIFO wait,
   * starvation guarantee, implicit retry, or cross-device borrowing.
   */
  std::optional<DeviceReservation> try_reserve_device(
      DeviceId device, const DeviceResourceVector& planned);

  /**
   * @brief Atomically spends one identity and reserves its complete vector.
   * @param identity Domain-separated complete invocation identity digest.
   * @param requested Exact Host-derived plugin resource demand.
   * @return Move-only one-use token owning the committed vector.
   * @throws PluginResourceAdmissionError for replay or quota exhaustion.
   * @throws std::bad_alloc when replay state cannot grow.
   * @throws std::system_error when root synchronization fails.
   * @note Replay lookup, quota checks, tombstone insertion, reservation, and
   * high-water update occur under one root lock. Quota failure inserts no
   * tombstone and no failure partially commits capacity.
   */
  PluginResourceToken issue_plugin_invocation(
      const PluginInvocationIdentityDigest& identity,
      const PluginResourceVector& requested);

  /**
   * @brief Copies limits, current commitments, and lifetime high-water values.
   * @return Non-authoritative immutable snapshot.
   * @throws std::system_error when internal mutex locking fails.
   */
  Snapshot snapshot() const;

  /**
   * @brief Copies isolated-plugin limits, commitments, and high-water values.
   * @return Non-authoritative immutable snapshot.
   * @throws std::system_error when root synchronization fails.
   */
  PluginSnapshot plugin_snapshot() const;

  /**
   * @brief Copies one configured device account and its lifetime high-water.
   * @param device Device identity to inspect.
   * @return Immutable snapshot, or `std::nullopt` when unconfigured.
   * @throws std::system_error when root synchronization fails.
   */
  std::optional<DeviceSnapshot> device_snapshot(DeviceId device) const;

  /**
   * @brief Copies every configured device account in deterministic order.
   * @return Snapshots ordered by `DeviceId::operator<`.
   * @throws std::bad_alloc from result allocation.
   * @throws std::system_error when root synchronization fails.
   */
  std::vector<DeviceSnapshot> device_snapshots() const;

 private:
  /** @brief Shared root retained until all deferred releases complete. */
  std::shared_ptr<ResourceLedgerRootState> state_;
};

}  // namespace ps
