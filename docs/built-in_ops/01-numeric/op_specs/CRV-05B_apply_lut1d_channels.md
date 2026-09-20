---
spec_schema_version: 1
id: CRV-05B
parent_id: CRV-05
function: apply_lut1d_channels
operation_family: curve.apply_lut1d_channels
proposed_operation_keys:
  - curve.apply_lut1d_channels_strict
  - curve.apply_lut1d_channels_accelerated_apple_silicon
  - curve.apply_lut1d_channels_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
verification_status: manual_public_workflows_and_independent_oracle
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-05B: apply_lut1d_channels

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed channel interface

Inputs in order are input[...,C], table[L,C] and a shared Float64 axis[3].
The final input axis is always the channel axis and its length equals the
table's second extent. Each input[...,c] uses only table[:,c]. Output values
preserves the complete input shape, including the channel axis when C=1.
It does not map one scalar to C outputs; that is the separate color-ramp family.

Inherit [single-table CRV-05A](CRV-05A_apply_lut1d.md) for floating dtype rules,
output default matching input, complete shared-axis validation, descending axes,
singleton tables, three domain modes, strict whole-formula interpolation, exact
zero/selection rules and the shared accelerated final FP32 bound. Output facets are empty. Each input/table
Value has one dtype; channels do not carry independently selected dtypes or axes.
Require C>=1, 1<=L<=1048576, positive input extents and input rank 1..8.
A rank-1 input [C] is one vector. Input and table logical element counts must
each be <=2^40, with products checked without overflow. There is no additional
small C limit. Input Float32/Float64 and table Float32/Float64 may differ.

## Static parameters, inference and demand

Static dtype and out_of_domain inherit CRV-05A's required String encodings,
constructor defaults and explicit direct-node parameters. There is no channel_axis,
per-channel dtype, per-channel policy or independent-axis mode. Validate the
table/input C relation at compile/preflight; output shape/dtype are static.

For output coordinate q=(...,c), input Control support is q and table Data is
the scalar lookup's selected singleton/pair indices paired only with c.
All axis[0:3] is shared Control/Validation for every nonempty request. Validate
the complete reconstructed coordinate grid once per admitted computation or
reuse a witnessed immutable valid index. Do not read other input components or
table columns merely because one channel in a row is requested. An unrequested
channel's bad table value does not affect this numeric observation. Recognized
typed Validation closure remains a separate obligation. Empty Q reads no payload.

Changed input[q] invalidates values[q]. Changed table[j,c] invalidates only
same-channel observations retaining that lookup contribution or validation.
Axis edits invalidate all dependent outputs, across channels. Retain source and
lookup witnesses even when a constant table yields unchanged numbers. No hidden
cross-channel normalization, clipping, premultiplication or color conversion occurs.

## Algorithm, resources and errors

Apply the single-table mathematical lookup independently to each requested
channel, sharing only the coordinate grid and compatible lookup state. A row's
input channels may have different query values and therefore different table
indices; do not share a selected table index solely because coordinates share
the same non-channel prefix. Use independent input/table values for each (q,c).

For M requested scalar output components, work is O(L+M log L) plus exact
arithmetic and validation, and output payload is b*M. Account the optional 8L
grid, requested pair/dedup metadata, selected table/input source owners and windows,
arithmetic limbs, validation and output fragment capacity. No full L*C or input
materialization is required for a partial channel request. Support arbitrary
valid immutable signed/zero strides and offsets along all axes, with packed
owned returned fragments at the original global output coordinates.

Inherit scalar cancellation intervals, host workers/admission, finite-value
failures, backend availability, cache-off and post-context ownership. A table
numerical error is attributed to the actual dependent output Atom including its
channel; a shared invalid axis affects all dependent observations. Compile/preflight
rejects type/rank/C/product violations. Upstream and typed-validation failures
retain their original scope rather than being renamed as local color errors.
Failed observations publish no partial successful Value.

## Acceptance and implementation status

Conceptual fixture: table=[[0,10],[2,8]], axis=[0,1,1],
input=[[0,1],[0.25,0.5]] -> values=[[0,8],[0.5,9]]. This detects an incorrect
implementation that shares a query/index across all channels in an input row.
Use independent per-channel exact rational interpolation and coordinate checks.
Strict matches result bits; accelerated results meet the shared FP32 bound; run shared endpoint, signed-zero,
domain, singleton-table, descending-axis and mixed-dtype cases for each channel.

Compare selected channels with independent scalar CRV-05A nodes and check exact
read/dirty witnesses. Include C=1, single vector input, large sparse logical
arrays, wrong C, remote bad table columns, shared bad axis, fragmented/strided
sources, low budgets, cancellation, cache-off and owner lifetime. The public
WorkflowDocument fixture must connect a multi-function CRV-04 table/axis and
inspect requested output channels through Compiler/ExecutionContext.

The maintained `apply_lut1d_channels_node` constructor in
`photospider/numeric/lut1d.hpp` provides the three independently named profile
keys. `examples/numeric_workflow/lut1d.cpp` executes the analytic fixture above,
scalar-column references, the two multi-function baking templates and sparse
2^39-channel composition. It also verifies per-channel cache/source selection,
independent channel failures and rank-8 Atom coordinates. The shared six manual
groups and 1416 independent cases/profile passed on native Clang strict/Apple
and WSL Clang strict/AVX2 on 2026-09-20; installed consumers passed. Full
algorithm/resource boundaries and commands are linked from
[CRV-05A](CRV-05A_apply_lut1d.md#maintained-implementation-and-verification).
Specification status remains Proposed; the executable has no CTest/integration
registration.

- [Family decisions](CRV-05_apply_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
