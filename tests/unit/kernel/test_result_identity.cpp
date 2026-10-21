#include <algorithm>
#include <array>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

#include "data/result_identity.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

int main() {
  using ps::data_internal::ResultIdentity;
  static_assert(!std::is_copy_constructible_v<ResultIdentity>);
  static_assert(!std::is_copy_assignable_v<ResultIdentity>);
  auto schema = ps::test::fixed_fixture::schema();
  auto reserved = ResultIdentity::reserve();
  PS_REQUIRE_OK(reserved);
  auto token = reserved.take_value();
  const auto id = token.id();
  auto moved = std::move(token);
  PS_CHECK(id && !token.id() && moved.id() == id);
  auto made =
      std::move(moved).materialize(ps::ResourceBudget{}, schema, "reserved");
  PS_REQUIRE_OK(made);
  PS_CHECK(!moved.id());
  auto builder = made.take_value();
  const std::uint8_t bytes[] = {1, 2, 3, 4};
  PS_REQUIRE_OK(builder.publish_tensor(0, ps::Region::whole({4}),
                                       {bytes, sizeof(bytes)}));
  auto sealed = builder.seal();
  PS_REQUIRE_OK(sealed);
  auto result = sealed.take_value();
  PS_CHECK(result.object_id() == id);
  auto again =
      std::move(moved).materialize(ps::ResourceBudget{}, schema, "repeated");
  PS_CHECK(!again.ok() &&
           again.status().code == ps::ErrorCode::InvalidArgument);
  auto descriptor = result.descriptor();
  PS_REQUIRE_OK(descriptor);
  auto ordinary =
      ps::ResultBuilder::start(ps::ResourceBudget{}, schema, "ordinary");
  PS_REQUIRE_OK(ordinary);
  auto ordinary_builder = ordinary.take_value();
  PS_REQUIRE_OK(ordinary_builder.publish_tensor(0, ps::Region::whole({4}),
                                                {bytes, sizeof(bytes)}));
  auto other = ordinary_builder.seal();
  PS_REQUIRE_OK(other);
  PS_CHECK(other.value().object_id() != id);
  std::uint8_t sample = 0;
  PS_CHECK(
      !other.value().read_tensor(descriptor.value(), 0, {0}, &sample, 1).ok());
  PS_REQUIRE_OK(result.read_tensor(descriptor.value(), 0, {0}, &sample, 1));
  PS_CHECK(sample == 1);

  // Rejected construction burns its reservation, including resource rejection.
  auto rejected_token = ResultIdentity::reserve();
  PS_REQUIRE_OK(rejected_token);
  auto rejected_identity = rejected_token.take_value();
  const auto rejected_id = rejected_identity.id();
  ps::ResourceLimits limits;
  limits.capacity[ps::ResourceKind::Metadata] = 0;
  ps::ResourceBudget budget(limits);
  auto rejected =
      std::move(rejected_identity).materialize(budget, schema, "budget");
  PS_CHECK(!rejected.ok() &&
           rejected.status().code == ps::ErrorCode::ResourceExhausted);
  PS_CHECK(!rejected_identity.id());
  PS_CHECK(budget.statistics().live[ps::ResourceKind::Metadata] == 0);
  auto fresh = ResultIdentity::reserve();
  PS_REQUIRE_OK(fresh);
  PS_CHECK(fresh.value().id() != rejected_id);

  // Ordinary builders and deferred identities share one concurrent allocator.
  std::array<std::array<std::uint64_t, 64>, 4> identities{};
  std::array<std::thread, 4> threads;
  for (unsigned worker = 0; worker < threads.size(); ++worker) {
    threads[worker] = std::thread([&, worker] {
      for (unsigned i = 0; i < identities[worker].size(); ++i) {
        if (i % 2) {
          auto value = ResultIdentity::reserve();
          if (value.ok())
            identities[worker][i] = value.value().id();
        } else {
          auto value = ps::ResultBuilder::start(ps::ResourceBudget{}, schema,
                                                "concurrent");
          if (value.ok())
            identities[worker][i] = value.value().reference().object_id();
        }
      }
    });
  }
  for (auto& thread : threads)
    thread.join();
  std::array<std::uint64_t, 256> sorted{};
  for (unsigned i = 0; i < identities.size(); ++i)
    std::copy(identities[i].begin(), identities[i].end(),
              sorted.begin() + i * identities[i].size());
  std::sort(sorted.begin(), sorted.end());
  PS_CHECK(sorted.front() != 0);
  PS_CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
  return 0;
}
