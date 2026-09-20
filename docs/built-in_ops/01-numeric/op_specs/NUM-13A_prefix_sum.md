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
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-13A: prefix_sum

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Produce boundary prefix sums along required static Int64 axis. For an input
axis length N, output axis length is N+1; output[k] sums input axis positions
[0,k). The first boundary k=0 is zero, and k=N includes the whole line. Other
axis lengths and rank are preserved. There is no inclusive/exclusive mode.

Input is named input and output values, with empty output facets. Shapes are
rank 1..8 with positive extents and input/output logical count <=2^40. Axis is a
nonnegative index less than rank. Shape addition and product are checked before
execution. This generic numeric scan does not infer physical units or image roles.

## Dtype and exact prefix semantics

Inherit [reduce_sum's dtype and numeric rules](NUM-11A_reduce_sum.md): four input
dtypes, required static dtype with integer default Int64 or floating default
Float64, explicit same-domain destination selection and one final range check
or correctly rounded conversion. Each requested prefix is the exact sum of its
own source interval, not a recurrence on previously rounded output values.
Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.

Empty prefixes yield integer zero or floating +0 without reading input values.
For nonempty prefixes, source NaN priority follows original logical axis order,
with shared quieting/payload conversion. Mixed signed infinities and finite zero
signs follow reduce_sum. The leading +0 is a boundary output, not an extra
arithmetic contribution: an all-negative-zero nonempty prefix yields -0.

A newly generated NaN at an earlier prefix is not a source element for later
prefixes. For source [+Inf,-Inf,NaN_payload], outputs are [+0,+Inf,canonical_NaN,
quiet_payload_NaN], following the actual source NaN priority at each boundary.

Only requested prefixes undergo destination representability checking. For
Int64 [INT64_MAX,1,-1], requesting only k=3 succeeds with INT64_MAX; unrequested
k=2 does not cause an overflow failure. A request including k=2 fails its own
observation, without poisoning an independently completed valid prefix request.

## Dependency, state and invalidation

For output coordinate o with k=o[axis], exact source Data support matches its
other axes and spans [0,k) on axis. Union these supports over requested Q;
only the largest requested k per line needs a source scan. Other lines and
positions >=k are not read. Add typed Validation closure separately. Empty Q,
or requests solely at k=0, read no source Data.

Use exact accumulated state, infinity classification and earliest source NaN
metadata while scanning bounded blocks. Never use rounded output as continuation
state or reject an unrequested prefix merely because its destination conversion
would overflow. Optional reusable prefix checkpoints retain source/version and
validation witnesses and consume host-accounted capacity; cache-off must preserve
logical results. No hidden unbounded state is allowed.

An input change at source axis i can affect output boundaries k>i on that line;
map dirty support to that suffix, with typed-validation invalidation separately.
Output inference and cache identity include source metadata, axis, dtype and
profile. Return owned dense fragments exactly covering requested global Q.

## Resource, error and acceptance requirements

Work is proportional to actually required source prefix lengths plus arithmetic
and requested result conversion. Account active exact accumulators, checkpoint
or continuation state, source owners/windows, dependency/validation metadata,
output payload and scratch. No full output or source line must be retained merely
to scan it. Check cancellation per block, at least every 4096 source elements,
and during exact/refinement work. Work/capacity/stage exhaustion fails explicitly.

Compile/preflight rejects axis/dtype/shape violations or N+1 product above 2^40.
Requested integer overflow uses OperationFailed/ArithmeticOverflow at the output
coordinate with Atom scope; floating overflow/nonfinite results follow numeric
semantics. A batch of eligible execute_atoms observations must preserve successful
k=0 and representable prefixes alongside an overflowing prefix failure.
Upstream/typed/resource/cancellation failures remain separate. Publish no partial
failed observation; final result owners remain valid after context destruction.

Conceptual public fixture: [1,2,3] -> [0,1,3,6]. Use independent exact prefix
sums and raw payload classification. Verify k=0 performs no source reads,
disjoint boundary requests read exactly the required prefix union, large
cancellation avoids unrequested-prefix failures, block partitions do not affect
bits and changes invalidate only dependent suffixes. Include multi-axis batches,
negative source strides, subnormals, NaN ordering, signed zeros, budget/cancellation,
cache-off and owner lifetime through public WorkflowDocument execution when
implemented. The implementation evidence below records the checks actually run.

## Implementation and executable acceptance

Six suffixed keys are registered through `numeric_scans.cpp`; public authoring
helpers live in `photospider/numeric/scans.hpp`. Each operation retains an exact
accumulator and a separate conversion snapshot, preserving original source
NaN/Inf/zero classification. Empty boundaries emit positive zero without Data.
Windows transport at most 64 numeric source values; typed Validation closure
is accounted separately and can require additional channel values.

Regional prefix requests group outputs by line and increasing boundary, scanning
only through each line's largest requested boundary. Integral rectangles are
streamed independently in original row-major order; repeated arithmetic is
charged. Separate executions and `execute_atoms` observations can recompute
source values. No persistent checkpoint or integral table is retained.

The output plan and complete per-observation association rows are host-accounted.
Each Need stage processes all requested rows; dense prefix boundary requests
can therefore require quadratic association work even though source terms are
scanned once. Work/capacity/stage limits reject excessive requests explicitly.
This is not a whole-execution linear-time or once-per-Run guarantee.

The manual `photospider_numeric_scans` target and `scan_oracle.py` are described
in [the workflow example](../../../../examples/numeric_workflow/README.md).
Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 runs passed
2,280 independent Fraction/raw-bit cases per profile on 2026-09-19, plus
public workflow fixtures, sparse/L-shaped support,
integer Atom isolation, typed/Empty/zero boundaries, negative strides, fenv,
output-cap checks, sorting work/cancellation cleanup, and 4,096 source values
through 64 windows with four sparse results and at most 16 KiB payload. The
installed strict/Apple consumers, focused compiler unit, formatting/lint and
independent math/entry reviews passed. No integration test or CTest
registration is added; specification status remains Proposed.
