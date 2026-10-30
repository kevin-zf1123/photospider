---
spec_schema_version: 1
id: GEN-common
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: partially_implemented
---

# Generation shared contract

These specifications define the category contracts. Members inherit [NUM common](../../01-numeric/op_specs/NUM_common_contract.md), [NUM accelerated](../../01-numeric/op_specs/NUM_accelerated_contract.md), [FMT common](../../02-format-color/op_specs/FMT_common_contract.md) and [FMT color](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md). The [generators](../generators.md) and [paths](../paths.md) indexes link every member. Existing NUM/CRV and FMT operations are reusable dependencies under their public contracts.

## Unchecked schema and payload contract

Generation schemas and random protocols are unchecked. Only static structure is checked: supported schema/version, member directory, element types, ranks, fixed positive extents, declared axes/layout, and the presence and types of static parameters. Dynamic counts, offsets, indices, IDs, association payloads, padding, random-address ranges, finite values and geometric/numerical domain conditions are caller preconditions. Producers construct the specified valid outputs without a separate publication validator; consumers assume these preconditions without scanning or comparing payloads. Static parameter types are checked, while their numerical or geometric domain validity is a caller premise unless imposed by the static Tensor structure itself.

Outside these preconditions, numerical results and diagnostics are not specified; invalid payloads do not promise rejection, a domain error, repair, clamping or fallback. A producer still follows the specified count, padding, identity and random-bit construction rules. Candidate rejection, convergence and correct-rounding refinement, explicitly requested quality measurement and normal stopping are mathematical algorithm steps. Completed outputs are not scanned or compared against numerical or geometric postconditions to decide publication; calling that check a quality certificate does not create an exception. Declared operational limit exhaustion, cancellation and no-solution behavior for valid inputs remain separate.

## Coordinates, descriptors and numerical baseline

Canvas H,W>=1 is static; rank 1..8 and logical element count <= 2^40, with checked arithmetic. origin_x/y are Int64, default 0. Pixel cells are [ox+x, ox+x+1] by [oy+y, oy+y+1], centers at half units, x right and y down. Logical HW/HWC axes do not imply interleaved storage. Images use planar storage. normalized_edge uses local (x+1/2)/W, (y+1/2)/H, while pixel coordinates include the global origin. Operation specialization identity includes the origin; ROI does not reset it. Large coordinates are constructed exactly; discrete addresses are never recovered from rounded floating coordinates.

Float ports accept their explicitly declared Float32/64 types without hidden broadcast or promotion. RN_T denotes correctly rounding the complete real/rational expression once, except explicit public-node or RN64 stages. Rounding is ties-to-even with gradual underflow and restored floating environment, not fast-math/FTZ. Ordinary computed exact zero is +0 unless a member declares otherwise; copy/endpoint paths preserve source bits and a nonzero negative underflow retains -0. Raw constant copies allow nonfinite bits; semantic geometry/color inputs require finite values.

Accelerated Float32 finite results obey the established four-ULP limit. Float64 inputs are not narrowed; for a normal finite FP32-range reference r, abs(a-r) <= 4*2^(floor(log2(abs(r)))-23). Zero, outside-normal-FP32-range and uncertain classification use strict behavior. Discrete decisions and copies are exact. This is a final-output budget, not a per-tap or per-octave error.

PathSet controls are finite Float64; exact polygon Results use their separately specified rational representation. DynamicPoints follow the [GEN-08 v1 schema](GEN-08_point_distributions_contract.md), with a static positive capacity, an ordinary count payload and zero padding. Valid rows use finite Float64 positions and associated Int64-width IDs. Zero Result counts are valid; ordinary Tensor extents are positive.

## Results and error classes

Static parameters and input descriptors alone determine Tensor shapes. Dynamic counts use formal Results. The canvas origin is an explicit coordinate parameter used in sample mathematics; it does not shift the tensor's schema-relative storage indices. Missing is not ExplicitZero.

Static structural errors and the operational outcomes NoSolution, NotConverged, InvalidQuality and ArithmeticOverflow for valid inputs remain distinct from each other and from ResourceExhausted, Cancelled and upstream statuses. A resource failure is never masked by a strict fallback or by lower quality. Complete Results publish atomically.

## Members and identity

The family identifiers are GEN-01..08, NOI-01..10 and PTH-01..12. Distinct output semantics, units, color rules and reproducible algorithm identities receive distinct members.

Every public operation key includes a version. Within the same operation version and public profile, identical valid inputs and parameters produce identical output bits, counts, order, IDs and topology. This includes CPU dispatch and all supported devices of one GPU-backend profile. Different profiles may differ only within their specified numerical contract. Accelerated error bounds do not authorize within-profile output drift.

An explicitly declared process-object association field, such as the GEN-08 report points_object_id, is exempt from cross-Run byte identity. Its producer declares deterministic=false; numerical samples, ordinal IDs, counts and stop reasons keep their deterministic contract. An object association is not a persistent content identity.

Changes to FMA use, reduction order, approximations, fallback selection or solver behavior require a new version when they change otherwise legal outputs. Bit-preserving optimization can retain the version. Callers maintaining external stored-result caches are responsible for versioned invalidation.

## Support and invariance

Regional, Halo and complete-input describe the mathematical support of a member: which input samples each output sample depends on. Input premises cover every consumed query or sample, not only the original ROI. A local fill may depend on the complete path without validating that payload.

Global-coordinate outputs are invariant under request size, tile partition, request order and thread scheduling. RNG addresses are never derived from tile IDs, thread IDs, execution order or local ROI origins.

GPU profiles are separated by backend. All admitted devices within one backend profile reproduce its versioned result bits. Cross-backend substitution requires explicit workflow permission to change profile; it is not an invisible scheduling choice.

## Color coverage

The FMT baseline applies: straight image representation, explicit same-tensor alpha roles, complete color interpretation, and Lab/LCh lightness l=L*/100. Each consumer receives its own static `TensorDescription` using the v4/v5 codec and explicitly selects required immutable resources. Results carry structural schemas, samples and storage owners; color, alpha, transfer, units and sampling descriptions remain caller-owned configuration. RGB/RGBA is never inferred from three or four raw components.

Numeric CRV descriptions identify one logical tuple at `channel_axis=0`. A downstream FMT description instead uses the actual tensor's channel axis, such as 2 for `[H,W,C]`. This authoring adaptation preserves coordinate meaning and introduces no sample conversion.

A composite member inherits its constituent operations' static checks, numerical caller preconditions, operational failures and public rounding stages. In particular, GEN-06C follows CRV-06's unchecked numerical premises, including output-tuple representability. Independent generation primitives also use unchecked numerical premises; the color description is not sample-validity certification.

Constant images copy valid samples in models supported by complete descriptions; color ramps define RGB, XYZ, CMYK, Lab, LCh, OKLab, OKLCh, HSL and YCbCr separately and follow the CRV/FMT mathematics. GEN-07 uses generic numeric components; NOI-09B requires linear RGB.

## Geometry object, error and topology

Original-geometry operations and explicitly named approximation operations are separate. Exact polygon area concerns the chosen polygon. A flatten-based member computes on its deterministic published polyline; correctly rounding that area is not correctly rounding the source curve's area. Ellipse area, circular stroke boundaries and true-curve distance have their own algorithms.

Flatten accepts epsilon_geom_px > 0 in target-canvas pixel units. Constructors write 0.05 by default; an explicit preview preset writes 0.25. The numeric value is persisted and is part of operation specialization identity. These are engineering defaults, not measured optimal settings. The bound includes flattening and coordinate-publication error. Subsequent transforms propagate that bound or compute the required new bound as part of their mathematics. The bound is never silently loosened.

Default source_topology_policy is allow_change. reject_unproven requires proof of the topology properties declared by the member. Distance error alone proves neither coverage error nor topology. Unverified is reported when no proof or check was performed; an actual change is reported only if established. An unproved strict request fails. Fixed algorithm, version and parameters also fix the approximation bits.

## Structured results, attributes and width

The fixed member directories in [PTH geometry](PTH_geometry_contract.md) define paths, arc-length tables, samples, partitions, linear width, source maps, exact/grid polygons and minimal reports. The public generation SDK provides schema factories for these (see that contract). Counts, references and association payloads are unchecked; explicit workflow values retain any source owners required by later mathematics. No generic attribute dictionary or expression graph exists.

Default attribute_policy=reject_unmappable. Explicit drop_unmappable drops only unmappable attributes and reports their keys and reasons. Attributes with defined correct mappings are propagated; labels and IDs are never interpolated. A failure never publishes a successful result with silently missing attributes.

Width denotes finite nonnegative full stroke width (diameter), in geometry pixel units. Each invocation explicitly selects either a separate path_width Result paired with its PathSet by the workflow, or an independent width-evaluation input. The v1 standard key is width; the separate schema stores Linear profiles and source bindings. PTH-05C preserves the original path identity and returns that path together with the new width Result. Independent PCHIP evaluation is a separate member; geometry Results carry no implicit width attribute.

Three parameter domains are distinguished (field spelling is a schema requirement): pathset_normalized_arclength, subpath_normalized_arclength and subpath_arclength_px. The first concatenates subpath lengths in declared order, excluding spatial jumps; the second restarts [0,1] on each subpath; the third restarts physical distance on each subpath. The domain is stored with the binding. A generic ArcLength label cannot substitute for these distinct domains.

Trim/dash preserves source-position width by default. The source mapping is carried or rebuilt; reapplying the full width function to each output fragment requires an explicit rebind. An unrepresentable mapping follows the attribute failure policy, not approximate interpolation. This preserves width, not every raster pixel: new endpoints can introduce new caps. Zero length, closed seams, domain bounds and exact mapping representation follow the member rules and the bounded rational source-map representation.

## Periodic coordinates and two-circle roots

Repeat uses the exact mathematical coordinate t-floor(t) in [0,1), then normal NUM rounding. Exact period boundaries output +0. A value inside the period that rounds to 1 remains 1, with no nextDown adjustment or second wrap; the following lookup interprets it as the endpoint. Angular periodic coordinates follow this rounding policy; pad/reflect keep their separately defined endpoints. Periodic query mapping does not normalize stored color hue or remove winding. CRV/FMT original hue interpolation remains authoritative.

For two circles define q=p-c0, d=c1-c0, s=r1-r0 and solve A*t^2+B*t+C=0 with A=d.d-s^2, B=-2*(q.d+r0*s), C=q.q-r0^2. Classification is exact, without epsilon snapping. Roots with r0+t*s >= 0 are kept; the largest valid real root is selected before spread, without clamping to [0,1]. A=0 uses the linear equation when applicable.

Identical circles are invalid input. For other circles, if A=B=C=0, the radius constraint decides: s<0 gives maximum t=-r0/s; s>=0 with nonnegative input radii has no finite maximum. The finite maximum is returned if present. Absence of roots or of a finite maximum means no returnable coordinate.

Default no_solution=valid_zero returns t=+0 and UInt8 valid=0; a returnable coordinate has valid=1. Explicit reject fails when a valid query has no returnable coordinate. valid is geometric coordinate validity, not alpha. Resource, cancellation, arithmetic and unfinished-certification errors are preserved and never become valid=0.

## Generic mesh fields

Rectangular bilinear, triangle mesh and regular-parameter bicubic patch members exist; arbitrary curved geometry inversion and folded patches are excluded. Interpolation is generic numeric: an explicit component axis is accepted and numeric fields are produced, without inferring color or alpha semantics. No special alpha interpolation is provided.

Bilinear uses four corner vectors and the full weighted expression. Triangle mesh uses exact containment/barycentric decisions and independent geometric validity. Bicubic uses the 4x4 tensor-product cubic Bernstein control vectors on the regular parameter domain, not an unspecified cubic filter. Color interpretation, conversions and alpha weighting belong to explicit workflows; their individual public-node rounding stages are part of the workflow contract and are not one final rounding of a fused expression.

## Random identity

Random generation uses Philox4x64-10 with direct, lossless address encoding into its 256-bit counter. Global x/y are signed32. frame, draw and stream each range 0..2^32-1, carried by Int64 parameters. channel is 16-bit and the built-in domain is 8-bit; seed denotes a 64-bit bit pattern. Address ranges are caller premises; out-of-range requests have no defined validation or aliasing semantics.

The [random contract](NOI_random_contract.md) fixes production v1 counter words c0=(x,y), c1=(frame,draw), c2=(stream,channel,domain), c3=0, with fields packed from low to high bits and all 72 reserved bits zero. It also fixes the key `(seed_bits,0)`, Float32 halfopen24 from the high 24 bits and Float64 halfopen53 from the high 53 bits of output word 0, and open52 from the high 52 bits of a selected word. Box-Muller uses word 0 for open52 and word 1 for halfopen53 within one block. The random contract fixes member word selections and addresses, including candidate ordinals and Bridson attempts. General integer sampling uses 64-bit word 0 with low-tail modulo rejection and nonwrapping draw increments; each consumer specifies its draw allocation and finite attempt budget.

Address injectivity is not uniqueness of individual output samples or a proof of statistical independence across keys. Seeds are never derived implicitly from node identity, clock, pointers or scheduling. The integer core agrees across profiles; floating transforms follow their declared profiles. Core known-answer tests validate Philox4x64-10 independently of application addressing; addressed references use the specified v1 packing and floating mapping.

## Point distributions and completion

Finite-candidate rejection and FIFO-active Bridson are separate members. Finite rejection examines candidates in ordinal order and accepts only published points satisfying the exact squared minimum-distance predicate. FIFO always processes the earliest remaining active point; success retains it and appends the new point, while exhausting its fixed attempts removes it. IDs follow acceptance order. Spatial indexes and parallel candidate evaluation must preserve these decisions.

max_count is a normal stop target: reaching it succeeds; exhausting candidates or the active list first succeeds with fewer points. stop_reason and points_object_id are reported using the fixed GEN-08 report schema. Candidate count, attempt count and max_count are algorithm inputs in operation specialization identity. Neither completion nor count satisfaction proves maximal packing or a fixed density. Exactly N points is not promised.

Hard iteration and capacity limits are separate from the normal stop; exhausting them fails without successful partial publication and is not evidence that no additional point fits.

## Rank resources

Generic rank lookup semantics come first. Only static structure and layout are checked; ranks, permutation, addressing and immutable content identity are unchecked caller premises. A permutation or rank fixture proves neither spatial blue-noise quality nor temporal STBN quality. Named blue/STBN variants additionally require a specific resource or generator with recorded provenance, license and byte SHA256, and accepted spatial/temporal spectrum and threshold-set quality reports. Resource conversion produces a new identity and requires requalification.

## Noise model boundaries

Electron Poisson shot, integer-looks multiplicative speckle and linear-RGB artistic grain are separate models. Shot consumes nonnegative expected electrons and returns Int64 counts; it adds no implicit dark current, read noise, gain, black level, full well or ADC. The finite random grid is not an exact continuous ideal distribution. Speckle has a separate explicit multiplier formula; signed raw inputs do not acquire physical-intensity meaning.

Grain forms correlated Gaussian g, then RN_T(C+strength*g). It exposes explicit normalization=none/sum/l2; strength is only the final multiplier, not a guaranteed standard deviation. sum requires a nonzero sum of weights; l2 requires a nonzero sum of squares. Boundary aliases refer to the same source variable, not independent samples. Normalization, kernel, boundary, strength and RNG parameters are persisted.

Monochrome/shared and independent-RGB addressing choices exist. The same RGB formula applies even at alpha=0, modifying hidden RGB; alpha bits are copied unchanged. Finite signed/HDR results are not clipped. Other working domains or a calibrated camera pipeline require separate explicit members or workflows.

## Geometry variants, publication and fitting

Exact-input and explicit-grid Boolean members are defined separately. Exact members interpret input floats as exact dyadic geometry. Grid members specify quantization, scale, arithmetic and observable behavior, and report quantization effects. No automatic fallback between them, snapping or small-feature removal is permitted for exact members. The source-curve approximation permission of the geometry approximation contract does not authorize additional unreported Boolean quantization.

Separate members provide the exact Result output and the Float64 PathSet output. Exact polygon results retain rational coordinates and topology for exact consumers. Float64 output performs deterministic rounding. Required displacement bounds, incidence, topology and finite representability after rounding are unchecked caller preconditions. The reported displacement bound is accumulated while constructing coordinates; the rounded geometry is not scanned and publication is not rejected for a numerical or geometric postcondition. Conversion reports topology_status=0 unless topology analysis is explicitly part of the selected operation. An explicit exact-to-Float64 conversion has matching publication rules. Chained Float64 outputs operate on the newly rounded geometry, not on an implicitly preserved exact history. Grid-to-Float64 conversion uses the same unchecked representability and publication premises. Rational polygon support does not cover exact offset or circular constructions, which may need a different number domain.

Different fitting solvers are different public operators. They share quality acceptance: endpoint/locked/corner constraints and a continuous bidirectional Hausdorff bound including publication error. Sample residuals are not a continuous certificate. Each solver operator/version/profile fixes initialization, solving, splitting, tie handling, stopping and returned bits. Distinct solvers may produce different valid controls and counts. There is no silent solver substitution and no minimum-segment optimality promise.

Fitting defaults to allow_change topology policy, with explicit reject_unproven. The topology properties are specified per member; distance success does not prove them. Unverified topology is reported as such, and strict unproved results fail. Resource exhaustion cannot publish an uncertified best guess.
