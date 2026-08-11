/**
 * @file test_isolated_cpu_invocation.cpp
 * @brief Verifies real fresh-process shared-memory CPU invocation behavior.
 */
#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "execution/isolated_cpu_invocation.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_ISOLATED_CPU_FIXTURE_PATH
#error "PS_TEST_ISOLATED_CPU_FIXTURE_PATH must name the process fixture"
#endif

namespace ps::execution {
namespace {

/**
 * @brief Unique owner for one test-only POSIX descriptor.
 * @throws Nothing for moves and destruction.
 */
class ScopedTestFd final {
 public:
  /**
   * @brief Takes ownership of one descriptor or invalid sentinel.
   * @param descriptor Descriptor to close once.
   * @throws Nothing.
   */
  explicit ScopedTestFd(int descriptor) noexcept : descriptor_(descriptor) {}

  /** @brief Closes the retained descriptor once. */
  ~ScopedTestFd() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  /** @brief Prevents duplicate ownership. */
  ScopedTestFd(const ScopedTestFd&) = delete;
  /** @brief Prevents duplicate assignment. */
  ScopedTestFd& operator=(const ScopedTestFd&) = delete;

  /**
   * @brief Returns the retained descriptor.
   * @return Descriptor or invalid sentinel.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

 private:
  /** @brief Sole owned descriptor. */
  int descriptor_ = -1;
};

/**
 * @brief Creates one deterministic nonzero invocation identity component.
 * @param seed First sequence byte.
 * @return Complete comparison-only opaque value.
 * @throws Nothing.
 */
IsolatedCpuOpaqueId integration_id(std::uint8_t seed) noexcept {
  IsolatedCpuOpaqueId id;
  for (std::size_t index = 0U; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::byte>(seed + index);
  }
  return id;
}

/**
 * @brief Creates one complete deterministic invocation identity.
 * @param domain Per-call identity domain below wraparound.
 * @return Valid tuple with current worker/plugin generations.
 * @throws Nothing.
 */
IsolatedCpuInvocationIdentity integration_identity(
    std::uint8_t domain) noexcept {
  IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = integration_id(1U);
  identity.job_id = integration_id(17U);
  identity.attempt_id = integration_id(33U);
  identity.worker_id = integration_id(49U);
  identity.worker_lease_generation = 3U;
  identity.plugin_package_id = integration_id(65U);
  identity.plugin_generation = 5U;
  identity.invocation_id = integration_id(domain);
  return identity;
}

/**
 * @brief Creates the shared two-by-three u8 descriptor used by integration
 * tests.
 * @return Unquantized NativeScalar descriptor.
 * @throws std::bad_alloc when shape storage cannot allocate.
 */
DenseTensorDescriptor integration_descriptor() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {2U, 3U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return descriptor;
}

/**
 * @brief Creates the exact contiguous layout for the integration descriptor.
 * @return Positive non-overlapping six-byte layout.
 * @throws std::bad_alloc when stride storage cannot allocate.
 */
StridedLayout integration_layout() {
  return StridedLayout{{3, 1}, 0U};
}

/**
 * @brief Publishes one six-byte immutable input Value.
 * @param bytes Exact payload bytes.
 * @return Fresh Ready CPU DenseTensor Value.
 * @throws ValueBuilder validation/allocation/publication errors unchanged.
 */
Value integration_input(const std::array<std::byte, 6U>& bytes) {
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      integration_descriptor(), std::nullopt, integration_layout(),
      bytes.size());
  {
    WriteLease lease = builder.acquire_write();
    std::memcpy(lease.data(), bytes.data(), bytes.size());
  }
  return builder.seal();
}

/**
 * @brief Creates one exact six-byte Host output plan.
 * @return Positive-stride unquantized CPU plan.
 * @throws std::bad_alloc when descriptor/layout vectors allocate.
 */
IsolatedCpuDenseTensorOutputPlan integration_output_plan() {
  return IsolatedCpuDenseTensorOutputPlan{
      integration_descriptor(), std::nullopt, integration_layout(), 6U};
}

/**
 * @brief Creates one exact one-byte Host output plan.
 * @return Rank-one u8 output plan.
 * @throws std::bad_alloc when descriptor/layout vectors allocate.
 */
IsolatedCpuDenseTensorOutputPlan one_byte_output_plan() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return IsolatedCpuDenseTensorOutputPlan{std::move(descriptor), std::nullopt,
                                          StridedLayout{{1}, 0U}, 1U};
}

/**
 * @brief Creates a Host invocation with one standard output plan.
 * @param operation Child fixture operation key.
 * @param domain Unique deterministic invocation-id domain.
 * @param inputs Ready input Values.
 * @return Complete high-level request.
 * @throws std::bad_alloc when copied state cannot allocate.
 */
IsolatedCpuHostInvocation integration_invocation(
    std::string operation, std::uint8_t domain,
    std::vector<Value> inputs = {}) {
  IsolatedCpuHostInvocation invocation;
  invocation.identity = integration_identity(domain);
  invocation.operation = std::move(operation);
  invocation.inputs = std::move(inputs);
  invocation.outputs.push_back(integration_output_plan());
  return invocation;
}

/**
 * @brief Copies one Ready contiguous Value into test-owned bytes.
 * @param value Valid six-byte output.
 * @return Exact physical storage bytes.
 * @throws DenseTensorView validation/readiness/access errors unchanged.
 */
std::array<std::byte, 6U> integration_bytes(const Value& value) {
  DenseTensorView view(value);
  if (view.storage_size() != 6U) {
    throw std::runtime_error("isolated CPU integration output size changed");
  }
  std::array<std::byte, 6U> bytes{};
  std::memcpy(bytes.data(), view.data(), bytes.size());
  return bytes;
}

/**
 * @brief Counts this process's currently open descriptors without retaining
 * one.
 * @return Open descriptor count excluding the transient directory descriptor.
 * @throws std::system_error when `/dev/fd` cannot be enumerated exactly.
 * @note The helper observes leak behavior only; descriptor identities remain
 * process-local and never enter the invocation protocol.
 */
std::size_t count_open_descriptors() {
  DIR* directory = ::opendir("/dev/fd");
  if (directory == nullptr) {
    throw std::system_error(errno, std::generic_category(),
                            "open /dev/fd for invocation leak test");
  }
  const int enumeration_fd = ::dirfd(directory);
  std::size_t count = 0U;
  errno = 0;
  while (const struct dirent* entry = ::readdir(directory)) {
    const char* const begin = entry->d_name;
    const char* const end = begin + std::strlen(begin);
    std::int64_t descriptor = -1;
    const auto parsed = std::from_chars(begin, end, descriptor, 10);
    if (begin != end && parsed.ec == std::errc() && parsed.ptr == end &&
        descriptor >= 0 && descriptor != enumeration_fd) {
      ++count;
    }
  }
  const int read_error = errno;
  if (::closedir(directory) != 0 && read_error == 0) {
    throw std::system_error(errno, std::generic_category(),
                            "close /dev/fd for invocation leak test");
  }
  if (read_error != 0) {
    throw std::system_error(read_error, std::generic_category(),
                            "read /dev/fd for invocation leak test");
  }
  return count;
}

/**
 * @brief Creates an executor targeting the built test runtime fixture.
 * @return Operability-validated non-supervised executor.
 * @throws Construction validation errors unchanged.
 */
NonSupervisedIsolatedCpuInvocationExecutor integration_executor() {
  return NonSupervisedIsolatedCpuInvocationExecutor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH));
}

TEST(IsolatedCpuInvocation, CopiesThroughFreshProcessAndPublishesFreshValue) {
  const std::array<std::byte, 6U> source{std::byte{0},   std::byte{1},
                                         std::byte{2},   std::byte{253},
                                         std::byte{254}, std::byte{255}};
  const Value input = integration_input(source);
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.increment_u8", 97U, {input});

  const IsolatedCpuHostInvocationResult result =
      integration_executor().invoke(invocation);

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  EXPECT_TRUE(result.diagnostic.empty());
  EXPECT_NE(result.outputs[0].allocation_identity(),
            input.allocation_identity());
  const std::array<std::byte, 6U> expected{std::byte{1},   std::byte{2},
                                           std::byte{3},   std::byte{254},
                                           std::byte{255}, std::byte{0}};
  EXPECT_EQ(integration_bytes(result.outputs[0]), expected);
}

TEST(IsolatedCpuInvocation, SupportsZeroInputAndExactOutputPlan) {
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.fill_sequence", 113U);
  const IsolatedCpuHostInvocationResult result =
      integration_executor().invoke(invocation);

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(integration_bytes(result.outputs[0]), expected);
}

TEST(IsolatedCpuInvocation, PreservesTypedFailureCancellationAndException) {
  NonSupervisedIsolatedCpuInvocationExecutor executor = integration_executor();
  IsolatedCpuHostInvocationResult failed =
      executor.invoke(integration_invocation("fixture.fail", 129U));
  EXPECT_EQ(failed.outcome, IsolatedCpuInvocationOutcome::PluginFailed);
  EXPECT_TRUE(failed.outputs.empty());
  EXPECT_FALSE(failed.diagnostic.empty());

  IsolatedCpuHostInvocationResult cancelled =
      executor.invoke(integration_invocation("fixture.cancel", 145U));
  EXPECT_EQ(cancelled.outcome, IsolatedCpuInvocationOutcome::Cancelled);
  EXPECT_TRUE(cancelled.outputs.empty());
  EXPECT_FALSE(cancelled.diagnostic.empty());

  IsolatedCpuHostInvocationResult raised =
      executor.invoke(integration_invocation("fixture.throw", 161U));
  EXPECT_EQ(raised.outcome, IsolatedCpuInvocationOutcome::PluginFailed);
  EXPECT_TRUE(raised.outputs.empty());
  EXPECT_NE(raised.diagnostic.find("fixture callback exception"),
            std::string::npos);
}

TEST(IsolatedCpuInvocation, RejectsAbnormalChildExitWithoutPublishingOutput) {
  EXPECT_THROW(integration_executor().invoke(
                   integration_invocation("fixture.crash", 177U)),
               IsolatedCpuInvocationError);
}

TEST(IsolatedCpuInvocation, RejectsRightsAfterFirstStreamSegmentWithoutFdLeak) {
  const std::size_t before = count_open_descriptors();
  try {
    static_cast<void>(integration_executor().invoke(
        integration_invocation("fixture.late_rights", 185U)));
    FAIL() << "late ancillary rights were accepted";
  } catch (const IsolatedCpuProtocolError& error) {
    EXPECT_NE(std::string(error.what()).find("after the first stream segment"),
              std::string::npos);
  }
  EXPECT_EQ(count_open_descriptors(), before);
}

TEST(IsolatedCpuInvocation, FreshExecClearsEnvironmentAndUnrelatedDescriptors) {
  ScopedTestFd lower_descriptor_one(::open("/dev/null", O_RDONLY));
  ScopedTestFd lower_descriptor_two(::open("/dev/null", O_RDONLY));
  ScopedTestFd inherited(::open("/dev/null", O_RDONLY));
  ASSERT_GE(lower_descriptor_one.get(), 0);
  ASSERT_GE(lower_descriptor_two.get(), 0);
  ASSERT_GE(inherited.get(), 5);
  IsolatedCpuHostInvocation invocation;
  invocation.identity = integration_identity(193U);
  invocation.operation = "fixture.verify_isolation";
  IsolatedCpuScalarParameter descriptor;
  descriptor.name = "inherited_fd";
  descriptor.kind = IsolatedCpuScalarKind::UnsignedInteger;
  descriptor.unsigned_value = static_cast<std::uint64_t>(inherited.get());
  invocation.parameters.push_back(std::move(descriptor));
  invocation.outputs.push_back(one_byte_output_plan());

  const IsolatedCpuHostInvocationResult result =
      integration_executor().invoke(invocation);
  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  DenseTensorView output(result.outputs[0]);
  ASSERT_EQ(output.storage_size(), 1U);
  EXPECT_EQ(output.data()[0], std::byte{1});
}

TEST(IsolatedCpuInvocation, RepeatedCallsRetireEveryFdMapAndChildLease) {
  NonSupervisedIsolatedCpuInvocationExecutor executor = integration_executor();
  const std::size_t before = count_open_descriptors();
  for (std::uint8_t iteration = 0U; iteration < 20U; ++iteration) {
    IsolatedCpuHostInvocation invocation = integration_invocation(
        "fixture.fill_sequence", static_cast<std::uint8_t>(201U + iteration));
    const IsolatedCpuHostInvocationResult result = executor.invoke(invocation);
    ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
    ASSERT_EQ(result.outputs.size(), 1U);
  }
  EXPECT_EQ(count_open_descriptors(), before);
}

}  // namespace
}  // namespace ps::execution
