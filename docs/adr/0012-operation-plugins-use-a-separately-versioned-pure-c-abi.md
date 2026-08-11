# ADR 0012: Operation Plugins Use a Separately Versioned Pure-C ABI

## Status

Accepted as the target contract for GitHub Issue #101 / S-10 on 2026-08-11.
This ADR decides the replacement boundary; it does not claim that the new
header, loader, SDK, plugins, or tests exist. Operation ABI v2 remains the
current installed interface until one later breaking migration implements and
validates v1 and deletes v2 completely.

English architecture and OpenSpec documents are authoritative. Live delivery
and remote-gate status remain in the Issue, Project, active OpenSpec change,
and `development_tracking.md`.

## Context

The current operation DSO exports the C-linkage symbol
`register_photospider_ops_v2`, but its registrar and callbacks exchange
`std::function`, standard-library values, public C++ objects, ownership,
allocators, RTTI, and exceptions. The symbol detects one expected generation;
it does not make the data boundary C-compatible or portable between C++
toolchains.

Photospider also has two independent pure-C plugin families:

- data-definition provider ABI v3 publishes Schema/Facet/Layout definitions
  and bounded semantic definition callbacks; and
- policy plugin ABI v1 ranks immutable Host-admissible scheduler candidates.

Neither owns operation configuration, inference, dirty/forward Region
propagation, dependency construction, or execution. Calling a replacement
“operation provider v3” would silently attach those authorities to the wrong
family and make unrelated version numbers appear compatible.

The current operation loader already has valuable lifetime behavior: staged
registration, atomic publication, per-slot revision and predecessor identity,
middle-generation splice, reverse unload, and DSO leases around in-flight
callbacks. A replacement must preserve those properties without preserving
the C++ ABI.

ADR 0011 establishes a separate security-domain direction. Operator-trusted
DSOs may execute in a Host process; tenant-supplied CPU code must eventually
execute in an isolated plugin runtime. Pure C is necessary for explicit,
validatable records, but it is not process isolation. Issues #102, #103, and
#104 already own the wire protocol, supervision, and trust/resource policy.

## Decision

### Family, discovery, and supported profile

The replacement is an independent **operation-plugin ABI v1**. Its future
self-contained C11/C++17 header is
`photospider/plugin/operation_plugin_api.h`, with ABI value one and exactly two
discovery exports:

```c
uint32_t ps_operation_plugin_get_abi_version(void);
ps_operation_status_v1 ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api);
```

In C++ they are `extern "C"` and `noexcept`. Every ABI callback uses
`PS_OPERATION_CALL`: the platform C calling convention and `__cdecl` on
Windows. The Host performs only the numeric handshake before requesting the
root API.

V1 supports 8-bit bytes, 4-byte `uint32_t`, 8-byte `uint64_t`, 8-byte data and
function pointers, natural 8-byte pointer/`uint64_t` alignment, Host-process
endianness, and the matching platform C convention. Packed, over-aligned,
32-bit, foreign-endian, or foreign-calling-convention records are incompatible.

### Exact records and separately versioned suites

Exactly the 20 versioned semantic records, Diagnostic through Tile, begin with
four `uint32_t` values: `struct_size`, `struct_kind`, `struct_version`, and
`flags`. Plain fixed identity/handle, byte-view, digest, array-reference,
configuration-value, and axis-range helpers carry no record header. The root
API and suite tables instead use their own root/suite prefixes. Every suite
table begins with `struct_size`, `suite_version`, `flags`, and `reserved0`. V1
has exact size, not a minimum
prefix: unknown kind/version/flags, nonzero reserved data, short/long records,
unknown tails, wrong strides, misalignment, and arithmetic/range overflow fail
closed. A new field requires a new owning-suite version or operation ABI v2.

The root API is 96/8 bytes/alignment. It begins with `struct_size`,
`abi_version`, `flags`, and `reserved0`, followed by permanent 128-bit plugin
identity, bounded implementation version, opaque plugin context,
`query_suite`, `destroy_plugin`, and zero-required reserved pointer words. A
successful null context remains valid and still receives exactly one destroy
attempt.

Every v1 suite table is 64/8. The frozen IDs and callback inventories are:

| ID | Suite | Requirement | Ordered callbacks |
| ---: | --- | --- | --- |
| 1 | Definition | Required | operation count/get; implementation count/get |
| 2 | Configuration | Required | validate; create context; destroy context |
| 3 | Inference | Required | infer output plans |
| 4 | Region | Required | backward dirty; forward active-edge propagation |
| 5 | Dependency | Required when any implementation declares data dependence | build dependency records |
| 6 | Execution | Required | synchronous monolithic; synchronous tiled |

Required callbacks cannot be null. An execution-shape callback may be null
only when no copied implementation declares that shape. Unknown suite IDs
return `UNSUPPORTED`; a missing or malformed required/declared suite rejects
the complete candidate before publication.

The Host pre-zeroes every Host-prepared output and sets its exact prefix; the
plugin preserves that prefix and fills declared remaining fields. Sink records
carry a complete plugin-authored prefix, which the Host validates first.

All suite/record versions are one. Closed numeric domains are record kinds 1
through 20 for Diagnostic, OutputSink, ConfigurationNode, ConfigurationView,
OperationDescriptor, ImplementationDescriptor, PortDescriptor,
ValueDescriptor, FacetView, BufferView, ValueView, InputBinding, OutputPlan,
MutableOutputBinding, Invocation, RegionAtom, RegionSetView, RegionBinding,
DependencyRecord, and Tile; configuration kinds 1 Null through 8 Object; port
directions 1 Input/2 Output; intent bits 1 HP/2 RT; shape bits 1 Monolithic/2
Tiled; device kind 1 CPU; access bits 1 Read/2 Write; behavior bits 1
SideEffect/2 DataDependent; Region outcomes 1 Exact through 4 Unknown; Region
atoms 1 Whole through 4 TensorSlice; and sink channels 1 Diagnostic through 4
DependencyRecord. ValueView flag bit 1 is PayloadAvailable; all other semantic-
record flags and all root/suite flags are zero in v1. Zero is invalid/absent,
unknown values/bits fail, and a boolean is zero or one.

Callback parameter order is also closed. Plugin context comes first, followed
by exact operation/implementation/invocation/configuration identity and views,
then pointer/count/exact-stride input, demand, output, or tile arguments, with
the Host sink last. Configuration destroy receives both operation and
implementation identity even when configured context is null. The sink is
`emit(host_context,channel,records,count,stride)`. The Host exposes no
cancellation callback: it checks before entry, on sink calls, and after return,
may normalize to `CANCELLED`, and discards a late result.

There is no allocator, registry, Host service, Graph, Run, scheduler, cache,
executor, resource ledger/token, device service, filesystem, artifact,
credential, logging, thread, or dynamic-symbol callback in v1.

### Opaque identities, handles, and contexts

Plugin, operation, implementation, port, Schema, Facet, and Layout identities
are publisher-assigned nonzero permanent 128-bit definition identities. They
cannot be reused for different semantics or layout. Value, edge, allocation,
binding, site, and Region identities use the same 16-byte carrier but are
Host-minted process-local runtime identities. They expire with their logical
value, Graph revision, allocation, binding/write grant, or invocation-snapshot
owner and are neither durable nor wire identities.

The Host mints nonzero, unpredictable 128-bit generation and invocation
handles as distinct plain helper types. They are process-local correlation
handles only: not semantic identities, pointers, lookup APIs, capabilities,
durable artifact identities, resource tokens, or wire values. Invocation-
scoped callbacks receive both handles. Definition, configuration-lifetime,
root-query, and destroy callbacks are bound by the exact DSO generation lease
and applicable explicit identities and contexts; they have no fabricated
invocation handle. Stale/mismatched output is `INVALID_DESCRIPTOR` and
publishes nothing.

The plugin context and configured-operation context are opaque plugin-owned
`void *` round-trip values. The Host never dereferences or frees them. A
successful create may return null and still earns one destroy obligation; a
failed create transfers none. A context never moves between generations,
operations, or implementations.

The sink `host_context` is a separate Host-owned callback-local round-trip
token. Plugin code may only pass it back to `emit`; it never dereferences,
frees, retains, or treats that token as semantic identity.

### Descriptor catalogue, bounds, and ownership

The OpenSpec design freezes ordered field groups and these natural
size/alignment pairs for the future header:

| Layout category | Size/alignment |
| --- | --- |
| record header | 16/4 |
| identity, generation/invocation handles, immutable/mutable bytes, exact-stride array reference, configuration value, axis range | 16/8 |
| SHA-256 digest | 32/8 |
| diagnostic, output sink, configuration view, Region-set view | 48/8 |
| configuration node, facet view, tile | 64/8 |
| buffer view, input binding, Region binding | 80/8 |
| output plan, mutable output binding, invocation, Region atom, dependency record | 96/8 |
| port descriptor | 112/8 |
| operation descriptor, value view | 128/8 |
| value descriptor | 192/8 |
| implementation descriptor | 192/8 |
| root API / every suite table | 96/8 and 64/8 |

The header must assert them in C11 and C++17. It may apply required
`ps_operation_*_v1` spelling prefixes, but it may not change frozen numerics,
parameter/field order, size, alignment, ownership, or meaning without revising
this decision. The OpenSpec design freezes every helper and semantic-record
field type and byte offset, plus the 96-byte root and each 64-byte suite slot.
In particular, the 128-byte operation descriptor stores each port sequence as
one 16-byte pointer/count/stride helper at offsets 96 and 112; it has no second
copy of either count. Thus all published sizes are mechanically realizable
under the frozen 64-bit profile rather than aspirational field-group sums.

All pointer/count/stride values are borrowed for one synchronous callback or
sink emission. Null corresponds exactly to zero count; stride equals the exact
element size. The receiver validates alignment, multiplication, base/offset,
subrange, aggregate bound, and write overlap before dereference. The Host
deep-copies accepted metadata/results before return and never retains a plugin
pointer.

Operation descriptors contain permanent identity, canonical type/subtype/
display names, borrowed exact-stride port arrays, flags, and configuration-
schema identity. Type/subtype are nonempty UTF-8 without NUL or `:` and form
the unique `type:subtype` key. Port/implementation names are nonempty UTF-8
without NUL; display/exclusive key may be empty but are otherwise UTF-8 without
NUL. No name is normalized, case-folded, or truncated. Implementation
descriptors contain parent and permanent implementation identities, HP/RT
intent, monolithic/tiled shape, CPU device profile, tile/access/side-effect/
data-dependence facts, reentrancy, maximum parallelism, retained/scratch bytes,
cost, and optional exclusive key. The Host validates and publishes callback,
metadata, identity, source generation, and revision as one slot.

Configuration is a Host-owned immutable tree with null, boolean, signed
64-bit integer, binary64, UTF-8 string, bytes, array, and object nodes. It is
not YAML, `ParameterMap`, or a C++ variant. Object keys are unique and
unsigned-byte lexicographically ordered.

Value records keep Schema/Facet/Layout identities, their structural versions,
logical value revision, allocation/binding identities, and descriptor/content/
layout digests distinct. Inference, Region, and dependency receive descriptor-
only views with payload pointers cleared. Execution alone receives payload;
only explicit Host mutable-output grants permit writes.

V1 structural maxima are:

- 128 bytes per canonical name or exclusive key;
- 4 KiB per implementation version or diagnostic;
- 4,096 operations per plugin and 256 implementations per operation;
- 256 input and 256 output ports;
- rank 16, 64 facets, and 64 buffers per value;
- 4,096 configuration nodes, depth 64, and 1 MiB configuration bytes;
- 64 Region atoms per result; and
- 4,096 dependency records per invocation.

Structural excess is `TOO_COMPLEX`; available-capacity failure is
`OUT_OF_MEMORY`. Nothing is silently truncated.

### Planning and execution authority

Inference produces one complete immutable output plan per declared output port
before Host allocation. The plan fixes Schema/Facet/Layout identity,
rank/extents, buffers, sizes, and access. Execution cannot replace or widen it.

Region v1 has explicit backward dirty and forward active-edge propagation.
Outcomes are Exact, Whole, Empty, or Unknown; atoms are Whole, Empty,
ImageRect, or TensorSlice with half-open checked coordinates. A loadable plugin
cannot rely on identity fallback.

An implementation declaring data dependence must expose Dependency v1. Its
bounded records map output port/site/region facts to input edge/region facts.
The Host validates and copies all identities, rank, extents, generation, and
invocation before cache use.

Execution is synchronous and CPU-addressable only. Monolithic callbacks receive
complete immutable inputs and Host-owned mutable outputs; tiled callbacks also
receive one checked tile. A plugin writes only granted ranges and cannot mutate
descriptor, binding, extent, readiness, identity, access, or ownership facts.

V1 has no native device handle, device-resident binding, fence, asynchronous
completion, retained invocation owner, or delayed sink. Private device work is
valid only when the plugin synchronously stages output into the Host CPU
binding before return. The repository Metal example must use that pattern or
move behind a Host-private adapter before v2 deletion. Native/async execution
requires a future separately versioned suite or ABI.

### Host-owned output and exact lifetime

V1 has no allocator callback. Definition strings are copied before return;
execution writes Host-owned buffers; and inference, Region, dependency, and
diagnostics use one 48-byte callback-local Host output sink. Its single emit
function accepts only callback-appropriate closed channels and exact-stride
records. The Host synchronously validates/copies them. The first sink failure
is sticky even if the plugin ignores it or later returns success.

The Host destroys Host memory. The plugin destroys plugin memory. Every
successfully created root or configured context gets exactly one destroy
attempt after dependent calls finish and while the exact generation/DSO lease
is live. Failure is recorded, not retried.

The status type is `uint32_t` with exact values:

| Value | Status |
| ---: | --- |
| 0 | `OK` |
| 1 | `INVALID_ARGUMENT` |
| 2 | `OUT_OF_MEMORY` |
| 3 | `UNSUPPORTED` |
| 4 | `INVALID_DESCRIPTOR` |
| 5 | `TOO_COMPLEX` |
| 6 | `CANCELLED` |
| 7 | `FAILED_PRECONDITION` |
| 8 | `INTERNAL_ERROR` |

Unknown status is an ABI fault and maps to Host internal failure. One non-OK
callback may emit one copied UTF-8 diagnostic up to 4 KiB; text changes no
status or authority. No exception, unwind, `longjmp`, signal-recovery object,
or language-runtime value crosses the DSO. C++ wrappers map `std::bad_alloc` to
`OUT_OF_MEMORY`, invalid caller input to `INVALID_ARGUMENT`, and all other
catchable failures to `INTERNAL_ERROR` inside the DSO.

### Atomic publication and generation-safe retirement

The future loader performs numeric, root, suite, descriptor, callback, bound,
and identity validation into one shadow generation. It assigns a Host
generation handle and atomically publishes only after complete validation.
Plugins never mutate `OpRegistry` directly.

Each executable slot owns its complete callback set, copied descriptor,
identity, source generation, revision, and predecessor. Direct Host mutation
serializes with publication. Middle-generation retirement splices only its
owned predecessors, and reverse unload never restores unmapped code.

Every callback/context keeps its exact DSO generation leased through sink
validation, status normalization, and destroy. Retirement removes publication,
waits for leases, destroys in reverse creation/load order, and unmaps last.
No registry, publication, scheduler, or execution lock is held during plugin
code.

If an in-process callback never returns, its invocation, write grants, context,
generation, and DSO may remain live forever. Cancellation can reject its result
but cannot fabricate return, destroy, quiescence, or safe unload.

### Trust boundary and follow-up ownership

Operation-plugin ABI v1 is an operator-trusted in-process compatibility and
validation boundary. It cannot prevent memory corruption, syscalls, secret
access, threads, crashes, hangs, OOM, forged callbacks, or Host corruption.

Tenant-untrusted CPU code never uses pointer-bearing ABI records as IPC:

- Issue #102 owns pointer-free shared-memory/FD invocation wire records,
  serialization, and exact offset/range/ownership validation;
- Issue #103 owns authenticated `PluginRuntimeSupervisor` lifecycle,
  heartbeat/deadline, crash/hang/OOM/bad-output containment, restart, and reap;
- Issue #104 owns package allowlist/signature, sandbox/capability, and
  enforceable resource policy.

This ADR preselects none of their frame, handle, authentication, or policy
formats. Cross-process GPU/native-handle work remains a later decision.

### One complete breaking migration

Issue #101 is documentation and specification only. The later implementation
must add v1 header/SDK/loader/conformance tests, migrate all repository
operations and installed consumers, and then remove operation v2 in the same
migration release. Deletion includes `register_photospider_ops_v2`,
`OperationPluginRegistrar`, the public C++ callback contracts, v2-only SDK/
runtime package surface, fixtures, package assertions, and stale docs.

There is no wrapper, alias, dual loader, forwarding header, v2-to-v1 adapter,
missing-tail interpretation, or runtime fallback. Independent C11/C++17
consumers plus suite, rejection, ownership, replacement, middle-unload, and
in-flight-lease tests gate deletion. Rollback is a revert of the complete
migration, not a v2 mode.

That later migration is not an Issue #101 closure or decision-archive gate.
The decision change may be archived and Issue #101 may close once its bilingual
artifacts pass local validation, fresh independent diff review, authorized
exact-head PR Integration, fresh Codex exact-head review with findings
adjudicated, zero unresolved review threads, and Issue/Project administration.
At that point v2 may still be current and v1 target-only.

## Consequences

- C and C++ extension authors get one explicit operation contract without one
  shared C++ allocator/ABI/runtime requirement.
- Exact records and Host sinks make validation and ownership mechanical, at
  the cost of copying and explicit version bumps.
- The first ABI intentionally excludes native/async operation execution, so it
  has no hidden fence, completion, or device-owner contract.
- The existing strong loader/lifetime behavior remains a required property,
  but compatibility wrappers and dual registries do not.
- Pure C is never used as evidence that in-process tenant code is safe.

## Rejected Alternatives

- **Rename the target “provider v3”.** Rejected because provider v3 already
  names the implemented definition-only family.
- **Extend operation v2 in place.** Rejected because C++ values and ownership
  cannot become pure C through a symbol-compatible tail.
- **Reuse policy v1 or data-provider records.** Rejected because their
  authority, lifecycle, and versioning are deliberately narrower.
- **Accept minimum-size prefixes and unknown tails.** Rejected because they
  permit silent layout/semantic disagreement.
- **Add an allocator callback.** Rejected because Host sinks and output grants
  avoid cross-DSO allocation ownership entirely.
- **Include native GPU or async completion in v1.** Rejected because their
  handles, fences, completion owners, cancellation, and retirement semantics
  need an independent decision.
- **Treat pure C as an isolation boundary.** Rejected because native code in
  one process retains ambient process authority.
- **Ship v1 beside v2 indefinitely.** Rejected because dual publication and
  restoration paths preserve ambiguity and migration residue.

## References

- [Plugin ABI](../kernel-architecture/Plugin-ABI.md)
- [Compute Boundaries](../kernel-architecture/Compute-Boundaries.md)
- [Kernel Evolution](../roadmap/Kernel-Evolution.md)
- [ADR 0003](0003-process-owned-execution-resources.md)
- [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
- [ADR 0011](0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md)
- [GitHub Issue #101](https://github.com/kevin-zf1123/photospider/issues/101)
