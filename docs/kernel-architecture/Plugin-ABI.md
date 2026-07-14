# Plugin ABI

Photospider supports operation plugins and scheduler plugins. Operation plugins
extend the process-owned `OpRegistry` through a host-provided registrar.
Scheduler plugins provide `IScheduler` implementations through a numeric
handshake-gated transitional C++ ABI. The installable authoring contracts live
only under `include/photospider/{plugin,scheduler}`.

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
  metadata, and debug metadata. The host converts the complete value to its
  private representation before attaching the private DSO lease.
- `RoiContext` exposes ordered `InputEdgeView` topology snapshots; forward ROI
  callbacks receive the active edge, and dependency builders return an owned
  `DependencyLutSnapshot` that the host validates before caching.
- `ParameterTypeError` reports an explicit `ParameterValue` alternative
  mismatch. Parameter conversion and allocation failures propagate unchanged
  before callback entry.

Host conversion before callback entry and after a successful return remains
outside the plugin exception fence and preserves its host-owned type. The
actual plugin invocation retains an explicit DSO lease and normalizes every
plugin-origin exception before that lease can be released: plugin
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
  it contains no registry, loader, graph, scheduler, or compute state.
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
HP compatibility bridge retain the same callback target, and scheduler or
reader snapshots may retain that same logical target as well. These paths may
invoke it concurrently. The callback provider must therefore make the target
reentrant or synchronize its shared mutable state. The registry serializes only
ownership mutation, coherent snapshot capture, publication, and unload; it never
holds its state lock to serialize callback execution. Callers must not infer
single-threaded execution from a shared operation key, device, or intent.

Direct replacement uses the same retirement discipline outside manager-driven
unload. A replacement callback is prepared before locking and swapped with the
active slot; the displaced callable remains in the parameter-local retirement
value until the registry guard has exited. Whole-key unregister extracts the
legacy, metadata, implementation, and ownership map nodes together, then
destroys the extracted values outside the guard. Device implementation values
and their revision vector therefore remain parallel, and a direct device
registration after whole-key unregister cannot inherit a stale plugin token.

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
temporary retire the old image/YAML/data/spatial/debug state before releasing
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

## Scheduler Plugin ABI

A scheduler plugin explicitly defines all seven required C exports:

```text
ps_scheduler_plugin_get_abi_version
ps_scheduler_plugin_get_count
ps_scheduler_plugin_get_name
ps_scheduler_plugin_get_description
ps_scheduler_plugin_create
ps_scheduler_plugin_destroy
ps_scheduler_plugin_get_version
```

Required exports:

| Export | Required | Meaning |
| --- | --- | --- |
| `get_abi_version` | Yes, first gate | Non-throwing numeric handshake with signature `uint32_t() noexcept`; it must return `PS_SCHEDULER_PLUGIN_ABI_VERSION`, currently `1`. |
| `get_count` | Yes | Number of scheduler types in the plugin; it must be at least one. |
| `get_name` | Yes | Stable type name at an index; every index below count must return non-null, non-empty text. |
| `get_description` | Yes | Human-readable type description at an index. |
| `create` | Yes | Create a scheduler instance for a type. `IScheduler` already publicly derives from `SchedulerTaskRuntime`. |
| `destroy` | Yes | Destroy a plugin-created scheduler instance. |
| `get_version` | Yes | Human-readable implementation version, called once and cached; it is not the compatibility gate. |

Count, index, and worker-count values use fixed `uint32_t` widths. The
installable SDK supplies the ABI constant, typed function-pointer aliases, and
`PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT`; it deliberately supplies no declaration
or single-scheduler implementation macro. Each DSO declares every export and
therefore makes its lifecycle and visibility contract explicit.

## Scheduler Plugin Load Transaction

Loading one scheduler plugin is a strong transaction over the scheduler type
map, type metadata, retained-library map, and ordered load diagnostics. The
loader opens a POSIX candidate with `RTLD_NOW | RTLD_LOCAL`, resolves and calls
only `ps_scheduler_plugin_get_abi_version`, and requires exact equality with
the SDK value before resolving any other export. Missing or mismatched
handshakes release the candidate with a structured diagnostic; implementation
version, count, name, description, create, and destroy are not called and no
candidate state is published.

For a compatible candidate, the loader creates local shadow copies of all four
containers. Results from the once-only implementation-version call, count,
name, and description callbacks, duplicate/conflict diagnostics,
registered-type bookkeeping, and the candidate `PluginHandle` are recorded
only in those shadows. No candidate type can therefore become visible before
its cached metadata and retained library are ready.

A compatible candidate is still rejected when count is zero, any in-range name
is null or empty, or every valid name conflicts with an existing type. Such a
rejection discards all candidate shadows, releases the candidate library, and
appends one structured diagnostic without changing the prior diagnostic
prefix. Conflicts remain recoverable when at least one valid non-conflicting
type is available: that type, its metadata, the retained handle, and the staged
conflict diagnostics commit together.

If a discovery callback throws, the loader applies the same host-owned mapping
as the operation callback fence while the candidate lease is live. If host
metadata copy, diagnostic construction, or container allocation throws, that
host-owned exception retains its type. In both cases the shadow state is
destroyed before the candidate shared-library lifetime is released; the exact
pre-call type, metadata, handle, and error prefixes remain active, and the same
plugin path can be retried immediately. A complete candidate is published under
the loader mutex by no-throw container swaps, with the retained handle swapped
first. Library-open and missing-export failures still return false with one
diagnostic, but that diagnostic append is itself staged and cannot partially
alter the prior error sequence.

## Scheduler Instance Runtime Contract

The scheduler plugin instance is a C++ object returned as `ps::IScheduler*`.
`IScheduler` publicly derives from `SchedulerTaskRuntime`, so one object has
exactly one lifecycle/runtime interface and creation performs no cross-DSO
runtime type discovery.

`IScheduler::attach` receives a borrowed public `SchedulerHostContext&`, never
`GraphRuntime`. The context exposes only device-capability queries, task
worker/epoch context set/clear, and trace publication. Its protected destructor
prevents plugin-side deletion; the host keeps it alive from successful attach
through shutdown and detach. A scheduler clears every retained context pointer
during detach. Built-in and plugin schedulers therefore preserve TLS metrics
and trace attribution without access to graph ownership or native Metal
handles.

The host lifetime owner is a transparent runtime wrapper. It forwards
`available_devices()` plus initial and worker-ready borrowed-`TaskHandle`
batches, any-thread callback submission, completion wait, first-exception
publication, completion-counter mutation, and trace publication directly to
the plugin instance. Both handle-batch methods are pure virtual, so the SDK
cannot split an atomic batch into per-item base fallbacks and thereby change
ordering. Every returned device is validated against CPU, Metal, CUDA, and
ASIC/NPU before host planning; an unknown enumerator becomes a host-owned
`GraphError(InvalidParameter)`. `TaskExecutor` uses protected virtual
destruction, so the plugin cannot delete the dispatcher-owned executor through
its borrowed base pointer.

Plugin discovery, create, lifecycle, and runtime failures use the same
host-owned exception mapping as operation callbacks while an explicit scheduler
DSO lease is live. Host tasks are different: before a `TaskHandle` executor,
any-thread callback, or non-null `set_exception` value enters plugin code, the
owner preallocates an append-only identity slot. The relay records the original
`exception_ptr` without allocation and uses bare `throw;`; plugin code therefore
observes the original dynamic type, message, and object. When the same pointer
later surfaces, the host recognizes it before plugin-failure normalization and
rethrows it exactly. Recording and lookup allocate nothing, no plugin call holds
the registry guard, rejected admission only marks its slots inactive, and the
matching wait clears all slots. Registry clear swaps storage under the guard and
destroys retired exception objects afterward, so an exception destructor may
reenter scheduler APIs without deadlocking.

This is part of the current transitional C++ ABI. Plugin authors derive only
from `IScheduler` and implement its inherited runtime operations until a
separately versioned pure C ABI replaces this boundary.

## Scheduler Instance Ownership

Scheduler instances created by a plugin must be destroyed through that plugin's
destroy function. The loader must not rely on default C++ deletion for
plugin-created instances.

This rule avoids allocator, runtime, and dynamic-library boundary problems.

Immediately after a non-null create result, the loader installs a non-allocating
stack guard containing the raw instance, destroy function, and shared library
lifetime. The guard remains active through heap allocation of the host owner
and copied type-name construction. If owner
allocation or string copy throws `std::bad_alloc` (or any other construction
exception), the guard calls plugin destroy exactly once and keeps the library
mapped until that call returns. Ownership transfers only after the complete
host owner has been constructed.

The completed host owner has a `noexcept` destructor. Destruction first clears
its host-side raw/runtime pointers, then attempts `shutdown()` and `detach()`
behind two independent catch-all fences. Failure in either lifecycle call,
including `std::bad_alloc`, cannot skip the later stage. The owner then calls
the plugin destroy export exactly once behind a third no-throw ABI fence. A
throwing destroy export is not retried because the host cannot know whether the
plugin already ended or partially ended the object lifetime. The shared library
lifetime is released only after that single destroy attempt returns, so
`shutdown`, `detach`, destroy, and any plugin-side destructor code all execute
while the library remains mapped.

These catch-all suppression fences apply only to destructor fallback and
raw-owner construction cleanup. Explicit `attach`, `start`, `shutdown`, and
`detach` calls remain observable, but plugin-origin failures use the host-owned
mapping above rather than exporting a DSO exception object. This distinction
keeps the public lifecycle contract observable while preventing either an
unmapped dynamic type or a hostile cleanup exception from reaching the host.

Before every explicit `attach()` or `start()` attempt, the owner marks the
matching detach or shutdown fallback as required. This happens before control
enters the plugin: if a repeated attach/start publishes partial state and then
throws, an earlier successful detach/shutdown cannot make the destructor skip
the cleanup required by that failed retry.

The scheduler plugin library must remain loaded while any scheduler instance
created by that plugin may still exist.

## Current ABI Status

The scheduler handshake rejects unknown Photospider interface generations
before discovery or object creation, but the accepted interface still uses C
symbol names around a C++ `ps::IScheduler*`. Binary compatibility therefore
also depends on a compatible compiler, standard library, exception model,
RTTI configuration, and C++ ABI. The human-readable implementation version is
diagnostic metadata only and never substitutes for the numeric handshake.

This ABI is explicitly provisional. ADR 0003 and the kernel evolution roadmap
record the accepted replacement direction; they do not change the current
loader contract described here.

## Compatibility Guidelines

- Operation plugins should use the published registration APIs and public data
  contracts.
- Operation plugins must use `ps::plugin::OperationPluginRegistrar` and
  `register_photospider_ops_v2`; v1 and the no-argument registration ABI are
  unsupported.
- Operation plugins must not link `photospider` merely to share registry
  state. Link `Photospider::operation_sdk`, adding
  `Photospider::operation_opencv` only when the public OpenCV adapter is used.
- Scheduler plugins derive from `IScheduler`, implement its inherited runtime
  contract, and export the exact numeric handshake plus all six remaining
  required functions.
- The host should use plugin destroy for plugin-created scheduler instances.
- No pure C operation or scheduler ABI compatibility is currently provided.
