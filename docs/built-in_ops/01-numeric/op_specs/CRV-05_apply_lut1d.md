---
spec_schema_version: 1
id: CRV-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
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
operations use six explicit CPU-profile keys and public constructors, with
numerical and workflow acceptance recorded in the individual specifications.
Specification acceptance remains Proposed.

- [Single-table draft](CRV-05A_apply_lut1d.md).
- [Per-channel table specification](CRV-05B_apply_lut1d_channels.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).

## Fixed execution, resources and caller preconditions

Each input Result has one tensor member under any schema id/version/key. Complete `sample_shape()` includes batch axes. Static preparation checks all three input schemas and parameters, including inputs whose payload is inactive. Output `values` uses `photospider.tensor` v1/member `samples`, preserves the complete input shape as ordinary axes and selects Float32 or Float64.

Each selected output tile has one fixed CPU task. For L>1 its reads cover the matching input region, all L table rows and axis positions 0 and 1. The channel operation restricts table columns to the output tile's final-axis interval. Each source's own tile grid closes these regions; an atomic source axis can enlarge coverage. Step is a consistency precondition and has no separate payload dependency. The kernel reconstructs RN64 knots on demand during bounded binary search and reads the selected one/two table entries through prepared input views.

For L=1 the task reads only the sole table row, restricted to selected columns for channels. Input and axis have schema-only references and no payload reads; their producers need not execute for this observation. The singleton axis `[a,a,+0]`, finite input and reject's equality to a remain caller preconditions. This constant-output path does not diagnose violations. It converts the selected table entry directly and uses zero scratch; a valid invocation may supply `scratch=nullptr` and `scratch_count=0`.

Callers guarantee finite consumed queries/table entries, a valid complete axis, the selected reject domain and finite representable outputs. For L>1 this includes endpoint/step consistency and strict ordering of the entire endpoint-weighted RN64 grid. These numerical properties are unchecked. Generic table entries outside mathematical stencils have no additional finite requirement, although their declared source tiles remain dependencies. An upstream producer must satisfy its own contract for every output tile selected by that closure.

Scratch for L>1 contains one preallocated `ExactSampling` plus `ExactCurve` state per invocation, independent of L and the full output shape. Lookup costs O(M log L) coordinate reconstructions plus exact arithmetic for M selected elements. L=1 costs O(M) conversions. Output storage covers selected tiles; metadata and source tile descriptors scale with the physical closure. The operation allocates neither a full axis grid nor a complete dense output/table copy. DirectTiles may retain affine source views, while DenseHost imports use explicit Plan COPY tasks and their declared storage.

The invocation borrows prepared views and writes only preallocated output. Independent Runs share immutable preparation and own their arithmetic state. FloatingEnvironment establishes round-to-nearest and gradual underflow, preserving the caller's state; cancellation is checked per output, search iteration and exact arithmetic step. Internal exact-capacity exhaustion remains an operational error; mathematical divisors satisfy caller premises. Static schema/parameter, binding/layout, resource, backend and cancellation failures retain their categories. Numerical precondition violations have no promised diagnostic or numerical result. Successful tile writes become internal dependencies; the public call Result appears only after all required writes succeed and the call retires in order. Retained output owners survive context destruction. Empty output requests read no payload. Content caching and repeated Result identity are outside this operation contract.

## Maintained implementation and validation

`plugins/ops/01-numeric/lut1d_application.cpp` registers six fixed CPU profile keys. Preparation verifies the selected symbol, exact scratch size, canonical output write and exact source tile sets. Descending lookup compares keys in table order and swaps both x and y before the positive-denominator `ExactCurve` calculation. The endpoint-weighted coordinate formula and single final conversion are specified in [CRV-05A](CRV-05A_apply_lut1d.md).

Use the retained [Fraction reference](../../../../oracle/ops/numeric/crv05/reference.py) for valid numerical inputs and temporary public Compiler/ExecutionContext checks. Include both axis directions, each domain policy, singleton tables, mixed input/table/output dtypes, independent channel queries, signed zero and subnormal values. Check selected output tiles with ordinary, atomic and batch source axes, reverse/unaligned/zero strides, concurrent Runs, source rebinding and exported/imported plans.

Exercise schema-only singleton input/axis edges, omission of an ordinary step-only source tile, Empty, malformed preparation, low output/scratch budgets, huge sparse logical shapes, maximum L with affine table storage, cancellation and retained output lifetime. Connect all six CRV-04 templates to the consumer and check each producer's own numerical preconditions. Source samplers may legally produce repeated coordinates; callers must choose a grid satisfying this consumer's stricter axis precondition. Numerical precondition violations are outside the valid-input oracle checks.
