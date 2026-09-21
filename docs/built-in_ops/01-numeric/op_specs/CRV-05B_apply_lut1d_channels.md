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

Every nonempty request uses one CPU Whole callback over complete input, table
and axis Values, including recognized typed validation and upstream failures.
Validate the complete reconstructed axis first, then every finite/domain query
before table arithmetic. Compute every output element/channel and publish one
immutable dense owner with the complete input shape. Sparse demand restricts
returned coverage, not computation or full output memory. Empty reads no payload;
all static metadata checks still apply.

Mathematical table selection is unchanged: knot/clamp/singleton converts one
entry; interpolation/extrapolation uses its adjacent pair, independently per
channel. Generic entries outside all evaluated stencils receive no additional
finite scan. Typed validation and upstream collection cover the complete table.
Errors in otherwise-unrequested queries or evaluated channels can fail the Run.
Any input/table/axis edit invalidates recorded output demand. Cache identity
retains all input versions, profile, metadata and parameters. There is no
per-channel Atom success isolation. Output dtype, shape and empty facets remain
unchanged; arbitrary input offsets, unaligned and signed/zero strides remain
legal. The complete immutable owner survives context destruction.

## Algorithm, resources and errors

For M total input elements, work is O(L+M log L) plus exact arithmetic, full
input collection and typed validation. Singleton lookup is constant time per
query. The callback retains a host-accounted Float64 grid of 8*L element bytes
(up to 8 MiB) plus allocator/metadata overhead, fixed exact workspace and O(rank)
coordinate state. It retains no per-output certificates, rows or lookup table.
Output costs dtype_bytes*M, regardless of requested coverage. Complete table
collection costs its full L (or L*C) logical payload when a dense collect is
needed; retained source owners are accounted separately.

The complete axis uses endpoint-weighted RN64 coordinates. One caller-preserving
floating environment covers axis reconstruction and curve evaluation; unresolved
accelerated bounds still use the same exact fallback. Work/cancellation is checked
in axis generation, input reads, lookup and exact arithmetic, and before publishing.
Resource failure never authorizes skipping axis validation or weaker arithmetic.
Unpublished output/workspace is released; no partial success is published.
Numeric InvalidDomain/ArithmeticOverflow failures have Run scope. Typed, upstream,
resource, stale, backend and cancellation failures retain their categories.
Whole does not expose per-value fallback counters; report those as unavailable.

Each channel may have a different input query; sharing a row prefix does not
permit sharing the selected table index. Apply the unchanged scalar formula
with that channel's coordinates and entries. No color conversion is inferred.

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

The maintained constructor preserves six-family formal registration and the
channel shape, including C=1 and rank-1 vectors. Public workflows compare
independent scalar references, both multi baking chains, full-input typed/source
failure, rank-8 Run failures, cache replacement and complete-output budget
rejection for huge logical channels. Current native strict/Apple share the six
manual groups and 1416 independent Fraction cases described in CRV-05A. Other
platforms were not rerun for Whole; specification status remains Proposed.

- [Family decisions](CRV-05_apply_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
