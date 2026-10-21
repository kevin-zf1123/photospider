#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <memory>

#include "photospider/plugin/operation_extensions.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
const ExtensionIdentity identity{"test.counter", 1, "counter-v1"};
DiagnosticDefinition definition() {
  DiagnosticDefinition result;
  result.identity = identity;
  result.payload_bytes = sizeof(std::uint64_t);
  result.validate = [](ByteView bytes) {
    return bytes.size() == sizeof(std::uint64_t)
               ? Status::success()
               : Status{ErrorCode::InvalidArgument, "counter size"};
  };
  result.merge = [](ByteView before, ByteView incoming,
                    DiagnosticDestination output) {
    std::uint64_t a = 0, b = 0;
    std::memcpy(&a, before.data(), sizeof(a));
    std::memcpy(&b, incoming.data(), sizeof(b));
    // Write before rejecting to check that a failed merge cannot commit bytes.
    std::memset(output.data, 0xff, output.size);
    if (b > UINT64_MAX - a)
      return Status{ErrorCode::ResourceExhausted, "counter overflow"};
    a += b;
    std::memcpy(output.data, &a, sizeof(a));
    return Status::success();
  };
  return result;
}
ByteView bytes(const std::uint64_t& value) {
  return {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)};
}
int accumulation() {
  ResourceBudget root;
  {
    auto type = std::make_shared<const DiagnosticDefinition>(definition());
    DiagnosticAccumulator first(root);
    PS_REQUIRE_OK(first.initialize({type}));
    PS_CHECK(first.find(identity).status().code == ErrorCode::NotFound);
    const std::uint64_t seven = 7, three = 3, overflow = UINT64_MAX;
    PS_REQUIRE_OK(first.report(identity, bytes(seven)));
    DiagnosticAccumulator snapshot(first);
    PS_REQUIRE_OK(snapshot.report(0, bytes(three)));
    PS_CHECK(snapshot.report(identity, bytes(overflow)).code ==
             ErrorCode::ResourceExhausted);
    PS_CHECK(snapshot.initialize({type, type}).code ==
             ErrorCode::InvalidArgument);
    std::uint64_t observed = 0;
    auto copied = snapshot.find(identity);
    PS_REQUIRE_OK(copied);
    std::memcpy(&observed, copied.value().data(), sizeof(observed));
    PS_CHECK(observed == 10);
    PS_REQUIRE_OK(first.merge(snapshot));
    auto combined = first.find(identity);
    PS_REQUIRE_OK(combined);
    std::memcpy(&observed, combined.value().data(), sizeof(observed));
    PS_CHECK(observed == 17);
    snapshot.clear();
    PS_CHECK(snapshot.find(identity).status().code == ErrorCode::NotFound);
    PS_REQUIRE_OK(first.merge(snapshot));
    auto unchanged = first.find(identity);
    PS_REQUIRE_OK(unchanged);
    std::memcpy(&observed, unchanged.value().data(), sizeof(observed));
    PS_CHECK(observed == 17);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int owner_environment() {
  struct Restore {
    std::fenv_t saved{};
    Restore() { std::fegetenv(&saved); }
    ~Restore() { std::fesetenv(&saved); }
  } restore;
  for (unsigned mode = 0; mode < 3; ++mode) {
    unsigned definitions_destroyed = 0, owners_destroyed = 0;
    PS_CHECK(std::fesetround(FE_DOWNWARD) == 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    ResourceBudget root;
    {
      DiagnosticAccumulator accumulator(root);
      auto type = std::shared_ptr<DiagnosticDefinition>(
          new DiagnosticDefinition(definition()), [&](auto* value) {
            delete value;
            ++definitions_destroyed;
            std::fesetround(FE_UPWARD);
            std::feraiseexcept(FE_INVALID);
          });
      type->owner =
          std::shared_ptr<const void>(new int(1), [&](const void* owner) {
            delete static_cast<const int*>(owner);
            ++owners_destroyed;
            std::fesetround(FE_TOWARDZERO);
            std::feraiseexcept(FE_OVERFLOW);
          });
      PS_REQUIRE_OK(accumulator.initialize({type}));
      type.reset();
      if (mode == 1)
        PS_REQUIRE_OK(accumulator.initialize({}));
      if (mode == 2)
        accumulator = DiagnosticAccumulator(root);
      PS_CHECK(std::fegetround() == FE_DOWNWARD &&
               std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    }
    PS_CHECK(definitions_destroyed == 1 && owners_destroyed == 1);
    PS_CHECK(std::fegetround() == FE_DOWNWARD &&
             std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(accumulation() == 0);
  PS_CHECK(owner_environment() == 0);
  return 0;
}
