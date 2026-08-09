/**
 * @file tenant_quota.cpp
 * @brief Implements Issue #99 atomic tenant quota admission and settlement.
 */
#include "server/tenant_quota.hpp"

#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace ps::server {
namespace {

/** @brief Guards the post-publication reservation return as a no-throw move. */
static_assert(std::is_nothrow_move_constructible_v<TenantQuotaReservation>,
              "quota reservation must move without throwing after admission");

/**
 * @brief Returns checked scalar addition for quota validation.
 * @param current Current accounted value.
 * @param requested Additional requested value.
 * @return Exact sum.
 * @throws std::overflow_error when addition cannot be represented.
 */
std::uint64_t checked_add(std::uint64_t current, std::uint64_t requested) {
  if (requested > std::numeric_limits<std::uint64_t>::max() - current) {
    throw std::overflow_error("tenant quota accounting overflowed");
  }
  return current + requested;
}

/**
 * @brief Throws one typed rejection when a checked sum exceeds capacity.
 * @param current Current accounted value.
 * @param requested Additional requested value.
 * @param capacity Configured inclusive capacity.
 * @param dimension Exact rejected dimension.
 * @param label Stable diagnostic label.
 * @return Checked sum when admission fits.
 * @throws TenantQuotaExceeded for arithmetic overflow or excess capacity.
 */
std::uint64_t admitted_sum(std::uint64_t current, std::uint64_t requested,
                           std::uint64_t capacity,
                           TenantQuotaDimension dimension,
                           std::string_view label) {
  if (requested > capacity || current > capacity - requested) {
    throw TenantQuotaExceeded(
        dimension,
        std::string("tenant quota exceeded for ") + std::string(label));
  }
  return current + requested;
}

/**
 * @brief Returns configured device capacity keyed by label.
 * @param request Canonical configured-device vector.
 * @return Ordered label/capacity map.
 * @throws std::bad_alloc when map allocation fails.
 */
std::map<std::string, std::uint64_t> device_map(
    const JobResourceRequest& request) {
  std::map<std::string, std::uint64_t> result;
  for (const DeviceResourceRequest& device : request.devices) {
    result.emplace(device.device_id, device.bytes);
  }
  return result;
}

/**
 * @brief Validates that one complete active request can be subtracted.
 * @param request Exact retained reservation request.
 * @param usage Current usage inspected without mutation.
 * @return Nothing.
 * @throws std::logic_error when internal accounting is inconsistent.
 * @note Caller holds the authority mutex. This helper never mutates `usage`.
 */
void validate_request_subtraction(const JobResourceRequest& request,
                                  const TenantQuotaSnapshot& usage) {
  if (usage.active_attempts == 0U || usage.cpu_slots < request.cpu_slots ||
      usage.host_memory_bytes < request.host_memory_bytes ||
      usage.output_bytes < request.output_bytes ||
      usage.staging_bytes < request.staging_bytes ||
      usage.retention_bytes < request.retention_bytes) {
    throw std::logic_error("tenant quota active accounting is inconsistent");
  }
  for (const DeviceResourceRequest& device : request.devices) {
    const auto found = usage.device_bytes.find(device.device_id);
    if (found == usage.device_bytes.end() || found->second < device.bytes) {
      throw std::logic_error("tenant device quota accounting is inconsistent");
    }
  }
}

/**
 * @brief Subtracts one validated complete active request from current usage.
 * @param request Exact retained reservation request.
 * @param usage Non-null current usage mutated after invariant checks.
 * @return Nothing.
 * @throws std::logic_error when internal accounting is inconsistent.
 * @note Caller holds the authority mutex. Every scalar and device invariant is
 * checked before the first subtraction, so an exception preserves `usage`.
 */
void subtract_request(const JobResourceRequest& request,
                      TenantQuotaSnapshot* usage) {
  if (usage == nullptr) {
    throw std::logic_error("tenant quota usage owner is null");
  }
  validate_request_subtraction(request, *usage);
  --usage->active_attempts;
  usage->cpu_slots -= request.cpu_slots;
  usage->host_memory_bytes -= request.host_memory_bytes;
  usage->output_bytes -= request.output_bytes;
  usage->staging_bytes -= request.staging_bytes;
  usage->retention_bytes -= request.retention_bytes;
  for (const DeviceResourceRequest& device : request.devices) {
    usage->device_bytes.at(device.device_id) -= device.bytes;
  }
}

}  // namespace

/** @copydoc ps::server::TenantQuotaAuthority::TenantQuotaAuthority */
TenantQuotaAuthority::TenantQuotaAuthority(TenantId tenant_id,
                                           TenantQuotaLimits limits,
                                           TenantQuotaAuthorityOptions options)
    : tenant_id_(std::move(tenant_id)),
      limits_(std::move(limits)),
      options_(std::move(options)) {
  if (!tenant_id_.valid() || limits_.maximum_active_attempts == 0U) {
    throw std::invalid_argument(
        "tenant quota identity or concurrency is invalid");
  }
  validate_job_resource_request(limits_.capacity);
  usage_.device_bytes = device_map(limits_.capacity);
  for (auto& device : usage_.device_bytes) {
    device.second = 0U;
  }
}

/** @copydoc ps::server::TenantQuotaAuthority::recover_retained_artifact */
void TenantQuotaAuthority::recover_retained_artifact(
    const ArtifactId& artifact_id, std::uint64_t payload_bytes) {
  if (!artifact_id.valid() || payload_bytes == 0U) {
    throw std::invalid_argument("recovered artifact quota charge is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = retained_artifacts_.find(artifact_id.value());
  if (found != retained_artifacts_.end()) {
    if (found->second != payload_bytes) {
      throw std::logic_error("recovered artifact quota charge conflicts");
    }
    return;
  }
  const std::uint64_t retained = admitted_sum(
      usage_.retention_bytes, payload_bytes, limits_.capacity.retention_bytes,
      TenantQuotaDimension::Retention, "retention");
  retained_artifacts_.emplace(artifact_id.value(), payload_bytes);
  usage_.retention_bytes = retained;
  usage_.retained_artifacts = retained_artifacts_.size();
}

/** @copydoc ps::server::TenantQuotaAuthority::reserve */
TenantQuotaReservation TenantQuotaAuthority::reserve(
    const JobId& job_id, const JobResourceRequest& request) {
  if (!job_id.valid()) {
    throw std::invalid_argument("tenant quota Job identity is invalid");
  }
  validate_job_resource_request(request);
  std::lock_guard<std::mutex> lock(mutex_);
  if (usage_.active_attempts >= limits_.maximum_active_attempts) {
    throw TenantQuotaExceeded(TenantQuotaDimension::Concurrency,
                              "tenant quota exceeded for concurrency");
  }
  const std::uint64_t cpu = admitted_sum(usage_.cpu_slots, request.cpu_slots,
                                         limits_.capacity.cpu_slots,
                                         TenantQuotaDimension::Cpu, "CPU");
  const std::uint64_t host =
      admitted_sum(usage_.host_memory_bytes, request.host_memory_bytes,
                   limits_.capacity.host_memory_bytes,
                   TenantQuotaDimension::HostMemory, "host memory");
  const std::uint64_t output = admitted_sum(
      usage_.output_bytes, request.output_bytes, limits_.capacity.output_bytes,
      TenantQuotaDimension::Output, "output");
  const std::uint64_t staging = admitted_sum(
      usage_.staging_bytes, request.staging_bytes,
      limits_.capacity.staging_bytes, TenantQuotaDimension::Staging, "staging");
  const std::uint64_t retention =
      admitted_sum(usage_.retention_bytes, request.retention_bytes,
                   limits_.capacity.retention_bytes,
                   TenantQuotaDimension::Retention, "retention");

  std::map<std::string, std::uint64_t> devices = usage_.device_bytes;
  const std::map<std::string, std::uint64_t> capacity =
      device_map(limits_.capacity);
  for (const DeviceResourceRequest& device : request.devices) {
    const auto configured = capacity.find(device.device_id);
    if (configured == capacity.end()) {
      throw TenantQuotaExceeded(
          TenantQuotaDimension::Device,
          "tenant quota has no configured device named " + device.device_id);
    }
    devices.at(device.device_id) = admitted_sum(
        devices.at(device.device_id), device.bytes, configured->second,
        TenantQuotaDimension::Device, "device " + device.device_id);
  }
  if (next_reservation_sequence_ == 0U) {
    throw std::overflow_error("tenant quota reservation identity exhausted");
  }
  const QuotaReservationId id("quota-v1-" +
                              std::to_string(next_reservation_sequence_));
  TenantQuotaReservation receipt{id, job_id, request};
  ReservationRecord record{job_id, request};
  reservations_.emplace(id.value(), std::move(record));
  ++next_reservation_sequence_;
  usage_.active_attempts += 1U;
  usage_.cpu_slots = cpu;
  usage_.host_memory_bytes = host;
  usage_.output_bytes = output;
  usage_.staging_bytes = staging;
  usage_.retention_bytes = retention;
  usage_.device_bytes = std::move(devices);
  return receipt;
}

/** @copydoc ps::server::TenantQuotaAuthority::release_attempt */
void TenantQuotaAuthority::release_attempt(
    const QuotaReservationId& reservation_id) {
  if (!reservation_id.valid()) {
    throw std::invalid_argument("tenant quota reservation identity is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = reservations_.find(reservation_id.value());
  if (found == reservations_.end()) {
    throw std::logic_error("tenant quota reservation is absent or settled");
  }
  validate_request_subtraction(found->second.request, usage_);
  if (options_.release_attempt_observer) {
    options_.release_attempt_observer();
  }
  subtract_request(found->second.request, &usage_);
  reservations_.erase(found);
}

/** @copydoc ps::server::TenantQuotaAuthority::commit_retained_artifact */
void TenantQuotaAuthority::commit_retained_artifact(
    const QuotaReservationId& reservation_id, const ArtifactId& artifact_id,
    std::uint64_t payload_bytes) {
  if (!reservation_id.valid() || !artifact_id.valid() || payload_bytes == 0U) {
    throw std::invalid_argument(
        "committed artifact quota settlement is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto reservation = reservations_.find(reservation_id.value());
  if (reservation == reservations_.end()) {
    throw std::logic_error("tenant quota reservation is absent or settled");
  }
  if (payload_bytes > reservation->second.request.retention_bytes) {
    throw std::logic_error("artifact charge exceeds reserved retention");
  }
  const auto retained = retained_artifacts_.find(artifact_id.value());
  if (retained != retained_artifacts_.end() &&
      retained->second != payload_bytes) {
    throw std::logic_error("artifact retention charge conflicts");
  }

  TenantQuotaSnapshot settled_usage = usage_;
  std::unordered_map<std::string, std::uint64_t> settled_retained =
      retained_artifacts_;
  subtract_request(reservation->second.request, &settled_usage);
  if (retained == retained_artifacts_.end()) {
    const std::uint64_t charged =
        checked_add(settled_usage.retention_bytes, payload_bytes);
    if (charged > limits_.capacity.retention_bytes) {
      throw std::logic_error(
          "settled artifact exceeds reserved retention truth");
    }
    settled_retained.emplace(artifact_id.value(), payload_bytes);
    settled_usage.retention_bytes = charged;
  }
  settled_usage.retained_artifacts = settled_retained.size();
  if (options_.retained_artifact_commit_observer) {
    options_.retained_artifact_commit_observer();
  }
  reservations_.erase(reservation_id.value());
  retained_artifacts_ = std::move(settled_retained);
  usage_ = std::move(settled_usage);
}

/** @copydoc ps::server::TenantQuotaAuthority::release_retained_artifact */
std::uint64_t TenantQuotaAuthority::release_retained_artifact(
    const ArtifactId& artifact_id) {
  if (!artifact_id.valid()) {
    throw std::invalid_argument("retained artifact identity is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = retained_artifacts_.find(artifact_id.value());
  if (found == retained_artifacts_.end()) {
    return 0U;
  }
  if (usage_.retention_bytes < found->second) {
    throw std::logic_error("tenant retained quota accounting is inconsistent");
  }
  const std::uint64_t released = found->second;
  usage_.retention_bytes -= released;
  retained_artifacts_.erase(found);
  usage_.retained_artifacts = retained_artifacts_.size();
  return released;
}

/** @copydoc ps::server::TenantQuotaAuthority::snapshot */
TenantQuotaSnapshot TenantQuotaAuthority::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return usage_;
}

}  // namespace ps::server
