#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "photospider/data/footprint.hpp"
#include "support/test_support.hpp"

namespace {
using ps::Footprint;
using ps::Region;
using ps::Status;
using Point = std::vector<std::uint64_t>;
Footprint subset(unsigned mask) {
  std::vector<Region> boxes;
  for (unsigned i = 0; i < 6; ++i)
    if ((mask >> i) & 1U)
      boxes.emplace_back(
          std::vector<ps::RegionDimension>{{i / 3, 1}, {i % 3, 1}});
  return Footprint::from_regions({2, 3}, boxes).take_value();
}
int exhaustive() {
  for (unsigned a = 0; a < 64; ++a) {
    const auto left = subset(a);
    std::vector<Point> order;
    PS_CHECK(left.visit(
                     [&](const auto& c) {
                       order.push_back(c);
                       return Status::success();
                     },
                     6)
                 .ok());
    PS_CHECK(std::is_sorted(order.begin(), order.end()));
    PS_CHECK(std::set<Point>(order.begin(), order.end()).size() ==
             order.size());
    PS_CHECK(left.element_count().value() == order.size());
    PS_CHECK(left.unite(left).value() == left);
    PS_CHECK(left.subtract(left).value().empty());
    for (unsigned b = 0; b < 64; ++b) {
      const auto right = subset(b);
      const auto joined = left.unite(right);
      const auto common = left.intersect(right);
      const auto difference = left.subtract(right);
      PS_CHECK(joined.ok() && common.ok() && difference.ok());
      // Independent literal bit-mask oracle; no mapper is used for expected
      // membership. Equal-set canonicalization is checked independently too.
      for (unsigned p = 0; p < 6; ++p) {
        const Point point{p / 3, p % 3};
        PS_CHECK(joined.value().contains(point) == !!(((a | b) >> p) & 1U));
        PS_CHECK(common.value().contains(point) == !!(((a & b) >> p) & 1U));
        PS_CHECK(difference.value().contains(point) ==
                 !!(((a & ~b) >> p) & 1U));
      }
      PS_CHECK(joined.value() == subset(a | b));
      PS_CHECK(common.value() == subset(a & b));
      PS_CHECK(difference.value() == subset(a & ~b));
      auto reversed = joined.value().boxes();
      std::reverse(reversed.begin(), reversed.end());
      PS_CHECK(Footprint::from_regions({2, 3}, reversed).value() ==
               joined.value());
    }
  }
  return 0;
}
int single_rectangle_limits() {
  for (std::size_t rank = 1; rank <= 8; ++rank) {
    const Point shape(rank, 4);
    std::vector<ps::RegionDimension> whole(rank, {0, 4});
    auto left = whole, right = whole;
    left[0] = {0, 2};
    right[0] = {2, 2};
    auto canonical = Footprint::from_regions(shape, {Region(whole)});
    auto split = Footprint::from_regions(shape, {Region(left), Region(right)});
    PS_CHECK(canonical.ok() && split.ok() &&
             canonical.value() == split.value());
  }
  ps::FootprintLimits limits;
  std::uint64_t measured = 999;
  limits.maximum_work = 0;
  PS_CHECK(Footprint::from_regions({4}, {Region({{0, 2}})}, limits, &measured)
                   .status()
                   .code == ps::ErrorCode::ResourceExhausted &&
           measured == 0);
  limits.maximum_work = 100;
  limits.maximum_boxes = 0;
  PS_CHECK(
      Footprint::from_regions({4}, {Region({{0, 2}})}, limits).status().code ==
      ps::ErrorCode::ResourceExhausted);
  limits.maximum_boxes = 1;
  ps::CancellationSource stop;
  unsigned calls = 0;
  limits.cancellation = stop.token();
  limits.consume_work = [&](std::uint64_t) {
    if (++calls == 2)
      stop.cancel();
    return Status::success();
  };
  auto cancelled = Footprint::from_regions(
      {4}, {Region({{0, 2}}), Region({{2, 0}})}, limits, &measured);
  PS_CHECK(cancelled.status().code == ps::ErrorCode::Cancelled &&
           measured == 2);
  return 0;
}
int bounds() {
  using ps::ErrorCode;
  const Point huge{UINT64_MAX, UINT64_MAX, 7, 2, 3, 4, 5, 6};
  auto all = Footprint::all(huge);
  PS_CHECK(all.ok() && all.value().boxes().size() == 1);
  PS_CHECK(all.value().element_count().status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(all.value().contains({UINT64_MAX - 1, 0, 0, 0, 0, 0, 0, 0}));
  PS_CHECK(!all.value().contains({UINT64_MAX, 0, 0, 0, 0, 0, 0, 0}));
  const auto none = Footprint::none(huge).take_value();
  PS_CHECK(all.value().subtract(none).value() == all.value());
  PS_CHECK(!Footprint::all({}).ok());
  PS_CHECK(!Footprint::all({2, 0}).ok());
  PS_CHECK(!Footprint::from_regions({2}, {Region({{2, 1}})}).ok());
  PS_CHECK(!Footprint{}.unite(Footprint{}).ok());
  PS_CHECK(!subset(1).unite(Footprint::all({6}).value()).ok());
  auto fragments =
      Footprint::from_regions({1000000000},
                              {Region({{0, 1}}), Region({{999999999, 1}})})
          .take_value();
  auto tiles = fragments.tile_cover({4}).take_value();
  PS_CHECK(tiles.element_count().value() == 2);
  PS_CHECK(tiles.contains({0}) && tiles.contains({249999999}));
  PS_CHECK(!tiles.contains({1}));
  PS_CHECK(!fragments.tile_cover({0}).ok());
  auto count =
      all.value().visit([](const auto&) { return Status::success(); }, 3);
  PS_CHECK(count.code == ErrorCode::ResourceExhausted);
  ps::CancellationSource cancel;
  auto status = fragments.visit(
      [&](const auto&) {
        cancel.cancel();
        return Status::success();
      },
      2, cancel.token());
  PS_CHECK(status.code == ErrorCode::Cancelled);
  ps::FootprintLimits limits;
  limits.maximum_boxes = 1;
  PS_CHECK(Footprint::from_regions(fragments.shape(), fragments.boxes(), limits)
               .status()
               .code == ErrorCode::ResourceExhausted);
  limits.maximum_boxes = 65536;
  limits.maximum_work = 4;
  const std::vector<Region> duplicate(20, Region({{0, 1}}));
  PS_CHECK(Footprint::from_regions({1}, duplicate, limits).status().code ==
           ErrorCode::ResourceExhausted);
  limits.cancellation = cancel.token();
  PS_CHECK(Footprint::none({1}, limits).status().code == ErrorCode::Cancelled);
  // Different decompositions of the same L-shaped set must canonicalize alike.
  const auto a = Footprint::from_regions({3, 3}, {Region({{0, 3}, {0, 1}}),
                                                  Region({{0, 1}, {0, 3}})})
                     .take_value();
  const auto b = Footprint::from_regions({3, 3}, {Region({{1, 2}, {0, 1}}),
                                                  Region({{0, 1}, {0, 3}})})
                     .take_value();
  PS_CHECK(a == b && a.element_count().value() == 5);
  return 0;
}
int indexed_limits_and_sweep() {
  const auto single =
      Footprint::from_regions({8}, {Region({{1, 1}})}).take_value();
  const auto sparse =
      Footprint::from_regions({8}, {Region({{1, 2}}), Region({{6, 1}})})
          .take_value();
  ps::FootprintLimits limits;
  limits.maximum_boxes = 0;
  PS_CHECK(single.unite(single, limits).status().code ==
           ps::ErrorCode::ResourceExhausted);
  PS_CHECK(single.intersect(single, limits).status().code ==
           ps::ErrorCode::ResourceExhausted);
  for (std::uint64_t bound = 0; bound < 100; ++bound) {
    limits.maximum_boxes = 100;
    limits.maximum_work = bound;
    std::uint64_t consumed = 0;
    limits.consume_work = [&](auto n) {
      consumed += n;
      return Status::success();
    };
    auto joined = single.unite(sparse, limits);
    PS_CHECK(consumed <= bound);
    if (joined.ok())
      PS_CHECK(joined.value() == sparse);
    consumed = 0;
    auto common = single.intersect(sparse, limits);
    PS_CHECK(consumed <= bound);
    if (common.ok())
      PS_CHECK(common.value() == single);
    consumed = 0;
    auto removed = single.subtract(sparse, limits);
    PS_CHECK(consumed <= bound);
    if (removed.ok())
      PS_CHECK(removed.value().empty());
  }
  std::vector<Region> nested;
  for (std::uint64_t i = 0; i < 256; ++i)
    nested.emplace_back(std::vector<ps::RegionDimension>{{i, 512 - 2 * i}});
  std::uint64_t work = 0;
  auto normalized = Footprint::from_regions({512}, nested, {}, &work);
  PS_REQUIRE_OK(normalized);
  PS_CHECK(normalized.value() == Footprint::all({512}).take_value());
  PS_CHECK(work < 64 * nested.size());
  return 0;
}
int event_sort_limits() {
  // Exercise large event sets, equal end/begin coordinates, and high unsigned
  // digits against literal interval membership instead of a sorting oracle.
  const auto base = UINT64_MAX - 256;
  std::vector<Region> gaps, fills;
  for (std::uint64_t i = 0; i < 64; ++i) {
    gaps.emplace_back(std::vector<ps::RegionDimension>{{base + 2 * i, 1}});
    fills.emplace_back(std::vector<ps::RegionDimension>{{base + 2 * i + 1, 1}});
  }
  gaps.emplace_back(std::vector<ps::RegionDimension>{{0, 1}});
  auto sparse = Footprint::from_regions({UINT64_MAX}, gaps);
  PS_REQUIRE_OK(sparse);
  PS_CHECK(sparse.value().boxes().size() == 65);
  for (std::uint64_t i = 0; i < 128; ++i)
    PS_CHECK(sparse.value().contains({base + i}) == !(i & 1U));
  std::reverse(gaps.begin(), gaps.end());
  PS_CHECK(Footprint::from_regions({UINT64_MAX}, gaps).value() ==
           sparse.value());
  gaps.insert(gaps.end(), fills.begin(), fills.end());
  auto joined = Footprint::from_regions({UINT64_MAX}, gaps);
  PS_REQUIRE_OK(joined);
  auto expected = Footprint::from_regions(
      {UINT64_MAX}, {Region({{0, 1}}), Region({{base, 128}})});
  PS_REQUIRE_OK(expected);
  PS_CHECK(joined.value() == expected.value());
  auto filled_gaps = Footprint::from_regions({UINT64_MAX}, fills);
  PS_REQUIRE_OK(filled_gaps);
  PS_CHECK(joined.value().subtract(filled_gaps.value()).value() ==
           sparse.value());
  PS_CHECK(sparse.value().intersect(filled_gaps.value()).value().empty());
  bool completed = false;
  for (const std::uint64_t bound : {0, 128, 256, 512, 4096, 16384}) {
    ps::FootprintLimits limits;
    limits.maximum_work = bound;
    std::uint64_t charged = 0, measured = 0;
    limits.consume_work = [&](auto n) {
      charged += n;
      return Status::success();
    };
    auto limited =
        Footprint::from_regions({UINT64_MAX}, gaps, limits, &measured);
    PS_CHECK(charged == measured && measured <= bound);
    if (limited.ok()) {
      PS_CHECK(limited.value() == expected.value());
      completed = true;
    } else {
      PS_CHECK(limited.status().code == ps::ErrorCode::ResourceExhausted);
    }
  }
  PS_CHECK(completed);
  ps::FootprintLimits limits;
  bool rejected = false;
  limits.consume_work = [&](auto n) {
    if (n > 1) {
      rejected = true;
      return Status{ps::ErrorCode::ResourceExhausted, "event pass denied"};
    }
    return Status::success();
  };
  PS_CHECK(Footprint::from_regions({UINT64_MAX}, gaps, limits).status().code ==
           ps::ErrorCode::ResourceExhausted);
  PS_CHECK(rejected);
  ps::CancellationSource stop;
  limits.cancellation = stop.token();
  limits.consume_work = [&](auto n) {
    if (n > 1)
      stop.cancel();
    return Status::success();
  };
  PS_CHECK(Footprint::from_regions({UINT64_MAX}, gaps, limits).status().code ==
           ps::ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(exhaustive() == 0);
  PS_CHECK(indexed_limits_and_sweep() == 0);
  PS_CHECK(event_sort_limits() == 0);
  PS_CHECK(bounds() == 0);
  PS_CHECK(single_rectangle_limits() == 0);
  return 0;
}
