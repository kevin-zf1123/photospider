---
spec_schema_version: 1
id: MASK-common
kind: shared_operator_contract
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: partially_implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK: shared specification and execution baseline

This specification defines the selected MASK-01..17 semantics through explicit
members and family contracts. MASK-01..11, MASK-14 and MASK-17 have all 43
members registered in the bundled strict CPU set through fixed invocation ABI 8.
The remaining 20 members are unimplemented proposals. Member front matter
separates registered keys and implementation status from the Proposed spec status.
Independent Python
oracle coverage also includes unimplemented members and does not establish their
registration. No paged-components backend is registered.

## Authority and inherited numerical rules

Inherit [NUM-common](../../01-numeric/op_specs/NUM_common_contract.md),
[NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md),
[FMT-common](../../02-format-color/op_specs/FMT_common_contract.md),
[FMT-COLOR](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md),
[relative coordinates](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
and the [planar/codec boundary](../../02-format-color/op_specs/FMT_codec_boundary.md).
Precedence: member > family > this file > inherited generic baseline. FMT's
explicit interpretation policy and NUM numerical accuracy both apply. References import only the named shared obligations,
not another operation's ports, defaults or complete-input callback.

## Static structure and unchecked execution

Compilation and static preparation check schema/version, member directories, dtype, rank, positive extents, axes and shape relations, parameter presence/types/syntax, fixed reads/writes and scratch, and static capacity arithmetic. Runtime counts, offsets, indices, label IDs, associations, canonical padding/encoding and numerical/geometric conditions are unchecked caller premises. Finite/range/order/positivity conditions remain numerical premises even when supplied as static parameters. Producers construct the specified results and consumers trust those premises, without payload scans or publication validators.

Outside those premises no result, diagnostic or recovery is guaranteed. Preserve algorithmic predicates, candidate selection, explicit threshold/clipping behavior, rounding refinement and requested quality measurements. Resource exhaustion, cancellation and defined no-solution outcomes for valid inputs remain operational. Oracle helpers may diagnose invalid reference requests; those checks are tooling behavior, not required production failures. Fixed invocation ABI 8 and the frozen kernel retain structural, ownership and lifecycle checks under the [execution contract](../../../kernel-architecture/Compiler-and-Execution.md).

## Numerical authority

All numerical rounding and strict/accelerated accuracy rules are inherited from
[NUM-common](../../01-numeric/op_specs/NUM_common_contract.md) and
[NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md).
MASK defines no alternative rounding mode or tolerance. RN_T denotes the NUM
nearest/ties-to-even result in the declared Float32/Float64 dtype, with gradual
underflow. Strict expressions retain the rounding boundaries explicitly stated
by each member; Float64 is not narrowed. Copies, discrete predicates, indices,
counts, selected endpoints and special values follow the exact NUM obligations.
Preserve the caller's floating environment and the NUM failure/fallback rules.
MASK members specify their formulas, domain restrictions, signed-zero selection
rules and any stage boundaries. Stored floating samples and static parameters
represent their actual binary values. Numerically consumed samples exclude NaN as an unchecked premise.
The distance no-feature infinity convention below defines a domain value, not an
arithmetic rounding exception or permission to suppress finite overflow.

Strict is the registered profile for the implemented subset. An accelerated profile requires
an explicit member/key and inherits NUM-acceleration, including exact discrete
decisions and its prescribed fallback rules. Backend/ISA identity belongs in
operation/profile identity and implementation measurements. Integer dimensions, coordinates, IDs and counts
use exact arithmetic. Static dimensions and capacity products are checked; runtime index/ID/count validity and representable arithmetic results are caller premises. Generated Binary values are floating +0 and 1; the Binary input domain includes either sign of zero. Identity copies retain bits.

## Tensor domains, ports and static inference

All ports carry Results with fixed Tensor member schemas. A member's tensor notation describes its Result payload, and all listed inputs are required unless explicitly optional. Member port order is normative.
Spatial primitives use one rank-two `[H,W]` tensor. H,W are positive and N=H*W
is checked, at most 2^40 and within all host layout/address limits. Empty images
are not introduced; an empty requested Region is legal. Per-image batching is
explicit authoring, not an implicit extra axis. MASK-04/11/17 use planar
`[C,H,W]` as expressly listed, with C positive and total elements <=2^40.
Coordinates are global, zero-based `(y,x)`, pixel centers at those integer
coordinates, cells `[y-.5,y+.5] × [x-.5,x+.5]`. x grows right; y grows down.
Spacing `(sy,sx)` consists of positive finite Float64 values in an explicitly
selected distance unit; default authoring units are pixels with both spacings 1.
No metadata-derived mm/px conversion occurs implicitly.

| Alias | Storage and consumed sample domain | Output meaning |
| --- | --- | --- |
| Coverage | Float32 or Float64, finite [0,1] | Soft mask; not a color/alpha attachment |
| Binary | Float32/Float64, exactly numeric 0 or 1 | Binary membership; 255 is outside the domain |
| Field | Float32/Float64, finite, signed/HDR allowed | Numeric field, not coverage |
| Distance | Float32/Float64, nonnegative or signed; no-feature infinities as specified | Explicit units/metric/basis/no-feature semantics |
| Labels | Int64, 0 background, positive foreground IDs | Explicit labeling basis, not a color plane |

Floating inputs to one arithmetic member have the same dtype unless the member
explicitly allows independent types. Coverage/binary-preserving outputs retain
input dtype. MASK-03 takes Field; MASK-04 takes Field color components; MASK-10
uses an explicit output_dtype. No Float16, UInt8 mask storage, packed bits, implicit UInt8 255
normalization, automatic thresholding or hidden conversion is defined. Bool binary
storage may be added later; its layout and conversion interface are not specified.
MASK-03E accepts raw UInt8 bytes only as an explicit input conversion and emits
Float32/Float64 Binary selected by output_dtype. Other mask outputs preserve the
primary mask input dtype; flood outputs use the seeds dtype, MASK-12G uses an
explicit output_dtype, and validity/within flags use their associated value dtype.
Skeleton outputs preserve the input Binary dtype independently of radius dtype. Binary-only consumers assume the binary domain on their required sample set. Floating coverage morphology computes directly on soft samples.

Results carry structural schemas and samples. Spatial coordinates, coverage/binary meaning, distance units/metric and label basis are explicit operation configuration or declared schema semantics. Outputs carry no attached descriptions or sample-validity proofs. The caller supplies the next consumer's interpretation. Semantic color selection checks a complete static group description using the operations-SDK TensorDescription v4/v5 codec and native CIELAB/CIELCh l=L*/100. Only the formula's selected coordinates are numerical inputs; group completeness adds no sample read closure. Raw channel arithmetic reads only its formula operands. MASK-04E/F use the registered operations-SDK model/skipped schema factories described in their specs. Other proposed domain helpers remain implementation gates; descriptive names do not add kernel facets or codecs.

All statics listed by a member must be supplied by direct WorkflowDocument
nodes; authoring recommendations are serialized by helpers. Reject unknown,
missing, ill-typed and conditionally irrelevant fields before payload reads.
Finite Float64 statics must not accept a Boolean as a number. Enum tokens are
case-sensitive. Integer parameters are true Int64, never routed through double.
Selectors use NUM's canonical comma-separated nonnegative index String syntax.
An ordered unique channel list preserves order and has exactly the declared C
entries. The custom footprint encoding is described in MASK-05. Output
shapes and static Result schemas are inferred solely from descriptors/statics.
Effective K is an ordinary Int64[1] payload; fixed positive capacity is derived during preparation and unused rows are zero-filled.

## Boundary, demand and publication

`Q` is the mathematical output set; execution first normalizes the selection to complete retained output tiles and derives support for all of their coordinates. `D` is the finite image
canvas. `Data` and `Control` describe mathematical sample support; `Descriptor` denotes static schema requirements. There is no additional Validation read set. Compilation checks every declared descriptor even for empty Q; empty Q adds no runtime payload or iterative work. Source-grid closure and static supersets may bind tiles beyond mathematical support, but every actual read must stay within a prepared view. Extra bound samples do not become arithmetic operands or validation inputs. Missing coverage is not zero. Out-of-canvas zero is a mathematical constant, not permission to read unprepared storage.

Elementwise members list exact same-position support. A finite footprint B
uses union `{p+b:p in Q,b in B} intersect D`, or the explicitly defined boundary
mapping. A composed operator must propagate support through ALL stages, not
claim the first stage's halo. complete-input means any nonempty demand requires the
specified complete input domains; it remains a global operation under tiling.
Support is derived from these Data/Control relations. Lowering closes the declared support on source tile grids and emits a finite fixed read set, including conservative supersets for data-dependent addresses. Complete-input requires the declared complete domains as mathematical input. Invocation cannot discover new producers or enlarge its reads.

MASK-05 and MASK-06 use zero extension on the infinite integer lattice. For
opening/closing the intermediate is NOT cropped and re-zeroed: evaluate the
mathematical stages outside D where needed and crop only the final output.
Explicit pad/stage/crop compilation is required for a composite workflow.
This distinction is observable on a full 1×1 image and is tested. Other boundary
choices are not silently inherited from SciPy/OpenCV defaults. Gaussian alone
adds named replicate and half-sample-reflect boundary mappings.

Requests normalize to complete retained output tiles. Each tile has an independent allocation and one declared producer; the operator initializes all declared writes, including member-defined padding. Explicit COPY tasks implement materialized identities, while complete aliases retain the existing Result identity only under the frozen kernel rules. Member default `layout=materialize` is an authoring policy, not an enum accepted by every primitive. A successful task enables its internal dependents; the call publishes selected Result exports in IR retirement order after all required tasks succeed. Static capacities, ordinary count payloads and source ObjectId associations are constructed according to the member contract, without a publication scan. Missing coverage is not an all-zero table.

## Failure mapping and resources

Use existing public Status/FailureReason enumerators, not the diagnostic labels
below as new enum values. Preserve the NUM host phase/origin conventions.

| Failure | Phase | Status / reason | Scope |
| --- | --- | --- | --- |
| Missing/mistyped static parameter, invalid syntax/enum or static structural constraint | Compile/preparation | InvalidArgument / InvalidDomain | Schema origin; no fabricated sample atom |
| Wrong dtype/rank/shape/basis/unsupported semantic description | Compile/preflight | TypeMismatch / None, except existing metadata codec error | Actual descriptor origin |
| Violated sample, numerical, geometry, count, ID, seed/marker or representation premise | Outside the contract | No validation or diagnostic guarantee | Caller responsibility |
| Exhausted algorithmic refinement or declared output capacity on valid inputs | Evaluation | Member-defined operational failure | Failing task and its call |
| Capacity/work/stage/refinement exhausted | Admission/evaluation | ResourceExhausted / CapacityLimit, WorkLimit or StageLimit | Preserve host resource scope |
| Mismatched runtime Result association | Outside the contract | No comparison or diagnostic guarantee | Caller responsibility |
| Unsupported backend | Capability | BackendUnavailable / None | Backend origin |
| Cancellation, upstream, stale input | Host phase | Preserve original Status | Preserve original scope |

Distance is the only domain here admitting no-feature infinities. Full unsigned
nearest distance returns +Inf when no site exists; signed center distance returns
+Inf for background without foreground and -Inf for foreground without background.
There is no separate valid output for full distances. Missing nearest coordinates
are (-1,-1); coordinate sentinels alone do not establish absence. Finite-distance
arithmetic must remain representable as a caller premise; an overflowing finite computation is outside that domain and is not a no-feature result. NaN is outside the Distance domain. Truncated distance returns the signed/unsigned finite limit and a
Binary within flag. Field and Coverage remain finite-only.
Distance-aware consumers MASK-07B/C and MASK-09 explicitly admit these infinities:
finite shifts preserve sign of infinity; threshold at finite r maps -Inf to 1 and
+Inf to 0; feather maps -Inf to 1 and +Inf to 0, including zero-width feather.
An operationally failed task prevents successful publication of its call. Already retired calls retain their Results; selected outputs of one call share its publication outcome. Iterative work uses explicit Plan tasks and CFG state under the same retirement and cancellation rules.

For each member charge output, scratch, index structures, exact-integer limbs,
coefficient enclosures, retained inputs and simultaneous old/new allocations.
Python object sizes are NOT production resource accounting. CPU is required;
GPU/JFA/FFT/engine adapters are not automatically admitted. Production execution
loops must poll at most every 1024 elementary visits and every iteration,
including exact arithmetic refinement. Work-cap exhaustion fails, not an early
approximate answer. Complete-input members have no implicit spill, unbounded
workers, auto-eviction or paging capability; none is registered by the current
bundled MASK implementation set.

## Independent acceptance and public implementation gate

[Oracle README](../../../../oracle/ops/mask_morphology/README.md) gives
commands, certified rational/MPFR scope, golden cases and optional external
comparisons. Every member names its oracle entry and analytic case. Numeric
checks do not replace separate schema/fixed-support/call-publication testing.
All local members need whole/selected-tile/nonzero-origin/disjoint-tile
equivalence, prepared-view bounds, odd strides and empty Q; globals need whole-demand proof,
cross-tile connectivity and explicitly no fixed-halo shortcut. Exercise invalid
static structure, parameter presence/types/syntax, signed zeros, dtype extremes, no-feature/empty-component results,
correctly paired associations, cancellation, low budgets, independent Runs and retained owner lifetime after context destruction. Test independent/joint output selection at call granularity. Invalid sample/association fixtures may exercise only the oracle's diagnostics; production checks use valid payloads and do not require rejection of violated premises.

Native acceptance requires public Compiler/ExecutionContext workflows, fixed-view checks and retained Result reads. Conceptual DAGs and Python oracle calls do not establish native execution.
Performance measurements identify hardware, build, dtype,
dimensions, radius, workers, Plan, repetitions, latency and managed backing.
The fixed ABI has no per-sample diagnostic callback; fallback frequency is
reported only when instrumented. External sources establish definitions or
feature existence only; commercial bit identity remains unverified.

## Registered implementation limits

The 43 implemented members accept the optional max_work Int64 operational guard,
default INT64_MAX, checked as nonnegative during preparation. It limits counted
work per kernel invocation; it is not a global per-Run sample quota. Work and
cancellation polls occur within at most 1024 elementary visits and at every
refinement. Host output/scratch admission still follows ExecutionContext limits.

Exact rational storage, fixed-width accumulation and directed refinement have
bounded capacity. Work or capacity exhaustion returns ResourceExhausted; an
unresolved algebraic/transcendental certification returns OperationFailed.
These are operational failures on a strict path, never approximate fallbacks.
MASK-17C empty_policy=error with a zero denominator is OperationFailed; selecting
valid alone evaluates no image samples and returns zero validity without that
value failure.
