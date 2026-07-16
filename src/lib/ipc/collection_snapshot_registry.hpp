#pragma once

#include <algorithm>
#include <any>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "ipc/protocol_bounds.hpp"

namespace ps::ipc::internal {

/**
 * @brief Exact private identity frozen around one full-value Host call.
 *
 * @throws std::bad_alloc when owned text cannot be allocated.
 * @note `original_params` is a router-owned canonical identity, not a public
 *       JSON envelope. Empty `session_id` denotes a process-global method.
 */
struct CollectionSnapshotBinding {
  /** @brief Exact version 1 method name. */
  std::string method;

  /** @brief Frozen opaque session id, or empty for a global method. */
  std::string session_id;

  /** @brief Canonical identity of the original non-page request parameters. */
  std::string original_params;
};

/**
 * @brief Expected bounded-registry outcome independent of wire encoding.
 *
 * @throws Nothing.
 * @note The router maps these private outcomes to `invalid_params`,
 *       `capacity_exceeded`, `response_too_large`, or `cursor_not_found`.
 */
enum class CollectionSnapshotError {
  /** @brief The operation completed successfully. */
  None,
  /** @brief Page shape, limit, or arithmetic is invalid. */
  InvalidParams,
  /** @brief Slot or pre-Host byte reservation cannot be admitted. */
  CapacityExceeded,
  /** @brief The measured full Host value exceeds one snapshot bound. */
  ResponseTooLarge,
  /** @brief Cursor is absent, expired, out of order, or binding-mismatched. */
  CursorNotFound,
  /** @brief Registry admission has stopped for daemon shutdown. */
  Stopped,
};

/**
 * @brief Injectable snapshot count, byte, page, and lifetime policy.
 *
 * @throws Nothing.
 * @note Production reserves 64 MiB before Host access, retains at most 64
 *       snapshots and 256 MiB, and never refreshes the 15-minute TTL.
 */
struct CollectionSnapshotLimits {
  /** @brief Maximum published records plus outstanding reservations. */
  std::size_t records = 64;

  /** @brief Maximum retained bytes plus outstanding byte reservations. */
  std::size_t total_bytes = 256U * 1024U * 1024U;

  /** @brief Exact byte amount reserved before every full-value Host call. */
  std::size_t reservation_bytes = kSnapshotMaxBytes;

  /** @brief Maximum recursive public entries in one measured Host result. */
  std::size_t snapshot_entries = kSnapshotMaxEntries;

  /** @brief Maximum bytes accepted from one measured Host result. */
  std::size_t snapshot_bytes = kSnapshotMaxBytes;

  /** @brief Maximum entries copied into one response page. */
  std::size_t page_entries = kGeneralPageMaxEntries;

  /** @brief Fixed lifetime measured from cursor publication. */
  std::chrono::steady_clock::duration ttl = std::chrono::minutes(15);
};

/**
 * @brief Private bounded owner of type-erased stable collection snapshots.
 *
 * A caller reserves one slot and the configured worst-case bytes before its
 * one existing full-value Host call. Publication then measures through caller
 * supplied exact recursive-entry/byte counts, moves the public collection into
 * private storage, adjusts quota to actual retained bytes, and emits ordered
 * pages.
 * Later pages use only frozen record state and never resolve a live session.
 *
 * @throws std::bad_alloc when callbacks or registry storage cannot allocate.
 * @throws std::invalid_argument for an invalid limits policy.
 * @note This class owns no JSON, Host, backend, or public ABI type. It supports
 *       heterogeneous `std::vector<T>` snapshots through private type erasure.
 */
class CollectionSnapshotRegistry {
 public:
  /**
   * @brief Monotonic clock callback used by TTL decisions.
   *
   * @return A `steady_clock` sample that is not earlier than any sample this
   *         callback previously returned to the same registry.
   * @throws Whatever the injected callable throws.
   * @note The registry invokes this callback synchronously while holding its
   *       mutex, so calls from one registry are serialized. The callable must
   *       not re-enter that registry, wait for work that can enter it, or
   *       acquire locks in an order that can form a cycle with the registry
   *       mutex. Any captured state borrowed by reference must remain valid
   *       until the registry is destroyed.
   */
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  /**
   * @brief Opaque 32-lowercase-hex cursor generator callback.
   *
   * @return A candidate cursor containing exactly 32 lowercase hexadecimal
   *         characters.
   * @throws Whatever the injected callable throws.
   * @note The registry may invoke this callback repeatedly for collision
   *       recovery while holding its mutex, so calls from one registry are
   *       serialized. The callable must not re-enter that registry, wait for
   *       work that can enter it, or acquire locks in an order that can form a
   *       cycle with the registry mutex. Any captured state borrowed by
   *       reference must remain valid until the registry is destroyed.
   */
  using IdGenerator = std::function<std::string()>;

  /**
   * @brief Move-only ownership of one pre-Host slot/byte reservation.
   *
   * @throws Nothing while moved or destroyed.
   * @note Destruction rolls back an unpublished reservation. The registry must
   *       outlive every reservation issued by it.
   */
  class Reservation {
   public:
    /** @brief Creates an inactive reservation. @throws Nothing. */
    Reservation() = default;

    /** @brief Rolls back an unpublished reservation. @throws Nothing. */
    ~Reservation() noexcept;

    /** @brief Prevents duplicate quota ownership. @throws Nothing. */
    Reservation(const Reservation&) = delete;

    /**
     * @brief Prevents duplicate quota ownership by assignment.
     * @return No value because copying is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    Reservation& operator=(const Reservation&) = delete;

    /**
     * @brief Transfers reservation ownership.
     * @param other Reservation made inactive.
     * @throws Nothing.
     */
    Reservation(Reservation&& other) noexcept;

    /**
     * @brief Rolls back current ownership and transfers another reservation.
     * @param other Reservation made inactive.
     * @return This reservation.
     * @throws Nothing.
     */
    Reservation& operator=(Reservation&& other) noexcept;

    /**
     * @brief Reports whether this value owns unpublished quota.
     * @return True only before publish, cancellation, or shutdown invalidation.
     * @throws Nothing.
     */
    bool active() const noexcept;

   private:
    friend class CollectionSnapshotRegistry;

    /**
     * @brief Creates active ownership for one registry generation.
     * @param owner Registry whose quota is held.
     * @param token Unique reservation token.
     * @throws Nothing.
     */
    Reservation(CollectionSnapshotRegistry* owner, std::size_t token) noexcept;

    /** @brief Owning registry while active. */
    CollectionSnapshotRegistry* owner_ = nullptr;

    /** @brief Unique reservation token used for shutdown-safe cancellation. */
    std::size_t token_ = 0;
  };

  /**
   * @brief Result of pre-Host quota admission.
   *
   * @throws Nothing while moved when no owned callback allocates.
   */
  struct ReserveResult {
    /** @brief Expected admission outcome. */
    CollectionSnapshotError error = CollectionSnapshotError::None;

    /** @brief Active quota ownership only when `error` is None. */
    Reservation reservation;
  };

  /**
   * @brief Typed copied page returned by publication or later lookup.
   *
   * @tparam T Stable copied public row type.
   * @throws Whatever copying `T` throws.
   */
  template <typename T>
  struct PageResult {
    /** @brief Expected operation outcome. */
    CollectionSnapshotError error = CollectionSnapshotError::None;

    /** @brief Stable cursor only while a later page remains. */
    std::optional<std::string> cursor;

    /** @brief Exact zero-based offset represented by this page. */
    std::size_t offset = 0;

    /** @brief Ordered copy of at most the requested page limit. */
    std::vector<T> entries;

    /** @brief True only while the retained snapshot has later entries. */
    bool has_more = false;
  };

  /**
   * @brief Creates a stopped registry with production or injected policy.
   *
   * @param limits Count, byte, page, and TTL policy.
   * @param clock Monotonic clock; empty selects `steady_clock::now`.
   * @param id_generator Cursor source; empty selects OS entropy.
   * @throws std::bad_alloc when callback storage cannot be allocated.
   * @throws std::invalid_argument for zero/inconsistent limits or nonpositive
   *         TTL.
   */
  explicit CollectionSnapshotRegistry(CollectionSnapshotLimits limits = {},
                                      Clock clock = {},
                                      IdGenerator id_generator = {});

  /** @brief Prevents copying mutex and quota ownership. @throws Nothing. */
  CollectionSnapshotRegistry(const CollectionSnapshotRegistry&) = delete;

  /**
   * @brief Prevents copying mutex and quota ownership by assignment.
   * @return No value because copying is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  CollectionSnapshotRegistry& operator=(const CollectionSnapshotRegistry&) =
      delete;

  /**
   * @brief Starts or restarts empty snapshot admission.
   * @return Nothing.
   * @throws Nothing.
   * @note Restart is valid only after `finish_shutdown()` cleared prior state.
   */
  void start() noexcept;

  /**
   * @brief Atomically reserves one record and worst-case snapshot bytes.
   * @return Active reservation or CapacityExceeded/Stopped before Host access.
   * @throws std::bad_alloc if reservation tracking cannot allocate.
   * @throws std::runtime_error if the reservation-token counter wraps and its
   *         bounded collision search cannot find an unused nonzero token.
   * @throws Whatever the injected monotonic clock callback throws.
   * @note Expired records are removed first. Success changes no cursor state.
   */
  ReserveResult reserve();

  /**
   * @brief Publishes a measured collection and returns its first page.
   *
   * @tparam T Stable copied public row type.
   * @param reservation Active quota reserved before the Host call.
   * @param binding Frozen method/session/original-params identity.
   * @param entries Complete Host-returned public collection moved on success.
   * @param measured_entries Exact recursive public vector/map/fixed-array entry
   *        count from the value codec. This may exceed `entries.size()` because
   *        retained rows can contain nested public collections, but it must
   *        never be smaller than that top-level row count.
   * @param measured_bytes Exact retained byte accounting from the value codec.
   * @param page_limit Requested first-page size in `1..page_entries`.
   * @param retained_page_limit Optional stricter frame-safe ceiling retained
   *        for every continuation; the default preserves caller-selected page
   *        sizes up to the registry policy.
   * @return First page, or transactional expected rejection.
   * @throws std::bad_alloc if cursor, page, or record storage cannot allocate.
   * @throws std::runtime_error if the id source returns malformed data or its
   *         collision retry bound is exhausted.
   * @throws Whatever the injected id/clock callbacks or copying `T` into the
   *         response page throws.
   * @note Oversize and every exception roll back reservation quota without a
   *       published cursor or retained snapshot copy. The first page uses the
   *       smaller of requested and retained frame-safe limits.
   */
  template <typename T>
  PageResult<T> publish(Reservation reservation,
                        CollectionSnapshotBinding binding,
                        std::vector<T>&& entries, std::size_t measured_entries,
                        std::size_t measured_bytes, std::size_t page_limit,
                        std::size_t retained_page_limit =
                            std::numeric_limits<std::size_t>::max());

  /**
   * @brief Reads exactly the next ordered page from frozen snapshot state.
   *
   * @tparam T Stable copied public row type selected at publication.
   * @param cursor Well-formed stable cursor from a preceding page.
   * @param binding Exact frozen identity supplied to publication.
   * @param offset Required next zero-based offset.
   * @param page_limit Requested size in `1..page_entries`.
   * @return Ordered page; final success atomically releases the record.
   * @throws std::bad_alloc or whatever the injected clock/copying `T` throws;
   *         failure leaves the record and offset unchanged.
   * @note The fixed publication TTL is never refreshed. No session lookup or
   *       Host call occurs. A publication-time frame-safe ceiling may return
   *       fewer than the requested entries; callers advance by the returned
   *       page size and may request a smaller later limit.
   */
  template <typename T>
  PageResult<T> page(const std::string& cursor,
                     const CollectionSnapshotBinding& binding,
                     std::size_t offset, std::size_t page_limit);

  /**
   * @brief Stops new reservations while preserving pages already admitted.
   * @return Nothing.
   * @throws Nothing.
   */
  void begin_shutdown() noexcept;

  /**
   * @brief Releases all records/reservations and permits a later empty restart.
   * @return Nothing.
   * @throws Nothing.
   */
  void finish_shutdown() noexcept;

  /**
   * @brief Returns current retained record count for deterministic tests.
   * @return Number of published records, excluding reservations.
   * @throws Nothing.
   */
  std::size_t record_count() const noexcept;

  /**
   * @brief Returns exact current retained byte accounting.
   * @return Sum of measured bytes for published records.
   * @throws Nothing.
   */
  std::size_t retained_bytes() const noexcept;

 private:
  /**
   * @brief Owns one type-erased immutable collection and its paging state.
   *
   * Publication constructs the complete record while `mutex_` is held, then
   * moves it into `records_` before the matching reservation is consumed.
   * The record owns its frozen binding and concrete `std::vector<T>` through
   * `std::any`; no field borrows caller, Host, session, or codec storage.
   * `next_offset` is mutated only by a successful page copy. `expires_at` is
   * fixed at publication and is never refreshed by lookup.
   *
   * @throws std::bad_alloc if owned binding or type-erased storage cannot be
   *         allocated.
   * @throws Whatever construction or movement of the stored vector throws.
   * @note Construction, lookup, offset mutation, TTL expiry, and destruction
   *       occur only while `mutex_` is held. Final-page, TTL, or shutdown
   *       erasure destroys the owned collection and releases measured quota.
   */
  struct Record {
    /** @brief Exact identity frozen at publication. */
    CollectionSnapshotBinding binding;
    /** @brief `std::vector<T>` owned without introducing a public wire type. */
    std::any entries;
    /** @brief Exact concrete vector type used to reject mismatched reads. */
    std::type_index type = typeid(void);
    /** @brief Actual top-level row count used only for paging invariants. */
    std::size_t entry_count = 0;
    /** @brief Exact measured retained byte accounting. */
    std::size_t measured_bytes = 0;
    /** @brief Publication-time frame-safe ceiling for every later page. */
    std::size_t page_entry_limit = 1;
    /** @brief Next ordered offset accepted by `page()`. */
    std::size_t next_offset = 0;
    /** @brief Fixed expiry set once at cursor publication. */
    std::chrono::steady_clock::time_point expires_at;
  };

  /**
   * @brief Validates and atomically consumes one active reservation.
   * @param reservation Candidate ownership to consume on success.
   * @param retained_bytes Exact bytes transferred to published ownership;
   *        zero for an empty or single-page publication with no retained
   *        record.
   * @return True only when the reservation belongs to this registry and its
   *         token remains active in the current lifecycle.
   * @throws Nothing.
   * @note Caller holds `mutex_`. Success erases the token, replaces its
   *       worst-case reserved quota with `retained_bytes`, and makes the
   *       passed value inactive at the same linearization point. Failure
   *       changes neither registry quota nor the reservation.
   */
  bool consume_reservation_locked(Reservation* reservation,
                                  std::size_t retained_bytes) noexcept;

  /**
   * @brief Cancels one active reservation token.
   * @param token Unique issued token.
   * @return Nothing.
   * @throws Nothing.
   */
  void cancel_reservation(std::size_t token) noexcept;

  /**
   * @brief Checks whether one reservation token is still owned.
   * @param token Candidate token.
   * @return True only while the token holds pre-Host quota.
   * @throws Nothing.
   */
  bool reservation_active(std::size_t token) const noexcept;

  /**
   * @brief Validates the exact private cursor representation.
   * @param cursor Candidate cursor bytes.
   * @return True only for 32 lowercase hexadecimal characters.
   * @throws Nothing.
   * @note Defined out of line to keep this header independent from JSON codec
   *       implementation and dependencies.
   */
  static bool valid_cursor(std::string_view cursor) noexcept;

  /**
   * @brief Removes records whose fixed publication TTL has elapsed.
   * @param now Current monotonic time sampled by the caller.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_`.
   */
  void expire_locked(std::chrono::steady_clock::time_point now) noexcept;

  /**
   * @brief Computes a fixed TTL deadline without duration overflow.
   * @param now Publication-time monotonic clock sample.
   * @return `now + ttl`, saturated at the clock time-point maximum.
   * @throws Nothing.
   */
  std::chrono::steady_clock::time_point expiration_at(
      std::chrono::steady_clock::time_point now) const noexcept;

  /**
   * @brief Erases one record and releases its exact measured quota.
   * @param iterator Existing record iterator.
   * @return Iterator following the erased record.
   * @throws Nothing.
   * @note Caller holds `mutex_`.
   */
  std::map<std::string, Record>::iterator erase_locked(
      std::map<std::string, Record>::iterator iterator) noexcept;

  /** @brief Serializes admission, cursor order, TTL, and shutdown state. */
  mutable std::mutex mutex_;

  /** @brief Immutable injected policy. */
  CollectionSnapshotLimits limits_;

  /** @brief Monotonic clock callback. */
  Clock clock_;

  /** @brief Stable opaque cursor generator callback. */
  IdGenerator id_generator_;

  /** @brief Published records keyed by stable cursor. */
  std::map<std::string, Record> records_;

  /** @brief Outstanding reservation tokens. */
  std::map<std::size_t, bool> reservations_;

  /** @brief Exact bytes retained by published records. */
  std::size_t retained_bytes_ = 0;

  /** @brief Exact worst-case bytes held by active reservations. */
  std::size_t reserved_bytes_ = 0;

  /** @brief Monotonic token source; zero is never issued. */
  std::size_t next_reservation_token_ = 1;

  /** @brief True only while new pre-Host reservations are admitted. */
  bool accepting_ = false;
};

/** @copydoc CollectionSnapshotRegistry::publish */
template <typename T>
CollectionSnapshotRegistry::PageResult<T> CollectionSnapshotRegistry::publish(
    Reservation reservation, CollectionSnapshotBinding binding,
    std::vector<T>&& entries, std::size_t measured_entries,
    std::size_t measured_bytes, std::size_t page_limit,
    std::size_t retained_page_limit) {
  static_assert(std::is_copy_constructible<T>::value,
                "snapshot page entries must be copy constructible");
  PageResult<T> result;
  if (page_limit == 0 || page_limit > limits_.page_entries ||
      retained_page_limit == 0) {
    result.error = CollectionSnapshotError::InvalidParams;
    return result;
  }
  if (measured_entries < entries.size()) {
    result.error = CollectionSnapshotError::InvalidParams;
    return result;
  }
  if (measured_entries > limits_.snapshot_entries ||
      measured_bytes > limits_.snapshot_bytes) {
    result.error = CollectionSnapshotError::ResponseTooLarge;
    return result;
  }

  const std::size_t stored_page_limit =
      std::min(retained_page_limit, limits_.page_entries);
  const std::size_t first_count =
      std::min(entries.size(), std::min(page_limit, stored_page_limit));
  result.entries.reserve(first_count);
  result.entries.insert(
      result.entries.end(), entries.begin(),
      entries.begin() + static_cast<std::ptrdiff_t>(first_count));
  result.has_more = first_count < entries.size();
  if (!result.has_more) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!consume_reservation_locked(&reservation, 0)) {
      result.entries.clear();
      result.error = CollectionSnapshotError::InvalidParams;
    }
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (reservation.owner_ != this ||
      reservations_.count(reservation.token_) == 0) {
    result.entries.clear();
    result.has_more = false;
    result.error = CollectionSnapshotError::InvalidParams;
    return result;
  }

  std::string cursor;
  for (std::size_t attempt = 0; attempt < 128; ++attempt) {
    cursor = id_generator_();
    if (!valid_cursor(cursor)) {
      throw std::runtime_error(
          "collection cursor generator returned invalid data");
    }
    if (records_.count(cursor) == 0) {
      break;
    }
    cursor.clear();
  }
  if (cursor.empty()) {
    throw std::runtime_error("collection cursor collision limit reached");
  }

  const auto now = clock_();
  const std::size_t entry_count = entries.size();
  Record record{std::move(binding),
                std::any(std::move(entries)),
                std::type_index(typeid(std::vector<T>)),
                entry_count,
                measured_bytes,
                stored_page_limit,
                first_count,
                expiration_at(now)};

  result.cursor = cursor;
  const auto inserted = records_.emplace(cursor, std::move(record));
  if (!inserted.second) {
    throw std::runtime_error("collection cursor reservation invariant failed");
  }
  if (!consume_reservation_locked(&reservation, measured_bytes)) {
    records_.erase(inserted.first);
    result.cursor.reset();
    result.entries.clear();
    result.has_more = false;
    result.error = CollectionSnapshotError::InvalidParams;
  }
  return result;
}

/** @copydoc CollectionSnapshotRegistry::page */
template <typename T>
CollectionSnapshotRegistry::PageResult<T> CollectionSnapshotRegistry::page(
    const std::string& cursor, const CollectionSnapshotBinding& binding,
    std::size_t offset, std::size_t page_limit) {
  static_assert(std::is_copy_constructible<T>::value,
                "snapshot page entries must be copy constructible");
  PageResult<T> result;
  result.offset = offset;
  if (!valid_cursor(cursor) || page_limit == 0 ||
      page_limit > limits_.page_entries ||
      offset > std::numeric_limits<std::size_t>::max() - page_limit) {
    result.error = CollectionSnapshotError::InvalidParams;
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = clock_();
  expire_locked(now);
  const auto found = records_.find(cursor);
  if (found == records_.end()) {
    result.error = CollectionSnapshotError::CursorNotFound;
    return result;
  }
  Record& record = found->second;
  if (record.binding.method != binding.method ||
      record.binding.session_id != binding.session_id ||
      record.binding.original_params != binding.original_params ||
      record.type != std::type_index(typeid(std::vector<T>)) ||
      offset != record.next_offset) {
    result.error = CollectionSnapshotError::CursorNotFound;
    return result;
  }
  const auto* entries = std::any_cast<std::vector<T>>(&record.entries);
  if (entries == nullptr || record.entry_count != entries->size() ||
      offset > entries->size()) {
    result.error = CollectionSnapshotError::CursorNotFound;
    return result;
  }

  const std::size_t effective_limit =
      std::min(page_limit, record.page_entry_limit);
  const std::size_t end = std::min(entries->size(), offset + effective_limit);
  result.entries.reserve(end - offset);
  result.entries.insert(result.entries.end(),
                        entries->begin() + static_cast<std::ptrdiff_t>(offset),
                        entries->begin() + static_cast<std::ptrdiff_t>(end));
  result.has_more = end < entries->size();
  if (result.has_more) {
    result.cursor = cursor;
    record.next_offset = end;
  } else {
    (void)erase_locked(found);
  }
  return result;
}

}  // namespace ps::ipc::internal
