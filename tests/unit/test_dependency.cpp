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
int translated_pieces() {
  const auto all = Footprint::all({5}).take_value();
  const auto first =
      Footprint::from_regions({5}, {Region({{0, 2}})}).take_value();
  const auto second =
      Footprint::from_regions({5}, {Region({{2, 3}})}).take_value();
  std::vector<DependencyMapPiece> pieces{{first, {{0, 1, {{0, {}, 3}}, {}}}},
                                         {second, {{0, 1, {{0, {}, -2}}, {}}}}};
  auto certificate =
      DependencyCertificate::create_mapped("translated", all, {{5}}, pieces);
  PS_CHECK(certificate.ok());
  const std::uint64_t expected[] = {3, 4, 0, 1, 2};
  for (std::uint64_t i = 0; i < 5; ++i) {
    auto query = Footprint::from_regions({5}, {Region({{i, 1}})}).take_value();
    auto source =
        Footprint::from_regions({5}, {Region({{expected[i], 1}})}).take_value();
    PS_CHECK(certificate.value().backward(query).value()[0].samples == source);
    PS_CHECK(certificate.value().transpose({0, 1, source, {}}).value() ==
             query);
    auto restricted = certificate.value().restrict(query).take_value();
    PS_CHECK(restricted.row({i}).value().inputs[0].samples == source);
    auto explicit_row =
        DependencyCertificate::create("translated", query, {{5}},
                                      {{{i}, {{0, 1, source, {}}}}})
            .take_value();
    PS_CHECK(restricted.merge(explicit_row).ok());
  }
  auto left = certificate.value().restrict(first).take_value();
  auto right = certificate.value().restrict(second).take_value();
  PS_CHECK(left.merge(right).value().backward(all).value()[0].samples == all);
  for (std::int64_t shift :
       {INT64_MIN, INT64_MAX, static_cast<std::int64_t>(-1),
        static_cast<std::int64_t>(4)}) {
    auto invalid = pieces;
    invalid[0].inputs[0].axes[0].translation = shift;
    PS_CHECK(
        !DependencyCertificate::create_mapped("invalid", all, {{5}}, invalid)
             .ok());
  }
  const std::uint64_t extent = UINT64_C(1) << 40;
  auto huge = Footprint::all({extent}).take_value();
  auto slab = Footprint::from_regions({extent}, {Region({{extent - 5, 5}})})
                  .take_value();
  auto compact = DependencyCertificate::create_mapped(
      "giant", slab, {{5}},
      {{slab,
        {{0, 1, {{0, {}, -static_cast<std::int64_t>(extent - 5)}}, {}}}}});
  PS_CHECK(compact.ok());
  PS_CHECK(compact.value().backward(slab).value()[0].samples == all);
  PS_CHECK(compact.value().transpose({0, 1, all, {}}).value() == slab);
  FootprintLimits low;
  low.maximum_work = 1;
  PS_CHECK(compact.value().transpose({0, 1, all, {}}, low).status().code ==
           ErrorCode::ResourceExhausted);
  const auto negative_domain =
      Footprint::from_regions({(UINT64_C(1) << 63) + 1},
                              {Region({{UINT64_C(1) << 63, 1}})})
          .take_value();
  const auto singleton = Footprint::all({1}).take_value();
  auto negative_edge =
      DependencyCertificate::create_mapped(
          "negative-edge", negative_domain, {{1}},
          {{negative_domain, {{0, 1, {{0, {}, INT64_MIN}}, {}}}}})
          .take_value();
  PS_CHECK(negative_edge.backward(negative_domain).value()[0].samples ==
           singleton);
  PS_CHECK(negative_edge.transpose({0, 1, singleton, {}}).value() ==
           negative_domain);
  auto positive_edge = DependencyCertificate::create_mapped(
                           "positive-edge", singleton, {{UINT64_C(1) << 63}},
                           {{singleton, {{0, 1, {{0, {}, INT64_MAX}}, {}}}}})
                           .take_value();
  auto tail =
      Footprint::from_regions({UINT64_C(1) << 63},
                              {Region({{UINT64_C(0x7ffffffffffffffe), 2}})})
          .take_value();
  PS_CHECK(positive_edge.transpose({0, 1, tail, {}}).value() == singleton);
  static_cast<void>(huge);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(exhaustive() == 0);
  PS_CHECK(boundaries() == 0);
  PS_CHECK(translated_pieces() == 0);
  return 0;
}
