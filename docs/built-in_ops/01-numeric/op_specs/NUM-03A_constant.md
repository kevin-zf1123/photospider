---
spec_schema_version: 1
id: NUM-03A
parent_id: NUM-03
function: constant
operation_family: numeric.constant
proposed_operation_keys:
  - numeric.constant_strict
  - numeric.constant_accelerated_apple_silicon
  - numeric.constant_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-03A: constant

Constant consumes a dynamic scalar Result with complete sample shape `[1]` and fills a statically declared target shape. The Result contains one tensor under any schema/member key. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are supported. Output preserves every scalar bit, including signaling/quiet NaN payloads, infinities and signed zeros. No arithmetic or conversion is performed; all three CPU profiles have identical raw-bit semantics.

Required static String `shape` is a canonical comma-separated list of 1..8 positive extents, with product at most 2^40. Dtype is inferred from the scalar. Output port `values` uses `photospider.tensor`/`samples` with ordinary target axes. The old layout policy is removed from direct nodes and the SDK; `constant_node` accepts profile immediately after shape.

## Fixed tasks and storage

Every selected output tile becomes one CPU task that reads scalar index zero once and fills a separate preallocated dense allocation. It initializes one element, grows the initialized prefix and then copies blocks of at most 64 KiB. Each memcpy source/target interval is disjoint and confined to that output tile. Empty selection emits no task or payload read.

Only selected output tiles are allocated and filled. A sparse request over a 2^40-element logical shape can remain small; a full request must admit the full selected dense payload. There is no scalar-backed zero-stride output path. Negative/zero strides and unaligned scalar offsets are supported after physical validation. Output owners do not retain the scalar's source allocation and outlive context destruction.

Preparation checks metadata, all input/output views, empty task parameters and zero scratch. Runtime uses bounded stack state and allocates no payload. Cancellation is checked before reading and between copies of at most 64 KiB. Operational resource/cancellation failures prevent successful call publication. Unsupported profiles fail before invocation. Invalid shape/parameters return InvalidArgument; incompatible scalar metadata returns TypeMismatch. Per-sample diagnostics, work counters and content caches are unavailable.

## Acceptance

For Int64 scalar 7 and shape `[2,3]`, expect six sevens in selected independent dense tiles. Temporary checks cover all 256 UInt8 values, all seven dtypes, integer extrema and raw IEEE specials, rank one/eight, sparse requests at the 2^40 limit, malformed shapes and scalar metadata, source strides/offsets, concurrent Runs, artifact roundtrips, cancellation and owner lifetime. Include a tile exceeding 64 KiB to exercise the bounded copy loop. See [fixed array execution](../arrays-execution.md). This execution contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.
