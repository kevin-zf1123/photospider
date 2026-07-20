#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "scheduler/scheduler_factory.hpp"
#include "scheduler/scheduler_plugin_loader.hpp"
#include "scheduler/scheduler_worker_limits.hpp"

namespace ps {
namespace {

constexpr char kPlanningOnlyRegisteredType[] = "issue_43_planning_scheduler";

/**
 * @brief Returns shared construction-call evidence retained by the registry.
 * @return Stable process-lifetime counter owner.
 * @throws std::bad_alloc On first access if shared allocation fails.
 */
std::shared_ptr<std::atomic<unsigned int>> planning_create_calls() {
  static const auto calls = std::make_shared<std::atomic<unsigned int>>(0U);
  return calls;
}

/**
 * @brief Registers one non-hardcoded scheduler type exactly once for planning.
 * @return Nothing.
 * @throws std::bad_alloc If loader registry or callable storage cannot
 * allocate.
 * @note The retained factory only records accidental construction; planning
 * tests never invoke it, and shared counter ownership avoids dangling captures.
 */
void ensure_planning_registered_type() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto calls = planning_create_calls();
    SchedulerPluginLoader::instance().register_builtin(
        kPlanningOnlyRegisteredType, "planning-only registered scheduler",
        [calls](unsigned int) -> std::unique_ptr<IScheduler> {
          calls->fetch_add(1U, std::memory_order_relaxed);
          return nullptr;
        });
  });
}

/**
 * @brief Proves an unknown type produces no scheduler plan.
 * @throws Nothing when planning returns `std::nullopt` without construction.
 */
TEST(SchedulerFactoryPlanning, UnknownTypeHasNoPlan) {
  EXPECT_FALSE(
      SchedulerFactory::plan_for_hardware("issue_43_unknown_scheduler", 0U, 64U)
          .has_value());
}

/**
 * @brief Proves built-in serial is the sole zero-grant, zero-charge plan.
 * @throws Nothing when automatic and explicit legal requests remain workerless.
 */
TEST(SchedulerFactoryPlanning, SerialDebugConsumesNoWorkerSlots) {
  const auto automatic =
      SchedulerFactory::plan_for_hardware("serial_debug", 0U, 64U);
  ASSERT_TRUE(automatic.has_value());
  EXPECT_EQ(automatic->type_name(), "serial_debug");
  EXPECT_EQ(automatic->worker_grant(), 0U);
  EXPECT_EQ(automatic->reservation_slots(), 0U);
  EXPECT_FALSE(automatic->is_builtin_cpu());

  const auto explicit_limit = SchedulerFactory::plan_for_hardware(
      "serial_debug", kSchedulerWorkerRequestMax, 1U);
  ASSERT_TRUE(explicit_limit.has_value());
  EXPECT_EQ(explicit_limit->worker_grant(), 0U);
  EXPECT_EQ(explicit_limit->reservation_slots(), 0U);
  EXPECT_THROW((void)SchedulerFactory::plan_for_hardware(
                   "serial_debug", kSchedulerWorkerRequestMax + 1U, 1U),
               std::invalid_argument);
}

/**
 * @brief Proves CPU plans resolve auto once and charge the exact grant.
 * @throws Nothing when unavailable/large hardware and explicit values resolve.
 */
TEST(SchedulerFactoryPlanning, CpuPlanChargesResolvedGrant) {
  const auto unavailable =
      SchedulerFactory::plan_for_hardware("cpu_work_stealing", 0U, 0U);
  ASSERT_TRUE(unavailable.has_value());
  EXPECT_EQ(unavailable->worker_grant(), 1U);
  EXPECT_EQ(unavailable->reservation_slots(), 1U);
  EXPECT_TRUE(unavailable->is_builtin_cpu());

  const auto clamped = SchedulerFactory::plan_for_hardware(
      "cpu_work_stealing", 0U, kSchedulerWorkerRequestMax + 42U);
  ASSERT_TRUE(clamped.has_value());
  EXPECT_EQ(clamped->worker_grant(), kSchedulerWorkerRequestMax);
  EXPECT_EQ(clamped->reservation_slots(), kSchedulerWorkerRequestMax);
  EXPECT_TRUE(clamped->is_builtin_cpu());

  const auto explicit_workers =
      SchedulerFactory::plan_for_hardware("cpu_work_stealing", 4U, 1U);
  ASSERT_TRUE(explicit_workers.has_value());
  EXPECT_EQ(explicit_workers->worker_grant(), 4U);
  EXPECT_EQ(explicit_workers->reservation_slots(), 4U);
  EXPECT_TRUE(explicit_workers->is_builtin_cpu());
}

/**
 * @brief Proves GPU and heterogeneous plans include one potential GPU worker.
 * @throws Nothing when both names retain CPU grants and conservative charges.
 */
TEST(SchedulerFactoryPlanning, GpuAliasesChargeGrantPlusPotentialDeviceWorker) {
  for (const std::string type : {"gpu_pipeline", "heterogeneous"}) {
    const auto automatic = SchedulerFactory::plan_for_hardware(type, 0U, 0U);
    ASSERT_TRUE(automatic.has_value()) << type;
    EXPECT_EQ(automatic->worker_grant(), 1U) << type;
    EXPECT_EQ(automatic->reservation_slots(), 2U) << type;
    EXPECT_FALSE(automatic->is_builtin_cpu()) << type;

    const auto exact_limit = SchedulerFactory::plan_for_hardware(
        type, kSchedulerWorkerRequestMax, 1U);
    ASSERT_TRUE(exact_limit.has_value()) << type;
    EXPECT_EQ(exact_limit->worker_grant(), kSchedulerWorkerRequestMax) << type;
    EXPECT_EQ(exact_limit->reservation_slots(), kGpuSchedulerWorkerInstanceMax)
        << type;
  }
}

/**
 * @brief Proves a registered non-built-in type is charged its full grant.
 * @throws Nothing when pure planning consults registration without
 * construction.
 */
TEST(SchedulerFactoryPlanning, RegisteredTypeChargesGrantWithoutConstruction) {
  ensure_planning_registered_type();
  auto create_calls = planning_create_calls();
  create_calls->store(0U, std::memory_order_relaxed);

  const auto automatic = SchedulerFactory::plan_for_hardware(
      kPlanningOnlyRegisteredType, 0U, kSchedulerWorkerRequestMax + 42U);
  ASSERT_TRUE(automatic.has_value());
  EXPECT_EQ(automatic->worker_grant(), kSchedulerWorkerRequestMax);
  EXPECT_EQ(automatic->reservation_slots(), kSchedulerWorkerRequestMax);
  EXPECT_FALSE(automatic->is_builtin_cpu());
  const auto explicit_workers =
      SchedulerFactory::plan_for_hardware(kPlanningOnlyRegisteredType, 3U, 1U);
  ASSERT_TRUE(explicit_workers.has_value());
  EXPECT_EQ(explicit_workers->worker_grant(), 3U);
  EXPECT_EQ(explicit_workers->reservation_slots(), 3U);
  EXPECT_EQ(create_calls->load(std::memory_order_relaxed), 0U);
}

/**
 * @brief Proves every known threaded plan rejects rather than clamps nine.
 * @throws Nothing when built-in and registered planning raise invalid_argument.
 */
TEST(SchedulerFactoryPlanning, KnownThreadedTypesRejectLimitPlusOne) {
  ensure_planning_registered_type();
  for (const std::string type :
       {"cpu_work_stealing", "gpu_pipeline", "heterogeneous",
        kPlanningOnlyRegisteredType}) {
    EXPECT_THROW((void)SchedulerFactory::plan_for_hardware(
                     type, kSchedulerWorkerRequestMax + 1U, 1U),
                 std::invalid_argument)
        << type;
  }
}

/**
 * @brief Proves slot planning uses checked addition instead of wrapping.
 * @throws Nothing when the exact maximum succeeds and maximum-plus-one fails.
 */
TEST(SchedulerFactoryPlanning, CheckedSlotAdditionRejectsOverflow) {
  const unsigned int maximum = std::numeric_limits<unsigned int>::max();
  EXPECT_EQ(checked_add_scheduler_worker_slots(maximum - 1U, 1U), maximum);
  EXPECT_THROW((void)checked_add_scheduler_worker_slots(maximum, 1U),
               std::overflow_error);
}

}  // namespace
}  // namespace ps
