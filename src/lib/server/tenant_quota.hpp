/**
 * @file tenant_quota.hpp
 * @brief Declares Issue #99 atomic tenant quota admission and settlement.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "server/job_contract.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/** @brief Tag for one process-local server quota reservation identity. */
struct QuotaReservationIdDomain final {};
/** @brief Opaque exact-attempt server quota reservation identity. */
using QuotaReservationId = OpaqueTextId<QuotaReservationIdDomain>;

/**
 * @brief Closed tenant quota dimension used by typed admission failures.
 * @throws Nothing for value operations.
 */
enum class TenantQuotaDimension : std::uint8_t {
  /** @brief Maximum simultaneously active attempt count. */
  Concurrency,
  /** @brief Server CPU-slot accounting. */
  Cpu,
  /** @brief Declared host-memory accounting. */
  HostMemory,
  /** @brief One configured device capacity. */
  Device,
  /** @brief Active output payload capacity. */
  Output,
  /** @brief Active private staging capacity. */
  Staging,
  /** @brief Reserved plus committed durable retention capacity. */
  Retention,
};

/**
 * @brief Typed fail-closed tenant quota admission rejection.
 * @throws std::bad_alloc only while constructing diagnostic storage.
 * @note Rejection occurs before any partial reservation is published.
 */
class TenantQuotaExceeded final : public std::runtime_error {
 public:
  /**
   * @brief Creates one dimension-specific admission rejection.
   * @param dimension First exact capacity dimension that rejected admission.
   * @param message Human-readable bounded diagnostic.
   * @throws std::bad_alloc when storing the diagnostic exhausts memory.
   */
  TenantQuotaExceeded(TenantQuotaDimension dimension, std::string message)
      : std::runtime_error(std::move(message)), dimension_(dimension) {}

  /**
   * @brief Returns the exact rejecting dimension.
   * @return Closed tenant quota dimension.
   * @throws Nothing.
   */
  TenantQuotaDimension dimension() const noexcept { return dimension_; }

 private:
  /** @brief Exact dimension whose capacity check failed. */
  TenantQuotaDimension dimension_;
};

/**
 * @brief Trusted total capacity configured for one tenant authority.
 * @throws Nothing for default/value operations; vector copies may allocate.
 * @note `capacity.retention_bytes` governs active reservations plus committed
 * durable payload charges. Metadata and filesystem allocation overhead are
 * trusted service infrastructure rather than tenant bulk-payload quota.
 */
struct TenantQuotaLimits final {
  /** @brief Positive maximum simultaneous active attempt reservations. */
  std::size_t maximum_active_attempts = 0U;
  /** @brief Positive scalar and configured-device total capacity. */
  JobResourceRequest capacity;
};

/**
 * @brief Complete immutable view of current tenant quota accounting.
 * @throws Nothing for default/value operations; map copies may allocate.
 */
struct TenantQuotaSnapshot final {
  /** @brief Number of live attempt reservations. */
  std::size_t active_attempts = 0U;
  /** @brief CPU slots reserved by active attempts. */
  std::uint64_t cpu_slots = 0U;
  /** @brief Host-memory bytes declared by active attempts. */
  std::uint64_t host_memory_bytes = 0U;
  /** @brief Output bytes reserved by active attempts. */
  std::uint64_t output_bytes = 0U;
  /** @brief Staging bytes reserved by active attempts. */
  std::uint64_t staging_bytes = 0U;
  /** @brief Active reservations plus exact committed payload retention. */
  std::uint64_t retention_bytes = 0U;
  /** @brief Active configured-device bytes keyed by exact configured label. */
  std::map<std::string, std::uint64_t> device_bytes;
  /** @brief Number of committed artifacts carrying retained charges. */
  std::size_t retained_artifacts = 0U;
};

/**
 * @brief One successful atomic complete-envelope reservation receipt.
 * @throws Nothing for default/value operations; copied identities may allocate.
 * @note The receipt is observation, not a forgeable release capability. Only
 * `TenantQuotaAuthority` validates and mutates its retained reservation map.
 */
struct TenantQuotaReservation final {
  /** @brief Fresh process-local reservation identity. */
  QuotaReservationId id;
  /** @brief Durable Job whose exact attempt owns the reservation. */
  JobId job_id;
  /** @brief Exact completely reserved demand. */
  JobResourceRequest request;
};

/**
 * @brief Sole server-side quota authority for one configured tenant.
 *
 * Admission performs checked component-wise validation and changes all usage
 * plus the reservation map under one mutex, or changes nothing. Attempt
 * settlement releases the complete reservation exactly once. Successful
 * artifact commit converts reserved retention to exact durable payload usage;
 * restart imports only validated retained artifacts, never active attempts.
 *
 * @throws Constructor rejects invalid limits. Public mutation methods throw
 * `std::invalid_argument`, `std::logic_error`, `std::overflow_error`,
 * `TenantQuotaExceeded`, or allocation/synchronization failures as documented.
 * @note This class does not mint `ResourceLedger` grants, enforce OS memory or
 * device bounds, own paths, or expose reservation mutation to workers/plugins.
 */
class TenantQuotaAuthority final {
 public:
  /**
   * @brief Creates one empty authority for a validated tenant capacity.
   * @param tenant_id Valid configured tenant identity.
   * @param limits Positive complete capacity and concurrency limit.
   * @throws std::invalid_argument for invalid tenant or limits.
   * @throws std::bad_alloc when storing configured device capacity fails.
   */
  TenantQuotaAuthority(TenantId tenant_id, TenantQuotaLimits limits);

  /**
   * @brief Prevents duplicate ownership of one quota truth.
   * @param other Authority that cannot be copied.
   */
  TenantQuotaAuthority(const TenantQuotaAuthority& other) = delete;

  /**
   * @brief Prevents duplicate assignment of one quota truth.
   * @param other Authority that cannot be copied.
   * @return No assignment result because the operation is deleted.
   */
  TenantQuotaAuthority& operator=(const TenantQuotaAuthority& other) = delete;

  /**
   * @brief Imports one validated committed artifact during restart recovery.
   * @param artifact_id Valid tenant-scoped committed artifact identity.
   * @param payload_bytes Positive exact retained payload charge.
   * @return Nothing after one idempotent exact charge is present.
   * @throws std::invalid_argument for invalid identity or zero charge.
   * @throws TenantQuotaExceeded when recovered retention exceeds configuration.
   * @throws std::logic_error when the same artifact has a different charge.
   * @throws std::overflow_error for checked accounting overflow.
   * @note Call before accepting new attempts. Duplicate exact recovery is a
   * no-op and never double charges.
   */
  void recover_retained_artifact(const ArtifactId& artifact_id,
                                 std::uint64_t payload_bytes);

  /**
   * @brief Atomically reserves one complete active attempt envelope.
   * @param job_id Valid durable Job identity.
   * @param request Valid complete Job demand.
   * @return Fresh reservation receipt after every dimension is charged.
   * @throws std::invalid_argument for invalid inputs.
   * @throws TenantQuotaExceeded when any dimension lacks capacity.
   * @throws std::overflow_error for reservation identity/accounting overflow.
   * @throws std::bad_alloc before publication if retained storage fails.
   * @note Any exception leaves all usage and reservations unchanged.
   */
  TenantQuotaReservation reserve(const JobId& job_id,
                                 const JobResourceRequest& request);

  /**
   * @brief Releases one failed/cancelled attempt reservation exactly once.
   * @param reservation_id Current reservation returned by `reserve`.
   * @return Nothing after the complete active envelope is removed.
   * @throws std::invalid_argument for an invalid id.
   * @throws std::logic_error when the id is absent or already settled.
   */
  void release_attempt(const QuotaReservationId& reservation_id);

  /**
   * @brief Settles success by converting reservation to retained payload use.
   * @param reservation_id Current exact attempt reservation.
   * @param artifact_id Stable committed artifact identity.
   * @param payload_bytes Positive exact committed tight payload length.
   * @return Nothing after active dimensions are released and retention is
   * charged exactly once.
   * @throws std::invalid_argument for invalid identities or zero charge.
   * @throws std::logic_error for absent reservation, charge above its retained
   * bound, or same artifact with a conflicting charge.
   * @throws std::overflow_error for impossible accounting overflow.
   * @note An artifact already recovered/charged with the exact size makes this
   * an idempotent conversion rather than a second retention charge.
   */
  void commit_retained_artifact(const QuotaReservationId& reservation_id,
                                const ArtifactId& artifact_id,
                                std::uint64_t payload_bytes);

  /**
   * @brief Releases one durably deleted artifact's retention charge.
   * @param artifact_id Exact artifact whose authoritative manifest is gone.
   * @return True when one charge was removed; false when already absent.
   * @throws std::invalid_argument for an invalid identity.
   */
  bool release_retained_artifact(const ArtifactId& artifact_id);

  /**
   * @brief Captures one mutex-consistent usage view.
   * @return Complete current active and retained accounting.
   * @throws std::bad_alloc when copying configured-device usage fails.
   * @throws std::system_error for synchronization failure.
   */
  TenantQuotaSnapshot snapshot() const;

 private:
  /** @brief Internal complete active reservation owner. */
  struct ReservationRecord final {
    /** @brief Durable Job owning the current attempt reservation. */
    JobId job_id;
    /** @brief Exact complete charged request. */
    JobResourceRequest request;
  };

  /** @brief Configured tenant identity. */
  TenantId tenant_id_;
  /** @brief Immutable trusted total quota limits. */
  TenantQuotaLimits limits_;
  /** @brief Serializes reservations, usage, and retained charges. */
  mutable std::mutex mutex_;
  /** @brief Current complete accounting. */
  TenantQuotaSnapshot usage_;
  /** @brief Exact current reservation records keyed by opaque id text. */
  std::unordered_map<std::string, ReservationRecord> reservations_;
  /** @brief Exact retained payload charges keyed by ArtifactId text. */
  std::unordered_map<std::string, std::uint64_t> retained_artifacts_;
  /** @brief Next checked nonzero process-local reservation sequence. */
  std::uint64_t next_reservation_sequence_ = 1U;
};

}  // namespace ps::server
