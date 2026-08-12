#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "runtime/resource_ledger.hpp"

namespace ps {
namespace {

static_assert(
    !std::is_copy_constructible<ResourceLedger::PluginResourceToken>::value,
    "plugin tokens must not duplicate Host resource authority");
static_assert(
    !std::is_copy_assignable<ResourceLedger::PluginResourceToken>::value,
    "plugin tokens must not copy-assign Host resource authority");
static_assert(std::is_nothrow_move_constructible<
                  ResourceLedger::PluginResourceToken>::value,
              "plugin token moves must preserve exact settlement");
static_assert(
    !std::is_copy_constructible<ResourceLedger::PluginResourceLease>::value,
    "plugin leases must not duplicate consumed authority");

/**
 * @brief Creates one deterministic nonzero invocation identity digest.
 * @param seed Byte repeated with an increasing offset.
 * @return Complete fixed-size Host identity digest.
 * @throws Nothing.
 */
PluginInvocationIdentityDigest identity_digest(std::uint8_t seed) noexcept {
  PluginInvocationIdentityDigest digest{};
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    digest[index] = static_cast<std::uint8_t>(seed + index);
  }
  return digest;
}

/**
 * @brief Proves issuance, consumption, settlement, and replay are one ledger
 * transaction.
 * @throws Nothing when the Host-only owners preserve exact accounting.
 */
TEST(PluginResourceToken, ConsumesOnceSettlesExactlyAndKeepsReplayTombstone) {
  const PluginResourceVector limits{2U, 3U, 4096U, 2048U, 16U};
  const PluginResourceVector requested{1U, 2U, 1024U, 512U, 5U};
  const PluginInvocationIdentityDigest identity = identity_digest(7U);
  ResourceLedger ledger(ResourceVector{}, {}, limits);

  auto token = ledger.issue_plugin_invocation(identity, requested);
  ASSERT_TRUE(token.active());
  EXPECT_EQ(token.identity_digest(), identity);
  EXPECT_EQ(token.resources(), requested);
  EXPECT_EQ(ledger.plugin_snapshot().reserved, requested);
  EXPECT_EQ(ledger.plugin_snapshot().high_water, requested);

  try {
    static_cast<void>(ledger.issue_plugin_invocation(identity, requested));
    FAIL() << "an issued invocation identity must remain spent";
  } catch (const PluginResourceAdmissionError& error) {
    EXPECT_EQ(error.code(), PluginResourceAdmissionErrorCode::Replay);
  }

  {
    auto lease = std::move(token).consume(identity, requested);
    EXPECT_FALSE(token.active());
    EXPECT_TRUE(lease.active());
    EXPECT_EQ(lease.resources(), requested);
    EXPECT_EQ(ledger.plugin_snapshot().reserved, requested);
  }
  EXPECT_EQ(ledger.plugin_snapshot().reserved, PluginResourceVector{});
  EXPECT_EQ(ledger.plugin_snapshot().high_water, requested);
  EXPECT_THROW(ledger.issue_plugin_invocation(identity, requested),
               PluginResourceAdmissionError);
}

/**
 * @brief Proves quota failure and mismatched consumption publish no partial
 * authority.
 * @throws Nothing when rejected paths preserve exact snapshots and rollback.
 */
TEST(PluginResourceToken, QuotaAndBindingFailuresHaveNoPartialCommitOrLeak) {
  const PluginResourceVector limits{1U, 1U, 1024U, 256U, 4U};
  const PluginResourceVector admitted{1U, 1U, 1024U, 128U, 3U};
  const PluginInvocationIdentityDigest first = identity_digest(31U);
  const PluginInvocationIdentityDigest second = identity_digest(63U);
  ResourceLedger ledger(ResourceVector{}, {}, limits);

  try {
    static_cast<void>(ledger.issue_plugin_invocation(
        second, PluginResourceVector{1U, 1U, 1024U, 257U, 1U}));
    FAIL() << "over-quota plugin resources must fail closed";
  } catch (const PluginResourceAdmissionError& error) {
    EXPECT_EQ(error.code(), PluginResourceAdmissionErrorCode::QuotaExceeded);
  }
  EXPECT_EQ(ledger.plugin_snapshot().reserved, PluginResourceVector{});

  {
    auto token = ledger.issue_plugin_invocation(first, admitted);
    EXPECT_THROW(std::move(token).consume(second, admitted),
                 PluginResourceAdmissionError);
    EXPECT_TRUE(token.active());
    EXPECT_EQ(ledger.plugin_snapshot().reserved, admitted);
  }
  EXPECT_EQ(ledger.plugin_snapshot().reserved, PluginResourceVector{});

  auto retry = ledger.issue_plugin_invocation(second, admitted);
  EXPECT_TRUE(retry.active());
}

}  // namespace
}  // namespace ps
