# Plugin ABI

Photospider supports operation plugins and policy plugins. Operation plugins
extend the process-owned `OpRegistry` through a Host-provided registrar and
remain a provisional C++ ABI. Policy plugins rank immutable Host-admissible
candidates through a versioned pure C ABI; they own no worker, queue, device,
resource, Run, or Graph capability. The installable authoring contracts live
only under `include/photospider/plugin/` and
`include/photospider/policy/policy_plugin_api.h`.

## Operation Plugin ABI

An operation plugin exports a versioned registrar entry:

```cpp
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void
register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar);
```

The loader opens the candidate eagerly and locally (`RTLD_NOW | RTLD_LOCAL` on
POSIX), resolves only this exact symbol, and invokes it with a borrowed
registrar. The registrar writes into a host-side shadow transaction; plugins
never receive `OpRegistry` or any other mutable backend owner.

The C-linkage symbol protects only exact entrypoint lookup. The registrar table
and operation contracts still carry public C++ values, `std::function`,
standard-library containers, shared ownership, and exceptions across the DSO
boundary. A loadable operation plugin therefore requires the matching
Photospider SDK and a compatible compiler, standard library, C++ ABI,
allocator/runtime, exception model, and RTTI configuration. Version two is
provisional: it promises neither pure C consumption, cross-toolchain binary
compatibility, nor long-term ABI stability.

Neither the v1 `register_photospider_ops_v1` entry nor the old no-argument
`register_photospider_ops()` entry is a supported compatibility ABI. A DSO that
exports only either old symbol is rejected with no callback publication. The
version bump is required because v2 callbacks use different node, parameter,
input, output, ROI, and dependency types.

Supported operation registrations include:

| Registration | Meaning |
| --- | --- |
| HP monolithic | Full-image HP implementation. |
| HP tiled | Tile-based HP implementation. |
| RT tiled | Tile-based RT implementation. |
| Dirty ROI propagator | Backward ROI propagation. |
| Forward ROI propagator | Downstream ROI projection. |
| Dependency LUT builder | Data-dependent spatial dependency map. |
| Device implementation | CPU, Metal, CUDA, or another supported public `Device` capability. |

The canonical registry identity is `type:subtype`. Both segments must be
non-empty and neither may contain the reserved `:` separator, otherwise two
different pairs could collide. The public C++ registrar helpers additionally
reject embedded NUL bytes before calling `.c_str()`, preventing raw-ABI
truncation from changing the identity. Host raw callbacks independently
validate the visible C-string segments. Every rejection occurs inside the
candidate shadow transaction and publishes no callback, source, or handle.
Every registration also requires a non-empty callable. Typed C++ helpers reject
an empty `std::function` before entering the raw ABI, and host raw callbacks
repeat that check rather than trusting the plugin wrapper. The loader reports
either violation as an `InvalidParameter` candidate diagnostic with zero shadow
publication.

The callback boundary is host-independent:

- `NodeView` exposes borrowed identity strings plus a deep-owned effective
  `ParameterValue` tree. It never exposes `Node`, `YAML::Node`, cache state, or
  a graph/runtime owner.
- `OperationInputView` and `OperationTileInputView` borrow immutable image,
  named-data, and spatial snapshots only for the callback duration.
- `OperationOutput` owns its image descriptor, named parameter values, spatial
  metadata, and debug metadata. Named values are copied or moved directly
  between `ParameterMap` storage and the private `NodeOutput`; the host
  validates the complete output before attaching the private DSO lease.
- `RoiContext` exposes ordered `InputEdgeView` topology snapshots; forward ROI
  callbacks receive the active edge, and dependency builders return an owned
  `DependencyLutSnapshot` that the host validates before caching.
- `ParameterTypeError` reports an explicit `ParameterValue` alternative
  mismatch inside plugin code. Document conversion has already completed before
  Graph publication; callback preparation can fail only while copying owned
  snapshots or allocating storage.

Host snapshot preparation before callback entry and output validation after a
successful return remain outside the plugin exception fence and preserve their
host-owned types. The actual plugin invocation retains an explicit DSO lease
and normalizes every plugin-origin exception before that lease can be released:
plugin
`std::bad_alloc` becomes a fresh host `std::bad_alloc`; plugin `GraphError`
becomes a host copy with the same fixed-width code and message;
`std::invalid_argument` maps to `GraphErrc::InvalidParameter`; and other
standard or unknown failures map to `GraphErrc::ComputeError`. The plugin
exception object is inspected and destroyed under the lease, so its identity
and DSO-defined dynamic type never reach the host. `GraphErrc` has the fixed
`uint32_t` representation and explicit values `1..9`.

## Operation SDK Targets and Linkage

Operation plugins do not link the broad static `photospider` product to reach
registry symbols. An ordinary plugin requests the `operation_sdk` package
component and links only `Photospider::operation_sdk`. That interface target
carries the installed headers and transitively links
`Photospider::operation_runtime`, whose static archive implements public
image-buffer factories without linking back to the SDK or requiring an
external package.

OpenCV is an explicit opt-in. A plugin that uses
`photospider/plugin/opencv_adapter.hpp` additionally requests and links
`Photospider::operation_opencv`. That target owns the adapter implementation
and discovers only OpenCV `core`; it does not add `imgproc`, `imgcodecs`, or
`videoio`. A concrete plugin declares such additional OpenCV modules itself
when its own algorithm requires them.

The generic `ImageBuffer::context` remains backend-specific and opaque. The
public OpenCV adapter interprets only `Device::CPU` descriptors with non-null
`data`; it rejects a non-CPU or context-only descriptor instead of casting an
arbitrary backend resource to an OpenCV object. Host dirty staging deep-copies
CPU data, immutably shares a non-CPU descriptor until a tiled write requires
CPU staging, and treats monolithic output as full replacement. Downsample
planning preserves a non-CPU HP descriptor and its full extent as an explicit
backend-preserving passthrough; it does not fabricate reduced pixels or a false
reduced extent. Cache and metrics pixel inspection skips descriptors for which
no matching device adapter exists.

This split supports the static-host direction:

- The static Photospider process owns one `OpRegistry` and one operation
  `PluginManager`, shared by every embedded Host.
- Dynamic operation plugins receive registration callbacks from the host, so
  registry mutation stays in that process-owned instance.
- `Photospider::operation_runtime` contains value-factory implementation only;
  it contains no registry, loader, Graph, policy, execution, or compute state.
- Plugin callback objects and plugin-instantiated return-value internals may
  still point into plugin code, so the process owner and copied value leases
  retain libraries until all such state has been destroyed.

Symbol visibility rules:

- Operation registrar entries use `PHOTOSPIDER_OPERATION_PLUGIN_EXPORT`, and
  the loader treats only `register_photospider_ops_v2` as an entry point.
- Operation targets define `PHOTOSPIDER_PLUGIN_BUILD`, which exports the
  registrar on Windows and selects default visibility on supported POSIX
  toolchains.
- The loader resolves the exact versioned symbol name.
- This remains a C++ ABI boundary because callbacks use `std::function`,
  standard-library containers, and public C++ value types. Compiler, standard
  library, exception model, RTTI settings, and Photospider SDK compatibility
  are version-sensitive. No cross-toolchain or pure C compatibility is
  promised by the current ABI.

Current operation plugins that are intended to be loadable through
`plugin_dirs` must also register explicit dirty and forward ROI propagators. The
registry still provides an identity compatibility fallback, but that
fallback is not a complete plugin contract. Pointwise image operations can
register pass-through ROI functions; side-effecting monolithic operations must
document their side-effect semantics and still register explicit propagators
that describe upstream demand and downstream affected-region metadata.

The standard example plugins follow this rule:

| Plugin op | Execution shape | ROI contract |
| --- | --- | --- |
| `image_process:invert` | HP monolithic pointwise image transform | Explicit pass-through dirty and forward ROI. |
| `image_process:threshold` | HP monolithic pointwise image transform | Explicit pass-through dirty and forward ROI. |
| `io:save` | HP monolithic side-effect sink | Explicit pass-through planning metadata; execution rewrites the full file. |
| `image_generator:perlin_noise_metal` | HP monolithic Metal generator | Explicit generator-local pass-through ROI metadata; tiled Metal execution is not enabled. |

## Operation Plugin Load Transaction

Loading one operation plugin is a strong transaction over all observable
loader state. Before invoking `register_photospider_ops_v2`, the loader creates
staged copies of the target `OpRegistry`, operation-source map, structured load
result, and retained-handle map. The host-provided registrar points at the
staged registry, so plugin callbacks never mutate the active registry during
registration. Registration capture, previous-source calculation, restoration
snapshots, result aggregation, and handle insertion also mutate only staged
state.

The process manager serializes the complete registry snapshot-to-publication
interval. Direct registry registration therefore cannot land between a
transactional copy and its final swap and then be lost. Registry reads return
independent callback snapshots rather than borrowed pointers; candidate filters
run after the registry lock has been released. A direct mutation that starts
while a registrar is staging waits for publication, then applies to the newly
published registry state; both operations complete without overwriting the
direct update or deadlocking.

Ownership is tracked below the operation-key level. Every successful write to a
legacy callback, metadata, HP/RT callback, propagation callback, dependency
builder, aggregate dependency flag, or device implementation element receives a
stable revision token. Plugin registration capture records only the final tokens
that the registrar actually wrote and prunes predecessor snapshots to those
replaced slots; append-only device predecessors remain live instead of being
duplicated in restoration state. A same-key direct mutation after publication
receives new tokens for its changed slots. Source inspection reports `mixed`
while direct and plugin-owned slots coexist instead of attributing the complete
key to the plugin.

Live device implementation elements use stable immutable owners rather than
storing `std::function` targets directly in the growing registry vector. A new
monolithic or tiled device value, including its plugin lease wrapper, is fully
constructed before the registry lock is acquired. Under the lock, registration
grows and publishes only shared owners and the parallel revision tokens.
Readers retain a coherent owner list under the lock and copy callback targets
only after releasing it. The legacy HP compatibility slot for a first CPU
candidate is a forwarding bridge that retains the same stable owner instead of
copying the original target. During mixed plugin/direct unload, plugin-owned
owners are swapped into preallocated retirement slots and later direct owners
are swapped only into gaps already made empty; tail removal therefore destroys
only empty owners. No existing or removed device callback target is copied,
moved, destroyed, or allowed to release its final library lease under the
registry lock. Failure to construct the new stable value or compatibility
bridge occurs before key, callback, or ownership publication and leaves the
registry unchanged.

Stable ownership is not an execution mutex. The first CPU device value and its
HP compatibility bridge retain the same callback target, and executor or
reader snapshots may retain that same logical target as well. These paths may
invoke it concurrently. The callback provider must therefore make the target
reentrant or synchronize its shared mutable state. The registry serializes only
ownership mutation, coherent snapshot capture, publication, and unload; it never
holds its state lock to serialize callback execution. Callers must not infer
single-threaded execution from a shared operation key, device, or intent.

Repository-owned CPU OpenCV providers implement that contract with immutable
inputs, callback-local or task-owned `cv::Mat` state, and no process-wide outer
operation mutex. The optional built-in provider fixes OpenCV internal CPU
threading at one before callback publication, so execution grants own the outer
parallelism. Its provider-local fence converts OpenCV resource exhaustion to a
fresh `std::bad_alloc` and all other `cv::Exception` values to host-owned
`GraphError`. Building with
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` omits those slots while the
registry and public v2 registrar remain usable by another provider.
Provider-local synchronization is still required for actual shared backend
state: the Metal Perlin DSO mutex protects only its shared Metal lifecycle.
[ADR 0004](../adr/0004-opencv-cpu-operations-are-reentrant-provider-work.md)
records the decision and its accounting limits.

Direct replacement uses the same retirement discipline outside manager-driven
unload. A replacement callback is prepared before locking and swapped with the
active slot; the displaced callable remains in the parameter-local retirement
value until the registry guard has exited. Whole-key unregister extracts the
legacy, metadata, implementation, and ownership map nodes together, then
destroys the extracted values outside the guard. Device implementation values
and their revision vector therefore remain parallel, and a direct device
registration after whole-key unregister cannot inherit a stale plugin token.
Manager-driven v2 registration applies those same slot semantics to the
optional OpenCV provider: a DSO may own every active resize slot, execute
without OpenCV through public `ImageBuffer` values, and restore the captured
OpenCV predecessor on unload.

The transaction has three outcomes:

- If the registrar throws `std::bad_alloc`, the plugin exception is inspected
  and destroyed under the candidate lease and a fresh host `std::bad_alloc`
  propagates. If a later host staging step throws `std::bad_alloc`, that already
  host-owned exception propagates directly. Registry callbacks, sources,
  diagnostics, and retained handles remain byte-for-byte logically equivalent
  to their pre-candidate state.
- If the registrar throws another standard exception, the loader commits only
  the structured diagnostic for that candidate after destroying the plugin
  exception under the lease. No callback, source, restoration snapshot, or
  handle becomes active.
- After every staging allocation succeeds, commit first swaps the candidate
  library into the retained-handle map, then swaps source/result state, and
  publishes the full registry last. These operations are required to be
  `noexcept`; there is no allocating rollback path.

The candidate library is the transaction object's first-owned member and is
therefore destroyed last. On any failed registration, staged registry callback
objects and their captured plugin-owned state are destroyed before the dynamic
library is unmapped. On success, the retained handle is visible before the
registry containing plugin callbacks becomes active. These two ordering rules
prevent both failure-path destructor calls into an unloaded library and
success-path callbacks without a live handle.

## Operation Plugin Library Lifetime

Operation callbacks registered by a plugin may point to code or callable
objects inside that plugin's dynamic library. `PluginManager::process_instance`
is the unique process-lifetime owner for operation-plugin source labels,
handles, restoration snapshots, and successful-load ordering. Every Kernel and
embedded Host reaches this same owner. Destroying a Host or Kernel never unloads
operation plugins; explicit unload through any Host changes registry/source
visibility for every Host.

A successful load records the absolute plugin path, operation keys registered
or replaced through the host-provided registrar, the exact per-slot revisions
owned by that plugin, pruned previous registry/source state, preallocated empty
callback-retirement slots, and a retained RAII library handle. It also records a
monotonic successful-load sequence. The production low-level loader requires an
unforgeable process-owner token, so a caller cannot publish into the global
registry with a second source/handle/restoration map. `PluginManager` is the
only production loading surface; there is no legacy wrapper that accepts a
caller source map or copies manager state after a completed load transaction.

Every registrar callback is wrapped with a shared dynamic-library lease. A
resolved callback snapshot therefore remains callable after explicit global
unload removes its registry entry. Monolithic callback results also attach the
same lease to the host-private `NodeOutput` after public-value conversion. The
lease is the first-declared and last-destroyed
member. Copy construction retains it before copying payload state; move
construction transfers the complete state through a no-throw swap. Copy and move
assignment first stage a complete replacement, swap it into place, and let the
temporary retire the old image/ParameterValue/spatial/debug state before releasing
the old lease. A failed copy leaves the destination unchanged. Consequently,
plugin-defined image/context deleters remain mapped even when a cached output is
copied, moved, or overwritten after explicit global unload. These leases contain
no reference back to the manager or registry and therefore form no ownership
cycle.

Unload consumes only preallocated keys, ownership tokens, snapshots, and
retirement slots. For each scalar or device element, it compares the active
revision with the plugin's publication token. Matching slots are restored from
their pruned predecessor or swapped into empty retirement storage; later direct
slots have different tokens and remain active. Device compaction swaps stable
owners through already-empty gaps and shrinks only an empty-owner tail, so it
does not rely on any `std::function` move implementation. Empty registry values
can then be erased without destroying plugin callback state under the registry
lock. The retired plugin record is destroyed after that lock is released. There
is no temporary key collection, callback copy, callable comparison, or
allocating rollback, so `unload_all_plugins()` remains a `noexcept` cleanup path
even when global allocation is failing.

The process owner itself is intentionally not destroyed at static teardown;
explicit unload defines plugin cleanup semantics and avoids static-destruction
ordering against `OpRegistry`.

`unload_by_plugin_path()` first tries the exact absolute key recorded by the
successful load. That lookup and the following cleanup are allocation-free, so
callers that retain the reported source key get the same cleanup guarantee as
unload-all. Relative or otherwise non-normalized input remains
a convenience API: `std::filesystem::absolute` and string construction may
allocate before cleanup begins. If that normalization fails, the original
exception propagates before registry, source, result, or retained-handle state
changes.

Unload first removes or restores every callback and source record. Retired
callback state is then destroyed after releasing the registry lock and while
the manager lock remains same-thread reentrant; plugin callback or DSO
destructors can perform diagnostic registry/manager reads without self-deadlock.
Only afterward is the retained handle released. `unload_all_plugins()` walks
successful loads in
strict reverse sequence so a built-in-to-old-plugin-to-new-plugin override
chain unwinds as new plugin, old plugin, then built-in. Path sorting is not a
valid unload order because each newer snapshot depends on the immediately
preceding implementation.

If an older plugin has already been shadowed by a newer plugin, unloading the
older plugin may remove no active operation keys. `PluginManager` uses the same
slot tokens to splice only the older plugin-owned predecessor values into the
newer plugin snapshot before retiring the middle callback. The newer plugin can
later restore the real predecessor, but can never restore code from the unmapped
middle library. This applies to a real built-in or host-registered sentinel
predecessor as well as to an absent key; each retired plugin callback is
destroyed before its own library is unmapped.

Built-in callback registration also belongs to the process owner. It runs at
most once, before process-owner plugin publication; later Host seed calls only
reconcile source labels and cannot replay built-ins over an active plugin
replacement.
## Policy Plugin ABI

A policy plugin exports exactly two functions declared by the self-contained
C11/C++17 header:

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *out_api);
```

The numeric handshake returns `PS_POLICY_PLUGIN_ABI_VERSION`, currently one.
Only after exact equality does the Host resolve `get_api_v1`. The API table
contains four mandatory callbacks:

| Callback | Responsibility |
| --- | --- |
| `get_metadata` | Return one copied type row for an index in `[0,type_count)`. |
| `create` | Create one class-specific logical context. |
| `select` | Select one candidate from an immutable original snapshot or abstain. |
| `destroy` | Destroy one successfully created logical context exactly once. |

All statuses, classes, masks, decision kinds, structure kinds, flags, counts,
sizes, and generations use fixed-width integer domains. The supported ABI
profile requires eight-bit bytes, 32-bit `uint32_t`, 64-bit `uint64_t`,
64-bit pointers, and eight-byte pointer/integer alignment. Compile-time
assertions freeze every record's natural layout:

| Record | Size | Alignment |
| --- | ---: | ---: |
| `ps_policy_string_view_v1` | 16 | 8 |
| `ps_policy_type_metadata_v1` | 80 | 8 |
| `ps_policy_create_args_v1` | 40 | 8 |
| `ps_policy_candidate_v1` | 120 | 8 |
| `ps_policy_selection_snapshot_v1` | 64 | 8 |
| `ps_policy_decision_v1` | 48 | 8 |
| `ps_policy_plugin_api_v1` | 80 | 8 |

ABI v1 has no tail-extension rule. The Host requires exact sizes, structure
kinds, field offsets, callback pointers, enum values, bounds, and zero-reserved
storage. A packing pragma or unsupported target profile fails the header's
layout assertions; a new record shape requires a new ABI generation.

Type names are 1..128 lowercase ASCII bytes matching
`[a-z][a-z0-9_.-]*`. Descriptions and implementation versions are copied,
valid UTF-8 strings of at most 4,096 bytes. One DSO exposes 1..256 types and
must not use the Host-reserved names `interactive` or `throughput`.
Supported-class masks are nonzero subsets of the Interactive and Throughput
bits.

The Host creates a separate logical context for each class binding, even when
both bindings use the same DSO type. The create record contains only the class
and nonzero binding generation. Successful null context is valid and still
requires one destroy call. A failed create must return null and reclaim all
partial plugin allocation.

A selection snapshot contains 1..4,096 exact-stride candidate records.
Candidates contain only opaque identities and Host-authored scalar ranking
metadata. Snapshot storage is borrowed and immutable until `select` returns;
the plugin must not retain it. A selection echoes the exact binding and snapshot
generations and names one unique candidate from the original snapshot.
Abstention must return a zero candidate id. Neither form grants execution
authority.

C++ inclusion gives exports and callbacks C linkage plus `noexcept`; C11 uses
the platform C calling convention. The Host still fences callback entry so an
incorrect C++ DSO cannot export an exception object into Host state. Setup
`OUT_OF_MEMORY` becomes a fresh Host `std::bad_alloc`; invalid or unsupported
setup becomes `GraphErrc::InvalidParameter`; internal, unknown, or escaping
setup failures become `GraphErrc::ComputeError`. Selection failures are
classified as generation-local policy faults instead of unwinding through a
Run.

## Policy SDK Target and Linkage

A policy plugin requests the `policy_sdk` package component and links
`Photospider::policy_sdk`. This is an interface-only target carrying the
installed include directory and C11/C++17 compile features. It does not link
the static `photospider` product, operation runtime, OpenCV, a registry, an
executor, or any worker-owning implementation.

The policy ABI deliberately contains no C++ standard-library value, exception,
RTTI object, virtual interface, allocator owner, or Host callback. A compatible
DSO may be authored in C11 or C++17 under the frozen 64-bit natural-layout
profile. The ABI does not promise compatibility with a different pointer size,
alignment model, calling convention, or future ABI generation.

`PHOTOSPIDER_POLICY_PLUGIN_EXPORT` selects the platform export visibility and
`PS_POLICY_CALL` selects the declared calling convention. Plugins export only
the exact two names. There is no scheduler SDK target, `IScheduler` base,
scheduler factory, worker-count create argument, or compatibility shim.

## Policy Plugin Load Transaction

`PolicyRegistry` is the process owner for immutable built-in and DSO type
records. Loading one DSO follows this order:

1. reject empty/NUL-containing paths and same-thread policy-callback mutation;
2. normalize the absolute path and open it eagerly and locally;
3. resolve and call only `ps_policy_plugin_get_abi_version`;
4. require exact ABI equality, then resolve and call
   `ps_policy_plugin_get_api_v1`;
5. validate the complete exact-size API table;
6. copy and validate every metadata row into a private map;
7. under the registry lock, reject every visible-name conflict, stage the
   complete next type/path containers, and publish both by swap.

Missing symbols, ABI mismatch, malformed API bytes, invalid UTF-8, invalid
bounds/masks, reserved built-in names, duplicate rows, or visible conflicts
publish no type and no path for that DSO. The candidate DSO lease remains live
while its callbacks and borrowed metadata are inspected and while staged
records are destroyed. Only completely copied Host-owned metadata becomes
observable.

The registry calls no DSO callback while holding its mutex. The version, API,
metadata, create, select, and destroy boundaries are all marked as policy
callback intervals. Read-only registry observation may reenter from a callback;
same-thread load, scan, unload, binding creation, or service-level policy
mutation is rejected before it can wait on a registry or binding lock.

`scan` preserves caller directory order, sorts matching DSO candidates within
each directory, and invokes the same single-DSO transaction for each candidate.
It is intentionally not a transaction over the whole scan: an earlier complete
DSO remains published if a later filesystem or load operation fails.

## Policy Binding and Library Lifetime

A visible type record owns copied metadata, the validated API table, its
zero-based row index, and a shared DSO lease. Binding preparation copies that
record under the registry lock, releases the lock, invokes `create`, and
constructs one immutable class/generation/context owner before service
publication. Built-ins use the same binding, generation, first-fault, and
decision-validation interfaces without a DSO callback.

Interactive and Throughput bindings are distinct contexts with independent
nonzero generations. Replacement prepares the candidate outside the service
publication lock. A failed create or publication leaves the active binding and
generation unchanged. Successful publication retires the old shared binding;
the final owner invokes its plugin `destroy` exactly once, without retry, and
keeps the DSO mapped through the call. Destroy status and catchable failure are
diagnostic-only in this no-throw retirement path. A successful null context is
also destroyed once.

Each selection retains shared binding ownership for the full callback and
validation interval. The Host initializes the complete snapshot and decision
records, invokes the callback without registry, binding-state, ready-store,
resource-ledger, Graph, or Run locks, and validates the returned decision
against the immutable original call. A first invalid-plugin result is stored as
a sticky fault on that exact binding generation. Concurrent later faults cannot
replace it. Successful replacement starts with a new generation and no fault.

Registry unload removes all DSO rows and path visibility atomically while
preserving the two built-ins. Existing bindings retain their type records,
callback table, contexts, and DSO leases and therefore remain valid until their
last invocation and binding owner retire. This unload primitive is reserved for
tests and process cleanup; it is not a public Host lifecycle command. The
process registry itself intentionally has process lifetime.

An honest in-process callback that never returns can indefinitely retain its
binding and DSO lease. The Host provides no timeout, forced unwind, destroy, or
unload-progress guarantee across that boundary. Process-isolated plugin
supervision is a separate architecture generation.

## Boundaries and Rationale

The two current plugin boundaries intentionally have different compatibility
profiles:

| Boundary | Data ABI | Authority |
| --- | --- | --- |
| Operation plugin v2 | Provisional C++ registrar and callback values | Operation computation and returned values under Host validation |
| Policy plugin v1 | Exact-size pure C records under a frozen 64-bit profile | Ranking only; no resource or execution capability |

The operation C-linkage entry name is an identity/generation gate, not a stable
C data ABI. Binary compatibility still depends on the matching SDK, compiler,
standard library, C++ ABI, allocator/runtime, exception model, and RTTI
configuration.

The policy boundary uses only fixed-width scalars, opaque `void *` context,
borrowed immutable arrays, and C function pointers. Exact layout assertions and
validation make the supported profile explicit, but do not sandbox a hostile
DSO. A plugin still executes trusted native code in the Host process and can
block, corrupt memory, allocate outside accounting, or create unreported
threads. It simply receives no legitimate execution capability through the
ABI.

Shadow publication prevents partial operation-registry or policy-type-map
visibility. DSO leases keep callback state and plugin-owned values or contexts
inside the lifetime of their defining library. Matching operation restoration
tokens and policy binding generations prevent a removed or replaced plugin from
silently reclaiming current ownership.

[ADR 0003](../adr/0003-process-owned-execution-resources.md) records the
process-owned execution direction. [ADR 0007](../adr/0007-compute-runs-and-process-execution-have-separate-owners.md)
requires policy and execution to remain separate and forbids restoration of the
old worker-owning scheduler boundary. The
[process execution domain target](../roadmap/Kernel-Evolution.md#process-execution-domain)
and [server and plugin isolation target](../roadmap/Kernel-Evolution.md#server-and-plugin-isolation)
record the follow-up direction.

## Compatibility Guidelines

- Operation plugins use `ps::plugin::OperationPluginRegistrar` and export only
  `register_photospider_ops_v2`; v1 and the no-argument registration ABI are
  unsupported.
- Operation plugins link `Photospider::operation_sdk`, adding
  `Photospider::operation_opencv` only for the public OpenCV adapter. They do
  not link the broad static product merely to share registry state.
- Policy plugins include
  `photospider/policy/policy_plugin_api.h`, request the `policy_sdk`
  component, and link `Photospider::policy_sdk`.
- Policy plugins export the exact two v1 symbols, fill exact-size records,
  preserve every Host-initialized prefix/reserved field, and return only
  declared status/enum values.
- Policy callbacks retain no snapshot memory and treat every candidate id as
  opaque. They never create workers or claim that selection starts work.
- There is no operation v1 compatibility path, scheduler SDK, scheduler ABI,
  `IScheduler` adapter, or execution-route plugin ABI.

## Implementation and Validation Entry Points

- `include/photospider/plugin/plugin_api.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/plugin/operation_host_adapter.*`
- `src/lib/plugin/plugin_loader.*`
- `src/lib/plugin/plugin_manager.*`
- `src/lib/policy/policy_registry.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_plugin_manager.cpp`
- `tests/unit/test_op_registry_m31.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/integration/static_product_consumer_smoke.py`
- `tests/integration/graph_cli_plugin_compute_smoke.py`
