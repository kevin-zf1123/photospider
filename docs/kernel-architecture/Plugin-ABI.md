# Operation and Data-Definition ABI

Photospider installs two narrow same-trust extension headers:

- operation ABI v2: copied semantic traits, closed typed parameter schema,
  plan-derived input demands, and one synchronous Value callback;
- data-provider ABI v1: copied schema key, element type, and maximum rank.

## Operation records

An operation descriptor has a length-framed key, input count, flags, estimated
bytes, output element type, closed scalar/preserve/match/fixed shape and
Whole/Elementwise/Halo Region rules, halo radius, cacheability, a bounded
parameter-schema pointer/count, callback, and opaque plugin state. Parameter
records publish unique keys, exact Int64/Float64/Bool/String types, and required
presence. The compiler rejects unknown, missing, wrong-type, and conflicting
parameters before semantic IR; callbacks receive only validated canonical
values and there is no hidden default fallback.

The callback receives bounded dense whole-Region input views, per-input planned
demand offsets/extents, bounded facet arrays, backend enum, cooperative
cancellation callback, and a host-owned output sink. It may publish at most one
output with bounded facets, which the host copies and validates as a dense
whole-Region Value. A DSO input view covers exactly its logical contiguous
bytes; trailing backing bytes are rejected instead of becoming invisible
plugin state.

The synchronous callback retains its `int` signature but returns one closed
version-two result: success, ordinary failure, cancellation, or backend
unavailable. Backend unavailable is distinct from ordinary failure and may
request CPU fallback only from a GPU attempt whose copied traits allow it.
Unknown nonzero integers are ordinary `OperationFailed` results. A callback
reporting backend unavailable must not invoke the output sink. If it does, an
accepted output is a terminal `OperationFailed` contract violation and a
rejected output retains the sink's exact typed failure. Neither path exposes
`BackendUnavailable` or triggers CPU fallback, while host cancellation remains
the highest-priority result.

A C++ `OperationTraits::Fixed` record describes only the logical output
descriptor. Registration validates a nonzero rank-1..8 shape, closed element
type/rules, and the ordinary trait combinations without evaluating a dense
element or byte product. The callback may return any Value layout that passes
normal publication validation, including an eight-byte zero-stride broadcast
over a huge logical shape. `estimated_bytes` is an independent modeled
admission estimate. A C DSO Fixed descriptor is stricter because ABI v2 carries
no output strides: loading separately requires representable contiguous
signed strides and uint64 byte count. For total dense bytes `B`, the loader
also requires `B > 0`, zero-based last byte `B - 1 <= INT64_MAX`, and
`B <= SIZE_MAX`. Thus a UInt8 `{INT64_MAX + 1}` descriptor and
`{2, 2^62}` are representable on a 64-bit host, while adding one element to
either boundary is rejected. This distinction adds no ABI field and does not
change Preserve or Match inference.

## Validation

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
structure bytes and publishes no v1 compatibility entry point.

Malformed registration publishes nothing. Multi-record registry publication
uses copy-then-swap, so allocation failure cannot expose a prefix. An embedding
C++ operation callback's `std::bad_alloc` propagates so the caller can preserve
resource-exhaustion policy. Every other `std::exception` becomes
`OperationFailed`; a null `what()` pointer is normalized to an empty diagnostic
without constructing a string from null. Nonstandard exceptions receive a
stable generic `OperationFailed` diagnostic. Output is copied before callback
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

Paths come only from embedding-process startup configuration. Registries are
assembled and then frozen before compiler/executor use. DSOs execute in process
with the same trust as the host. ABI checks are correctness validation, not a
sandbox, signature, certificate, package-admission, or process-isolation
system.

There is no policy ABI/SDK/DSO, external scheduling plugin, or plugin path over
IPC. The data-definition ABI does not construct Values or provide storage.
