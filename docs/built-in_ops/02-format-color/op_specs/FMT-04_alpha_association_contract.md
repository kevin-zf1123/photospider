---
spec_schema_version: 1
id: FMT-04
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-04: explicit alpha representation boundary adapters

Implementation update: package 0.20.0 [removes the legacy format/color code](FMT_legacy_retirement.md).
Descriptions of old registrations below record the inspected baseline only;
those keys and pixel callbacks are no longer available. This target remains
Proposed and unimplemented.

Inherit [FMT-common](FMT_common_contract.md), its NUM baseline and the
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
Members [A associate](FMT-04A_associate_alpha.md) and
[B unassociate](FMT-04B_unassociate_alpha.md) remain proposed default-registry
primitives. The 2026-09-23 decisions replace the former dual-state canonical
image interface and persistent external alpha bindings. No runtime registration
or migration is implemented by this document.

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
complete color group is premultiplied. B may declare a complete canonical image
only when all its complete color groups have straight interpretations. This is
a structural descriptor check, not a sample-validation closure over other groups.

A requires the selected group to be straight with an internal alpha reference;
B requires it to be explicitly premultiplied with an internal alpha reference.
Already-target states fail preflight. A color group without alpha must first
receive an internal alpha through an explicit assembly/edit. Override can change
effective source interpretation, but not the operation's output representation.
The original input and its other consumers remain unchanged.

The arithmetic acts on stored coordinates: P=alpha*C even when C has encoded
transfer. No implicit transfer decode, linearization, gamut/range conversion or
alpha encoding occurs. Callers explicitly choose color/transfer boundary order.
RGB/Gray samples may be finite signed/HDR values; there is no RGB<=alpha bound.
Alpha must be finite in [0,1] when consumed by semantic color conversion.

## Ports, shape and static parameters

Input `input` and output `values` have the same shape and Float32/Float64 dtype.
In semantic modes the input has a declared channel axis, selected RGB indices
or a Gray index, and a distinct internal alpha index outside those color indices.
Resolve index/name/role selectors by the FMT-01 exact unique namespace rules.
There is no HWC assumption. Let S erase the channel axis. Rank 1..8, positive
extents and checked 2^40 logical-count rules inherit NUM/FMT. A rank-one channel
vector is legal and retains its rank; no rank-zero tensor is produced.

| Parameter | Definition / default |
| --- | --- |
| metadata_mode | respect by default; explicit override or raw. |
| metadata_override | Call-local effective source description; only with override. No external sample binding. |
| group | Required explicit unique RGB/Gray group in semantic modes. |
| input_structure / axis | Structure/axis assertion consistent with the effective description; raw supplies structure explicitly. |
| components | Raw-only nonempty distinct selected indices, or component 0 for a tensor without a channel axis. |
| alpha_source | Semantic: selected group's internal alpha reference, optionally asserted by an explicit matching internal selector. Raw: required internal index, explicit plane input or explicit scalar input. |
| alpha | Raw-only optional explicit weight port, same dtype; plane has exactly S (full input shape for a component input), scalar has exactly [1]. |

Semantic use of an external alpha port fails preflight. A standalone Gray plane
must be assembled with alpha first. In raw mode, plane versus scalar is explicit;
no implicit squeeze, broadcast or resizing occurs. Empty S uses internal/scalar
raw weights rather than rank-zero external planes. Structure, selectors, group
state and source kinds are static; input samples may change between executions.

Each member has strict, Apple Silicon and x86-64 CPU profile entries. Arithmetic
outputs are materialized; there is no auto/view switch or alpha=1 view guarantee.
No integer alpha normalization, GPU implementation or mixed-dtype promotion is
implied. Integer-coded inputs require explicit numeric decoding/casting.

## Mathematical reference and numeric profiles

Let x be one requested participating input component, a its selected alpha and
RN_t the NUM strict rounding to input/output dtype t, with ties-to-even and
gradual underflow. Semantic modes read both operands and require finite x and
finite a in [0,1]. Then:

```
associate:
    a == +/-0: y = +0
    a > 0:     y = RN_t(x * a)

unassociate:
    a == +/-0 and x == +/-0: y = +0
    a == +/-0 and x != 0:   semantic domain failure
    a > 0:                 y = RN_t(x / a)
```

All positive alpha values, including subnormals, follow the arithmetic branch.
Never replace a small positive value by zero or insert an epsilon denominator.
Semantic output must remain finite: a division result rounding to infinity fails
with ArithmeticOverflow rather than clipping. Gradual underflow to a subnormal
or signed zero is allowed. The special a=0 branch is canonical positive zero;
for a>0, zero signs follow NUM arithmetic. Internal alpha and other pass-through
components are copied bit-for-bit, preserving even the sign of zero.

Strict multiplication/division rounds the exact result once to t, as in NUM-05C/D;
do not introduce the legacy Float64-intermediate-then-Float32 reference. Named
accelerated arithmetic inherits those NUM profile guarantees, including exact
classification, predicates, copied values and required fallback. It does not
change semantic validity/overflow decisions or gain a new FMT-specific tolerance.
Preserve the caller's floating environment. Opacity a=1 is numerically identity
after required semantic operand checks; it is not permission to skip failures.
Round-trip association is not generally lossless because rounding/underflow and
zero-alpha hidden-color removal can lose information.

In raw, use exactly NUM multiply(x,a) or divide(x,a), with x as the first operand
and a as the second. Read both operands even when an identity/special value
determines the result. No coverage range, finite-color or source association
check applies. NUM's NaN priority/payload, signed zero, division-by-zero and
floating-overflow outputs apply; 0/0 is NaN, not the semantic +0 branch.
This is deliberately a different formula from semantic association at zero.

Raw does not change an association label, establish an external alpha relation,
or claim completion of a boundary conversion. Retain applicable descriptive
fields without sample-validity evidence. When raw edits an internal alpha
channel, its descriptor still refers to that current logical channel, never an
old snapshot. Original input samples supply every operand, including when the
selected component is also the raw weight. No raw operation defines canonical
premultiplied images or semantic premultiplied Lab/CMYK.

## Output metadata and partial coverage

A changes the selected group's boundary interpretation to premultiplied and
removes the claim that the whole result is a canonical complete image. B changes
the selected group to straight; it may restore canonical image interpretation
when no other complete group remains premultiplied. Retain model, primaries,
white, transfer, units and explicit internal alpha indices. Remove inapplicable
sample guarantees, including binary Gray storage after multiplication. Other
components retain their applicable meaning; no full-color sample validity is
certified by producing only a subset.

There is no external alpha owner, snapshot or origin identity in result metadata.
The alpha index refers only to this output tensor's channel. A request for R may
read input alpha without publishing output alpha. A downstream unassociate or
other consumer needing alpha must explicitly request the alpha channel through
the declared DAG. If only a detached partial result with missing alpha coverage
is available, access fails under normal missing-coverage rules. It must not read
an old input alpha via metadata or manufacture alpha=0/1. This rule preserves
exact output coverage and does not force alpha production for R alone.

Owners, read windows and declared DAG edges have their normal retained lifetime.
Source/context destruction must not invalidate produced samples or active reads;
unproduced samples do not become available through a hidden recovery mechanism.

## Exact observation, validation and dirty support

Let Q be requested output coordinates and T its selected color/component subset.
Data includes original input at every Q coordinate and alpha at the mapped
nonchannel coordinates of T. Internal alpha support is deduplicated with Q;
raw scalar alpha contributes index zero iff T is nonempty. No unrequested color
peer or AOV sample is added. Semantic Validation is each requested participating
x and a, including at zero or one alpha. Pass-through alpha/AOV-only requests
copy exact bits without extra finite/range validation. Static descriptor, group,
source-state and channel-reference checks apply regardless of requested samples.

No value-dependent Control read or short circuit at a=0 suppresses a required
operand failure. Upstream Whole work retains its original scope. A failed
observation publishes no success and does not revoke unrelated completed output.
Input component changes map pointwise to the same output; internal alpha changes
also fan out to selected group components at that coordinate. Raw external plane
changes fan out at matching coordinates; raw scalar changes affect all requested
participating observations. Intersect dirty maps with actual observations.
Descriptor changes invalidate inference. Even at alpha=0/1, retain all required
Data and Validation dependencies.

## Reference algorithm, storage and resources

1. Resolve mode, effective description, source representation, axis and selectors.
   Infer unchanged shape/dtype and target interpretation; reject invalid static
   relationships without scanning samples.
2. Compute exact Q/T input support; admit and acquire its immutable read windows.
3. Reserve result storage and prepare needed pages before work. Copy unselected
   bytes or evaluate the semantic/raw formula for each requested coordinate.
4. Check cancellation/currentness and publish only successful requested coverage.
   Publish internal channel references, never external alpha bindings.

Materialized image-shaped boundary payloads use the same planar storage rules;
changing a semantic label does not legalize interleaving or bypass image storage.
Generic non-image raw tensors use their admitted owned representation. Canonical
images reserve one full virtual span with the DAG tile geometry, valid edge rows,
width padding and page-aligned tile starts. Only required pages receive backing;
all produced pages remain retained until owner retirement. Read exports are not
new persistent image layouts.

For rank r, Nq requested samples and F windows, mapping/compute costs are
O(Nq*r+F*r), plus static metadata, page sets and required upstream work. Bounded
selection tables and exact admitted windows avoid per-pixel descriptions or an
eager per-reserved-page directory. SIMD cannot read padding, unrelated channels
or unprepared pages. Mask semantic zero-alpha lanes before division without
skipping x validation or changing the caller's floating environment.

Charge actual supplied backing, simultaneous input/output, retained normal
owners, windows, numeric scratch and metadata; deduplicate shared owners. Virtual
span, logical sample bytes and physical RSS are distinct. No extra alpha capture,
external-binding ancestry, eviction, producer replay or private thread pool is
introduced. Poll cancellation/currentness at most every 1024 entries/samples and
before publication. Release unpublished resources on failure.

Initial optional result caching remains disabled pending a conforming cache for
exact coverage, ordinary input identity and representation metadata. Future cache
identity includes mode, profile, selectors, effective representation and ordinary
inputs; semantic/raw or canonical/boundary results cannot alias under a key that
omits their different formula, failures or metadata. There is no alpha-origin
snapshot mechanism to reconstruct.

## Errors and support limits

| Condition | Phase | Outcome |
| --- | --- | --- |
| Missing/malformed selector, ambiguity, illegal mode/axis/parameter | Compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Wrong representation/model, external semantic alpha, missing internal alpha, dtype/rank/shape conflict | Static preflight | TypeMismatch / None. |
| Requested nonfinite semantic color, invalid alpha, or B nonzero p at zero alpha | Semantic evaluation | OperationFailed / InvalidDomain, affected observation. |
| Semantic result becomes nonfinite | Semantic evaluation | OperationFailed / ArithmeticOverflow. |
| Raw nonfinite input, zero division or floating overflow | Raw evaluation | NUM-defined successful result. |
| Missing coverage, resource/work, backend, cancellation or stale failure | Inherited phase | Preserve NUM/kernel code, origin and scope. |

Diagnostics name the member, group/component, alpha channel/source and global
coordinate for sample failures; static errors have no invented pixel coordinate.
Strict and named CPU profiles retain NUM arithmetic guarantees and identical
semantic validity decisions. No legacy typed-image behavior is accepted as a
compatibility implementation.

## Acceptance and implementation dependencies

Use independent exact binary-rational strict rounding, NUM bit/special-value
oracles and finite-set Data/Validation/dirty mapping. Members specify analytic
fixtures. Cover RGB/Gray, noncanonical channel placement, ranks/axes, internal
alpha in semantic modes, all explicit raw weight sources, source-state rejection,
zero/tiny/one alpha, signed/HDR color and signed zeros.

Compare whole, offset/disjoint and cross-tile requests, R-only, alpha-only and
AOV-only output. Check invalid unrequested peers and required failures at zero
alpha. For A -> B requests, B must explicitly demand A's alpha output channel;
a detached A result lacking alpha must fail rather than use external provenance.
Check complete-image rejection of boundary payloads, multiple-group intermediate
payloads, source immutability, context-independent produced coverage, final lease
release, low budgets, cancellation and restored floating environment.

Benchmark Float32 [4096,4096,4] internal-alpha inputs, transparent/opaque/tiny
alpha, full/R-only/alpha-only output and y/x=[127,130) at tile size 128. Record
axes, profile/ISA/build, workers, source/page state, time statistics, bytes,
virtual span, backing, ordinary retained owners and scratch/metadata peaks.
Correctness gates timing; this document claims no throughput or runtime result.

Implementation needs generic group/boundary metadata, same-tensor alpha mapping,
exact partial-region dependency and publication, materialized planar callbacks
and the proposed member registrations. Deliver actual public compile/execute
examples with independently checked expected output. The removed legacy
alpha.associate/alpha.unassociate implementations used typed Image paths
and binary64 intermediates; neither their historical tests nor the current planar CPU
subset implements this spec. Previous arithmetic-only checks do not establish
the revised boundary or missing-alpha coverage behavior.
