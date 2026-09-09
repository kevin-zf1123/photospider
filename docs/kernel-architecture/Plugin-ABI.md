# Operation and Data-Definition ABI

Photospider installs two narrow same-trust extension headers:

- operation ABI v7: copied semantic traits, closed typed parameter schema,
  ordered scalar/image port constraints, plan-derived input demands, and one synchronous Value callback;
- data-provider ABI v1: copied schema key, element type, and maximum rank.

The installed C++ convenience wrapper `operation_plugin.hpp` is a direct,
self-contained header: it includes its own `<cstdint>` dependency and the
operation C ABI before exposing `element_type_value`. It does not depend on a
consumer including another Photospider header first. The exported
`Photospider::operation_sdk` interface target propagates `cxx_std_17`, so a
C++ consumer receives the wrapper's actual language requirement from the
package instead of having to restate it privately. The maintained consumer
proves that propagation with a compiler-appropriate dialect assertion:
`_MSVC_LANG` when an MSVC-compatible frontend defines it, and `__cplusplus`
otherwise. It adds no private standard flag and does not require
`/Zc:__cplusplus`.

The provider contract remains pure C. `Photospider::data_provider_sdk`
propagates include directories but no C++ compile feature, so a C11
translation unit may consume it without acquiring a C++ language requirement.
The package does not publish a separate `data_definition_sdk` alias.

## Operation records

An operation descriptor has a length-framed key, input count, flags, estimated
bytes, output element type, closed scalar/preserve/match/fixed shape and
Whole/Elementwise/Halo Region rules, halo radius, cacheability, a bounded
parameter-schema pointer/count, input-schema pointer/count, output port constraint,
callback, and opaque plugin state. Parameter
records publish unique keys, exact Int64/Float64/Bool/String types, and required
presence. The compiler rejects unknown, missing, wrong-type, and conflicting
parameters before semantic IR; callbacks receive only validated canonical
values and there is no hidden default fallback.

The callback receives regional strided input views with separate storage origin,
byte offset, signed strides, valid coverage and planned demand. Pointers expire
at callback return. The output sink declares exact output descriptor/Region and
packed byte size; allocate_output supplies host-owned bytes, and allocate_scratch
supplies host-owned callback-local scratch. Publishing the allocated output
freezes it without copying. Publishing stack/caller data copies it through the
same host allocator. Callbacks must allocate computation buffers through the
host, and must not free or retain borrowed pointers.

The first publish attempt claims the sink even on failure. A second attempt
sets a sticky violation and cannot replace the first result. It produces
OperationFailed after the callback, subject to host cancellation priority.
Null context has no side effects. Resource allocation failures remain typed.
Generic input views may include backing padding; callbacks address only valid
coverage via the supplied origin and strides.

The synchronous callback retains its `int` signature but returns one closed
version-seven result: success, ordinary failure, cancellation, or backend
unavailable. Backend unavailable is distinct from ordinary failure and may
request CPU fallback only from a GPU attempt whose copied traits allow it.
Unknown nonzero integers are ordinary `OperationFailed` results. A callback
reporting backend unavailable must not invoke the output sink. If it does, an
accepted output is a terminal `OperationFailed` contract violation and a
rejected generic output retains the sink's exact typed failure. A malformed
image output is OperationFailed; explicit callback cancellation and resource
exhaustion retain their categories. Neither path exposes
`BackendUnavailable` or triggers CPU fallback, while host cancellation remains
the highest-priority result. After that cancellation check, a duplicate sink
violation outranks success, backend unavailability, ordinary failure,
callback-reported cancellation, and unknown results. It therefore never
publishes the first Value or requests CPU fallback.

Before any C++ or DSO callback entry, `OperationRegistry::invoke` validates
the operation/input/demand counts, checks each input `Value::valid()` before
reading its descriptor, validates every demand and parameter, observes host
cancellation, rejects any backend value other than CPU or GPU, and then checks
backend capability. A known but unsupported backend remains
`BackendUnavailable`; an unknown numeric backend is `InvalidArgument` and is
never translated to GPU by the DSO adapter. After those higher-priority
checks, the registry uses `resolve_operation_traits` and `infer_operation_output`
to compute dtype, shape and canonical output facets through the same implementation
as semantic lowering. Preserve/Match compare shapes independently of dtype;
input dtype restrictions are explicit port constraints. A mismatch is rejected
before callbacks. Callback output validation
reuses the precomputed descriptor; a successful callback that returns a
default-invalid generic `Value` remains a safe `TypeMismatch`; invalid image
output is `OperationFailed`. Direct invocation and physical planning share
the checked input-demand rule. The registry rejects insufficient Value/halo
coverage, partial-channel image output and mismatched mask shape before
callback entry. Computed mask numeric failures are OperationFailed; bound
mask numeric failures remain InvalidArgument.

A C++ `OperationTraits::Fixed` record describes only the logical output
descriptor. Registration validates a nonzero rank-1..8 shape, closed element
type/rules, and the ordinary trait combinations without evaluating a dense
element or byte product. The callback may return any Value layout that passes
normal publication validation, including an eight-byte zero-stride broadcast
over a huge logical shape. `estimated_bytes` is an independent modeled
admission estimate. A C DSO Fixed descriptor is stricter because ABI v7 carries
no output strides: loading separately requires representable contiguous
signed strides and uint64 byte count. For total dense bytes `B`, the loader
also requires `B > 0`, zero-based last byte `B - 1 <= INT64_MAX`, and
`B <= SIZE_MAX`. Thus a UInt8 `{INT64_MAX + 1}` descriptor and
`{2, 2^62}` are representable on a 64-bit host, while adding one element to
either boundary is rejected. The copied `requires_dense_output` trait also checks the complete output using
the resolved dtype before semantic IR publication, including Fixed outputs
whose dtype comes from an input or static parameter. C++ Fixed broadcast
semantics remain available when that requirement is false.

## Validation

Before any Windows, Linux, or Darwin native-loader call, operation and provider
loading validates the exact `std::string` path as nonempty, at most 4096 bytes,
and free of embedded NUL. A malformed path is `InvalidArgument`; a legal exact
path that the platform cannot load remains `NotFound`. No truncated prefix is
opened, no registry key/schema is published, and no native-owner lifecycle is
started for a rejected path.

Loading validates exact ABI version/structure sizes, pointer and array
alignment, pointer/count pairs, bounded key/count/rank/parameter values,
strict UTF-8 operation/parameter-schema/provider-schema keys, duplicate
parameter declarations, closed enum/flag/type combinations, required
callbacks, logical C++ fixed descriptors, dense C DSO fixed stride/byte
representability including signed last-byte and host allocation-size bounds,
output element/shape/byte count, facet arrays/key/version/payload, arithmetic
overflow, and exactly-once destroy ownership. Key
validation rejects invalid continuation bytes, truncation, overlong encodings,
UTF-16 surrogates, values above U+10FFFF, embedded nulls, and ASCII controls
before publication; it does not normalize Unicode. Ordinary facet payload and
Value bytes remain opaque binary data. This ABI version rejects trailing
structure bytes and publishes no old operation compatibility entry point.

Malformed registration publishes nothing. A built-in/embedding definition and
every DSO definition are fully constructed before publication, then retained by
a private immutable owning handle. The registry map, DSO transaction staging,
and invocation snapshot copy only that handle: no embedding callable copy or
execution occurs while the registry mutex is held. Multi-record publication
uses copy-then-swap over the handle map, so allocation failure cannot expose a
prefix; the replaced map retires after unlock. An invocation's handle keeps its
callback and any captured DSO lease alive until callback completion. An
embedding C++ operation callback's `std::bad_alloc` propagates so the caller can
preserve resource-exhaustion policy. Every other `std::exception` becomes
`OperationFailed`; a null `what()` pointer is normalized to an empty diagnostic
without constructing a string from null. Nonstandard exceptions receive a
stable generic `OperationFailed` diagnostic. Output is frozen or copied before callback
return. Plugin-owned descriptor tables are destroyed before library unload.

Dense layout products use checked uint64 division before multiplication, then
validate the complete byte range. Boundary fixtures load real DSOs and require
transactional rejection when a later descriptor is unrepresentable, with one
destroy and one native close. A compile-time-width helper instantiates the
32-bit allocation-size path even on a 64-bit test builder.

Immediately after native open, a move-only stack owner holds each operation or
provider handle. Once an exact API structure prefix is readable, that owner
also assumes its available destroy callback. Symbol, table, schema, heap-owner,
or later staging failure therefore calls every acquired destroy callback and
native close exactly once. Successful loading explicitly moves the same owner
into the published heap lease.

The provider registry declares native leases before copied schema records so
reverse destruction retires every registry-owned schema before the final
provider destroy callback and native unload. A `find()` result owns its copied
key and contains no DSO pointer, so it may outlive registry teardown without
borrowing mapped provider memory.

## Lifecycle and boundary

Paths come only from embedding-process startup configuration and are consumed
as exact NUL-free byte sequences. Registries are assembled and then frozen
before compiler/executor use. DSOs execute in process with the same trust as
the host. ABI checks are correctness validation, not a sandbox, signature,
certificate, package-admission, or process-isolation system.

There is no policy ABI/SDK/DSO, external scheduling plugin, or plugin path over
IPC. The data-definition ABI does not construct Values or provide storage.

## Version-seven semantic and output contracts

Package 0.7.0/operation ABI and traits 7 replace 0.6/6. The host checks version
before `get_api_v7`; no old table, symbol alias or image-v1 reader remains.
WorkflowDocument schema 2/provider ABI 1/C++17 remain.

`SemanticDescriptor` in `data/semantic.hpp` encodes image-v2 and semantic-v1
facets with a 4096-byte canonical payload. Helpers construct RGBA/coverage
semantics, validate metadata and regional samples, and convert to/from the
8192-character lowercase-hex static `semantic` parameter. Callers use typed
helpers rather than writing hex. Channel names/roles/units, color/white/transfer/
reference/association and sampling axis/value units are separate. Images are
Float32 HWC; finite signed/HDR values are supported by typed image validation.
Vector coordinate tokens distinguish pixel/normalized displacement/position;
complex fields declare full unshifted spectra, DC zero, negative unnormalized
forward transform and inverse /N. This describes data without implementing FFT.
Generic opaque facets and unrestricted generic floating bytes remain available.
Malformed known typed facets are rejected when constructing Value metadata.

Each C port has an optional exact-sized semantic constraint record for kind,
facets, dtype and rank. `element_type_mask` optionally accepts a dtype set:
low bits 0..3 mean UInt8/Int64/Float64/Float32, zero is unrestricted, and it is
mutually exclusive with nonzero exact `element_type`. Unknown bits and conflicting
fields reject registration. The copied mask enters compiler and result identities.
An optional operation contract selects declared/input/
static-parameter dtype, rank-1..8 axes (constant, positive Int64 parameter,
input axis or actual input count) with checked nonnegative offset, and output
semantics (drop, preserve input, establish facets or static semantic parameter).
The fixed input prefix may be followed by one homogeneous group; active groups
have minimum>=1 and bounded maximum, with total input count <=1024. The loader
copies all records before atomic publication. Lowering expands a template to
its exact ordered input table. New axes, typed ports and repeated groups use
Whole. No per-port G4 spatial inference is introduced.

SemanticNode and PlanStep retain real output facets. The C sink supplies the
same resolved dtype/shape/facets to callbacks. Published output is validated
against these facts; a typed facet mismatch is OperationFailed. Drop removes
known typed semantic guarantees; unrelated opaque generic facets retain their
existing publication rules. Complete constraints and output rules enter v7
compiler identities and v3 result-region keys.

The shared contract and the eight existing operations now support image-v2
signed/HDR RGB with canonical coverage-premultiplied D65 semantics, including
eligible native Metal execution. The eight operations explicitly preserve the
first input's semantic facet. Snapshots and memory/native/disk caches retain
supported image-v2 representations and real canonical facets; disk format 2
rejects old formats. Bounded scalars accept compatible computed Float32 `{1}` results, including
dimensionless Scalar/single-sample Signal facets. Each consumer validates its
range before callback entry, including cache hits; direct bindings retain
preflight checks. Logical scalar addresses support padded and strided views.
The default registry also supplies the CPU Whole [numeric operations](Numeric-Operations.md). The general typed image validator also accepts straight
representations; the eight existing operation ports require canonical RGBA.

## S3 scaled ports

ABI 7 includes the S3 Shrink shape/Region rules and a required spatial_factor_parameter pointer/length pair. The bounded Int64 parameter resolves in [1,16], producing ceil-divided H/W and clipped box input demand. Masks can be outputs. Unknown layouts, pointer/count mismatch, invalid bounds and old ABI 6 fail before publication.

## S4 host GPU service

The ABI 7 output sink carries an invocation-local ps_gpu_service_v7 pointer,
null on CPU. buffer() creates a bounded token for host allocation and cannot
promote frozen inputs to writable. execute() validates source/entry, bindings,
constants and grid, and returns only after native completion. Failures are
sticky and override callback success. Tokens and pointers expire at callback
return. Submitted device execution errors terminate the Run; unpublished numeric
or backend rejection may use trait-permitted CPU fallback. Both built-ins and
the independent C module use this service without exposing Objective-C types.
See the public C header and [S4 Workflow](S4-Workflow.md).
