# Managed resources

## Scope & Ownership

Every `ExecutionContext` creates a resource root shared by controlled CPU and GPU work. `ExecutionContextConfig::managed_resources`, when present, supplies limits for that root; otherwise the root uses default `ResourceLimits`. `ExecutionContext::resource_budget()` returns the initialized budget as a `Result`. Retained leases can outlive the context. Managed accounting covers declared capacity, not process RSS. Thread stacks, driver-private allocations, operating-system page cache, and allocations outside managed allocators remain outside the guarantee.

```cpp
#include <cstdint>

#include "photospider/core/resources.hpp"

namespace ps {
Result<ResourceLease> reserve_capacity(const ResourceBudget& budget,
                                       std::uint64_t bytes) {
  return budget.reserve(ResourceCapacity::host(bytes));
}
}
```

`CancellationToken`, `ResourceBudget`, `ResourceLease`, `ResourceAllocationScope`, and `ResourceAllocator` are public core facilities declared under `photospider/core/`. Cancellation is cooperative and monotone. `ResourceBudget::consume` admits cumulative work atomically; issued work is not refunded after failure or cancellation. Resource accounting bounds instrumented capacity and work, not process RSS.

The budget tracks Host, Device, Shared, Metadata, Referenced, Entries, Queue, and Payload dimensions. Some dimensions describe the same physical bytes: Host includes Metadata and Shared, and Device includes Shared. Do not add these overlapping counters as if they were separate allocations. Payload is additionally limited by `maximum_live_bytes`. Referenced caller-owned storage is separately admitted under Referenced and does not consume the Payload sublimit.

`ExecutionContext` constructs and owns a `ResourceBudget` directly. Its `maximum_live_bytes` must be positive; the constructor throws `std::invalid_argument` for zero. The effective Payload limit is the smaller of that value and the optional `managed_resources` Payload capacity. A standalone `ResourceBudget` may have zero Payload capacity; an execution context always requires a positive `maximum_live_bytes`. Scheduling and Result publication charge and admit work directly against the context's Root budget.

The internal `data_internal::MemoryBudget` and `MemoryReservation` implement an independent quota for snapshot aggregate capacity and reserved working sets. A snapshot budget requires positive aggregate capacity, and its constructor throws `std::invalid_argument` for zero. Snapshot reservations and the execution Root maintain separate accounting responsibilities.

`reserve(bytes)` sets aside a bounded working set or returns `ResourceExhausted` when live reservations would exceed the aggregate capacity. Its allocator admits only blocks within that reservation and returns `ResourceExhausted` after sealing or when a block would exceed the reserved working set. Sealing releases unused capacity, while allocated blocks stay charged until their last storage owners retire.

## Data Layout & Memory

```cpp
#include <cstdint>

#include "photospider/data/storage.hpp"

namespace ps {
void allocate_with_quotas(BufferAllocator allocator,
                          std::uint64_t maximum_capacity,
                          std::uint64_t maximum_requested,
                          std::uint64_t bytes) {
  auto scoped = allocator.limited(maximum_capacity);
  auto requested = scoped.limited_requested(maximum_requested);
  auto allocation = requested.allocate(bytes);
}
}  // namespace ps
```

Copies of a lease share one reservation. Its capacity remains charged until the last lease owner is destroyed. A grow reserves the additional capacity atomically. Callers shrink only after the corresponding storage has been released; failed admission returns `ResourceExhausted` with a capacity-limit status and does not wait for another owner to retire. `ResourceAllocator` admits its requested block and alignment header before allocation and retains the lease in the allocation header until after physical storage is freed. Payload-kind STL allocations also count their elements toward Payload; the allocator header remains Metadata.

`CpuStorage` exposes immutable borrowed bytes while an owning storage reference remains alive. Its native owner is destroyed before capacity and requested-byte leases are released. `BufferAllocator::limited()` constrains actual backing capacity; `limited_requested()` constrains the sum of live requested bytes. Nested scopes can apply both limits. Native conversion preserves allocation provenance and failure observers, and charges actual native capacity to the root.

`reference(storage)` admits the full capacity of storage owned outside the resource root under Referenced, deduplicated by live storage owner within one root. If the root allocator already owns the storage, the method returns it without a Referenced reservation; its existing Payload and Host capacity leases remain in force. Execute bindings use this admission for retained inputs when the managed root is enabled. A downstream view keeps the selected owner and its existing accounting lease alive.

## Execution & State

`consume(ResourceWork)` admits work and stage counters atomically. Work exhaustion returns `ResourceExhausted` with `WorkLimit`; stage exhaustion returns `StageLimit`. Rejected work does not increment issued counters. Issued work is not refunded after failure, fallback, or cancellation. Callback submission also charges one cumulative root stage. For ordinary callback submissions, Queue capacity counts callbacks waiting to start and is released when a worker removes the callback; callback envelope metadata remains charged through callback retirement. A CPU tile job retains its waiting admission and managed Queue lease until all submitted tiles retire and the job is unlinked.

The coordinator validates each `TileNeed` against the input schema and published coverage before exposing the corresponding capability. A `ResultTensorInput` grants reads only for the requested samples. Tensor-window acquisition retains the granted backing owners and leases; the window remains valid independently of the callback while its owning handle is retained.

The structured Result cache and the checkpoint cache state each store their own `ResourceBudget` handle, copied from the execution root. A copy shares the same root, so both components keep charging that root after the coordinator's wrapper is reassigned or destroyed, and their correctness does not depend on member declaration order in the coordinator.

Structured Result execution accounts Tensor backing, coverage, tile records, frozen shared evidence and retained owners under the same Root. Materialization allocates actual requested sample capacity; view publication retains existing owners without copying payload. Metadata and directories count as Metadata, numerical backing as Payload, and external retained storage as Referenced where applicable. `capture()` fixes descriptor revision and authorized coverage; subsequent progress does not widen old captures.

Tile dependency metadata and published Tensor payload use the selected Root. Each transaction checks work, payload, stage and I/O limits. Frozen evidence reuse does not grant sample access. Result schemas carry no resources. Operation resources are generic `ImmutableResource` handles: owning immutable bytes with a `ResourceIdentity` (byte length and SHA-256), admitted under a root without any format parsing. `ImmutableResource::import` copies and hashes bytes; `from_storage` hashes existing storage without copying; both charge work in the supplied budget and observe cancellation, and a failure releases every temporary allocation. `ResourceSet::create` deduplicates backing and rejects collisions; `select` retains exactly the requested identities and fails with `NotFound` for a missing one; `reference` re-admits the set under another root without copying payload. The compiler selects each operation's declared identities from the caller's set and the plan retains them; when an execution root differs from the owning root, the references are admitted under it. A Result never keeps a resource alive on its own.

The kernel invokes explicitly bound validators without adding default sample-domain rules. Float32 scalar intervals are the ops SDK `ops.scalar-range` validator contract. Validator reads consume work, scratch is admitted before entry, and spent work is not refunded on failure. Algorithms retain their own domain checks.

GPU contexts use the execution context resource root for native allocations when a device is created. The root exists whether or not custom managed-resource limits were supplied. Invocation metadata has a separate explicitly supplied accounting domain. The native pipeline cache retains at most 64 entries; a dispatch batch retains up to 32 commands, each with up to 31 storage bindings; an invocation retains at most 1024 live buffer views. The address map keeps weak `CpuStorage` references and does not own native buffers.

When managed native metadata admission fails, the device clears its native pipeline cache and retries once. GPU payload admission can first reclaim eligible pending disk writes and result-cache owners, then reserves actual native capacity atomically. These recovery paths do not retry operation callbacks.

Operations explicitly acquire and publish authorized views. Compatible fragment mappings are checked; reshape additionally requires an affine join. Directly publishing a caller-owned `CpuStorage` without source Result owners admits that backing through the target Root's `reference(storage)` path (unless the same Root already owns it). A single-source or multi-piece Result view instead retains its source `ResultRef` owners and their original Root backing leases: the target Root charges its own view metadata and publication work, but does not create a second Referenced charge for source-backed payload. This also holds when those Results originate from different Roots. New backing uses the active allocator. Dense collection and device packing/transfer are explicit operations with capacity, work and cancellation checks, not coordinator policy.

## Algorithms & Math

Resource products, alignment, and page rounding are checked before allocation or publication. The kernel uses one set of unsigned helpers in `include/photospider/core/checked_math.hpp`: checked addition, multiplication, multiply-add and alignment, each with an optional upper bound, plus saturating multiplication. A failed check leaves the output unchanged, and each call site keeps its own outcome:

| Quantity | On overflow |
| --- | --- |
| Allocation, storage, transfer and work charges | Rejected, normally with `ResourceExhausted` |
| Logical domain size and reshape cost estimates | Saturate at the bound |
| Cost estimate for an optional checkpoint cache | The cache path is skipped; the operation still runs |

A capacity check accepts a request that exactly fills the remaining capacity; for example, a result cache configured for 24 bytes admits a 16-byte and an 8-byte entry without eviction. Admission operates on declared or queried allocation capacity; `live` reports currently admitted capacity including unused reservations, and `peak` reports the observed maximum of that counter.

## Limitations & Non-Goals

- Managed capacity is a `WithinBudgetOrFail` guarantee for accounted allocations; it does not cap total RSS or opaque driver and standard-library storage.
- Empty containers and some internal geometry/reconstruction metadata are not comprehensively charged. Reconstructed outer `Tensor` metadata may outlive its original publication token, while each `Tensor` retains its storage owner.
- A caller that extracts a raw `Tensor` or vector and copies it outside the managed allocator assumes that copy's memory cost.
- Result C ABI 6 releases ordinary C callback scratch when each bridge `poll` ends. Retained tensor handles and immutable block-state Results created by `create_block_state` persist across polls.
