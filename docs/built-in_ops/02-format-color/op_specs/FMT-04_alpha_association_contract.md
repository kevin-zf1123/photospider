---
spec_schema_version: 1
id: FMT-04
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
---

# FMT-04: explicit alpha representation boundary adapters

The default registry provides `alpha.associate_<profile>` and
`alpha.unassociate_<profile>` for `strict`, `accelerated_apple_silicon`, and
`accelerated_x86_64`. Each uses fixed invocation ABI 8. Semantic calls bind one Result; raw
plane/scalar calls may bind a second Result. The family and its members remain
Proposed; status records design acceptance separately from implementation.

Inherit [FMT-common](FMT_common_contract.md), its NUM baseline and the
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
Members [A associate](FMT-04A_associate_alpha.md) and
[B unassociate](FMT-04B_unassociate_alpha.md) are registered Result CPU
operations with three profile keys each. The contract replaces persistent
external alpha bindings with explicit graph inputs and keeps canonical images
straight.
[ADR 0022](../../../adr/0022-freeze-operation-c-abi-v3.md) D9 (Proposed) keeps
this boundary payload as an ordinary Result payload at the operation ABI level.

## Purpose and confirmed boundary

Canonical complete images use straight colors and internal alpha. Semantic A
produces explicitly described premultiplied numeric payloads for export or an
external interface; semantic B converts explicitly described premultiplied
payloads back to straight. A boundary payload is a generic tensor with a source
or target representation description, not a second canonical image type.
A normal image consumer must reject a premultiplied boundary interpretation
unless its contract explicitly performs conversion or the caller explicitly
reinterprets the numeric values without claiming preservation of color.

Both semantic adapters require alpha in the same tensor and preserve shape,
dtype, channel locations and alpha bits. Independent planes/scalars are first
explicitly assembled, using FMT-02 or FMT-05 where applicable. FMT-04 does not
insert/remove alpha slots, implicitly append alpha or return an external binding.
Raw numerical modes may still receive explicit external weights as described
below; those are ordinary input ports, not persistent metadata relationships.

Each call converts one explicit RGB or Gray group. Unselected groups, depth,
normals, emission and AOVs are copied unchanged. There is no premultiplied
Lab/CMYK semantic conversion. Multiple groups are converted by explicit nodes;
a partially converted multi-group tensor remains a boundary payload while any
complete color group is premultiplied. A downstream consumer may use a complete straight-image interpretation
only when all required groups have been converted. This is
a check of the explicit description, not a sample-validation closure over other groups.

A requires the selected group to be straight with an internal alpha reference;
B requires it to be explicitly premultiplied with an internal alpha reference.
Already-target states fail preflight. A color group without alpha must first
receive an internal alpha through an explicit assembly/edit. The caller supplies
`input_description` for this invocation; it does not change the operation's arithmetic.
The original input and its other consumers remain unchanged.

The arithmetic acts on stored coordinates: P=alpha*C even when C has encoded
transfer. No implicit transfer decode, linearization, gamut/range conversion or
alpha encoding occurs. Callers explicitly choose color/transfer boundary order.
RGB/Gray samples may be finite signed/HDR values; there is no RGB<=alpha bound.
The caller supplies finite alpha in [0,1] when semantic color conversion consumes it.

## Ports, shape and static parameters

Input `input` and output `values` have the same shape and Float32/Float64 dtype. A call connects one source Result and optionally one explicit alpha Result for raw plane/scalar modes; each connected Result contains exactly one tensor. Nonscalar batch prefixes match exactly; scalar weights are unbatched shape `[1]`. The complete sample rank, including batch and cell axes, is at most 8 and full sample count is at most 2^40.
With `input_description`, the input has a declared channel axis, selected RGB indices
or a Gray index, and a distinct internal alpha index outside those color indices.
Resolve index/name/role selectors by the FMT-01 exact unique namespace rules.
There is no HWC assumption. A channel axis contains at most 65,536 slots. Let S
erase the channel axis. The complete sample rank includes batch and cell axes and
is at most 8; each full input/output sample count is at most 2^40 with positive
extents. A rank-one channel vector is legal
and retains its rank; no rank-zero tensor is produced.

| Parameter | Definition / default |
| --- | --- |
| input_description | Optional explicit TensorDescription. Its presence selects described RGB/Gray association; its absence selects raw arithmetic with explicit structure and components. |
| group | Required explicit unique RGB/Gray group in semantic modes. |
| input_structure / axis | Structure/axis assertion consistent with the effective description; raw supplies structure explicitly. |
| components | Raw-only nonempty distinct selected indices, or component 0 for a tensor without a channel axis. |
| alpha_source | Semantic: selected group's internal alpha reference, optionally asserted by an explicit matching internal selector. Raw: required internal index, explicit plane input or explicit scalar input. |
| alpha | Raw-only optional explicit weight port, same dtype; plane has exactly S (full input shape for a component input), scalar has exactly [1]. |

Described association with an external alpha port fails preflight. A standalone Gray plane
must be assembled with alpha first. In raw mode, plane versus scalar is explicit;
no implicit squeeze, broadcast or resizing occurs. Empty S uses internal/scalar
raw weights rather than rank-zero external planes. Structure, selectors, group
state and source kinds are static; input samples may change between executions.

Every connected input participates in static schema checks. The compiler emits
fixed tasks with mapped payload reads and independent dense output tiles.
Association and unassociation expose no layout parameter. Empty demand has no
tasks. The three CPU profiles retain their capability requirements; this family
provides no integer normalization, mixed-dtype promotion or GPU backend.

## Mathematical reference and numeric profiles

Let x be one requested participating input component, a its selected alpha and
RN_t the NUM strict rounding to input/output dtype t, with ties-to-even and
gradual underflow. Semantic modes read both operands. The caller supplies finite x and finite a
in [0,1] for every evaluated participating coordinate. Then:

```
associate:
    a == +/-0: y = +0
    a > 0:     y = RN_t(x * a)

unassociate:
    a == +/-0 and x == +/-0: y = +0
    a == +/-0 and x != 0:   outside the semantic caller preconditions
    a > 0:                 y = RN_t(x / a)
```

All positive alpha values, including subnormals, follow the arithmetic branch.
Never replace a small positive value by zero or insert an epsilon denominator.
The caller guarantees that the correctly rounded semantic result is finite,
including unassociation by a small positive alpha. The operation applies no clipping. Gradual underflow to a subnormal
or signed zero is allowed. The special a=0 branch is canonical positive zero;
for a>0, zero signs follow NUM arithmetic. Internal alpha and other pass-through
components are copied bit-for-bit, preserving even the sign of zero.

Strict multiplication/division rounds the exact result once to t, as in NUM-05C/D. Named
accelerated arithmetic inherits those NUM profile guarantees, including exact
classification, predicates, copied values and required fallback. It uses the same caller preconditions and gains no new FMT-specific tolerance.
Preserve the caller's floating environment. Opacity a=1 is numerically identity
while retaining the same caller preconditions and declared input reads.
Round-trip association is not generally lossless because rounding/underflow and
zero-alpha hidden-color removal can lose information.

In raw, use exactly NUM multiply(x,a) or divide(x,a), with x as the first operand
and a as the second. Read both operands even when an identity/special value
determines the result. No coverage range, finite-color or source association
check applies. NUM's NaN priority/payload, signed zero, division-by-zero and
floating-overflow outputs apply; 0/0 is NaN, not the semantic +0 branch.
This is deliberately a different formula from semantic association at zero.

Raw output contains only structure and samples. Callers explicitly provide any
association and alpha interpretation to downstream operations. Original input samples supply every operand, including when the
selected component is also the raw weight. No raw operation defines canonical
premultiplied images or semantic premultiplied Lab/CMYK.

## Output interpretation and partial coverage

A computes premultiplied samples and B computes straight samples for the selected
group. Neither writes a description into the result. A caller connecting A to B
must supply B with an explicit premultiplied input description; unrelated groups
and any later interpretation are likewise configured by the caller. Shape,
channel positions, alpha bits and copied samples follow the mapping above.
No full-color validity is certified by producing only a subset.

The output alpha index refers to this output tensor. Reading input alpha does
not itself enlarge the declared write set; produced coverage is determined by
the complete retained output tiles. A sample request for R can retain a spatial
tile containing alpha and therefore produce alpha as well. Alpha coverage is
absent only where its output tiles were not retained. Downstream consumers
request the result's required tiles explicitly. Detached missing coverage remains
an ordinary access/binding failure, without an external alpha owner, hidden
snapshot, or manufactured zero/one value.

Owners, read windows and declared DAG edges have their normal retained lifetime.
Source/context destruction must not invalidate produced samples or active reads;
unproduced samples do not become available through a hidden recovery mechanism.

## Fixed reads and caller preconditions

Each retained output tile becomes one CPU task. For every converted component
in that complete tile, the task declares its original input and mapped alpha.
Pass-through channels declare only their own source. Per-port source-grid closures
are unioned without spanning gaps in the maps. Batch and atomic axes can make
source and output grids different. Scalar alpha contributes its sole sample only
when a participating component is evaluated. Unused ports retain schema dependencies.

Semantic preconditions cover the participating coordinates actually evaluated in
the complete output tile. They also apply at alpha zero or one. Additional source
tile coverage does not itself cause evaluation or add color peers. Raw arithmetic
retains its defined IEEE results without semantic finite/coverage restrictions.
Violating a semantic premise promises no numerical result or diagnostic; runtime
performs no sample-validation scan or output certification.

The numerical semantics do not suppress declared reads at alpha zero or one.
Required producer failures propagate through the executor. Public Results appear
when the complete call succeeds and retires in order; internal task dependencies
can consume completed private tiles earlier. Retained Results own their storage
independently of input and execution-context lifetime.

## Arithmetic and bounded resources

Static preparation resolves descriptions, selectors, source maps and output
schema. The kernel preparation callback checks the exact input slot union,
canonical output tile, symbol, contract and scratch. Closure size is bounded
before constructing each tile set, including malformed tasks with missing slots.
The executor admits distinct dense output allocations and scratch before issue.
Spatial metadata specifies logical axes independently of physical row-major
output strides. No full-image virtual allocation is needed for a selected tile.

Copied-only tasks declare zero scratch. A task with participating color uses a
128-sample gather/output block with fixed offset storage, `sizeof(SampleBlock)`
(currently 4096 bytes). `algorithm=reference` adds the caller-owned 4352-bit
`RatioWorkspace`, giving `sizeof(ReferenceScratch)` (currently 6888 bytes).
All mutable arithmetic belongs to that scratch; prepared maps and read-slot
indexes are immutable and shared across Runs.

Strict scalar arithmetic uses a nearest-even, gradual-underflow environment;
SIMD uses true multiplication/division, with safe semantic zero-alpha division
and exact NUM repair for raw exceptional lanes. The reference algorithm rounds
fixed integer products or ratios directly. No runtime rational/vector allocation
or private thread pool is used. Cancellation is checked every block of at most
128 samples and at bounded exact-division steps. The caller's floating environment
is restored on every return.

## Errors and verification scope

Malformed parameters, descriptions, selectors, channel relationships and shapes
fail static preparation. Unsupported profile/backend and malformed kernel
signatures fail before issue. Resource admission, cancellation, stale state and
producer failures follow the executor protocol. Semantic domain and finite-result
premises are not promised runtime error checks or coordinate diagnostics.

Use independent rational/NUM bit references for both dtypes, semantic zero and
subnormal alpha, raw NaN/Inf/zero behavior, all weight source kinds and exact
pass-through samples. Check complete and selected output tiles, per-port closure,
noncanonical axes, batch/spatial metadata, signed/zero/unaligned inputs, malformed
signatures, zero scratch for copies, cancellation, floating environment, concurrent
Runs and owners retained after context destruction. The independent
[alpha oracle](../../../../oracle/ops/format_color/fmt04/generate.py) and generated
[bit fixtures](../../../../oracle/ops/format_color/fmt04/golden.hpp) are retained;
temporary public execution checkers are removed after validation. Native backend
or performance claims require evidence from the relevant hardware.
