# Numeric mathematical implementation

This document records the implemented numerical machinery for numeric and curve
operators and their shared facilities. Specification acceptance remains independent: the
operator specifications retain their Proposed status. Numerical correctness is
the selected discrete mathematical result, including raw special-value rules;
it is not a claim that every input succeeds under every resource budget.

Current measurements and coverage limits are in the
[accelerated specification](op_specs/NUM_accelerated_contract.md#current-implementation);
reproduction commands are in the [workflow README](../../../examples/numeric_workflow/README.md).

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
pool and rounding workspace are inline members of the operation continuation.
The host admits the complete `sizeof` capacity before constructing that state.
Nested frames reuse slots and restore the previous cursor, including exception
paths. No limb operation uses an external allocator or persistent mutable cache.

Directed addition is exact within checked capacity. Multiplication and division
retain the remainder and round endpoints outward. For a negative quotient,
floor increments the magnitude when the remainder is nonzero; ceiling does not.
Four endpoint combinations enclose products and quotients. Division rejects a
denominator interval containing zero as an unresolved mathematical refinement,
not as permission to guess a numeric result. A work/capacity/cancellation Status
is sticky and never converted into a refinement retry.

Every primitive charges bounded work and polls cancellation. Multiword division
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
python3 examples/numeric_workflow/generate_math_constants.py --check
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

NUM-01 evaluates four samples together in a fixed node-by-lane continuation.
Coordinates, coefficients and requested coverage retain their existing rules.
Outward intervals cover each strict RN64 expression step. Only a final certified
result is published; uncertain lanes replay the entire strict expression,
including domain/failure classification. The same kernel handles vector tails.
`2*x+1` and ordinary exp can therefore avoid both per-sample continuation work
and multiprecision math. Cancellation and all continuation storage are accounted.

DependencyPath diagnostics identify the numerical implementation, compiler/OS/build identity and selected profile. The build identity includes the pinned SLEEF sources and integration module, preventing old/new accelerated cache identity reuse. In that path, evaluations are admitted before arithmetic and copied values are counted before their fixed publication store. Those counts and an attempted fallback survive later resource or cancellation failure. Cache hits add no fabricated arithmetic work. This accounting model does not describe the NUM-11/13 Whole accumulators below.

NUM-04/05 now use Whole callbacks. Their arithmetic and fallback rules are
unchanged, but per-value diagnostics described above are unavailable (N/A).
The older timing and regional acceptance tables below describe their recorded
revisions; see [current Whole measurements](point-math-whole.md).

NUM-11 reductions and NUM-13 scans keep one `NumericDiagnostics` object local to each Whole `write`. After `MathTensorReader` returns an input word, the callback increments `evaluated_values` immediately before passing that word to the exact accumulator. On successful execution, the count therefore matches sample words supplied to the accumulator, not output groups or positions. Prefix zero boundaries, integral-image carries and final output conversions do not add input attempts. The reducer's `implementation` identity names `photospider.reduction-exact/1;accumulator-u64x68`; scans use `photospider.scan-exact/1;accumulator-u64x68`. Both append the selected profile's `selection_implementation` and host/build identity. That ISA label describes the four-word output-selection scratch helper; it does not claim that the complete reduction or scan algorithm uses SIMD arithmetic.

For these exact accumulator paths, Strict and Apple checks report zero `strict_math_calls`, zero `strict_fallbacks` and zero fallback reasons. This records that these operations did not dispatch a strict fallback; it is not a measure of scalar work. `reduce_count` uses a separate metadata-only identity and reports zero evaluated values. Empty requests do no sample arithmetic, and a completed-result cache hit contributes no new input attempts.

The callback attempts one final report after a normal return or a status it handles through `finish`. It records a handled operation failure before reporting and preserves that primary status if diagnostic admission is rejected. WorkLimit or cancellation can prevent the report from being merged. A `Status` thrown while acquiring or reading a tensor window, or `std::bad_alloc` caught by the outer `math_callback`, is recorded by that outer layer and can bypass the local final-report attempt. These counters do not promise a partial progress record for every terminal path.

## Independent reference and execution

The kernel has no MPFR, GMP, Boost or libbf dependency. The manual Python oracle
uses MPFR 4.2+ in a separate process and refines downward/upward results until
both round to the same output bits. Whole sinc and exact rational pi references
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

The public workflow, oracle, low-resource/error tests and local timing command
are in [the numeric workflow example](../../../examples/numeric_workflow/README.md).
The manual executable is excluded from the default build and is not registered
with CTest or integration testing. WSL is used only for correctness; performance
observations must identify the native hardware, shape, dtype, worker/cache
settings and actual measured execution scope.

The public workflow executable checks all functions, negative input strides,
four caller rounding modes and preserved flags, exact sparse support and dirty
mapping, typed validation, Atom overflow/denominator diagnostics, required
upstream errors, cache changes to NaN bits, escaped result lifetime, and
work/cancellation/capacity cleanup including fallback counters on failure.
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
is part of the admitted Whole workspace; work and cancellation checks occur in
AST evaluation and inside long arithmetic/refinement. Bounded unresolved
transcendental refinement returns ResourceExhausted.

Pure static preparation owns the program across compiler stages, outputs and
dynamic runs. Sealed handles validate registry/definition identity, all static
input metadata and exact parameter bits, including signed zeros and NaNs.
Direct preflight prepares once per call, or reuses an explicit matching handle;
Whole execution receives and reuses the sealed plan owner. Static plan/program
allocations use ordinary host storage outside runtime managed scratch. They
have source/program size bounds, but no separately enforced preparation budget
or global preparation cache. The synchronous callback borrows the AST while
holding its preparation owner. Package 0.17/traits15 requires a C++ rebuild;
canonical framing14 and C ABI9 remain.

All nonempty values requests execute Whole with static active-input projection,
no per-index stages or dependency certificates. The selected output materializes
all N values before consumer projection; any numerical failure is Run scoped.
Axis-only skips coefficients, AST evaluation and adjacent-coordinate validation;
its actual scratch omits the evaluator, while common workspace admission reserves
the maximum values state. Count=1 excludes end. Whole numeric counters are
unavailable. Current migration checks and timing are in
[expression Whole](expression-whole.md).

The diagnostic `strict_math_calls` counts actual dispatched mathematical calls,
including exact special paths and failed calls. Per-function fallback counters
attribute accelerated strict fallbacks; rejected pre-dispatch domains add no
call. Legacy reporters without per-function attribution merge into `Other`.
All numeric diagnostic merging is checked for overflow before publication.

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
part of the admitted Result continuation workspace. Endpoint limits compare
exact cross-products.
Direct node/clamp conversion, two-point linear degeneration and exterior
tangent evaluation have smaller bounds. Arithmetic polls work/cancellation
inside limb multiplication and final division. Independent scoped arithmetic
review also compared 3208 Fraction formula expansions successfully.

For every nonempty request, the CPU Whole Result continuation collects complete
x, y and query inputs, including recognized typed validation and upstream
failures. It validates all x knots and query controls before y arithmetic,
evaluates every query and output column, and returns one immutable dense Result
of shape [N] or [N,C]. Empty reads no payload; static metadata validation still
applies. Sparse demand records the requested dependency region Q, but does not
reduce input collection, computation or the complete output owner. The Result
can retain full computed coverage, with sample coordinates in global positions.

The mathematical stencil remains unchanged: an exact knot/clamp uses one y;
linear uses two endpoints; PCHIP uses its fixed local stencil. Generic y values
outside every evaluated stencil do not undergo an additional finite scan. Typed
validation and upstream execution cover complete inputs, including unused values.
All query rows and output columns are evaluated, so errors in unrequested rows
or columns can fail the run. Reject still performs full upstream collection.

Any input change invalidates the recorded output demand. Cache identity includes
complete input versions, profile, metadata and parameters. Numerical failures
have Run scope and publish no partial successful output; they do not provide
independent per-column Atom success. Input strides, offsets, zero strides and
negative strides remain legal. Packed output storage outlives the context.

Search/classification costs O(K+N log K), followed by N scalar evaluations
(or N*C for multi). Classification is shared across columns. The complete dense
output costs b*N (or b*N*C) bytes; reserve it even for one requested cell. Input
collection and retained owners also require admission. The continuation declares
its fixed exact arithmetic workspace, and the host-accounted knot vector has 8*K
element bytes plus allocator/metadata overhead. K<=65536 bounds its elements at
524288 bytes. No full slope table is required.

Poll work/cancellation during reads, binary search, exact arithmetic and before
publication. Capacity and work exhaustion return ResourceExhausted, with failed
output/workspace released. A giant logical broadcast can therefore fail a small
payload budget even for sparse demand. Resource limits do not authorize weaker
arithmetic. Backend, typed, upstream, stale and cancellation failures retain their
categories. Numeric failures are OperationFailed/InvalidDomain or final-output
ArithmeticOverflow, with Run scope and offending port/index where available.

They do not validate the current Result workflow or establish its performance. They
cover Result declarations and bindings, Whole sparse support and error paths,
layouts and floating-environment preservation, Empty and metadata validation,
work/cancellation/resource limits, cache and binding replacement, typed Mask
validation, upstream failures, and Result/read-window lifetime and release. See the
workflow README for commands and exact evidence boundaries.

## CRV-02 exact Bezier function sampling

`bezier_function.cpp` registers three explicit degree-2/3 sampler profiles. `ExactSampling` shares NUM-01's exact weighted grid and RN64 addition/conversion. `ExactPolynomial` stores signed integer coefficients in units 2^-1075; `ExactBezier` validates exact derivative minima and solves the unique interior Bx root by dyadic bisection. Interval Horner bounds the whole By image. Integer pseudo-remainder GCD and exact low-degree root tests decide shared zeros and rounding midpoints; they avoid assuming that a small residual proves rounding. Nonzero half-subnormal boundaries use the boundary sign when the tie rounds to zero. Direct dyadic roots and constant ordinates use exact evaluation.

The fixed arena has 640 64-bit limbs (40960 bits) per number and 96 slots, all inside the admitted Result continuation state. Initial coefficients have fewer than 2105 bits. The worst degree-three integer remainder chain grows through 4211, 10529 and fewer than 25280 bits. Horner evaluation at the 8192 fractional bit refinement cap stays below 26683 bits; final rounding temporaries fit the same arena. The static maximum live-slot bound is 44, including temporary GCD calls. Explicit work/cancellation runs inside long arithmetic; unresolved refinement or storage limits return ResourceExhausted/CapacityLimit. The solver has a finite budget, not a promise that every algebraic input resolves at any caller-selected budget. All profiles use this exact numerical path, so no numerical fallback is reported; profile-specific integer helpers supply the scalar/NEON/AVX2 operations. The Whole callback does not expose per-value solver/fallback counters.

Both outputs use CPU Whole Result continuations. A nonempty `values` request collects complete anchors and handles plus start and, only when count>1, end with Data, Validation and Descriptor roles. Typed validation and upstream failures apply to all collected inputs. The continuation validates all x topology, then every generated coordinate, domain condition and adjacent-separation control before y arithmetic. It evaluates every sample and publishes a complete dense Result, including for sparse demand. The Result certifies full coverage in global sample coordinates. Empty demand reads no input payload and executes no numeric kernel; the runtime can still poll metadata and seal the Result.

Mathematical selection remains local: exact anchor/clamp uses that anchor y; interior evaluation uses the selected segment's anchor y and relative y handles. Generic y outside every evaluated stencil is not additionally finite-checked. All output samples are evaluated, so an invalid otherwise-unrequested sample can fail the complete values Run. Failure publishes no partial successful values. Each output publishes through its own all-or-nothing Result transaction. Axis collects only start and end, with count=1 excluding end, computes its 24-byte tuple and does not read controls or validate per-coordinate separation. Static metadata for all four edges is checked for either output.

The Result association records actual source ObjectIds. Input changes update recorded dependency dirtiness for the requested region; anchor and handle changes affect `values`, while start/end affect outputs that depend on them. The count=1 dependency omits `end`. Complete immutable input versions, profile, metadata and parameters remain in cache identity, while binding topology can reuse the static PreparedOperation. Both outputs retain independent identities, names, dtypes and empty facets. Result owners and authorized read windows remain valid past context and source-handle retirement; releasing the final window releases the Root allocation.

It covers Result declarations/bindings/reads, count-one and axis dependency projection, sparse full-output coverage and source association, same-demand and fresh-source cache behavior, preparation reuse, error ordering, all-port layouts, caller/worker fenv, work/cancellation/output/scratch limits, typed validation, and Result/read-window lifetime. It also verifies that retained values and axis windows keep 48 Payload bytes alive after all four source owners retire, that releasing the values window leaves the 24-byte axis readable, and that releasing the final window returns all Root usage to zero. Generic y data outside the mathematical stencil is not scanned; recognized typed validation still applies to the complete active Result input. The maximum-count oracle cases verify that a sparse request cannot admit the complete dense output under a 1 MiB Payload budget. They check capacity rejection at the operation node, not successful million-element numerical execution.

Four additional cases per profile check maximum-count full-output capacity rejection. Reproduction commands, installed consumer configuration and the manual benchmark protocol are in the [numeric workflow README](../../../examples/numeric_workflow/README.md#bezier-function-sampling-crv-02).

## CRV-03 parametric Bezier evaluation

`parametric_bezier.cpp` evaluates explicit t and does not solve an inverse. The
public `evaluate_bezier_node` accepts four Result inputs and specializes the
output schema from their descriptors and static degree/dtype parameters. The
manual `parametric.cpp` workflow declares schema-backed inputs, binds source
Results, executes through the public compiler/context API and reads output
Results through authorized tensor access. Its `Value` objects provide immutable
source storage only; source Results refer to that storage and account it as
Referenced. Result tensor schemas carry descriptor and semantic facets.

The Whole Result continuation requests active inputs with Data, Validation and
Descriptor (role 13), validates every segment/t row before component arithmetic,
evaluates all N*D output cells and publishes one immutable dense [N,D] Result
with full certified coverage in global coordinates. A sparse query restricts
recorded dependency/dirty mapping while retaining full input collection,
computation and output allocation. Empty output coverage is empty and Empty does
not poll a failing handle producer; static metadata and other execution stages
remain active. Nonempty requests preserve upstream handle failures before
endpoint or invalid-query arithmetic. Each output transaction publishes
atomically; a failure releases unpublished state. Output association records the
current input ObjectIds.

Endpoint t=0/1 reads only the selected anchor for the mathematical value. An
interior reads both anchors and all relative handles for its segment and
component. Generic numeric data outside evaluated stencils is not additionally
finite-checked. Complete typed validation and upstream execution still cover all
active inputs. The fixture's typed RGBA handle Result has spatial layout and
channel axis 2. Its sparse query asks for column 0 at t=.5, but Whole execution
computes all D=4 columns and reads the alpha handle as well. A separate t=0
case shows that endpoint mathematics skips the handle while complete typed
validation still checks it and rejects an invalid alpha value. Every query row
and output component is evaluated, so any bad segment/t or evaluated component
fails the Run and no partial Result is published. No global mathematical
topology scan is performed.

RN64 reconstruction and the exact Bernstein/power-Horner implementation are
unchanged. The scalar numerator bound is 5329 bits for degree<=3 and t in [0,1].
Endpoints convert one anchor. For an interior exact zero, the result is negative
only when every reconstructed control is negative zero; nonzero underflow keeps
the mathematical sign. All profiles retain the same exact calculation with
scalar/NEON/AVX2 integer helpers and the shared accelerated contract. Caller and
worker floating environments are preserved.

The continuation retains one row and fixed arithmetic workspace independent of
N and D. It classifies rows before component arithmetic and reclassifies each
row for all columns. Work is O(N+N*D*degree), plus exact arithmetic, input
collection and typed validation. Complete output costs b*N*D bytes. Admission
accounts for that output and fixed workspace even for a one-cell request, as
well as collected inputs, retained owners and metadata. Public constant-node
composition checks with D=2^39 and N=2^40 request only one cell but fail at the
parametric node with ResourceExhausted/CapacityLimit because the complete output
does not fit. These cases verify complete-output capacity rejection. No per-cell
dependency certificates or full coefficient table is retained.

The host worker and resource ledger account computation. Cancellation is polled
on reads, row controls, exact arithmetic and before publication. Computation
WorkLimit, output/scratch capacity failure and cancellation propagate their
categories and release unpublished output and all Root resources. Invalid
segment/t is InvalidArgument/InvalidDomain; used nonfinite controls are
OperationFailed/InvalidDomain; actual RN64 reconstruction or output conversion
overflow is OperationFailed/ArithmeticOverflow. Typed, upstream, stale and
backend errors preserve their categories. Replacing segment or t source Results
reuses static preparation; completed-demand repetition returns the same Result
object, while a fresh equivalent four-source bundle demonstrates a content-cache
hit and refreshes all four source associations. Whole execution exposes no
per-value counters, so zero counters do not imply zero arithmetic or fallbacks.

The fixture runs all 16 combinations of reversed, unaligned layouts across all
four inputs and a separate zero-stride source composition. Under each caller
rounding mode it verifies the actual computation worker's floating environment.
Nine malformed static Result schema/parameter cases are
rejected before execution. Lifetime checks cover Float32 and Float64 3x2 outputs:
all four source backings expire, the escaped Result and authorized read window
share one 24-byte or 48-byte output owner, and releasing both returns all Root
live capacity to zero. The manual Strict/Apple checks also cover typed
validation, source association, static preparation, full sparse coverage,
upstream error ordering, work/cancellation and output/scratch cleanup. The installed 0.32.0
package consumer compiled and linked the same manual source. The exact consumer
configuration is recorded in the [workflow README](../../../examples/numeric_workflow/README.md#parametric-bezier-evaluation-crv-03).

## CRV-04 public LUT1D authoring

`numeric/lut1d.hpp` exposes six authoring functions that append ordinary runtime nodes to a caller-owned `WorkflowDocument`: expression and Bezier samplers each produce `values` and `axis`, while linear and PCHIP interpolation templates first produce Float64 linspace queries and then interpolation values. `BakedLut1d` returns node-output references; `outputs()` creates caller-named workflow exports. Construction creates graph metadata only. It reserves existing and referenced node IDs, allocates the lowest available positive IDs, enforces the 65,536-node limit and appends nodes only after the complete expansion succeeds. A failed construction leaves the graph unchanged. The caller owns output declarations and compilation.

The source operations use Result inputs and outputs. `SequenceInput` pairs a workflow edge with an immutable Result schema hint for a Float32/Float64 `[1]` endpoint; the compiler checks the actual binding independently. The manual fixture keeps arrays as local `Value` backing, publishes their storage as immutable source Results, and binds those Results through `ExecutionBindings`. It does not import values through a snapshot store. `Fixture::run` returns `DemandResult.results`, and the fixture reads values through authorized Result tensor access.

All expanded formal outputs use CPU Whole continuations. A nonempty `values` demand computes the full table and reports its full coverage, including for a sparse request. Interpolation templates allocate the full Float64 query array (`8*count` bytes), followed by the full table (`count*C*dtype_size` bytes), in addition to source owners and source-specific workspace. The host accounts these allocations through one Root resource budget. A sparse million-row multi-PCHIP request under a 1 MiB Payload budget checks full-output rejection; it does not demonstrate sparse success or successful maximum-size execution.

`axis` is a separate Float64 `[3]` Result. Axis-only demand skips expression coefficients, Bezier controls and interpolation x/y. For `count=1`, the expanded source omits the end payload. For `count>1`, a first-value request still collects the end input and computes the complete grid. Each source keeps its own mathematical formula, selection, typed validation and numerical allowance. Active Result collection can propagate typed or upstream errors outside the requested numerical region, and numerical failures retain the source operation's Run scope. Empty `values` demand returns empty coverage without polling its failing numerical producer or reading payload; metadata processing and Result sealing remain ordinary execution work.

Source associations contain the active Result ObjectIds, while dependency support and dirty mapping follow the output request. Repeated identical demand retains the completed Result identity. A fresh same-content source bundle reuses cached outputs with current associations. Each output association follows its direct producer inputs: an interpolated table points to x, y and the current query Result; the query Result and independent axis point to current endpoint Results. A one-node sampler asserts exactly two cache hits for `values` and `axis`; an interpolation graph also exports `query_values` and asserts exactly three hits. Replacing bindings reuses static `PreparedOperation` objects and recomputes outputs for the new endpoints. Function controls affect values demand; they do not dirty the independent axis.

The manual harness in `examples/numeric_workflow/baking.cpp` contains seven groups. Its generated-versus-explicit graph check spans six templates, two output dtypes and four demand modes: 48 graph pairs per profile. It compares Result descriptors, coverage, requested bits, source support and dirty mapping, and checks separately encoded analytic sample values. These fixture values test composition equivalence; they are not a replacement for the independent numeric oracles for the source operations. Other groups exercise count-one and failing-end behavior, empty-demand and axis/source isolation, source failures, cache/association behavior, static preparation reuse, authoring boundaries, mixed endpoint/table dtype, reversed/unaligned/scalar-zero source layouts, caller and actual-worker floating-environment restoration, work/payload limits, active cancellation, and recovery.

The ownership group covers all six templates for Float32 and Float64 outputs. It retires each source backing while retaining separate values and axis Results and authorized read windows. The two output buffers remain independently charged to Root Payload; each window remains readable after its Result handle is released. Releasing the final window returns live Root resources to zero.

The million-row sparse admission case returns `ResourceExhausted / CapacityLimit` at node 1 before the complete linspace query Result can be admitted. It verifies that the full query payload is budgeted before interpolation, not that a million-row table was numerically computed. The fixture also injects work exhaustion and cancellation during actual computation; both paths release live Payload to zero. The direct numerical oracles for expression, sequences, curves and Bezier remain in their source families. The baking graph comparison verifies that the six public constructors expand to the corresponding source nodes without changing any source formula.

## CRV-05 dynamic-axis LUT1D

`UniformAxis` reconstructs each RN64 endpoint-weighted knot, verifies exact step,
strict ordering and singleton raw endpoints/+0. Scalar/channel lookup preserves
one-entry selections and exact whole-formula linear interpolation. Descending
pairs reorder both x/y to satisfy ExactCurve's positive-denominator internal
representation. Channel queries are classified independently.

Every nonempty request uses a CPU Whole Result continuation. It requests input,
table and axis with Data, Validation and Descriptor roles, including typed
validation and upstream failures. The continuation validates the complete
reconstructed axis, then every finite/domain query before table arithmetic. It
computes every output element/channel and publishes an immutable packed Result
with full coverage and global coordinates, even when the request is sparse.
Empty reads no payload; static metadata checks still apply.

Mathematical table selection is unchanged: knot/clamp/singleton converts one
entry; interpolation/extrapolation uses its adjacent pair, independently per
channel. Generic entries outside all evaluated stencils receive no additional
finite scan. Typed validation and upstream collection cover the complete table.
Errors in otherwise-unrequested queries or evaluated channels can fail the Run.
The Result association retains the actual source ObjectIds. Dirty mapping follows
the recorded query region, while an input/table/axis replacement invalidates that
demand. Static preparation remains reusable across those bindings. Cache identity
retains all input versions, profile, metadata and parameters. There is no
per-channel Atom success isolation. Output dtype, shape and empty facets remain
unchanged; arbitrary input offsets, unaligned and signed/zero strides remain
legal. The complete immutable owner survives context destruction.

For M input elements, work is O(L+M log L) plus exact arithmetic and typed
validation. Singleton lookup is constant time per query. The continuation retains
the reconstructed Float64 grid in a Root allocation charged to Metadata by the
default ResourceAllocator; its element bytes are 8*L, up to 8 MiB, plus allocator
overhead. Input and table reads use authorized zero-copy Root windows over their
source storage; the operation does not make a dense copy of the complete table.
Root accounts retained owners and windows. Fixed exact workspace and O(rank)
coordinate state are also admitted. The continuation retains no per-output
certificates, rows or lookup table. Output costs dtype_bytes*M regardless of
requested coverage.

The complete axis uses endpoint-weighted RN64 coordinates. One caller-preserving
floating environment covers axis reconstruction and curve evaluation; unresolved
accelerated bounds still use the same exact fallback. Work/cancellation is checked
in axis generation, input reads, lookup and exact arithmetic, and before publishing.
Resource failure never authorizes skipping axis validation or weaker arithmetic.
Unpublished output/workspace is released; no partial success is published.
Numeric InvalidDomain/ArithmeticOverflow failures have Run scope. Typed, upstream,
resource, stale, backend and cancellation failures retain their categories.
Whole does not expose per-value fallback counters; report those as unavailable.

The Result manual workflow now checks seven groups under Strict and the locally
available Apple profile. Its independent Fraction driver passes 1,416 bit-equal
cases for each profile. The six CRV-04 baking chains connect to the Result LUT
consumer; source replacement retains the same static preparation, fresh source
Results produce cache hits with current associations, and sparse requests retain
full Whole output coverage. The fixture also covers typed RGBA validation,
upstream failures, all-port layouts, caller/worker fenv, resource and cancellation
rollback, output lifetime and Root release. Its maximum L=1,048,576 case uses a
16 MiB Payload capacity; Root diagnostics show the 8 MiB axis grid charged to
Metadata while peak Payload remains below 8 MiB, confirming no dense table copy.
A 2^39-channel sparse request fails with CapacityLimit at the LUT node.

The existing `test_numeric_result_math_lut1d` integration fixture retains additional
LUT1D checks, including custom Result schemas, channel batch shape [2,2,2],
eight scalar Fraction golden bits, axis/query/table error precedence, and
cancellation inside a 352-limb `ExactCurve` slot. Its 1 MiB Payload cases reject
a sparse input[262144] request whose full Float64 output needs 2 MiB, and accept
a zero-stride Float32 table [262145] while adding only the 8-byte output to live
Payload. These are integration-fixture results, separate from the manual
`lut1d.cpp` groups.

## CRV-06 ColorArray and color ramps

The 15 independent primitives provide 45 CPU-profile Whole keys. Nonempty
requests collect all input arrays, validate every stop and query, then apply the
unchanged one/two-row mathematics to every position. A complete dense ColorArray
Value retains its facet, trailing channel closure and ICC resources; sparse
public fragments retain that complete owner. Numeric failures affect the run;
any input edit dirties all recorded output demand. Unused generic color rows
stay mathematically unused, while complete upstream/typed validation can fail.
Fixed allocator-owned arithmetic state and O(K) stop keys replace per-output
points, dependency records and certificates. Account 8K stop element bytes plus
metadata overhead, all collected inputs and N*C*sizeof(dtype) output payload.

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
association conversion, linear transfer and gamma=2. The latter rounds a
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
exact real meanings. The final `m` and alpha ratios are each split into a
unit-scale interval and an exact binary exponent. Their exponents are passed
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
still fail complete-color validation. Correctly rounded zero alpha with nonzero
rounded RGB produces AssociationUnderflow; otherwise transparent black is +0.

All profiles currently return strict bits, including the accelerated RGB entries
whose contract allows four ULP. Profile-specific integer comparisons are used;
there is no attempted approximate transfer backend and no fabricated fallback.
Whole callbacks do not expose DependencySession numerical counters; attempted,
copy and fallback counts are N/A. Scalar and native NEON integer comparisons are
retained; AVX2 remains registered for its platform. No Accelerate/SME floating
backend or NUM-14 certificate is applied. Exact state and unpublished output
retire under work, capacity or cancellation failure.

ColorArray's canonical model/white/primary/transfer/hue/association metadata is
independent of the Image semantic enum. Exact 8192-bit integer determinant tests
validate the primary basis and positive white constraints. Explicit immutable
ICC v2/v4 CMYK printing profiles carry SHA-256/length identity and owned bytes;
`ResourceBindings` resolves and references the matching allocation at compiler,
Value, snapshot, dependency, result-v2 and structured execution boundaries.
Duplicate identities require equal original bytes and canonical owner selection.
Empty color results retain required resources. Sample-only optional caches skip
resource-bearing results. Package 0.16 requires C++ consumers to rebuild; C ABI 9
and the existing semantic/digest version numbers remain unchanged.

 These cover strict numerical references, mixed dtype, model domains,
RGB association, hue units, resources, arbitrary layouts, active cancellation,
full-input typed failure and full-output budgets. Focused numeric, compiler,
color and resource CTests pass.
The retained accelerated allowance is contractual; bit matching in these cases
does not replace that contract. Public commands are in the workflow README.

## CRV-07 joint three-dimensional LUT application

`lut3d_application.cpp` registers separate trilinear and tetrahedral primitives,
with three CPU identities each. Three `UniformAxis` instances globally validate
the Float64 `[3,3]` dynamic axes, including every reconstructed coordinate and
the exact RN64 step. Their independently ascending/descending keys determine
stored cells; an interior exact hit starts at that knot, and the terminal hit
uses the final cell. Original input colors are validated before clamp.

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

The six registered profiles use Whole Result continuations. Static preparation
validates port schemas, dtypes, shapes, descriptions, and ColorArray facets,
then retains the immutable program state for reuse across source rebinding.
Each nonempty execution validates all three axes and every original input
color/query before clamp, then performs exact selected-vertex mathematics.
Typed validation and upstream failures cover the complete active Results and
can precede callback numeric checks. Only exact positive-weight vertices enter
the formula; zero-weight generic samples remain mathematically unused.

The continuation records the current input, table, and axis ObjectIds. Its
published Result has full certified coverage in global coordinates, including
for a sparse request, and retains the complete three-component ColorArray facet.
Input or table replacement updates dirty mapping from the recorded source
support; the prepared operation remains reusable across those bindings. Empty
demand performs no numerical producer poll. Each invocation publishes through
an all-or-nothing Result transaction, and numeric failures have Run scope.
Input/table samples use authorized zero-copy Root windows. Escaped Results and
windows retain their storage after source owners or the context retire; releasing
the final window returns the corresponding Root allocation. `UniformAxis` owns
Float64 grids costing 8*(N0+N1+N2) bytes, in addition to the packed output and
fixed exact workspace. Capacity, work, cancellation, and allocation failures
release unpublished output state.

All profiles use exact integer arithmetic and the existing scalar/NEON/AVX2
integer helpers. The implementation adds no approximate backend or NUM-14
certificate; numerical counters from the former dependency session do not
apply to this Result continuation.

The independent Fraction oracle reconstructs RN64 grids, cells, and simplexes,
then rounds rational sums using separate Python integer IEEE logic.
The fixture exercises Result declarations and bindings, sparse full-color
coverage, source associations and cache reuse, preparation reuse, Empty and
failed table producers, typed ColorArray validation, reversed/unaligned layouts,
caller and worker floating environments, owner retirement, and window lifetime.
Two interpolation outputs retain 12 and 24 Payload bytes while their escaped
read windows remain alive after source retirement; releasing the final windows
returns Root usage to zero.

A Float64 table with maximum 256^3 grid shape succeeds using an 8-byte backing
window under an 8 MiB Root Payload cap, showing that the execution does not copy
the 384 MiB dense table. A separate sparse query requiring 2^38 output positions
is rejected as `CapacityLimit` at node 1 because the complete output exceeds the
budget. This is a capacity rejection, not successful numerical execution at
that output size. The `test_numeric_lut3d_result` behavior test runs the manual
fixture under Strict. The shared `test_numeric_result_math_lut3d` includes distinct
LUT workflow, boundary, preparation, and resource fixtures; those are separate
evidence from the six manual groups. Reproduction and installed-consumer
commands are in the [numeric workflow README](../../../examples/numeric_workflow/README.md#joint-three-axis-color-luts).
No x86 execution or native GPU execution is established by this evidence.

## CRV-08 whole-formula shapers

The public linear pair expands existing whole-rational remap and scalar views.
The inverse explicitly computes `remap(lower,lower,upper,lower,lower)` before
broadcasting the target lower bound. This enforces source bound order while
preserving raw lower bits, including -0. Float32 zero/one literals use an exact
existing scalar cast; graph construction performs no payload computation. The
shared bounded ID allocator is also used by LUT1D baking.

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
Whole does not expose DependencySession fallback/evaluation counters. Existing
scalar/NEON/AVX2 arithmetic identities remain; there is no new Accelerate/SME
backend or application of NUM-14's finite four-term certificate.

All six log keys use Whole callbacks with complete input/bounds collection and
full dense output. Typed/upstream errors remain observable anywhere in inputs;
callback bounds validation precedes IEEE special handling. Whole errors have
Run scope and any input edit invalidates all recorded demand. Fixed admitted
ExactShaper state and O(rank) traversal replace per-output points, certificates
and publication owners. Account full collected input, full output and workspace.
Linear templates already expand NUM Whole remap/constant nodes; no redundant
linear primitive is added and inverse bound-order validation remains connected.

Native strict/Apple each pass 4196 Fraction/directed MPFR-4.2.0-p12 cases across
all four forms and both dtypes, near-equal/extreme bounds, exact roots/midpoints,
IEEE payload/zero/Inf, underflow/overflow and monotonic groups. Six public manual
groups inspect whole support/dirty/cache behavior, arbitrary layouts and fenv,
ColorArray validation, Empty, inverse -0, log direct workspace/refinement
interruption and all four public paths' work/output/cancellation/upstream errors.
Focused numeric/compiler CTests pass. Public commands are in the shaper workflow README.

## CRV-09 measured LUT3D baking

`bake_lut3d` stages an ordinary `WorkflowDocument`, invokes the authoring-only
source builder for grid colors `[N0,N1,N2,3]` and validation colors `[P,3]`, then
checks both expansions with Compiler metadata analysis. The helper protects
existing declarations, Result schemas, nodes and exports; a failed builder
leaves the document unchanged. It retains no builder or capture. A SHA-256
recipe identity includes the expanded source, designated endpoints and compiler
semantic identity. Both expansions execute under one frozen binding snapshot,
including shared dynamic inputs and optional `ResourceBindings`. Pointwise
independence is the caller's assertion.

`baking3d.cpp` binds source fixtures from `Value` backing into immutable
Root-owned Referenced Results. The public bake graph publishes only Result
outputs: table, axis and report. Geometry programs use Whole Result with
role-13 validation and complete outputs. Static preparation validates metadata
and stores reusable geometry or report POD state. Empty tensor demand skips
payload evaluation; source, table, axis and report failures preserve their
observed failure status and scope.

The axis and grid generators use `UniformAxis`; center coordinates average the
actual neighboring Float64 endpoints and round once. Every grid color remains a
global report dependency even when a constant source ignores its generated
color input. The owned table is a `curve.bake_lut3d.table` v2 Result, and the
exported table is an immutable `photospider.tensor` Result with `samples`, the
chosen dtype, ColorArray v1 and `atomic_trailing_axes=1`. Unpack supplies the
actual table to CRV-07 with explicit `table_dtype` and reject policy. The
CompleteBundle report stores eleven fixed fields and associates observed input
ObjectIds in port order; entry 2 identifies the owned table. Gate validates all
report fields and the table with role 13, checks that association, then publishes
the requested complete-color region in global coordinates. An association
mismatch need not precede table-pixel validation.

Per-component error and `atol+rtol*abs(reference)` are compared as exact
integers in `2^-2148` units. Error needs fewer than 3173 bits; the threshold
product needs fewer than 4197, within the 4352-bit workspace. Exact integer
comparison selects maxima and preserves the earliest ordinal on ties. The
reported maximum is rounded to Float64, then advanced by one positive IEEE step
if that rounded down; an unrepresentable error is diagnostic +Inf. The independent
Fraction oracle checks 384 cases per profile against the current Result harness.

Measurement visits row-major cell centers and then extra points. The continuation
stores only counts, maxima and first-failure data, and seals all eleven fields
together after complete measurement. Its Result fields retain global support
across axis, grid, owned table, validation points, source reference and applied
colors. The specialized report schema validates its fields, domain and metadata.
Its canonical resolved schema participates in graph, plan and cache identity.

Pack, Unpack and Gate first request authorized Result tensor views. Legal mappings
retain the existing common storage owner; a physical `ViewUnavailable` triggers
transactional materialization of the requested region, preserving sample bits
and dependency support while charging Root Payload. The baking harness checks
Float32/Float64 across all five geometries, reversed and unaligned layouts, and
verifies the mapped row address equals the source backing address with zero Root
Payload. Pack uses two polls for this path. Report reads use bounded windows;
table or report results can be retained and released independently, while table
read-window ancestry may keep backing alive. Releasing the final window releases
all Root live capacity. Source axis owners expire after context and fixture
retirement.

All source operations, full grid colors, validation points, converted colors,
table backing, report fields, windows and scratch consume their respective host
budgets. A small requested table region does not reduce the full bake or global
measurement work. The harness verifies a huge grid is rejected with
CapacityLimit at its geometry node; it does not demonstrate successful
maximum-shape execution. Measured acceptance applies only at the listed centers
and extra points under the selected interpolation method and table dtype; it is
not a continuous-domain bound. A completed threshold failure returns a readable
report with `passed=false`, while a dependent table request fails with
`LutApproximationToleranceExceeded`. Axis remains independent. Source, invalid
data, cancellation, stale binding and resource failures remain execution errors.

 Its bake assertions
are independent fixture coverage, not all cases from `baking3d.cpp`. Its cache
fixture uses a 64 MiB Result cache,
1,048,576 `maximum_dependency_cache_metadata` proof units and 128 Mi
dependency-cache work units; 14 hits were observed, while the assertion requires
only a positive count. The metadata limit is measured in proof units, not bytes. Exact commands and
coverage are in the [numeric workflow README](../../../examples/numeric_workflow/README.md#measured-three-dimensional-lut-baking).
No current x86, native-GPU or successful maximum-shape execution is claimed.

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
22528-bit, 96-slot continuation-owned arena fits the arithmetic with fewer than
64 live slots. Every refinement, allocation and limb multiplication is charged;
failed arithmetic status is checked before interpreting a comparison as zero.
The extracted shared Hermite helper retains forward interpolation's original
1074-unit defaults and identical algebra.

`curve_inverse.cpp` registers all six formal keys as Whole. Complete x/y/query
collection precedes global topology and all-query control validation. Each query
then selects its original pair/stencil from promoted x/y and executes the unchanged
inverse math. Only fixed state plus 16*K promoted element bytes (and allocator
metadata) are retained, with no per-query certificates. Complete inputs and
N*sizeof(dtype) output are budgeted. Any input edit invalidates the full output;
dynamic numerical failures have Domain/Run scope, including undelivered queries
or output overflow. Empty demand reads nothing. Cancellation/work checks and
publication remain atomic. DependencySession numerical counters are N/A.

Accelerated Float32 uses the existing certified bracket with unique destination
rounding, then exact fallback when unresolved. Float64 uses the exact inverse.
Collinear stencils still bypass lattice search; noncollinear wide-integer
comparison is scalar and restores the surrounding profile. Scalar/NEON/AVX2
implementations remain; no Accelerate/SME inverse backend is introduced.

The [public inverse workflow](../../../examples/numeric_workflow/README.md#inverse-curves)
uses a Result-backed manual fixture with local Value source backing, immutable
source Results, workflow bindings, Result outputs and authorized read windows.
Its five groups run under Strict and Apple. The manual fixture checks
both caller and actual computation-worker floating environments, negative and
zero strides, true fresh-source cache reuse with refreshed direct associations,
same-demand ObjectId retention, static preparation reuse, Empty propagation
without polling a failed producer, typed SampledSignal error attribution,
K=65536 execution, and retained Float32/Float64 outputs after source-owner
retirement. An escaped Result and read window retain 12 or 24 Payload bytes;
closing the last window returns Root live capacity to zero. A public N=2^40
constant View is rejected with CapacityLimit at the inverse node because Whole
execution requires complete output storage; it does not demonstrate numerical
execution at that output size. `test_numeric_result_math_baking` separately covers
`inverse_workflows` and `inverse_boundaries`; those integration fixtures are not
the manual fixture.

## CRV-11 resampling and certified lowpass

The four resampling helpers append existing CRV-01 interpolation and independent
`core.identity` nodes. Identity preserves new-position dtype, facets and raw
special bits; only sample demand adds curve validation. Exports remain caller
controlled and IDs reserve declared/referenced nodes. No new interpolation
primitive, implicit filtering or sample-rate inference is introduced.

Accelerated uniform lowpass builds certified coefficient enclosures once per
Whole invocation at 128 fractional bits, retaining them in a resource-accounted vector.
The hardware convolution propagates coefficient, product, sum and normalization
error to the final FP32 gate. Exact zero taps, constant shortcuts, special values
and source demand remain governed by the original discrete reference. Unresolved
coefficients or outputs use the original strict convolution. Nonuniform lowpass
continues to use the directed integration path below.

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

`DirectedLowpassKernel` encloses complete mathematical coefficients. Rational-π
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

Continuous geometry keeps coordinates as signed integers in `2^-1074` units.
Floor quotient/remainder select exact periods; forward segments choose the right
piece at knots and reflected backward segments choose the left. World coordinate
copies are clipped to positive-length support, with no wrap seam bridge. Stored
coordinate combinations fit below 2102 bits; the common 12288-bit records and ResourceVector maps are admitted under host capacity. All 30 formal low-pass keys use Whole. A nonempty request validates the complete active inputs and reads authorized Root windows directly; the callback does not collect or copy the complete source tensors. It computes all centers and columns, then publishes one dense output. Nonuniform evaluation validates positions globally and retains one output's exact piece map for reuse. Full source access and output storage are required for partial delivery; any input edit invalidates all recorded output demand. Typed/upstream failures and nonuniform remote invalid values can fail the run. Dynamic numerical errors have Domain/Run scope. Empty requests read no sample payload after static preflight.
Uniform tap order/zero omissions and nonuniform positive-length integration
remain mathematical rules. No per-output dependency records remain.

For continuous exact landmarks, merge the positive and negative partitions and
compare `F(u)+F(-u)` by integer rational cross products. If every overlap has zero
slope and the same exact constant C, return `RN(C/2)`. This proves interior affine,
collinear insertion, odd cancellation, zero-boundary half-constant and subnormal
midpoint cases without endless interval refinement. Numerator/slope integers need
less than 4204 bits, paired numerator/denominator less than 6306/4204, and equality
cross products less than 10510, within the 12288-bit arena. All source endpoints
are read/validated before this shortcut. Full continuous normalization is strictly
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

Both families refine Q precision from 128 through 4096; continuous Taylor order
is at most 512. Coefficients are separately admitted ResourceVectors, not hidden
heap limbs or cached rounded weights. Global series can be expensive or fail on
high frequencies/large beta/support-to-sigma ratios; unresolved zero/near-midpoint
cases return ResourceExhausted rather than guessing a sign or publishing a fixed
quadrature approximation. No arbitrary one-sided interval is declared negative
zero. Nonuniform accelerated keys still use strict fallback. Whole numeric/fallback
counters are unavailable (N/A).
Every scale scan, partition piece, coefficient/refinement and limb operation has
work/cancellation checks. The production path has no MPFR/Fraction dependency;
those remain independent manual references.

Public examples, exact fixtures, response checks and runtime commands are in
[resampling](../../../examples/numeric_workflow/README.md#signal-resampling),
[uniform lowpass](../../../examples/numeric_workflow/README.md#uniform-lowpass)
and [nonuniform lowpass](../../../examples/numeric_workflow/README.md#nonuniform-lowpass).

The four maintained manual executables exercise distinct parts of this family. The
resampling examples preserve CRV-01's existing exact-copy and numeric-accuracy
checks; this family has no separate numerical oracle.  Manual checks cover all-port negative, unaligned and zero strides, worker and caller floating-environment preservation, an independent two-constant oracle on a non-last axis, complete Whole input/dirty support, remote-column Run errors, uniform NaN/Inf tap order, typed validation, cache replacement and source association, static preparation reuse, real-work cancellation, 2^40-output capacity rejection, and escaped owners. Resampling additionally checks raw and typed position forwarding, output-specific support, remote-query failure, K=65536 execution, and a zero-copy mapped position view at N=2^40 while the full samples output is rejected for capacity. Float64 layout checks cover all four templates and eight source layouts under each caller/worker rounding mode; Float32 checks separately cover bit-preserving positions, typed validation, and owner lifetime. These are execution checks, not an independent resampling oracle.

## Shared rounding and regional execution

Dyadic final ratios detect power-of-two denominators and extract rounding bits
directly, with guard/sticky ties-even handling. General final ratios may opt in
to a normalized conservative hardware quotient and final FP32 acceptance; control
predicates, expression intermediates and certification endpoints never opt in.
Small-divisor interval operations visit active limbs. Interval multiplication
shares floor/ceiling products and uses two endpoint products for positive ranges.
All strict outputs retain their original bits.

Range operations and smoothstep publish requested regions in a single continuation.
Sort projects requested boxes onto unique logical lines, reuses one permutation
per line and output, and writes dense offsets without changing stable order or
source witnesses. Prefix and cumulative integral scans request up to 64 source
samples per stage, while each published observation retains only its actual
prefix support. No private thread pool is used.

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
