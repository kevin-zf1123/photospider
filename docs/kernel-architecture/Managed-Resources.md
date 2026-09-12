# Managed resources and mandatory temporary storage

`ExecutionContextConfig::managed_resources` enables one shared `ResourceBudget`
for existing controlled buffer reservations and explicit temporary-storage
clients. `maximum_live_bytes` remains a payload sublimit. Obtain the root with
`ExecutionContext::resource_budget()`. It is independent of completed-result
cache configuration and retained leases can outlive the context.

## Capacity and work

`ResourceCapacity` separates host, device, shared, metadata, referenced input,
temporary disk, entries, files, I/O slots, queue and Payload constraints.
Payload counts managed buffer bytes and is capped by `maximum_live_bytes` in
an execution context, including structured callbacks. Host includes
metadata and shared bytes; device includes shared bytes. Overlapping dimensions
must not be added to report physical memory. Whole vectors are checked before
admission. `ResourceLease` copies share one owner. Growth includes simultaneous
old/new capacity; shrinking is allowed only after storage retirement or an
unissued reservation is abandoned. Protected cleanup capacity cannot be spent
by ordinary stages. Admission never waits on retained owners: insufficient
capacity returns `ResourceExhausted`.

Lease object capacity is charged automatically, while managed buffer and
file/window owners charge their declared C++ object capacity. Root bootstrap,
unmanaged allocator control blocks/headers, standard-library private allocations,
thread stacks, driver state and OS page cache are outside this accounting
model. Legacy execution metadata without a resource lease remains outside the
model. `ResourceAllocator` admits its requested block and explicit alignment
header before allocating; cancellation state/control storage and its flattened
source list use that allocator. Retained allocator-aware diagnostics own their
capacity independently of result payloads. `ResourceAllocationKind::Payload`
marks STL computation data: its element block also counts toward Payload, while
the explicit header remains Metadata. Copy, rebind and active-scope copying
preserve that role. This API does not certify process RSS or native-device allocator overhead.
The counter `live` means live admitted capacity, including unused reservations;
`peak` is an observed peak of that counter, not a proved input-class bound.
The guarantee is `WithinBudgetOrFail` for the declared capacity model.

`reference(storage)` admits the full caller allocation capacity under the
Referenced sublimit, deduplicated by actual storage owner within one root.
The returned alias preserves the `CpuStorage` address and retains the reference
lease through downstream views. Execute bindings use this admission when the
managed root is enabled. Source-private state is not inferred from callbacks.
Concurrent first references and last-reference retirement are serialized so
the same live owner never needs a second capacity reservation.

`consume(ResourceWork)` precharges work, bytes, requests and stages atomically.
Issued work is never refunded after failure, fallback or cancellation. Singleton
and joint dependency sessions charge their current Run root before issuing work,
including start failures and GPU discovery normalization. `FootprintLimits` can
carry a borrowed host work callback for precharged set construction; this callback
is not stored in an immutable Footprint or any semantic identity.
Every queued source, Whole/backend attempt, singleton/joint dependency callback
and structured callback also precharges one root stage before submission. The
root stage count is cumulative across Runs. Queue counts callbacks waiting for
a worker and is released before callback entry; envelope metadata remains
charged through callback retirement. These limits apply with the cache disabled.

## Temporary backing

`TemporaryStorage` owns a private, unbuffered temporary file with arithmetic byte
addressing. Encoded extents are rounded separately to 4096 bytes. The disk limit
counts the encoded file capacity, not filesystem blocks or physical device I/O.
No per-page resident directory, mmap or optional-cache eviction is required.
Every read is explicit, range checked and window bounded; no producer computation
occurs inside storage access. Reads allocate an owning immutable buffer, retaining
the backing and root until the last window is released.

Append reserves growth before writing. Failure restores the previous allocation
end or quarantines its reservation if rollback cannot be verified. Successful
file close settles quarantine; failed close keeps capacity charged. A monotone
frozen prefix cannot be overwritten. Sealing stops further production. Prefix
finality and cross-field association validation belong to the result publisher,
not to this byte-storage primitive. Cancellation stops new I/O; already submitted
synchronous calls finish before owners are released. Cleanup needs no new window.

## Focused checks

```sh
cmake --build build/issue257-shared --target test_resources test_dependency_program test_dependency_joint test_joint_execution test_footprint test_memory_liveness -j 8
ctest --test-dir build/issue257-shared -R '^(test_resources|test_dependency_program|test_dependency_joint|test_joint_execution|test_footprint|test_memory_liveness)$' --output-on-failure
```

`test_resources` includes a 65536-byte temporary payload under a 16384-byte
managed host limit, independent owner/accounting assertions, alias lifetime,
concurrent admission, referenced-owner deduplication, cancellation and bounded
failure. `test_dependency_program` reproduces the parent Need/upstream/resume
fuel sequence: root 10000 admits only the upstream's 6000 algorithm units; root
30000 admits both 6000-unit phases. Host protocol work consumes additional units.
`test_managed_dispatch` checks zero/cumulative root stages, zero/one Queue slots
across Whole, source, dependency and atom execution, and structured source error
provenance through returned failures and exceptions.

The installed target `photospider_resource_consumer` compiles the same public
API behavior checks through `find_package(Photospider 0.10 CONFIG REQUIRED)`.
It does not include private kernel headers or link a source-tree kernel target.
