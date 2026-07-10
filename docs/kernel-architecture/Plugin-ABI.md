# Plugin ABI

Photospider supports operation plugins and scheduler plugins. Operation plugins
extend the process-owned `OpRegistry` through a host-provided registrar.
Scheduler plugins provide `IScheduler` implementations.

## Operation Plugin ABI

An operation plugin exports a versioned registrar entry:

```cpp
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar);
```

The loader calls this function after loading the dynamic library. The host
creates an `OperationPluginRegistrar`, passes it to the plugin, and forwards
each registration into the one process-owned `OpRegistry`. Operation plugins
must not call `OpRegistry::instance()` during registration, because a dynamic
plugin and a static host can otherwise observe different registry singletons.

The old no-argument `register_photospider_ops()` entry is not a supported
compatibility ABI. It used the same C symbol shape without a signature marker,
so the loader now resolves only `register_photospider_ops_v1` and rejects old
plugins instead of guessing at an incompatible function pointer type.

Supported operation registrations include:

| Registration | Meaning |
| --- | --- |
| HP monolithic | Full-image HP implementation. |
| HP tiled | Tile-based HP implementation. |
| RT tiled | Tile-based RT implementation. |
| Dirty ROI propagator | Backward ROI propagation. |
| Forward ROI propagator | Downstream ROI projection. |
| Dependency LUT builder | Data-dependent spatial dependency map. |
| Device implementation | CPU, Metal, CUDA, or future backend implementation metadata. |

Operation plugins depend on the public `ImageBuffer` and `NodeOutput`
contracts.

## Operation Plugin Shim and Linkage

Operation plugins do not link the broad static `photospider` product to reach
registry symbols. Repo-owned operation plugins register through
`OperationPluginRegistrar`, and standard plugin targets link the narrow
`photospider_operation_plugin_shim` when they need runtime helper symbols such
as `ImageBuffer`/OpenCV adapter conversion functions. The shim deliberately
does not include `OpRegistry`, built-in operation registration, plugin manager,
plugin loader, graph, scheduler, or compute-service code.

This split supports the static-host direction:

- The static Photospider process owns one `OpRegistry` and one operation
  `PluginManager`, shared by every embedded Host.
- Dynamic operation plugins receive registration callbacks from the host, so
  registry mutation stays in that process-owned instance.
- `photospider_operation_plugin_shim` is only a shared runtime helper boundary
  for plugin callback code that needs buffer adapter functions.
- Plugin callback objects and plugin-instantiated return-value internals may
  still point into plugin code, so the process owner and copied value leases
  retain libraries until all such state has been destroyed.

Symbol visibility rules:

- Operation plugin registrar entries use `PLUGIN_API` and the loader only
  treats `register_photospider_ops_v1` as the supported operation-plugin ABI
  entry. Any other externally visible callback helper symbols are not loader
  entry points or compatibility contracts.
- Operation plugin targets define `PHOTOSPIDER_PLUGIN_BUILD`, which keeps
  `PHOTOSPIDER_API` empty even on Windows. Plugin callback code must not import
  backend library symbols through public value contracts such as
  `ps::GraphError`; only the registrar entry is exported through `PLUGIN_API`.
- The loader resolves the exact versioned symbol name.
- The shim exports runtime adapter helper symbols needed by plugin callbacks;
  on Windows it uses `WINDOWS_EXPORT_ALL_SYMBOLS`.
- This remains a C++ ABI boundary because callbacks use `std::function`,
  `NodeOutput`, `Node`, and `OpMetadata`. Compiler, standard library, and
  Photospider header compatibility are version-sensitive until a future pure C
  ABI replaces the callback shapes.

Current operation plugins that are intended to be loadable through
`plugin_dirs` must also register explicit dirty and forward ROI propagators. The
registry still provides legacy identity fallback for migration, but that
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
loader state. Before invoking `register_photospider_ops_v1`, the loader creates
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

Direct replacement uses the same retirement discipline outside manager-driven
unload. A replacement callback is prepared before locking and swapped with the
active slot; the displaced callable remains in the parameter-local retirement
value until the registry guard has exited. Whole-key unregister extracts the
legacy, metadata, implementation, and ownership map nodes together, then
destroys the extracted values outside the guard. Device implementation values
and their revision vector therefore remain parallel, and a direct device
registration after whole-key unregister cannot inherit a stale plugin token.

The transaction has three outcomes:

- If the registrar or any later staging step throws `std::bad_alloc`, the
  exact exception propagates. Registry callbacks, sources, diagnostics, and
  retained handles remain byte-for-byte logically equivalent to their
  pre-candidate state.
- If the registrar throws another standard exception, the loader commits only
  the structured diagnostic for that candidate. No callback, source,
  restoration snapshot, or handle becomes active.
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
same lease to `NodeOutput`. The lease is the first-declared and last-destroyed
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
slots have different tokens and remain active. Empty registry values can then be
erased without destroying plugin callback state under the registry lock. The
retired plugin record is destroyed after that lock is released. There is no
temporary key collection, callback copy, callable comparison, or allocating
rollback, so `unload_all_plugins()` remains a `noexcept` cleanup path even when
global allocation is failing.
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

A scheduler plugin exports C symbols with these names:

```text
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
| `get_count` | Yes | Number of scheduler types in the plugin. |
| `get_name` | Yes | Type name at an index. |
| `create` | Yes | Create a scheduler instance for a type. The returned object must implement both `IScheduler` and `SchedulerTaskRuntime`. |
| `get_description` | No | Human-readable type description. |
| `destroy` | Required for ownership | Destroy plugin-created scheduler instance. |
| `get_version` | No | Plugin version string. |

## Scheduler Plugin Load Transaction

Loading one scheduler plugin is a strong transaction over the scheduler type
map, type metadata, retained-library map, and ordered load diagnostics. After
opening the candidate and resolving its exports, the loader creates local
shadow copies of all four containers. Results from version, count, name, and
description callbacks, duplicate/conflict diagnostics, registered-type
bookkeeping, and the candidate `PluginHandle` are recorded only in those
shadows. No candidate type can therefore become visible before its metadata and
retained library are ready.

If any plugin callback, metadata copy, diagnostic construction, or container
allocation throws, the shadow state is destroyed before the candidate shared
library lifetime is released. The exact pre-call type, metadata, handle, and
error prefixes remain active and the original exception propagates unchanged,
so the same plugin path can be retried immediately. A complete candidate is
published under the loader mutex by no-throw container swaps, with the retained
handle swapped first. Library-open and missing-export failures still return
false with one diagnostic, but that diagnostic append is itself staged and
cannot partially alter the prior error sequence.

## Scheduler Instance Runtime Contract

The current scheduler plugin instance is a C++ object returned as
`ps::IScheduler*`, but the host also requires the same object to implement
`ps::SchedulerTaskRuntime`. The loader validates this with `dynamic_cast` during
creation. A plugin that exports valid C symbols and returns an `IScheduler`
which does not also implement `SchedulerTaskRuntime` can be discovered, but it
will be rejected when the host tries to instantiate that scheduler type.

The host lifetime owner is a transparent runtime wrapper. It forwards
`available_devices()` plus every current callback and borrowed-`TaskHandle`
single/batch submission method directly to the plugin instance. It must not
fall back to `SchedulerTaskRuntime` base implementations, because doing so can
replace a plugin's CPU/Metal device inventory, split an atomic batch into
per-item submissions, change task ordering, or alter exception identity.

This is part of the current transitional C++ ABI. Plugin authors should inherit
both interfaces in the concrete scheduler class until the long-term pure C ABI
replaces this requirement.

## Scheduler Instance Ownership

Scheduler instances created by a plugin must be destroyed through that plugin's
destroy function. The loader must not rely on default C++ deletion for
plugin-created instances.

This rule avoids allocator, runtime, and dynamic-library boundary problems.

Immediately after a non-null create result, the loader installs a non-allocating
stack guard containing the raw instance, destroy function, and shared library
lifetime. The guard remains active through `SchedulerTaskRuntime` validation,
heap allocation of the host owner, and copied type-name construction. If owner
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

These fences apply only to destructor fallback and raw-owner construction
cleanup. Explicit `attach`, `start`, `shutdown`, and `detach` calls preserve and
propagate plugin exceptions to their caller. This distinction keeps the public
lifecycle contract observable while preventing a hostile plugin from
terminating the process during host destruction.

The scheduler plugin library must remain loaded while any scheduler instance
created by that plugin may still exist.

## Current ABI Status

The current scheduler plugin interface uses C symbol names but returns C++
`ps::IScheduler*`. That means binary compatibility currently depends on a
compatible C++ ABI, compiler, standard library, and Photospider interface
version.

This is transitional. The long-term direction is a pure C scheduler ABI using
opaque handles or callback tables so plugins do not depend on C++ binary ABI.

## Compatibility Guidelines

- Operation plugins should use the published registration APIs and public data
  contracts.
- Operation plugins must use `OperationPluginRegistrar` and
  `register_photospider_ops_v1`; the no-argument registration ABI is not
  supported.
- Operation plugins must not link `photospider` merely to share registry
  state. Use `photospider_operation_plugin_shim` only for narrow runtime helper
  symbols needed by plugin callback code.
- Scheduler plugins should treat both `IScheduler` and `SchedulerTaskRuntime`
  ABI compatibility as version sensitive.
- Scheduler plugin authors should implement `ps_scheduler_plugin_destroy`.
- The host should use plugin destroy for plugin-created scheduler instances.
- Future C ABI work should be done as a separate compatibility change.
