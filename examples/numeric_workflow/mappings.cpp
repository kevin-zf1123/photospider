#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ps::Footprint subset(const std::vector<std::uint64_t>& shape, unsigned mask) {
  std::vector<ps::Region> boxes;
  unsigned index = 0;
  const auto all = take(ps::Footprint::all(shape));
  require(all.visit(
                 [&](const auto& coordinate) {
                   if (mask & (1U << index)) {
                     std::vector<ps::RegionDimension> dims;
                     for (auto c : coordinate)
                       dims.push_back({c, 1});
                     boxes.emplace_back(std::move(dims));
                   }
                   ++index;
                   return ps::Status::success();
                 },
                 8)
              .ok(),
          "small subset enumeration");
  return take(ps::Footprint::from_regions(shape, boxes));
}
bool same(const std::vector<ps::DependencyNeed>& a,
          const std::vector<ps::DependencyNeed>& b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].port != b[i].port || a[i].roles != b[i].roles ||
        a[i].samples != b[i].samples || a[i].tags != b[i].tags)
      return false;
  return true;
}
std::vector<ps::DependencyMappedNeed> mapping() {
  return {{0, 1, {{1, {0, 0}}}, {}}, {0, 4, {{-1, {0, 3}}}, {{1, 7}}}};
}
void comparisons() {
  const auto all = take(ps::Footprint::all({2, 3}));
  auto mapped = take(ps::DependencyCertificate::create_mapped(
      "mapping", all, {{3}}, {{all, mapping()}}));
  std::vector<ps::AtomCertificate> rows;
  for (std::uint64_t i = 0; i < 2; ++i)
    for (std::uint64_t j = 0; j < 3; ++j)
      rows.push_back({{i, j},
                      {{0, 1, subset({3}, 1U << j), {}},
                       {0, 4, subset({3}, 7), {{1, 7}}}}});
  auto explicit_rows =
      take(ps::DependencyCertificate::create("mapping", all, {{3}}, rows));
  require(take(mapped.materialize()).size() == 6,
          "explicit bounded materialization");
  bool rejected = false;
  try {
    static_cast<void>(mapped.rows());
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "mapped rows must not pretend to be an empty row list");
  for (unsigned mask = 0; mask < 64; ++mask) {
    const auto query = subset({2, 3}, mask);
    require(
        same(take(mapped.backward(query)), take(explicit_rows.backward(query))),
        "mapped exact backward differs");
    auto restricted = take(mapped.restrict(query));
    auto expected = take(explicit_rows.restrict(query));
    for (unsigned changed = 0; changed < 8; ++changed) {
      for (std::uint32_t role : {1U, 4U, 5U}) {
        ps::DependencyNeed dirty{0, role, subset({3}, changed), {}};
        require(take(restricted.transpose(dirty)) ==
                    take(expected.transpose(dirty)),
                "mapped exact transpose differs");
      }
    }
    const auto rest = subset({2, 3}, 63U ^ mask);
    auto joined = take(restricted.merge(take(mapped.restrict(rest))));
    require(same(take(joined.backward(all)), take(explicit_rows.backward(all))),
            "mapped merge lost source support");
    auto mixed = take(restricted.merge(take(explicit_rows.restrict(rest))));
    require(same(take(mixed.backward(all)), take(explicit_rows.backward(all))),
            "row/map merge lost support");
  }
  auto tagged = take(mapped.transpose({0, 4, subset({3}, 0), {{1, 7}}}));
  require(tagged == all, "mapped tag transpose");
  auto altered = mapping();
  altered[0].axes[0] = {-1, {0, 1}};
  auto conflict = take(ps::DependencyCertificate::create_mapped(
      "mapping", all, {{3}}, {{all, altered}}));
  require(!mapped.merge(conflict).ok(), "conflicting overlap must fail");
  require(!ps::DependencyCertificate::create_mapped(
               "mapping", all, {{3}}, {{all, mapping()}, {all, mapping()}})
               .ok(),
          "overlapping pieces must fail");
  std::cout << "axis-map certificates: 64 Q subsets x 8 dirty subsets x 3 "
               "roles match explicit rows\n";
}
void large_and_failures() {
  const std::vector<std::uint64_t> shape{UINT64_C(274877906944), 3};
  const auto all = take(ps::Footprint::all(shape));
  auto mapped = take(ps::DependencyCertificate::create_mapped(
      "large", all, {{3}}, {{all, mapping()}}));
  auto need = take(mapped.backward(all));
  require(need.size() == 2 && take(need[0].samples.element_count()) == 3,
          "large view support must stay three samples");
  auto dirty = take(mapped.transpose({0, 1, subset({3}, 2), {}}));
  require(dirty.boxes().size() == 1 && take(dirty.element_count()) == shape[0],
          "large dirty replication must be geometric");
  ps::FootprintLimits small;
  small.maximum_boxes = 16;
  require(!mapped.materialize(small).ok(),
          "large explicit materialization must fail before enumeration");
  auto rebound = take(mapped.with_identity("rebound", small));
  require(rebound.identity() == "rebound" &&
              rebound.metadata_entries() == mapped.metadata_entries(),
          "compact identity rebind");
  small.maximum_boxes = 1;
  require(!mapped.with_identity("too-small", small).ok(),
          "metadata limit must reject copy");
  ps::CancellationSource cancelled;
  cancelled.cancel();
  ps::FootprintLimits stopped;
  stopped.cancellation = cancelled.token();
  require(
      mapped.backward(all, stopped).status().code == ps::ErrorCode::Cancelled,
      "mapped cancellation");
  const auto diagonal_domain = take(ps::Footprint::all({2}));
  require(!ps::DependencyCertificate::create_mapped(
               "diagonal", diagonal_domain, {{2, 2}},
               {{diagonal_domain, {{0, 1, {{0, {0, 0}}, {0, {0, 0}}}, {}}}}})
               .ok(),
          "repeated observation axis cannot become a Cartesian box");
  const auto one = take(ps::Footprint::all({1}));
  auto tags = take(ps::DependencyCertificate::create_mapped(
      "tags", one, {{1}},
      {{one, {{0, 1, {}, {{1, 1}, {1, 2}, {1, 3}, {1, 4}}}}}}));
  ps::FootprintLimits two_entries;
  two_entries.maximum_boxes = 2;
  require(!tags.backward(one, two_entries).ok(),
          "projected tags must obey total metadata limit");
  require(!tags.materialize(two_entries).ok(),
          "materialized row needs must obey total metadata limit");
  const auto two = take(ps::Footprint::all({2}));
  auto empty_support = take(ps::DependencyCertificate::create_mapped(
      "no-input", two, {{1}}, {{subset({2}, 1), {}}, {subset({2}, 2), {}}}));
  ps::FootprintLimits one_work;
  one_work.maximum_work = 1;
  require(!empty_support.transpose({0, 1, one, {}}, one_work).ok(),
          "empty piece traversal must consume work");
  std::vector<ps::AtomCertificate> repeated_rows;
  for (std::uint64_t i = 0; i < 3; ++i)
    repeated_rows.push_back({{i}, {{0, 1, one, {{1, 7}}}}});
  auto repeated = take(ps::DependencyCertificate::create(
      "repeated", take(ps::Footprint::all({3})), {{1}}, repeated_rows));
  require(
      repeated.storage_entries() >= 3 * 10,
      "retained duplicate row coordinates/support must count in cache storage");
  require(
      take(repeated.backward(repeated.coverage())).size() == 1,
      "source support projection is independent from retained storage cost");
  std::vector<ps::Region> stairs;
  for (std::uint64_t j = 0; j < 10; ++j)
    stairs.push_back(ps::Region({{2 * j, 1}, {0, j + 1}}));
  auto staircase = take(ps::Footprint::from_regions({20, 10}, stairs));
  auto permutation = take(ps::DependencyCertificate::create_mapped(
      "permuted-staircase", staircase, {{10, 20}},
      {{staircase, {{0, 7, {{1, {}}, {0, {}}}, {}}}}}));
  ps::FootprintLimits capacity;
  capacity.maximum_boxes = 150;
  require(!permutation.backward(staircase, capacity).ok(),
          "normalized permutation expansion must obey total metadata limit");
  std::cout << "large mapped replication: 274877906944 copies, 3 source "
               "samples, bounded metadata passed\n";
}
}  // namespace
int main() {
  try {
    comparisons();
    large_and_failures();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
