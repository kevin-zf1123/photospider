#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Tensor scattered(ElementType type, std::uint64_t add = 0) {
  const TensorDescriptor descriptor{type, {1000}};
  std::vector<TensorSpan> fragments;
  std::vector<Region> boxes;
  for (const auto at : {0U, 2U, 63U, 64U, 999U}) {
    Region box({{at, 1}});
    auto writer =
        TensorWriter::allocate(descriptor, box, BufferAllocator{}).take_value();
    const std::uint64_t bits = add + at + 1;
    std::memcpy(writer.data(), &bits, writer.size());
    fragments.push_back(std::move(writer).publish().take_value());
    boxes.push_back(box);
  }
  auto coverage = Footprint::from_regions({1000}, boxes).take_value();
  return Tensor::create(descriptor, coverage, fragments).take_value();
}
int exact_bytes() {
  for (const auto type : {ElementType::UInt8, ElementType::Int64,
                          ElementType::Float32, ElementType::Float64}) {
    auto input = scattered(type);
    auto plan = FragmentAtlasPlan::prepare(input, {64}).take_value();
    const auto width = TensorSpan::element_size(type);
    PS_CHECK(plan.occupied_tiles() == 3 &&
             plan.payload_allocation_bytes() == 5 * width &&
             plan.directory_allocation_bytes() == 8 * 80);
    auto made = plan.materialize(input, BufferAllocator{});
    PS_CHECK(made.ok());
    auto atlas = made.take_value();
    unsigned order = 0;
    for (const auto at : {0U, 2U, 63U, 64U, 999U}) {
      auto offset = atlas.address({at});
      PS_CHECK(offset.ok() && offset.value() == order++ * width);
      const std::uint64_t expected = at + 1;
      PS_CHECK(std::memcmp(atlas.payload.bytes().data() + offset.value(),
                           &expected, width) == 0);
    }
    for (const auto hole : {1U, 3U, 62U, 65U, 998U})
      PS_CHECK(atlas.address({hole}).status().code == ErrorCode::NotFound);
    PS_CHECK(atlas.address({1000}).status().code == ErrorCode::InvalidArgument);
    // Inspect wire slots independently of address(): keys 0,1,15 and exact
    // masks/offsets. All unused words remain zero, with no bbox-hole payload.
    unsigned occupied = 0;
    for (unsigned slot = 0; slot < 8; ++slot) {
      std::array<std::uint64_t, 10> words{};
      const auto* bytes = atlas.directory.bytes().data() + slot * 80;
      for (unsigned word = 0; word < 10; ++word)
        for (unsigned byte = 0; byte < 8; ++byte)
          words[word] |= static_cast<std::uint64_t>(bytes[word * 8 + byte])
                         << (8 * byte);
      for (unsigned i = 1; i < 8; ++i)
        PS_CHECK(words[i] == 0);
      if (!words[8]) {
        PS_CHECK(words[0] == 0 && words[9] == 0);
        continue;
      }
      ++occupied;
      if (words[0] == 0) {
        PS_CHECK(words[8] == (UINT64_C(1) | 4 | (UINT64_C(1) << 63)) &&
                 words[9] == 0);
      } else if (words[0] == 1) {
        PS_CHECK(words[8] == 1 && words[9] == 3 * width);
      } else {
        PS_CHECK(words[0] == 15 && words[8] == (UINT64_C(1) << 39) &&
                 words[9] == 4 * width);
      }
    }
    PS_CHECK(occupied == 3);
    auto changed = plan.materialize(scattered(type, 5), BufferAllocator{});
    PS_CHECK(changed.ok() && changed.value().payload.bytes().data()[0] == 6);
  }
  return 0;
}
int metadata_lifetime() {
  const auto input = scattered(ElementType::UInt8);
  TensorDescriptor rank8{ElementType::UInt8, {1, 1, 1, 1, 1, 1, 1, 2}};
  auto empty_coverage = Footprint::none(rank8.shape);
  PS_REQUIRE_OK(empty_coverage);
  auto empty = Tensor::create(rank8, empty_coverage.value(), {});
  PS_REQUIRE_OK(empty);
  for (unsigned mode = 0; mode < 3; ++mode) {
    ResourceBudget root;
    FragmentAtlasPlan retained;
    std::uint64_t bytes = 0;
    {
      ResourceAllocationScope scope(root);
      std::vector<std::uint64_t> geometry;
      if (mode == 1) {
        geometry.reserve(8);
        geometry.push_back(64);
      } else if (mode == 2) {
        // Empty capacity below rank must not trigger geometric growth beyond
        // the admitted retained array size when automatic geometry is filled.
        geometry.reserve(5);
      }
      auto made = FragmentAtlasPlan::prepare(mode == 2 ? empty.value() : input,
                                             std::move(geometry));
      PS_REQUIRE_OK(made);
      retained = made.take_value();
      bytes = root.statistics().allocated_metadata_bytes;
      PS_CHECK(bytes > ResourceBudget::lease_metadata_bytes());
      PS_CHECK(bytes == root.statistics().live[ResourceKind::Metadata]);
      auto alias = retained;
      PS_CHECK(root.statistics().allocated_metadata_bytes == bytes);
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == bytes);
    std::thread release([plan = std::move(retained)]() mutable { plan = {}; });
    release.join();
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int materialized_metadata_lifetime() {
  const auto input = scattered(ElementType::UInt8);
  auto plan = FragmentAtlasPlan::prepare(input, {64});
  PS_REQUIRE_OK(plan);
  ResourceBudget first, second;
  const auto make = [&](const ResourceBudget& root) {
    ResourceAllocationScope scope(root);
    return plan.value().materialize(input, root.allocator());
  };
  FragmentAtlas a, b;
  {
    auto one = make(first), two = make(second);
    PS_REQUIRE_OK(one);
    PS_REQUIRE_OK(two);
    a = one.take_value();
    b = two.take_value();
  }
  const auto retained = second.statistics().allocated_metadata_bytes;
  PS_CHECK(retained == second.statistics().live[ResourceKind::Metadata]);
  a = b;
  PS_CHECK(first.statistics().allocated_metadata_bytes == 0);
  PS_CHECK(second.statistics().allocated_metadata_bytes == retained);
  a.descriptor.shape[0] = 999;
  a.tile_shape[0] = 32;
  PS_CHECK(b.descriptor.shape[0] == 1000 && b.tile_shape[0] == 64);
  a = {};
  {
    auto one = make(first);
    PS_REQUIRE_OK(one);
    a = one.take_value();
  }
  a = std::move(b);
  PS_CHECK(first.statistics().allocated_metadata_bytes == 0);
  PS_CHECK(second.statistics().allocated_metadata_bytes == retained);
  {
    auto copy = a;
    a = {};
    const auto geometry =
        ResourceBudget::lease_metadata_bytes() + 2 * sizeof(std::uint64_t);
    PS_CHECK(second.statistics().allocated_metadata_bytes ==
             retained - geometry);
    PS_CHECK(copy.address({999}).value() == 4);
    PS_CHECK(copy.payload.bytes().data()[4] == static_cast<std::uint8_t>(1000));
    a = std::move(copy);
  }
  std::thread release([value = std::move(a)]() mutable { value = {}; });
  release.join();
  PS_CHECK(second.statistics().allocated_metadata_bytes == 0);
  for (auto live : second.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int strided_and_huge() {
  const double data[]{8, 7, 6, 5, 4, 3, 2, 1};
  std::vector<std::uint8_t> bytes(sizeof(data));
  std::memcpy(bytes.data(), data, sizeof(data));
  auto value = TensorSpan::create({ElementType::Float64, {8}},
                                  Region::whole({8}), {56, {-8}}, bytes)
                   .take_value();
  auto set = Footprint::from_regions({8}, {Region({{1, 2}}), Region({{6, 1}})})
                 .take_value();
  auto source = Tensor::create(value.descriptor(), set, {value}).take_value();
  auto atlas = FragmentAtlasPlan::prepare(source, {4})
                   .value()
                   .materialize(source, BufferAllocator{});
  PS_CHECK(atlas.ok());
  for (auto at : {1U, 2U, 6U}) {
    double sample = 0;
    const auto offset = atlas.value().address({at}).take_value();
    std::memcpy(&sample, atlas.value().payload.bytes().data() + offset, 8);
    PS_CHECK(sample == at + 1);
  }
  const std::vector<std::uint64_t> shape(8, UINT64_C(1) << 40);
  std::vector<RegionDimension> dimensions(8, {shape[0] - 1, 1});
  Region corner(dimensions);
  auto writer = TensorWriter::allocate({ElementType::UInt8, shape}, corner,
                                       BufferAllocator{})
                    .take_value();
  writer.data()[0] = 173;
  auto fragment = std::move(writer).publish().take_value();
  auto coverage = Footprint::from_regions(shape, {corner}).take_value();
  auto huge =
      Tensor::create(fragment.descriptor(), coverage, {fragment}).take_value();
  auto packed = FragmentAtlasPlan::prepare(huge).value().materialize(
      huge, BufferAllocator{});
  PS_CHECK(packed.ok() && packed.value().payload_bytes == 1);
  std::vector<std::uint64_t> coordinate(8, shape[0] - 1);
  PS_CHECK(packed.value().address(coordinate).value() == 0 &&
           packed.value().payload.bytes().data()[0] == 173);
  --coordinate[0];
  PS_CHECK(packed.value().address(coordinate).status().code ==
           ErrorCode::NotFound);
  return 0;
}
int limits_and_owners() {
  auto input = scattered(ElementType::Float64);
  FootprintLimits tiny;
  tiny.maximum_boxes = 2;
  PS_CHECK(FragmentAtlasPlan::prepare(input, {64}, tiny).status().code ==
           ErrorCode::ResourceExhausted);
  tiny.maximum_boxes = 65536;
  tiny.maximum_work = 1;
  PS_CHECK(FragmentAtlasPlan::prepare(input, {64}, tiny).status().code ==
           ErrorCode::ResourceExhausted);
  auto plan = FragmentAtlasPlan::prepare(input, {64}).take_value();
  FootprintLimits exact_work;
  exact_work.maximum_work = plan.materialization_work();
  PS_CHECK(plan.materialize(input, BufferAllocator{}, exact_work).ok());
  --exact_work.maximum_work;
  PS_CHECK(
      plan.materialize(input, BufferAllocator{}, exact_work).status().code ==
      ErrorCode::ResourceExhausted);
  exact_work.maximum_work = plan.preparation_work();
  PS_CHECK(FragmentAtlasPlan::prepare(input, {64}, exact_work).ok());
  --exact_work.maximum_work;
  PS_CHECK(FragmentAtlasPlan::prepare(input, {64}, exact_work).status().code ==
           ErrorCode::ResourceExhausted);
  const auto total =
      plan.payload_allocation_bytes() + plan.directory_allocation_bytes();
  auto limited = BufferAllocator{}.limited(total - 1);
  PS_CHECK(plan.materialize(input, limited).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(limited.allocate(total - 1).ok());
  PS_CHECK(plan.materialize(input, BufferAllocator{}.limited(total)).ok());
  CancellationSource cancel;
  cancel.cancel();
  tiny = {};
  tiny.cancellation = cancel.token();
  PS_CHECK(plan.materialize(input, BufferAllocator{}, tiny).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(FragmentAtlasPlan::prepare(input, {65}).status().code ==
           ErrorCode::InvalidArgument);
  std::weak_ptr<const CpuStorage> owner;
  FragmentAtlasPlan independent;
  {
    auto source = scattered(ElementType::UInt8);
    owner = source.fragments()[0].storage();
    independent = FragmentAtlasPlan::prepare(source).take_value();
  }
  PS_CHECK(owner.expired() && independent.valid());
  auto empty = Tensor::create({ElementType::UInt8, {8}},
                              Footprint::none({8}).take_value(), {})
                   .take_value();
  auto none = FragmentAtlasPlan::prepare(empty).value().materialize(
      empty, BufferAllocator{});
  PS_CHECK(none.ok() && none.value().payload_bytes == 0 &&
           none.value().address({0}).status().code == ErrorCode::NotFound);
  auto empty_plan = FragmentAtlasPlan::prepare(empty).take_value();
  for (unsigned at = 1; at <= 2; ++at) {
    CancellationSource interrupted;
    unsigned allocations = 0, active = 0;
    BufferAllocator allocator([&](std::uint64_t) {
      ++allocations;
      ++active;
      if (allocations == at)
        interrupted.cancel();
      std::shared_ptr<void> lease(new unsigned(1), [&](void* pointer) {
        delete static_cast<unsigned*>(pointer);
        --active;
      });
      return Result<std::shared_ptr<void>>(std::move(lease));
    });
    FootprintLimits stopped;
    stopped.cancellation = interrupted.token();
    PS_CHECK(empty_plan.materialize(empty, allocator, stopped).status().code ==
             ErrorCode::Cancelled);
    PS_CHECK(allocations == at && active == 0);
  }
  FootprintLimits zero;
  zero.maximum_work = 0;
  PS_CHECK(FragmentAtlasPlan::prepare(input, {64}, zero).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(plan.materialize(input, BufferAllocator{}, zero).status().code ==
           ErrorCode::ResourceExhausted);
  zero.cancellation = cancel.token();
  PS_CHECK(FragmentAtlasPlan::prepare(input, {64}, zero).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(plan.materialize(input, BufferAllocator{}, zero).status().code ==
           ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(metadata_lifetime() == 0);
  PS_CHECK(materialized_metadata_lifetime() == 0);
  PS_CHECK(exact_bytes() == 0);
  PS_CHECK(strided_and_huge() == 0);
  PS_CHECK(limits_and_owners() == 0);
  return 0;
}
