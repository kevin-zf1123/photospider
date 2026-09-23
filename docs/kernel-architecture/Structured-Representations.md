# Structured representation contracts

The installed `photospider/data/representation.hpp` provides eight named,
version-1 schema families. Typed `*_schema` factories validate static inputs;
matching `*_spec` functions decode immutable metadata. Their complete contracts
enter `SchemaTemplate` identity. All use CompleteBundle: the structured
coordinator validates associated, sealed fields through bounded read windows
before notifying observers, inserting a shared result, or serving downstream
consumers. A malformed imported object cannot become an empty success.

Schemas remain fixed while row counts are discovered. Field records can span
many physical pages. Empty logical collections have zero rows and no Value
with a fabricated zero extent. Validation debits root work and explicit I/O;
`maximum_result_window_bytes` must fit at least one record. The optional
validation work hook adds an admission condition and cannot replace root work.
Capacity, lifetime and exclusions follow [Global results](Global-Results.md).
These are managed-capacity guarantees, with no process RSS certificate.

## Spectrum

`SpectrumSpec` retains original shape, transformed axes, stored axis order,
packing/packed axis, sign, normalization, cyclic shifts, sampling origin/step,
unit and real-input policy. `samples` is Float64 complex pairs in the declared
slow-to-fast axis order. Domain uses the original shape, including odd/even
lengths that have the same packed width. The default transform convention is
negative unscaled forward and inverse divided by the transformed size; other
explicit normalization/sign modes have different schema identities.

R2CHalf stores `0..floor(N/2)` on one transformed axis and requires zero shifts.
Its omitted half is defined by conjugation. Stored mirror pairs, boundary
columns and self-conjugate points are checked; an entire Nyquist column is not
required to be real. ExactHermitian compares finite real/imaginary components
directly, including very large finite conjugate pairs. It does not compute a
potentially overflowing complex magnitude merely to multiply it by zero.
RealProjectionMeasured uses complex magnitude and named absolute/relative
max-pair tolerance. Pure absolute tolerance is compared in original units, preserving small
differences next to large equal components. Positive relative tolerance compares normal binary64 fractions with separate
exponents for the defect and tolerance, preserving small differences and
avoiding overflow; this is a binary64 acceptance mode, not a certified
transform error bound. Inverse implementations must explicitly report their
real projection and imaginary residual. The FFT workflow supplies its own
independent numerical reference.

## Bands

Version 1 names duplicate-last Haar: analysis gives `(a+b)/2,(a-b)/2`, synthesis
uses low plus/minus high and crops to the recorded parent shape. This is the
implemented filter/boundary contract, with no general filter-bank claim.
`BandsSpec` preserves original shape, ordered axes, filter snapshot, levels and
explicit member descriptors. Each member includes shape, parent shape, origin,
step, phase and ExplicitZero. The required Mallat keys are final `(L,0)` plus
all nonzero axis masks at levels 1..L. Missing, duplicate and ExplicitZero are
distinct. At level zero, the sole member is the original approximation.

The `samples` field concatenates present members in declared order;
`band_range(result,level,mask)` resolves their logical intervals independently
of page boundaries. An ExplicitZero member has a logical shape but no stored
coefficient interval. The five-sample public example reconstructs
`[1,3,5,7,9]` from low `[2,6,9]` and high `[-1,-1,0]`, then crops the sixth sample.

## PathSet

Coordinates are exactly two finite binary64 components. Schema selects one
geometry authority. CoreVerbs uses M/L/Q/C/Z with arities 1/1/2/3/0, exact
control offsets, subpath boundaries and explicit closure. A nonempty subpath
starts with M; Z is terminal and agrees with `closed`. Empty PathSet has empty
verbs/controls/subpaths and both offset arrays `[0]`. A single M represents a
zero-length open subpath.

Primitive authority uses explicit `PrimitiveRef` records; the core verb array
is empty and its control offsets are `[0]`. Each reference identifies one
payload record, field and the owning ObjectId. The association slot preserves
the unsigned ObjectId bit pattern in an Int64-width record. Fields are:

| Index | Field | Record |
| --- | --- | --- |
| 0..4 | verbs, control_offsets, controls, subpath_offsets, closed | Core arrays; controls are Float64 pairs |
| 5 | primitives | tag, record index, payload field, association |
| 6 | bezier | Four control pairs; unused Line/Quadratic padding is zero |
| 7 | arcs | center, axis u, axis v, start angle, sweep or full-turn direction |
| 8 | hermite | P0, P1, d0, d1; derivatives use normalized t in [0,1] |
| 9 | splines | degree, first control, control count, first knot, first weight (-1 means nonrational) |
| 10..11 | knots, weights | Scalar binary64 arrays |
| 12 | attributes | owner domain, interpolation, components, count, value offset, parameter offset |
| 13..14 | attribute_values, arc_positions | Scalar values and normalized arc-length samples |

Bezier uses its usual Bernstein polynomial. Hermite's equivalent cubic
controls are P0, P0+d0/3, P1-d1/3, P1. ArcSweep is `c+u cos(theta)+v sin(theta)`
with a nonzero sweep strictly inside one turn. Nonorthogonal axes are allowed;
a bounded outward determinant interval must prove nondegeneracy. FullTurn has
an explicit signed direction and its endpoint references its start. Zero,
degenerate and multiple-turn inputs require an explicit conversion; they are
not silently replaced with a line. No sin(2*pi) equality establishes closure.

Nonperiodic B-splines require sufficient controls, nondecreasing knots,
`knots[degree] < knots[control_count]`, and positive rational weights. Interior
multiplicity degree+1 requires splitting subpaths. Zero denominator terms use
zero, and the right domain endpoint uses the left limit. Degree-zero constant
pieces are allowed. Periodic seam validation is outside this named schema.
Joined primitive endpoints must agree exactly. Arcs and unclamped splines with
unresolved endpoint equality are supported as single open primitives; an
explicit FullTurn may be closed. Mixed subpaths that would require an
unproved endpoint snap fail. This limitation does not imply a topology,
boolean-path or antialiasing error guarantee.

Attribute domains are Path/Subpath/Segment/Control/ArcLength (1..5), with
Constant/Linear interpolation (1..2). Core Segment indices refer to verb
records; primitive Segment indices refer to PrimitiveRefs. Value spans are
complete and nonoverlapping. ArcLength parameters strictly span [0,1]; changing
total path length may affect every normalized attribute sample.

## Dynamic points and components

Points store `ids`, finite `positions`, finite `attributes`, and sorted
`id_to_row` pairs. Rows may be physically reordered, but the explicit mapping
must be a complete bijection. InputPosition IDs lie within the fixed input
basis; Ordinal IDs are 0..count-1. A semantic maximum count is distinct from
physical growth/resource limits. Count zero is valid.

Components store dense `labels` plus dynamic rows `(id,area,min_position)`.
MinPixel uses `1+min_position`; CompactMinOrder uses 1..K in minimum-position
order. Validation checks exact areas, ordering, complete label/table membership
and basis. It does not prove connectivity of arbitrary imported labels.
The labels/area/filter workflow separately proves its four-connected union
recipe against an independent BFS reference. The current representation
validator uses charged scans and binary searches; it may fail a work budget
rather than allocate an unbounded resident index.

## Planar YCbCr

This retained version-1 data schema records the former converter's fixed
representation. Package 0.20.0 removes `color.rgb_to_ycbcr420`; the schema is not
an implementation of an I/O codec. FMT-16 is retired; internal image planes
remain same-size and planar. This legacy schema does not authorize heterogeneous
planes as a canonical kernel image.
Its historical fields are:
BT.709 transfer/matrix, sRGB D65 primaries, preserved scene/display reference,
full-range Float32 Y in [0,1] and signed Cb/Cr in [-.5,.5]. Plane roles and
reconstruction/box-clipped boundary mode are fixed by the schema version.
Chroma shape is ceil(H/2) by ceil(W/2). Nominal center (.5,.5), step (2,2), and
actual clipped source support remain distinct. For 3x5, the last nominal center
is (2.5,4.5), but its actual source support is the single pixel (2,4).

## Brush and iterative states

Brush `state_ids` contains stroke, next event, finalized event prefix, finalized
dab count, initial/current canvas versions, generation, seed, counter and end.
`state` contains last position, distance to next dab, P[3], A, E[3]. Pending IDs
and positions have matching bounded counts; end requires no pending events.
Finalized dabs must be ordered and agree with positive spacing/carried phase.
The binary64 consistency tolerance is 32 epsilon times the largest involved
position/spacing plus 32 smallest subnormal units. It is a validation mode,
not a CertifiedBound for rendering. Carry uses coverage alpha in [0,1], zero
premultiplied P when alpha is zero, and separate signed finite emission.

`advance_causal_brush` computes the named one-dimensional constant-spacing fold
with all cross-batch phase retained. It returns newly emitted root-owned dabs
and a new state, leaving the input untouched on failure. Dab vectors use the
Payload allocator role, which remains subject to the payload sublimit when
copied; growth admits simultaneous old and new blocks. Positive spacing that
cannot advance in binary64 fails; fuel/count limits never silently truncate.
Lookahead/pending state is representable, while the helper explicitly selects
the causal mode. It does not implement smoothing or smudge.

Iterative fields are `(generation,iteration_count,stop_reason)`, estimate,
diagonal, rhs and measured infinity-norm residual. Stop reasons are Converged=0
and IterationLimit=1. System snapshot, initialization, size, iteration cap and
convergence policy enter the schema. Zero initialization requires x0=0.
The validator recomputes `max(abs(b - rounded(a*x)))` with a distinct product
rounding and checks the declared stop policy. Approximate consumption must be
explicit. Finite measured residual, including zero, carries no certified error
bound. Each published ResultRef freezes its estimate and associated data.

## Public example and checks

```sh
cmake --build build/issue257-shared --target photospider_representations_workflow test_representations -j 8
ctest --test-dir build/issue257-shared -R '^(photospider_representations_workflow|test_representations)$' --output-on-failure
cmake --install build/issue257-shared --prefix out/phase-a-delivery/install
cmake -S examples/representations_workflow -B out/phase-a-delivery/representations-consumer -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/representations-consumer -j 8
out/phase-a-delivery/representations-consumer/photospider_representations_workflow
```

The source fixture is an in-memory numeric test wire, not a persistent file
format: a little-endian 16-count header followed by host primitive field bytes.
A staged producer discovers counts and copies only bounded pages into mandatory
backing. A real downstream Result consumer counts descriptor rows; the example
then compares every retained field against independently prepared fixture
bytes after context destruction. It also checks odd Haar reconstruction and
brush batch partitions against explicit reference values. Optional first CLI
argument selects one printed fixture name for counterexample reproduction.

The `iteration-resume-generation` fixture executes an approximate zero initial
Result through a paged diagonal step into a new converged Result and another
active consumer. It checks generation 1→2, iteration 0→1, exact solution [3,4],
unchanged retained x0, and ancestry retention after context destruction.

Numerical schema/content validation and the causal brush helper establish the
default nearest/gradual-underflow environment, then restore the caller's floating state. Failure to establish it is an operational failure.
