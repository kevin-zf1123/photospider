# Bounded GPU dependency discovery

C++ `DependencyPhase::discover(capacity, candidates, compute)` and the C
`ps_dependency_services_v8::discover` service run a separate bounded discovery
callback over already supplied inputs. The host allocates a zeroed native table
through existing MemoryBudget admission, runs synchronous native work, freezes
the buffer, validates it and appends decoded needs to the current poll. There is
no shader page-fault executor or automatic tracing of arbitrary kernels.

A nonempty successful table requires the program to return Need. The Session
attaches rows for the current Atomic observation, or full original terminal
RequestRecord, then the existing Run resolves upstream inputs and supplies the
next poll. Numerical completion with unresolved table requests fails. Discovery
inside a pure block, a pure block inside discovery and recursive discovery are
rejected; a cached block cannot suppress required table population. An empty
table adds no dependency and may precede a constant/control-only completion.

The callback borrows table pointers only until return. Native work drains before
host decoding; freezing revokes writable native views. C services return Boolean
1 success / 0 failure, while the C compute callback returns operation result
codes, never NEED. C discovery services expose ready reads, scratch, atlas,
native views/dispatch, work and cancellation, without publication, association,
checkpoint or block services. Errors remain sticky even if ignored.

## Wire format and bounds

`PS_GPU_DISCOVERY_MSL_V8` supplies the Metal `ps_discovery_emit` helper. The table
starts with four little-endian uint32 words: attempted emit count, overflow,
zero and zero. It is followed by `capacity` records of 144 bytes each:

| Byte offset | Field |
| --- | --- |
| 0, 4, 8, 12 | uint32 port, role mask, rank, zero |
| 16..79 | uint64 offsets[8] |
| 80..143 | uint64 extents[8] |

The atomic counter includes every emit attempt. A slot beyond capacity sets the
sticky overflow flag and writes no record. Programs declare a positive upper
bound `candidates` on all attempts across their discovery dispatches, including
duplicates and overflow. This is at most UINT32_MAX. The trusted shader must
respect its declared finite work; the host does not sandbox arbitrary loops.

The host verifies header, count, rank, port, role mask (Data/Control/Validation),
positive extents, unused zero axes and global domain bounds. Every raw image
record must cover full C before normalization; two half-channel records cannot
combine to bypass image-v2 closure. Invalid records fail; overflow returns
ResourceExhausted and never an empty/partial successful dependency set.

`DependencyLimits::maximum_gpu_requests` limits one table, defaults to 65536 and
can be zero to disable discovery. The hard capacity limit is also 65536. Existing
invocation work, stage and metadata limits bound repeated calls; this table limit
also participates in Flight identity. Capacity and initialization/decoding work
are charged before allocation, and candidate work before the callback. Each
native table allocation rounds its own logical byte span to real device capacity
before admission. Table, atlas, state, scratch and output leases remain charged
until their last owner retires.

Normalization groups share the same remaining invocation work. The optional
`Footprint::from_regions(..., consumed_work)` counter reports charged work on
success, failure and exceptions after entry; the decoder deducts actual work
between groups. Per-poll discovery metadata also shares one limit across calls,
including row/need/box units and role expansion. Each group receives only its
remaining box grant. Raw callback associations are bounded and their search is
charged before automatic attachment; deduplication cannot erase this work.

## Public verification

`examples/g4_gpu_workflow/discovery_plugin.c` is a real C11 staged plugin using
Int64 controls and Float32 data. For each observation a GPU discovery dispatch
reads the supplied control atlas and requests two distant coordinates. After
host supply, a second dispatch reads the data atlas and computes the result.
Integer controls avoid a CPU/Metal floating-point address-rounding difference;
negative/out-of-domain controls fail on both paths.

The independent expected values are 8 and 24. Two observations perform four
native dispatches. A control edit changes the first result to 24 and replaces its
data edges; old data edits are clean while new data edits dirty that output. A
frozen execution still returns 8. Capacity-one overflow, disabled discovery and
premature numerical completion fail. A 128-entry table has 18448 logical bytes
and 32768 native allocation bytes; the current C adapter's complete minimum
stage reservation is 33108 bytes, and 33107 fails finitely.

```sh
cmake --build build/issue257-static --target test_gpu_discovery photospider_g4_discovery_workflow -j 8
ctest --test-dir build/issue257-static -R '^(test_gpu_discovery|photospider_g4_discovery_workflow)$' --output-on-failure
build/issue257-static/photospider_g4_discovery_workflow
```

`test_gpu_discovery` uses explicitly nonnative protocol mocks for malformed
headers/records, shared work and metadata budgets, raw row limits, ignored
failures, recursive/block misuse, cancellation, terminal full-Q preservation,
partial image channels and a rank-eight domain beyond uint64 dense cardinality.
These mocks are not native evidence. Static/shared installed consumers run them
and compile the real C11 module/loader; the standalone example builds against the
installed public package. Native unavailability returns 77 after CPU checks.

Synchronous GPU producers and CPU fallback within dependency templates are
described in [Fragment Atlas](Fragment-Atlas.md#synchronous-producers-and-cpu-fallback).
