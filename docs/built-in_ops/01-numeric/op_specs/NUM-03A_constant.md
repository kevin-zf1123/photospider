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

Constant consumes a dynamic scalar Result with complete sample shape `[1]` and fills a statically declared target shape. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

The input Result contains one tensor under any schema/member key. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are supported; the output dtype is inferred from the scalar. Required static String `shape` is a canonical comma-separated list of 1..8 positive extents, with product at most 2^40. Output port `values` uses `photospider.tensor`/`samples` with ordinary target axes. The SDK helper `constant_node` takes the CPU profile immediately after shape. Invalid shape/parameters return InvalidArgument; incompatible scalar metadata returns TypeMismatch.

## Mathematics

Every output element equals the scalar. The output preserves every scalar bit, including signaling/quiet NaN payloads, infinities and signed zeros. No arithmetic or conversion is performed; all three CPU profiles have identical raw-bit semantics.

## Behavior

Every selected output element depends on the scalar at index zero. Empty selection reads nothing. Only selected output tiles are produced, so a sparse request over a 2^40-element logical shape stays small; a full request produces the full dense payload. Output owners do not retain the scalar's source allocation. Unsupported profiles fail before invocation.

## Acceptance

For Int64 scalar 7 and shape `[2,3]`, expect six sevens. Cases: all 256 UInt8 values, all seven dtypes, integer extrema and raw IEEE specials, rank one and eight, sparse requests at the 2^40 limit, malformed shapes and scalar metadata, and a tile exceeding 64 KiB.
