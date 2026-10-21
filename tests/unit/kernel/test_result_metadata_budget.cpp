#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include "data/affine_view.hpp"
#include "data/result_window_access.hpp"
#include "execution/task_schedule.hpp"
#include "execution/tile_view.hpp"
#include "photospider/photospider.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/physical_program_fixture.hpp"
#include "support/test_support.hpp"

namespace {
#if defined(PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
constexpr bool interpose_allocations = false;
#else
constexpr bool interpose_allocations = true;
#endif
std::atomic<bool> inspect_allocations{false};
std::atomic<bool> fail_next_allocation{false};
std::atomic<int> fail_after_allocations{-1};
std::atomic<unsigned> large_allocations{0};
std::atomic<bool> arm_failure_path{false}, inspect_failure_path{false};
std::atomic<unsigned> failure_path_allocations{0};
#if !defined(PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
void record(std::size_t size) {
  if (inspect_failure_path.load())
    ++failure_path_allocations;
  if (inspect_allocations.load() && size > 16384)
    ++large_allocations;
}
#endif
}  // namespace
// The target statically links the kernel, so this observes its actual C++
// allocation calls, including any accidental ordinary STL metadata copy.
// TSAN owns these symbols; that build retains budget and lifetime assertions
// but leaves allocator interception and fault injection to ordinary builds.
#if !defined(PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
void* operator new(std::size_t size) {
  if (fail_next_allocation.exchange(false))
    throw std::bad_alloc();
  if (fail_after_allocations.load() >= 0 &&
      fail_after_allocations.fetch_sub(1) == 0)
    throw std::bad_alloc();
  record(size);
  if (void* p = std::malloc(size ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void operator delete[](void* p) noexcept {
  ::operator delete(p);
}
void operator delete(void* p, std::size_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  ::operator delete(p);
}
void* operator new(std::size_t size, std::align_val_t value) {
  record(size);
  void* p = nullptr;
  const auto alignment = static_cast<std::size_t>(value);
#if defined(_WIN32)
  p = _aligned_malloc(size ? size : 1, alignment);
#else
  if (posix_memalign(&p, alignment, size ? size : 1) != 0)
    p = nullptr;
#endif
  if (!p)
    throw std::bad_alloc();
  return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete[](void* p, std::align_val_t alignment) noexcept {
  ::operator delete(p, alignment);
}
void operator delete(void* p, std::size_t,
                     std::align_val_t alignment) noexcept {
  ::operator delete(p, alignment);
}
void operator delete[](void* p, std::size_t,
                       std::align_val_t alignment) noexcept {
  ::operator delete(p, alignment);
}
#endif

namespace {
int schema_allocation_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto source = test::fixed_fixture::schema();
  source.tensors[0].descriptor.shape = {2, 3};
  source.tensors[0].batch_axes = {2};
  bool completed = false;
  for (int fail_after = 0; fail_after < 64 && !completed; ++fail_after) {
    ResourceBudget root;
    try {
      fail_after_allocations = interpose_allocations ? fail_after : -1;
      auto copied = source.managed_copy(root);
      fail_after_allocations = -1;
      if (copied.ok()) {
        completed = true;
        PS_CHECK(copied.value().same_schema(source));
        PS_CHECK(root.statistics().allocated_metadata_bytes ==
                 root.statistics().live[ResourceKind::Metadata]);
        const auto retained = root.statistics().allocated_metadata_bytes;
        {
          auto ordinary = copied.value().tensors[0];
          PS_CHECK(!ordinary.metadata_owner.valid());
          ordinary.descriptor.shape[0] = 5;
          PS_CHECK(copied.value().tensors[0].descriptor.shape[0] == 2);
        }
        PS_CHECK(root.statistics().allocated_metadata_bytes == retained);
      } else {
        PS_CHECK(copied.status().code == ErrorCode::ResourceExhausted);
      }
    } catch (const std::bad_alloc&) {
      fail_after_allocations = -1;
      PS_CHECK(!completed);
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(completed);
  ResourceBudget first, second;
  ResultTensorSpec a, b;
  {
    auto one = source.managed_copy(first);
    auto two = source.managed_copy(second);
    PS_REQUIRE_OK(one);
    PS_REQUIRE_OK(two);
    a = std::move(one.value().tensors[0]);
    b = std::move(two.value().tensors[0]);
  }
  const auto retained = second.statistics().allocated_metadata_bytes;
  a = b;
  PS_CHECK(first.statistics().allocated_metadata_bytes == 0);
  PS_CHECK(!a.metadata_owner.valid() &&
           a.descriptor.shape == b.descriptor.shape);
  a = {};
  PS_CHECK(second.statistics().allocated_metadata_bytes == retained);
  {
    auto rebuilt = source.managed_copy(first);
    PS_REQUIRE_OK(rebuilt);
    a = std::move(rebuilt.value().tensors[0]);
  }
  a = std::move(b);
  PS_CHECK(first.statistics().allocated_metadata_bytes == 0);
  PS_CHECK(second.statistics().allocated_metadata_bytes == retained);
  a = {};
  PS_CHECK(second.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int window_allocation_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  ResultRef source;
  {
    auto started =
        ResultBuilder::start(root, test::fixed_fixture::schema(), "window");
    PS_REQUIRE_OK(started);
    auto builder = started.take_value();
    auto allocated = root.allocator().allocate(4);
    PS_REQUIRE_OK(allocated);
    auto bytes = allocated.take_value();
    for (unsigned i = 0; i < 4; ++i)
      bytes.data()[i] = i + 1;
    PS_REQUIRE_OK(builder.publish_tensor(0, Region::whole({4}), {0, {1}, {0}},
                                         std::move(bytes).freeze()));
    auto sealed = builder.seal();
    PS_REQUIRE_OK(sealed);
    auto captured = sealed.value().capture();
    PS_REQUIRE_OK(captured);
    source = captured.take_value();
  }
  auto facts = source.descriptor();
  PS_REQUIRE_OK(facts);
  const auto baseline = root.statistics();
  const auto excess = [&] {
    const auto now = root.statistics();
    return (now.live[ResourceKind::Metadata] -
            baseline.live[ResourceKind::Metadata]) -
           (now.allocated_metadata_bytes - baseline.allocated_metadata_bytes);
  };
  {
    auto first = source.acquire_tensor(facts.value(), 0, Region({{0, 2}}));
    PS_REQUIRE_OK(first);
    PS_CHECK(excess() == sizeof(ResultTensorReadWindow));
    auto left = first.take_value();
    const auto one = root.statistics().allocated_metadata_bytes;
    auto second = source.acquire_tensor(facts.value(), 0, Region({{2, 2}}));
    PS_REQUIRE_OK(second);
    left = second.take_value();
    PS_CHECK(root.statistics().allocated_metadata_bytes == one);
    PS_CHECK(left.region().dimensions()[0].offset == 2);
  }
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           baseline.allocated_metadata_bytes);
  ResultTensorReadWindow retained;
  {
    ResourceVector<ResultTensorReadWindow> windows;
    for (std::uint64_t offset : {0U, 2U}) {
      auto part =
          source.acquire_tensor(facts.value(), 0, Region({{offset, 2}}));
      PS_REQUIRE_OK(part);
      windows.push_back(part.take_value());
    }
    auto combined = execution_internal::ResultWindowAccess::compose(
        root, Region::whole({4}), std::move(windows), {});
    PS_REQUIRE_OK(combined);
    retained = combined.take_value();
    // Original leases retain their admission, but their old Region arrays
    // have been freed and must no longer contribute actual metadata.
    PS_CHECK(excess() ==
             3 * sizeof(ResultTensorReadWindow) + 2 * sizeof(RegionDimension));
    ResourceVector<ResultTensorReadWindow> nested;
    nested.push_back(std::move(retained));
    auto recombined = execution_internal::ResultWindowAccess::compose(
        root, Region::whole({4}), std::move(nested), {});
    PS_REQUIRE_OK(recombined);
    retained = recombined.take_value();
    PS_CHECK(excess() ==
             4 * sizeof(ResultTensorReadWindow) + 3 * sizeof(RegionDimension));
    std::array<std::uint8_t, 4> bytes{};
    PS_REQUIRE_OK(retained.copy_to(bytes.data(), bytes.size()));
    PS_CHECK((bytes == std::array<std::uint8_t, 4>{1, 2, 3, 4}));
    const auto before = root.statistics().allocated_metadata_bytes;
    {
      auto affine =
          execution_internal::ResultWindowAccess::input_view(retained, {});
      PS_REQUIRE_OK(affine);
      PS_CHECK(affine.value().has_value());
      PS_CHECK(affine.value()->byte_address({3}).value() == 3);
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == before);
  }
  std::thread release(
      [window = std::move(retained)]() mutable { window = {}; });
  release.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           baseline.allocated_metadata_bytes);
  facts = Result<ResultDescriptor>(Status{ErrorCode::Stale, {}});
  source = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int footprint_allocation_failure() {
  using namespace ps;  // NOLINT(build/namespaces)
  const std::vector<Region> boxes{Region({{0, 2}}), Region({{6, 2}})};
  auto left = Footprint::from_regions({16}, boxes);
  auto right =
      Footprint::from_regions({16}, {Region({{1, 2}}), Region({{9, 2}})});
  PS_REQUIRE_OK(left);
  PS_REQUIRE_OK(right);
  for (unsigned mode = 0; mode < 7; ++mode) {
    bool completed = false, failed_after_commit = false;
    for (int fail_after = 0; fail_after < 256 && !completed; ++fail_after) {
      ResourceBudget root;
      {
        ResourceAllocationScope scope(root);
        try {
          fail_after_allocations = interpose_allocations ? fail_after : -1;
          auto made = mode == 0   ? Footprint::from_regions({16}, boxes)
                      : mode == 1 ? left.value().unite(right.value())
                      : mode == 2 ? left.value().intersect(right.value())
                      : mode == 3 ? left.value().subtract(right.value())
                      : mode == 4 ? left.value().tile_cover({4})
                      : mode == 5 ? Footprint::all(left.value().shape())
                                  : Footprint::from_region(left.value().shape(),
                                                           boxes.front());
          fail_after_allocations = -1;
          if (made.ok()) {
            completed = true;
            const auto statistics = root.statistics();
            PS_CHECK(statistics.allocated_metadata_bytes > 0 &&
                     statistics.allocated_metadata_bytes ==
                         statistics.live[ResourceKind::Metadata]);
          } else {
            PS_CHECK(made.status().code == ErrorCode::ResourceExhausted);
          }
        } catch (const std::bad_alloc&) {
          fail_after_allocations = -1;
          PS_CHECK(!completed);
        }
      }
      PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
      failed_after_commit |=
          !completed && root.statistics().peak_allocated_metadata_bytes > 0;
      for (auto live : root.statistics().live.values)
        PS_CHECK(live == 0);
    }
    PS_CHECK(completed);
    if constexpr (interpose_allocations)
      PS_CHECK(failed_after_commit);
  }
  return 0;
}
int atlas_allocation_failure() {
  using namespace ps;  // NOLINT(build/namespaces)
  const TensorDescriptor descriptor{ElementType::UInt8, {2, 3}};
  const auto region = Region::whole(descriptor.shape);
  auto writer = TensorWriter::allocate(descriptor, region, BufferAllocator{});
  PS_REQUIRE_OK(writer);
  auto writable = writer.take_value();
  for (unsigned i = 0; i < 6; ++i)
    writable.data()[i] = static_cast<std::uint8_t>(i + 1);
  auto span = std::move(writable).publish();
  PS_REQUIRE_OK(span);
  auto coverage = Footprint::from_regions(descriptor.shape, {region});
  PS_REQUIRE_OK(coverage);
  auto input = Tensor::create(descriptor, coverage.value(), {span.value()});
  PS_REQUIRE_OK(input);
  auto plan = FragmentAtlasPlan::prepare(input.value());
  PS_REQUIRE_OK(plan);
  bool completed = false, failed_after_commit = false;
  for (int fail_after = 0; fail_after < 128 && !completed; ++fail_after) {
    ResourceBudget root;
    {
      ResourceAllocationScope scope(root);
      const auto allocator = root.allocator();
      fail_after_allocations = interpose_allocations ? fail_after : -1;
      auto made = plan.value().materialize(input.value(), allocator);
      fail_after_allocations = -1;
      if (made.ok()) {
        completed = true;
        PS_CHECK(root.statistics().allocated_metadata_bytes ==
                 root.statistics().live[ResourceKind::Metadata]);
        PS_CHECK(made.value().payload.bytes().data()[5] == 6);
      } else {
        PS_CHECK(made.status().code == ErrorCode::ResourceExhausted);
      }
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    failed_after_commit |=
        !completed && root.statistics().peak_allocated_metadata_bytes > 0;
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(completed);
  if constexpr (interpose_allocations)
    PS_CHECK(failed_after_commit);
  return 0;
}
int tile_view_allocation_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (const auto rank : {1U, 8U}) {
    for (const auto include_layout : {false, true}) {
      physical::Layout source;
      source.rank = rank;
      source.byte_offset = 7;
      for (unsigned axis = 0; axis < rank; ++axis) {
        source.origin[axis] = axis + 3;
        source.shape[axis] = 2;
        source.strides[axis] = axis % 2 ? 0 : -8;
      }
      ResourceBudget root;
      auto admitted = root.reserve(
          ResourceCapacity::host(execution_internal::TileView::metadata_bytes,
                                 execution_internal::TileView::metadata_bytes));
      PS_REQUIRE_OK(admitted);
      auto lease = admitted.take_value();
      const auto baseline = root.statistics().allocated_metadata_bytes;
      bool completed = false;
      for (int fail_after = 0; fail_after < 8 && !completed; ++fail_after) {
        try {
          fail_after_allocations = interpose_allocations ? fail_after : -1;
          execution_internal::TileView view(lease, source, include_layout, 5);
          fail_after_allocations = -1;
          completed = true;
          const auto arrays =
              rank *
              (sizeof(RegionDimension) +
               (include_layout ? sizeof(std::int64_t) + sizeof(std::uint64_t)
                               : 0));
          PS_CHECK(root.statistics().allocated_metadata_bytes ==
                   baseline + arrays);
          PS_CHECK(view.region.rank() == rank);
          for (unsigned axis = 0; axis < rank; ++axis) {
            PS_CHECK(view.region.dimensions()[axis].offset ==
                     source.origin[axis]);
            PS_CHECK(view.region.dimensions()[axis].extent ==
                     source.shape[axis]);
            if (include_layout) {
              PS_CHECK(view.layout.byte_strides[axis] == source.strides[axis]);
              PS_CHECK(view.layout.origin[axis] == source.origin[axis]);
            }
          }
          PS_CHECK(!include_layout || view.layout.byte_offset == 12);
          auto moved = std::move(view);
          PS_CHECK(moved.region.rank() == rank);
          PS_CHECK(root.statistics().allocated_metadata_bytes ==
                   baseline + arrays);
        } catch (const std::bad_alloc&) {
          fail_after_allocations = -1;
          PS_CHECK(!completed);
        }
        PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
      }
      PS_CHECK(completed);
      lease = {};
      PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
      for (auto live : root.statistics().live.values)
        PS_CHECK(live == 0);
    }
  }
  return 0;
}
int publish_layout_allocation_failure() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto schema = test::fixed_fixture::schema();
  schema.tensors[0].descriptor = {ElementType::UInt8, {1, 1, 1, 1, 1, 1, 1, 2}};
  const auto region = Region::whole(schema.tensors[0].descriptor.shape);
  StridedLayout layout{0, {0, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 0, 0}};
  auto allocated = BufferAllocator{}.allocate(2);
  PS_REQUIRE_OK(allocated);
  auto buffer = allocated.take_value();
  buffer.data()[0] = 5;
  buffer.data()[1] = 9;
  auto storage = std::move(buffer).freeze();
  bool completed = false;
  for (int fail_after = 0; fail_after < 4096 && !completed; ++fail_after) {
    ResourceBudget root;
    {
      auto made = ResultBuilder::start(root, schema, "layout.failure");
      PS_REQUIRE_OK(made);
      auto builder = made.take_value();
      fail_after_allocations = interpose_allocations ? fail_after : -1;
      // An lvalue layout must be borrowed: an implicit parameter copy would
      // throw outside the publication function's typed-failure boundary.
      auto status = builder.publish_tensor(0, region, layout, storage);
      fail_after_allocations = -1;
      completed = status.ok();
      PS_CHECK(completed || status.code == ErrorCode::ResourceExhausted);
      PS_CHECK(layout.byte_strides[7] == 1 && layout.origin.size() == 8);
      if (completed) {
        auto result = builder.seal();
        PS_REQUIRE_OK(result);
      }
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(completed);
  return 0;
}
int publication_metadata_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto schema = test::fixed_fixture::schema();
  schema.tensors[0].batch_axes = {1};
  schema.tensors[0].descriptor = {ElementType::UInt8, {1, 1, 1, 1, 1, 4, 4}};
  const auto region = Region::whole(schema.tensors[0].sample_shape());
  bool completed = false;
  for (int fail_after = 0; fail_after < 4096 && !completed; ++fail_after) {
    ResourceBudget root;
    ResultRef exported;
    {
      auto made = ResultBuilder::start(root, schema, "publication", {}, 2);
      PS_REQUIRE_OK(made);
      auto builder = made.take_value();
      bool observed = false;
      fail_after_allocations = interpose_allocations ? fail_after : -1;
      auto status =
          builder.publish_tensor_kernel(0, region, [&](const auto& windows) {
            const auto statistics = root.statistics();
            observed = windows.size() == 4 &&
                       statistics.allocated_metadata_bytes > 0 &&
                       statistics.allocated_metadata_bytes <=
                           statistics.live[ResourceKind::Metadata];
            for (const auto& window : windows) {
              std::vector<std::uint64_t> at;
              for (const auto axis : window.region().dimensions())
                at.push_back(axis.offset);
              for (unsigned row = 0; row < 2; ++row, ++at[6]) {
                auto run = window.row_run(at);
                if (!run.ok())
                  return run.status();
                std::fill_n(run.value().data, run.value().samples, 7);
              }
            }
            return Status::success();
          });
      fail_after_allocations = -1;
      completed = status.ok();
      PS_CHECK(completed || status.code == ErrorCode::ResourceExhausted);
      if (completed) {
        PS_CHECK(observed);
        auto sealed = builder.seal();
        PS_REQUIRE_OK(sealed);
        exported = sealed.take_value();
        auto facts = exported.descriptor();
        PS_REQUIRE_OK(facts);
        auto window = exported.acquire_tensor(facts.value(), 0, region);
        PS_REQUIRE_OK(window);
        const auto before_copy = root.statistics().allocated_metadata_bytes;
        std::array<std::uint8_t, 16> bytes{};
        PS_REQUIRE_OK(window.value().copy_to(bytes.data(), bytes.size()));
        PS_CHECK(root.statistics().allocated_metadata_bytes == before_copy);
        PS_CHECK(std::all_of(bytes.begin(), bytes.end(),
                             [](auto value) { return value == 7; }));
      }
    }
    if (exported.valid()) {
      PS_CHECK(root.statistics().allocated_metadata_bytes > 0);
      std::thread release(
          [result = std::move(exported)]() mutable { result = {}; });
      release.join();
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(completed);
  return 0;
}
int affine_join_metadata() {
  using namespace ps;  // NOLINT(build/namespaces)
  const TensorDescriptor descriptor{ElementType::UInt8, {2, 3}};
  const auto domain = Region::whole(descriptor.shape);
  auto allocated = BufferAllocator{}.allocate(6);
  PS_REQUIRE_OK(allocated);
  auto storage = std::move(allocated.take_value()).freeze();
  const StridedLayout layout{0, {3, 1}, {0, 0}};
  auto first = TensorSpan::from_storage(descriptor, Region({{0, 1}, {0, 3}}),
                                        layout, storage);
  auto second = TensorSpan::from_storage(descriptor, Region({{1, 1}, {0, 3}}),
                                         layout, storage);
  PS_REQUIRE_OK(first);
  PS_REQUIRE_OK(second);
  const std::array<TensorSpan, 2> parts{first.take_value(),
                                        second.take_value()};
  bool completed = false;
  for (int fail_after = 0; fail_after < 128 && !completed; ++fail_after) {
    ResourceBudget root;
    {
      ResourceAllocationScope scope(root);
      auto scratch = root.reserve(ResourceCapacity::host(1024, 1024));
      PS_REQUIRE_OK(scratch);
      const auto baseline = root.statistics().allocated_metadata_bytes;
      std::uint64_t observed = 0;
      FootprintLimits limits;
      limits.consume_work = [&](auto) {
        observed = std::max(
            observed, root.statistics().allocated_metadata_bytes - baseline);
        return Status::success();
      };
      try {
        fail_after_allocations = interpose_allocations ? fail_after : -1;
        auto joined = input_internal::join_affine_view(
            descriptor, domain, parts, limits, scratch.value());
        fail_after_allocations = -1;
        if (joined.ok()) {
          completed = true;
          PS_CHECK(joined.value().has_value());
          PS_CHECK(observed == 3 * 2 * sizeof(std::uint64_t));
          PS_CHECK(joined.value()->byte_address({1, 2}).value() == 5);
        } else {
          PS_CHECK(joined.status().code == ErrorCode::ResourceExhausted);
        }
      } catch (const std::bad_alloc&) {
        fail_after_allocations = -1;
        PS_CHECK(!completed);
      }
      PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(completed);
  return 0;
}
int batched_window_join_metadata() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  {
    auto schema = test::fixed_fixture::schema();
    auto& spec = schema.tensors[0];
    spec.batch_axes = {1, 1, 1, 1, 1, 1};
    spec.descriptor = {ElementType::UInt8, {1, 2}};
    const auto shape = spec.sample_shape();
    const auto whole = Region::whole(shape);
    const StridedLayout layout{0,
                               {0, 0, 0, 0, 0, 0, 0, 1},
                               {0, 0, 0, 0, 0, 0, 0, 0}};
    auto allocated = root.allocator().allocate(2);
    PS_REQUIRE_OK(allocated);
    auto buffer = allocated.take_value();
    buffer.data()[0] = 3;
    buffer.data()[1] = 7;
    auto storage = std::move(buffer).freeze();
    auto made = ResultBuilder::start(root, schema, "batched.join");
    PS_REQUIRE_OK(made);
    auto builder = made.take_value();
    for (unsigned cell = 0; cell < 2; ++cell) {
      auto dims = whole.dimensions();
      dims.back() = {cell, 1};
      PS_REQUIRE_OK(
          builder.publish_tensor(0, Region(std::move(dims)), layout, storage));
    }
    auto source = builder.seal();
    PS_REQUIRE_OK(source);
    auto facts = source.value().descriptor();
    PS_REQUIRE_OK(facts);
    auto acquired = source.value().acquire_tensor(facts.value(), 0, whole);
    PS_REQUIRE_OK(acquired);
    auto window = acquired.take_value();
    const auto baseline = root.statistics().allocated_metadata_bytes;
    bool completed = false;
    for (int fail_after = 0; fail_after < 128 && !completed; ++fail_after) {
      std::uint64_t minimum = UINT64_MAX, maximum = 0;
      FootprintLimits limits;
      limits.consume_work = [&](auto) {
        const auto bytes =
            root.statistics().allocated_metadata_bytes - baseline;
        minimum = std::min(minimum, bytes);
        maximum = std::max(maximum, bytes);
        return Status::success();
      };
      try {
        fail_after_allocations = interpose_allocations ? fail_after : -1;
        auto joined =
            execution_internal::ResultWindowAccess::input_view(window, limits);
        fail_after_allocations = -1;
        if (joined.ok()) {
          completed = true;
          PS_CHECK(joined.value().has_value());
          PS_CHECK(minimum == ResourceBudget::lease_metadata_bytes() +
                                  8 * sizeof(std::uint64_t));
          PS_CHECK(maximum == ResourceBudget::lease_metadata_bytes() +
                                  4 * 8 * sizeof(std::uint64_t));
          PS_CHECK(joined.value()->descriptor().shape == shape);
          PS_CHECK(
              joined.value()->byte_address({0, 0, 0, 0, 0, 0, 0, 1}).value() ==
              1);
        } else {
          PS_CHECK(joined.status().code == ErrorCode::ResourceExhausted);
        }
      } catch (const std::bad_alloc&) {
        fail_after_allocations = -1;
        PS_CHECK(!completed);
      }
      PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
    }
    PS_CHECK(completed);
  }
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int capture_allocation_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  ResultRef source;
  {
    auto started =
        ResultBuilder::start(root, test::fixed_fixture::schema(), "capture");
    PS_REQUIRE_OK(started);
    auto builder = started.take_value();
    auto sealed = builder.seal();
    PS_REQUIRE_OK(sealed);
    source = sealed.take_value();
  }
  const auto baseline = root.statistics();
  bool completed = false;
  for (int fail_after = 0; fail_after < 32 && !completed; ++fail_after) {
    fail_after_allocations = interpose_allocations ? fail_after : -1;
    auto made = source.capture();
    fail_after_allocations = -1;
    if (made.ok()) {
      completed = true;
      auto captured = made.take_value();
      const auto retained = root.statistics();
      const auto bytes =
          retained.allocated_metadata_bytes - baseline.allocated_metadata_bytes;
      PS_CHECK(bytes > ResourceBudget::lease_metadata_bytes() &&
               bytes == retained.live[ResourceKind::Metadata] -
                            baseline.live[ResourceKind::Metadata]);
      auto weak = captured.weak();
      auto repeated = captured.capture();
      PS_REQUIRE_OK(repeated);
      auto alias = repeated.take_value();
      captured = {};
      PS_CHECK(root.statistics().allocated_metadata_bytes ==
               retained.allocated_metadata_bytes);
      std::thread release([value = std::move(alias)]() mutable { value = {}; });
      release.join();
      PS_CHECK(!weak.lock().valid());
    } else {
      PS_CHECK(made.status().code == ErrorCode::ResourceExhausted);
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes ==
             baseline.allocated_metadata_bytes);
    PS_CHECK(root.statistics().live.values == baseline.live.values);
  }
  PS_CHECK(completed);
  source = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int span_allocation_failure() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto allocated = BufferAllocator{}.allocate(4);
  PS_REQUIRE_OK(allocated);
  auto storage = std::move(allocated.take_value()).freeze();
  TensorDescriptor descriptor{ElementType::UInt8, {2, 2}};
  const auto region = Region::whole(descriptor.shape);
  StridedLayout layout{0, {2, 1}, {0, 0}};
  bool completed = false;
  bool failed_after_commit = false;
  for (int fail_after = 0; fail_after < 32 && !completed; ++fail_after) {
    ResourceBudget root;
    {
      ResourceAllocationScope scope(root);
      try {
        fail_after_allocations = interpose_allocations ? fail_after : -1;
        auto made =
            TensorSpan::from_storage(descriptor, region, layout, storage);
        fail_after_allocations = -1;
        if (made.ok()) {
          completed = true;
          const auto statistics = root.statistics();
          PS_CHECK(statistics.allocated_metadata_bytes > 0 &&
                   statistics.allocated_metadata_bytes ==
                       statistics.live[ResourceKind::Metadata]);
        } else {
          PS_CHECK(made.status().code == ErrorCode::ResourceExhausted);
        }
      } catch (const std::bad_alloc&) {
        fail_after_allocations = -1;
        PS_CHECK(!completed);
      }
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    failed_after_commit |=
        !completed && root.statistics().peak_allocated_metadata_bytes > 0;
    for (auto live : root.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(completed);
  if constexpr (interpose_allocations)
    PS_CHECK(failed_after_commit);
  return 0;
}
int scheduler_allocation_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto program = test::physical_fixture::diamond();
  PS_REQUIRE_OK(physical::finalize_program(&program));
  bool completed = false;
  for (int fail_after = 0; fail_after < 64 && !completed; ++fail_after) {
    ResourceBudget root;
    const auto bytes =
        execution_internal::TaskSchedule::metadata_bytes(program, 0);
    auto reserved = root.reserve(ResourceCapacity::host(bytes, bytes));
    PS_REQUIRE_OK(reserved);
    auto lease = reserved.take_value();
    const auto baseline = root.statistics().allocated_metadata_bytes;
    const auto capacity = root.statistics().live.values;
    try {
      fail_after_allocations = interpose_allocations ? fail_after : -1;
      execution_internal::TaskSchedule schedule(program, 0, 1, 1, 0, 0, lease);
      fail_after_allocations = -1;
      completed = true;
      PS_CHECK(root.statistics().allocated_metadata_bytes > baseline);
      PS_CHECK(root.statistics().allocated_metadata_bytes <=
               root.statistics().live[ResourceKind::Metadata]);
      // Moving through ready/submitted/completed/retired states does not
      // allocate or change the scheduler's retained array capacity.
      const auto retained = root.statistics().allocated_metadata_bytes;
      Status copied{ErrorCode::Cancelled, {}, FailureReason::MalformedEnvelope};
      copied.detail.node_id = 19;
      copied.message.reserve(256);
      fail_next_allocation = interpose_allocations;
      schedule.copy_settled_status(&copied);
      const bool unsettled = copied.code == ErrorCode::Internal;
      while (auto submitted = schedule.issue_next(physical::Backend::Cpu)) {
        schedule.complete(submitted->key, {});
        while (schedule.retire_next()) {
        }
      }
      schedule.copy_settled_status(&copied);
      const auto untouched = fail_next_allocation.exchange(false);
      PS_CHECK(untouched == interpose_allocations && schedule.drained());
      PS_CHECK(unsettled && copied.ok() && copied.message.empty() &&
               copied.reason == FailureReason::None &&
               copied.detail.node_id == 0);
      PS_CHECK(root.statistics().allocated_metadata_bytes == retained);
    } catch (const std::bad_alloc&) {
      fail_after_allocations = -1;
      fail_next_allocation = false;
      PS_CHECK(!completed);
    }
    PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
    PS_CHECK(root.statistics().live.values == capacity);
    if (fail_after > 0 || !interpose_allocations)
      PS_CHECK(root.statistics().peak_allocated_metadata_bytes > baseline);
    lease = {};
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  }
  PS_CHECK(completed);
  return 0;
}
int failure_copy_exhaustion() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.failure";
  ResultTensorSpec tensor;
  tensor.key = "rows";
  tensor.descriptor = {ElementType::Int64, {1}};
  schema.tensors.push_back(std::move(tensor));
  auto builder =
      ResultBuilder::start(root, schema, "copy-failure").take_value();
  auto reference = builder.reference();
  Status failure{ErrorCode::InvalidArgument,
                 std::string(128, 'x'),
                 FailureReason::MalformedEnvelope,
                 {FailureOrigin::Protocol, FailureScope::Group}};
  failure.detail.node_id = 19;
  builder.fail(failure);
  fail_next_allocation = interpose_allocations;
  auto observed = reference.production_status();
  PS_CHECK(!fail_next_allocation && observed.reason == failure.reason &&
           observed.detail.node_id == 19);
  PS_CHECK(interpose_allocations ? observed.message.empty()
                                 : observed.message == failure.message);
  return 0;
}
int backend_abort_without_allocation() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto program = test::physical_fixture::program();
  program.placements[0].backend = physical::Backend::Gpu;
  test::physical_fixture::output(&program,
                                 test::physical_fixture::task(&program, 0));
  PS_REQUIRE_OK(physical::finalize_program(&program));
  execution_internal::TaskSchedule schedule(program, 0, 1, 1);
  const auto task = schedule.issue_next(physical::Backend::Gpu);
  PS_CHECK(task);
  Status failure{ErrorCode::BackendUnavailable, std::string(128, 'g'),
                 FailureReason::MalformedEnvelope};
  failure.detail.origin = FailureOrigin::Backend;
  failure.detail.scope = FailureScope::Run;
  failure.detail.node_id = 41;
  failure.detail.task_id = 0;
  failure.detail.domain_code = 17;
  Status result;
  result.message.reserve(256);
  fail_next_allocation = interpose_allocations;
  schedule.abort(failure);
  auto completed = schedule.complete(task->key, failure);
  schedule.copy_settled_status(&result);
  const auto unconsumed = fail_next_allocation.exchange(false);
  PS_CHECK(unconsumed == interpose_allocations && completed.ok());
  PS_CHECK(result.code == failure.code && result.reason == failure.reason &&
           result.detail.origin == failure.detail.origin &&
           result.detail.scope == failure.detail.scope &&
           result.detail.domain_code == 17 && result.detail.node_id == 41 &&
           result.detail.task_id == 0 && result.message == failure.message);
  return 0;
}
int invalid_completion_without_allocation() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (bool duplicate : {false, true}) {
    auto program = test::physical_fixture::program();
    program.placements[0].backend = physical::Backend::Gpu;
    test::physical_fixture::output(&program,
                                   test::physical_fixture::task(&program, 0));
    PS_REQUIRE_OK(physical::finalize_program(&program));
    execution_internal::TaskSchedule schedule(program, 0, 1, 1);
    auto task = schedule.issue_next(physical::Backend::Gpu);
    PS_CHECK(task);
    auto invalid_key = task->key;
    if (duplicate)
      PS_REQUIRE_OK(schedule.complete(task->key, {}));
    else
      ++invalid_key.activation;
    fail_next_allocation = interpose_allocations;
    auto status = schedule.complete(invalid_key, {});
    const auto unconsumed = fail_next_allocation.exchange(false);
    PS_CHECK(unconsumed == interpose_allocations);
    PS_CHECK(status.code == ErrorCode::Internal &&
             status.reason == FailureReason::MalformedEnvelope);
    PS_CHECK(schedule.inflight() == (duplicate ? 0 : 1));
    if (!duplicate)
      PS_REQUIRE_OK(schedule.complete(task->key, {}));
    Status result;
    result.message.reserve(256);
    schedule.copy_settled_status(&result);
    PS_CHECK(result.code == ErrorCode::Internal &&
             result.reason == FailureReason::MalformedEnvelope);
  }
  return 0;
}
int callback_failure_copy_exhaustion() {
  using namespace ps;  // NOLINT(build/namespaces)
  ps_prepared_kernel_v8 raw{};
  raw.struct_size = sizeof(raw);
  raw.abi_version = 8;
  raw.invoke_cpu = [](const ps_kernel_invocation_v8*,
                      ps_kernel_status_v8* status) {
    std::array<char, 129> message;
    message.fill('x');
    message.back() = 0;
    *status = kernel_failure(ErrorCode::InvalidArgument, message.data(),
                             FailureReason::MalformedEnvelope);
    status->domain_code = 17;
    if (arm_failure_path.load()) {
      failure_path_allocations = 0;
      inspect_failure_path = true;
    }
  };
  // The coordinator reserves diagnostic capacity before issue. Invocation and
  // assignment must preserve the full bounded error without allocating.
  auto adopted = PreparedKernel::adopt(raw, {});
  PS_REQUIRE_OK(adopted);
  auto kernel = adopted.take_value();
  ps_kernel_invocation_v8 invocation{};
  kernel.bind(&invocation);
  Status observed;
  observed.message.reserve(PS_KERNEL_MESSAGE_CAPACITY_V8);
  fail_next_allocation = interpose_allocations;
  const auto reported = kernel.invoke(invocation);
  assign_kernel_status(&observed, reported);
  const auto unconsumed = fail_next_allocation.exchange(false);
  PS_CHECK(unconsumed == interpose_allocations);
  PS_CHECK(observed.code == ErrorCode::InvalidArgument &&
           observed.reason == FailureReason::MalformedEnvelope &&
           observed.detail.domain_code == 17 &&
           observed.message == std::string(128, 'x'));

  auto op = test::fixed_fixture::operation("test.callback_failure", 0);
  op.prepare_kernel = [raw](const physical::KernelDesc&,
                            const physical::Placement&,
                            const KernelPreparation&, const ResourceSet&,
                            const ps_native_gpu_service_v2*) {
    return Result<ps_prepared_kernel_v8>(raw);
  };
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(std::move(op)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{41, "test.callback_failure", {}, {}}};
  doc.outputs = {{"sink", 41, "out0"}};
  GraphContext graph(doc);
  auto compiled = Compiler(registry).compile(graph);
  PS_REQUIRE_OK(compiled);
  ResourceBudget retained;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    retained = context.resource_budget().take_value();
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    std::atomic<bool> delivered{false};
    ExecutionOptions options;
    options.on_call_retired = [&](const CallRecord& record) {
      delivered = record.status.message == observed.message &&
                  record.status.detail.domain_code == 17;
    };
    arm_failure_path = true;
    auto submitted = context.submit(prepared.value(), {}, {}, options);
    PS_REQUIRE_OK(submitted);
    auto result = submitted.value().join();
    inspect_failure_path = false;
    arm_failure_path = false;
    // Only join's caller-owned diagnostic copy allocates after the backend
    // publishes this failure. Completion, retirement and terminal latch reuse
    // their reserved buffers, including delivery to the observer.
    if constexpr (interpose_allocations)
      PS_CHECK(failure_path_allocations == 1);
    PS_CHECK(delivered);
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::InvalidArgument &&
             result.status().reason == FailureReason::MalformedEnvelope &&
             result.status().message == observed.message &&
             result.status().detail.origin == FailureOrigin::Backend &&
             result.status().detail.scope == FailureScope::Run &&
             result.status().detail.domain_code == 17 &&
             result.status().detail.node_id == 41 &&
             result.status().detail.task_id == 0);
    // The Run is terminal, so this failure injection cannot hit a coordinator
    // or worker allocation. Repeated join must preserve the locked outcome.
    fail_next_allocation = interpose_allocations;
    auto replay = submitted.value().join();
    const auto pending = fail_next_allocation.exchange(false);
    PS_CHECK(!pending && !replay.ok());
    PS_CHECK(replay.status().code == result.status().code &&
             replay.status().reason == result.status().reason &&
             replay.status().detail.origin == result.status().detail.origin &&
             replay.status().detail.scope == result.status().detail.scope &&
             replay.status().detail.domain_code == 17 &&
             replay.status().detail.node_id == 41 &&
             replay.status().detail.task_id == 0);
    PS_CHECK(interpose_allocations
                 ? replay.status().message.empty()
                 : replay.status().message == observed.message);
    auto intact = submitted.value().join();
    PS_CHECK(!intact.ok() && intact.status().message == observed.message);
  }
  for (auto live : retained.statistics().live.values)
    PS_CHECK(live == 0);
  PS_CHECK(retained.statistics().allocated_metadata_bytes == 0);
  return 0;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(schema_allocation_lifetime() == 0);
  PS_CHECK(window_allocation_lifetime() == 0);
  PS_CHECK(footprint_allocation_failure() == 0);
  PS_CHECK(atlas_allocation_failure() == 0);
  PS_CHECK(tile_view_allocation_lifetime() == 0);
  PS_CHECK(publish_layout_allocation_failure() == 0);
  PS_CHECK(publication_metadata_lifetime() == 0);
  PS_CHECK(affine_join_metadata() == 0);
  PS_CHECK(batched_window_join_metadata() == 0);
  PS_CHECK(capture_allocation_lifetime() == 0);
  PS_CHECK(span_allocation_failure() == 0);
  PS_CHECK(scheduler_allocation_lifetime() == 0);
  PS_CHECK(failure_copy_exhaustion() == 0);
  PS_CHECK(backend_abort_without_allocation() == 0);
  PS_CHECK(invalid_completion_without_allocation() == 0);
  PS_CHECK(callback_failure_copy_exhaustion() == 0);
  inspect_allocations = true;
  void* control = ::operator new(32768);
  inspect_allocations = false;
  ::operator delete(control);
  if constexpr (interpose_allocations)
    PS_CHECK(large_allocations == 1);
  SchemaTemplate schema;
  schema.id = "test.large_metadata";
  for (unsigned i = 0; i < 16; ++i) {
    ResultTensorSpec tensor;
    const auto key = "tensor_" + std::to_string(i) + std::string(100, 'x');
    tensor.key.assign(key.data(), key.size());
    tensor.descriptor = {ElementType::UInt8, {1, 1, 1, 1, 1, 1, 1, 1}};
    schema.tensors.push_back(std::move(tensor));
  }
  PS_CHECK(schema.validate().ok());
  PS_CHECK(schema.canonical_size() == schema.canonical().size());
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 1024;
  limits.capacity[ResourceKind::Metadata] = 1024;
  ResourceBudget budget(limits);
  large_allocations = 0;
  inspect_allocations = true;
  auto result = ResultBuilder::start(budget, std::move(schema), "scope");
  inspect_allocations = false;
  PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
  if constexpr (interpose_allocations)
    PS_CHECK(large_allocations == 0);
  PS_CHECK(budget.statistics().peak[ResourceKind::Host] <= 16384);
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  SchemaTemplate image_schema;
  image_schema.id = "test.large_image_metadata";
  ResultTensorSpec image;
  image.batch_axes = {1, 1};
  image.layout.spatial = true;
  image.key = "pixels";
  image.descriptor = {ElementType::Float32, {1, 1}};
  image.layout.channel_axis = {};
  image_schema.tensors.push_back(std::move(image));
  PS_CHECK(image_schema.validate().ok());
  large_allocations = 0;
  inspect_allocations = true;
  auto refused_image = ResultBuilder::start(budget, image_schema, "scope");
  inspect_allocations = false;
  PS_CHECK(refused_image.status().code == ErrorCode::ResourceExhausted);
  if constexpr (interpose_allocations)
    PS_CHECK(large_allocations == 0);
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
