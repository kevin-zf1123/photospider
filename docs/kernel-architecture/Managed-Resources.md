# Managed resources and temporary storage

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

`CancellationToken`, `ResourceBudget`, `ResourceLease`, `ResourceAllocationScope`, and `ResourceAllocator` are public core facilities declared under `photospider/core/`. Cancellation is cooperative and monotone. `ResourceBudget::consume` admits cumulative work atomically; issued work and I/O are not refunded after failure or cancellation. Resource accounting bounds instrumented capacity and work, not process RSS.

The budget tracks Host, Device, Shared, Metadata, Referenced, Disk, Entries, Files, I/O slots, Queue, and Payload dimensions. Some dimensions describe the same physical bytes: Host includes Metadata and Shared, and Device includes Shared. Do not add these overlapping counters as if they were separate allocations. Payload is additionally limited by `maximum_live_bytes`. Referenced caller-owned storage is separately admitted under Referenced and does not consume the Payload sublimit.

Inside the kernel, the data layer owns Payload reservations: the internal `MemoryBudget` and `MemoryReservation` types in `src/lib/data/memory_budget.hpp` implement the Payload capacity and the reservation leases that storage and Results retain. Admission that borrows Run or context state stays in the execution layer. The internal `ScopedMemoryAdmission` helper hands allocators a copyable admission callback, and its owner closes that callback before the borrowed state retires; payload leases never retain it. `test_resource_internals` exercises this helper directly. Public resource and storage headers only forward-declare these types and keep their layouts and signatures.

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

`consume(ResourceWork)` admits work, byte, request, and stage counters atomically. Work or I/O exhaustion returns `ResourceExhausted` with `WorkLimit`; stage exhaustion returns `StageLimit`. Rejected work does not increment issued counters. Issued work is not refunded after failure, fallback, or cancellation. Callback submission also charges one cumulative root stage. For ordinary callback submissions, Queue capacity counts callbacks waiting to start and is released when a worker removes the callback; callback envelope metadata remains charged through callback retirement. A CPU tile job retains its waiting admission and managed Queue lease until all submitted tiles retire and the job is unlinked.

The coordinator validates each `ResultObjectNeed` and `ResultTensorNeed` against the input schema and published coverage before exposing the corresponding capability. A `ResultTensorInput` grants reads only for the requested samples. Tensor-window acquisition retains the granted backing owners and leases; the window remains valid independently of the callback while its owning handle is retained. `TemporaryStorage` uses a separately passed `ResourceBudget` to account for its bounded private-file capacity and returned buffers.

Structured Result execution uses the same root for image pages, immutable sample coverage, relation witnesses, field storage, and retained source owners. When an image publication materializes PlanarImage storage, its backing is bounded per frame and layer; metadata and coverage maps are charged as Metadata, and planar pixel capacity is charged as Payload. Other tensor publication paths can retain affine or `CpuStorage` backing and source Result owners without copying payload. Those retained owners keep their existing accounting leases alive. An image read capability retains the Result and the captured descriptor that authorized its sample Region. Copying that capability also retains its backing and accounting lease. `ResultRef::capture()` retains an immutable descriptor revision, coverage, relations, and dependency evidence; shared waiters consume that publication snapshot rather than observing a later producer revision.

Result relation rows and published image payload use the selected resource root. Each publication must fit configured payload and work, I/O, and stage limits. `Exact`, `Conservative`, and `Unknown` relations have distinct dirty-propagation behavior; an unresolved relation cannot certify a clean output. PlanarImage materialization copies bytes into managed backing and charges that payload before exposing sample coverage; affine and storage-view publication retains source owners and their accounting instead of copying tensor bytes. Schema selection retains only declared typed image and Result metadata resources, including ICC and OCIO bindings. The compiler carries nested Result schemas and their resource identities into the plan; runtime Result bindings re-admit the required owners under the execution root.

Typed tensor sample validation charges work in credit batches of at most 256 scalar samples. Before it reads a sample with no remaining credit, the validator charges `min(256, remaining)` work units for the region, then validates every sample individually with the same numeric and semantic checks. A region of `N` samples that passes consumes exactly `N` units. A failing sample can leave up to 255 prepaid samples unread, and that work is not refunded. A rejected charge returns its status without retrying a smaller batch. The validator checks cancellation before each sample read and after each charge, and checks cancellation and the host stop state after each validated region, so a cancelled, stale or stopped Run ends no later than the next region boundary. The work recorded for a failed validation can therefore exceed the number of samples read.

Data-dependent relation tables, Footprint box indexes, radix sort scratch and operation-owned caches use `ResourceAllocator` on the execution root. A `ResultRelation::gather` table, its normalized support and the STMap map and source caches are charged as Host and Metadata capacity for as long as their owner lives; the published relation keeps its table until the last Result or dependency snapshot that references it is released. Host capacity includes Shared and Metadata, so the Host and Metadata peaks of one Run overlap and must not be added to estimate process memory.

`ExecutionDependencies` returns its coverage and guarantee maps with `ResourceMap` allocators tied to the same root. `source_support()` and `potential_dirty()` return root-owned `ResourceMap<Footprint>` values; `source_observations()` returns a root-owned `ResourceVector<SourceObservation>`. Each observation owns its `ResourceString` input name and `Footprint`, and records the typed target, slot, and roles. These values can outlive the `ExecutionDependencies` object and `ExecutionContext`; their allocator owners keep the accounting root alive until the last returned map, vector, name, or footprint is released.

```cpp
#include "photospider/execution/dependencies.hpp"

// dependencies is ExecutionResult::dependencies from a completed run.
ps::ResourceMap<ps::Footprint> coverage = dependencies.coverage();
ps::ResourceMap<ps::DependencyGuarantee> guarantees = dependencies.guarantees();
auto observations = dependencies.source_observations();
```

The maps and observation sequence are copied snapshots. Callers can retain or move them without borrowing the dependency object, while their root-owned storage stays charged.

GPU contexts use the execution context resource root for native allocations when a device is created. The root exists whether or not custom managed-resource limits were supplied. Invocation metadata has a separate explicitly supplied accounting domain. The native pipeline cache retains at most 64 entries; a dispatch batch retains up to 32 commands, each with up to 31 storage bindings; an invocation retains at most 1024 live buffer views. The address map keeps weak `CpuStorage` references and does not own native buffers.

When managed native metadata admission fails, the device clears its native pipeline cache and retries once. GPU payload admission can first reclaim eligible pending disk writes and result-cache owners, then reserves actual native capacity atomically. These recovery paths do not retry operation callbacks.

`TemporaryStorage` owns a private unbuffered temporary file with byte addressing. Encoded extents are rounded to 4096 bytes, and the disk limit counts encoded file capacity rather than filesystem blocks. Reads are bounded, range checked, synchronous, and return immutable owning buffers that keep the backing file and root lease alive. Appends reserve capacity before writing; failed rollback quarantines the reservation until successful file close. A frozen prefix cannot be overwritten, and sealing ends production. Cancellation prevents new I/O while submitted synchronous calls finish before owners retire.

Before it creates its file and before each append, write or read, `TemporaryStorage` asks the calling thread's I/O protocol fence for permission. The fence is a synchronous thread-local capability in the core layer: its owner supplies the policy, its state is borrowed until the scope exits, and a nested scope restores the previous one on exit. With no scope installed, I/O proceeds. Around a fenced Result callback, execution installs a scope bound to the callback's failure code and failure latch. A `TemporaryStorage` call made inside that callback is a mandatory I/O protocol violation and fails with `InvalidArgument`, `UnauthorizedRead` and Protocol/Group detail before the file changes. An earlier recorded failure stays the reported first cause, for example a prior `ResourceExhausted` with `CapacityLimit`, and the latched violation vetoes CPU retry.

An operation that declares `preserve_output_views` may publish an affine input view when it covers the required input region. Compatible fragments from the same owner may be combined only after address mapping proves their coverage. Storage owned outside the resource root receives a Referenced lease; storage already owned by the root keeps its existing Payload and Host accounting without another Referenced charge. Newly allocated output backing follows the active output allocator. The option supports CPU Atomic staged or Whole execution and excludes GPU and joint execution. `requires_input_views` further restricts the operation to Whole execution.

## Algorithms & Math

Resource products, alignment, and page rounding are checked before allocation or publication. Admission operates on declared or queried allocation capacity; `live` reports currently admitted capacity including unused reservations, and `peak` reports the observed maximum of that counter.

## Limitations & Non-Goals

- Managed capacity is a `WithinBudgetOrFail` guarantee for accounted allocations; it does not cap total RSS or opaque driver and standard-library storage.
- Empty containers and some internal geometry/reconstruction metadata are not comprehensively charged. Reconstructed outer `ValueFragments` metadata may outlive its original publication token, while each `Value` retains its storage owner.
- A caller that extracts a raw `Value` or vector and copies it outside the managed allocator assumes that copy's memory cost.
- Prefix finality and cross-field association validation belong to the Result publisher, not `TemporaryStorage`.
