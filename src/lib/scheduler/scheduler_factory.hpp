/**
 * @file scheduler_factory.hpp
 * @brief Declares pure scheduler planning and concrete scheduler construction.
 */
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"
#include "runtime/resource_ledger.hpp"

namespace ps {

/**
 * @brief Immutable scheduler construction and Host-ledger admission plan.
 *
 * @throws std::bad_alloc If copied scheduler type storage cannot allocate.
 * @note Only `SchedulerFactory` can construct a plan, so callers cannot forge
 * a grant or reservation charge that bypasses type-specific planning rules.
 */
class SchedulerPlan final {
 public:
  /**
   * @brief Returns the exact built-in alias or registered scheduler type.
   * @return Borrowed type string valid for this plan's lifetime.
   * @throws Nothing.
   */
  const std::string& type_name() const noexcept;

  /**
   * @brief Returns the resolved worker grant passed to construction.
   * @return Zero only for built-in `serial_debug`; otherwise a value in
   * `[1,8]`.
   * @throws Nothing.
   */
  unsigned int worker_grant() const noexcept;

  /**
   * @brief Returns the conservative CPU slots charged before construction.
   * @return Zero for built-in serial, the grant for CPU/plugin, or grant plus
   * one potential GPU worker for GPU/heterogeneous plans.
   * @throws Nothing.
   */
  unsigned int reservation_slots() const noexcept;

  /**
   * @brief Reports whether planning froze the built-in CPU branch.
   * @return True only for built-in CPU aliases resolved without plugin lookup.
   * @throws Nothing.
   * @note Kernel uses this private planning fact to publish ownerless
   * service routes for both intents. Scheduler display names are not route
   * authority.
   */
  bool is_builtin_cpu() const noexcept;

 private:
  friend class SchedulerFactory;

  /** @brief Construction branch frozen when the type is planned. */
  enum class Kind {
    /** @brief Built-in caller-thread serial scheduler. */
    Serial,
    /** @brief Built-in CPU work-stealing scheduler. */
    Cpu,
    /** @brief Built-in GPU pipeline or heterogeneous alias. */
    Gpu,
    /** @brief Any non-hardcoded current loader registration. */
    Plugin
  };

  /**
   * @brief Creates one already-validated immutable plan.
   * @param type_name Exact type/alias used for later construction.
   * @param kind Frozen built-in or registered construction branch.
   * @param worker_grant Resolved constructor grant.
   * @param reservation_slots Conservative Host-ledger CPU charge.
   * @throws Nothing after argument construction; strings move into ownership.
   */
  SchedulerPlan(std::string type_name, Kind kind, unsigned int worker_grant,
                unsigned int reservation_slots) noexcept;

  /** @brief Exact type or alias resolved during planning. */
  std::string type_name_;
  /** @brief Frozen construction branch unavailable to plan consumers. */
  Kind kind_ = Kind::Serial;
  /** @brief Nonzero resolved grant except for built-in serial. */
  unsigned int worker_grant_ = 0U;
  /** @brief Exact Host-ledger CPU capacity required before construction. */
  unsigned int reservation_slots_ = 0U;
};

/**
 * @brief Plans and constructs built-in or registered scheduler instances.
 *
 * @throws Scheduler construction and plugin-boundary failures as documented by
 * the selected operation.
 * @note Planning is side-effect free: it creates no scheduler, worker, plugin
 * instance, or reservation. Reserved construction transfers Host-ledger CPU
 * capacity into the returned scheduler lifetime owner.
 */
class SchedulerFactory final {
 public:
  /**
   * @brief Plans one scheduler using platform hardware concurrency.
   * @param type_name Built-in alias or currently registered scheduler type.
   * @param num_workers Zero for bounded automatic resolution or exact `[1,8]`.
   * @return Immutable plan, or `std::nullopt` for an unknown type.
   * @throws std::invalid_argument If a known type receives more than eight.
   * @throws std::overflow_error If type-specific slot arithmetic overflows.
   * @throws std::system_error If registered-type lookup locking fails.
   * @throws std::bad_alloc If plan or registry snapshot storage cannot
   * allocate.
   * @note This function creates no scheduler or worker and acquires no budget.
   */
  static std::optional<SchedulerPlan> plan(const std::string& type_name,
                                           unsigned int num_workers = 0U);

  /**
   * @brief Plans one scheduler with deterministic hardware detection input.
   * @param type_name Built-in alias or currently registered scheduler type.
   * @param num_workers Zero for bounded automatic resolution or exact `[1,8]`.
   * @param detected_hardware_workers Hardware concurrency; zero is unavailable.
   * @return Immutable plan, or `std::nullopt` for an unknown type.
   * @throws std::invalid_argument If a known type receives more than eight.
   * @throws std::overflow_error If type-specific slot arithmetic overflows.
   * @throws std::system_error If registered-type lookup locking fails.
   * @throws std::bad_alloc If plan or registry snapshot storage cannot
   * allocate.
   * @note Tests use this overload to prove automatic resolution without
   * replacing the platform detector. It creates no scheduler or worker.
   */
  static std::optional<SchedulerPlan> plan_for_hardware(
      const std::string& type_name, unsigned int num_workers,
      unsigned int detected_hardware_workers);

  /**
   * @brief Constructs a reservation-owned scheduler from a factory plan.
   * @param plan Validated plan whose grant is passed exactly to construction.
   * @param reservation Active exact-size admission transferred to the returned
   * scheduler owner.
   * @return Owning scheduler, or nullptr if a previously registered plugin type
   * became unavailable or its factory returned null.
   * @throws std::invalid_argument If the reservation is inactive or its slots
   * do not exactly match `plan.reservation_slots()`.
   * @throws std::bad_alloc If scheduler or owner allocation fails.
   * @throws std::system_error If registered scheduler lookup locking fails.
   * @throws GraphError After normalizing a plugin creation failure.
   * @throws Any built-in scheduler constructor exception unchanged.
   * @note Any null/exceptional construction exit destroys a created concrete
   * scheduler before returning the transferred reservation. Successful output
   * releases capacity only after concrete scheduler destruction.
   */
  static std::unique_ptr<IScheduler> create(
      const SchedulerPlan& plan, ResourceLedger::Reservation reservation);

  /**
   * @brief Returns hard-coded built-in scheduler types and aliases.
   * @return Built-in names in stable presentation order.
   * @throws std::bad_alloc If result storage cannot allocate.
   * @note Registered plugin types are exposed by the loader/interaction layer.
   */
  static std::vector<std::string> supported_types();

  /**
   * @brief Tests whether one built-in or registered type can currently resolve.
   * @param type_name Scheduler type to query.
   * @return True for built-ins/aliases or a current loader registration.
   * @throws std::system_error If registry mutex locking fails.
   */
  static bool is_supported(const std::string& type_name);

  /**
   * @brief Returns a human-readable scheduler type description.
   * @param type_name Built-in, alias, or registered scheduler type.
   * @return Copied description or `"Unknown scheduler type"`.
   * @throws std::system_error If registry mutex locking fails.
   * @throws std::bad_alloc If result or registry snapshot storage cannot
   * allocate.
   */
  static std::string description(const std::string& type_name);
};

}  // namespace ps
