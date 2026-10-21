---
spec_schema_version: 1
id: NUM-13A
parent_id: NUM-13
function: prefix_sum
proposed_operation_keys:
  - numeric.prefix_sum_strict
  - numeric.prefix_sum_accelerated_apple_silicon
  - numeric.prefix_sum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-13A: prefix_sum

The three `numeric.prefix_sum_<profile>` keys accept one Result tensor member under any member key. Output port `values` uses `photospider.tensor` with member `samples`. The output takes the complete input `sample_shape()`, including batch axes, as ordinary axes and extends the selected axis from N to N+1. Boundary k sums source positions [0,k); k=0 is zero and k=N includes the complete line. Other axes retain their extents. No inclusive/exclusive mode or image-role inference is provided.

Input rank is 1..8, extents are positive and both input/output logical counts are at most 2^40. Required static Int64 `axis` is a nonnegative index below rank. Required String `dtype` selects `uint8`, `int64`, `float32` or `float64` in the same integer/floating domain as the input. The public helper defaults to Int64 for integer inputs and Float64 for floating inputs. Shape addition, products, axes and dtype compatibility are checked before execution.

## Exact prefix semantics

Inherit [reduce_sum's arithmetic rules](NUM-11A_reduce_sum.md). Each boundary is the exact sum of its original source interval, followed by one destination conversion. Integer final representability is an unchecked caller precondition. Strict floating conversion is correctly rounded and bitwise reproducible. Accelerated profiles follow the [shared floating bound](NUM_accelerated_contract.md); discrete results and special-value classification remain exact. These fixed tasks use the shared exact accumulator and require CPU Host placement.

Empty mathematical prefixes produce integer zero or floating +0. That padding is not an arithmetic contribution: a nonempty prefix of only negative zeros produces -0. Source NaN priority follows logical axis order, with the sum contract's quieting and payload conversion. Mixed infinities follow the sum rules. A generated NaN from an earlier boundary never becomes a source element; `[+Inf,-Inf,NaN_payload]` produces `[+0,+Inf,canonical_NaN,quiet_payload_NaN]`.

Each declared output boundary is converted independently. The exact accumulator may exceed the destination range. Floating overflow produces infinity without contaminating later carry. For Int64 `[INT64_MAX,1,-1]`, a freshly compiled tile containing only boundary 3 can return `INT64_MAX`; any declared tile containing boundary 2 violates integer representability. No runtime diagnostic is promised for invalid integer inputs.

## Selection and resources

Requested output tiles with equal non-scan-axis origins and extents form one fixed task. Its source covers those non-scan coordinates and the prefix ending at its largest required positive boundary. The task reads all input tiles intersecting this region and writes its selected dense output tiles. A group containing only boundary zero has no input payload dependency and no arithmetic scratch. Empty selection emits no task. Tile granularity can require more source than a single mathematical boundary.

`ExecutionPlan::select_outputs` retains whole compiled tasks. Keeping one output tile from a multi-write task retains its other writes, reads and scratch, and integer representability applies to all retained writes. A fresh compilation with smaller output requests can shrink the prefix. The final Result exposes only selected output coverage.

Each source sample is visited once per group. A task uses two fixed exact workspaces in preallocated private scratch: one aggregate and one conversion snapshot. Output storage consists of declared dense tiles; there is no complete intermediate output. Prepare validates the complete immutable invocation signature. Concurrent Runs use separate mutable arithmetic state. Cancellation checks cover initialization, sample reads and exact arithmetic; there are no per-value diagnostics or sample work-limit counters. Successful Result owners outlive the execution context.

## Acceptance and implementation

Temporary checks compare `[1,2,3] -> [0,1,3,6]` and independent exact enumeration, including batch/nonadjacent axes, same-domain dtypes, negative/zero strides, special values, partial/empty selection, zero-only requests, artifact roundtrips, concurrent Runs and cancellation. Verify that an upstream tail failure affects a full scan but does not enter a freshly compiled earlier prefix when tile boundaries exclude the tail. Invalid schema/static parameters and malformed kernel signatures must fail before invocation.

The public helpers are in `photospider/ops/numeric/scans.hpp`. See [fixed scan execution](../scans-execution.md) for task grouping, ownership and validation. This family overrides the [NUM baseline](NUM_common_contract.md) where fixed execution or unchecked numerical preconditions differ. Proposed specification status is unchanged.
