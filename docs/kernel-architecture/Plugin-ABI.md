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

## Validation

Loading validates exact ABI version/structure sizes, pointer and array
alignment, pointer/count pairs, bounded key/count/rank/parameter values, text
bytes, duplicate parameter declarations, closed enum/flag/type combinations,
required callbacks, output element/shape/byte count, facet arrays/key/version/
payload, arithmetic overflow, and exactly-once destroy ownership. This ABI
version rejects trailing structure bytes and publishes no v1 compatibility
entry point.

Malformed registration publishes nothing. Multi-record registry publication
uses copy-then-swap, so allocation failure cannot expose a prefix. Operation
exceptions are fenced; output is copied before callback return. Plugin-owned
descriptor tables are destroyed before library unload.

Immediately after native open, a move-only stack owner holds each operation or
provider handle. Once an exact API structure prefix is readable, that owner
also assumes its available destroy callback. Symbol, table, schema, heap-owner,
or later staging failure therefore calls every acquired destroy callback and
native close exactly once. Successful loading explicitly moves the same owner
into the published heap lease.

## Lifecycle and boundary

Paths come only from embedding-process startup configuration. Registries are
assembled and then frozen before compiler/executor use. DSOs execute in process
with the same trust as the host. ABI checks are correctness validation, not a
sandbox, signature, certificate, package-admission, or process-isolation
system.

There is no policy ABI/SDK/DSO, external scheduling plugin, or plugin path over
IPC. The data-definition ABI does not construct Values or provide storage.
