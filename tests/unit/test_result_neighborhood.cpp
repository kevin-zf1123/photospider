#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Points = std::set<std::uint64_t>;
Points points(const Footprint& footprint) {
  Points result;
  auto status = footprint.visit(
      [&](const auto& at) {
        std::uint64_t flat = 0;
        for (std::size_t axis = 0; axis < at.size(); ++axis)
          flat = flat * footprint.shape()[axis] + at[axis];
        result.insert(flat);
        return Status::success();
      },
      1000);
  if (!status.ok())
    throw std::runtime_error(status.message);
  return result;
}
bool near(std::uint64_t a, std::uint64_t b, std::uint64_t size,
          std::uint64_t radius, bool periodic) {
  auto distance = a > b ? a - b : b - a;
  if (periodic)
    distance = std::min(distance, size - distance);
  return distance <= radius;
}
int oracle() {
  ResourceBudget root;
  {
    for (const auto& shape : {std::vector<std::uint64_t>{3, 5}, {1, 4}, {5, 1}})
      for (bool periodic : {false, true})
        for (auto ry : {UINT64_C(0), UINT64_C(1), UINT64_C(6), UINT64_MAX})
          for (auto rx : {UINT64_C(0), UINT64_C(2), UINT64_C(7)}) {
            const ResultSupport source{2, 5, 0, 0, ResultSupportTarget::Tensor,
                                       3};
            auto made = ResultRelation::neighborhood(root, shape, {ry, rx},
                                                     periodic, source);
            PS_CHECK(made.ok());
            auto relation = made.take_value();
            const auto all = Footprint::all(shape).take_value();
            PS_CHECK(relation.guarantee() == DependencyGuarantee::Exact &&
                     relation.certify(all).ok());
            std::vector<Region> queried, edited;
            Points expected_forward, expected_backward;
            for (std::uint64_t y = 0; y < shape[0]; ++y)
              for (std::uint64_t x = 0; x < shape[1]; ++x) {
                const auto flat = y * shape[1] + x;
                if (flat % 3 == 0)
                  queried.emplace_back(Region({{y, 1}, {x, 1}}));
                if (flat % 4 == 0)
                  edited.emplace_back(Region({{y, 1}, {x, 1}}));
                Points expected;
                bool dirty = false;
                for (std::uint64_t sy = 0; sy < shape[0]; ++sy)
                  for (std::uint64_t sx = 0; sx < shape[1]; ++sx) {
                    const auto sample = sy * shape[1] + sx;
                    if (!near(y, sy, shape[0], ry, periodic) ||
                        !near(x, sx, shape[1], rx, periodic))
                      continue;
                    expected.insert(sample);
                    if (flat % 3 == 0)
                      expected_forward.insert(sample);
                    dirty |= sample % 4 == 0;
                  }
                if (flat % 3 == 0 && dirty)
                  expected_backward.insert(flat);
                Points visited;
                PS_CHECK(
                    relation
                        .visit(flat, 10000,
                               [&](ResultSupport span) {
                                 if (span.input != 2 || span.roles != 5 ||
                                     span.slot != 3 ||
                                     span.target != ResultSupportTarget::Tensor)
                                   return Status{ErrorCode::Internal,
                                                 "wrong support address"};
                                 for (std::uint64_t i = 0; i < span.count; ++i)
                                   visited.insert(span.first + i);
                                 return Status::success();
                               })
                        .ok());
                PS_CHECK(visited == expected);
              }
            const auto request =
                Footprint::from_regions(shape, queried).take_value();
            const auto changes =
                Footprint::from_regions(shape, edited).take_value();
            Points projected;
            PS_CHECK(
                relation
                    .project(
                        request,
                        [&](ResultSupport address, const Footprint* samples) {
                          if (!samples || address.input != 2 ||
                              address.slot != 3)
                            return Status{ErrorCode::Internal,
                                          "wrong projected support"};
                          auto actual = points(*samples);
                          projected.insert(actual.begin(), actual.end());
                          return Status::success();
                        })
                    .ok());
            PS_CHECK(projected == expected_forward);
            auto inverse = relation.preimage(request, source, changes);
            PS_CHECK(inverse.ok() &&
                     points(inverse.value()) == expected_backward);
            auto other = source;
            other.roles = 2;
            PS_CHECK(
                relation.preimage(request, other, changes).value().empty());
            auto empty = Footprint::none(shape).take_value();
            unsigned visits = 0;
            PS_CHECK(relation
                         .project(empty,
                                  [&](auto, auto) {
                                    ++visits;
                                    return Status::success();
                                  })
                         .ok());
            PS_CHECK(!visits &&
                     relation.preimage(all, source, empty).value().empty());
          }
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int fragmented() {
  const ResultSupport source{0, 1, 0, 0, ResultSupportTarget::Tensor, 0};
  std::vector<Region> boxes;
  for (uint64_t i = 0; i < 100; ++i)
    boxes.emplace_back(Region({{i * 3, 1}}));
  const auto request = Footprint::from_regions({300}, boxes).take_value();
  ResourceBudget root;
  {
    auto identity =
        ResultRelation::neighborhood(root, {300}, {0}, false, source)
            .take_value();
    PS_CHECK(identity
                 .project(request,
                          [&](auto, const Footprint* samples) {
                            return samples && *samples == request
                                       ? Status::success()
                                       : Status{ErrorCode::Internal,
                                                "fragmented identity"};
                          })
                 .ok());
    FootprintLimits stop_at_bridge;
    stop_at_bridge.consume_work = [&](uint64_t amount) {
      return amount ? root.consume({amount})
                    : Status{ErrorCode::Cancelled, "bridge cancellation"};
    };
    unsigned visits = 0;
    PS_CHECK(identity
                 .project(
                     request,
                     [&](auto, auto) {
                       ++visits;
                       return Status::success();
                     },
                     stop_at_bridge)
                 .code == ErrorCode::Cancelled);
    PS_CHECK(!visits);
    const std::vector<uint64_t> shape(8, 4), radius(8, 1);
    auto neighborhood =
        ResultRelation::neighborhood(root, shape, radius, true, source)
            .take_value();
    auto corner = Footprint::from_regions(
                      shape, {Region(std::vector<RegionDimension>(8, {0, 1}))})
                      .take_value();
    auto expected = Footprint::none(shape).take_value();
    PS_CHECK(neighborhood
                 .project(corner,
                          [&](auto, const Footprint* samples) {
                            if (!samples)
                              return Status{ErrorCode::Internal, {}};
                            expected = *samples;
                            return Status::success();
                          })
                 .ok());
    PS_CHECK(expected.boxes().size() == 256 &&
             expected.element_count().value() == 6561);
    PS_CHECK(expected.contains({3, 0, 1, 3, 0, 1, 3, 0}) &&
             !expected.contains({2, 0, 1, 3, 0, 1, 3, 0}));
    auto inverse = neighborhood.preimage(Footprint::all(shape).take_value(),
                                         source, corner);
    PS_CHECK(inverse.ok() && inverse.value() == expected);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  ResourceLimits small;
  small.capacity[ResourceKind::Metadata] = 8192;
  ResourceBudget limited(small);
  {
    auto relation =
        ResultRelation::neighborhood(limited, {300}, {0}, false, source);
    PS_CHECK(relation.ok());
    PS_CHECK(relation.value()
                 .project(request, [](auto, auto) { return Status::success(); })
                 .code == ErrorCode::ResourceExhausted);
  }
  for (auto live : limited.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int boundaries() {
  ResourceBudget root;
  const ResultSupport source{0, 1, 0, 0, ResultSupportTarget::Tensor, 0};
  const auto n = UINT64_MAX - 4;
  auto large = ResultRelation::neighborhood(root, {n, n}, {3, 2}, false, source)
                   .take_value();
  auto request = Footprint::from_regions({n, n}, {Region({{n - 2, 1}, {0, 1}})})
                     .take_value();
  PS_CHECK(large.certify(request).ok());
  PS_CHECK(large
               .project(request,
                        [&](auto, const Footprint* samples) {
                          auto expected = Footprint::from_regions(
                              {n, n}, {Region({{n - 5, 5}, {0, 3}})});
                          return samples && expected.ok() &&
                                         *samples == expected.value()
                                     ? Status::success()
                                     : Status{ErrorCode::Internal,
                                              "wide neighborhood projection"};
                        })
               .ok());
  auto periodic =
      ResultRelation::neighborhood(root, {7, 9}, {1, 1}, true, source)
          .take_value();
  auto corner =
      Footprint::from_regions({7, 9}, {Region({{0, 1}, {0, 1}})}).take_value();
  const auto before_plain = root.statistics().issued.work;
  auto sink = [](auto, auto) { return Status::success(); };
  PS_CHECK(periodic.project(corner, sink).ok());
  const auto plain_work = root.statistics().issued.work - before_plain;
  FootprintLimits hooked;
  hooked.consume_work = [&](std::uint64_t units) {
    return root.consume({units});
  };
  const auto before_hook = root.statistics().issued.work;
  PS_CHECK(periodic.project(corner, sink, hooked).ok());
  PS_CHECK(root.statistics().issued.work - before_hook == plain_work);
  unsigned visits = 0;
  hooked.consume_work = [](auto) {
    return Status{ErrorCode::Cancelled, "stopped hook"};
  };
  PS_CHECK(periodic
               .project(
                   corner,
                   [&](auto, auto) {
                     ++visits;
                     return Status::success();
                   },
                   hooked)
               .code == ErrorCode::Cancelled);
  PS_CHECK(!visits);
  FootprintLimits limits;
  limits.maximum_boxes = 1;
  PS_CHECK(periodic.project(corner, sink, limits).code ==
           ErrorCode::ResourceExhausted);
  limits = {};
  limits.maximum_work = 1;
  PS_CHECK(periodic.project(corner, sink, limits).code ==
           ErrorCode::ResourceExhausted);
  CancellationSource cancellation;
  cancellation.cancel();
  limits = {};
  limits.cancellation = cancellation.token();
  PS_CHECK(periodic.project(corner, sink, limits).code == ErrorCode::Cancelled);
  PS_CHECK(periodic.preimage(corner, source, corner, limits).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(!ResultRelation::neighborhood(root, {7, 9}, {1}, true, source).ok());
  PS_CHECK(
      !ResultRelation::neighborhood(root, {7, 0}, {1, 1}, true, source).ok());
  auto wrong = source;
  wrong.roles = 8;
  PS_CHECK(
      !ResultRelation::neighborhood(root, {7, 9}, {1, 1}, true, wrong).ok());
  auto reshaped =
      ResultRelation::neighborhood(root, {9, 7}, {1, 1}, false, source)
          .take_value();
  PS_CHECK(!ResultRelation::unite(root, {periodic, reshaped}).ok());
  ResourceLimits low;
  low.capacity[ResourceKind::Metadata] = 1;
  PS_CHECK(ResultRelation::neighborhood(ResourceBudget(low), {7, 9}, {1, 1},
                                        true, source)
               .status()
               .code == ErrorCode::ResourceExhausted);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(oracle() == 0);
  PS_CHECK(boundaries() == 0);
  PS_CHECK(fragmented() == 0);
  return 0;
}
