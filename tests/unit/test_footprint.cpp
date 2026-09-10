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
}  // namespace
int main() {
  PS_CHECK(exhaustive() == 0);
  PS_CHECK(bounds() == 0);
  return 0;
}
