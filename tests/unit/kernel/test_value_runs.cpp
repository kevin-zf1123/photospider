#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "photospider/data/region_runs.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void take(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
void generic_runs() {
  const ValueDescriptor descriptor{ElementType::UInt16, {5, 6, 7}};
  const Region source_region({{1, 3}, {1, 4}, {1, 5}});
  const Region subregion({{1, 3}, {2, 2}, {2, 3}});
  std::array<unsigned, 3> order{0, 1, 2};
  unsigned scenarios = 0;
  do {
    for (unsigned signs = 0; signs < 8; ++signs) {
      for (unsigned broadcast = 0; broadcast <= 3; ++broadcast) {
        std::vector<std::int64_t> strides(3);
        std::int64_t size = 2;
        for (std::size_t i = 3; i-- > 0;) {
          const auto a = order[i];
          strides[a] = size;
          size *= static_cast<std::int64_t>(
              source_region.dimensions()[a].extent + 1);
        }
        std::int64_t min = 0, max = 0;
        for (unsigned a = 0; a < 3; ++a) {
          if (signs & (1U << a))
            strides[a] = -strides[a];
          if (broadcast == a)
            strides[a] = 0;
          const auto delta =
              strides[a] * static_cast<std::int64_t>(
                               source_region.dimensions()[a].extent - 1);
          min += std::min<std::int64_t>(0, delta);
          max += std::max<std::int64_t>(0, delta);
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(max - min + 18));
        for (std::size_t i = 0; i < bytes.size(); ++i)
          bytes[i] = static_cast<std::uint8_t>(i * 37 + 11);
        auto source = take(Value::create(
            descriptor, source_region,
            {static_cast<std::uint64_t>(8 - min), strides, {1, 1, 1}}, bytes));
        for (unsigned fixed = 0; fixed <= 3; ++fixed) {
          for (std::uint64_t limit : {1, 7, 64}) {
            std::uint64_t visited = 0;
            take(visit_value_runs(
                source, subregion, source_region, limit,
                fixed < 3 ? std::optional<std::uint32_t>(fixed) : std::nullopt,
                [&](const ValueReadRun& run) {
                  require(run.samples && run.samples <= limit &&
                              run.logical_element == visited,
                          "invalid run extent or logical order");
                  std::vector<std::uint64_t> first;
                  region_run_coordinate(subregion, run.logical_element, &first);
                  for (std::uint64_t i = 0; i < run.samples; ++i) {
                    std::vector<std::uint64_t> at;
                    region_run_coordinate(subregion, visited + i, &at);
                    const auto address = take(source.byte_address(at));
                    require(
                        std::memcmp(source.bytes().data() + address,
                                    run.data + static_cast<std::int64_t>(i) *
                                                   run.stride_bytes,
                                    2) == 0,
                        "run address differs from scalar oracle");
                    std::uint64_t target = 0;
                    for (unsigned a = 0; a < 3; ++a)
                      target = target * source_region.dimensions()[a].extent +
                               at[a] - source_region.dimensions()[a].offset;
                    require(target == run.destination_element + i,
                            "run crossed destination padding");
                    if (fixed < 3)
                      require(at[fixed] == first[fixed],
                              "run crossed fixed parameter axis");
                  }
                  visited += run.samples;
                  return Status::success();
                }));
            require(visited == take(subregion.element_count()),
                    "incomplete run coverage");
          }
        }
        std::vector<std::uint8_t> expected(120, 0xa5), actual = expected;
        for (std::uint64_t i = 0; i < take(subregion.element_count()); ++i) {
          std::vector<std::uint64_t> at;
          region_run_coordinate(subregion, i, &at);
          std::uint64_t target = 0;
          for (unsigned a = 0; a < 3; ++a)
            target = target * source_region.dimensions()[a].extent + at[a] -
                     source_region.dimensions()[a].offset;
          std::memcpy(expected.data() + target * 2,
                      source.bytes().data() + take(source.byte_address(at)), 2);
        }
        take(copy_value_region(source, subregion, source_region, actual.data(),
                               actual.size()));
        require(actual == expected, "copy changed bits outside subregion");
        ++scenarios;
      }
    }
  } while (std::next_permutation(order.begin(), order.end()));
  require(scenarios == 192, "layout matrix incomplete");
}
void contiguous_runs() {
  const ValueDescriptor d{ElementType::UInt8, {2, 3, 5}};
  auto source = take(Value::create(d, Region::whole(d.shape), {0, {15, 5, 1}},
                                   std::vector<std::uint8_t>(30, 9)));
  unsigned callbacks = 0;
  take(visit_value_runs(source, source.region(), source.region(), 64, {},
                        [&](const ValueReadRun& run) {
                          ++callbacks;
                          require(run.samples == 30 && run.stride_bytes == 1,
                                  "fully contiguous axes did not coalesce");
                          return Status::success();
                        }));
  require(callbacks == 1, "contiguous copy remained row-by-row");
  callbacks = 0;
  take(visit_value_runs(source, source.region(), source.region(), 64, 1,
                        [&](const ValueReadRun& run) {
                          ++callbacks;
                          require(run.samples == 5,
                                  "contiguous run crossed fixed axis");
                          return Status::success();
                        }));
  require(callbacks == 6, "fixed-axis partition lost rows");
}
void boundaries() {
  const ValueDescriptor d{ElementType::UInt8, {4097}};
  auto source = take(Value::create(d, Region::whole(d.shape), {0, {1}},
                                   std::vector<std::uint8_t>(4097, 3)));
  std::vector<std::uint8_t> output(4097, 0xa5);
  CancellationSource stop;
  stop.cancel();
  require(copy_value_region(source, source.region(), source.region(),
                            output.data(), output.size(), stop.token())
                  .code == ErrorCode::Cancelled,
          "pre-cancel did not stop copying");
  require(std::all_of(output.begin(), output.end(),
                      [](auto x) { return x == 0xa5; }),
          "pre-cancel wrote data");
  unsigned charges = 0;
  auto limited = copy_value_region(
      source, source.region(), source.region(), output.data(), output.size(),
      {}, [&](std::uint64_t n) {
        require(n <= 1024, "copy exceeded stop interval");
        return ++charges == 2
                   ? Status{ErrorCode::ResourceExhausted, "test limit"}
                   : Status::success();
      });
  require(limited.code == ErrorCode::ResourceExhausted && output[1023] == 3 &&
              output[1024] == 0xa5,
          "work failure wrote unadmitted chunk");
  require(!copy_value_region(source, source.region(), source.region(),
                             const_cast<std::uint8_t*>(source.bytes().data()),
                             source.bytes().size())
               .ok(),
          "overlapping copy accepted");
  require(!copy_value_region(source, source.region(), source.region(),
                             output.data(), output.size() - 1)
               .ok(),
          "wrong destination capacity accepted");
  unsigned callbacks = 0;
  auto invalid = visit_value_runs(source, Region({{4096, 2}}), source.region(),
                                  64, {}, [&](const auto&) {
                                    ++callbacks;
                                    return Status::success();
                                  });
  require(!invalid.ok() && callbacks == 0, "invalid region reached visitor");
}
void singleton_and_partial_destination() {
  const ValueDescriptor d{ElementType::UInt8, {2, 5, 3}};
  const Region region({{0, 2}, {2, 1}, {0, 3}});
  auto source = take(Value::create(d, region, {0, {3, -77, 1}, {0, 2, 0}},
                                   {1, 2, 3, 4, 5, 6}));
  const auto destination = Region::whole(d.shape);
  for (auto fixed :
       {std::optional<std::uint32_t>{}, std::optional<std::uint32_t>{1}}) {
    unsigned calls = 0;
    take(visit_value_runs(
        source, region, destination, 64, fixed, [&](const ValueReadRun& run) {
          require(run.samples == 3 && run.destination_element == calls * 15 + 6,
                  "singleton merged across destination padding");
          for (std::uint64_t i = 0; i < run.samples; ++i)
            require(run.data[i] == calls * 3 + i + 1,
                    "singleton addressing changed bits");
          ++calls;
          return Status::success();
        }));
    require(calls == 2, "singleton lost row");
  }
  std::vector<std::uint8_t> out(30, 0xa5), expected = out;
  for (unsigned row = 0; row < 2; ++row)
    for (unsigned x = 0; x < 3; ++x)
      expected[row * 15 + 6 + x] = static_cast<std::uint8_t>(row * 3 + x + 1);
  take(copy_value_region(source, region, destination, out.data(), out.size()));
  require(out == expected, "partial destination sentinel overwritten");
}
}  // namespace
int main() {
  generic_runs();
  contiguous_runs();
  boundaries();
  singleton_and_partial_destination();
}
