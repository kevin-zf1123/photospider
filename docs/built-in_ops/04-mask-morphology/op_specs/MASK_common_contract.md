---
spec_schema_version: 1
id: MASK-common
kind: shared_operator_contract
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK: shared specification and execution baseline

This specification defines the selected MASK-01..17 semantics through explicit
members and family contracts. Native implementation remains pending. The target
registry keys are declared by the member specifications;
no legacy key aliases or compatibility implementations are part of this specification.
The separately owned paged-components factory is outside this replacement scope.
The supplied Python oracles evaluate proposed mathematics independently of the
C++ kernel. Passing them is not evidence that any proposed key is registered.

## Authority and inherited numerical rules

Inherit [NUM-common](../../01-numeric/op_specs/NUM_common_contract.md),
[NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md),
[FMT-common](../../02-format-color/op_specs/FMT_common_contract.md),
[FMT-COLOR](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md),
[relative coordinates](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
and the [planar/codec boundary](../../02-format-color/op_specs/FMT_codec_boundary.md).
Precedence: member > family > this file > inherited generic baseline; FMT's later
metadata-consumption policy supersedes historical blanket facet clearing, not
NUM numerical accuracy. References import only the named shared obligations,
not another operation's ports, defaults or Whole callback.

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
represent their actual binary values. NaN is rejected when consumed numerically.
The distance no-feature infinity convention below defines a domain value, not an
arithmetic rounding exception or permission to suppress finite overflow.

Strict is the initial proposed registry profile. An accelerated profile requires
an explicit member/key and inherits NUM-acceleration, including exact discrete
decisions and its prescribed fallback rules. Backend/ISA identity belongs in
cache identity and diagnostics. Integer dimensions, coordinates, IDs and counts
use checked exact arithmetic. Generated Binary values are floating +0 and 1;
validation accepts either sign of zero. Identity copies retain bits.

## Tensor domains, ports and static inference

All input ports are required Values unless an individual member explicitly names
a Result. There are no hidden optional inputs. Member port order is normative.
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
| Binary | Float32/Float64, exactly numeric 0 or 1 | Binary membership; 255 is rejected |
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
Skeleton outputs preserve the input Binary dtype independently of radius dtype. Binary-only consumers validate the binary domain on their required
sample set. Floating coverage morphology does NOT threshold soft samples.

Outputs inherit applicable spatial descriptions. Generated masks remove color,
transfer, profile, alpha-association and label/distance meanings and establish
coverage/binary semantics. Numeric distances remove color/coverage semantics;
labels establish their own basis. Descriptions are not sample-validity proofs.
Raw channel arithmetic never validates unrelated color merely because metadata
is attached. Semantic color selection consumes the complete selected color
group in the FMT v3 relative coordinate convention. No old v1 Lab payload is
silently reinterpreted. Exact runtime codecs for any new mask/distance metadata
must be reviewed before registration; descriptive names in this draft are not
invented working facet IDs. Ordinary generic tensor outputs remain usable with
explicit call-local interpretation during prototype work.

All statics listed by a member must be supplied by direct WorkflowDocument
nodes; authoring recommendations are serialized by helpers. Reject unknown,
missing, ill-typed and conditionally irrelevant fields before payload reads.
Finite Float64 statics must not accept a Boolean as a number. Enum tokens are
case-sensitive. Integer parameters are true Int64, never routed through double.
Selectors use NUM's canonical comma-separated nonnegative index String syntax.
An ordered unique channel list preserves order and has exactly the declared C
entries. Proposed custom footprint encoding is described in MASK-05. Output
shapes and static Result schemas are inferred solely from descriptors/statics.
Dynamic K belongs in RuntimeCount, never in an ordinary zero-extent Value.

## Boundary, demand and publication

`Q` is the set of requested logical output coordinates. `D` is the finite image
canvas. `Data`, `Control`, `Validation` and `Descriptor` are distinct sets;
required reads are their union. Every descriptor is checked even for empty Q;
empty Q has no runtime sample, control, validation or iterative work. No
bounding-gap, row-padding, unrequested channel or missing-page over-read is
permitted. Missing coverage is not zero. Out-of-canvas zero is a mathematically
specified constant, not permission to read an unprepared page.

Elementwise members list exact same-position support. A finite footprint B
uses union `{p+b:p in Q,b in B} intersect D`, or the explicitly defined boundary
mapping. A composed operator must propagate support through ALL stages, not
claim the first stage's halo. Whole means any nonempty demand requires the
specified complete input domains; it remains a global operation under tiling.
Dirty support is the forward transpose of these same relations, including
Control/Validation. Declared Whole closures are conservative formula demand
but exact admitted payload/validation obligations; do not describe them as
minimal mathematical dependencies. Future sparse proofs require new traits.

MASK-05 and MASK-06 use zero extension on the infinite integer lattice. For
opening/closing the intermediate is NOT cropped and re-zeroed: evaluate the
mathematical stages outside D where needed and crop only the final output.
Explicit pad/stage/crop compilation is required for a composite workflow.
This distinction is observable on a full 1×1 image and is tested. Other boundary
choices are not silently inherited from SciPy/OpenCV defaults. Gaussian alone
adds named replicate and half-sample-reflect boundary mappings.

Dense outputs publish exactly their requested global coordinates with canonical
planar physical pages, immutable owned backing and verified valid coverage.
Member default `layout=materialize` is an authoring policy, not a payload enum
accepted by every primitive. No forced-view API is promised. Copy identities
may reuse immutable storage only if ownership, descriptor changes and all
required validation/observations survive. Result bundles publish atomically
where stated, with RuntimeCount and exact source ObjectId associations. There
are no independently valid partial tables or "all-zero means missing" shortcuts.

## Failure mapping and resources

Use existing public Status/FailureReason enumerators, not the diagnostic labels
below as new enum values. Preserve the NUM host phase/origin conventions.

| Failure | Phase | Status / reason | Scope |
| --- | --- | --- | --- |
| Invalid/missing static or inconsistent enum field | Compile/preflight | InvalidArgument / InvalidDomain | Schema origin; no fabricated sample atom |
| Wrong dtype/rank/shape/basis/unsupported semantic description | Compile/preflight | TypeMismatch / None, except existing metadata codec error | Actual descriptor origin |
| Consumed NaN or forbidden Inf, mask outside [0,1], nonbinary Binary, bad seed/marker | Evaluation | InvalidArgument / InvalidDomain | Actual dependent observation; Whole validation closure where declared |
| Finite formula rounds to Inf; final integer overflow | Evaluation | OperationFailed / ArithmeticOverflow | Actual demanded arithmetic observation |
| Exceeded declared component-count domain cap | Evaluation | OperationFailed / InvalidDomain | Complete component bundle |
| Capacity/work/stage/refinement exhausted | Admission/evaluation | ResourceExhausted / CapacityLimit, WorkLimit or StageLimit | Preserve host resource scope |
| Invalid Result source association | Validation | OperationFailed / InvalidAssociation | Actual association/source identity |
| Unsupported backend | Capability | BackendUnavailable / None | Backend origin |
| Cancellation, upstream, stale input | Host phase | Preserve original Status | Preserve original scope |

Distance is the only domain here admitting no-feature infinities. Full unsigned
nearest distance returns +Inf when no site exists; signed center distance returns
+Inf for background without foreground and -Inf for foreground without background.
There is no separate valid output for full distances. Missing nearest coordinates
are (-1,-1); coordinate sentinels alone do not establish absence. Finite-distance
arithmetic overflow remains ArithmeticOverflow, never a no-feature result. NaN is
always invalid. Truncated distance returns the signed/unsigned finite limit and a
Binary within flag. Field and Coverage remain finite-only.
Distance-aware consumers MASK-07B/C and MASK-09 explicitly admit these infinities:
finite shifts preserve sign of infinity; threshold at finite r maps -Inf to 1 and
+Inf to 0; feather maps -Inf to 1 and +Inf to 0, including zero-width feather.
No failed observation or incomplete global iteration publishes a success Value.
Already independent host observations retain their existing terminal outcomes;
Whole does not manufacture per-pixel isolation that the runtime cannot provide.

For each member charge output, scratch, index structures, exact-integer limbs,
coefficient enclosures, retained inputs and simultaneous old/new allocations.
Python object sizes are NOT production resource accounting. CPU is required;
GPU/JFA/FFT/engine adapters are not automatically admitted. Production execution
loops must poll at most every 1024 elementary visits and every iteration,
including exact arithmetic refinement. Work-cap exhaustion fails, not an early
approximate answer. Only the already existing explicit paged-components factory
has its published disk recipe; new Whole members do not acquire implicit spill,
unbounded workers, auto-eviction, or a paging claim.

## Independent acceptance and public implementation gate

[Oracle README](../../../../oracle/ops/mask_morphology/README.md) gives
commands, certified rational/MPFR scope, golden cases and optional external
comparisons. Every member names its oracle entry and analytic case. Numeric
checks do not replace separate descriptor/support/dirty/publication testing.
All local members need whole/ROI/nonzero-origin/disjoint-tile equivalence, no
unrequested NaN reads, odd strides and empty Q; globals need whole-demand proof,
cross-tile connectivity and explicitly no fixed-halo shortcut. Exercise invalid
statics, signed zeros, dtype extremes, no-feature/empty-component results,
association mismatch, cancellation, low budgets, cache-off and retained owner
lifetime after context destruction. Test multi-output independent/joint behavior
only at the actually promised host granularity.

Each future implementation must provide a real public Compiler/ExecutionContext
workflow, registry inventory, source-support observations and status checks.
Current conceptual DAGs and Python oracle calls are not those workflows. Report
hardware/backend/revisions/dtype/N/radius/workers/cache/repetitions, median/tail
latency, managed output/scratch/retained peaks, cancellation latency and actual
fallback counts. No performance numbers or runtime conformance are claimed by
this documentation delivery. External sources establish definitions or feature
existence only; commercial bit identity remains unverified.
