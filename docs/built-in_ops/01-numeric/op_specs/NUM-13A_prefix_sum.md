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
repository_commit: current working tree
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

The `numeric.prefix_sum_*` operations accept one `Result` tensor member under
any member key. The output port key is `values`; its Result schema is
`photospider.tensor` with tensor member `samples`. The source
`sample_shape()` includes batch axes; the output uses that complete shape as
ordinary axes and has no facets or batch topology. The operation computes
boundary prefix sums along the required static Int64 axis. For an input
axis length N, output axis length is N+1; output[k] sums input axis positions
[0,k). The first boundary k=0 is zero, and k=N includes the whole line. Other
axis lengths and rank are preserved. There is no inclusive/exclusive mode.

Shapes are rank 1..8 with positive extents and input/output logical count <=2^40. Axis is a
nonnegative index less than rank. Shape addition and product are checked before
execution. This generic numeric scan does not infer physical units or image roles.

## Dtype and exact prefix semantics

Inherit [reduce_sum's dtype and numeric rules](NUM-11A_reduce_sum.md): four input
dtypes, required static dtype with integer default Int64 or floating default
Float64, explicit same-domain destination selection and one final range check
or correctly rounded conversion. Each complete-output prefix is the exact sum of its
own source interval, not a recurrence on previously rounded output values.
Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.

Empty mathematical prefixes yield integer zero or floating +0. For every
nonempty output demand, Whole requests complete input support with Data,
Validation and Descriptor (role 13), including a request limited to k=0. The
kernel reads through authorized Result windows and does not pack the complete
input into another payload. It computes and publishes the complete output in
global coordinates; the requested footprint scopes observed dependency roots.
Consumers can read any coordinate within the Result's complete published
coverage. Empty demand has no sample coverage or arithmetic and issues no input
payload Need.
For nonempty prefixes, source NaN priority follows original logical axis order,
with shared quieting/payload conversion. Mixed signed infinities and finite zero
signs follow reduce_sum. The leading +0 is a boundary output, not an extra
arithmetic contribution: an all-negative-zero nonempty prefix yields -0.

A newly generated NaN at an earlier prefix is not a source element for later
prefixes. For source [+Inf,-Inf,NaN_payload], outputs are [+0,+Inf,canonical_NaN,
quiet_payload_NaN], following the actual source NaN priority at each boundary.

Every output prefix undergoes final conversion. For Int64 [INT64_MAX,1,-1],
output k=2 overflows and fails the Run even when the consumer requests only k=3.
The exact accumulator itself can exceed destination range; only an actual
complete-output final conversion fails. Floating overflow remains a numerical
infinity and does not contaminate the exact carry used by later outputs.

## Whole demand, state and invalidation

Nonempty demand requests and validates the complete input support, then computes
complete output, including all lines and boundaries. A request only at k=0 still
has full input validation and can fail upstream or typed checks, although that
boundary performs no sample addition. Any active source edit invalidates all
recorded observations. Each line uses an exact accumulator and a separate
conversion snapshot, preserving NaN/Inf/zero source classifications.

## Resources, errors and acceptance

Work is O(full_input_count) plus exact arithmetic and full-output conversions.
The operation owns the complete packed output and uses authorized source windows
with fixed exact state; it does not create a full input copy. Coordinate vectors
are bounded by rank 8. Work/cancellation checks cover each input, carry snapshot
and final conversion/publication. Integer `ArithmeticOverflow` is a Domain/Run
failure with the output coordinate; failures release unpublished output and
state. Schema caps, dtype rules and owner lifetime remain.

The current `test_numeric_scans_result` workflow checks `[1,2,3] -> [0,1,3,6]`,
independent small-integer enumeration, batch and nonadjacent axes, negative and
unaligned strides, special values, zero-boundary validation, Empty demand,
resource budgets and cancellation. It checks complete-output integer overflow,
including coordinates outside the requested projection. The older
`test_numeric_result_math.cpp` integration fixture contains additional scan
cases, but it was not rerun for this Result migration.

## Implementation and executable acceptance

The six registered profile keys use Whole execution. Public authoring helpers are
declared in `photospider/numeric/scans.hpp`; current execution and focused
validation details are in [NUM-13 Whole execution](../scans-whole.md). Proposed
specification status is unchanged.
