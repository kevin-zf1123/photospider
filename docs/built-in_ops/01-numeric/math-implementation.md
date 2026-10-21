# Numeric mathematical implementation

This document records the implemented numerical machinery for numeric and curve
operators and their shared facilities. Specification acceptance remains independent: the
operator specifications retain their Proposed status. Numerical correctness is
the selected discrete mathematical result, including raw special-value rules;
it is not a claim that every input succeeds under every resource budget.

Current measurements and coverage limits are in the
[accelerated specification](op_specs/NUM_accelerated_contract.md#current-implementation).

## Fixed execution and shared numerical helpers

NUM-04/05 lower selected output tiles to fixed ABI 8 CPU invocations. Prepare validates declared views and binds immutable kernel state; the runtime receives input handles and preallocated output/scratch. Both operands remain fixed dependencies for binary and rational-pi functions. Sparse requests compute selected tiles; empty selections emit no tasks. See [fixed mathematical execution](point-math-execution.md).

The numerical helpers below are also used by remaining continuation-based operator definitions. Those definitions are unavailable through the fixed executor until migrated. Their internal arithmetic descriptions do not establish runtime support. The [compiler and execution guide](../../kernel-architecture/Compiler-and-Execution.md) defines current Plan and invocation contracts.

## Exact elementary paths

`exact_elementary.hpp` performs integer transforms in signed 128-bit scratch,
checking the final UInt8/Int64 range before publication. Float sign, NaN and
integral rounding operations use unsigned IEEE fields. Addition/subtraction,
multiplication, division and sqrt use correctly rounded hardware arithmetic
in a saved/restored nearest-even floating environment after bit-level special
classification. The existing 4352-bit dyadic/ratio and squared-midpoint paths
remain fallbacks when that environment cannot be established. No Float32 NaN passes through a
Float64 conversion. Simple floating operations and named algebraic paths are
bitwise identical across the three profiles.

The current unary registration covers 18 ordinary functions and four exact
Int64 rational pi functions, each with three CPU suffixes. Generic binary
arithmetic and power/angle routines in these internal helpers are shared
facilities; their operator registration/acceptance is recorded separately in
[the implementation table](implementation.md).

## Directed interval representation and ownership

`directed_interval.hpp` represents signed endpoints as integers in units of
`2^-p`. Precision follows the fixed ladder 128, 256, 512, 1024, 2048, 4096.
Each integer has 192 uint64 limbs (12288 magnitude bits); the 128-slot temporary
pool and rounding workspace are inline members of the caller-owned arithmetic object. Fixed NUM-04/05 tasks include this object in the preallocated `PointMathScratch` aggregate.
Nested frames reuse slots and restore the previous cursor, including exception
paths. No limb operation uses an external allocator or persistent mutable cache.

Directed addition is exact within checked capacity. Multiplication and division
retain the remainder and round endpoints outward. For a negative quotient,
floor increments the magnitude when the remainder is nonzero; ceiling does not.
Four endpoint combinations enclose products and quotients. Division rejects a
denominator interval containing zero as an unresolved mathematical refinement,
not as permission to guess a numeric result. A work/capacity/cancellation Status
is sticky and never converted into a refinement retry.

Every primitive invokes the supplied work callback at bounded checkpoints. Fixed NUM-04/05 invocations use that callback to query cancellation without dynamic sample-work charging. Multiword division
and multiplication have checks inside their loops. Actual temporary widths,
source/result owners and region metadata remain accounted. Borrowed work
callbacks exist only during the synchronous evaluation; the binding guard
clears them before returning or unwinding.

## Constants and mathematical enclosures

`math_constants.hpp` contains generated lower integer bounds for pi and ln(2)
at 4096 fractional bits. The adjacent integer is the upper bound. The generator
uses exact Python Fraction arithmetic: Machin's formula with alternating atan
series for pi, and the positive atanh(1/3) series with geometric remainder for
ln(2). It emits a constant only after both rational bounds have the same scaled
integer floor. Recheck it with:

```sh
python3 -m oracle.ops.numeric.common.generate_constants --check
```

`directed_functions.hpp` propagates both arithmetic and analytic errors:

- exp reduces the argument by `2^14`, sums its Taylor series for `|r|<=1/16`,
  bounds the tail by twice the first omitted term, then performs 14 interval
  squarings. An entire input interval at or above 1024 certifies final infinity;
  one at or below -1024 certifies final zero. These are final IEEE range results,
  not fabricated real enclosures used in later arithmetic.
- ln first normalizes the exact IEEE significand/exponent to `m*2^e`,
  `1<=m<2`. It evaluates `2*atanh((m-1)/(m+1))+e*ln(2)` with a geometric tail.
  Normalization occurs before conversion to fixed precision, preserving tiny
  subnormals without an artificial zero-domain error.
- Radian sine/cosine certify a nearest multiple of pi/2 and retain the complete
  pi interval in the reduced remainder. An uncertain quotient triggers
  refinement. Alternating Taylor series on a certified remainder within
  `[-1,1]` use the first omitted term as a bound. Tangent divides the resulting
  enclosures; it is not a rounded sine divided by a rounded cosine.
- Pi functions reduce the exact binary or Int64 rational multiplier before
  multiplying the remainder by the pi enclosure. Integer/half/quarter and
  selected rational-third/sixth landmarks use exact bit or algebraic-root paths.
  Full original magnitude remains in a sincpi denominator.
- Sinc uses its whole even series near zero; elsewhere it divides enclosures of
  the mathematical sine and full argument. This avoids intermediate rounding
  and tiny-denominator precision loss near the removable singularity.
- Arctangent uses its alternating series with magnitude at most one half,
  using the pi/4 transformation for ratios above one half. Atan2 normalizes both
  operands by the larger exact binary exponent before forming the bounded ratio.
  Normalized angles divide the complete enclosure by exact pi's enclosure.
- Power uses `exp(b*ln(abs(a)))` enclosures after its ordered special table. A
  separate exact dyadic path detects midpoint powers, including noninteger
  exponents such as `81^8.5`. A possible binary32/64 midpoint has at most 25/54
  odd significand bits; perfect-power and exponent-divisibility checks bound
  this path without constructing enormous powers.

The pi error is not reset after argument reduction: a roughly 1024-bit quotient
can magnify a `2^-4096` pi enclosure to a remainder width around `2^-3072`.
That actual width continues through all subsequent operations. Operation-specific
capacity bounds include logarithmic power products around `2p+1035` bits,
exponential squaring around `2p+1478` bits, and full pi arguments around
`2p+1026` bits. Checked 12288-bit capacity covers these paths at p<=4096.

An ordinary result is published only when both enclosing endpoints round to the
same destination bits. Exact zeros, poles and midpoint powers are handled before
this test; one-sided underflow enclosures preserve the known nonzero real sign.
If 4096-bit refinement cannot certify rounding, the operator returns
`ResourceExhausted/CapacityLimit`. The implementation does not claim a proof of
successful resolution for all possible Float64/rational inputs.

## CPU profiles and diagnostics

The private SLEEF 3.9.0 binary64 u10 kernels, compiled from the
[builder-supplied source](../../../third_party/SLEEF.md), provide explicit
AdvSIMD and AVX2/FMA implementations for ordinary binary64 exp, ln, sin, cos, tan, pow and
atan2. Float64 inputs are never narrowed. `accelerated_math.hpp` admits bounded
ordinary argument ranges, expands kernel results to conservative binary64
intervals and checks the final output against the shared FP32 budget. Domain,
classification, zero, FP32 subnormal range and wider results use strict fallback.
Pi/rational-pi kernels reduce the original dyadic/Int64 ratio with exact integer
quadrants before conversion, then enclose small-angle SLEEF sin/cos. Sinc uses the
same complete numerator/denominator enclosure, retaining the full original
argument; ordinary sinc admits |x|<=1. Exact landmarks precede approximation.
Atan2pi divides the enclosed atan2 by an enclosed pi. Uncertain poles, tiny or
out-of-range final values retain directed strict evaluation. Fast-math remains disabled. The private adapter isolates explicit FMA
use, ISA dispatch and hidden symbols; static/shared consumers need no SLEEF target.

NUM-01 evaluates up to four selected samples together in preallocated node-by-lane scratch. Coordinates use global indices within the selected tile. Outward intervals cover each strict RN64 AST step; uncertain final bounds trigger strict replay. This preserves the final error contract while numerical domains remain caller preconditions. Cancellation is queried during AST traversal and arithmetic refinement. Fixed invocation scratch includes the scalar, SIMD and certified-math state.

DependencyPath diagnostics identify the numerical implementation, compiler/OS/build identity and selected profile. The build identity includes the pinned SLEEF sources and integration module, preventing old/new accelerated cache identity reuse. In that path, evaluations are admitted before arithmetic and copied values are counted before their fixed publication store. Those counts and an attempted fallback survive later resource or cancellation failure. Cache hits add no fabricated arithmetic work. Fixed NUM-11/13 invocations do not use this accounting model.

NUM-04/05 use fixed tile invocations with per-value numerical diagnostics unavailable. Their arithmetic and numerical refinement rules remain those described above; see [fixed mathematical execution](point-math-execution.md).

NUM-11 uses fixed reduction tasks with private preallocated exact state. Only selected output groups execute, and count has a schema-only dependency with dense output tiles. Per-value NumericDiagnostics are unavailable; exact-arithmetic callbacks query cancellation. See [fixed reduction execution](reductions-execution.md).

NUM-13 uses fixed CPU scan tasks grouped by non-scan output regions. Each task reads the bounding source prefix for its declared positive writes; zero-only groups need no input payload or arithmetic scratch. Exact carry is saved before selected output conversion. Private scratch and separate dense output tiles are preallocated. Integer representability applies to every retained task write as an unchecked caller precondition. Per-value NumericDiagnostics and sample work-limit counters are unavailable. See [fixed scan execution](scans-execution.md).

## Independent reference and execution

The kernel has no MPFR, GMP, Boost or libbf dependency. The manual Python oracle
uses MPFR 4.2+ in a separate process and refines downward/upward results until
both round to the same output bits. Whole-formula sinc and exact rational pi references
use explicit directed compositions; special values and NaN payloads are modeled
separately. `PHOTOSPIDER_ORACLE_MPFR` can select an explicit compatible library.
The oracle requires LP64 macOS/Linux and prints the actual loaded MPFR version;
its architecture must match the Python interpreter. Apple Silicon Homebrew
Python and `/opt/homebrew/opt/mpfr` use the native arm64 library.

This separation is deliberate. MPFR's custom interface still permits internal
temporary allocations, and GMP custom allocation hooks are process-wide and
cannot report ordinary recoverable allocation failure. The pinned libbf candidate
also documents unresolved transcendental error proofs and incomplete allocation
failure propagation. Neither library is silently substituted for the host's
memory/work/cancellation contract. Sources:
[MPFR custom interface](https://www.mpfr.org/mpfr-current/mpfr.html#Custom-Interface),
[GMP custom allocation](https://gmplib.org/manual/Custom-Allocation),
[libbf limitations](https://bellard.org/libbf/readme.txt).

Performance
observations must identify the native hardware, shape, dtype, worker/cache
settings and actual measured execution scope.

For fixed NUM-04/05 execution, temporary public checks should cover changed functions, signed input strides, caller rounding modes and preserved flags, exact declared tile reads, sparse/empty selections, concurrent Runs, raw NaN bits, retained output lifetime, malformed signatures and resource/cancellation cleanup. Integer representability and positive rational denominators are caller preconditions. Per-value fallback counters and cache behavior are outside this fixed invocation contract.

## NUM-01 expression and preparation

The three `numeric.sample_expression_*` keys use the same bounded immutable
postorder program. Parsing is iterative: 4096 ASCII source bytes, 256 nodes,
height 32 and bytewise canonical free names. Decimal literals use exact integer
conversion, including digits beyond binary64 precision; constants pi/e use
fixed correctly rounded binary64 bits. In the strict evaluator, every primitive rounds to binary64 in
specified left-to-right postorder. Final Float32 conversion is a separate
rounding boundary. NUM-02 exact endpoint interpolation supplies coordinates;
NUM-04/05 exact elementary and certified interval mathematics supply strict
primitives. Accelerated evaluation uses the final reference-enclosure check
described under CPU profiles and replays rejected samples through strict.
No compiler reassociation, host libm or floating environment defines the result.

Decimal scratch uses 256 limbs (16384 bits): a 4096-digit significand needs
at most 13607 bits, and the supported decimal-exponent branch has at most
14680 denominator bits. Larger exponents are classified as overflow or rounded
zero only after accounting for all source digits. Division alignment and final
midpoint comparison fit the remaining capacity. Runtime exact/math workspace
is part of preallocated invocation scratch; cancellation checks occur in AST evaluation and inside long arithmetic/refinement. Bounded unresolved
transcendental refinement returns ResourceExhausted.

Pure static preparation owns the program across compiler stages, outputs and
dynamic runs. Sealed handles validate registry/definition identity, all static
input metadata and exact parameter bits, including signed zeros and NaNs.
Direct preflight prepares once per call, or reuses an explicit matching handle;
Fixed kernel preparation retains and reuses the sealed plan owner. Static plan/program
allocations use ordinary host storage outside runtime managed scratch. They
have source/program size bounds, but no separately enforced preparation budget
or global preparation cache. The synchronous callback borrows the AST while
holding its preparation owner. Current package, traits, C ABI and compiler identity
versions are listed in the [version contract](../../development/Compiler-Version-Contract.md).

Each selected values tile has one fixed CPU task and a separate dense allocation. Axis is one atomic task using smaller coordinate state without the expression arena. Active scalar reads are explicit; count one excludes end and axis excludes coefficients while retaining their schemas. Values computes only selected coordinates. Finite inputs/intermediates/results, valid function domains and sampling precision are unchecked caller preconditions. Selected tasks share their IR call's publication result. Per-value diagnostic counters and content caches are unavailable. See [fixed expression execution](expression-execution.md).

The diagnostic `strict_math_calls` counts actual dispatched mathematical calls,
including exact special paths and failed calls. Per-function fallback counters
attribute accelerated strict fallbacks; rejected pre-dispatch domains add no
call. Legacy reporters without per-function attribution merge into `Other`.
All numeric diagnostic merging is checked for overflow before publication.

`NumericDiagnostics::evaluated_values` counts admitted numeric evaluations, including attempts that later fail. Most operations count one attempt per output value. Fixed NUM-11/13 invocations do not report these diagnostics. `OperationTiming::computed_elements` counts output elements separately. Fixed NUM-01 invocations do not report per-primitive mathematical calls. Fixed Bezier function invocations do not report per-value root-solver calls. Operations without call-level instrumentation leave `strict_math_calls` at zero.

## CRV-01 exact interpolation

Four primitives (linear/PCHIP, single/multiple functions) provide twelve profile
keys. Strict and Float64 output evaluation use exact integer rational formulas
followed by one direct RN-even conversion. Accelerated Float32 outputs first try
hardware interval evaluation of the complete linear/PCHIP formula. Both endpoints
must round to the same Float32 result, as well as satisfy the shared quality gate.
This stronger condition preserves cross-query monotonicity when mixed with strict
fallback. A mere 4 ULP32 bound is insufficient: neighboring binary64 queries can
otherwise produce a one-ULP64 reversal. Selected knots/clamps remain exact.
Unresolved cases use strict fallback; no requested-batch repair is applied.
Exact cross products detect a collinear complete local stencil and reduce its
PCHIP formula to the existing linear rational formula. Rounded slope equality
is insufficient and is never used for this decision.

Finite binary64 values are integers in units 2^-1074. Differences need fewer
than 2099 bits; PCHIP slope numerator/denominator magnitudes are below 2^6300.
The combined Hermite numerator is below 2^20999 and denominator below 2^18897.
The 352-limb (22528-bit) workspace covers these bounds plus denominator
alignment, 53 quotient trials and midpoint doubling. The fixed 96-slot arena
uses at most 49 live slots in the formula; every slot and ratio temporary is
part of each invocation's preallocated Host scratch. Endpoint limits compare
exact cross-products.
Direct node/clamp conversion, two-point linear degeneration and exterior
tangent evaluation have smaller bounds. Arithmetic polls work/cancellation
inside limb multiplication and final division. Independent scoped arithmetic
review and independent Fraction references validate the formulas separately from runtime execution.

Each selected output tile lowers to one fixed CPU Host task. For output rows `[r,r+R)` and, in the multi form, columns `[c,c+C)`, its immutable reads cover all `x[0:K]`, `query[r:r+R]` and `y[0:K,c:c+C]`. The scalar form reads all y. Each region is closed through its input's own tile grid, so batch, spatial or atomic axes can enlarge physical tile support. These reads are the static superset required for data-dependent knot search; the numerical stencil remains local.

The task evaluates only its selected global rows and columns. It searches x directly through fixed views, once per query row within that task, and reuses the classification across the task's columns. Query order and repetitions are unrestricted. Empty selection emits no payload task. Output tiles have separate dense allocations. `select_outputs` retains the closure of saved tasks; fresh compilation determines narrower task regions.

Finite x with strictly increasing values, finite consumed query/y values, representable final outputs and queries within the knot interval under `reject` are unchecked caller preconditions. Preparation validates schemas, parameters, canonical output views, the exact input tile set and fixed scratch capacity. The kernel performs no global numeric validation scan. Clamp, endpoint selection, extrapolation and exact rounding retain their mathematical definitions.

Each invocation constructs one `ExactCurve` in preallocated Host scratch of `sizeof(ExactCurve)`, containing the complete 96-slot integer arena and ratio workspace. Binary search uses bounded stack indices, and no knot vector or full slope table is allocated. The 352 limbs per integer are an arithmetic precision bound, not the total scratch byte count. Scratch is private to the invocation; prepared views are immutable and shared by concurrent Runs.

Search costs `O(R log K)` x reads per task plus `R*C` local evaluations (C=1 for scalar). Input tile owners, selected output allocations and granted-lane scratch require resource admission. Cancellation is polled during search, column traversal and exact arithmetic. Internal capacity/zero-denominator guards and accelerated quality certification remain active. Static errors retain `InvalidArgument` or `TypeMismatch`; unavailable profiles, resource exhaustion and cancellation retain their operational status. Violating numerical caller preconditions has no specified numeric diagnostic.

Tasks share their IR call's publication outcome. Successful task tiles can supply dependent tasks internally; public call Results appear only after all required tasks succeed and ordered retirement occurs. Rebinding reuses immutable preparation and produces independent Run-owned output. Content caching and repeated-demand identity are not provided. Result owners retain allocations beyond context lifetime.

Temporary public acceptance compares selected outputs with the independent Fraction reference in `oracle/ops/numeric/crv01/reference.py`. Cover mixed dtypes, exact node/clamp bits, PCHIP shape/zero rules, sparse rows and columns, static read sets, signed/unaligned input views, concurrent Runs, cancellation, resource admission and owner lifetime. Preserve the oracle and remove temporary runtime checkers after validation.

## CRV-02 exact Bezier function sampling

`bezier_function.cpp` registers three fixed CPU profiles. Each selected values tile reads complete controls plus active endpoints and searches anchor x directly, reconstructing only its selected segment. Axis has a scalar-only atomic task. Count one omits end from both outputs; its axis task copies [start,start,+0] with zero scratch. Static schemas are checked for all four original ports. Selected outputs share one call's ordered publication result.

`ExactSampling` retains the exact weighted grid and RN64 addition/conversion. `ExactPolynomial` stores signed integer coefficients in units 2^-1075. `ExactBezier` solves the unique interior Bx root by dyadic bisection; interval Horner encloses By, while integer pseudo-remainder GCD and exact low-degree root tests decide shared zeros and rounding midpoints. Nonzero half-subnormal boundaries supply their sign when a tie rounds to zero. Direct dyadic roots and constant ordinates use exact evaluation. Exact interior zero is +0, while anchor/clamp selection preserves the selected y zero sign.

Values scratch contains ExactSampling/ExactBezier state per invocation, independent of K/N; axis uses smaller coordinate state. There is no full x topology cache or complete values allocation. The exact arena has 640 64-bit limbs per number and 96 slots. Initial coefficients have fewer than 2105 bits; the worst degree-three integer remainder chain stays below 25280 bits. Horner at the 8192 fractional-bit cap stays below 26683 bits, with a maximum live-slot bound of 44 including GCD work. Capacity/refinement guards remain operational failures. All profiles use this exact mathematical path with profile-specific integer helpers.

Global x topology, finite consumed controls/reconstructions, selected reject domain and output representability are caller preconditions for values. Active endpoints are finite and distinct for count>1. Values require selected-coordinate neighbor separation; only selected axis requires representable nonzero step. Axis-only imposes no control requirements. Physical producer dependencies remain complete for required controls even if numerical selection uses only one anchor. FloatingEnvironment preserves caller state, cancellation is checked in value/search/arithmetic work, and Runs own independent scratch/output.

Use independent Fraction de Casteljau and rational Euclid/Sturm references over actual selected tile cells. Temporary checkers remove the reference runner's whole-output expansion/index projection and hardcoded maximum-count resource failure. Check sparse maximum count, count-one/axis projection, numerical midpoint and zero cases, source layouts, fenv, concurrent rebinding, malformed preparation, resource admission, cancellation and retained owners. Public Bezier baking must also match explicit graph expansion and compose with the fixed LUT consumer. See the [CRV-02 contract](op_specs/CRV-02_sample_bezier_function.md#fixed-execution-and-dependency-projection).

## CRV-03 parametric Bezier evaluation

`parametric_bezier.cpp` registers three fixed CPU keys for explicit local t evaluation. Each selected [rows,columns] output tile reads segment/t for those rows, all K anchor rows and all K-1 handle rows for those columns, closed through each source grid. A task classifies each row once and reuses it across its columns. Raw segment indices are checked against K-1 before addressing; negative Int64 encodings fail the same unsigned bound.

RN64 control reconstruction and exact Bernstein/power-Horner evaluation retain their separate rounding boundaries. Endpoints convert one anchor, preserving zero sign. Interior exact zero is negative only when all reconstructed controls are -0; nonzero underflow keeps its mathematical sign. All profiles use the same exact calculation with profile-specific integer helpers. The polynomial's trimmed degree determines its dyadic denominator, preserving constant and reduced-degree curves. For valid degree<=3 and t in [0,1], the numerator bound is 5329 bits.

Each invocation owns preallocated `ExactPolynomial` and `ExactSampling` scratch independent of K/N/D. Output allocations cover selected tiles, and the kernel keeps only one row plus four controls. Physical input metadata still scales with the declared closure. Prepared views support signed/zero strides and unaligned offsets. DirectTiles retains matching views; DenseHost imports have explicit COPY storage.

Finite t in [0,1], finite consumed controls and RN64 reconstructions, and finite representable outputs are unchecked caller preconditions. Mathematically unused controls have no additional finite scan, but all declared source tiles remain producer dependencies. Handles retain physical dependencies even for endpoint queries. FloatingEnvironment preserves the caller state; cancellation is checked per row/component and inside exact arithmetic. Internal capacity guards remain. Concurrent Runs share immutable preparation and own their scratch/output; required tasks share one call's ordered publication result.

Temporary acceptance uses the retained Fraction Bernstein oracle over actual selected tile cells, with full-output expansion disabled in the temporary checker. Check arithmetic bits independently from source-grid closure, malformed preparation, sparse large shapes, fenv, valid segment boundary selection, resource admission, cancellation, concurrent rebinding and retained output lifetime. See the [CRV-03 contract](op_specs/CRV-03_evaluate_bezier.md#fixed-execution-and-physical-demand).

## CRV-04 public LUT1D authoring

`numeric/lut1d.hpp` exposes six authoring functions that append ordinary runtime nodes to a caller-owned `WorkflowDocument`: expression and Bezier samplers each produce `values` and `axis`, while linear and PCHIP interpolation templates first produce Float64 linspace queries and then interpolation values. `BakedLut1d` returns node-output references; `outputs()` creates caller-named workflow exports. Construction creates graph metadata only. It reserves existing and referenced node IDs, allocates the lowest available positive IDs, enforces the 65,536-node limit and appends nodes only after the complete expansion succeeds. A failed construction leaves the graph unchanged. The caller owns output declarations and compilation.

The source operations use Result inputs and outputs. `SequenceInput` pairs a workflow edge with an immutable Result schema hint for a Float32/Float64 `[1]` endpoint; the compiler checks the actual binding independently. A temporary public checker can keep arrays as local `Tensor` backing, publish their
storage as immutable source Results, and bind them through `ExecutionBindings`.
Inspect the returned Results through authorized tensor windows.

The expression template executes through fixed NUM-01 tasks: each selected values tile is computed independently, and axis has its own atomic task. Sparse requests admit only selected values storage plus fixed scratch. Axis-only skips coefficients; count one excludes end payload; larger counts retain both endpoints. Finite/grid/function-domain rules are caller preconditions inherited from NUM-01. Selected tasks share their call's publication result. Interpolation templates execute fixed linspace and curve tasks. Sparse table selection propagates the required query tiles; axis-only selection excludes curve tasks and their x/y input payloads. Bezier expansions use fixed values tiles and a separate scalar-only axis task. Axis-only excludes controls; count one excludes end. Joint selection shares the sampler call's publication outcome.

The template and its explicit node expansion should produce identical schemas, read dependencies and numerical results for the same bindings. Validate that equivalence separately from Fraction/MPFR numerical references. Rebinding reuses immutable static preparation and creates new Run-owned outputs. Read Results through their tensor API; returned owners retain allocations after the context is destroyed. There is no content-cache identity guarantee in the fixed expression path.

Temporary acceptance covers Float32/Float64, values-only/axis-only/joint/empty selection, nonzero sparse tiles, count one with a failing end producer, mixed scalar dtypes, resource admission, cancellation and escaped output ownership. Check only executable source families; authoring metadata alone does not establish runtime delivery. Use the retained numerical references in `oracle/ops/numeric/` for the expanded sampler.

## CRV-05 dynamic-axis LUT1D

`lut1d_application.cpp` registers six fixed CPU keys for scalar/per-channel tables. Each selected output tile reads its input region, all L table rows in the selected column range and the two axis endpoints, closed through the individual source grids. Prepared point readers preserve affine offsets and signed/zero strides. Each channel classifies its query independently.

For L>1, bounded binary search reconstructs each needed RN64 endpoint-weighted knot through `ExactSampling`. Exact knot/clamp uses one table entry; interpolation/extrapolation uses its adjacent pair. Descending segments swap both x/y before `ExactCurve` evaluates the positive-denominator formula. Each invocation has preallocated `ExactSampling` plus `ExactCurve` scratch, independent of L. Selected outputs occupy only requested tiles; grid and full table/output arrays are not materialized by this kernel.

L=1 uses direct conversion, zero scratch and table-only payload support. Input and axis keep static schema checks but have no payload reads; their producers may be omitted. The singleton axis and reject equality still define valid calls. For every L, numerical axis validity, finite consumed data, reject domain and output representability are caller preconditions. Step consistency has no dedicated payload read. A complete source tile may nevertheless include step, or additional table columns; upstream producers must meet their own requirements for the physical closure.

FloatingEnvironment preserves the caller state and establishes RN/gradual underflow. Cancellation is checked in output iteration, search and exact arithmetic. Exact capacity/divisor guards remain operational errors. Concurrent Runs share only immutable prepared state. Required task writes participate in one call's ordered publication; output owners survive context teardown. Empty reads no payload and compatible source rebinding reuses preparation.

Use the retained Fraction oracle for valid numerical inputs, and temporary public workflow checks for tile support, sparse allocation, mixed dtypes, batch/atomic axes, signed/zero/unaligned layouts, concurrency, fenv, malformed preparation, resource rejection and cancellation. All six CRV-04 templates execute through this consumer, subject to their individual numerical preconditions. The complete numerical and demand contract is [CRV-05](op_specs/CRV-05_apply_lut1d.md).

## CRV-06 explicit color tuples and color ramps

The 15 independent primitives provide 45 fixed CPU keys using invocation ABI 8. Each selected output tile reads the corresponding query region and conservative complete stops/colors/p/q closures on each input's grid. The kernel searches source stops directly, reads one or two selected complete rows, and writes dense preallocated complete-color output. Numerical validity is a caller precondition; unused generic rows remain numerically unused. Output shape appends one atomic component axis, without preserving input batch grouping or attaching a color description. Empty emits no task. Only selected output tiles are allocated, with fixed arithmetic scratch per invocation.

`ExactColorCoordinate` stores source binary64 integers in units `B=2^-1074`,
with 144 uint64 limbs (9216 bits), 24 slots and a separate directed interval
workspace. For adjacent stops, `U=s1-x`, `V=x-s0`, `H=U+V` in integer units.
Floating coordinates evaluate `(U*c0+V*c1)/H * B`; rational hues evaluate
`(U*p0*q1+V*p1*q0)/(H*q0*q1)`. Original signed Int64 numerators and positive
denominators remain exact, including INT64_MIN. Floating numerators need fewer
than 4200 bits and rational hue numerators fewer than 2230. Pi conversion
multiplies or divides the whole ratio enclosure by certified pi. Equal units
cancel symbolically; direct stored zeros retain their signs and genuine mixed
exact zeros are +0. Polar hues retain every turn, including achromatic inputs.

`ExactRgb` uses the 40960-bit/96-slot polynomial integer arena for direct
dtype conversion, straight linear transfer and gamma=2. The latter rounds a
signed square root of the complete rational expression using squared-midpoint
comparisons. Intermediate premultiplied values and alpha are never rounded.
The ordinary gamma=2 bounds, including midpoint alignment, fit below 13000
bits. Gamma=3..8 has a separate exact weighted-power cancellation proof below
28600 bits; this avoids an interval refinement that could never distinguish
an exact zero. Other unrecognized exact cancellations can exhaust refinement.

General gamma uses positive-domain homogeneity: normalize both straight encoded
magnitudes by their maximum `m`, compute the signed alpha-weighted mixture `R`
of the normalized powers, and encode as `m*sign(R)*abs(R)^(1/gamma)`. Normalized
power bases and `abs(R)` are at most one. Gamma and its reciprocal retain their
exact real meanings. The final magnitude `m` is split into a unit-scale interval and an exact binary
exponent. That exponent is passed
to final IEEE rounding, preserving subnormal/HDR results without requiring the
whole output magnitude to fit the interval's absolute fractional scale.

The separate `DirectedColorFunctions` supplies wide positive logarithms and real
exponential enclosures. It never substitutes the NUM exp adapter's final-IEEE
overflow/zero result for an intermediate real. For `x<=-p`, `[0,2^-p]` encloses
exp(x); other admitted inputs reduce by `2^17`, use a Taylor tail bound, and
perform 17 directed squarings. The exponential's positive argument limit
preserves room for raw products in the 12288-bit interval arena. Every bounded
real approximation and the 128..4096 precision ladder remains host-accounted.

sRGB uses the specification's exact rational constants and exponents. Decode
branches compare exact input ratios; encode branches enclose both sides when
the interval crosses the standard threshold. The encoder has a small downward
jump, so a global monotonic endpoint assumption would be invalid. Direct and
complete-stored-row-identical cases bypass transfer, while additional exact
inverse simplifications require proof of the relevant branch. Finite hidden
straight RGB at zero alpha has zero contribution; nonfinite source components
obey complete selected-color caller preconditions. Representable output association requires zero rounded RGB when alpha rounds to zero; valid transparent black is canonical +0.

All profiles currently return strict bits within their declared contract. Scalar/NEON/AVX2 integer primitives serve their respective CPU profiles; there is no approximate transfer backend. ExactRgb and ExactColorCoordinate own checked interval state and borrowed synchronous work callbacks. They return terminal cancellation/resource failures before any precision reset and refine only uncertain expressions. Candidate output is accepted only while arithmetic remains active. Scratch destruction and floating-environment restoration happen at synchronous return, including failure.

TensorDescription model/white/primary/transfer/hue configuration belongs to
the operations SDK. Numeric tuples reuse exact FMT geometry calculations. Static description structure is checked; numerical/geometric validity is an unchecked caller premise.
The logical tuple axis is zero; sampled arrays retain a final component axis.
CIELAB/CIELCh use normalized l=L*/100 and RGB uses straight storage. Transfer
kind, gamma bits and alpha presence are resolved once during preparation;
obsolete premultiplied input/output branches and their extra exact ratios are
removed. Exact alpha weighting, direct-path zero signs and final rounding remain. CMYK
profile wrappers import immutable bytes with content identity; operations select
required identities explicitly into their preparation/plan resources. CMYK uses the once-per-preparation resource hook to check frozen ICC structure and the optional Profile ID without a full-profile copy; Empty and artifact import also pass this hook. Results
do not retain a profile because of a color description. Generic ResourceSet
checks duplicate identities for byte conflicts and retains admitted owners.

Acceptance should cover strict numerical references, mixed dtype, model domains,
straight RGB alpha, hue units, resources, arbitrary layouts, active cancellation,
sparse output allocation, direct-call scratch bounds and zero allocation during invocation.
The retained accelerated allowance is contractual; bit matching in these cases
does not replace that contract.

## CRV-07 joint three-dimensional LUT application

`lut3d_application.cpp` registers independent trilinear and tetrahedral primitives with three CPU identities each. Static preparation checks shapes, dtypes, extents and color descriptions. Lowering emits one fixed CPU task for each selected complete-color output tile, with reads for its query region, the complete table and six axis endpoints. Each input uses its own tile grid. The caller guarantees finite consistent axis data and strict reconstructed ordering; the kernel reconstructs knots into fixed scratch and locates cells by binary search. An interior exact hit starts at that stored knot, and the terminal hit uses the final cell.

`ExactLut3d` keeps each local coordinate as `n_i/h_i` with positive `h_i`.
Descending cells negate numerator and denominator together without changing
the vertex's stored index. Every weight uses common denominator `D=h0*h1*h2`.
Trilinear numerators multiply the chosen `n_i` or `h_i-n_i`. Tetrahedral orders
the exact fractions descending, preserving axis order on ties, then subtracts
the scaled `S_i=n_i*h_j*h_k` to obtain `D-Sa`, `Sa-Sb`, `Sb-Sc`, `Sc`.
Only positive weights enter the current vertex list used in table mathematics.
Rebuilding weights from immutable cells/queries avoids a per-output point cache.

The complete component numerator is `sum(W_v*C_v)`, where each finite source
float is an integer in `2^-1074` units. One ratio rounding produces the output
dtype. Differences need fewer than 2100 bits, denominator/weights fewer than
6300, and weighted sums fewer than 8400; final rounding alignment stays below
8500. The existing 40960-bit polynomial arena admits these bounds. The actual
peak is at most 48 of 96 slots, including descending-axis normalization and
per-component accumulation. Original contributing bits determine all-negative-
zero results; other exact cancellations are +0 and nonzero underflow retains
its sign. No floating local coordinate or intermediate blend is rounded.

The six keys use invocation ABI 8 with immutable preparation and mutable scratch owned by each invocation. The scratch contains 3*256 knot slots, ExactSampling, ExactLut3d and current cell/query/colors. It is independent of the selected output count. ExactPolynomial stores a borrowing WorkConsumer adapter by value for each synchronous arithmetic phase, clears it at phase end, and preserves cancellation before checking arena capacity. Arithmetic capacity diagnostics use fixed empty text.

Each task writes its preallocated dense output tile. Output uses the complete input sample shape, one atomic trailing component axis and no attached description or input batch partition. Empty requests emit no tasks. Source layouts may use unaligned addresses and negative/zero strides; the host checks address bounds before invocation. Result ownership survives context retirement.

Original query/model legality, axis consistency/order, contributing table colors, reject-domain membership and finite output are caller preconditions. Clamp changes coordinates after those original-color premises. The kernel reads only exact positive-weight vertices, although the conservative table closure retains dependencies on all table producers. Operational cancellation and capacity failures stop the invocation without publishing successful call output.

All profiles use exact integer arithmetic and the existing scalar/NEON/AVX2 integer helpers. The independent `oracle/ops/numeric/crv07/reference.py` reconstructs RN64 grids, cells and simplexes, then rounds rational sums using separate integer IEEE logic. Temporary public and direct acceptance checks should cover valid fixtures, sparse complete-color tiles, mixed source grids, concurrent Runs, source strides, floating environment, scratch/output boundaries, cancellation, allocation-free invocation and retained output lifetime. No physical x86 or GPU execution is implied by registration.

## CRV-08 whole-formula shapers

The public linear pair expands fixed whole-rational remap and dense constant output tiles. The inverse retains `remap(lower,lower,upper,lower,lower)` before expanding target_lower; it preserves lower bits under the unchecked finite ordered-bound precondition and does not enforce order at runtime. Float32 zero/one literals use fixed count-one linspace; both floating dtype compositions use fixed operations. Graph construction performs no payload computation. The shared bounded ID allocator is also used by LUT1D baking.

The six logarithmic keys use fixed CPU tasks for selected output tiles.

`ExactShaper` evaluates the logarithm ratio or `L*(U/L)^t` as one mathematical
expression. Endpoints and IEEE extensions use raw bits. Equal odd significands
reduce the forward ratio to exact integer exponent differences. For the inverse,
write `L=l*2^e`, `U/L=(a/b)*2^d` with coprime odd `a,b`, and `t=m*2^s`.
Exact integer square roots handle negative `s`; denominator factors must divide
`l` before exponentiation. The resulting dyadic odd core is retained through one
IEEE rounding, including actual midpoints and half-minsubnormal ties. A core
larger than the destination midpoint precision need not be forced into this
branch; certified intervals handle it. Pure powers classify very large positive
or negative total exponents without overflow in intermediate arithmetic.

Remaining cases use independently rounded log enclosures at fraction precisions
128,256,...,4096. Forward subtracts enclosed logarithms and divides enclosed
complete differences. Inverse first encloses `z=ln(L)+t*(ln(U)-ln(L))`; only this
complete `z` is range-classified. It then encloses `z-k*ln(2)` near zero,
exponentiates that residual and carries `2^k` to final rounding. This keeps
subnormal answers representable in scratch and prevents an overflowing rounded
`U/L` from changing semantics. Both enclosure endpoints must round to identical
output bits. Unresolved precision is `ResourceExhausted/CapacityLimit`; work and
cancellation failures are terminal, not refinement retries.

All returned finite numeric paths correctly round the same monotone function.
Thus their combination is monotone without sorting output requests or choosing
a path from batch values. Accelerated keys retain the strict certified path;
Per-value fallback/evaluation counters are unavailable. Existing
scalar/NEON/AVX2 arithmetic identities remain; there is no new Accelerate/SME
backend or application of NUM-14's finite four-term certificate.

Each selected output tile reads its matching query region and both scalar bounds, closed through the independent source grids. The invocation owns preallocated ExactShaper scratch, borrows prepared affine views and writes only selected global output cells. Finite 0<L<U is a caller precondition; a zero-significand guard protects ctz/clz and integer division. All IEEE input and successful overflow/underflow semantics remain explicit numerical behavior.

The shaper uses the checked control-flow instantiation of DirectedIntervalStorage/DirectedFunctionsStorage. Retry, range outcomes and first terminal failure live in fixed fields; failed pool allocations return private failure-propagation slots and never index beyond the arena. ExactShaper checks those outcomes before accepting bits or retrying precision. Cancellation, normal range results and unresolved refinement avoid C++ exception allocation. Other math consumers retain the default error protocol; the underlying interval arithmetic is shared.

Runs share immutable preparation and own arithmetic/output storage. Empty has no payload task, and compatible source replacement reuses preparation. Call publication waits for every required task and ordered retirement; retained output owners survive context teardown. Linear templates keep their fixed constant/remap composition and selected intermediates. No content-cache identity or per-value diagnostic counters are promised.

Validate against independent Fraction/directed-MPFR whole-expression references, retaining special inputs and successful IEEE overflow. Check near-equal/extreme bounds, exact roots/midpoints, quieted NaN payloads, signed zero, monotonic groups, selected tile/scalar closure, affine layouts, fenv, concurrent rebinding, malformed preparation, sparse huge shapes and resource/cancellation behavior. Direct probes check normal range, retry/capacity and cancellation without exception allocation and confirm the default interval instantiation's behavior. See [CRV-08](op_specs/CRV-08_shaper.md).

## CRV-09 measured LUT3D baking

`bake_lut3d` stages an ordinary `WorkflowDocument`, invokes the authoring-only
source builder for grid colors `[N0,N1,N2,3]` and validation colors `[P,3]`, then
checks both expansions with Compiler metadata analysis. The helper protects
existing declarations, Result schemas, nodes and exports; a failed builder
leaves the document unchanged. It retains no builder or capture. A SHA-256
recipe identity includes the expanded source, designated endpoints and compiler
semantic identity. Both expansions execute under the same immutable Result bindings,
including shared dynamic inputs and optional `ResourceSet`. Pointwise
independence is the caller's assertion.

The fifteen geometry profile keys use fixed CPU tasks. Axis copies selected cells with zero scratch. Grid and center tasks read the six axis endpoints through their source grid and use one preallocated `ExactSampling` object. Knots are RN64 endpoint-weighted coordinates; centers are RN64 averages of neighboring actual knots. No complete knot vector or complete output is retained by an invocation.

`points_extra` closes only selected extra rows; axis payload is read only when selected rows include centers. Extra-only tasks use zero scratch. Color copies same-dtype bits or converts each finite component once under RN, with zero scratch. Other geometry outputs use one atomic trailing component axis; axis uses ordinary axes. Each task allocates only its selected output tile, preserves global coordinates and borrows signed/unaligned source views. Empty emits no task. Finite ordered consistent axes, model-valid colors, in-range extras and finite converted values are caller preconditions.

Preparation checks complete signatures and exact scratch. Concurrent Runs share immutable state and own separate arithmetic/output allocations. Invocation cancellation returns before publication, and escaped Results retain their storage owners. The independent Fraction `geometry_reference` checks endpoint bits, grid reconstruction, two-stage center rounding and dtype conversion. Temporary public/direct checks additionally cover source closure, large sparse grids, layouts, floating environment, cancellation and zero invocation heap allocation.

Pack, Measure, Unpack and Gate use fixed CPU invocations. Pack/Unpack/Gate copy selected samples into independent dense tiles. Measure emits one task that waits for the complete owned table, scans every point/reference/applied color and writes the complete report. It exports the report on retirement; later Gate failure leaves an independently retained observer result valid. The public template connects ordinary source-grid/reference/application nodes with an independent axis and a global quality gate. The report v4 has thirteen tensor members and 361 payload bytes. Its `measured_table_id` is the actual owned table Result ObjectId, encoded little-endian; equal bytes with another ObjectId cannot satisfy that binding.

The report algorithm compares component error and `atol+rtol*abs(reference)` as exact integers in `2^-2148` units. Error needs fewer than 3173 bits and the threshold product fewer than 4197, within 4352-bit arithmetic. Exact comparisons retain the earliest ordinal on ties. Displayed maxima round upward, with diagnostic +Inf allowed. Centers precede extras in row-major order. Measurement threshold failure produces `passed=false`; the separate gate reports `LutApproximationToleranceExceeded`. Exact arithmetic and report storage use fixed scratch. Gate writes bounded diagnostics directly, checks configuration and ObjectId before tolerance, and copies table samples only on success. Legal finite samples and well-formed report values are caller premises.

`configuration_id` binds shape, method, tolerances, dtypes, both complete
canonical TensorDescription byte strings, extra count and source recipe under
`photospider.bake-configuration.v2`. Owned tables and reports are schema v4;
old v3 inputs are rejected. Native Lab/LCh axes, tables and validation points
use l=L*/100. A scalar atol must be reauthored for the three native coordinate
units; blindly dividing it by 100 also changes opponent/chroma acceptance. The explicit SDK reader checks this configuration and the report fields. Measured quality applies only at listed points under the selected interpolation method and table dtype, never to the whole continuous domain.

## CRV-10 inverse curves

`ExactCurve::inverse` reuses the original exact PCHIP derivatives and Hermite
formula. A linear inverse forms its entire signed rational expression and rounds
once. For PCHIP, x/y/query are integers in units of `2^-1075`, so every binary64
midpoint, including half a minimum subnormal, is exact. Slope denominators are
positive after sign normalization. At candidate coordinate X, compare
`N(X)-query*D(X)` exactly, reversing the sign for decreasing y. Coordinates outside
the selected segment are classified by the segment boundary before evaluating
the polynomial. Strict monotonicity makes that comparison determine root order.

The ordered destination lattice contains all finite output values plus two
infinity sentinels. Binary search never evaluates the sentinels as polynomial
coordinates. An exact root returns directly; otherwise adjacent lattice values
bracket the root, and one exact midpoint comparison chooses nearest/ties-to-even.
The finite/overflow midpoint is MAX plus half its last-binade ULP, with symmetric
negative handling. Canonical exact zero is +0; a negative nonzero root rounded to
zero retains its sign. Selected knots/clamps convert the original x bits directly.
An unreturned endpoint may exceed the destination range without causing failure.

In inverse units let B=2100 bound differences. Slope numerators/denominators have
less than 6303 bits, Hermite numerator less than 21009, and the comparison less
than 21010. Segment-exterior short circuits preserve this bound. The existing
22528-bit-per-integer, 96-slot invocation-private arena fits the arithmetic with fewer than
64 live slots. Every refinement and limb multiplication polls cancellation;
failed arithmetic status is checked before interpreting a comparison as zero.
The extracted shared Hermite helper retains forward interpolation's original
1074-unit defaults and identical algebra.

Each selected output tile lowers to one fixed CPU Host invocation. It declares complete x and y support and only the corresponding query row interval, closed through each source's own tile grid. Atomic source axes can enlarge the physical read set. Query values may be unordered or repeated. The task searches y directly through borrowed views, using the direction of its first two knots; it passes the original x/y stencil to the exact inverse algorithm.

The entire x and y arrays must be finite, x must strictly increase, and y must be globally strictly increasing or decreasing without plateaus. Consumed query values must be finite and within the closed numeric y range under `reject`. Selected final outputs must be finite and representable. These are unchecked caller preconditions. Preparation checks schemas, static parameters, canonical outputs, exact read membership and scratch; runtime performs no full-array topology or query-validation scan.

Only selected global output coordinates are evaluated and allocated. Each invocation constructs its own `ExactCurve` in preallocated Host scratch of `sizeof(ExactCurve)`, including the 96-slot arena and ratio workspace. No promoted x/y arrays or per-query certificates are retained. Search costs `O(R log K)` for R rows of a task, plus local inverse arithmetic/refinement. Input tile owners, selected output allocations and granted-lane scratch require admission; the full logical output need not fit in memory.

Empty selection emits no payload task while static metadata remains checked. Rebinding reuses immutable preparation and creates independent Run-owned output; there is no content-cache or repeated-demand identity guarantee. Legal immutable signed/zero strides and unaligned offsets are supported through the compiled views. Result owners survive context destruction. Successful task tiles can feed internal dependencies; public call Results appear only after all required tasks succeed and ordered retirement occurs.

Cancellation is polled during binary search, output traversal and exact root/limb operations. Internal arena, product, shift and zero-denominator guards remain active. PCHIP lattice comparison retains its segment-interior bounds, exact infinity-boundary midpoints and finite termination rules. Accelerated candidates require the existing unique-rounding certificate; otherwise exact evaluation proceeds. Fixed invocations do not expose per-value fallback counters.

Accelerated Float32 uses the existing certified bracket with unique destination
rounding, then exact fallback when unresolved. Float64 uses the exact inverse.
Collinear stencils still bypass lattice search; noncollinear wide-integer
comparison is scalar and restores the surrounding profile. Scalar/NEON/AVX2
implementations remain; no Accelerate/SME inverse backend is introduced.

Inverse-curve acceptance binds immutable source Results and compares selected outputs with the independent exact inverse oracle. Cover both y directions, mixed dtypes, knot/clamp and signed-zero boundaries, tiny/huge derivatives, affine views, concurrent Runs, caller/worker floating environments, empty requests, static preparation reuse, sparse large logical outputs, resource rejection, cancellation and output lifetime. The forward and inverse registrations share `fixed_curve.hpp`; a shared-adapter change also requires focused forward regression validation.

## CRV-11 resampling and certified lowpass

The four resampling helpers append existing CRV-01 interpolation and independent
`core.identity` nodes. Identity preserves new-position dtype and raw
special bits; sample demand uses fixed curve tasks and requires valid numerical inputs. Exports remain caller
controlled and IDs reserve declared/referenced nodes. No new interpolation
primitive, implicit filtering or sample-rate inference is introduced.

Uniform lowpass emits one ABI 8 CPU task per selected output tile. Static nonzero taps and boundary mapping determine the source-grid closure; wrap seams and reflect turning points are handled before tile enumeration. Only selected outputs are allocated. Preparation verifies canonical output, exact ordered reads and scratch. Each invocation borrows affine views and owns fixed arithmetic state plus sample words and optional coefficient enclosures.

Accelerated uniform lowpass prepares certified coefficient enclosures once per selected tile invocation at 128 fractional bits in fixed scratch. Hardware convolution propagates coefficient, product, sum and normalization error to the final FP32 gate. Numerical uncertainty uses strict convolution. `CheckedLowpassKernel` keeps cancellation/resource failures distinct from numerical refinement and returns them before shortcuts or retries. Accelerated convolution polls at most every 64 logical samples. Nonuniform lowpass uses the directed integration path below.

Uniform tap support is exact before coefficient evaluation. A 128-bit product
represents `2*cutoff*j` as a dyadic; integer phases are sinc zeros and the integer
part determines its lobe sign. Hann/Blackman endpoints are exact zeros. Gaussian
support is always positive. Mapped physical reads may merge, but logical offsets
retain their order for NaN payload selection and signed infinity contributions.
Finite pair sums are compared exactly in binary64 minimum-subnormal units;
if every nonzero ±j pair equals twice the center, evenness proves the final
center independently of coefficient approximation. Constant raw bits are checked
first to preserve -0. Even these uniform shortcuts require a certified positive
full discrete normalizer.

`CheckedLowpassKernel` supplies both families with shared mathematical coefficient formulas and explicit failure state. Rational-π
arguments are reduced by an exact integer chosen from an interval endpoint;
any chosen integer is valid if the remaining certified angle lies in [-2,2].
Taylor sin/cos terms have decreasing alternating tails after the checked index.
Tiny sinc arguments use their removable-singularity series. Decimal window
coefficients are exact rationals. Kaiser uses the positive series
`I0(beta*sqrt(1-u*u)) = sum (beta*beta*(1-u*u)/4)^n/(n!)^2`;
no rounded sqrt is introduced. Once subsequent ratios are at most 1/2, twice
the first omitted term bounds the tail. The common `1/I0(beta)` cancels from
both the complete numerator and denominator. Gaussian divides binary significands
before applying their exact exponent difference, avoiding a tiny denominator
being rounded to zero. Its negative exponential returns a real positive-value
enclosure even when only `[0,2^-precision]` is needed. Such a tap remains a
source/IEEE dependency.

Nonuniform lowpass emits one ABI 8 CPU task per selected output tile, reading all positions and the complete filtered values axis for the tile's other-axis rectangle. Numerical positions must be finite and strictly increasing; contributing endpoints and selected outputs must be finite. These are caller preconditions. The task searches source views directly, including mixed dtype and signed/unaligned affine layouts.

Continuous geometry keeps coordinates as signed integers in `2^-1074` units. `piece_at` returns the segment immediately left or right of a cursor; source search direction accounts for reflected branch reversal. A backward cursor exactly at a period seam uses the previous period. World segments retain full affine endpoints and separate clipped integration limits. Stored coordinate combinations fit below 2102 bits. Two piece records and fixed traversal integers occupy invocation scratch; geometry is replayed without a positions copy or partition vector.

For continuous exact landmarks, merge the positive and negative partitions and
compare `F(u)+F(-u)` by integer rational cross products. If every overlap has zero
slope and the same exact constant C, return `RN(C/2)`. This proves interior affine,
collinear insertion, odd cancellation, zero-boundary half-constant and subnormal
midpoint cases without endless interval refinement. Numerator/slope integers need
less than 4204 bits, paired numerator/denominator less than 6306/4204, and equality
cross products less than 10510, within the 12288-bit arena. All source endpoints
are read before this shortcut. Raw-bit constancy is checked over actual contributing pieces, preserving local -0 even when distant samples differ. Full continuous normalization is strictly
positive for every admitted tuple: Gaussian is positive; the four sinc windows
are nonnegative and nonincreasing on [0,R], and integration by parts against the
strictly positive sine-integral primitive proves positivity. Successive positive/
negative sine lobes paired against decreasing `1/t` prove that primitive's sign.

General continuous integration uses `u=(world-center)/R`; the Jacobian cancels.
Each source piece is an exact affine `A+B*u`. A global polynomial in `u^2`
encloses the true kernel with an explicit uniform error. Sinc/cos/Gaussian
coefficients use their full Taylor formulas; after a ratio bound of 1/2, twice
the next coefficient bounds the omitted tail on |u|<=1. Kaiser expands the finite
positive I0 series in `(1-u^2)^n`, retaining its independent uniform tail.
Product error is `Ea*|b|+Eb+Ea*Eb` with true sinc magnitude at most one.
Analytic monomial moments integrate the polynomial against each affine piece.
The numerator adds `piece_length*max_endpoint_magnitude*kernel_error`, while the
denominator adds twice the kernel error. A positive denominator enclosure and
identical RN endpoints of the full quotient are required for publication.
Sources are scaled by an exact power of two, restored only in final rounding;
coordinate differences divide as same-unit integers, avoiding premature underflow.

Both families refine Q precision from 128 through 4096; continuous Taylor order is at most 512. Nonuniform polynomial scratch contains 513 sinc intervals, 513 window intervals and 1025 product intervals. Blackman temporarily uses the product buffer for its second cosine before convolution. Uniform coefficient enclosures also occupy invocation scratch. Global series can be expensive or fail on
high frequencies/large beta/support-to-sigma ratios; unresolved zero/near-midpoint
cases return ResourceExhausted rather than guessing a sign or publishing a fixed
quadrature approximation. No arbitrary one-sided interval is declared negative
zero. Nonuniform accelerated keys use strict arithmetic. Geometry work scales with the number of pieces and repeated binary searches; large support can require many periods even though storage is fixed.
Every scale scan, partition piece, coefficient/refinement and limb operation has
work/cancellation checks. The production path has no MPFR/Fraction dependency;
those remain independent manual references.

Resampling acceptance inherits CRV-01's exact-copy and numeric-accuracy requirements and adds public composition checks. Check selected query/column support, actual explicit position copies, mixed dtypes, signed/unaligned source layouts, caller/worker floating environments, concurrent Runs, rebinding, cancellation, sparse large outputs and escaped owners. Position-only selection preserves raw special bits independently of interpolation. Uniform/nonuniform low-pass retains its separate numerical and execution acceptance contract.

## Shared rounding and regional execution

Dyadic final ratios detect power-of-two denominators and extract rounding bits
directly, with guard/sticky ties-even handling. General final ratios may opt in
to a normalized conservative hardware quotient and final FP32 acceptance; control
predicates, expression intermediates and certification endpoints never opt in.
Small-divisor interval operations visit active limbs. Interval multiplication
shares floor/ceiling products and uses two endpoint products for positive ranges.
All strict outputs retain their original bits.

Dynamic range and smoothstep use fixed per-tile tasks with their specified numerical ordering. Prefix and integral scans use exact source carry within each fixed group and convert only declared output tiles; their source closure follows the group's positive boundaries. Task-preserving output selection retains all reads and writes of a kept task. See [scans](scans-execution.md), [range](range-execution.md) and [interpolation](interpolation-execution.md). Layout transforms use [fixed gather tasks](layouts-execution.md) with exact static reshape/transpose reads and the possible source set for dynamic slice. Sorting definitions retain their separate complete-input contract. No private thread pool is used.

Accelerated Float32 exp now uses an IQK-derived NEON/AVX2 polynomial on [-80,80],
with one callback floating-environment guard, a fixed batch workspace and a
proved four-step error bound. The comparison backends remain removed. Packed callbacks now copy
blocks, classify Float32 lanes once and repair only rejected lanes, while
retaining bounded work/cancellation checks. The [exp report](exp-performance.md) records both platform oracles,
scalar/batched-SLEEF/IQK comparisons and the limits of the profiler evidence.

The [batch update](batch-performance.md) batches remaining supported SLEEF point
operations and amortizes certified/CRV environment setup. A following
[trigonometric update](trig-performance.md) replaces eligible Float32
sin/cos/sinpi/cospi/sinc outputs with proved SIMD polynomials, and implements
full finite-Float32 sincpi through integer-unit reduction and a central
polynomial. Proofs include the final output conversion and strict reference;
benchmark and independent-oracle scopes are recorded separately in those reports.

Measure and Gate carry Run-dependent identities: each newly produced table receives a fresh ObjectId. The fixed executor retains no cross-Run result/checkpoint/block cache. Report reading requires all thirteen members and the explicit matching configuration; the largest member window is 72 bytes.
