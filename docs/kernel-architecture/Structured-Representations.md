# Structured representation contracts

[Chinese reader version](zh/Structured-Representations.zh.md).

## 1. Scope and ownership

`representation.hpp` defines eight closed schema families for structured Results: Spectrum, Bands, PathSet, Points, Components, YCbCr420, Brush, and Iterative. Typed `*_schema` functions validate static identity; matching `*_spec` functions decode that identity. The schema facet participates in Result identity and is independent of physical page geometry.

The coordinator owns validation and publication. Recognized structured Results use CompleteBundle: it validates associated sealed fields through bounded windows before notifying observers, caching the Result, or serving consumers. A malformed object cannot appear as a successful empty collection. Dynamic row counts do not change the schema and do not create a fabricated zero-extent Value.

## 2. Data layout and memory

```cpp
Result<SchemaTemplate> spectrum_schema(const SpectrumSpec&);
Result<SchemaTemplate> bands_schema(const BandsSpec&);
Result<SchemaTemplate> path_set_schema(const PathSetSpec&);
Result<SchemaTemplate> point_set_schema(const PointSetSpec&);
Result<SchemaTemplate> components_schema(const ComponentsSpec&);
Result<SchemaTemplate> ycbcr420_schema(const YCbCr420Spec&);
Result<SchemaTemplate> brush_schema(const BrushSpec&);
Result<SchemaTemplate> iterative_schema(const IterativeSpec&);
Status validate_representation(const ResultRef&, const ResourceBudget&,
                               std::uint64_t maximum_window,
                               const CancellationToken& = {},
                               const std::function<Status(std::uint64_t)>& = {});
```

Each schema factory checks enum values, dimensions, counts, and arithmetic before data I/O. The schema stores its family contract as immutable metadata. The Result fields remain separate from that metadata, so a logical shape or coordinate declaration does not imply physical contiguity.

YCbCr420 validates Y in `[0,1]` and Cb/Cr in `[-0.5,0.5]`. Its default primaries, transfer, and reference strings are `srgb-d65`, `bt709`, and `display`. Chroma uses ceil-divided half-resolution extents with nominal origin `(0.5,0.5)` and step `(2,2)`; clipped source support remains distinct from that nominal coordinate. The schema does not perform conversion or file I/O.

| Family | Data and identity |
| --- | --- |
| Spectrum | Float64 real/imaginary pairs; original shape, transformed axes/order, shifts, packing, sign, normalization, sample origin/step/unit, real policy and tolerances |
| Bands | Concatenated coefficient fields plus keyed members; each member records shape, parent shape, origin, step, phase and explicit-zero state |
| PathSet | Exactly one geometry authority; verbs or primitive references, geometry payloads, subpath offsets, and optional attributes |
| Points | IDs, finite positions/attributes, and sorted `id_to_row` pairs |
| Components | Dense labels plus `(id,area,min_position)` table |
| YCbCr420 | Fixed BT.709/full-range schema identity and associated Y/Cb/Cr fields |
| Brush | State identity, carry state, pending events/positions, and finalized event/dab prefix |
| Iterative | Generation/iteration/stop reason, estimate, diagonal, right-hand side and measured residual |

Spectrum's packed axis stores indices `0..floor(N/2)` for that axis; original dimensions preserve odd/even identity. Packed spectra require zero shifts. The default convention uses a negative-sign unscaled forward transform and divides the inverse by the product of transformed axis lengths; explicit normalization modes produce different schema identities. Bands v1 names duplicate-last Haar: `low=(a+b)/2`, `high=(a-b)/2`; synthesis adds/subtracts and crops to the recorded parent shape. Required Mallat members are the final `(L,0)` and every nonzero axis mask at levels 1 through L. Missing, duplicate, and ExplicitZero members have distinct meanings. An ExplicitZero member has a logical shape but no coefficient interval; `band_range` returns `NotFound` for a missing key.

Path coordinates contain exactly two finite binary64 values. Core verbs are M/L/Q/C/Z with control arities 1/1/2/3/0. A nonempty subpath starts with M; Z is terminal and agrees with the `closed` field. An empty path has empty geometry and offsets `[0]`; a single M is a zero-length open subpath. Primitive authority uses `PrimitiveRef(tag,record_index,payload_member,association)`, where association preserves the unsigned ObjectId bits in an Int64-width slot. Attribute domains are Path, Subpath, Segment, Control, and normalized ArcLength; interpolation is Constant or Linear. Attribute value spans must be complete and nonoverlapping; normalized arc-length parameters strictly cover `[0,1]`.

| Field | Stored records |
| --- | --- |
| 0 | Path verbs |
| 1 | Control offsets for each verb |
| 2 | Float64 control coordinate pairs |
| 3 | Subpath offsets into verbs |
| 4 | Closed flag for each subpath |
| 5 | Primitive tag, record index, payload field, and owning ObjectId association |
| 6 | Four Bezier control pairs; unused Line/Quadratic padding is zero |
| 7 | Arc center, axes u/v, start angle, and sweep or full-turn direction |
| 8 | Hermite P0/P1/d0/d1; derivatives use normalized t |
| 9 | Spline degree, first control/count, first knot, and first weight (`-1` for nonrational) |
| 10-11 | Scalar Float64 knots and weights |
| 12 | Attribute owner, interpolation, component width/count, value offset, parameter offset |
| 13-14 | Attribute values and normalized arc-length positions |

Primitive authority requires an empty core-verb array and control offsets `[0]`. Primitive Segment attributes index PrimitiveRefs; Core Segment attributes index verb records. A single open Arc or an unclamped spline with unresolved endpoint equality remains representable. A mixed subpath that requires an unproved endpoint snap fails.

Points use a complete ID-to-row bijection even when records are physically reordered. InputPosition IDs stay inside the fixed input basis; Ordinal IDs are `0..count-1`. `maximum_count` expresses semantic limits and does not replace physical resource admission.

## 3. Execution and state machine

```text
schema factory -> immutable family identity
                         |
producer -> sealed associated fields -> coordinator bounded validation
                                             |                 |
                                        all checks pass       failure
                                             |                 |
                                  observer/cache/consumer     publish nothing
```

The coordinator validates recognized schemas before publication. Validation reads bounded windows, checks cancellation and root work/I/O, and may also use the optional additional work hook. The window must hold at least one record; otherwise validation fails with a resource error. A loaded read window owns the backing it needs after its plan, Result, or execution context is destroyed. Dynamic empty collections retain descriptor support without inventing a data row.

Brush state carries the finalized event prefix and dab count between calls. `advance_causal_brush` consumes one ordered batch and returns a new state plus newly emitted dabs. End is irreversible. Failed work or resource admission leaves the prior state usable and publishes no partial advance. Iterative Results freeze the estimate and its associated system data at each published ResultRef.

## 4. Algorithms and math

For a transformed spectrum axis of length N packed as R2CHalf, stored indices are `0..floor(N/2)` and the omitted values are defined by conjugate reflection. Original dimensions remain in the schema to distinguish odd and even sizes. ExactHermitian checks finite real and imaginary fields directly. RealProjectionMeasured compares stored value `a` with the conjugate of its stored mirror `m` and accepts defect `d` under the named tolerance:

$$
|d| \le atol + rtol\max(|a|,|m|),
$$

where `d=a-conj(m)`. The absolute comparison preserves small differences next to large equal values; the relative path uses exponent-separated binary64 fractions to avoid overflow. This acceptance rule is not a certified FFT error bound. A Nyquist column need not be entirely real; only points self-conjugate on all transformed axes must be real.

Bands v1 implements duplicate-last Haar: `low=(a+b)/2`, `high=(a-b)/2`; synthesis uses low plus/minus high and crops to the recorded parent shape. For an odd five-sample input `[1,3,5,7,9]`, low `[2,6,9]` and high `[-1,-1,0]` reconstruct the original after cropping the duplicated sixth sample. Required membership is one final `(L,0)` member plus every nonzero mask for levels 1 through L in declared axis order. ExplicitZero is a present logical member with no stored coefficients, not a missing key.

Path Bezier uses its Bernstein polynomial. Hermite `(P0,P1,d0,d1)` maps to cubic controls `(P0,P0+d0/3,P1-d1/3,P1)`; derivatives use normalized `t in [0,1]`. Arc geometry is `c+u cos(theta)+v sin(theta)`. Sweep arcs have a nonzero sweep strictly shorter than one turn; full turns store signed direction and close at their start. Nonorthogonal axes are valid only if an outward determinant interval proves nondegeneracy. Zero, degenerate, and multiple-turn inputs require explicit conversion; `sin(2*pi)` does not prove closure.

Nonperiodic B-splines require enough controls, nondecreasing knots, `knots[degree] < knots[control_count]`, and positive rational weights. Interior knot multiplicity degree+1 requires splitting subpaths; degree-zero constant spans are valid. Zero denominator terms evaluate to zero, and the active right endpoint uses the left limit. A single open arc and an unclamped spline with unresolved endpoint equality are representable. Mixed primitive paths that require an unproved endpoint snap fail. Path attributes have complete nonoverlapping value spans; normalized ArcLength parameters strictly cover `[0,1]`. Core Segment attributes index verb records; primitive Segment attributes index PrimitiveRefs.

Component IDs are either `1+min_position` or compact integers ordered by minimum position. Table area/minimum and label membership are checked exactly. The four-connected labels operation visits left/top edges and constructs the generated partition; structural validation of imported labels does not establish connectivity.

Brush identity fields record stroke, next event, finalized event prefix, finalized dab count, initial/current canvas versions, generation, seed/counter, and end state. Carry contains last position, remaining distance to the next dab, premultiplied `P[3]`, coverage `A`, and independent emission `E[3]`. Positions must be finite and nondecreasing; emission is finite and signed; `A` lies in `[0,1]`, and `A=0` requires `P=0`. Pending event IDs and positions have matching bounded counts; an ended state has none. State consistency compares finalized dabs and carried spacing within `32*epsilon*max(|a|,|b|,spacing)+32*denorm_min`, where `a,b` are the compared positions. This tolerance validates state, not rendered pixels. The helper performs the one-dimensional constant-spacing fold and carries phase across batches; it does not smooth or smudge. Dab vectors use the Payload allocator role. Copies remain subject to the Payload sublimit, and growth admits old and new blocks while both are live. Positive spacing that cannot advance in binary64 fails without changing the previous state.

Iterative identity includes system snapshot, initialization, size, iteration cap, and convergence policy. Stop reasons are `Converged=0` and `IterationLimit=1`. Zero initialization requires `x0=0`. The validator checks the declared stop policy and recomputes the measured infinity residual with a separately rounded product:

$$
r_\infty = \max_i |b_i - \operatorname{round}_{64}(a_i x_i)|.
$$

A finite residual, including zero, does not certify solution error. Approximate consumption must be explicit. The multiplication is rounded separately before subtracting from `b_i`. Schema/content validation and brush stepping establish nearest rounding with gradual underflow for their work and restore the caller's floating-point environment.

## 5. Limitations and non-goals

- YCbCr420 records full-range Y in `[0,1]` and Cb/Cr in `[-0.5,0.5]`, BT.709 transfer, sRGB D65 primaries, and a declared scene/display reference. Chroma has ceil-divided half-resolution extents, nominal origin `(0.5,0.5)` and step `(2,2)`; clipped source support is separate. The schema is not an I/O codec. Internal image planes follow the same-size planar contract.
- PathSet does not establish topology, boolean path correctness, or antialiasing error from geometric tolerance. Periodic spline seam validation is outside this schema.
- Component structural validation does not prove connectivity of arbitrary imported labels.
- Brush state can represent bounded pending/lookahead events, while the public helper computes only causal constant-spacing behavior; it does not implement smoothing or smudge.
- An iterative measured residual is not a CertifiedBound.
- Managed work, I/O, window, and capacity limits do not bound process RSS.
