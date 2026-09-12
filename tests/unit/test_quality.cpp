#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <limits>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  auto host = root.allocator();
  auto work = [&](std::uint64_t n) { return root.consume({n}); };
  auto measured =
      QualityReport::measured_residual("ill-conditioned-double", 2, 1e-12, host)
          .take_value();
  PS_CHECK(measured.evidence() == QualityEvidence::Measured &&
           !measured.error_bound() && measured.proof_count() == 0);
  PS_CHECK(measured.owned_by(host) && measured.dimension() == 2 &&
           measured.residual() == 1e-12);
  struct Case {
    std::int64_t a, x, b;
    double residual, bound;
  };
  constexpr std::int64_t k = 1LL << 26;
  // Hex upper endpoints computed independently with exact Fraction arithmetic.
  const Case cases[] = {{3, 1, 2, 1, 0x1.5555555555556p-2},
                        {10, 0, 1, 1, 0x1.999999999999bp-4},
                        {k, 0, -1, 1, 0x1.0000000000001p-26},
                        {k, k, -k, 4503599694479360.0, 0x1.0000004000001p26},
                        {1, k, k, 0, 0}};
  for (const auto& example : cases) {
    auto report =
        QualityReport::certify_integer_diagonal(
            "exact-system", &example.a, &example.x, &example.b, 1, host, work)
            .take_value();
    PS_CHECK(report.evidence() == QualityEvidence::CertifiedBound);
    PS_CHECK(report.residual() == example.residual &&
             report.error_bound() == example.bound);
    PS_CHECK(report.proof_row(0).value() ==
             (std::array<std::int64_t, 3>{example.a, example.x, example.b}));
    PS_CHECK(!report.proof_row(1).ok());
    auto retained = report;
    report = {};
    PS_CHECK(retained.snapshot() == "exact-system" &&
             retained.proof_count() == 1);
  }
  const std::int64_t invalid = std::numeric_limits<std::int64_t>::min(),
                     zero = 0, one = 1;
  PS_CHECK(!QualityReport::certify_integer_diagonal("exact", &invalid, &one,
                                                    &one, 1, host, work)
                .ok());
  PS_CHECK(!QualityReport::certify_integer_diagonal("exact", &zero, &one, &one,
                                                    1, host, work)
                .ok());
  PS_CHECK(!QualityReport::certify_integer_diagonal("exact", &one, &one, &one,
                                                    0, host, work)
                .ok());
  PS_CHECK(!QualityReport::measured_residual("bad", 1, -1, host).ok());
  PS_CHECK(!QualityReport::measured_residual(
                "bad", 1, std::numeric_limits<double>::infinity(), host)
                .ok());
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 1;
  ResourceBudget constrained(limits);
  PS_CHECK(
      QualityReport::measured_residual("small", 1, 0, constrained.allocator())
          .status()
          .code == ErrorCode::ResourceExhausted);
  auto no_work = [](std::uint64_t) {
    return Status{ErrorCode::ResourceExhausted, {}};
  };
  PS_CHECK(QualityReport::certify_integer_diagonal("exact", &one, &one, &one, 1,
                                                   host, no_work)
               .status()
               .code == ErrorCode::ResourceExhausted);
  const auto previous = std::fegetround();
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  auto nearest =
      QualityReport::certify_integer_diagonal(
          "nearest", &cases[0].a, &cases[0].x, &cases[0].b, 1, host, work)
          .take_value();
  PS_CHECK(nearest.error_bound() == cases[0].bound &&
           std::fegetround() == FE_UPWARD);
  PS_CHECK(std::fesetround(previous) == 0);
  return 0;
}
