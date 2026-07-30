#pragma once

#include <cstddef>
#include <cstdint>

#include "photospider/core/device.hpp"
#include "photospider/memory/buffer_handle.hpp"

/**
 * @file access_plan.hpp
 * @brief Dependency-neutral explicit device-access planning values.
 */

namespace ps {

class Value;
class ValueRevisionId;

/**
 * @brief Exhaustive result of one consumer access-planning decision.
 *
 * @throws Nothing for ordinary value operations.
 * @note No outcome performs work by itself. `Transfer`, `Map`, and `Import`
 *       require a separately owned execution object.
 */
enum class AccessPlanKind : std::uint32_t {
  /** @brief Existing binding is directly eligible after stated visibility. */
  Direct = 0U,
  /** @brief Existing allocation requires an explicit mapping scope. */
  Map = 1U,
  /** @brief Existing allocation requires an explicit backend import. */
  Import = 2U,
  /** @brief A distinct destination binding must be produced. */
  Transfer = 3U,
  /** @brief No registered provider can satisfy the requested capability. */
  Unsupported = 4U,
};

/**
 * @brief Consumer authority produced after an access plan is discharged.
 *
 * @throws Nothing for ordinary value operations.
 * @note This classification does not itself own a lease or destination. It
 * lets callers distinguish observation-only direct use from scoped host/map/
 * import access and revision-preserving destination publication.
 */
enum class AccessLeaseKind : std::uint32_t {
  /** @brief No scoped payload authority is produced. */
  None = 0U,
  /** @brief A retaining immutable host ReadLease is required. */
  HostRead = 1U,
  /** @brief A future provider must issue a scoped mapping lease. */
  Mapping = 2U,
  /** @brief A future provider must issue a scoped imported-resource lease. */
  Imported = 3U,
  /** @brief Transfer publishes a distinct immutable destination Value. */
  DestinationValue = 4U,
};

/**
 * @brief Visibility obligations attached to an explicit access plan.
 *
 * @throws Nothing for ordinary value operations.
 * @note These observations grant no synchronization capability. The owning
 *       plan executor must discharge every true obligation before access.
 */
struct VisibilityObligations final {
  /** @brief Consumer must wait asynchronously for producer completion. */
  bool await_producer = false;
  /** @brief Backend cache or managed-memory synchronization is required. */
  bool synchronize_memory = false;
  /** @brief Queue or ownership-family transfer is required. */
  bool transfer_ownership = false;

  /**
   * @brief Compares every visibility obligation.
   * @param other Obligations to compare.
   * @return True only when all flags match.
   * @throws Nothing.
   */
  constexpr bool operator==(const VisibilityObligations& other) const noexcept {
    return await_producer == other.await_producer &&
           synchronize_memory == other.synchronize_memory &&
           transfer_ownership == other.transfer_ownership;
  }
};

/**
 * @brief Explicit target capability requested by one Value consumer.
 *
 * @throws Nothing for ordinary value operations after DeviceId construction.
 * @note `host_read` requests a scoped immutable host pointer; device and
 *       domain alone never imply that capability.
 */
struct AccessTarget final {
  /** @brief Concrete target device. */
  DeviceId device{DeviceBackend::CPU};
  /** @brief Preferred target allocation domain. */
  MemoryDomain memory_domain = MemoryDomain::Host;
  /** @brief Whether the resulting access must issue a host ReadLease. */
  bool host_read = false;
  /** @brief Whether the consumer requires a distinct physical allocation. */
  bool require_distinct_binding = false;
};

/**
 * @brief Immutable classified plan for accessing one Value on a target.
 *
 * @throws Nothing for construction and ordinary observation.
 * @note The plan contains identity and resource observations only. It owns no
 *       native command, fence completer, mapping, queue, or mutable payload.
 */
class AccessPlan final {
 public:
  /**
   * @brief Creates one classified access plan.
   * @param kind Exhaustive access outcome.
   * @param source_revision Nonzero source logical revision scalar.
   * @param source_binding Complete immutable source allocation facts.
   * @param target Requested consumer capability.
   * @param visibility Explicit visibility obligations.
   * @param transfer_bytes Exact Value range moved by transfer outcomes.
   * @throws Nothing.
   * @note Revision is stored as its scalar to keep this header independent
   *       from the complete Value definition.
   */
  constexpr AccessPlan(AccessPlanKind kind, std::uint64_t source_revision,
                       StorageBinding source_binding, AccessTarget target,
                       VisibilityObligations visibility,
                       std::size_t transfer_bytes) noexcept
      : kind_(kind),
        source_revision_(source_revision),
        source_binding_(source_binding),
        target_(target),
        visibility_(visibility),
        transfer_bytes_(transfer_bytes) {}

  /**
   * @brief Returns the exhaustive access outcome.
   * @return Direct, Map, Import, Transfer, or Unsupported.
   * @throws Nothing.
   */
  constexpr AccessPlanKind kind() const noexcept { return kind_; }
  /**
   * @brief Returns the nonzero source ValueRevisionId scalar.
   * @return Process-local logical revision token.
   * @throws Nothing.
   */
  constexpr std::uint64_t source_revision() const noexcept {
    return source_revision_;
  }
  /**
   * @brief Returns complete immutable source binding facts.
   * @return Allocation, device, domain, byte envelope, and host visibility.
   * @throws Nothing.
   * @note Observation grants no pointer, mapping, native handle, or transfer.
   */
  constexpr StorageBinding source_binding() const noexcept {
    return source_binding_;
  }
  /**
   * @brief Returns the concrete source device.
   * @return Process-local source DeviceId.
   * @throws Nothing.
   */
  constexpr DeviceId source_device() const noexcept {
    return source_binding_.device;
  }
  /**
   * @brief Returns the source memory domain.
   * @return Immutable source allocation domain.
   * @throws Nothing.
   */
  constexpr MemoryDomain source_domain() const noexcept {
    return source_binding_.memory_domain;
  }
  /**
   * @brief Returns the requested target capability.
   * @return Complete target device/domain/access requirements.
   * @throws Nothing.
   */
  constexpr AccessTarget target() const noexcept { return target_; }
  /**
   * @brief Returns every visibility obligation.
   * @return Readiness, coherency, and ownership work observations.
   * @throws Nothing.
   */
  constexpr VisibilityObligations visibility() const noexcept {
    return visibility_;
  }
  /**
   * @brief Returns the scoped authority produced after plan execution.
   * @return HostRead, Mapping, Imported, DestinationValue, or None.
   * @throws Nothing.
   * @note V-8 executes Direct host reads and Transfer destinations; general
   * Map and Import providers remain future work.
   */
  constexpr AccessLeaseKind lease_kind() const noexcept {
    switch (kind_) {
      case AccessPlanKind::Direct:
        return target_.host_read ? AccessLeaseKind::HostRead
                                 : AccessLeaseKind::None;
      case AccessPlanKind::Map:
        return AccessLeaseKind::Mapping;
      case AccessPlanKind::Import:
        return AccessLeaseKind::Imported;
      case AccessPlanKind::Transfer:
        return AccessLeaseKind::DestinationValue;
      case AccessPlanKind::Unsupported:
        return AccessLeaseKind::None;
    }
    return AccessLeaseKind::None;
  }
  /**
   * @brief Returns the exact Value range moved by Transfer.
   * @return Source Value storage size for Transfer, otherwise zero.
   * @throws Nothing.
   */
  constexpr std::size_t transfer_bytes() const noexcept {
    return transfer_bytes_;
  }

 private:
  /** @brief Exhaustive classified outcome. */
  AccessPlanKind kind_ = AccessPlanKind::Unsupported;
  /** @brief Source ValueRevisionId scalar. */
  std::uint64_t source_revision_ = 0U;
  /** @brief Complete immutable source allocation/binding facts. */
  StorageBinding source_binding_;
  /** @brief Requested consumer target. */
  AccessTarget target_;
  /** @brief Visibility work required before access. */
  VisibilityObligations visibility_;
  /** @brief Observational transfer envelope. */
  std::size_t transfer_bytes_ = 0U;
};

/**
 * @brief Classifies access to one immutable Value without payload access.
 * @param value Valid Value whose binding facts are inspected.
 * @param target Explicit consumer capability.
 * @return Direct, Transfer, or Unsupported plan for the current V-8 subset.
 * @throws std::invalid_argument when value is invalid.
 * @throws std::logic_error for inconsistent retained binding facts.
 * @note This function polls only producer state without waiting, touches no
 * payload, and queues no work. V-8 does not yet implement general Map or
 * Import providers.
 */
AccessPlan plan_value_access(const Value& value, AccessTarget target);

}  // namespace ps
