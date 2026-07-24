/**
 * @file resource_ledger.hpp
 * @brief Declares host-authoritative checked resource admission and grants.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

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

/** @brief Opaque shared root state retained by outstanding reservations. */
struct ResourceLedgerRootState;

/** @brief Opaque shared reservation state retained by outstanding grants. */
struct ResourceReservationState;

/**
 * @brief Host-authoritative resource reservation and child-grant ledger.
 *
 * One ledger owns immutable composition-root limits. Root reservations commit
 * complete checked vectors atomically. A live reservation may suballocate only
 * within its committed vector, and root capacity remains committed until both
 * the reservation owner is gone and every child grant has released.
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
   * @param limits Maximum committed vector.
   * @throws std::bad_alloc when root state allocation fails.
   * @note Zero limits are valid and reject only positive requests.
   */
  explicit ResourceLedger(ResourceVector limits);

  /**
   * @brief Releases root state after every outstanding owner is gone.
   * @throws Nothing.
   * @note Outstanding reservations/grants retain their shared root state until
   * their own exact releases complete.
   */
  ~ResourceLedger() noexcept;

  /**
   * @brief Prevents duplicating one Host authority.
   * @param other Ledger whose authority cannot be copied.
   * @throws Nothing because this operation is unavailable.
   */
  ResourceLedger(const ResourceLedger& other) = delete;

  /**
   * @brief Prevents assigning duplicated Host authority.
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
   * @brief Prevents move-assigning Host authority.
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
   * @brief Copies limits and current root commitments for diagnostics/tests.
   * @return Non-authoritative immutable snapshot.
   * @throws std::system_error when internal mutex locking fails.
   */
  Snapshot snapshot() const;

 private:
  /** @brief Shared root retained until all deferred releases complete. */
  std::shared_ptr<ResourceLedgerRootState> state_;
};

}  // namespace ps
