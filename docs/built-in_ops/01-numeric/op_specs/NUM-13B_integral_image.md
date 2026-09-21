---
spec_schema_version: 1
id: NUM-13B
parent_id: NUM-13
function: integral_image
proposed_operation_keys:
  - numeric.integral_image_strict
  - numeric.integral_image_accelerated_apple_silicon
  - numeric.integral_image_accelerated_x86_64
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

# NUM-13B: integral_image

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute two-axis rectangular prefix sums with leading zero boundaries. Required
static String axes names exactly two distinct nonnegative input axes; normalize
the pair to increasing order. Input rank is 2..8 with positive extents. Output
values retains all other axes and adds one to each selected axis. All other axes
are independent batches, with no image/color semantics inferred. Input/output
logical counts must not exceed 2^40; output facets are empty.

For selected axes a,b and an output coordinate o, sum the input rectangle
s[a] in [0,o[a]), s[b] in [0,o[b]), with all other coordinates equal to o.
This is the exact rectangle sum, not a composition of rounded row and column
prefix outputs. Inherit reduce_sum's four input dtypes, same-domain destination
selection (integer default Int64, float default Float64) and final range checking
or correct rounding. Static dtype is explicit in direct nodes.

## Prefix semantics and exact demand

Inherit [prefix_sum's arithmetic and failure rules](NUM-13A_prefix_sum.md), using
rectangle contributors rather than line contributors. If either selected output
boundary is zero, the rectangle is empty: return integer zero or floating +0
while Whole still prepares the full input. Nonempty rectangle NaN priority follows original
logical row-major coordinates, independent of which selected axis is processed
first. Preserve the shared payload conversion and mixed-infinity/zero rules.

All complete-output rectangles are converted/range-checked; any integer overflow
fails Domain/Run, including outside the consumer projection. Internal exact sums
can exceed the destination range. Rounded integral outputs never become exact
carry. Strict remains reproducible; accelerated floating results retain their
own shared bound. Empty demand reads nothing; nonempty zero-boundary requests
still read and validate all source cells. Any source edit invalidates all recorded
observations, including projections at zero boundaries.

## Algorithms, resources and errors

For each independent plane, visit the lower-numbered selected axis as outer rows
and the higher-numbered axis as inner columns. The current row's exact prefix
is added to the saved exact prefix rectangle for all previous rows at that column.
These disjoint contributions cover precisely the desired rectangle. Save exact
magnitude/sign, first-NaN, both infinity flags and all-negative-zero flag before
any destructive final conversion. Earlier rows have NaN priority over the current
row; within each row preserve source logical order. Reset all columns per plane.

This takes O(full_input_count) exact updates/conversions with one fixed pair of
full workspaces and one compact carry per inner-axis column. Column element
capacity is560*W bytes on the recorded arm64 build, plus ResourceAllocator
header/alignment/Entries, charged as metadata. Full input collection and complete
dense output are additional payload. W is the higher-numbered selected axis's
extent; it is not chosen by physical stride or assumed to be the shorter axis.
No repeated rectangle scan, per-output descriptors or persistent table is retained.
Work/cancellation checks cover resets, reads, merges, conversions and publication.

Schema/output caps remain compile/preflight errors. Whole failures release output,
state and columns; floating exceptional results follow exact sum rules. Budget
failure never changes arithmetic precision. Final output owners outlive context.

## Acceptance and implementation status

Conceptual fixture: input=[[1,2],[3,4]], axes="0,1" yields
[[0,0,0],[0,1,3],[0,4,10]]. Use independent exact rectangle summation as the
oracle. Test zero boundaries with full source validation, non-leading integral axes,
independent batches, disjoint corners with an L-shaped source union, Run failure
when an unrequested integer rectangle overflows despite representable later results, NaN priority across axes and all inherited dtype/stride/resource cases.

Four-corner rectangle queries on rounded floating outputs may introduce their
own rounding/cancellation error; this operator guarantees each prefix result,
not exact recovery from arbitrary downstream subtraction. Test exact integer
fixtures separately from floating retrieval accuracy. Deliver public workflow
runs and lifetime/invalidation checks; the implementation evidence below records the checks actually run.

## Implementation and executable acceptance

All formal profiles use the exact Whole algorithm above. Current public workflow,
Fraction rectangle oracle, cross-plane/NaN/layout/resource checks and measured
public/core performance are in [NUM-13 Whole execution](../scans-whole.md).
Older regional WSL/installed acceptance is historical.
