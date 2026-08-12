# Plugin ABI

Photospider supports operation plugins, data-definition providers, and policy
plugins. Operation plugins extend the process-owned `OpRegistry` through a
Host-provided registrar and remain the currently installed provisional C++ ABI
v2. [ADR 0012](../adr/0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md)
accepts a separate pure-C operation-plugin ABI v1 as the replacement target;
its header, loader, SDK, and migrated plugins do not yet exist. Data-definition
providers publish immutable Schema/Facet/Layout bundles and bounded semantic
callbacks through pure-C ABI v3; they receive no access, conversion, execution,
device, registry-mutation, or graph capability. Policy plugins rank immutable
Host-admissible candidates through pure-C ABI v1; they own no worker, queue,
device, resource, Run, or Graph capability. Current installable authoring
contracts live only under `include/photospider/plugin/` and
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

Every executable registration carries one `OperationMetadata` value. In
addition to tile, device, cost, and dependency hints, the CPU execution
contract contains:

| Field | Meaning |
| --- | --- |
| `reentrant` | Whether callbacks of the exact implementation may overlap; defaults to `true`. |
| `maximum_parallelism` | Exact-implementation callback cap; zero means no implementation-specific cap. |
| `retained_memory_bytes` | Additional Host-retained bytes per in-flight callback; zero is an explicit declaration. |
| `scratch_bytes` | Additional Host scratch bytes per in-flight callback; zero is an explicit declaration. |
| `exclusive_key` | Optional execution-domain exclusion key shared across implementations, Runs, and Graphs. |

`reentrant=false` has an effective cap of one regardless of
`maximum_parallelism`. A nonempty exclusive key is limited to 128 bytes and
must not contain an embedded NUL. The Host validates the same rules for core
and plugin registrations before publication. These fields extend the
provisional C++ v2 metadata layout without changing the registrar symbol or
callback signatures. Existing v2 DSOs must be rebuilt against the matching
SDK; no missing-tail, stale-layout, or compatibility interpretation exists.

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
`Photospider::operation_runtime`, whose shared library implements public
image-buffer factories, explicit-binding DenseTensor Value and checked view
symbols, dependency-neutral device/access facts, and Region value/algebra,
without linking back to the SDK or requiring an external package. The static
Host product and
independently loaded Value-using
operation DSOs therefore resolve allocation/revision minting through that one
runtime image instead of copying counters into each DSO. This is an ordinary
dynamic dependency, not ELF/Mach-O symbol interposition or a plugin ABI
callback. Those data/memory headers are available for dependency-neutral
plugin-internal work, but operation v2 callback records still receive and
return the current ImageBuffer/OperationOutput values.

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
- `Photospider::operation_runtime` contains ImageBuffer, immutable DenseTensor
  and provider-defined Value, Region, and data-definition registry
  implementation. It contains no operation registry, platform loader, Graph,
  policy, execution, or compute state and constructs no global definition
  registry. Its one process-wide identity
  authority owns separate monotonic allocation and Value-revision sequences.
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

## Native Plugin Trust Admission

Every current operation-v2 and policy-v1 DSO load begins with one shared,
process-immutable trust policy. On first authorization, the process reads the
files named by `PHOTOSPIDER_PLUGIN_TRUST_MANIFEST`,
`PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE`, and
`PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY`. All three are required. The canonical
LF-terminated manifest is verified with Ed25519 through OpenSSL EVP and binds
each sorted entry's closed `operation`, `policy`, or `isolated-runtime` kind,
nonzero 128-bit package id, positive generation, and SHA-256 content digest.
Duplicate `(kind, package id, generation)` identities and duplicate
`(kind, digest)` content-role mappings are rejected, so bytes and role select
one package generation.
The first successful policy or fail-closed configuration result is retained for
the process lifetime; neither a later environment change nor an IPC value can
mint or replace trust authority.

Authorization opens a non-followed regular file, hashes the bounded bytes from
that candidate, and checks stable pre/post metadata, but never returns that
mutable inode as authority. Linux copies approved bytes into an anonymous
`memfd`, applies write/grow/shrink/seal seals, and confirms SHA-256 on the sealed
descriptor before operation/policy mapping through `/proc/self/fd/N`. Darwin
copies an approved DSO into a mode-0700 private directory, syncs and reopens it,
confirms SHA-256, immediately unlinks the file and directory, and maps only the
anonymous descriptor through `/dev/fd/N`. Consequently a rename, symlink,
pathname replacement, hard link, or preopened writer cannot substitute later
bytes. Current Windows and every other unsupported DSO profile fail closed
instead of using a pathname fallback.

Every `PluginTrustError`, including missing or unreadable trust configuration
and an unopenable, unsigned, wrong-kind, or changed candidate, is exposed by
the operation load report or policy Host surface as
`GraphErrc::InvalidParameter`. A native `dlopen` failure after successful trust
authorization remains `GraphErrc::Io`; missing ABI symbols or malformed ABI
content remain `GraphErrc::InvalidParameter`. This keeps authorization failure
distinct from failure to map an already authorized native object.

Approval does not sandbox an in-process DSO. An approved operation or policy
library remains operator-trusted native code with the ordinary ability to
block, allocate outside Host accounting, issue syscalls, or corrupt its Host
process. The separate isolated-runtime trust and resource controls below do not
retrofit containment onto these two in-process ABIs.

## Operation Plugin Load Transaction

Loading one operation plugin is a strong transaction over all observable
loader state. Before native mapping or invoking
`register_photospider_ops_v2`, the loader authorizes the exact opened candidate
as a signed `operation` artifact. It then creates staged copies of the target
`OpRegistry`, operation-source map, structured load result, and retained-handle
map. The host-provided registrar points at the staged registry, so plugin
callbacks never mutate the active registry during registration. Registration
capture, previous-source calculation, restoration snapshots, result
aggregation, and handle insertion also mutate only staged state.

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

Executable scalar slots are atomic schedulable values rather than a callback
plus one mutable intent-wide metadata record. Monolithic HP, tiled HP, and
tiled RT each own their exact callback, `OperationMetadata`, and nonzero
implementation identity in one `OpImplementation`. Registering a sibling shape
cannot rewrite an existing slot's scheduling declarations or identity.
Replacement, capture, retirement, and unload exchange the complete slot, so a
reader can never combine one scalar callback with another registration's
reentrancy, cap, retained bytes, scratch bytes, or exclusive key.

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

Stable ownership is not itself an execution mutex. The registry serializes only
ownership mutation, coherent snapshot capture, publication, and unload; it
never holds its state lock during callback execution. Product planning selects
one coherent callback, metadata, device, and nonzero ownership revision, stores
only callback-free identity/metadata in the plan, and re-resolves the exact
identity before admission. Within one injected `ExecutionService`, the Host
then enforces `reentrant`, `maximum_parallelism`, and `exclusive_key` at
reserved start across Runs and Graphs. A shared operation key, device, intent,
or callback owner does not imply serialization unless the selected metadata
declares it. Providers must still protect shared state reached outside this
Host boundary or omitted from their declaration.

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
state that a provider owns. The Metal Perlin DSO owns neither a native
lifecycle nor a lifecycle mutex: it borrows the command queue,
invocation-scoped allocator, and pipeline cache from the current process
executor, while execution metadata gates provide implementation/key
serialization.
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

## Accepted Operation Plugin ABI v1 Target

Issue #101 freezes the replacement contract without implementing it. The
target is an independent operation-plugin ABI v1, not a provider-v3 suite or a
policy-v1 extension. Its future self-contained C11/C++17 header is
`photospider/plugin/operation_plugin_api.h`, and discovery uses only:

```c
#if defined(__cplusplus)
extern "C" {
#endif

PS_OPERATION_PLUGIN_EXPORT uint32_t PS_OPERATION_CALL
ps_operation_plugin_get_abi_version(void) PS_OPERATION_NOEXCEPT;

PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1 PS_OPERATION_CALL
ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api_out) PS_OPERATION_NOEXCEPT;

#if defined(__cplusplus)
}
#endif
```

The Host performs only the numeric handshake before requesting the root API.
`PS_OPERATION_PLUGIN_EXPORT` is the platform export/default-visibility
annotation for only those named declarations. `PS_OPERATION_CALL` is the
platform C convention and `__cdecl` on Windows; `PS_OPERATION_NOEXCEPT` is
`noexcept` in C++17 and empty in C11. Both resolved entrypoint typedefs and
every callback carry the latter two macros. The v1 profile freezes 8-bit bytes,
4/8-byte `uint32_t`/`uint64_t`, 8-byte data and every named function-pointer
type, natural 8-byte data/function-pointer and `uint64_t` alignment,
Host-process endianness, and that matching convention. Packed, over-aligned,
32-bit, foreign-endian, or foreign-convention records are incompatible. Object
pointers, function pointers, and integer slots remain distinct C types.

Exactly the 20 versioned semantic records, Diagnostic through Tile, begin with
exact `struct_size`, `struct_kind`, `struct_version`, and `flags` fields. Plain
fixed identity/handle, byte-view, digest, array-reference, configuration-value,
and axis-range helpers carry no record header. Root and suite tables use their
own prefixes. Every suite begins with four `uint32_t` fields: `struct_size`,
`suite_id`, `suite_version`, and `flags`. V1 rejects unknown kinds or suite IDs,
versions, flags, nonzero reserved data, short/long records, unknown tails,
wrong stride/alignment, and arithmetic/range overflow. A new field requires a
new owning-suite version or operation ABI v2; v1 never interprets a minimum-
size prefix.

The root API is exactly 96/8 bytes/alignment and begins with `struct_size`,
`abi_version`, `flags`, and `reserved0`, followed by permanent 128-bit plugin
identity, bounded implementation-version view, opaque plugin
context, typed `query_suite`, typed `destroy_plugin`, and exactly
`uint64_t reserved[3]`. Each v1 suite table is exactly 64/8:

| ID | Suite | Requirement | Callbacks |
| ---: | --- | --- | --- |
| 1 | Definition | Always | operation count/get; implementation count/get |
| 2 | Configuration | Always | validate; create context; destroy context |
| 3 | Inference | Always | infer complete output plans |
| 4 | Region | Always | backward dirty; forward active-edge propagation |
| 5 | Dependency | If any implementation declares data dependence | build dependency records |
| 6 | Execution | Always | synchronous monolithic; synchronous tiled |

Before each query the Host initializes every field of the concrete 64-byte
suite to its C semantic zero value—integer fields zero and pointer fields
null—and sets its nonnull `ps_operation_suite_header_v1` to size 64, requested
suite ID, requested version, and zero flags. This does not assume that
byte-zero is a null pointer. The plugin preserves those four prefix fields. Unknown IDs or
versions return `UNSUPPORTED`. After `OK`, a returned size/ID/version/flags
mismatch is `INVALID_DESCRIPTOR` and is rejected before a callback slot is
read. A missing, malformed, or incomplete required/declared suite rejects the
complete candidate before any callback, source, or handle becomes visible. The
tables expose no allocator, registry, Host service, Graph, Run, scheduler,
cache, executor, device service,
resource token, filesystem, artifact, credential, thread, or symbol lookup.

Before `get_api_v1`, the Host initializes every field of the concrete 96-byte
root to its C semantic zero value and sets size 96, ABI version one, zero flags,
and zero `reserved0`; the plugin preserves that prefix.
Host-prepared semantic records and suites use complete exact size/kind/version/
flags or size/ID/version/flags prefixes. Plugin-authored sink records carry a
complete semantic-record prefix. The receiver validates the applicable prefix
before any later field.

All suite/record versions are one. Suite IDs are exactly 1 Definition,
2 Configuration, 3 Inference, 4 Region, 5 Dependency, and 6 Execution. The
other closed numeric sets are record kinds 1
through 20 (Diagnostic through Tile in the ADR catalogue), configuration kinds
1 Null through 8 Object, directions 1 Input/2 Output, intent bits 1 HP/2 RT,
shape bits 1 Monolithic/2 Tiled, device 1 CPU, access bits 1 Read/2 Write,
behavior bits 1 SideEffect/2 DataDependent, Region outcomes 1 Exact through 4
Unknown, Region atoms 1 Whole through 4 TensorSlice, and sink channels 1
Diagnostic through 4 DependencyRecord. Unknown values/bits and invalid zero
fail as `INVALID_DESCRIPTOR`; booleans are zero or one. ValueView flag bit 1 is
PayloadAvailable; all other semantic-record flags and all root/suite flags are
zero in v1.

ADR 0012 freezes complete normative C typedef prototypes, not only callback
order. The resolved entrypoints are
`ps_operation_plugin_get_abi_version_fn_v1` and
`ps_operation_plugin_get_api_fn_v1`; root uses
`ps_operation_query_suite_fn_v1` and `ps_operation_destroy_plugin_fn_v1`; the
sink uses `ps_operation_emit_fn_v1`; and Definition, Configuration, Inference,
Region, Dependency, and Execution use exactly 4/3/1/2/1/2 named typed callbacks
in table order. Every callback returns `ps_operation_status_v1` except the
numeric entrypoint (`uint32_t`) and null-only reserved callback (`void`). The
sink prototype has exactly `void *host_context`, `uint32_t channel`,
`const void *records`, `uint32_t count`, and `uint32_t stride`. All identity/
view/record/helper
inputs and sinks are `const` pointers, Host outputs are writable pointers,
indices/counts/channel/stride are `uint32_t`, opaque contexts are `void *`, and
array inputs are `const ps_operation_array_ref_v1 *`. Configuration destroy
receives operation and implementation identity even for a null context. No
cancellation callback is exposed; the Host checks before entry, on sink calls,
and after return, may normalize to `CANCELLED`, and discards a late result.

Plugin, operation, implementation, port, Schema, Facet, and Layout identities
are permanent publisher-assigned definition identities. Value, edge,
allocation, binding, site, and Region identities are Host-minted process-local
runtime identities scoped respectively to their logical value, Graph revision,
allocation, binding/write grant, or invocation snapshot. The Host separately
mints nonzero, unpredictable 128-bit generation and invocation handles as
distinct helper types. Those handles are process-local correlation handles,
not semantic identities, pointers, lookup APIs, capabilities, durable
identities, resource tokens, or wire values. Invocation-scoped callbacks carry
both handles; definition/configuration-lifetime/root/destroy callbacks rely on
their exact DSO generation lease and explicit identities/contexts instead.
Plugin and configured-
operation contexts are opaque plugin-owned `void *` round-trip values. A
successful null context is valid and still receives exactly one matching
destroy attempt; a failed create transfers no destroy obligation.

The sink `host_context` is a Host-owned callback-local round-trip token. Plugin
code may only pass it back to `emit` and may not dereference, free, retain, or
interpret it as semantic identity.

The future header freezes the following natural record size/alignment classes;
the detailed ordered field groups are normative in ADR 0012 and the active
OpenSpec design:

| Layout category | Size/alignment |
| --- | --- |
| record header / suite header | 16/4 and 16/4 |
| identity, generation/invocation handles, immutable/mutable byte views, exact-stride array reference, configuration value, axis range | 16/8 |
| SHA-256 digest | 32/8 |
| diagnostic, output sink, configuration view, Region-set view | 48/8 |
| configuration node, facet view, tile | 64/8 |
| buffer view, input binding, Region binding | 80/8 |
| output plan, mutable output binding, invocation, Region atom, dependency record | 96/8 |
| port descriptor | 112/8 |
| operation descriptor and value view | 128/8 |
| value descriptor | 192/8 |
| implementation descriptor | 192/8 |
| root API and every suite v1 table | 96/8 and 64/8 |

The 29 fixed-layout payload types are nine plain helpers plus 20 semantic
records; record/suite headers, root, and suites are separately typed. The
active OpenSpec design freezes every C field type and byte offset, every exact
type/typedef spelling, and every field name that it spells explicitly. The
128-byte operation descriptor is realizable because offsets 96 and 112 are
the two 16-byte input/output port `{pointer,count,stride}` helpers; it does not
store duplicate count fields. The root uses its exact prefix at 0, plugin
identity at 16, version view at 32, context/query/destroy at 48/56/64, and
`uint64_t reserved[3]` words at 72. The six suite tables use typed callback
slots from offset 16 and null `ps_operation_reserved_callback_fn_v1` slots
through byte 63: Definition reserves two at 48, Configuration three at 40,
Inference five at 24, Region four at 32, Dependency five at 24, and Execution
four at 32. No object pointer or integer substitutes for any function-pointer
slot.

All pointer/count/stride views are borrowed for one synchronous call or sink
emission. Null corresponds exactly to zero count, stride equals the exact
element size, and both sides check alignment, multiplication, base/offset,
subranges, aggregate bounds, and output overlap before dereference. The Host
deep-copies accepted descriptors and emitted results before return.

Operation and port descriptors freeze permanent identities, canonical names,
borrowed exact-stride port arrays, directions, configuration schema, and
Schema/Facet/Layout identities. Type/subtype are nonempty UTF-8 without NUL or
`:`, forming the unique `type:subtype` key. Port/implementation names are
nonempty UTF-8 without NUL; display/exclusive key may be empty but are otherwise
UTF-8 without NUL. Names are never normalized, case-folded, or truncated.
Implementation descriptors freeze HP/RT intent, monolithic/tiled shape, CPU
device profile, tile/access/side-effect/data-dependence facts, reentrancy,
maximum parallelism, retained/scratch bytes, relative cost, and exclusive key.
Callback, copied metadata, implementation identity, source generation, and
revision publish and restore as one slot.

Configuration is a Host-owned immutable exact-stride tree of null, boolean,
signed 64-bit integer, binary64, UTF-8 string, bytes, array, and object nodes,
not YAML, `ParameterMap`, or a C++ variant. Value records keep Schema/Facet/
Layout identities and versions, logical revision, allocation/binding
identities, and descriptor/content/layout digests distinct. Inference, Region,
and dependency calls receive descriptor-only views with payload pointers
cleared. Execution alone receives payload; only Host mutable-output grants
permit writes.

Inference produces every immutable output plan before allocation. Region v1
has explicit backward dirty and forward active-edge propagation with Exact,
Whole, Empty, and Unknown outcomes and Whole, Empty, ImageRect, and TensorSlice
atoms. Data-dependent implementations require Dependency v1; copied records
bind output port/site/region facts to input edge/region facts before cache use.

Execution v1 is synchronous and CPU-addressable only. It exposes neither
native device handles nor device-resident buffers, fences, deferred completion,
retained invocation owners, or delayed sinks. A plugin may use a private device
only if it copies into the Host CPU output before return. The repository Metal
example must use synchronous CPU staging or move behind a Host-private adapter
before v2 deletion. Native/async work requires a future separate suite or ABI.

V1 has no allocator callback. Definition strings are copied before return,
execution writes Host-owned buffers, and planning/diagnostic records use one
callback-local 48-byte Host output sink. Its closed channels accept diagnostic,
output-plan, Region-binding, or dependency records as appropriate. The first
sink failure is sticky, including when the plugin ignores it and returns
success. Host memory is destroyed by the Host; plugin memory and successful
contexts receive exactly one plugin destroy attempt under the exact DSO lease.

`ps_operation_status_v1` has exact `uint32_t` values 0 through 8 for `OK`,
`INVALID_ARGUMENT`, `OUT_OF_MEMORY`, `UNSUPPORTED`, `INVALID_DESCRIPTOR`,
`TOO_COMPLEX`, `CANCELLED`, `FAILED_PRECONDITION`, and `INTERNAL_ERROR`.
Unknown status fails as an ABI fault. One non-OK callback may emit one copied
UTF-8 diagnostic up to 4 KiB. No exception or foreign unwind crosses the DSO;
C++ wrappers map `std::bad_alloc`, invalid input, and every other catchable
failure inside the plugin.

The implementation must preserve the current strong shadow transaction while
removing the C++ boundary. It validates and copies the complete root, suites,
descriptors, callbacks, identities, and bounds; assigns one Host generation;
and atomically publishes immutable per-slot callback/metadata/identity/source/
revision values. Every callback/context retains its exact generation and DSO.
Retirement removes publication, waits for leases, destroys in reverse order,
and unmaps last. Middle-generation unload splices only owned predecessors.
No Host registry, publication, scheduler, or execution lock is held during
plugin code.

An in-process callback that never returns may retain its invocation, write
grants, contexts, generation, and DSO forever. Operation ABI v1 is therefore
only an operator-trusted compatibility and validation boundary, never a
sandbox. Tenant-untrusted pointer-free wire records, runtime supervision, and
trust/resource enforcement are independently versioned boundaries delivered by
Issues #102, #103, and #104. Issue #104 now protects current operation-v2 and
policy-v1 native admission and the private isolated runtime composition; it
does not implement this target ABI, select an end-user operation, or add a
general syscall/network sandbox.

The later migration adds v1 and migrates every repository operation and
installed consumer before deleting v2 in the same release. It leaves no
wrapper, alias, dual loader, forwarding header, v2-to-v1 adapter, missing-tail
interpretation, or runtime fallback. Until those implementation and test gates
pass, this section is a target and the preceding v2 sections remain current.

## Data-Definition Provider ABI v3

A data-definition provider exports exactly the two functions declared by the
self-contained C11/C++17 header:

```c
uint32_t ps_data_provider_get_abi_version(void);
ps_data_status_v3 ps_data_provider_get_api_v3(
    ps_data_provider_api_v3 *api);
```

The numeric handshake must return `PS_DATA_PROVIDER_ABI_VERSION`, exactly
three. The Host calls no other candidate function before equality succeeds.
It then supplies one pre-zeroed exact-size API table. The provider returns one
nonzero permanent provider identity, a bounded implementation version, a
bounded immutable typed definition array, one opaque context, and every
mandatory callback:

| Callback | V-14 responsibility |
| --- | --- |
| `validate` | Validate the complete Schema/Facet/Layout and checked multi-buffer payload. |
| `query` | Evaluate one pure metadata-only property request. |
| `evaluate_region` | Evaluate one pure bounded metadata-only Region request. |
| `evaluate_spec` | Evaluate one pure bounded metadata-only DataSpec relation. |
| `visit_content` | Append logical content bytes in provider canonical order. |
| `create_owner` / `destroy_owner` | Own one optional opaque object while its exact generation remains mapped. |
| `destroy_provider` | Perform final generation retirement before the module lease releases. |

This is the definition suite only. V-14 contains no access/map/import/transfer,
conversion, inference, execution, asynchronous completion, native-device, or
operation ABI replacement callback. Pure property, Region, and DataSpec calls
receive descriptor, Layout, envelope, byte-size, index, role, and allocation-
identity metadata, but every payload pointer and payload-available flag is
cleared. Payload is exposed only for explicit validation and canonical-content
traversal inside a retained call.

Every callback also receives one callback-local `ps_data_output_sink_v3`.
Borrowed `ps_data_bytes_v3` views are inputs only: diagnostics and BYTES
properties carry scalar `message_size` / `bytes_size` fields and copy their
complete variable-size field through the output sink while the provider source
is alive. A nonempty diagnostic uses the diagnostic channel exactly once; an
empty diagnostic does not use it. An Available BYTES property uses the property
channel exactly once even when empty. The Host checks channel permission,
pointer/count framing, duplicate use, the 4 KiB diagnostic bound, and the
64 KiB property bound before dereference, synchronously copies into one
per-invocation state, and treats the first sink failure as authoritative even
when provider code ignores it. Callback return, concurrent calls, generation
replacement, and module retirement therefore expose no delayed provider
output pointer.

The `ps_data_byte_sink_v3` used by `visit_content` is a separate synchronous
streaming channel, not one of those bounded diagnostic/property fields. The
Host may invoke the callback more than once for one digest: it first measures
with checked `uint64_t` accumulation, writes the frozen canonical field length,
then repeats the same active generation under the same immutable Value view and
payload read leases while hashing each segment directly. Each invocation owns
independent diagnostic and sticky-failure state. The provider must reproduce
the same logical byte sequence, although append-call boundaries may differ.
Null/nonzero pointer-count pairs, measurement overflow, ignored sink failure,
and measured/hash count drift are rejected. The cumulative stream is neither
materialized nor subject to the 4 KiB/64 KiB output bounds or an arbitrary
64 MiB content ceiling; only frozen SHA-256 length framing limits it.

The Region request remains rank-general at the ABI adapter. When a provider
returns Exact for a canonical nonempty TensorSlice, the Host computes the
checked `uint64_t` product of all half-open axis lengths. The provider's
`selected_site_count` must match exactly; product overflow, an incorrect
nonzero count, or zero for a nonempty slice returns InvalidDescriptor with
zero. Empty and non-Exact outcomes retain their existing typed semantics.

All records use fixed-width scalars, borrowed bounded input views, exact struct
sizes, zero-required reserved storage, and the platform C calling convention.
The supported profile requires 8-bit bytes, 8-byte data and function pointers,
and natural 8-byte alignment. The header freezes these v3 layouts:

| Record | Size | Alignment |
| --- | ---: | ---: |
| `ps_data_identity_v3` / `ps_data_bytes_v3` | 16 | 8 |
| `ps_data_definition_v3` / `ps_data_extension_v3` | 64 | 8 |
| `ps_data_buffer_view_v3` | 56 | 8 |
| `ps_data_buffer_envelope_v3` | 48 | 8 |
| `ps_data_value_view_v3` | 88 | 8 |
| `ps_data_diagnostic_v3` | 48 | 8 |
| `ps_data_property_query_v3` / `ps_data_property_result_v3` | 40 / 56 | 8 |
| `ps_data_region_request_v3` / `ps_data_region_result_v3` | 72 / 40 | 8 |
| `ps_data_spec_request_v3` / `ps_data_spec_result_v3` | 64 / 40 | 8 |
| `ps_data_byte_sink_v3` | 40 | 8 |
| `ps_data_output_sink_v3` | 40 | 8 |
| `ps_data_provider_api_v3` | 160 | 8 |

There is no tail-extension rule. The Host rejects an unexpected size, offset,
kind, enum, bound, pointer/count pair, required callback, reserved byte, or
duplicate typed key. Definition names are diagnostic lowercase ASCII
`[a-z][a-z0-9_.-]*`; permanent 128-bit identities plus nonzero structural
versions are authoritative. Schema, Facet, and Layout occupy separate typed
namespaces, so equal numeric identities across different kinds do not collide.

`DataDefinitionRegistry` is an injected C++ authority, not a global or
function-static singleton. It owns one publication mutex, one generation
source, one provider map, and three typed definition maps. Candidate loading
copies and validates the complete bundle, stages all next maps, and publishes
them together. ABI mismatch, callback failure, malformed metadata, duplicates,
or a typed key owned by another active provider publishes nothing and preserves
the previous generation. Provider callbacks never execute while the registry
mutex is held; same-thread registry mutation from a provider callback is
rejected.

Replacement is by permanent provider identity and publishes one fresh complete
generation atomically. Unload removes the active generation from new lookup
visibility. Existing `DataDefinitionLease`, provider-defined `Value`, indexed
`ProviderReadLease`, callback staging, and `ProviderOwner` values keep the
retiring callbacks, context, definitions, and module lease alive. Final owner
destroy runs once; final provider destroy runs after all generation users and
before the module lease releases.

If the last `ProviderOwner` or generation reference is released inside any
provider callback on the same Host thread, the Host does not recursively enter
the corresponding destroy callback. The owner/generation state embeds its own
cleanup node, so final shared release appends it to a per-thread FIFO without
allocating. The outer callback guard clears the active-callback fence and then
drains that FIFO after provider code returns, for both provider success and
failure and during normal C++ stack unwinding of the Host invocation. An owner
or generation released by a destroy callback or by cleanup member destruction
joins the FIFO tail; one iterative drain preserves existing FIFO order and
prevents recursive cleanup callback entry. Releases outside provider callbacks
use the same queue but drain synchronously. Owner state retains its exact
generation throughout `destroy_owner`, and generation state retains the module
lease throughout `destroy_provider`, including all callback-tail cascades.
Normal callback return empties the per-thread queue before normal thread exit;
a callback that never returns can still retain its generation indefinitely.
This Host-side lifetime repair changes no v3 record layout, callback signature,
or provider responsibility. V-14 provides no forced unwind or process
isolation.

## Data-Definition SDK Target and Linkage

A producer requests the `data_provider_sdk` package component and links
`Photospider::data_provider_sdk`. This interface-only target carries the
installed include directory and C11/C++17 compile features. It links no
operation runtime, static product, OpenCV, yaml-cpp, Threads, registry, loader,
executor, or device SDK. C11 and C++17 producers export the same two exact C
names; C++ declarations are `extern "C"` and `noexcept`.

A C++ Host-side consumer that instantiates `DataDefinitionRegistry` or creates
a provider-defined `Value` links `Photospider::operation_runtime` directly or
through `Photospider::operation_sdk`. Supplying platform-resolved function
pointers plus a nonnull module lease is an explicit composition responsibility;
V-14 installs no directory scanner or second mutable registry authority. The
dependency-disabled install smoke independently builds and executes exact-name
C11 and C++17 producers from the installed package, compiles independent output
record/sink layout assertions, emits a nonempty property from callback-local
storage, and loads each through the real registry transaction.

V-15 adds one separately requested installed component,
`openexr_deep_provider`. When
`PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER=ON`, requesting that component imports
`Photospider::openexr_deep_provider` and then discovers `OpenEXR::OpenEXR`;
requesting only neutral components does neither. With the default-OFF build,
an optional request reports the component unavailable and a required request
fails with a Photospider-owned component diagnostic before OpenEXR discovery.
The installed target is the provider MODULE only. Its source-private C++ codec
adapter is neither installed nor exported, and the provider still exposes
only the two frozen v3 C entry points.

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
2. normalize the absolute path, authorize the exact opened object as a signed
   `policy` artifact, and retain that capability;
3. open the authorized descriptor path eagerly and locally;
4. resolve and call only `ps_policy_plugin_get_abi_version`;
5. require exact ABI equality, then resolve and call
   `ps_policy_plugin_get_api_v1`;
6. validate the complete exact-size API table;
7. copy and validate every metadata row into a private map;
8. under the registry lock, reject every visible-name conflict, stage the
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

The three current extension boundaries and the accepted replacement target
intentionally have different compatibility and authority profiles:

| Boundary | Data ABI | Authority |
| --- | --- | --- |
| Operation plugin v2 | Provisional C++ registrar and callback values | Operation computation and returned values under Host validation |
| Operation plugin v1 target | Exact-size pure C, separately versioned suites, Host sinks, synchronous CPU grants | Future operation definition/planning/execution under Host validation; not yet installed |
| Data-definition provider v3 | Exact-size pure C definition-suite records under a frozen 64-bit profile | Schema/Facet/Layout validation and bounded semantic observation only |
| Policy plugin v1 | Exact-size pure C records under a frozen 64-bit profile | Ranking only; no resource or execution capability |

### Implemented V-2 through V-15 SDK and definition-provider subset

[ADR 0008](../adr/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
accepts separately versioned pure-C provider suites for Schema, Facet, Layout,
access, conversion, inference, query, region, digest, and execution. V-14
implements the exact v3 definition suite described above, together with public
C++ envelopes, `DataDefinitionRegistry`, generation leases, provider owners,
and provider-defined `Value` access. It places no STL, exception, RTTI, virtual
class, allocator ownership, or `Value` PImpl across the C boundary. The access,
conversion, inference, execution, asynchronous, and native-device suites remain
future separately bounded work rather than implicit v3 authority.

V-2 implements the dependency-neutral C++ CPU DenseTensor `Value`,
`StridedLayout`, `DenseTensorView`, and `ImageView` subset in
`operation_runtime`. V-3 adds installed BufferHandle ranges, read/write leases,
ValueBuilder sealing, byte offsets, bounded signed immutable views, and
process-local allocation/revision identity. The runtime is shared so the Host
and every Value-using DSO call one minting authority; the durable loader
regression opens two independent DSOs and proves both identity domains remain
distinct. One built-in operation uses that
surface behind a private dual-representation bridge: it preserves the sealed
result Value and derives an ImageBuffer compatibility snapshot.

V-4 adds installed `RegionSet` and bounded algebra to `operation_runtime`.
Region does not enter a new public v2 slot. A source-private bridge recognizes
only the exact core dense callback in the actual revisioned implementation
selected with execution's route-visible device inventory and request intent,
and invokes its Region-aware implementation. The bridge never performs a
scalar-only lookup or filters candidates to force core fallback; a same-key
device or plugin override selected by the route keeps ordinary complete-output
v2 behavior. Exact ImageRect may adapt current propagation callbacks, while
TensorSlice never crosses the rectangular v2 contract.

V-6 adds the installed dependency-neutral `ReadyFence` observer and
synchronous Value readiness to `operation_runtime`. Pending publication
authority and `ValueTransferTask` remain source-private, so an SDK consumer can
observe readiness but cannot create a pending producer or transfer task.

V-8 adds installed checked `DeviceBackend`, `DeviceId`, `MemoryDomain`,
`StorageBinding`, producer identity, and pure `AccessPlan` observation. Native
allocation construction, native handles, mutable pending-device producers,
completion admission, `ResidencyManager`, and CPU/Metal transfer submission
remain source-private. The repository Metal operation receives its borrowed
device/queue/allocator context only through the source-private invocation
boundary, publishes a pending native Value internally, and performs explicit
asynchronous readback. A third-party v2 callback receives no such context and
cannot infer one from `ImageBuffer::context`.

V-14 adds installed byte-preserving Schema/Facet/Layout envelopes, typed
property/DataSpec/Region outcomes, tagged canonical digests, artifact-envelope
serialization, the exact v3 C header, and one injectable C++ registry. The
runtime proves provider-defined multi-buffer Values with a dependency-neutral
synthetic `VariableSampleField` generation. Generic Host validation precedes
the provider callback and identity mint; pure calls expose no payload; content
traversal controls logical bytes but never owns the Host digest state; and old
Values, reads, callbacks, and owners survive atomic replacement and unload.
This slice neither installs a platform DSO scanner nor enters provider-defined
Values into graph compute, operation ABI v2, cache policy, or codecs.

V-15 supplies one repository-owned implementation of the same unexpanded
definition suite. The module publishes exactly four definitions—the
`VariableSampleField` Schema, `ImageFacet`, `DeepSampleFacet`, and deep
multi-buffer Layout—and validates their versioned payloads and complete Value
envelopes through the existing callbacks. Its explicit implementation-version
bytes are diagnostic; permanent provider/definition identities and structural
version govern interpretation. The module exports no codec entry point and
owns no registry, path policy, executor, cache, or commit policy. A
source-private adapter performs OpenEXR read/write and calls the ordinary
registry/Value APIs while the module lease is retained. Thus OpenEXR is one
optional codec/provider implementation, not new v3 authority or a fourth ABI
boundary.

None of these slices places Value, BufferHandle, leases, Region, ReadyFence,
device/access records, or a PImpl in a v2 callback record. V-14 adds the third
boundary in the table without changing the other two. Operation ABI v2 remains the current
operation contract until every
repository-owned operation and installed consumer has migrated. The completion
boundary then deletes v2, its entry point, SDK, fixtures, and package surface
without a permanent dual loader, wrapper, alias, forwarding header, or
v2-to-v1 shim. The replacement is the separately versioned operation-plugin
ABI v1 accepted by ADR 0012; it is not a provider-v3 suite. Policy ABI v1
remains independently versioned.

The operation C-linkage entry name is an identity/generation gate, not a stable
C data ABI. Binary compatibility still depends on the matching SDK, compiler,
standard library, C++ ABI, allocator/runtime, exception model, and RTTI
configuration.

The policy boundary uses only fixed-width scalars, opaque `void *` context,
borrowed immutable arrays, and C function pointers. Exact layout assertions and
validation make the supported profile explicit, but do not sandbox a hostile
DSO. A plugin still executes trusted native code in the Host process and can
block, corrupt memory, allocate outside accounting, or create unreported
threads. Signed immutable-snapshot admission constrains which bytes and role may
enter that process, but does not change those powers. The plugin simply
receives no legitimate execution capability through the ABI.

Issue #93 changes none of these three ABI inventories or record layouts. Its
`I1Host`, `ComputeRunObservationSink`, accepted-boundary collector, inner-row
evaluator, and exact runner are source-private benchmark/Host mechanisms. They
are absent from installed Host requests, operation registrar records,
data-provider v3 records, policy-plugin v1 records, SDK targets, IPC, and CLI.
The observer receives copied facts and an immutable final Value only; it is not
a fourth extension boundary and exposes no callback to a DSO.

Issue #94 likewise changes none of the installed ABI inventories, layouts,
symbols, or package components. `ProgressiveComputeOptions`,
`ProgressiveFinalGate`, I2 Host/profile/evidence types, and their observation
callbacks remain under `src/lib`; no field enters the installed Host request,
IPC or CLI grammar, operation registrar, data-provider v3 table, or policy-
plugin v1 table. The exact preview primitive is an internal CPU helper. The
rank-three HWC Metal upload is an internal generalization of the existing
process-owned executor and publishes through unchanged `Value`, `AccessPlan`,
residency, and ledger contracts; it exports neither native handles nor a new
provider callback. The manual runner and deterministic tests are consumers of
these private seams, not SDK or extension surfaces.

The installed `compute_content_digest(Value)` now handles built-in DenseTensor
values through a Host/runtime canonical-v1 implementation in addition to the
existing provider-defined traversal. The built-in path uses reserved
Schema/ImageFacet identities, descriptor metadata, and logical payload bytes;
it does not invoke or add a data-provider callback. Provider-defined values
continue to use the unchanged mandatory v3 `visit_content` callback. Therefore
the data-provider API table remains 160 bytes, every frozen v2/v3/v1 record and
symbol inventory remains unchanged, and no compatibility generation is
introduced.

Execution-profile evidence does not strengthen that trust boundary. A valid
`execution-profile-slo-v1` row freezes and hashes the exact in-process
operation/provider and policy generations, rejects unreported worker pools or
resource authorities, and treats allocations outside current ledger/device
authority as diagnostic rather than silently accounted memory. Its latency,
throughput, fairness, determinism, waste, and memory verdicts say nothing about
sandboxing or hostile-code containment. [ADR 0010](../adr/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.md)
defines that evidence boundary; process supervision and isolated invocation
remain the separate server/plugin-isolation target.

[ADR 0011](../adr/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md)
fixes that target boundary. The server control plane and WorkerManager load no
DSO. Every DSO loaded into a Host remains operator-trusted native code,
including pure-C policy and data-definition records. Tenant-supplied CPU
operation code instead crosses a separately versioned, attempt-scoped process
protocol carrying only bounded descriptors and shared-memory/file-descriptor
capabilities; trusted Host code revalidates every returned descriptor and
ownership claim. Pure C is a record-compatibility choice, not a sandbox.

The current Issue #102 slice realizes the transport portion through a
source-private Darwin/Linux protocol v1. It accepts only Ready, Host-visible,
unquantized NativeScalar Strided DenseTensor values, scalar parameters, and
checked dense-tensor output plans. A fresh one-call runtime receives no ABI
pointer record, Host callback, allocator, Graph/Run owner, or resource token;
its process-local callback seam does not call or migrate operation ABI v2 or
the target-only operation ABI v1. Direct use remains non-supervised and offers
no authentication, deadline, heartbeat, restart, or bounded hang recovery.
Issue #104 now adds signed package admission, Host resource admission, and
pre-exec OS limits to that direct entry, but not a general syscall/network
sandbox.

Issue #103 now realizes the supervised portion through source-private
`PluginRuntimeSupervisor` and `PluginInvocationExecutor`. A dedicated fixed-
size lifecycle protocol on a Unix datagram socket binds an OS-random 128-bit
nonce, the complete invocation identity, worker/plugin generations, a
Host-selected heartbeat interval, and strictly increasing event sequences to
the exact execed PID. Startup, invocation, heartbeat-gap, response, TERM, KILL,
and reap bounds use absolute monotonic deadlines. Complete request transfer
receives one independent full invocation-duration window and finishes only
after all bytes and descriptor rights are sent, Host `SHUT_WR` succeeds, and
the same absolute transfer deadline is observed again. The precise monotonic
sample that passes that observation is `accepted_at`; both the callback
invocation deadline and initial heartbeat-gap deadline derive directly from
it. A late successful shutdown is an invocation-deadline fault and cannot arm
fresh callback or heartbeat budgets; shutdown failure remains a channel fact.
Scheduling after acceptance consumes those budgets, and a later caller clock
sample cannot grant another window. Construction validates every configured
duration for positivity, the inclusive 24-hour cap, exact steady-clock
representation, and heartbeat ordering before child ownership; it does not and
cannot validate a future base sum. Every actual deadline derivation checks its
same captured base against `time_point::max() - duration` before addition. An
exact fit is accepted. Before ownership, one tick beyond it fails closed
through a controlled exception. After ownership, a pre-cleanup lifecycle or
short exact-status-observation overflow maps to the current phase-typed fault
and exact cleanup; arithmetic failure while deriving a termination/reap-
cleanup or restart-backoff deadline instead preserves an already established
primary fault. If those cleanup deadlines are representable but the exact PID
remains unobservable at the final bound, sole ownership moves to the deferred
reaper and `ReapPending` deliberately outranks the earlier phase fact. The
supervisor never wraps, saturates, clamps, or resamples a base to manufacture
another window. A real channel/status-observation syscall failure remains
`Channel` when no stronger process or deadline fact is available.
The nonce proves only private launch/session binding and liveness; because the
child learns it, the protocol does not attest plugin truth or make returned
bytes trusted.

The supervisor preserves typed observable deadline, protocol, channel,
bad-output, natural-exit, signal, and escalation facts. It never attributes OOM
from wait status: `SIGKILL` is only memory-pressure-compatible. A fault closes
both channels, escalates exact-PID termination when required, keeps reap
ownership, and never falls back in-process. A later call waits bounded restart
backoff and starts another fresh process. Product-linked real-exec tests cover
that lifecycle. If exact child status is reaped while an authenticated
completion remains queued, the monitor drains the lifecycle queue before
classifying the status as exit without completion, then keeps the status for
the mandatory response/EOF/decode/publication checks. Zero exit alone never
authorizes success. The tests also compose the executor inside an
`ExecutionService` ready callback; the request boundary publishes only the
owning Run as Failed and the fixed worker executes a later unrelated Run.

Issue #104 now composes package trust and Host resource authority around both
maintained isolated entries. Before shared-memory, descriptor, socket, mapping,
or process effects, side-effect-free preflight derives a five-dimensional
`PluginResourceVector`: runtime processes, CPU slots, address-space bytes,
shared-memory bytes, and descriptor count. The attempt-local `ResourceLedger`
atomically mints a move-only token bound to a domain-separated digest of the
complete invocation identity and exact vector. Consumption against equal
facts creates one RAII lease; replay tombstones survive settlement for the
ledger lifetime, while every unconsumed token or consumed success/failure path
returns the exact capacity once. No token, trust root, manifest, signature, or
digest override enters the invocation wire.

The admitted vector drives `RLIMIT_AS`, positive `RLIMIT_CPU`, checked
`RLIMIT_NOFILE`, and zero `RLIMIT_CORE` before native exec. The child still
receives an empty environment, `/dev/null` standard streams, fixed data/status/
lifecycle descriptors, one fixed executable descriptor, and only the declared
invocation capabilities; descriptors above that closed set are removed. Linux
executes the post-copy verified sealed descriptor with `fexecve`. Darwin has no
supported anonymous-descriptor runtime exec, so authorization reports
`ExactObjectUnsupported` during direct or supervised executor construction,
before token issuance, capability materialization, socket creation, or fork;
no runtime pathname snapshot exists. Unsupported platforms, including the
current Windows runtime profile, also fail closed instead of falling back to an
unverified path. These controls impose
package identity and resource ceilings; they do not provide a general syscall
or network sandbox, and a `SIGKILL` observation remains only
memory-pressure-compatible rather than proof of OOM.

The adapter, endpoint, supervisor, and executor are compiled into the
installable product archive, but no current operation loader or product
composition root constructs an isolated request for an end-user Graph
operation. In particular, `ExecutionService`, `WorkerManager`, the embedded
Host/CLI, and `photospider-worker` have no selection caller. Operation ABI v2
and target-only v1 remain absent from this wire. Atomic operation-ABI migration
and end-user selection, stronger platform sandbox profiles, and long-lived
fuzz/audit evidence remain separately owned work.

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
- Data-definition providers include
  `photospider/plugin/data_provider_api.h`, request `data_provider_sdk`, link
  `Photospider::data_provider_sdk`, and export only
  `ps_data_provider_get_abi_version` plus `ps_data_provider_get_api_v3`.
- Definition providers fill exact-size records, preserve every zero-required
  reserved field, retain callback metadata until final generation destroy, and
  never retain borrowed per-call views. C++ registry consumers link
  `Photospider::operation_runtime`; no installed provider scanner is implied.
- The repository OpenEXR provider is available only through the separately
  requested `openexr_deep_provider` component. It exports the same two v3
  symbols, never puts OpenEXR types in public records, and is absent—including
  dependency discovery and target exports—from default-OFF installations.
- Policy plugins include
  `photospider/policy/policy_plugin_api.h`, request the `policy_sdk`
  component, and link `Photospider::policy_sdk`.
- Policy plugins export the exact two v1 symbols, fill exact-size records,
  preserve every Host-initialized prefix/reserved field, and return only
  declared status/enum values.
- Policy callbacks retain no snapshot memory and treat every candidate id as
  opaque. They never create workers or claim that selection starts work.
- There is currently no installed operation-v1 header, loader, SDK, or dual-
  compatibility path. The accepted v1 target replaces and deletes v2 only
  after the complete migration gate; scheduler SDK/ABI, `IScheduler` adapter,
  and execution-route plugin ABI remain absent.

## Implementation and Validation Entry Points

- `include/photospider/data/value.hpp`
- `include/photospider/data/extension.hpp`
- `include/photospider/core/device.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/memory/access_plan.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/plugin_api.hpp`
- `include/photospider/plugin/data_definition_registry.hpp`
- `include/photospider/plugin/data_provider_api.h`
- `include/photospider/plugin/op_contract.hpp`
- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/core/value.cpp`
- `src/lib/core/dense_tensor_content_digest.*`
- `src/lib/core/extension.cpp`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/plugin/operation_host_adapter.*`
- `src/lib/execution/device_completion.*`
- `src/lib/execution/isolated_cpu_invocation.*`
- `src/lib/execution/plugin_runtime_supervisor.hpp`
- `src/lib/execution/residency_manager.*`
- `src/lib/execution/value_transfer_task.*`
- `src/lib/execution/metal_device_executor.{mm,stub.cpp}`
- `src/lib/compute/progressive_compute.*`
- `src/lib/benchmark/i2_host.hpp`
- `src/lib/benchmark/i2_profile.*`
- `src/lib/benchmark/i2_evidence.*`
- `src/lib/plugin/plugin_loader.*`
- `src/lib/plugin/plugin_manager.*`
- `src/lib/plugin/data_definition_registry.cpp`
- `plugins/data/openexr_deep_scanline_provider.cpp`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `src/lib/policy/policy_registry.*`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_plugin_manager.cpp`
- `tests/unit/test_op_registry_m31.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_metal_device_executor.cpp`
- `tests/unit/test_device_residency.cpp`
- `tests/fixtures/value_identity_dso.cpp`
- `tests/integration/test_value_identity_dso.cpp`
- `tests/unit/test_dense_tensor_content_digest.cpp`
- `tests/integration/test_variable_sample_field_extensions.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `tests/integration/openexr_deep_provider_option_off_smoke.py`
- `tests/integration/dependency_disabled_install_smoke.py`
- `tests/integration/static_product_consumer_smoke.py`
- `tests/integration/test_i2_product_path.cpp`
- `tests/integration/test_plugin_runtime_supervisor.cpp`
- `tests/unit/test_progressive_compute.cpp`
- `tests/integration/graph_cli_plugin_compute_smoke.py`
