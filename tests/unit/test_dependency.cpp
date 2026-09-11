#include <cstdint>
#include <vector>

#include "photospider/data/dependency.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Footprint bits(unsigned mask) {
  std::vector<Region> regions;
  for (unsigned i = 0; i < 3; ++i)
    if ((mask >> i) & 1U)
      regions.emplace_back(std::vector<RegionDimension>{{i, 1}});
  return Footprint::from_regions({3}, regions).take_value();
}
DependencyCertificate relation(unsigned table, unsigned coverage) {
  std::vector<AtomCertificate> rows;
  for (unsigned o = 0; o < 3; ++o)
    if ((coverage >> o) & 1U)
      rows.push_back({{o}, {{0, 1, bits((table >> (o * 3)) & 7), {}}}});
  return DependencyCertificate::create("contract/snapshot", bits(coverage),
                                       {{3}}, rows)
      .take_value();
}
int exhaustive() {
  for (unsigned table = 0; table < 512; ++table) {
    const auto full = relation(table, 7);
    for (unsigned coverage = 0; coverage < 8; ++coverage) {
      const auto restricted = full.restrict(bits(coverage)).take_value();
      const auto independent = relation(table, coverage);
      for (unsigned dirty = 0; dirty < 8; ++dirty) {
        auto transposed =
            restricted.transpose({0, 1, bits(dirty), {}}).take_value();
        auto other =
            independent.transpose({0, 1, bits(dirty), {}}).take_value();
        PS_CHECK(transposed == other);
        for (unsigned o = 0; o < 3; ++o) {
          const bool expected =
              ((coverage >> o) & 1U) && (((table >> (o * 3)) & 7) & dirty);
          PS_CHECK(transposed.contains({o}) == expected);
        }
      }
      for (unsigned query = 0; query < 8; ++query) {
        const auto backward = restricted.backward(bits(query));
        if (query & ~coverage) {
          PS_CHECK(!backward.ok());
          continue;
        }
        PS_CHECK(backward.ok());
        unsigned expected = 0;
        for (unsigned o = 0; o < 3; ++o)
          if ((query >> o) & 1U)
            expected |= (table >> (o * 3)) & 7;
        auto actual = bits(0);
        for (const auto& need : backward.value())
          actual = actual.unite(need.samples).take_value();
        PS_CHECK(actual == bits(expected));
      }
      const auto complement = full.restrict(bits(7 & ~coverage)).take_value();
      auto merged = restricted.merge(complement);
      PS_CHECK(merged.ok());
      PS_CHECK(merged.value().merge(full).ok());
    }
  }
  return 0;
}
int boundaries() {
  auto identity = relation(1 | (2 << 3) | (4 << 6), 7);
  auto swapped = relation(2 | (1 << 3) | (4 << 6), 7);
  PS_CHECK(identity.backward(bits(7)).value()[0].samples ==
           swapped.backward(bits(7)).value()[0].samples);
  PS_CHECK(identity.transpose({0, 1, bits(1), {}}).value() == bits(1));
  PS_CHECK(swapped.transpose({0, 1, bits(1), {}}).value() == bits(2));
  PS_CHECK(!identity.merge(swapped).ok());
  auto tags =
      DependencyCertificate::create(
          "tags", bits(1), {{3}, {3}},
          {{{0}, {{0, 3, bits(2), {{1, 42}}}, {1, 4, bits(0), {{2, 7}}}}}})
          .take_value();
  PS_CHECK(tags.transpose({0, 2, bits(0), {{1, 42}}}).value() == bits(1));
  PS_CHECK(tags.transpose({1, 2, bits(0), {{1, 42}}}).value().empty());
  PS_CHECK(tags.transpose({1, 4, bits(0), {{2, 7}}}).value() == bits(1));
  PS_CHECK(tags.transpose({0, 8, bits(2), {}}).value().empty());
  PS_CHECK(tags.backward(bits(0)).value().empty());
  PS_CHECK(!tags.restrict(bits(2)).ok());
  PS_CHECK(
      !DependencyCertificate::create("missing", bits(3), {{3}}, {{{0}, {}}})
           .ok());
  PS_CHECK(!DependencyCertificate::create("duplicate", bits(3), {{3}},
                                          {{{0}, {}}, {{0}, {}}})
                .ok());
  PS_CHECK(!DependencyCertificate::create("bad port", bits(1), {{3}},
                                          {{{0}, {{1, 1, bits(1), {}}}}})
                .ok());
  PS_CHECK(!DependencyCertificate::create("bad roles", bits(1), {{3}},
                                          {{{0}, {{0, 16, bits(1), {}}}}})
                .ok());
  FootprintLimits limits;
  limits.maximum_boxes = 1;
  PS_CHECK(!DependencyCertificate::create("bounded", bits(1), {{3}},
                                          {{{0}, {{0, 1, bits(1), {}}}}},
                                          limits)
                .ok());
  limits.maximum_boxes = 4;
  PS_CHECK(!DependencyCertificate::create("role expansion", bits(1), {{3}},
                                          {{{0}, {{0, 15, bits(1), {{1, 1}}}}}},
                                          limits)
                .ok());
  limits.maximum_work = 1;
  // Even unmatched rows consume traversal work; no intermediate intersection
  // or nonempty output exists to enforce this budget on the caller's behalf.
  PS_CHECK(identity.transpose({0, 8, bits(1), {}}, limits).status().code ==
           ErrorCode::ResourceExhausted);
  limits.maximum_work = 1048576;
  limits.maximum_boxes = 2;
  PS_CHECK(identity.merge(identity, limits).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(identity.transpose({0, 1, bits(7), {}}, limits).status().code ==
           ErrorCode::ResourceExhausted);
  limits.maximum_boxes = 9;
  PS_CHECK(identity.merge(identity, limits).ok());
  CancellationSource cancellation;
  cancellation.cancel();
  limits.cancellation = cancellation.token();
  PS_CHECK(identity.transpose({0, 1, bits(1), {}}, limits).status().code ==
           ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(exhaustive() == 0);
  PS_CHECK(boundaries() == 0);
  return 0;
}
