#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "photospider/core/graph_error.hpp"
#include "policy/policy_registry.hpp"
#include "support/policy_fixture_controller.hpp"

#ifndef PS_TEST_POLICY_PLUGIN_PATH
#error "PS_TEST_POLICY_PLUGIN_PATH must identify the compatible policy fixture"
#endif

#ifndef PS_TEST_MISSING_API_POLICY_PLUGIN_PATH
#error \
    "PS_TEST_MISSING_API_POLICY_PLUGIN_PATH must identify the missing-API fixture"
#endif

#ifndef PS_TEST_MISMATCHED_ABI_POLICY_PLUGIN_PATH
#error "PS_TEST_MISMATCHED_ABI_POLICY_PLUGIN_PATH must identify the ABI fixture"
#endif

namespace ps::policy {
namespace {

using ps::test::PolicyFixtureController;

/** @brief Maximum time used by fixture-controlled synchronization tests. */
constexpr std::chrono::seconds kFixtureWaitTimeout{5};

/**
 * @brief Captures one expected `GraphError` code from a callable.
 * @param operation Callable expected to throw `GraphError`.
 * @return Captured stable error code.
 * @throws Any non-`GraphError` exception from `operation`.
 */
GraphErrc graph_error_code(const std::function<void()>& operation) {
  try {
    operation();
  } catch (const GraphError& error) {
    return error.code();
  }
  ADD_FAILURE() << "Expected GraphError";
  return GraphErrc::Unknown;
}

/**
 * @brief Synchronizes one fixture callback with its controlling test thread.
 * @note Every field except `registry` is protected by `mutex`.
 */
struct HookState final {
  /** @brief Registry used for read-only and rejected mutation reentry. */
  PolicyRegistry* registry = nullptr;

  /** @brief Serializes callback/test synchronization fields. */
  std::mutex mutex;

  /** @brief Publishes callback entry and release. */
  std::condition_variable condition;

  /** @brief Whether the controlled callback entered its hook. */
  bool entered = false;

  /** @brief Whether the controlling test released the hook. */
  bool released = false;

  /** @brief Whether read-only registry reentry completed successfully. */
  bool read_succeeded = false;

  /** @brief Whether same-thread mutation was rejected with the exact code. */
  bool mutation_rejected = false;
};

/**
 * @brief Exercises permitted read-only and forbidden mutation reentry.
 * @param context Nonnull `HookState` owned by the calling test.
 * @param event Callback event; only selection is expected.
 * @return Zero, ignored by the fixture.
 * @throws Nothing.
 */
std::uint32_t PS_POLICY_CALL
reentrant_hook(void* context, ps_policy_fixture_hook_event event) noexcept {
  auto* state = static_cast<HookState*>(context);
  if (state == nullptr || event != PS_POLICY_FIXTURE_HOOK_SELECT ||
      state->registry == nullptr) {
    return 0U;
  }
  try {
    const std::vector<std::string> types = state->registry->available_types();
    state->read_succeeded = !types.empty();
  } catch (...) {
    state->read_succeeded = false;
  }
  try {
    (void)state->registry->unload_all_plugins();
  } catch (const GraphError& error) {
    state->mutation_rejected = error.code() == GraphErrc::InvalidParameter;
  } catch (...) {
    state->mutation_rejected = false;
  }
  return 0U;
}

/**
 * @brief Blocks one selection callback until its test thread releases it.
 * @param context Nonnull `HookState` owned through callback completion.
 * @param event Callback event; only selection is controlled.
 * @return Zero, ignored by the fixture.
 * @throws Nothing.
 */
std::uint32_t PS_POLICY_CALL
blocking_hook(void* context, ps_policy_fixture_hook_event event) noexcept {
  auto* state = static_cast<HookState*>(context);
  if (state == nullptr || event != PS_POLICY_FIXTURE_HOOK_SELECT) {
    return 0U;
  }
  std::unique_lock<std::mutex> lock(state->mutex);
  state->entered = true;
  state->condition.notify_all();
  state->condition.wait(lock, [state] { return state->released; });
  return 0U;
}

/**
 * @brief Throws across the nonconforming fixture callback boundary.
 * @return Never returns.
 * @throws std::runtime_error deliberately for Host-fence verification.
 */
std::uint32_t PS_POLICY_CALL throwing_hook(void*,
                                           ps_policy_fixture_hook_event) {
  throw std::runtime_error("fixture callback exception");
}

/**
 * @brief Verifies the exact natural-layout C++17 ABI profile at compile time.
 * @throws Nothing.
 */
TEST(PolicyPluginAbi, MatchesFrozenNaturalLayoutAndCallbackProfile) {
  static_assert(sizeof(void*) == 8U);
  static_assert(alignof(void*) == 8U);
  static_assert(sizeof(ps_policy_string_view_v1) == 16U);
  static_assert(sizeof(ps_policy_type_metadata_v1) == 80U);
  static_assert(sizeof(ps_policy_create_args_v1) == 40U);
  static_assert(sizeof(ps_policy_candidate_v1) == 120U);
  static_assert(sizeof(ps_policy_selection_snapshot_v1) == 64U);
  static_assert(sizeof(ps_policy_decision_v1) == 48U);
  static_assert(sizeof(ps_policy_plugin_api_v1) == 80U);
  static_assert(offsetof(ps_policy_candidate_v1, candidate_id) == 8U);
  static_assert(offsetof(ps_policy_candidate_v1, reserved) == 104U);
  static_assert(offsetof(ps_policy_plugin_api_v1, reserved) == 48U);
  static_assert(
      std::is_nothrow_invocable_r_v<
          ps_policy_status_v1, ps_policy_select_fn_v1, void*,
          const ps_policy_selection_snapshot_v1*, ps_policy_decision_v1*>);
  SUCCEED();
}

/**
 * @brief Builds one complete authority-free candidate for binding tests.
 * @param candidate_id Unique opaque ready-entry identity.
 * @param enqueue_sequence Stable nonzero publication sequence.
 * @return Exact ABI-v1 candidate with conservative valid scalar fields.
 * @throws Nothing.
 * @note The fixture policy selects the last element and does not interpret
 * Host-private scoring fields.
 */
ps_policy_candidate_v1 make_candidate(std::uint64_t candidate_id,
                                      std::uint64_t enqueue_sequence) noexcept {
  ps_policy_candidate_v1 candidate{};
  candidate.struct_size = sizeof(candidate);
  candidate.struct_kind = PS_POLICY_STRUCT_CANDIDATE;
  candidate.candidate_id = candidate_id;
  candidate.graph_id = 1U;
  candidate.run_id = 1U;
  candidate.deadline_ns = PS_POLICY_NO_DEADLINE_NS;
  candidate.weight = 1U;
  candidate.work_units = 1U;
  candidate.ready_bytes = 1U;
  candidate.enqueue_sequence = enqueue_sequence;
  return candidate;
}

/**
 * @brief Verifies immutable built-in registration and class support.
 * @throws Standard allocation or synchronization exceptions from the isolated
 * registry under test.
 */
TEST(PolicyRegistry, StartsWithExactBuiltinsAndClassSpecificBindings) {
  PolicyRegistry registry;

  EXPECT_EQ(registry.available_types(),
            (std::vector<std::string>{"interactive", "throughput"}));
  EXPECT_EQ(registry.description("interactive"),
            "Built-in interactive policy.");
  EXPECT_EQ(registry.description("throughput"), "Built-in throughput policy.");
  EXPECT_TRUE(registry.loaded_plugins().empty());

  const std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("interactive", PolicyClass::Interactive, 17U);
  ASSERT_NE(binding, nullptr);
  EXPECT_TRUE(binding->is_builtin());
  EXPECT_EQ(binding->type_name(), "interactive");
  EXPECT_EQ(binding->policy_class(), PolicyClass::Interactive);
  EXPECT_EQ(binding->generation(), 17U);
  EXPECT_THROW(
      registry.create_binding("interactive", PolicyClass::Throughput, 18U),
      GraphError);
  EXPECT_THROW(
      registry.create_binding("throughput", PolicyClass::Throughput, 0U),
      GraphError);
}

/**
 * @brief Verifies pre-publication loader failures leave no visible residue.
 * @throws Standard allocation, filesystem, loader, or synchronization
 * exceptions outside the expected rejected-fixture boundaries.
 */
TEST(PolicyRegistry, RejectsMissingApiAndMismatchedAbiTransactionally) {
  PolicyRegistry registry;
  const std::vector<std::string> initial_types = registry.available_types();

  const std::vector<std::pair<std::string, std::string>> rejected_fixtures = {
      {PS_TEST_MISSING_API_POLICY_PLUGIN_PATH, "ps_policy_plugin_get_api_v1"},
      {PS_TEST_MISMATCHED_ABI_POLICY_PLUGIN_PATH,
       "ABI version is incompatible"},
  };
  for (const auto& fixture : rejected_fixtures) {
    try {
      registry.load(fixture.first);
      FAIL() << "Expected policy fixture rejection: " << fixture.first;
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
      EXPECT_NE(std::string(error.what()).find(fixture.second),
                std::string::npos);
    }
    EXPECT_EQ(registry.available_types(), initial_types);
    EXPECT_TRUE(registry.loaded_plugins().empty());
  }
}

/**
 * @brief Verifies compatible publication, selection, and binding-held DSO
 * lifetime after registry visibility is removed.
 * @throws Standard allocation, filesystem, loader, callback, or
 * synchronization exceptions from the exercised production path.
 */
TEST(PolicyRegistry, LoadsFixtureAndKeepsActiveBindingValidAfterUnload) {
  PolicyRegistry registry;
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);

  EXPECT_EQ(registry.available_types(),
            (std::vector<std::string>{"fixture_policy", "interactive",
                                      "throughput"}));
  EXPECT_EQ(registry.description("fixture_policy"),
            "Deterministic policy test fixture.");
  ASSERT_EQ(registry.loaded_plugins().size(), 1U);
  EXPECT_EQ(registry.loaded_plugins().front(),
            std::filesystem::absolute(PS_TEST_POLICY_PLUGIN_PATH)
                .lexically_normal()
                .string());

  const std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("fixture_policy", PolicyClass::Throughput, 23U);
  ASSERT_NE(binding, nullptr);
  EXPECT_FALSE(binding->is_builtin());
  EXPECT_EQ(binding->type_name(), "fixture_policy");
  EXPECT_EQ(binding->policy_class(), PolicyClass::Throughput);
  EXPECT_EQ(binding->generation(), 23U);

  const std::vector<ps_policy_candidate_v1> candidates = {
      make_candidate(101U, 1U), make_candidate(202U, 2U)};
  PolicyInvocationResult result = binding->select(candidates, 31U, 41U);
  EXPECT_EQ(result.kind, PolicyInvocationResult::Kind::Selected);
  EXPECT_EQ(result.candidate_id, 202U);
  EXPECT_FALSE(result.fault.has_value());

  EXPECT_EQ(registry.unload_all_plugins(), 1U);
  EXPECT_EQ(registry.available_types(),
            (std::vector<std::string>{"interactive", "throughput"}));
  EXPECT_TRUE(registry.loaded_plugins().empty());
  EXPECT_THROW(
      registry.create_binding("fixture_policy", PolicyClass::Interactive, 24U),
      GraphError);

  result = binding->select(candidates, 32U, 42U);
  EXPECT_EQ(result.kind, PolicyInvocationResult::Kind::Selected);
  EXPECT_EQ(result.candidate_id, 202U);
}

/**
 * @brief Verifies first-fault publication is sticky for one exact generation.
 * @throws Standard allocation or synchronization exceptions from binding
 * construction and copied fault ownership.
 */
TEST(PolicyRegistry, PreservesFirstBindingFault) {
  PolicyRegistry registry;
  const std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("throughput", PolicyClass::Throughput, 51U);

  EXPECT_TRUE(binding->publish_first_fault(
      PolicyFaultSnapshot{PolicyFaultReason::CallbackStatus,
                          std::optional<std::uint32_t>{4U}, "first fault"}));
  EXPECT_FALSE(binding->publish_first_fault(PolicyFaultSnapshot{
      PolicyFaultReason::Abstained, std::nullopt, "later fault"}));

  const std::optional<PolicyFaultSnapshot> fault = binding->fault();
  ASSERT_TRUE(fault.has_value());
  EXPECT_EQ(fault->reason, PolicyFaultReason::CallbackStatus);
  EXPECT_EQ(fault->callback_status, std::optional<std::uint32_t>{4U});
  EXPECT_EQ(fault->message, "first fault");
}

/**
 * @brief Verifies API-table statuses and malformed bytes reject atomically.
 * @throws Standard fixture-loader and Host allocation exceptions outside the
 * expected rejection boundaries.
 */
TEST(PolicyRegistry, RejectsApiStatusAndStructureMatrixTransactionally) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  struct GraphCase final {
    ps_policy_fixture_api_mode mode;
    GraphErrc expected;
  };
  const std::vector<GraphCase> status_cases = {
      {PS_POLICY_FIXTURE_API_STATUS_INVALID, GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_API_STATUS_UNSUPPORTED, GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_API_STATUS_INTERNAL, GraphErrc::ComputeError},
      {PS_POLICY_FIXTURE_API_STATUS_UNKNOWN, GraphErrc::ComputeError},
  };
  for (const GraphCase& test_case : status_cases) {
    SCOPED_TRACE(test_case.mode);
    control.reset();
    control.set_api_mode(test_case.mode);
    PolicyRegistry registry;
    EXPECT_EQ(graph_error_code(
                  [&registry] { registry.load(PS_TEST_POLICY_PLUGIN_PATH); }),
              test_case.expected);
    EXPECT_EQ(registry.available_types(),
              (std::vector<std::string>{"interactive", "throughput"}));
    EXPECT_TRUE(registry.loaded_plugins().empty());
  }

  control.reset();
  control.set_api_mode(PS_POLICY_FIXTURE_API_STATUS_OUT_OF_MEMORY);
  PolicyRegistry oom_registry;
  EXPECT_THROW(oom_registry.load(PS_TEST_POLICY_PLUGIN_PATH), std::bad_alloc);
  EXPECT_TRUE(oom_registry.loaded_plugins().empty());

  const std::vector<ps_policy_fixture_api_mode> malformed_modes = {
      PS_POLICY_FIXTURE_API_SIZE_UNDER,
      PS_POLICY_FIXTURE_API_SIZE_OVER,
      PS_POLICY_FIXTURE_API_KIND_INVALID,
      PS_POLICY_FIXTURE_API_ABI_INVALID,
      PS_POLICY_FIXTURE_API_TYPE_COUNT_ZERO,
      PS_POLICY_FIXTURE_API_TYPE_COUNT_OVER,
      PS_POLICY_FIXTURE_API_CALLBACK_NULL,
      PS_POLICY_FIXTURE_API_RESERVED_NONZERO,
  };
  for (ps_policy_fixture_api_mode mode : malformed_modes) {
    SCOPED_TRACE(mode);
    control.reset();
    control.set_api_mode(mode);
    PolicyRegistry registry;
    EXPECT_EQ(graph_error_code(
                  [&registry] { registry.load(PS_TEST_POLICY_PLUGIN_PATH); }),
              GraphErrc::InvalidParameter);
    EXPECT_EQ(registry.available_types(),
              (std::vector<std::string>{"interactive", "throughput"}));
    EXPECT_TRUE(registry.loaded_plugins().empty());
  }
}

/**
 * @brief Verifies metadata statuses, bounds, UTF-8, names, and reserved bytes.
 * @throws Standard fixture-loader and Host allocation exceptions outside the
 * expected rejection boundaries.
 */
TEST(PolicyRegistry, RejectsMetadataStatusAndStructureMatrixTransactionally) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  struct GraphCase final {
    ps_policy_fixture_metadata_mode mode;
    GraphErrc expected;
  };
  const std::vector<GraphCase> status_cases = {
      {PS_POLICY_FIXTURE_METADATA_STATUS_INVALID, GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_METADATA_STATUS_UNSUPPORTED,
       GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_METADATA_STATUS_INTERNAL, GraphErrc::ComputeError},
      {PS_POLICY_FIXTURE_METADATA_STATUS_UNKNOWN, GraphErrc::ComputeError},
  };
  for (const GraphCase& test_case : status_cases) {
    SCOPED_TRACE(test_case.mode);
    control.reset();
    control.set_metadata_mode(test_case.mode);
    PolicyRegistry registry;
    EXPECT_EQ(graph_error_code(
                  [&registry] { registry.load(PS_TEST_POLICY_PLUGIN_PATH); }),
              test_case.expected);
    EXPECT_TRUE(registry.loaded_plugins().empty());
  }

  control.reset();
  control.set_metadata_mode(PS_POLICY_FIXTURE_METADATA_STATUS_OUT_OF_MEMORY);
  PolicyRegistry oom_registry;
  EXPECT_THROW(oom_registry.load(PS_TEST_POLICY_PLUGIN_PATH), std::bad_alloc);
  EXPECT_TRUE(oom_registry.loaded_plugins().empty());

  const std::vector<ps_policy_fixture_metadata_mode> malformed_modes = {
      PS_POLICY_FIXTURE_METADATA_SIZE_UNDER,
      PS_POLICY_FIXTURE_METADATA_SIZE_OVER,
      PS_POLICY_FIXTURE_METADATA_KIND_INVALID,
      PS_POLICY_FIXTURE_METADATA_RESERVED0_NONZERO,
      PS_POLICY_FIXTURE_METADATA_RESERVED_NONZERO,
      PS_POLICY_FIXTURE_METADATA_MASK_ZERO,
      PS_POLICY_FIXTURE_METADATA_MASK_UNKNOWN,
      PS_POLICY_FIXTURE_METADATA_NAME_NULL,
      PS_POLICY_FIXTURE_METADATA_NAME_NONCANONICAL,
      PS_POLICY_FIXTURE_METADATA_NAME_RESERVED,
      PS_POLICY_FIXTURE_METADATA_DESCRIPTION_INVALID_UTF8,
      PS_POLICY_FIXTURE_METADATA_DESCRIPTION_TOO_LONG,
  };
  for (ps_policy_fixture_metadata_mode mode : malformed_modes) {
    SCOPED_TRACE(mode);
    control.reset();
    control.set_metadata_mode(mode);
    PolicyRegistry registry;
    EXPECT_EQ(graph_error_code(
                  [&registry] { registry.load(PS_TEST_POLICY_PLUGIN_PATH); }),
              GraphErrc::InvalidParameter);
    EXPECT_TRUE(registry.loaded_plugins().empty());
  }

  control.reset();
  control.set_api_mode(PS_POLICY_FIXTURE_API_TWO_TYPES);
  control.set_metadata_mode(PS_POLICY_FIXTURE_METADATA_DUPLICATE_NAMES);
  PolicyRegistry duplicate_registry;
  EXPECT_EQ(graph_error_code([&duplicate_registry] {
              duplicate_registry.load(PS_TEST_POLICY_PLUGIN_PATH);
            }),
            GraphErrc::InvalidParameter);
  EXPECT_TRUE(duplicate_registry.loaded_plugins().empty());
}

/**
 * @brief Verifies successful null/nonnull instances each destroy exactly once.
 * @throws Standard fixture-loader, callback, and Host allocation exceptions.
 */
TEST(PolicyRegistry, DestroysEverySuccessfulLogicalCreateExactlyOnce) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  PolicyRegistry registry;
  control.reset();
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);

  const std::vector<ps_policy_fixture_create_mode> success_modes = {
      PS_POLICY_FIXTURE_CREATE_SUCCESS_NULL,
      PS_POLICY_FIXTURE_CREATE_SUCCESS_NONNULL,
  };
  std::uint64_t generation = 100U;
  for (ps_policy_fixture_create_mode mode : success_modes) {
    SCOPED_TRACE(mode);
    control.reset();
    control.set_create_mode(mode);
    {
      const std::shared_ptr<PolicyBinding> binding = registry.create_binding(
          "fixture_policy", PolicyClass::Interactive, generation++);
      ASSERT_NE(binding, nullptr);
      EXPECT_EQ(control.create_count(), 1U);
      EXPECT_EQ(control.destroy_count(), 0U);
    }
    EXPECT_EQ(control.destroy_count(), 1U);
  }

  control.reset();
  control.set_create_mode(PS_POLICY_FIXTURE_CREATE_SUCCESS_NONNULL);
  control.set_destroy_mode(PS_POLICY_FIXTURE_DESTROY_STATUS_INTERNAL);
  {
    const std::shared_ptr<PolicyBinding> binding = registry.create_binding(
        "fixture_policy", PolicyClass::Throughput, generation++);
    ASSERT_NE(binding, nullptr);
  }
  EXPECT_EQ(control.destroy_count(), 1U);

  control.reset();
  control.set_create_mode(PS_POLICY_FIXTURE_CREATE_SUCCESS_NONNULL);
  std::shared_ptr<PolicyBinding> binding = registry.create_binding(
      "fixture_policy", PolicyClass::Interactive, generation++);
  control.set_destroy_mode(PS_POLICY_FIXTURE_DESTROY_HOOK);
  control.set_hook(&throwing_hook, nullptr);
  binding.reset();
  EXPECT_EQ(control.destroy_count(), 1U);
  control.set_hook(nullptr, nullptr);
}

/**
 * @brief Verifies failed create never destroys and maps every status exactly.
 * @throws Standard fixture-loader and Host allocation exceptions outside the
 * expected rejection boundaries.
 */
TEST(PolicyRegistry, RejectsFailedCreateWithoutDestroyingReturnedContext) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  PolicyRegistry registry;
  control.reset();
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);

  struct GraphCase final {
    ps_policy_fixture_create_mode mode;
    GraphErrc expected;
  };
  const std::vector<GraphCase> graph_cases = {
      {PS_POLICY_FIXTURE_CREATE_STATUS_INVALID_NULL,
       GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_CREATE_STATUS_INVALID_NONNULL,
       GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_CREATE_STATUS_UNSUPPORTED,
       GraphErrc::InvalidParameter},
      {PS_POLICY_FIXTURE_CREATE_STATUS_INTERNAL, GraphErrc::ComputeError},
      {PS_POLICY_FIXTURE_CREATE_STATUS_UNKNOWN, GraphErrc::ComputeError},
  };
  std::uint64_t generation = 200U;
  for (const GraphCase& test_case : graph_cases) {
    SCOPED_TRACE(test_case.mode);
    control.reset();
    control.set_create_mode(test_case.mode);
    EXPECT_EQ(graph_error_code([&registry, &generation] {
                (void)registry.create_binding(
                    "fixture_policy", PolicyClass::Interactive, generation++);
              }),
              test_case.expected);
    EXPECT_EQ(control.create_count(), 1U);
    EXPECT_EQ(control.destroy_count(), 0U);
  }

  control.reset();
  control.set_create_mode(PS_POLICY_FIXTURE_CREATE_STATUS_OUT_OF_MEMORY);
  EXPECT_THROW(registry.create_binding("fixture_policy",
                                       PolicyClass::Interactive, generation++),
               std::bad_alloc);
  EXPECT_EQ(control.create_count(), 1U);
  EXPECT_EQ(control.destroy_count(), 0U);

  control.reset();
  control.set_create_mode(PS_POLICY_FIXTURE_CREATE_HOOK_SUCCESS);
  control.set_hook(&throwing_hook, nullptr);
  EXPECT_EQ(graph_error_code([&registry, &generation] {
              (void)registry.create_binding(
                  "fixture_policy", PolicyClass::Interactive, generation++);
            }),
            GraphErrc::ComputeError);
  EXPECT_EQ(control.create_count(), 1U);
  EXPECT_EQ(control.destroy_count(), 0U);
  control.set_hook(nullptr, nullptr);
}

/**
 * @brief Verifies every plugin decision outcome has one exact classification.
 * @throws Standard fixture-loader, callback, and Host allocation exceptions.
 */
TEST(PolicyRegistry, ClassifiesCompleteDecisionMatrix) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  PolicyRegistry registry;
  control.reset();
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);
  const std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("fixture_policy", PolicyClass::Interactive, 301U);
  const std::vector<ps_policy_candidate_v1> candidates = {
      make_candidate(101U, 1U), make_candidate(202U, 2U)};

  struct SelectedCase final {
    ps_policy_fixture_select_mode mode;
    std::uint64_t expected_candidate;
  };
  for (const SelectedCase& test_case :
       std::vector<SelectedCase>{{PS_POLICY_FIXTURE_SELECT_FIRST, 101U},
                                 {PS_POLICY_FIXTURE_SELECT_LAST, 202U}}) {
    SCOPED_TRACE(test_case.mode);
    control.set_select_mode(test_case.mode);
    const PolicyInvocationResult result = binding->select(candidates, 1U, 1U);
    EXPECT_EQ(result.kind, PolicyInvocationResult::Kind::Selected);
    EXPECT_EQ(result.candidate_id, test_case.expected_candidate);
    EXPECT_FALSE(result.fault.has_value());
  }

  struct FaultCase final {
    ps_policy_fixture_select_mode mode;
    PolicyFaultReason reason;
    std::optional<std::uint32_t> callback_status;
  };
  const std::vector<FaultCase> fault_cases = {
      {PS_POLICY_FIXTURE_SELECT_ABSTAIN, PolicyFaultReason::Abstained,
       std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_STATUS_INVALID,
       PolicyFaultReason::CallbackStatus,
       std::optional<std::uint32_t>{PS_POLICY_STATUS_INVALID_ARGUMENT}},
      {PS_POLICY_FIXTURE_SELECT_STATUS_OUT_OF_MEMORY,
       PolicyFaultReason::CallbackStatus,
       std::optional<std::uint32_t>{PS_POLICY_STATUS_OUT_OF_MEMORY}},
      {PS_POLICY_FIXTURE_SELECT_STATUS_UNKNOWN,
       PolicyFaultReason::CallbackStatus,
       std::optional<std::uint32_t>{UINT32_MAX}},
      {PS_POLICY_FIXTURE_SELECT_SIZE_UNDER,
       PolicyFaultReason::MalformedDecision, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_SIZE_OVER, PolicyFaultReason::MalformedDecision,
       std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_KIND_INVALID,
       PolicyFaultReason::MalformedDecision, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_RESERVED0_NONZERO,
       PolicyFaultReason::MalformedDecision, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_RESERVED_NONZERO,
       PolicyFaultReason::MalformedDecision, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_DECISION_UNKNOWN,
       PolicyFaultReason::MalformedDecision, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_ABSTAIN_NONNULL,
       PolicyFaultReason::MalformedDecision, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_BINDING_GENERATION_MISMATCH,
       PolicyFaultReason::GenerationMismatch, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_SNAPSHOT_GENERATION_MISMATCH,
       PolicyFaultReason::GenerationMismatch, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_CANDIDATE_ZERO,
       PolicyFaultReason::CandidateOutsideSnapshot, std::nullopt},
      {PS_POLICY_FIXTURE_SELECT_CANDIDATE_OUTSIDE,
       PolicyFaultReason::CandidateOutsideSnapshot, std::nullopt},
  };
  std::uint64_t sequence = 2U;
  for (const FaultCase& test_case : fault_cases) {
    SCOPED_TRACE(test_case.mode);
    control.set_select_mode(test_case.mode);
    const PolicyInvocationResult result =
        binding->select(candidates, sequence, sequence);
    ++sequence;
    EXPECT_EQ(result.kind, PolicyInvocationResult::Kind::InvalidPluginDecision);
    EXPECT_EQ(result.candidate_id, 0U);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->reason, test_case.reason);
    EXPECT_EQ(result.fault->callback_status, test_case.callback_status);
  }

  control.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);
  control.set_hook(&throwing_hook, nullptr);
  const PolicyInvocationResult exception_result =
      binding->select(candidates, sequence, sequence);
  EXPECT_EQ(exception_result.kind,
            PolicyInvocationResult::Kind::InvalidPluginDecision);
  ASSERT_TRUE(exception_result.fault.has_value());
  EXPECT_EQ(exception_result.fault->reason,
            PolicyFaultReason::CallbackException);
  EXPECT_FALSE(exception_result.fault->callback_status.has_value());
  control.set_hook(nullptr, nullptr);
}

/**
 * @brief Verifies invalid Host snapshots fail before entering policy code.
 * @throws Standard fixture-loader and Host allocation exceptions.
 */
TEST(PolicyRegistry, RejectsInvalidHostCandidatesBeforeCallbackEntry) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  PolicyRegistry registry;
  control.reset();
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);
  const std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("fixture_policy", PolicyClass::Throughput, 401U);

  std::vector<ps_policy_candidate_v1> candidates = {make_candidate(1U, 1U),
                                                    make_candidate(2U, 2U)};
  candidates[0].reserved[0] = 1U;
  EXPECT_EQ(binding->select(candidates, 1U, 1U).kind,
            PolicyInvocationResult::Kind::BuiltinViolation);
  candidates[0] = make_candidate(1U, 1U);
  candidates[1].candidate_id = candidates[0].candidate_id;
  EXPECT_EQ(binding->select(candidates, 2U, 2U).kind,
            PolicyInvocationResult::Kind::BuiltinViolation);
  EXPECT_EQ(binding->select({}, 3U, 3U).kind,
            PolicyInvocationResult::Kind::BuiltinViolation);
  EXPECT_EQ(control.select_count(), 0U);
}

/**
 * @brief Verifies callback read reentry succeeds and mutation reentry rejects.
 * @throws Standard fixture-loader, callback, and Host allocation exceptions.
 */
TEST(PolicyRegistry, AllowsReadOnlyReentryAndRejectsMutationBeforeLocking) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  PolicyRegistry registry;
  control.reset();
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);
  const std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("fixture_policy", PolicyClass::Interactive, 501U);
  HookState state;
  state.registry = &registry;
  control.set_hook(&reentrant_hook, &state);
  control.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);

  const PolicyInvocationResult result =
      binding->select({make_candidate(1U, 1U)}, 1U, 1U);
  EXPECT_EQ(result.kind, PolicyInvocationResult::Kind::Selected);
  EXPECT_TRUE(state.read_succeeded);
  EXPECT_TRUE(state.mutation_rejected);
  EXPECT_EQ(registry.available_types(),
            (std::vector<std::string>{"fixture_policy", "interactive",
                                      "throughput"}));
  control.set_hook(nullptr, nullptr);
}

/**
 * @brief Verifies unload cannot retire a binding during a blocked callback.
 * @throws Standard fixture-loader, callback, threading, and Host allocation
 * exceptions.
 */
TEST(PolicyRegistry, RetainsBindingAndDestroysAfterBlockedSelectionReturns) {
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  PolicyRegistry registry;
  control.reset();
  registry.load(PS_TEST_POLICY_PLUGIN_PATH);
  control.set_create_mode(PS_POLICY_FIXTURE_CREATE_SUCCESS_NONNULL);
  std::shared_ptr<PolicyBinding> binding =
      registry.create_binding("fixture_policy", PolicyClass::Interactive, 601U);
  HookState state;
  control.set_hook(&blocking_hook, &state);
  control.set_select_mode(PS_POLICY_FIXTURE_SELECT_HOOK_LAST);
  std::optional<PolicyInvocationResult> result;

  std::thread selector([captured = binding, &result]() mutable {
    result = captured->select({make_candidate(7U, 1U)}, 1U, 1U);
    captured.reset();
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    entered = state.condition.wait_for(lock, kFixtureWaitTimeout,
                                       [&state] { return state.entered; });
  }
  if (!entered) {
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.released = true;
    }
    state.condition.notify_all();
    selector.join();
    FAIL() << "Timed out waiting for controlled policy callback entry.";
    return;
  }
  EXPECT_EQ(registry.unload_all_plugins(), 1U);
  EXPECT_EQ(registry.available_types(),
            (std::vector<std::string>{"interactive", "throughput"}));
  binding.reset();
  EXPECT_EQ(control.destroy_count(), 0U);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.released = true;
  }
  state.condition.notify_all();
  selector.join();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind, PolicyInvocationResult::Kind::Selected);
  EXPECT_EQ(result->candidate_id, 7U);
  EXPECT_EQ(control.destroy_count(), 1U);
  control.set_hook(nullptr, nullptr);
}

/**
 * @brief Proves a truly nonreturning callback is honestly blocked and bounded
 * only by an external parent watchdog.
 * @throws Standard fixture-loader errors before process creation.
 */
TEST(PolicyRegistry, ParentWatchdogTerminatesNonreturningCallbackProcess) {
#if defined(_WIN32)
  GTEST_SKIP() << "The repository watchdog fixture currently uses POSIX fork.";
#else
  PolicyFixtureController control(PS_TEST_POLICY_PLUGIN_PATH);
  control.reset();
  control.set_select_mode(PS_POLICY_FIXTURE_SELECT_NONRETURNING);
  const pid_t child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    try {
      PolicyRegistry registry;
      registry.load(PS_TEST_POLICY_PLUGIN_PATH);
      const std::shared_ptr<PolicyBinding> binding = registry.create_binding(
          "fixture_policy", PolicyClass::Interactive, 701U);
      (void)binding->select({make_candidate(1U, 1U)}, 1U, 1U);
      _exit(2);
    } catch (...) {
      _exit(3);
    }
  }

  int status = 0;
  bool exited_early = false;
  for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
    const pid_t wait_result = waitpid(child, &status, WNOHANG);
    ASSERT_NE(wait_result, -1);
    if (wait_result == child) {
      exited_early = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!exited_early) {
    ASSERT_EQ(kill(child, SIGKILL), 0);
    ASSERT_EQ(waitpid(child, &status, 0), child);
  }
  EXPECT_FALSE(exited_early);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
  control.reset();
#endif
}

}  // namespace
}  // namespace ps::policy
