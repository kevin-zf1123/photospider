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
or correctly rounded conversion. Each complete-output prefix is the exact sum of its
own source interval, not a recurrence on previously rounded output values.
Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.

Empty mathematical prefixes yield integer zero or floating +0; Whole still
reads the active input for nonempty output demand.
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

Nonempty demand collects/validates complete input and computes complete output,
including all lines and boundaries. Empty demand reads nothing; a request only
at k=0 still reads input and can fail upstream/typed validation. Any active source
edit invalidates all recorded observations. No per-output source-set, association
row or persistent checkpoint remains. Each line uses an exact accumulator and a
separate conversion snapshot, preserving NaN/Inf/zero source classifications.

## Resources, errors and acceptance

Work is O(full_input_count) plus exact arithmetic and full-output conversions.
Own the complete packed output and potentially a full collected input, plus fixed
exact state. Coordinate vectors are bounded by rank8. Work/cancellation checks
cover each input, carry snapshot and final conversion/publication. Integer
ArithmeticOverflow is Domain/Run with the output coordinate; failures release
all unpublished output/state. Schema caps, dtype rules and owner lifetime remain.

Public [1,2,3] -> [0,1,3,6] and floating cancellation/special-value fixtures must
retain exact bits. Independent prefix/rectangle sums verify every sampled result;
integer oracle also checks the entire output for Whole overflow. Test arbitrary
strides, whole-input dirty, unselected failure, Empty, zero boundary validation,
work/output/state budgets and active cancellation.

## Implementation and executable acceptance

All six formal scan keys use Whole through photospider/numeric/scans.hpp.
See [NUM-13 Whole execution](../scans-whole.md) for current public workflow,
independent oracle and separate public/core timing. Older regional WSL/installed
checks predate this implementation. Proposed status is unchanged.
