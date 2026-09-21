# Numeric mathematical implementation

This document records the implemented numerical machinery for numeric and curve
operators and their shared facilities. Specification acceptance remains independent: the
operator specifications retain their Proposed status. Numerical correctness is
the selected discrete mathematical result, including raw special-value rules;
it is not a claim that every input succeeds under every resource budget.

Current measurements and coverage limits are in the
[accelerated specification](op_specs/NUM_accelerated_contract.md#current-implementation);
reproduction commands are in the [workflow README](../../../examples/numeric_workflow/README.md).
Historical delivery measurements below
retain their original dates and do not describe current timings.

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
AdvSIMD and AVX2/FMA implementations for ordinary exp, ln, sin, cos, tan, pow and
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

Dependency-path diagnostics identify the numerical implementation, compiler/OS/build identity
and actual selected profile. The build identity includes the pinned SLEEF sources
and integration module, preventing old/new accelerated cache identity reuse. Evaluations
are admitted before arithmetic; copied values are counted before their fixed
publication store. Both counts and an already attempted fallback survive a later
resource or cancellation failure. Cache hits add no fabricated arithmetic work.

NUM-04/05 now use Whole callbacks. Their arithmetic and fallback rules are
unchanged, but per-value diagnostics described above are unavailable (N/A).
The older timing and regional acceptance tables below describe their recorded
revisions; see [current Whole measurements](point-math-whole.md).

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

## NUM-04 validation and native timing

On 2026-09-19 the combined 22-function suite passed 7,524 independent cases per
profile on native Apple M5 / Clang 21 (strict and Apple, MPFR 4.2.2), and on
Intel Core i9-12900 Ubuntu WSL / Clang 18 (strict and AVX2, MPFR 4.2.1).
These are sampled correctness checks, not exhaustive domain proofs. Additional
exact-Fraction primitive/enclosure probes and independent mathematical review
checked the directed arithmetic and analytic remainders.

The public workflow executable checks all functions, negative input strides,
four caller rounding modes and preserved flags, exact sparse support and dirty
mapping, typed validation, Atom overflow/denominator diagnostics, required
upstream errors, cache changes to NaN bits, escaped result lifetime, and
work/cancellation/capacity cleanup including fallback counters on failure.
Both local installed-package consumers passed, as did the focused compiler
unit and ClangFormat 21/cpplint. No new test joins CTest or integration testing.

Native timing below is an observed small workload on Apple M5, Clang 21,
RelWithDebInfo: Float64, Whole, one worker, cache off, three repetitions.
Compilation and freezing precede timing; synchronous execution and result
assembly are timed. Results and evaluation counts are checked before accepting
each measurement. Columns give median microseconds at N=1 / N=256. These figures
are not general performance guarantees or evidence of accelerated speedup.

| Function | Strict median us, N=1 / 256 | Apple median us, N=1 / 256 |
| --- | ---: | ---: |
| abs | 109 / 175 | 115 / 160 |
| neg | 93 / 145 | 69 / 172 |
| sqrt | 95 / 6335 | 143 / 6120 |
| exp | 247 / 36949 | 279 / 36947 |
| ln | 196 / 15163 | 146 / 15239 |
| sin | 357 / 65466 | 343 / 65836 |
| cos | 362 / 65130 | 344 / 66203 |
| tan | 389 / 76652 | 365 / 75894 |
| floor | 94 / 177 | 99 / 168 |
| ceil | 81 / 173 | 75 / 166 |
| round | 88 / 164 | 76 / 168 |
| sign | 64 / 164 | 61 / 167 |
| reciprocal | 72 / 1216 | 87 / 960 |
| sinpi | 281 / 51651 | 273 / 51657 |
| cospi | 276 / 51416 | 319 / 51597 |
| tanpi | 320 / 60632 | 339 / 60488 |
| sinc | 216 / 34872 | 231 / 34568 |
| sincpi | 218 / 30095 | 223 / 30412 |
| sinpi_rational | 365 / 69828 | 375 / 70781 |
| cospi_rational | 372 / 70843 | 391 / 71098 |
| tanpi_rational | 428 / 79284 | 419 / 79736 |
| sincpi_rational | 287 / 41593 | 278 / 40893 |

Peak controlled payload was 2,864 / 4,904 bytes for elementary operations and
208,272 / 210,312 bytes for transcendental operations at N=1 / 256. Ordinary
Apple transcendental inputs recorded 1 / 256 strict fallbacks; elementary
inputs recorded zero. Rational timing uses p/q=1/7. The executable emits max
latency and all configuration fields as CSV for fresh measurements. WSL was
used only for correctness.

## NUM-05 validation and native timing

On 2026-09-19 the nine-function public workflow suite passed 14,174 independent
integer/Fraction/MPFR cases per profile: native Apple M5 / Clang 21 strict and
Apple with MPFR 4.2.2, and Intel Core i9-12900 Ubuntu WSL / Clang 18 strict and
AVX2 with MPFR 4.2.1. Exact dyadic midpoint powers, parity boundaries, extreme
ratios, signed zeros, both-NaN priority, overflow and subnormals are included.
These sampled checks do not establish exhaustive domain success.

The manual executable also passed all nine public fixtures, both-port sparse
support/dirty checks, UInt8/Int64 per-Atom overflow, independent/all-port negative
and unaligned strides, zero strides, preserved fenv, typed validation on either
port, required failing producers despite special identities, actual fallback and
inner-work cancellation, capacity cleanup, cache mutation and composition.
Local installed consumers and the focused compiler unit passed. The public
manual target remains outside CTest/integration registration.

The following native Apple M5 / Clang 21 RelWithDebInfo measurements use a=2,
b=.3, Float64, Whole, one worker, cache off and three repetitions. Scope and
capacity accounting match the NUM-04 timing above. Median microseconds are
shown for N=1 / N=256; the executable emits max latency and full configuration
as CSV. WSL supplies correctness results only.

| Function | Strict median us, N=1 / 256 | Apple median us, N=1 / 256 |
| --- | ---: | ---: |
| add | 162 / 1660 | 142 / 1425 |
| subtract | 111 / 1588 | 111 / 1476 |
| multiply | 107 / 1534 | 108 / 1456 |
| divide | 101 / 1598 | 106 / 1423 |
| minimum | 91 / 178 | 89 / 216 |
| maximum | 87 / 190 | 95 / 193 |
| pow | 284 / 42747 | 273 / 45761 |
| atan2 | 325 / 53030 | 325 / 51931 |
| atan2pi | 366 / 62054 | 369 / 62801 |

Peak controlled payload is 2,864 / 4,904 bytes for elementary operations and
208,272 / 210,312 bytes for power/angles. Apple power/angles record 1 / 256
strict fallbacks; elementary functions record zero. No speedup claim follows.

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

The following dated results describe the pre-Whole implementation.

On 2026-09-19, native Apple M5 / Clang 21 strict and Apple (MPFR 4.2.2), and
Intel Core i9-12900 Ubuntu WSL / Clang 18.1.3 strict and AVX2 (MPFR 4.2.1),
each passed 715 independent coordinate/literal/stepwise Fraction/MPFR cases.
The suite includes the sensitive `1/(exp(x)-a)` boundary, adjacent coefficient
values, intermediate RN64 cancellation, both dtypes and named coefficients.
Separate exact-decimal/grammar probes and scoped numerical review supplement
these sampled checks; they are not an exhaustive expression-domain proof.

All four profiles passed the seven public workflow groups: sampling and axis,
mixed bindings/plan reuse/cache/dirty, staged scalar reads/strides/fenv/diagnostics,
schema/grammar and unused upstream failures, work/cancel/stage/payload limits,
metadata-exhaustion recovery/Atom isolation, and second-box failure/cancellation
with unpublished-owner release. Prepared-program and installed 0.15 workflows,
old-minor rejection, focused compiler/resources/dependency units, formatting and
lint passed locally. Existing installed consumer configuration also checks that
0.14 is rejected and 0.15 is accepted. Independent scoped reviews cover the
parser, mathematics, preparation identity/lifetime and exception cleanup.
Manual examples are excluded from default builds and CTest/integration testing.

The diagnostic `strict_math_calls` counts actual dispatched mathematical calls,
including exact special paths and failed calls. Per-function fallback counters
attribute accelerated strict fallbacks; rejected pre-dispatch domains add no
call. Legacy reporters without per-function attribution merge into `Other`.
All numeric diagnostic merging is checked for overflow before publication.

### Native expression timing

Apple M5 / Clang 21 / RelWithDebInfo, Float64, start=0/end=1, one worker,
cache off, three repetitions. Compile/freeze precede timing; synchronous
execution and result assembly are timed. Session and Run work limits are each
2^50. Seven independently derived output checkpoints and exact counters are
checked before a measurement is accepted. Columns show median/max microseconds.
The three-point ROI uses indices 1, N/2 and N-2. These are observed timings,
not a general performance guarantee or an accelerated speedup claim.

| Expression | N | Demand | Strict median/max us | Apple median/max us |
| --- | ---: | --- | ---: | ---: |
| `2*x+1` | 256 | three-point-ROI | 220/1680 | 266/363 |
| `2*x+1` | 256 | Whole | 3711/3789 | 3944/4808 |
| `2*x+1` | 65536 | three-point-ROI | 174/186 | 194/227 |
| `2*x+1` | 65536 | Whole | 856049/867781 | 829437/948635 |
| `2*x+1` | 1048576 | three-point-ROI | 166/214 | 235/260 |
| `2*x+1` | 1048576 | Whole | 13582296/13620404 | 12909805/13425266 |
| `exp(x)` | 256 | three-point-ROI | 563/3116 | 634/694 |
| `exp(x)` | 256 | Whole | 35757/35882 | 36257/36432 |
| `exp(x)` | 65536 | three-point-ROI | 625/637 | 585/628 |
| `exp(x)` | 65536 | Whole | 9288761/9325761 | 9519921/9924564 |
| `exp(x)` | 1048576 | three-point-ROI | 544/583 | 555/579 |
| `exp(x)` | 1048576 | Whole | 148981539/157373847 | 149846235/152819554 |

Every row records two polls and two unique scalar source coordinates. Whole
peak controlled payload is 218312 / 740552 / 8604872 bytes at the three N
values; every three-point ROI uses 216288 bytes. Polynomial math-call/fallback
counts are zero. Exp records M strict mathematical calls; strict fallback counts
are zero, Apple counts M-1 for Whole (exp(0) is exact) and M for this ROI.
These counters describe actual backend calls, not a distinct approximate exp
implementation. Native results show that this exact implementation remains
expensive for large full arrays. WSL timing is not used. The manual CSV reports
controlled payload; static plan/program allocations and unmanaged host container
storage are outside that field.

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
part of the admitted callback workspace. Endpoint limits compare exact cross-products.
Direct node/clamp conversion, two-point linear degeneration and exterior
tangent evaluation have smaller bounds. Arithmetic polls work/cancellation
inside limb multiplication and final division. Independent scoped arithmetic
review also compared 3208 Fraction formula expansions successfully.

For every nonempty request, one CPU Whole callback collects complete x, y and
query inputs, including recognized typed validation and upstream failures. It
validates all x knots and all query controls before y arithmetic, evaluates every query and every output column, and returns
one immutable dense output of shape [N] or [N,C]. Empty reads no payload; static
metadata validation still applies. Sparse demand restricts publication coverage,
but does not reduce input collection, computation or the complete output owner.

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
collection and retained owners also require admission. The callback declares its
fixed exact arithmetic workspace, and the host-accounted knot vector has 8*K
element bytes plus allocator/metadata overhead. K<=65536 bounds its elements at
524288 bytes. No full slope table is required.

Poll work/cancellation during reads, binary search, exact arithmetic and before
publication. Capacity and work exhaustion return ResourceExhausted, with failed
output/workspace released. A giant logical broadcast can therefore fail a small
payload budget even for sparse demand. Resource limits do not authorize weaker
arithmetic. Backend, typed, upstream, stale and cancellation failures retain their
categories. Numeric failures are OperationFailed/InvalidDomain or final-output
ArithmeticOverflow, with Run scope and offending port/index where available.


Clang 21.1.3 RelWithDebInfo on Apple M5 / macOS 27.0 (26A5425a), package
0.18.0, passed 2487 independent Fraction cases for each strict/Apple profile,
the public workflow groups, active cancellation/budget checks, and focused
numeric/compiler tests. The existing Apple inverse workflow also passed after
the shared exact evaluator gained an optional already-normalized environment.
Other platforms were not rerun for this migration.

Native sampling (2026-09-21) uses K=17, N=512, x[j]=j, y[j,c]=j+c,
query[i]=(i%64)/4+1/8, dense inputs, complete demand, one CPU worker and cache
disabled. Strict uses Float64, Apple uses Float32. One warm invocation precedes
seven measured invocations, with analytic identity checks outside timing. Both
adapters use the same kernel; the comparison adapter is the CRV-01 source at
3d35f5eb. Public latency includes execution, managed metering, collection and
result assembly, excluding compile/freeze. The core column measures the current
numeric callback on prepared complete Values, including lookup, arithmetic,
allocation and publication, but excluding scheduling, collection and managed
metering. It is not a pure arithmetic instruction benchmark.

Budgets: 1 GiB payload, 2 GiB Host, 512 MiB Metadata, 512 MiB dependency state,
2^40 dependency/Run work units, default unlimited managed work. Raising Metadata
above the 16 MiB default is necessary for the old C=4 adapter; at that default it
fails dependency poll allocation. Context-reported peak Metadata changes from
15,814,976/65,098,224 bytes (C=1/4) to 3,208 bytes; this excludes uninstrumented
process memory. Complete input collection can slightly increase payload: C=4
Apple peak is 294,128 before and 296,380 bytes after.

All times below are median [min,max] milliseconds for these seven samples.

| Operation | C | Profile/dtype | Public before | Public Whole | Numeric callback core |
| --- | ---: | --- | ---: | ---: | ---: |
| linear | 1 | strict/Float64 | 10.026 [9.726,10.364] | 2.650 [2.602,2.939] | 2.449 [2.417,2.555] |
| linear | 1 | apple/Float32 | 7.286 [7.084,7.381] | 0.145 [0.141,0.162] | 0.080 [0.076,0.083] |
| linear | 4 | strict/Float64 | 43.516 [42.457,46.465] | 10.580 [10.360,10.659] | 9.887 [9.775,10.553] |
| linear | 4 | apple/Float32 | 32.964 [32.647,35.991] | 0.316 [0.311,0.330] | 0.301 [0.243,0.411] |
| pchip | 1 | strict/Float64 | 13.403 [13.284,13.714] | 5.448 [5.293,5.686] | 5.014 [4.870,5.303] |
| pchip | 1 | apple/Float32 | 7.511 [7.446,7.657] | 0.225 [0.220,0.237] | 0.167 [0.166,0.171] |
| pchip | 4 | strict/Float64 | 55.231 [54.921,56.102] | 21.307 [21.054,21.696] | 19.805 [19.645,20.042] |
| pchip | 4 | apple/Float32 | 32.468 [32.017,33.298] | 0.701 [0.669,0.741] | 0.617 [0.546,0.673] |

Three 12-second Time Profiler captures of Apple Float32 PCHIP C=4 retain
11,781/11,769/11,745 execution-stack samples for the old adapter, initial Whole
adapter and final Whole adapter respectively. Inclusive counts overlap. The old
adapter has DependencySession on 84.14% and Footprint methods on 52.42% of these
samples. Initial Whole shows repeated fegetenv/fesetenv on 12.59%; moving the
same guard around the complete callback removes sampled per-value fenv frames
in the final capture. Final nextafter interval expansion is 42.66%, managed
ResourceBudget methods 6.84%, and ValueFragments::collect 0.55%. No NUM-14
finite-Float32 certificate is used. Scalar exact arithmetic and accelerated
interval/fallback paths retain their existing mathematical contract.

Raw driver, build script, CSV sample ranges, logs, trace exports and analysis
are local ignored artifacts under `build/crv-whole/` (`curves-final-times.csv`,
`curves-{before,after,final}.trace`, `profile-summary.txt`). These workloads do
not establish performance for nonlinear exact fallback, all K/N/C sizes or
other CPUs. Whole diagnostics do not expose per-value fallback counts.

## CRV-02 exact Bezier function sampling

`bezier_function.cpp` registers three explicit degree-2/3 sampler profiles.
`ExactSampling` shares NUM-01's exact weighted grid and RN64 addition/conversion.
`ExactPolynomial` stores signed integer coefficients in units 2^-1075;
`ExactBezier` validates exact derivative minima and solves the unique interior
Bx root by dyadic bisection. Interval Horner bounds the whole By image. Integer
pseudo-remainder GCD and exact low-degree root tests decide shared zeros and
rounding midpoints; they avoid assuming that a small residual proves rounding.
Nonzero half-subnormal boundaries use the boundary sign when the tie rounds to
zero. Direct dyadic roots and constant ordinates use exact evaluation.

The fixed arena has 640 64-bit limbs (40960 bits) per number and 96 slots,
all inside the admitted callback state. Initial coefficients have fewer than
2105 bits. The worst degree-three integer remainder chain grows through
4211, 10529 and fewer than 25280 bits. Horner evaluation at the 8192 fractional
bit refinement cap stays below 26683 bits; final rounding temporaries fit the
same arena. The static maximum live-slot bound is 44, including temporary
GCD calls. Explicit work/cancellation runs inside long arithmetic; unresolved
refinement or storage limits return ResourceExhausted/CapacityLimit. The solver
has a finite budget, not a promise that every algebraic input resolves at any
caller-selected budget. All profiles use this exact numerical path, so no
numerical fallback is reported; profile-specific integer helpers supply the
scalar/NEON/AVX2 operations. The Whole callback does not expose per-value
solver/fallback counters.

Values uses one CPU Whole callback. For every nonempty values request, collect
complete anchors and handles plus start and, only when count>1, end. Recognized
typed validation and upstream failures apply to all collected components. Validate
all x topology, then every generated coordinate/domain/adjacent-separation control
before y arithmetic. Evaluate all count outputs and allocate one complete dense
values owner, even for sparse demand. Empty reads no payload.

Mathematical selection remains local: exact anchor/clamp uses that anchor y;
interior evaluation uses the selected segment's anchor y and relative y handles.
Generic y outside every evaluated stencil is not additionally finite-checked.
All output samples are evaluated, so an invalid otherwise-unrequested sample can
fail the complete values Run. Failure publishes no partial successful values.
Axis independently collects start/end only (start for count=1), computes its
24-byte tuple and never validates control payloads or per-coordinate separation.
Static descriptors of all four edges are still validated for either output.

Any collected anchor/handle edit invalidates the recorded values demand; controls
never dirty axis. Start/end affect both outputs, except that count=1 ignores end.
Complete immutable input versions, profile, metadata and parameters remain in
cache identity. Both outputs retain their existing names, dtypes, empty facets
and independent identity; there is no new pairing object. Returned owners survive
context destruction. Sparse publication coverage retains the complete owner.


Native Clang 21.1.3 strict/Apple public workflows and each profile's 382
independent Fraction/de Casteljau/Euclid-Sturm numeric/error cases passed; four
additional maximum-count cases verify full-output admission failure at 1 MiB.
These four are resource checks, not successful million-element numeric runs.
Focused numeric/compiler tests, active cancellation/work/output/workspace
checks, arbitrary layouts/fenv and typed-input validation passed.

Sampling on Apple M5 / macOS 27.0 (26A5425a), package 0.18.0, uses degree 2/3,
K=2, N=129, Float64 inputs/output, anchors [[0,0],[1,1]], quadratic offset
[.5,.5], or cubic offsets [.25,.25],[-.25,-.25]. The grid is [0,1] inclusive.
Every output is independently checked against RN64(i/128). One warm invocation
precedes seven measured samples. One CPU worker, cache off, 1 GiB payload,
2 GiB Host, 512 MiB Metadata and dependency state, 2^40 dependency/Run work,
and default unlimited managed work apply. Compile/freeze/checking are outside
public execution timing. The comparison adapter is CRV-02 at 3d35f5eb linked
to the same kernel; the current core column calls the prepared complete-Value
numeric callback, including allocation/classification/solver/publication but
excluding scheduler, collection and managed metering. All times are median
[min,max] ms. Small overlapping sample ranges cannot establish that core is
faster or slower than public execution.

| Degree | Profile | Public before | Public Whole | Numeric callback core |
| --- | --- | ---: | ---: | ---: |
| quadratic | strict | 9.919 [9.838,10.164] | 5.328 [5.049,5.457] | 5.051 [5.018,5.104] |
| quadratic | apple | 9.654 [9.525,9.858] | 4.826 [4.793,4.921] | 4.850 [4.812,4.901] |
| cubic | strict | 87.547 [86.536,87.955] | 76.894 [76.396,78.221] | 76.885 [76.514,77.787] |
| cubic | apple | 85.549 [84.666,86.681] | 74.007 [73.099,75.409] | 74.512 [73.850,74.814] |

Context-reported peak Metadata drops from 7,281,016/7,281,024 bytes to
3,584/3,592 bytes for quadratic/cubic. These are managed statistics, not RSS.
The 12-second Apple cubic Time Profiler capture contains 11,932 execution-stack
samples: ExactBezier::inverse is inclusive in 99.18%, ExactPolynomial methods
97.45%, ResourceBudget 2.99%, ExactSampling 0.07%, and collect 0.02%. Inclusive
categories overlap. Exclusive samples identify fixed-integer top/subtract/add
and shifting as the main remaining work. The observed cubic improvement is
limited by exact inverse arithmetic; no cheaper unproved inverse or NUM-14
Float32 certificate is substituted.

Raw drivers/build commands, seven-sample ranges, trace/XML, logs and analysis
remain locally under ignored `build/crv-whole/` (`bezier-times.csv`,
`bezier-after.trace`, `bezier-profile-summary.txt`). This is native evidence for
the stated grids; it does not establish all-shape or cross-platform speedups.


## CRV-03 parametric Bezier evaluation

`parametric_bezier.cpp` retains ExactSampling RN64 reconstruction and
ExactPolynomial integer Bernstein/power-Horner evaluation with direct output
rounding. Unlike CRV-02, it evaluates explicit t and does not solve an inverse.
The scalar numerator bound is 5329 bits for degree<=3 and t in [0,1]. Endpoints
convert one anchor; interior exact zero is negative only if every reconstructed
control is negative zero. All profiles retain the current exact calculation,
with scalar/NEON/AVX2 integer helpers and the unchanged accelerated contract.

One CPU Whole callback collects all four complete inputs with recognized typed
validation, then validates every segment_indices/t row before any component
arithmetic. It evaluates all N*D output cells and publishes one immutable dense
[N,D] owner. Sparse demand restricts returned coverage but retains complete input
collection, computation and output memory. Empty reads no payload; complete
static metadata still validates.

Mathematical selection is unchanged: t=0/1 uses only the selected anchor
component; an interior uses both anchors and all relative handles of that segment
and component. Generic numeric data outside every evaluated stencil is not
additionally finite-checked. Complete typed and upstream validation still covers
unused inputs. All rows/components are evaluated, so formerly unrequested bad
index/t/components can fail the Run. No partial successful output is published.
There is no global mathematical topology/monotonicity scan.

Any input edit invalidates recorded output demand. Cache identity includes all
input versions, profile, metadata and parameters. Output name, dtype, rank-2
shape (including D=1) and empty facets are unchanged. Input zero/negative strides,
offsets and unaligned storage remain legal. Output storage survives its context.

The callback retains one row classification and fixed arithmetic workspace,
independent of N and D. It validates all rows, then reclassifies each row once
for all columns. Work is O(N+N*D*degree), plus exact arithmetic, complete input
collection and typed validation. Complete output costs b*N*D bytes. Admit that
output and fixed workspace even for one requested cell, together with collected
inputs/retained owners and metadata. Giant broadcast inputs/output can therefore
fail a small payload budget. No per-cell dependency certificates or full
coefficient table is retained.

Use the host worker and resource ledger; poll cancellation on reads, row controls,
inside exact arithmetic and before publication. Work/capacity failures preserve
ResourceExhausted and release unpublished state/output. Numeric failures have Run
scope, identifying offending port/index where available. Invalid segment/t is
InvalidArgument/InvalidDomain; used nonfinite controls are OperationFailed/
InvalidDomain; actual RN64 reconstruction or final conversion overflow is
OperationFailed/ArithmeticOverflow. Typed, upstream, stale, backend and
cancellation errors preserve their categories. Whole numeric counters are not
available; zero counters must not be interpreted as zero arithmetic/fallbacks.


Native Clang 21.1.3 strict/Apple public manual checks and 1428 independent
Bernstein/RN64 cases per profile passed for the Whole implementation. Focused
numeric/compiler tests passed. Other CPU platforms were not rerun. The giant
2^39-column/2^40-row cases validate complete-output budget rejection rather than
successful sparse numerical evaluation.

Native Apple M5 / macOS 27.0 (26A5425a), package 0.18.0 sampling uses K=2,
N=128, D=4, degree 2/3, Float64 inputs/output, indices all zero, t[i]=(i%64)/64,
anchors[0,c]=c and anchors[1,c]=c+1. Quadratic offsets are .5; cubic offsets
are .25 and -.25. An independent dyadic polynomial checks c+t (quadratic) or
c+.75*t+.75*t*t-.5*t*t*t (cubic) outside timing. One warm invocation precedes
seven measurements. One CPU worker, cache off, 1 GiB payload, 2 GiB Host,
512 MiB Metadata and dependency state, 2^40 dependency/Run work, default
unlimited managed work apply. The old adapter is source at 3d35f5eb linked to
the same kernel. Public timing excludes compile/freeze and includes execution,
collection, metering and result assembly. Core means the prepared complete-Value
numeric callback with allocation and publication but no scheduling/collection or
managed metering. Times are median [min,max] milliseconds.

| Degree | Profile | Public before | Public Whole | Numeric callback core |
| --- | --- | ---: | ---: | ---: |
| quadratic | strict | 14.542 [13.915,14.971] | 4.491 [4.446,4.595] | 4.309 [4.284,4.350] |
| quadratic | apple | 13.943 [13.780,14.672] | 4.410 [4.353,4.558] | 4.189 [4.089,4.314] |
| cubic | strict | 19.812 [19.624,19.997] | 9.592 [9.486,9.674] | 9.124 [9.042,9.292] |
| cubic | apple | 19.570 [19.045,20.265] | 9.183 [9.149,9.418] | 8.823 [8.638,9.035] |

Context-reported peak Metadata falls from 20,554,216 to 3,432 bytes. Whole
collection slightly increases peak payload: cubic 523,744 to 525,768 bytes.
A 12-second Apple cubic Time Profiler capture retains 11,879 execution-stack
samples: ParametricState::evaluate is inclusive in 98.75%, ExactPolynomial in
94.01%, ResourceBudget in 3.00%, ExactSampling in 0.35%, and collect in 0.07%.
Inclusive categories overlap; exact polynomial arithmetic is the remaining
observed bottleneck. Numerical bounds and the fixed workspace are unchanged;
no NUM-14 certificate or floating approximation is introduced.

Raw driver/build commands, ranges, trace/XML and analysis are local ignored
`build/crv-whole/parametric-*` artifacts, including `parametric-times.csv` and
`parametric-profile-summary.txt`. These results establish the stated native
workload, not arbitrary-shape or cross-platform speedups.


## CRV-04 public LUT1D authoring

`numeric/lut1d.hpp` expands six constructors into existing runtime nodes.
Expression/Bezier export one source's two outputs; interpolation exports the
interpolator values and Float64 linspace axis. The final table dtype does not
alter query generation. The constructor owns metadata only and appends after
successful local construction, reserving existing/supplied producer IDs and the
65536-node cap. It exports references through BakedLut1d and never edits the
caller's output labels implicitly. Source contracts supply all runtime resource,
precision, dependency, cache and failure semantics.

All expanded formal source outputs now use CPU Whole. A nonempty values request
computes the complete baked table and retains its full output owner, even when
only a few cells are returned. Interpolation bakes materialize the complete
Float64 linspace query array (8*count bytes) before the complete interpolation
output (count*C*dtype bytes). Account simultaneous source owners, query/table
buffers and each source's fixed workspace through the same host resource root.
There is no additional opaque bake primitive, cache, runtime function object or
pairing certificate.

Axis remains an independent Float64[3] output: axis-only does not execute the
expression coefficients, Bezier controls or interpolation x/y. Count=1 still
omits end payload. For count>1, even a request for only the first value collects
end and computes the full grid. Each source retains its mathematical formula,
selection/NaN/typed rules and numeric allowance; complete active input collection
can propagate previously unrequested upstream/typed failures. Numeric failures
have their source Whole Run scope. Any active source edit invalidates recorded
values demand; function controls never dirty axis. Empty reads no payload.
Output names, generic shapes/dtypes, C=1, explicit exports and post-context
immutable ownership remain unchanged. Cache-off retains active ownership.

Clang21 strict/Apple each pass 48 generated/explicit graph pairs and independent
analytic fixtures, complete dependencies/dirty mapping, native source selection,
cache/source failures, named exports/authoring limits, managed work/payload
budgets and active cancellation. Six-template signed/unaligned/scalar-zero
snapshot-import tests preserve logical values. A million-row sparse request
verifies full-output rejection under 1 MiB, not sparse success. Source numeric
and direct-layout oracles remain in their separately validated families.


Native sampling on Apple M5/macOS 27.0 (26A5425a), Clang 21.1.3 O2/debug,
package 0.18.0 uses all six public templates at count=129, Float64 inputs/output,
C=1 for scalar and C=2 for multi, with the control fixtures in baking.cpp.
Expression and quadratic Bezier implement x^2 on [0,1]; linear is 2*x on [0,2];
PCHIP uses x=[0,1,2], y=[0,1,4] on [0,2]; linear multi uses [2*x,10-2*x] on
[0,1]; PCHIP multi uses [P(x),4-P(x)] on [.5,1.5]. An independent dyadic
piecewise polynomial checks all values outside timing. Both values and axis are
requested. One worker, cache off, 1 GiB payload, 2 GiB Host, 512 MiB Metadata
and dependency state, 2^40 dependency/Run work, default unlimited managed work;
one warm invocation precedes seven samples. Compile/freeze is excluded.

The before adapter replaces only CRV-01/02 with their 3d35f5eb sources in the
same kernel. Expression was already Whole and has no implementation change;
its before/after differences are sampling variation. These templates have no
independent numerical callback. The current core column is the sum of the host's
monotonic expanded-source callback durations (microsecond resolution), including
numeric arithmetic, allocation and work metering but excluding input collection
and graph scheduling. This differs from the unmetered direct-core measurements
for individual source families. Times are median [min,max] milliseconds.

| Template | Profile | Public before | Public Whole | Expanded callback sum |
| --- | --- | ---: | ---: | ---: |
| expression | strict | 0.119 [0.100,0.176] | 0.105 [0.100,0.120] | 0.038 [0.037,0.044] |
| expression | apple | 0.095 [0.094,0.113] | 0.094 [0.091,0.113] | 0.034 [0.033,0.039] |
| Bezier | strict | 10.469 [10.269,13.890] | 5.398 [5.300,5.697] | 5.289 [5.197,5.547] |
| Bezier | apple | 10.260 [10.223,10.479] | 5.211 [5.184,5.293] | 5.117 [5.076,5.202] |
| linear | strict | 2.755 [2.703,2.788] | 0.814 [0.809,0.898] | 0.724 [0.703,0.773] |
| linear | apple | 2.771 [2.685,3.046] | 0.807 [0.796,0.883] | 0.709 [0.696,0.753] |
| PCHIP | strict | 12.639 [12.262,13.035] | 9.457 [9.419,9.693] | 9.324 [9.265,9.529] |
| PCHIP | apple | 12.268 [12.222,12.448] | 9.007 [8.924,9.236] | 8.883 [8.771,9.062] |
| linear multi | strict | 5.696 [5.458,5.829] | 1.449 [1.419,1.554] | 1.347 [1.324,1.436] |
| linear multi | apple | 5.434 [5.360,5.515] | 1.394 [1.367,1.454] | 1.286 [1.264,1.316] |
| PCHIP multi | strict | 25.563 [25.138,25.875] | 19.718 [19.122,20.369] | 19.541 [18.972,20.181] |
| PCHIP multi | apple | 24.633 [24.577,24.911] | 18.163 [18.019,18.697] | 17.997 [17.861,18.549] |

For multi PCHIP, context-reported peak Metadata falls from 8,219,352 to 4,376
bytes; reported payload changes from 290,176 to 290,024 bytes. These are managed
capacities, not RSS. The 12-second Apple multi-PCHIP Time Profiler trace retains
11,948 execution-stack samples, with ExactCurve::evaluate inclusive in 98.05%,
ResourceBudget in 6.05%, and collect in 0.06%; no DependencySession frame is
sampled. Inclusive categories overlap. Exact curve arithmetic remains the
observed bottleneck for this nonlinear Float64 template.

Ignored local raw artifacts are `build/crv-whole/baking-times.csv`,
`baking-perf.cpp`, its build script, `baking-after.trace`, exported XML and
`baking-profile-summary.txt`. Results cover these native fixtures, not an
all-shape/platform claim. No new primitive or numerical standard was introduced.


## CRV-05 dynamic-axis LUT1D

`UniformAxis` reconstructs each RN64 endpoint-weighted knot, verifies exact step,
strict ordering and singleton raw endpoints/+0. Scalar/channel lookup preserves
one-entry selections and exact whole-formula linear interpolation. Descending
pairs reorder both x/y to satisfy ExactCurve's positive-denominator internal
representation. Channel queries are classified independently.

Every nonempty request uses one CPU Whole callback over complete input, table
and axis Values, including recognized typed validation and upstream failures.
Validate the complete reconstructed axis first, then every finite/domain query
before table arithmetic. Compute every output element/channel and publish one
immutable dense owner with the complete input shape. Sparse demand restricts
returned coverage, not computation or full output memory. Empty reads no payload;
all static metadata checks still apply.

Mathematical table selection is unchanged: knot/clamp/singleton converts one
entry; interpolation/extrapolation uses its adjacent pair, independently per
channel. Generic entries outside all evaluated stencils receive no additional
finite scan. Typed validation and upstream collection cover the complete table.
Errors in otherwise-unrequested queries or evaluated channels can fail the Run.
Any input/table/axis edit invalidates recorded output demand. Cache identity
retains all input versions, profile, metadata and parameters. There is no
per-channel Atom success isolation. Output dtype, shape and empty facets remain
unchanged; arbitrary input offsets, unaligned and signed/zero strides remain
legal. The complete immutable owner survives context destruction.

For M total input elements, work is O(L+M log L) plus exact arithmetic, full
input collection and typed validation. Singleton lookup is constant time per
query. The callback retains a host-accounted Float64 grid of 8*L element bytes
(up to 8 MiB) plus allocator/metadata overhead, fixed exact workspace and O(rank)
coordinate state. It retains no per-output certificates, rows or lookup table.
Output costs dtype_bytes*M, regardless of requested coverage. Complete table
collection costs its full L (or L*C) logical payload when a dense collect is
needed; retained source owners are accounted separately.

The complete axis uses endpoint-weighted RN64 coordinates. One caller-preserving
floating environment covers axis reconstruction and curve evaluation; unresolved
accelerated bounds still use the same exact fallback. Work/cancellation is checked
in axis generation, input reads, lookup and exact arithmetic, and before publishing.
Resource failure never authorizes skipping axis validation or weaker arithmetic.
Unpublished output/workspace is released; no partial success is published.
Numeric InvalidDomain/ArithmeticOverflow failures have Run scope. Typed, upstream,
resource, stale, backend and cancellation failures retain their categories.
Whole does not expose per-value fallback counters; report those as unavailable.

Native strict/Apple each pass six public workflow groups and 1416 independent
Fraction grid/linear cases, covering complete output semantics, mixed dtypes,
Float32/64 fenv/layouts, typed input, cache/source failures and active arithmetic/
axis cancellation. The maximum L=1048576 grid retains an 8 MiB numeric index and
uses 16 MiB payload admission for full table collect. Giant channel output is
verified to reject an insufficient budget. All six public baking-to-LUT chains
verify their separate approximation and axis constraints. Other CPUs were not
rerun for this migration.


Native Apple M5/macOS 27.0 (26A5425a), Clang 21.1.3 O2/debug, package 0.18.0
sampling uses L=17, 512 query rows, C=1 for scalar or C=4 for channels, dense
input/table and Float64 axis=[0,16,1]. Table[j,c]=j+c and query[i,c]=
((i+c)%64)/4+1/8; an independent identity-line check verifies query+c outside
timing. Strict uses Float64 input/table/output; Apple uses Float32. One worker,
cache off, 1 GiB payload, 2 GiB Host, 512 MiB Metadata and dependency state,
2^40 dependency/Run work, default unlimited managed work; one warm invocation
precedes seven measured samples. The comparison adapter uses CRV-05 source at
3d35f5eb with the same kernel. Public timing excludes compile/freeze and includes
execution, collect, metering and result assembly. Core is the current prepared
complete-Value callback with allocation/lookup/arithmetic/publication but no
scheduler, collection or managed work metering. Times are median [min,max] ms.

| Variant | Profile/dtype | Public before | Public Whole | Numeric callback core |
| --- | --- | ---: | ---: | ---: |
| scalar | strict/Float64 | 10.060 [9.604,13.724] | 2.654 [2.619,2.774] | 2.427 [2.403,2.468] |
| scalar | apple/Float32 | 7.306 [7.211,7.412] | 0.145 [0.141,0.163] | 0.079 [0.075,0.087] |
| channels | strict/Float64 | 45.436 [44.719,48.061] | 10.491 [10.373,10.585] | 9.777 [9.671,10.225] |
| channels | apple/Float32 | 35.210 [33.301,35.548] | 0.449 [0.440,0.484] | 0.299 [0.290,0.307] |

Context-reported peak Metadata changes from 19,951,808/83,804,528 bytes
(scalar/channels) to 3,208 bytes; the comparison needs more than the default
16 MiB Metadata cap. Full input collect increases channel Apple payload from
295,176 to 303,624 bytes. These statistics exclude unmanaged memory/RSS.
The 12-second Apple channel Time Profiler trace retains 11,765 execution-stack
samples: ExactCurve::evaluate is inclusive in 29.23%, nextafter in 11.49%,
ResourceBudget in 17.44%, UniformAxis::validate in 0.13%, and collect in 1.06%.
No per-value fesetenv frame is sampled after guard reuse. Inclusive categories
overlap. Exclusive samples also identify strided address calculation and mutex
work, rather than dependency-description construction, among the remaining costs.
The mathematical exact/interval bounds are unchanged; no NUM-14 certificate is
used. This is evidence for the stated native workload, not all-shape speedups.

Raw driver/build commands, ranges, trace/XML and summaries remain in ignored
`build/crv-whole/lut1d-*`, including `lut1d-times.csv` and
`lut1d-profile-summary.txt`. Current focused numeric/compiler tests, ClangFormat21,
cpplint and scoped code/spec review passed.


## CRV-06 ColorArray and color ramps

The 15 independent primitives provide 45 CPU-profile keys. A four-poll
continuation reads all finite ordered stops, requested positions, then only
selected complete color rows before publishing owned packed ColorArray tuples.
The final channel axis is the semantic observation unit: compiler/root requests
touching any component close to the whole color, while generic NUM consumers
retain local Data and separately close typed Validation. Per-observation
dependency proofs cannot borrow another Atom's validation. Joint preflight checks
the union of declared roles before accepting complete typed transport.

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
Diagnostics distinguish `exact-linear-light` and `exact-rational-pi`. RGB counts
all component attempts before whole-color arithmetic, records copies only after
success, and preserves attempted counts on failure. Exact state, callbacks and
unpublished payload retire under work, capacity or cancellation failure.

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

Native Clang 21 Strict/Apple and Ubuntu WSL Clang 18.1.3 Strict/AVX2 passed
1784 independent Fraction/Machin-pi and 352 RGB rational-root/Decimal cases per
profile. These include sRGB golden bits and join neighbors, exact integer-gamma
cancellation, extreme stops, tiny alpha with HDR premultiplied storage, subnormal
output and a separately bounded huge-gamma example. ColorArray's 1847 exact
metadata cases and ICC's 75 structural/MD5/SHA cases passed on both architectures.
The public manuals inspect ownership/closure and workflow behavior, with commands
in the [example README](../../../examples/numeric_workflow/README.md).
These sampled cases and scoped proofs do not establish successful evaluation of
every legal input under the fixed work/precision capacities. No integration test
or new CTest registration is added; WSL supplies correctness evidence only.

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
Only positive weights enter the compact vertex list used in the later table
Need. Rebuilding weights from immutable cells/queries avoids a large per-point
integer cache while preserving exactly the declared support.

The complete component numerator is `sum(W_v*C_v)`, where each finite source
float is an integer in `2^-1074` units. One ratio rounding produces the output
dtype. Differences need fewer than 2100 bits, denominator/weights fewer than
6300, and weighted sums fewer than 8400; final rounding alignment stays below
8500. The existing 40960-bit polynomial arena admits these bounds. The actual
peak is at most 48 of 96 slots, including descending-axis normalization and
per-component accumulation. Original contributing bits determine all-negative-
zero results; other exact cancellations are +0 and nonzero underflow retains
its sign. No floating local coordinate or intermediate blend is rounded.

The four-poll continuation reads global axes, complete requested input colors,
then exactly contributing table colors, and publishes complete packed results.
It allocates no full logical table/output for sparse demand. Per-Need certificate
reservation is `4096+32768*M` Metadata bytes in addition to managed vectors,
grid, continuation, source windows and output owners. Dense requests can exceed
host metadata/work budgets independently of output payload size. Work and
cancellation checks run inside support construction as well as final sums.
All profiles use the same exact arithmetic with their selected integer compare
helpers, so no numerical fallback is attempted.

The independent Fraction oracle reconstructs RN64 grids, selects cells and
simplexes and rounds complete rational sums using separate Python integer IEEE
logic. Its 1062 cases cover all eight models, source/output dtype combinations,
all axis directions, unequal extents, split ties and cube boundaries, tiny and
extreme values, negative-zero rules and zero-weight invalid vertices. Five public
manual groups additionally inspect actual support/dirty/cache effects, maximum
`256^3` scalar-backed table and `2^38`-position composition, typed strides/fenv,
work/state/stage limits, support/final-sum interruption, per-color Atom failures
and required upstream producer order. Executable commands are in the
[public example](../../../examples/numeric_workflow/README.md#joint-three-axis-color-luts).
No integration or CTest entry is added, and WSL is a correctness environment.

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
a path from batch values. Accelerated keys report `FunctionUnsupported` scalar
fallback exactly when general strict interval evaluation begins, including an
attempt subsequently interrupted by the host. No claimed SIMD throughput or
WSL performance result follows from this correctness path.

The three-poll log continuation requests shared scalar Control/Validation,
then local input Data/Validation, then publishes immutable generic fragments.
Typed input closure can read the full containing color while output remains a
scalar observation. Bounds errors precede input special values. The state uses
host-owned fixed arithmetic, vectors and publication owners; Need reservation
is `4096+16384*M` metadata bytes. Each certificate and extended arithmetic loop
consumes host work. Empty is completed by the host without entering the state.

The public manual and 4196-case Fraction/directed MPFR reference cover both
dtypes, four forms, exact roots and midpoint ties, near-equal and extreme
bounds, raw IEEE special values, under/overflow and monotonic clusters. Five
manual groups additionally inspect partition/cache/dirty behavior, all-port
strides and fenv, ColorArray closure, Empty, inverse -0, state/work/stage limits,
refinement cancellation, fallback diagnostics and escaped owners. See the
[commands and editable workflow](../../../examples/numeric_workflow/README.md#scalar-coordinate-shapers).

## CRV-09 measured LUT3D baking

`bake_lut3d` stages an ordinary WorkflowDocument, invokes the authoring source
builder for the grid and validation shapes and validates both through Compiler
metadata analysis. Prefix identity protects existing nodes, declarations, layout
origins and exports. Source output shape/dtype and attached color descriptions
are checked; optional ICC ResourceBindings resolve unrelated/shared typed inputs.
A SHA-256 recipe identity frames the expanded source document, designated endpoints
and compiler semantic identity. The helper retains no executable callback.
Both graph expansions execute under the final plan's one frozen binding snapshot.
Source pointwise independence remains the caller's assertion.

Generated axes reuse `UniformAxis`; centers reuse exact endpoint averaging with
one RN64 conversion. Every grid color is an explicit global report dependency,
including when a constant source ignores its generated input. Color validation
checks source and converted table samples, then a CompleteBundle sampled-table
Result owns converted colors with their recipe/schema. Unpack feeds that object
to the existing CRV-07 method with explicit `table_dtype` and reject policy.
The report associates the actual sampled-table ObjectId. Gate requires an exactly
matching schema and that object association; it cannot reuse a passed report for
another same-shaped table. It copies only requested complete-color table regions
while retaining global report support.

Per-component error is an exact nonnegative integer in `2^-2148` units. Opposite
signs add magnitudes; equal signs subtract them. The threshold is
`atol+rtol*abs(reference)` in the same units. Error needs fewer than 3173 bits;
the threshold/product needs fewer than 4197, within the 4352-bit workspace.
Selection compares exact integers, replacing maxima only on strict increase;
ordinal zero initializes every component. Report rounding happens afterward:
RN64 the maximum, compare that encoded value back to the exact integer and
increment one positive IEEE step if it rounded downward. Unrepresentable error
is diagnostic +Inf. The helper was independently checked against 432 Fraction
cases including ±MAX, subnormals, opposite signs and exact tolerance boundaries.

Measurement enumerates row-major centers followed by extras, at most 64 colors
per Value request. It stores only fixed maxima/first-failure/counts. Eleven fields
are appended through explicit Result I/O actions and sealed together; no prefix
can claim completed measurement. Each field and the descriptor have global
Cartesian support across axis, grid, owned table, points, source and applied
colors. Result schema specialization preserves protocol-2 kind/id/version and
validates all fields/domain/metadata through the closed schema vocabulary.
The canonical resolved schema already participates in graph/plan/cache identity.
No C++ record layout, C ABI or workflow framing version changes are required.

The sampled table is packed through at most 4096-byte staging buffers and bounded
read windows, including nonzero global window origins. Its registered validator
scans all colors with cancellation/work accounting. The report validator checks
canonical schema, counts/classifications, reconstructed axis and recorded point
domains without recomputing the source. Grid and requested Value outputs have
host-owned continuation/publication metadata; storage aliases retain admission
past context retirement. Report/table owners retain mandatory temporary backing.
Source callback state never enters execution. Source operation allocations and
repeated validation count against host budgets; generated full-grid validation
currently admits the full grid. Large dense bakes can exceed work, stage or
capacity limits despite a small requested exported fragment.

Seven public manual groups and a 480-case independent Fraction workflow oracle
cover numerical reports, object/recipe association, both methods/dtypes and eight
models, repeated extras, rounded centers, casts, shared snapshots, ICC authoring,
malformed data, cancellation, resource limits and multiwindow/partial ownership.
The executable stays outside integration tests and CTest. Commands and modifiable
source builders are in the [example](../../../examples/numeric_workflow/README.md#measured-three-dimensional-lut-baking).

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

`curve_inverse.cpp` stages global x/y Control+Validation, local query
Control+Validation, and selected pair/stencil Data+Validation before publication.
It retains at most 16K bytes of promoted global inputs and requested-output state;
no allocation scales with unrequested N. All x/y changes invalidate every dependent
observation, while query changes are pointwise. Dynamic topology errors and
requested finite/overflow failures carry the dependent Atom; host/source failures
retain their categories. Accelerated Float32 inverse queries use bracketed
hardware interval bisection, publishing only when both bracket endpoints round
to the same Float32 output. Float64, ambiguous comparisons and unresolved rounds
use the exact inverse; actual fallback is reported. Exact collinear stencils
bypass lattice search, while noncollinear refinement retains scalar wide-integer
comparison. The internal profile is scoped and restored on all exits. This yields
the same monotone mapping independent of request partitions.

The [public inverse workflow](../../../examples/numeric_workflow/README.md#inverse-curves)
contains four manual groups and a 407-case independent Fraction reference that
bisects real x with normalized Hermite evaluation. Native Clang21 Strict/Apple
and WSL Clang18 Strict/AVX2 passed both, and the installed 0.16 consumer passed
all four groups on both native profiles. The shared
forward interpolation and LUT1D regressions passed 2487 and 1416 cases respectively
on both native profiles. The focused compiler unit passed. No integration test
was registered or run for this feature.

## CRV-11 resampling and certified lowpass

The four resampling helpers append existing CRV-01 interpolation and independent
`core.identity` nodes. Identity preserves new-position dtype, facets and raw
special bits; only sample demand adds curve validation. Exports remain caller
controlled and IDs reserve declared/referenced nodes. No new interpolation
primitive, implicit filtering or sample-rate inference is introduced.

Accelerated uniform lowpass builds certified coefficient enclosures once per
continuation at 128 fractional bits, retaining them in a resource-accounted vector.
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
coordinate combinations fit below 2102 bits; the common 12288-bit records and
ResourceVector maps are admitted under host capacity. Global positions and local
value endpoints receive separate Control/Data and typed Validation certificates.
Output publication metadata is admitted before constructing mutable fragments,
including on failed/cancelled arithmetic paths.

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
zero. Nonuniform accelerated keys still report FunctionUnsupported strict fallback.
Every scale scan, partition piece, coefficient/refinement and limb operation has
work/cancellation checks. No integration tests or external math dependency enter
the product. MPFR/Fraction are independent manual references only.

Public examples, exact fixtures, response checks and runtime commands are in
[resampling](../../../examples/numeric_workflow/README.md#signal-resampling),
[uniform lowpass](../../../examples/numeric_workflow/README.md#uniform-lowpass)
and [nonuniform lowpass](../../../examples/numeric_workflow/README.md#nonuniform-lowpass).

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
