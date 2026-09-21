---
spec_schema_version: 1
id: CRV-05
kind: shared_operator_contract
category: 01-numeric
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

# CRV-05: apply_lut1d family

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed split

Provide two independent operations, both preserving input shape:

- Single-table application: table[L] maps every numeric element of input.
- Per-channel multi-table application: table[L,C] column c maps input[...,c].

Both target interfaces are fully clarified. Mapping one
scalar to a vector/color ramp is CRV-06 rather than this same-shape LUT family.
Both receive explicit axis[3], use fixed linear interpolation and inherit their
specified finite-value, exact-rounding and mathematical table-selection contracts. These
operations are implemented through six explicit CPU-profile keys and public
constructors, with numerical and workflow acceptance recorded in the individual
specifications. Specification acceptance remains Proposed.

- [Single-table draft](CRV-05A_apply_lut1d.md).
- [Per-channel table specification](CRV-05B_apply_lut1d_channels.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).

## Whole execution

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
