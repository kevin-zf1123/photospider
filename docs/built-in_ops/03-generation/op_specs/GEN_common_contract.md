---
spec_schema_version: 1
id: GEN-common
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# Generation shared contract

These are target specifications, not registry/ABI implementation claims. Inherit [NUM
common](../../01-numeric/op_specs/NUM_common_contract.md), [NUM
accelerated](../../01-numeric/op_specs/NUM_accelerated_contract.md), [FMT
common](../../02-format-color/op_specs/FMT_common_contract.md) and [FMT
color](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md). The
[generators](../generators.md) and [paths](../paths.md) indexes link every member;
[oracle coverage](../oracle-coverage.md) states current mathematical evidence.

## Coordinates, descriptors and numerical baseline

Canvas H,W>=1 is static; rank1..8 and logical element count<=2^40 with checked
arithmetic. origin_x/y are Int64, default0. Pixel cells are [ox+x,ox+x+1] by
[oy+y,oy+y+1], centers at half units, x right/y down. Logical HW/HWC axes do not imply
interleaved storage. Images use planar storage. normalized_edge uses local
(x+1/2)/W,(y+1/2)/H, while pixel coordinates include global origin. Cache identity
includes origin; ROI does not reset it. Construct large coordinates exactly; never
recover discrete addresses from rounded floating coordinates.

Float ports accept their explicitly declared Float32/64 types without hidden
broadcast/promotion. RN_T denotes correctly rounding the complete real/rational
expression once, except explicit public-node or RN64 stages. Use ties-to-even, gradual
underflow and restored floating environment, not fast-math/FTZ. Ordinary computed exact
zero is +0 unless a member declares otherwise; copy/endpoint paths preserve source bits
and nonzero negative underflow retains -0. Raw constant copies allow nonfinite bits;
semantic geometry/color inputs require finite values.

Accelerated Float32 finite results obey the established four-ULP limit. Float64 inputs
are not narrowed; normal finite FP32-range reference r permits
abs(a-r)<=4*2^(floor(log2(abs(r)))-23). Zero, outside-normal-FP32-range and uncertain
classification use strict behavior. Discrete decisions/copies remain exact. This is a
final-output budget, not per-tap/octave error. Admission requires proved bounds or
strict fallback, not only sampled agreement.

PathSet controls are finite Float64; exact polygon Results use their separately
specified rational representation. DynamicPoints use finite Float64 positions and
associated Int64-width IDs. Zero Result counts are valid; ordinary Value extents are
positive.

## Publication, resources and error boundaries

Validate shape/dtype/schema before evaluation; static parameters and input descriptors
alone determine Value shapes. Dynamic counts use formal Results. No Python dictionary or
oracle fixture constitutes a public codec or association. Accept valid negative/zero
strides, unaligned offsets and nonzero source origins. Return actual global Region,
layout, owner and produced coverage; missing is not ExplicitZero. Materialize only the
range required by the member execution rule. No data-dependent identity/view guess is an
implicit contract.

Charge outputs, scratch, exact integer limbs, events/indexes, backing, validation
I/O/work and retained ancestry. Work/stage/capacity are distinct; managed peak does not
promise RSS. Poll bounded loops and preserve ResourceExhausted, Cancelled, Stale and
upstream statuses. Invalid parameters, NoSolution, NotConverged, InvalidQuality and
ArithmeticOverflow remain distinct. Never mask budget failure with strict fallback or
lower quality. Complete Results publish atomically; independent completed outputs follow
host rules. Retained values/read windows remain valid after context destruction until
their final owner is released.

## Members and identity

The family identifiers are GEN-01..08, NOI-01..10 and PTH-01..12 with 91 original member
concepts and separately specified variants. Concrete technical details are completed per
family. Distinct output semantics, units, color rules and reproducible algorithm
identities receive distinct members. Candidate count is not a registry-key count.
Solver-specific fitting members and exact/grid geometry variants may expand it.

Every public operation key includes a version. Within the same operation version and
public profile, identical valid inputs and parameters produce identical output bits,
counts, order, IDs and topology. This includes CPU dispatch and all supported devices of
one GPU-backend profile. Different profiles may differ only within their specified
numerical contract. Accelerated error bounds do not authorize within-profile output
drift.

Changes to FMA use, reduction order, approximations, fallback selection or solver
behavior require a new version when they change otherwise legal outputs. Bit-preserving
optimization can retain the version. Contract violations are bugs; a version bump does
not legalize them. A correction affecting stored results must invalidate affected caches
and document its impact.

## Execution and backend scope

Use exact requested-region computation for local members, necessary input halos for
neighborhood members, and Whole execution for genuinely global algorithms. Each member
separately specifies output computation, input reads, complete input validation and
dirty propagation. Regional output does not imply regional control validation: a local
fill may still require complete path validation.

Global-coordinate outputs are invariant under request size, tile partition, request
order and thread scheduling. Do not derive RNG addresses from tile IDs, thread IDs,
execution order or local ROI origins.

Provide selected GPU members as real workloads for subsequent scheduler work; Perlin
noise is a candidate workload. Validate through public workflows including demand,
allocation, transfers, dependencies and result return, rather than only isolated device
kernels. GPU profiles are separated by backend. All admitted devices within one backend
profile must reproduce its versioned result bits. Cross-backend substitution requires
explicit workflow permission to change profile; it is not an invisible scheduling
optimization.

## Color coverage

Preserve the established FMT baseline: straight image representation, explicit
same-tensor alpha roles, complete color interpretation, and Lab/LCh lightness l=L*/100.
Never infer RGB/RGBA from three/four raw components.

Choose support by operation: constant images validate and copy models supported by
complete descriptions; color ramps define RGB, XYZ, CMYK, Lab, LCh, OKLab, OKLCh, HSL
and YCbCr separately and follow the current CRV/FMT mathematics. GEN-07 uses generic
numeric components; NOI-09B requires linear RGB.

## Geometry object, error and topology

Keep original-geometry operations and explicitly named approximation operations
separate. Exact polygon area concerns the chosen polygon. A flatten-based member
computes on its deterministic published polyline; correctly rounding that area is not
correctly rounding the source curve's area. Ellipse area, circular stroke boundaries and
true-curve distance still require their own algorithms.

Flatten accepts epsilon_geom_px > 0 in target-canvas pixel units. Constructors write
0.05 by default; an explicit preview preset writes 0.25. Persist the numeric value and
include it in cache identity. These are engineering defaults, not measured optimal
settings. Include flattening and coordinate-publication error in the bound. Subsequent
transforms require propagation/revalidation of that bound. Do not silently loosen it on
budget exhaustion.

Default source_topology_policy is allow_change. reject_unproven explicitly requires
proof of the topology properties declared by the member. Distance error alone proves
neither coverage error nor topology. Report unverified when no proof/check was
performed; report actual change only if established. An unproved strict request fails.
Fixed algorithm/version/parameters also fix the approximation bits.

## Structured results, attributes and width

Add formal Result schemas where existing types cannot accurately express fields, source
associations and quality. Review ArcLengthTable, PathSamples, PathPartition, FitReport
and GeometryReport as candidates; five names do not mandate five new schemas. Review
factories, codecs, ownership, lifetime, validation, counts and consumers. JSON examples
are not implemented associations. Exact geometry Results defined by the geometry
contracts belong to this review as well.

Default attribute_policy=reject_unmappable. Explicit drop_unmappable drops only
unmappable attributes and reports their keys and reasons. Propagate attributes with
defined correct mappings; do not invent interpolation for labels or IDs. Failure must
not publish a successful result with silently missing attributes.

Width denotes finite nonnegative full stroke width (diameter), in geometry pixel units.
Support a formal attached width attribute and an independent width input; each
invocation explicitly selects exactly one source, with no fallback/override precedence.
Freeze the standard key during schema review; do not infer semantics from arbitrary
attribute names.

Explicitly distinguish these parameter domains (field spelling remains a schema
requirement): pathset_normalized_arclength, subpath_normalized_arclength, and
subpath_arclength_px. The first concatenates subpath lengths in declared order,
excluding spatial jumps; the second restarts [0,1] on each subpath; the third restarts
physical distance on each subpath. Store the domain with the binding. Do not silently
reinterpret an existing generic ArcLength tag.

Trim/dash preserves source-position width by default. Carry or rebuild the source
mapping; reapplying the full width function to each output fragment requires an explicit
rebind. An unrepresentable mapping follows the attribute failure policy, not approximate
interpolation. This preserves width, not every raster pixel: new endpoints can introduce
new caps. Zero length, closed seams, domain bounds and exact mapping representation
remain unimplemented member/schema details.

## Periodic coordinates and two-circle roots

Repeat uses the exact mathematical coordinate t-floor(t) in [0,1), then normal NUM
rounding. Exact period boundaries output +0. A value inside the period that rounds to 1
remains 1: no nextDown adjustment or second wrap. The following lookup interprets it as
the endpoint. Angular periodic coordinates follow this rounding policy; pad/reflect
retain their separately defined endpoints. Periodic query mapping does not normalize
stored color hue or remove winding. CRV/FMT original hue interpolation remains
authoritative.

For two circles define q=p-c0, d=c1-c0, s=r1-r0 and solve A*t^2+B*t+C=0 with A=d.d-s^2,
B=-2*(q.d+r0*s), C=q.q-r0^2. Use exact classification, not epsilon snapping. Keep roots
with r0+t*s >= 0; select the largest valid real root before spread, without clamping to
[0,1]. A=0 uses the linear equation when applicable.

Identical circles are invalid input. For other circles, if A=B=C=0, inspect the radius
constraint: s<0 gives maximum t=-r0/s; s>=0 with nonnegative input radii has no finite
maximum. Return the finite maximum if present. Treat absence of roots or of a finite
maximum as absence of a returnable coordinate.

Default no_solution=valid_zero returns t=+0 and UInt8 valid=0; a returnable coordinate
has valid=1. Explicit reject fails on invalid requested coordinates. valid is geometric
coordinate validity, not alpha. Preserve resource, cancellation, arithmetic and
unfinished-certification errors; they cannot become valid=0.

## Generic mesh fields

Retain rectangular bilinear, triangle mesh and regular-parameter bicubic patch members.
Do not include arbitrary curved geometry inversion or folded patches. Use generic
numeric interpolation: accept an explicit component axis and produce numeric fields,
without inferring color or alpha semantics. No special alpha interpolation is provided.

Bilinear uses four corner vectors and the full weighted expression. Triangle mesh uses
exact containment/barycentric decisions and independent geometric validity; concrete
boundary/overlap rules must be fixed with the member. Bicubic uses the 4x4
tensor-product cubic Bernstein control vectors on the regular parameter domain, not an
unspecified cubic filter. No channel receives special alpha treatment.

Color interpretation, conversions and alpha weighting belong to explicit workflows.
Their individual public-node rounding stages are part of the workflow contract and must
not be described as one final rounding of a fused expression.

## Random identity

Use Philox4x64-10 with direct, lossless address encoding into its 256-bit counter.
Global x/y are signed32. frame, draw and stream each range 0..2^32-1, carried by
existing Int64 parameters. Retain channel 16-bit and built-in domain 8-bit pending full
layout review; seed denotes a 64-bit bit pattern. Reject out-of-range addresses rather
than truncating or wrapping.

The 184 address bits fit without lossy hashing. Concrete field order, seed-to-key
injection, unused bits, output-word selection, floating-grid mappings and each member's
draw allocation remain member implementation requirements. The candidate c0=(x,y),
c1=(frame,draw), c2=(stream,channel,domain), c3=0 layout is a candidate, not a frozen
sequence. Unused bits cannot acquire new semantics silently.

Address injectivity is not uniqueness of individual output samples or a proof of
statistical independence across keys. Do not seed implicitly from node identity, clock,
pointers or scheduling. The integer core agrees across CPU/GPU profiles; floating
transforms follow their declared profiles. Core known-answer tests validate
Philox4x64-10 independently of the candidate addressed sequence; addressed goldens
require a fully specified production packing and floating mapping.

## Point distributions and completion

Keep finite-candidate rejection and FIFO-active Bridson as separate members. Finite
rejection examines candidates in ordinal order and accepts only published points
satisfying the exact squared minimum-distance predicate. FIFO always processes the
earliest remaining active point; success retains it and appends the new point, while
exhausting its fixed attempts removes it. IDs follow acceptance order. Spatial indexes
and parallel candidate evaluation must preserve decisions.

max_count is a normal stop target: reaching it succeeds; exhausting candidates or the
active list first succeeds with fewer points. Report the stop reason, with final field
names to be reviewed. Candidate count, attempt count and max_count are algorithm inputs
in cache identity. Neither completion nor count satisfaction proves maximal packing or a
fixed density. No promise of exactly N points is made.

Memory/work/iteration safety budgets remain separate hard limits. Exhaustion fails
without successful partial publication; it is not evidence that no additional point
fits. Freeze boundary cases and member address allocation with the RNG.

## Rank resources

Deliver generic rank lookup semantics first. Validate structure, layout, ranges,
addressing and immutable resource identity. A permutation/rank fixture proves neither
spatial blue-noise quality nor temporal STBN quality.

Named blue/STBN production capability remains gated on selecting a specific
resource/release or generator, recording provenance, license and byte SHA256, and
accepting spatial/temporal spectrum and threshold-set quality reports. Resource
conversion produces a new identity and requires revalidation. No production resource or
external SDK is approved by these specifications. Generic lookup can exercise real
resource/demand/scheduler paths without a blue-noise claim.

## Noise model boundaries

Keep separate electron Poisson shot, integer-looks multiplicative speckle and linear-RGB
artistic grain. Shot consumes nonnegative expected electrons and returns Int64 counts;
it adds no implicit dark current, read noise, gain, black level, full well or ADC. The
finite random grid is not an exact continuous ideal distribution. Speckle retains a
separate explicit multiplier formula; signed raw inputs do not acquire
physical-intensity meaning.

Grain forms correlated Gaussian g, then RN_T(C+strength*g). Expose explicit
normalization=none/sum/l2; strength is only the final multiplier, not a guaranteed
standard deviation. sum requires nonzero sum of weights; l2 requires nonzero sum of
squares. Boundary aliases refer to the same source variable, not independent samples.
Persist normalization, kernel, boundary, strength and RNG parameters.

Retain monochrome/shared and independent-RGB addressing choices. Apply the same RGB
formula even at alpha=0, modifying hidden RGB; copy alpha bits unchanged. Do not clip
finite signed/HDR results. Other working domains or a calibrated camera pipeline require
separate explicit members/workflows.

## Geometry variants, publication and fitting

Define exact-input and explicit-grid Boolean members separately. Exact members interpret
input floats as exact dyadic geometry. Grid members specify quantization, scale,
arithmetic and observable backend behavior, and report quantization effects. No
automatic fallback between them, snapping or small-feature removal is authorized for
exact members. Grid behavior must also be specified rather than inferred from a library
name. The geometry approximation contract's source-curve approximation permission does
not authorize additional unreported Boolean quantization.

Provide separate exact Result output and Float64 PathSet output members. Exact polygon
results retain rational coordinates/topology for exact consumers. Float64 output
performs deterministic rounding and validates displacement, incidence and absence of
newly introduced crossings/merges before publication; failure is InvalidQuality. Define
an explicit exact-to-Float64 conversion with matching publication rules. Chained Float64
outputs operate on the newly rounded geometry, not an implicitly preserved exact
history. Grid-to-Float64 conversion also requires its representability checks. Exact
offset/circular constructions may need a different number domain; rational polygon
support does not prove them.

Validate candidate backends before selecting production dependencies. Check crossings,
overlaps, contacts, holes, degeneracy and narrow gaps; exact Result round trips and
chained consumption; canonical output order; accounting, cancellation and lifetime; and
publication failures. Review license/build/platform requirements before adopting a
concrete package. No CGAL/Clipper2 backend is selected by this specification.

Different fitting solvers are different public operators. They share quality acceptance:
endpoint/locked/corner constraints and a continuous bidirectional Hausdorff bound
including publication error. Sample residuals are not a continuous certificate. Each
solver operator/version/profile fixes initialization, solving, splitting, tie handling,
stopping and returned bits. Distinct solvers may produce different valid
controls/counts. No silent runtime solver substitution and no minimum-segment optimality
promise. Register only after the concrete solver and verifier are validated and frozen.

Fitting defaults to allow_change topology policy, with explicit reject_unproven. Specify
topology properties for that member; distance success does not prove them. Unverified
topology is reported as such; strict unproved results fail. Resource exhaustion cannot
publish an uncertified best guess.
