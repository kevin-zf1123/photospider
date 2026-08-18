# Plugin ABI

Photospider has three independently versioned installed extension contracts:
operation ABI v1, data-definition provider ABI v3, and policy ABI v1. Each is a
pure-C DSO boundary with an optional header-only C++ authoring layer. The Host
never passes a C++ standard-library object, exception, allocator, registry,
owner, or process-local runtime object across these boundaries.

English architecture documents and installed headers are authoritative. This
document describes the implemented boundary. Historical registration surfaces
have been removed without a dual loader, forwarding header, adapter shim,
alias, or missing-tail fallback.

## Operation Plugin ABI v1

### Installed surface

The complete installed operation authoring surface is:

- `include/photospider/plugin/operation_plugin_api.h`: self-contained C11 and
  C++17 pure-C ABI;
- `include/photospider/plugin/operation_plugin.hpp`: header-only C++17 helper
  that constructs the same records and fences plugin-local exceptions;
- CMake component and target `operation_plugin_sdk` /
  `Photospider::operation_plugin_sdk`;
- optional Value/runtime target `Photospider::operation_runtime`; and
- optional OpenCV adapter component and target `operation_opencv` /
  `Photospider::operation_opencv`.

`operation_plugin_sdk` has no external package or link dependency. A pure-C or
header-only plugin needs only this target. A plugin that directly uses public
Value/runtime helpers links `operation_runtime` explicitly. The OpenCV adapter
discovers only OpenCV `core`; algorithm-specific modules remain the plugin's
responsibility.

Every operation DSO exports exactly:

```c
uint32_t ps_operation_plugin_get_abi_version(void);

ps_operation_status_v1 ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api);
```

Numeric discovery is side-effect free and must return exactly
`PS_OPERATION_PLUGIN_ABI_VERSION`. Root discovery fills a Host-prepared exact
96-byte v1 table. The root exposes a permanent plugin identity, bounded
implementation-version bytes, one opaque round-trip generation context, suite
query, and exactly-once generation destruction. Reserved fields must be zero.

The Host requests exact 64-byte version-one suites:

| Suite | Responsibility |
| --- | --- |
| Definition | Enumerate immutable operations and implementations. |
| Configuration | Validate configuration and create/destroy configured context. |
| Inference | Emit immutable Host-validated output plans. |
| Region | Propagate demanded/affected Regions. |
| Dependency | Emit bounded execution dependencies when declared. |
| Execution | Execute trusted monolithic or tiled callbacks. |

No table uses minimum-size compatibility. Root, suite, semantic-record,
stride, version, flags, and reserved fields must match the installed v1
contract exactly. Unknown kinds, nonzero tails, oversized counts, invalid
pointer/count pairs, or unsupported enum values fail before publication or
invocation.

### Exact record catalogue

All semantic records begin with the 16-byte
`ps_operation_record_header_v1`, use natural 8-byte alignment, and carry exact
sizes. The catalogue contains 30 record kinds:

| Kinds | Records | Size/alignment |
| --- | --- | --- |
| 1–7 | Diagnostic, OutputSink, ConfigurationNode/View, Operation/Implementation/PortDescriptor | 48, 48, 64, 48, 128, 192, 112 / 8 |
| 8 | `ps_operation_value_descriptor_v1` | 224/8 |
| 9–12 | FacetView, BufferView, ValueView, InputBinding | 64, 80, 128, 96 / 8 |
| 13–15 | OutputPlan, MutableOutputBinding, Invocation | 112, 128, 96 / 8 |
| 16–20 | RegionAtom, RegionSetView, RegionBinding, DependencyRecord, Tile | 96, 48, 80, 96, 64 / 8 |
| 21 | `ps_operation_dense_tensor_descriptor_v1` | 96/8 |
| 22 | `ps_operation_strided_layout_v1` | 64/8 |
| 23 | `ps_operation_image_facet_v1` | 160/8 |
| 24–25 | `ps_operation_channel_v1`, `ps_operation_channel_group_v1` | 48, 64 / 8 |
| 26–28 | ChannelSampleDomain, SampleDomainFacet, ColorFacet | 64, 80, 64 / 8 |
| 29–30 | OutputBufferPlan, OutputGrantSpan | 64, 64 / 8 |

Helper records have equally frozen layouts: identity, generation, invocation
identity, byte/mutable-byte views, array references, axis ranges, and SHA-256
digests. Every array reference has an exact element stride, documented bound,
alignment, and pointer/count relationship. Every callback-local byte or record
view is borrowed only for that call unless the record explicitly documents
DSO-lifetime immutable metadata.

The ABI bounds rank at 16; facets and buffers at 64; channels, groups, and
group members at 4096; total group memberships at 65536; configuration nodes,
depth, and bytes at 4096, 64, and 1 MiB; Region atoms at 64; dependency rows at
4096; and output/grant spans at 1,048,576. Names, diagnostics, plugin versions,
operations, implementations, and ports also have installed bounds. Checked
addition, multiplication, offset, extent, signed-window, and stride arithmetic
is mandatory at every Host fence.

### DenseImage and generic Value projection

`ps_operation_value_descriptor_v1` preserves the exact Schema, Facet, and
Layout identities, versions, and digests. An ordinary DenseImage points to:

- a `DenseTensorDescriptor` containing rank, exact `uint64_t` extents, element
  semantics, storage encoding, and optional quantization block shape and
  binary32 scales;
- an `ImageFacet` containing explicit x/y/optional-channel axes, signed data
  and optional display windows, channels and groups, optional per-channel
  sample-domain overrides, and optional SampleDomain and Color facets; and
- a physical `StridedLayout` containing rank, buffer index, byte offset,
  signed byte strides, and the exact storage span.

Channels and groups use stable 64-bit identities. Their diagnostic names are
bounded byte views. Group membership and per-channel overrides are
exact-stride arrays. Optional-record presence bits are closed; an absent
optional record has C-semantic zero storage. ABI enums are fixed-width and
one-based, with zero reserved for absent or invalid.

`ValueView` combines one validated descriptor, bounded facet and buffer rows,
and content/storage identity. `InputBinding` adds permanent port identity,
dense slot, optional edge identity, exact logical Region, and connected or
disconnected state. Disconnected slots remain explicit; compaction is never
allowed to change port identity.

An ordinary Host-built DenseImage that predates retained operation metadata is
projected with the frozen Host-published DenseTensor Schema, ImageFacet, and
Strided Layout identities and version-one compatibility spelling. The input
port validates those publisher facts; it never supplies them. A custom or
provider identity declared only by the consumer is rejected before trusted
callback entry or supervised process creation in both monolithic and tiled
routes.

### Output planning and Host-owned grants

Inference emits `OutputPlan` records through a Host sink. A plan names the
output port and a Host-minted opaque plan identity, references one complete
value descriptor and full logical Region, and contains exact-stride
`OutputBufferPlan` rows. Each row freezes buffer index, access, offset, exact
size, and alignment. The Host validates and deep-copies the complete plan
before allocating anything.

Execution receives `MutableOutputBinding` records produced only by the Host.
Each binding echoes the accepted plan, binding identity, and callback-scoped
grant identity. Its `OutputGrantSpan` rows contain checked allocation offsets,
sizes, alignments, access masks, and mutable CPU byte views. A plugin may write
only bytes covered by those spans and the current tile/Region.

The plugin receives no allocator, seal callback, `ValueBuilder`, or
transferable owner. `OK` plus valid sink/output state retires grants and permits
one Host seal. A non-OK status, first sink failure, exception, cancellation,
malformed echo, out-of-plan write, missing retirement, or late result fails the
whole binding closed. The Host publishes no partial output.

### Region, dependency, and diagnostic sinks

Region records carry bounded exact atoms and preserve signed image windows or
generic tensor selections. Backward propagation maps demanded output Regions
to input/edge identities; forward propagation maps changed input Regions to
affected output identities. Exact, whole, empty, and unsupported outcomes are
closed values. The Host canonicalizes, validates, deep-copies, and bounds every
emitted row before planning uses it.

Dependency records name the implementation, direction, input/output identity,
and Region relation without exposing a scheduler, queue, Run, worker, or
resource grant. Implementations whose output depends on data must expose the
Dependency suite; omission is a registration failure.

Every callback may emit bounded diagnostics through a Host-owned sink. The
first sink failure is sticky and authoritative. Messages are copied
synchronously into Host-owned storage. A plugin-local C++ helper catches
`std::bad_alloc`, `std::invalid_argument`, other standard exceptions, and
unknown exceptions inside the DSO and returns a frozen status; no exception
object crosses the ABI.

### Registration and publication transaction

The process `PluginManager` owns one operation registry and performs a single
transaction for each candidate:

1. resolve and authorize the exact opened object under the native trust policy;
2. call only numeric discovery until ABI v1 is confirmed;
3. prepare the exact root and required suites in Host storage;
4. enumerate bounded operations and implementations;
5. validate every nested record, identity, relationship, callback, execution
   mode, and runtime-package identity;
6. deep-copy all definition metadata and prepare callback/context owners;
7. install the combined sealed-object/native DSO lease; and
8. atomically publish all private `OpRegistry` callbacks with no throwing work
   under the visible-registry lock.

Any failure before step 8 leaves the visible registry unchanged. Definition
identities and `(type, subtype)` keys are unique. Published callbacks capture
the exact immutable generation, operation and implementation identities,
suite callback, and DSO lease. The source-private C++ registry model is a Host
projection and is never an installed ABI.

Each successful publication mints a revision and retains its predecessor.
Replacing the active definition shadows but does not destroy the older
generation while snapshots remain. Unloading a shadowed middle generation
splices its predecessor into the newer snapshot. Unload-all follows reverse
successful-publication order. The registry lock is released before callbacks,
context destruction, generation destruction, or DSO close.

### Context and DSO lifetime

A resolved implementation may create one configured context from a validated
configuration snapshot. `OK` with null is a valid stateless context. Each
successful create receives exactly one matching destroy with the same
operation/implementation identities and context; failed create receives none.

The complete generation receives one `destroy_plugin` attempt only after all
registry definitions, configured contexts, output plans/results, callback
snapshots, and in-flight invocations release. Destruction runs while the exact
DSO lease remains live and is never retried. A destroy failure becomes bounded
Host-owned diagnostics and does not roll back an already committed replacement.
This lease discipline prevents callbacks, metadata, or contexts from surviving
their code image.

### Trusted and supervised CPU routes

An implementation declares exactly one execution mode:

- `TRUSTED_IN_PROCESS`: the Host invokes the pure-C suite callback under the
  generation lease and catches foreign unwinds while the DSO is still mapped;
- `SUPERVISED_PROCESS`: the descriptor contains a nonzero opaque signed
  runtime-package identity, and the Host resolves the matching installed
  private `PluginInvocationExecutor` route.

Supervised mode never contains or serializes a path, PID, descriptor, mapping
address, callback, context, pointer, native handle, or in-process generation
owner. A missing/mismatched route fails before direct callback entry and has no
trusted fallback.

The independently versioned isolation protocol is version 2. Requests carry
canonical bounded copies of operation/implementation identity, configuration,
input descriptor/facet/layout/Region facts, immutable output plans, and
directional shared-memory capability indices. Responses carry status, bounded
diagnostics, plan echoes, Region/dependency rows where applicable, and
written-range facts. The runtime validates framing and records before callback
entry. The Host decodes into fresh bounded objects and revalidates every result
against the immutable request, current invocation/resource identity, output
plan, and Host grant before seal/publication.

Unknown tails, duplicate rows, hostile count/stride/reserved values, forged
capability indices, out-of-plan ranges, lossy metadata, and pointer-bearing
records fail closed. Existing authentication, deadline, fresh-process,
resource-ledger settlement, fault classification, child reaping, and recovery
semantics remain owned by `PluginRuntimeSupervisor` and
`PluginInvocationExecutor`.

Darwin deliberately rejects exact-object supervised runtime construction
before capability materialization or child creation. Portable compile/layout
and route-before-process failure tests run there; native exact-object DSO and
positive supervised execution remain Linux integration gates.

## Native Plugin Trust Admission

Dynamic code is admitted before plugin discovery. The loader opens and
authorizes the exact object it later maps, verifies the configured trust
bundle and signature/digest policy, rejects writable or replaced candidates,
and carries the sealed-object identity into the lifetime lease. A trusted
in-process plugin remains native code with process authority; pure C provides
binary compatibility and validation, not a sandbox.

Operation, policy, and data-definition contracts share this admission model
but keep separate ABI numbers, entry symbols, roots, suites, registries, and
authorities. Failure in one family never enables another family's entry point.

## Data-Definition Provider ABI v3

Data-definition providers publish immutable Schema, Facet, and Layout
definition bundles through:

```c
uint32_t ps_data_provider_get_abi_version(void);
ps_data_provider_status_v3 ps_data_provider_get_api_v3(
    ps_data_provider_api_v3 *api);
```

The installed dependency-neutral component is `data_provider_sdk` and the
target is `Photospider::data_provider_sdk`. Provider callbacks validate and
observe pure properties, DataSpec/Region relationships, canonical content,
and generation lifetime. They receive no allocation, conversion, execution,
device, Graph, registry mutation, or codec authority. Host-side C++ registry
users link `Photospider::operation_runtime` separately.

OpenEXR deep support is an optional provider/adapter family. Only its explicit
component discovers OpenEXR 3; neutral SDK and runtime components never do.
Unknown provider bytes remain byte-preserving when the provider is absent, and
generation-owner leases keep provider code live during retained traversal.

## Policy Plugin ABI v1

Policy DSOs export exactly:

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *api);
```

The installed dependency-neutral component is `policy_sdk` and the target is
`Photospider::policy_sdk`. Callbacks receive bounded immutable candidate and
ranking snapshots and return one candidate identity or abstention. They
receive no worker, queue, resource grant, executor, Run, Graph, allocator,
completion route, or lifecycle authority.

Policy loading is staged and atomically published. Bindings and in-flight
ranking snapshots retain the exact DSO generation. The first callback fault is
stable, diagnostics are Host-owned, and unload/destruction happens outside the
registry lock after all holders release.

## Compatibility Rules

- ABI families are versioned independently; an operation change does not
  renumber provider, policy, IPC, worker, or durable formats.
- Exact current sizes are required. No smaller prefix, missing tail, alias,
  wrapper, dual loader, or forwarding compatibility is accepted.
- Entry points, calling conventions, fixed-width enums, counts, bounds,
  alignments, and reserved-zero fields are part of the binary contract.
- Plugins must be rebuilt against the matching installed SDK when any owning
  ABI version changes.
- Process-local pointers may exist only during an authorized in-process
  callback. They never cross the isolation wire or become durable identity.
- DI-4 still owns final migration of public Host, IPC/worker, durable, codec,
  CLI, and remaining `ImageBuffer` surfaces outside the operation/isolation
  boundary.

## Implementation and Validation Entry Points

Primary implementation entry points:

- `include/photospider/plugin/operation_plugin_api.h`
- `include/photospider/plugin/operation_plugin.hpp`
- `src/lib/plugin/plugin_loader.cpp`
- `src/lib/plugin/operation_host_adapter.hpp`
- `src/lib/plugin/operation_host_adapter.cpp`
- `src/lib/plugin/operation_runtime_router.hpp`
- `src/lib/plugin/operation_runtime_router.cpp`
- `src/lib/execution/isolation/isolated_cpu_invocation_protocol.hpp`
- `src/lib/execution/isolation/isolated_cpu_invocation_protocol.cpp`
- `src/lib/execution/device/plugin_runtime_supervisor.hpp`

Durable validation entry points include independent installed C11/C++17
consumers, exact layout assertions, hostile root/suite/record fixtures,
repository and OpenCV provider behavior, output-plan/grant/Region/dependency
tests, replacement and middle-generation lifecycle tests, in-flight DSO and
destroy-once tests, isolation protocol-v2 round trips, hostile response tests,
route-before-process fail-closed behavior, and supported-platform supervised
runtime tests. Migration residue scans remain local development checks and are
not registered as CTest or CI behavior tests.
