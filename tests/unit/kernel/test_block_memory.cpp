#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "execution/block_memory.hpp"
#include "execution/diagnostics.hpp"
#include "support/physical_program_fixture.hpp"
#include "support/status_check.hpp"

int main() {
  using ps::execution_internal::BlockMemory;
  auto program = ps::test::physical_fixture::diamond();
  program.tiles[0].alignment = 4096;
  program.tasks[0].scratch[0] = 8;
  PS_REQUIRE_OK(ps::physical::finalize_program(&program));
  ps::ResourceLimits limits;
  const auto reservation = program.blocks[0].reservation.outputs[0] + 16;
  limits.capacity[ps::ResourceKind::Payload] = reservation - 1;
  ps::ResourceBudget small(limits);
  auto refused = BlockMemory::allocate(program, 0, 2, small);
  PS_CHECK(!refused.ok());
  PS_CHECK(refused.status().code == ps::ErrorCode::ResourceExhausted);
  PS_CHECK(small.statistics().live[ps::ResourceKind::Payload] == 0);
  PS_CHECK(small.statistics().live[ps::ResourceKind::Metadata] == 0);

  limits.capacity[ps::ResourceKind::Payload] = reservation;
  ps::ResourceBudget root(limits);
  auto observed = std::make_shared<ps::execution_internal::MemoryObservation>();
  std::uint64_t allocations = 0;
  std::shared_ptr<const ps::CpuStorage> exported;
  {
    auto allocated = BlockMemory::allocate(program, 0, 2, root, 0, nullptr,
                                           nullptr, &allocations, observed);
    PS_REQUIRE_OK(allocated);
    auto block = allocated.take_value();
    PS_CHECK(allocations == 5);
    PS_CHECK(observed->snapshot().reserved_bytes_by_domain[0] == reservation &&
             observed->snapshot().live_bytes_by_domain[0] == 36);
    PS_CHECK(root.statistics().live[ps::ResourceKind::Payload] == reservation);
    const auto full_metadata = root.statistics();
    PS_CHECK(full_metadata.allocated_metadata_bytes >
             7 * sizeof(ps::CpuStorage));
    PS_CHECK(full_metadata.allocated_metadata_bytes <=
             full_metadata.live[ps::ResourceKind::Metadata]);
    PS_CHECK(reinterpret_cast<std::uintptr_t>(block.tile(0).data()) % 4096 ==
             0);
    for (unsigned i = 0; i < 5; ++i) {
      PS_CHECK(block.tile(i).size() == 4);
      for (unsigned j = 0; j < i; ++j)
        PS_CHECK(block.tile(i).data() != block.tile(j).data());
    }
    PS_CHECK(block.scratch(0).size() == 8);
    PS_CHECK(block.scratch(0).data() != block.scratch(1).data());
    block.tile(0).data()[0] = 42;
    exported = block.publish(0);
    PS_CHECK(!block.tile(0).data());
    PS_CHECK(root.allocator().owns(*exported));
    auto alias = root.reference(exported);
    PS_REQUIRE_OK(alias);
    PS_CHECK(root.statistics().live[ps::ResourceKind::Referenced] == 0);
    block.discard(1);
    PS_CHECK(root.statistics().allocated_metadata_bytes <
             full_metadata.allocated_metadata_bytes - sizeof(ps::CpuStorage));
    PS_CHECK(root.statistics().live[ps::ResourceKind::Metadata] ==
             full_metadata.live[ps::ResourceKind::Metadata]);
    PS_CHECK(observed->snapshot().reserved_bytes_by_domain[0] == reservation &&
             observed->snapshot().live_bytes_by_domain[0] == 32);
    PS_CHECK(root.statistics().live[ps::ResourceKind::Payload] == reservation);
    // Destroying an active directory through move assignment releases only its
    // own owners; an exported tile retains precisely its independent capacity.
    block = {};
    PS_CHECK(root.statistics().live[ps::ResourceKind::Payload] == 4);
    PS_CHECK(exported->bytes()[0] == 42);
  }
  PS_CHECK(root.statistics().live[ps::ResourceKind::Payload] == 4);
  PS_CHECK(observed->snapshot().reserved_bytes_by_domain[0] == 4 &&
           observed->snapshot().live_bytes_by_domain[0] == 4);
  exported.reset();
  PS_CHECK(observed->snapshot().reserved_bytes_by_domain[0] == 0 &&
           observed->snapshot().live_bytes_by_domain[0] == 0);
  const auto settled = root.statistics();
  PS_CHECK(settled.allocated_metadata_bytes == 0 &&
           settled.peak_allocated_metadata_bytes > 0);
  for (auto live : settled.live.values)
    PS_CHECK(live == 0);
  PS_CHECK(settled.peak[ps::ResourceKind::Payload] == reservation);

  // Independent exported allocations may retire concurrently after the block.
  auto parallel_memory = BlockMemory::allocate(program, 0, 2, root);
  PS_REQUIRE_OK(parallel_memory);
  auto parallel_block = parallel_memory.take_value();
  auto left = parallel_block.publish(0);
  auto right = parallel_block.publish(1);
  parallel_block = {};
  PS_CHECK(root.statistics().live[ps::ResourceKind::Payload] == 8);
  std::mutex mutex;
  std::condition_variable changed;
  unsigned ready = 0;
  bool release = false;
  auto retire = [&](std::shared_ptr<const ps::CpuStorage> owner) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    owner.reset();
  };
  std::thread a(retire, std::move(left));
  std::thread b(retire, std::move(right));
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return ready == 2; });
    release = true;
  }
  changed.notify_all();
  a.join();
  b.join();
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);

  // A failure after successful admission/earlier allocations rolls everything
  // back, including storage descriptors and committed payload observations.
  auto too_large = program;
  too_large.tiles.back().bytes = std::uint64_t{INT64_MAX} + 1;
  PS_REQUIRE_OK(ps::physical::finalize_program(&too_large));
  ps::ResourceLimits unlimited;
  unlimited.capacity.values.fill(UINT64_MAX);
  unlimited.cleanup.values.fill(0);
  ps::ResourceBudget rollback(unlimited);
  auto failed_allocation = BlockMemory::allocate(
      too_large, 0, 2, rollback, 0, nullptr, nullptr, &allocations, observed);
  PS_CHECK(!failed_allocation.ok());
  PS_CHECK(allocations > 0 && allocations < 5);
  PS_CHECK(observed->snapshot().reserved_bytes_by_domain[0] == 0 &&
           observed->snapshot().live_bytes_by_domain[0] == 0);
  PS_CHECK(failed_allocation.status().code == ps::ErrorCode::ResourceExhausted);
  for (auto live : rollback.statistics().live.values)
    PS_CHECK(live == 0);
  PS_CHECK(rollback.statistics().allocated_metadata_bytes == 0 &&
           rollback.statistics().peak_allocated_metadata_bytes > 0);

  // Default and explicitly aligned host buffers preserve zero initialization.
  ps::BufferAllocator allocator;
  auto ordinary = allocator.allocate(13);
  PS_REQUIRE_OK(ordinary);
  auto ordinary_buffer = ordinary.take_value();
  for (unsigned i = 0; i < 13; ++i)
    PS_CHECK(ordinary_buffer.data()[i] == 0);
  PS_CHECK(!allocator.allocate_aligned(4, 3).ok());
  PS_CHECK(!allocator.allocate_aligned(4, 0).ok());
  auto over_aligned = allocator.allocate_aligned(13, 256);
  PS_REQUIRE_OK(over_aligned);
  auto aligned_buffer = over_aligned.take_value();
  PS_CHECK(reinterpret_cast<std::uintptr_t>(aligned_buffer.data()) % 256 == 0);
  return 0;
}
