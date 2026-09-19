# Numeric mathematical implementation

This document records the implemented numerical machinery for numeric and curve
operators and their shared facilities. Specification acceptance remains independent: the
operator specifications retain their Proposed status. Numerical correctness is
the selected discrete mathematical result, including raw special-value rules;
it is not a claim that every input succeeds under every resource budget.

## Exact elementary paths

`exact_elementary.hpp` performs integer transforms in signed 128-bit scratch,
checking the final UInt8/Int64 range before publication. Float sign, NaN and
integral rounding operations use unsigned IEEE fields. Addition/subtraction,
multiplication and division use the existing 4352-bit dyadic/ratio accumulator;
sqrt uses exact squared-midpoint comparisons. No Float32 NaN passes through a
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

No approximate transcendental backend is selected. Ordinary transcendental
requests on either accelerated profile enter the strict scalar interval engine
and report one `FunctionUnsupported` strict fallback. Special values and exact
algebraic landmarks do not report a fallback. The profile still performs its
explicit ISA publication path; exact elementary/root work uses its corresponding
limb comparison path. These implementation facts imply no speedup guarantee.

Diagnostics identify `photospider.math/1`, the operation, exact or interval
algorithm, compiler/OS/build identity and actual selected profile. Evaluations
are admitted before arithmetic; copied values are counted before their fixed
publication store. Both counts and an already attempted fallback survive a later
resource or cancellation failure. Cache hits add no fabricated arithmetic work.

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
fixed correctly rounded binary64 bits. Every primitive rounds to binary64 in
specified left-to-right postorder. Final Float32 conversion is a separate
rounding boundary. NUM-02 exact endpoint interpolation supplies coordinates;
NUM-04/05 exact elementary and certified interval mathematics supply primitives.
No compiler reassociation, host libm or floating environment defines the result.

Decimal scratch uses 256 limbs (16384 bits): a 4096-digit significand needs
at most 13607 bits, and the supported decimal-exponent branch has at most
14680 denominator bits. Larger exponents are classified as overflow or rounded
zero only after accounting for all source digits. Division alignment and final
midpoint comparison fit the remaining capacity. Runtime exact/math workspace
is part of the admitted continuation; work and cancellation checks occur in
AST evaluation and inside long arithmetic/refinement. Bounded unresolved
transcendental refinement returns ResourceExhausted.

Pure static preparation owns the program across compiler stages, outputs and
dynamic runs. Sealed handles validate registry/definition identity, all static
input metadata and exact parameter bits, including signed zeros and NaNs.
Direct preflight prepares once per call, or reuses an explicit matching handle;
a joint request prepares once for compatible members. Static plan/program
allocations use ordinary host storage outside runtime managed scratch. They
have source/program size bounds, but no separately enforced preparation budget
or global preparation cache. Session retirement destroys the continuation
before its program; the program is destroyed before the definition/library
lease. Package 0.15 requires a C++ rebuild; traits/framing 14 and C ABI 9 remain.

At most 16 inputs use compact static mappings and two regional polls: acquire
the shared scalars, then evaluate only the requested coordinates. Larger
coefficient sets use staged reads of at most 16 ports. Their exact per-Atom
certificates can require a larger box budget; the 24-coefficient fixture uses
512. Axis-only execution does not allocate the expression evaluator, read
coefficients or check adjacent value coordinates. Count=1 never reads end.
An ordinary failed batch publishes no partial Value; `execute_atoms` retains
independent successes. Coordinator metadata-admission failure now returns
ResourceExhausted and retires unpublished state rather than escaping bad_alloc.

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
keys. All use exact integer rational formulas followed by one direct RN-even
conversion. Strict, Apple and AVX2 currently agree bitwise, including PCHIP;
no approximate slope or per-batch clipping is used. Integer comparisons and
publication select scalar, NEON or AVX2 paths and diagnostics identify the path.
No strict fallback is needed for this exact implementation.

Finite binary64 values are integers in units 2^-1074. Differences need fewer
than 2099 bits; PCHIP slope numerator/denominator magnitudes are below 2^6300.
The combined Hermite numerator is below 2^20999 and denominator below 2^18897.
The 352-limb (22528-bit) workspace covers these bounds plus denominator
alignment, 53 quotient trials and midpoint doubling. The fixed 96-slot arena
uses at most 49 live slots in the formula; every slot and ratio temporary is
part of the admitted continuation. Endpoint limits compare exact cross-products.
Direct node/clamp conversion, two-point linear degeneration and exterior
tangent evaluation have smaller bounds. Arithmetic polls work/cancellation
inside limb multiplication and final division. Independent scoped arithmetic
review also compared 3208 Fraction formula expansions successfully.

The regional continuation acquires complete x Control/Validation, then the
projection of Q onto query rows, then only column-local y Data/Validation.
All x must be finite and increasing. Exact node/clamp reads one y, linear reads
two, PCHIP reads its fixed local stencil even when a slope branch is zero.
Query classification runs once per distinct requested row. Bounded rectangle
projection and association construction add metadata work; no whole N*C array
or full slope table is allocated. Immutable packed fragments retain correct
global origins and owners. The public examples set explicit session/Run work
budgets; a default direct invoke can exhaust its discovery budget on a small
PCHIP batch. That failure preserves ResourceExhausted rather than weakening
precision or demand.

On 2026-09-19, native Apple M5 / Clang 21 strict and Apple, and Intel Core
i9-12900 Ubuntu WSL / Clang 18.1.3 strict and AVX2, passed 2484 independent
Fraction cases per profile. These cover mixed source/output types, all domain
policies, random irregular knots, limiter branches, extreme finite arithmetic,
midpoints, subnormals, signed zeros and ordered adjacent-query shape checks.
All four profiles passed the five public manual groups: basic single/multi
fixtures and tangent extrapolation; sparse support/dirty/Atom isolation and
lifetime; strides/fenv/schema/Empty/resource interruption; cache reselection,
upstream order and giant sparse public composition; typed Mask validation.
Local installed consumers, focused compiler unit, ClangFormat 21/cpplint and
scoped entry/math reviews passed. No new CTest or integration entry was added.

The following Apple M5 / Clang 21 RelWithDebInfo native measurements use x=0..16,
y[j,c]=j+c, query[i]=i/4+1/8, K=17, N=1/64, C=1 for single and 2 for multi,
Float64, Whole, one worker, cache off, three repetitions. Compile/freeze precede
timing; synchronous execution and result assembly are timed. Every returned
sample is checked against query[i]+c, with four polls, N*C evaluations and zero
fallbacks. Session and Run work limits are 8 Gi and 16 Gi work units. Columns
show median/max microseconds; these measurements do not establish a general
speedup claim. WSL supplies correctness evidence only.

| Operation | N | Strict median/max us | Apple median/max us |
| --- | ---: | ---: | ---: |
| linear | 1 | 208/666 | 205/294 |
| linear | 64 | 2561/2577 | 2535/2646 |
| linear_multi | 1 | 208/244 | 208/215 |
| linear_multi | 64 | 5159/5359 | 4755/5061 |
| pchip | 1 | 224/246 | 230/252 |
| pchip | 64 | 7042/7278 | 6660/7034 |
| pchip_multi | 1 | 356/399 | 337/345 |
| pchip_multi | 64 | 13849/13917 | 13777/14245 |

Peak controlled payload is 285944/286448 bytes for single and 285952/286960
bytes for multi at N=1/64. The diagnostic does not include ordinary static
plan storage or unmanaged host containers. The separate giant composition
checks one last-column result over a logical [2,2^39] constant view, with actual
value 7; it is a sparse correctness fixture, not a large dense performance test.


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
all inside the admitted continuation. Initial coefficients have fewer than
2105 bits. The worst degree-three integer remainder chain grows through
4211, 10529 and fewer than 25280 bits. Horner evaluation at the 8192 fractional
bit refinement cap stays below 26683 bits; final rounding temporaries fit the
same arena. The static maximum live-slot bound is 44, including temporary
GCD calls. Explicit work/cancellation runs inside long arithmetic; unresolved
refinement or storage limits return ResourceExhausted/CapacityLimit. The solver
has a finite budget, not a promise that every algebraic input resolves at any
caller-selected budget. All profiles use this exact numerical path, so no
numerical fallback is reported; profile-specific integer helpers supply the
scalar/NEON/AVX2 operations. Diagnostics record the actual build/profile.

The four-poll values protocol obtains sampling inputs, global X
Control/Validation, then local X Data and selected Y Data/Validation, and
finally publishes immutable packed fragments. Axis has its own two-poll state
without the polynomial arena and ignores all controls. N=1 also ignores end.
Topology is validated once per values continuation and searched per requested
sample; it is not a shared persistent index across independent Atom runs.
Knot/clamp paths read only one y. Every interior control remains required.
`strict_math_calls` counts each attempted Bx sign evaluation in refinement,
including failed calls, excluding topology/interval/GCD work and cache hits.

On 2026-09-20, Apple M5 Clang 21 strict/Apple and Ubuntu WSL i9-12900 Clang
18.1.3 strict/AVX2 passed five public manual groups and 386 independent
Fraction/de Casteljau/rational Euclid-Sturm cases per profile. They cover RN64
reconstruction, inverse rather than direct-t semantics, derivative topology,
midpoints/subnormals/zero signs, selected-component errors, sparse maximum
count, source/typed errors, strides/fenv, cache/Atom isolation and resource
interruption/recovery. Private independent arithmetic probes compared 1758
common-root cases and 419 inverse cases; review independently checked another
226 topology and output-boundary cases and found/fixed the signed half-subnormal
zero issue. Installed 0.15 consumers, focused compiler unit, ClangFormat 21,
cpplint and final scoped math/entry reviews passed. Shared NUM-01 sampling
extraction also passed 715 cases/profile plus its seven manual groups on native
and WSL Clang. No integration/CTest entry was added.

Native Apple M5/Clang 21 RelWithDebInfo timing below uses one worker, Float64,
cache off and three repetitions after compile/freeze. The identity curves have
anchors (j,j), quadratic offsets (.5,.5), or cubic outgoing (.25,.25) and
incoming (-.25,-.25); the sampling interval is [0,K-1]. Every returned value
matches the independently rounded exact grid coordinate. The full 48-row
matrix per profile covers degrees 2/3, K=2/64/4096/65536, N=256/65536/1048576
and Whole/ROI {0,N/2,N-1}. There are 32 successful rows and 16 dense resource
failures per profile. WSL is not used for performance comparisons.

Budgets are 32 MiB controlled payload, default managed capacity (16 MiB
Metadata), default maximum_boxes=65536, 64 Mi footprint work, 64 Gi session
work and 128 Gi Run work. Every Need reserves 4096+16384*M metadata bytes;
full N=65536 exceeds that capacity and N=1048576 exceeds the association
lower bound before per-point staging. Raising only the payload budget cannot
make those dense requests complete. Successful N=256 Whole rows peak around
14 MiB Metadata, while three-point ROI stays below 1.8 MiB even at K=65536.
The payload peak is 523808 bytes for Whole and 519760 for ROI; separately
accounted topology capacity contributes to Host/Metadata. Statistics describe
managed capacities and exclude unmanaged allocation/RSS. Source counts are
unique support coordinates; direct bound inputs do not provide physical read
call telemetry. CSV root_calls and issued_work give distinct computation counts.

| Degree | K | N | Region | Strict median/max us | Apple median/max us |
| --- | ---: | ---: | --- | ---: | ---: |
| 2 | 2 | 256 | Whole | 223783/227461 | 208945/274228 |
| 2 | 2 | 1048576 | ROI3 | 883/922 | 811/1138 |
| 2 | 65536 | 256 | Whole | 375821/385161 | 373107/379647 |
| 2 | 65536 | 1048576 | ROI3 | 269675/269694 | 253764/253853 |
| 3 | 2 | 256 | Whole | 356167/358147 | 334603/350061 |
| 3 | 2 | 1048576 | ROI3 | 1569/1574 | 1367/1373 |
| 3 | 65536 | 256 | Whole | 802690/808716 | 740536/741101 |
| 3 | 65536 | 1048576 | ROI3 | 710799/722415 | 656185/658154 |

The separate stress command uses cubic x=t^3 at t=2^-10 (q=2^-30),
y=.75*t+.75*t^2-.5*t^3, and a rational root t=1/12 with control y values
[-17,49,98,742]*2^-1074 whose exact result is -2^-1075. The latter rounds to
-0. Strict median/max are 508/1057 us and 460/499 us; Apple 495/622 us and
452/480 us. They require 10 and 8 Bx sign attempts respectively, one evaluated
value and zero fallbacks, with 519720 payload bytes. These small measurements
do not establish a general accelerated speedup.


## CRV-03 parametric Bezier evaluation

`parametric_bezier.cpp` uses the existing ExactSampling and ExactPolynomial
facilities without an inverse/topology stage. Demanded controls widen exactly,
relative handles reconstruct with RN64, and exact Bernstein coefficients are
converted to power form. Horner uses integer t*2^1074 and denominator
2^(actual_degree*1074), retaining coefficient units 2^-1075. Trimming the degree
also changes the denominator; constant/zero polynomials use denominator one.
The exact numerator has fewer than 5329 bits for degree<=3 and t in [0,1];
rounding temporaries need under approximately 5330 bits and at most 16 live
arena slots. The existing 40960-bit/96-slot continuation admits those bounds
without allocating private arithmetic state. Interior exact zero tests raw
reconstructed-control zero signs; a nonzero underflow keeps its exact sign.
Endpoint conversion bypasses all other controls. Finite output obeys the
correctly converted control convex hull, including extended infinite bounds.

The three-poll regional protocol projects Q onto query rows, reads/validates
segment indices and t once per row, then declares local component control
Data plus recognized typed Validation. Image handles close Validation across
all channels while preserving scalar Data. Each output cell retains its own
certificate; transport deduplicates source coordinates, and no global K/D/N
scan or full output allocation is used for sparse demand. Explicit metadata
reservation per Need is 4096+16384*M bytes in addition to managed row/point
vectors, output fragments, publication owners and exact continuation scratch.
Dense association/resource exhaustion is an explicit failure. Work checks
cover reads, row/component loops, reconstruction, limb arithmetic and publication.

On 2026-09-20 native Apple M5 Clang 21 strict/Apple and Ubuntu WSL i9-12900
Clang 18.1.3 strict/AVX2 passed 1428 independent Fraction Bernstein cases per
profile. The oracle reconstructs controls with its own integer IEEE rounding
and uses direct Bernstein weights instead of production power-Horner. Cases
include mixed types, unordered/repeated segments, partial components, wide
exponents, subnormal and near-one t, selected/unselected invalid data, RN64
midpoints, signed zeros and Float32 finite cancellation across extreme controls.
Five public manual groups passed on each profile: analytic and reconstruction
fixtures; sparse dependency/dirty and Atom isolation; strides/fenv/Empty/schema
and resource interruption; cache/large sparse composition and typed closure;
upstream ordering. The public constant compositions check 2^39 columns and
2^40 rows with tiny actual requests. Independent review checked 96 additional
Fraction Bernstein/power expansions and closed the endpoint-overflow diagnostic
fix using a t=1 regression whose unneeded handles are NaN. Installed 0.15
consumers, focused compiler unit, formatting/lint and scoped math/entry reviews
passed. No new CTest/integration registration or performance claim was added.


## CRV-04 public LUT1D authoring

`numeric/lut1d.hpp` expands six constructors into existing runtime nodes.
Expression/Bezier export one source's two outputs; interpolation exports the
interpolator values and Float64 linspace axis. The final table dtype does not
alter query generation. The constructor owns metadata only and appends after
successful local construction, reserving existing/supplied producer IDs and the
65536-node cap. It exports references through BakedLut1d and never edits the
caller's output labels implicitly. Source contracts supply all runtime resource,
precision, dependency, cache and failure semantics.

On 2026-09-20 native Apple M5 Clang 21 strict/Apple and Ubuntu WSL i9-12900
Clang 18.1.3 strict/AVX2 passed the five baking manual groups. Each profile
compares 48 generated/explicit graph pairs across six sources, two dtypes and
four demand modes; checked results include source/dirty witnesses and analytic
values. Additional tests cover mixed scalar types, N=1 failing end, axis-only
failing function sources, source-specific equal-endpoint and first-coordinate
behavior, cached reversed bindings, million-row sparse multi PCHIP under 1 MiB
payload, C=1 and multiple named bakes, reference/UINT64_MAX IDs and partial
construction rejection. Runtime cancellation coverage here is pre-cancellation;
resource recovery checks create a new context. Underlying source clusters cover
arithmetic-time interruption and owner retirement. Returned results are checked
after each helper execution context is destroyed. Installed 0.15 consumers,
ClangFormat 21/cpplint and independent authoring/acceptance reviews passed.
CRV-05 below covers all six downstream application chains and the expected
discretization error; graph equivalence alone is not a proof of approximation quality. No new
runtime primitive, integration-test registration or performance claim is added.


## CRV-05 dynamic-axis LUT1D

`UniformAxis` validates finite start/end/step, singleton raw endpoint/+0 rules,
RN64 derived step, and every rounded endpoint-weighted coordinate in table order.
It retains an admitted ResourceVector of 8L bytes; no table value is needed for
that validation. The four-poll LUT continuation reads shared axis, then exactly
requested query coordinates, then selected table singleton/pairs and publishes
all requested packed fragments. Descending lookup compares reversed numerical
order keys; before exact linear evaluation it reverses both coordinates and
values to meet ExactCurve's positive-denominator precondition. This is the
same mathematical negative-denominator formula. Direct hits/clamps/singletons
convert only the selected entry; other paths preserve both endpoint witnesses
and apply the whole-formula zero rule. Profile-specific integer helpers retain
identical results. ExactCurve now clears its borrowed work callback on every
return/exception; native CRV-01 five groups and 2484 Fraction cases regressed
successfully after that lifetime-only change.

Query Control remains per scalar; recognized Image Validation closes channels
separately. Channel tables do not share a query/index merely because a row prefix
matches. Numeric errors carry complete global scalar Atom coordinates up to rank
8. Full axis is revalidated per admitted continuation, with no persistent shared
grid cache. Work is O(L+M log L) plus exact sampling/interpolation and association
work. The default direct session work budget can be exhausted by a small grid;
public fixtures explicitly allow 32 Gi session/64 Gi Run work units. At L=1048576,
8 MiB of admitted grid capacity fits the default Metadata budget for a tiny Q.
Per-stage certificate reservation is 4096+16384*M bytes, so output payload alone
does not bound large dense requests. All output/point/grid/exact scratch and
publication owners use host admission; resource failure never skips axis checks.

On 2026-09-20 native Apple M5 Clang 21 strict/Apple and Ubuntu WSL i9-12900
Clang 18.1.3 strict/AVX2 passed 1416 independent Fraction grid/linear cases per
profile and six manual groups. The oracle independently reconstructs RN64 knots
and exact signed-denominator interpolation, then applies integer IEEE rounding.
Coverage includes mixed dtypes, both orders/all domain modes/singletons, extreme
cancellation, mismatched/overflowed/underflowed steps, collapsed coordinates,
subnormal queries, direct and mixed/extrapolated zero signs, rank-eight queries
and selected versus remote invalid entries. The manual path checks exact source
support/dirty, channel Atom isolation, cache changes to all three inputs, all-port
negative/unaligned/zero strides and fenv, typed Image closure, source failure
ordering, axis-loop and arithmetic/second-box cancellation, work/state/stage
admission and unpublished Payload retirement. It executes a complete maximum-L
grid using one scalar-backed public constant table and a sparse 2^39-channel
constant composition. All six baking constructors feed their consumer, with
explicit discrete expected values and rejection of repeated-grid axes.
Installed 0.15 consumers, focused compiler unit, ClangFormat 21/cpplint and
independent math/entry/oracle reviews passed. No new CTest/integration entry or
performance claim was added; WSL results establish correctness only.

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
retain their categories. Current accelerated cubic queries explicitly report
FunctionUnsupported strict fallback; linear, K=2 and selected paths are exact
without that fallback. This yields a monotone mapping independent of partitions.

The [public inverse workflow](../../../examples/numeric_workflow/README.md#inverse-curves)
contains four manual groups and a 404-case independent Fraction reference that
bisects real x with normalized Hermite evaluation. Native Clang21 Strict/Apple
and WSL Clang18 Strict/AVX2 passed both, and the installed 0.16 consumer passed
all four groups on both native profiles. The shared
forward interpolation and LUT1D regressions passed 2484 and 1416 cases respectively
on both native profiles. The focused compiler unit passed. No integration test
was registered or run for this feature.

## CRV-11 resampling and certified lowpass

The four resampling helpers append existing CRV-01 interpolation and independent
`core.identity` nodes. Identity preserves new-position dtype, facets and raw
special bits; only sample demand adds curve validation. Exports remain caller
controlled and IDs reserve declared/referenced nodes. No new interpolation
primitive, implicit filtering or sample-rate inference is introduced.

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
zero. Current accelerated keys report FunctionUnsupported strict fallback.
Every scale scan, partition piece, coefficient/refinement and limb operation has
work/cancellation checks. No integration tests or external math dependency enter
the product. MPFR/Fraction are independent manual references only.

Public examples, exact fixtures, response checks and runtime commands are in
[resampling](../../../examples/numeric_workflow/README.md#signal-resampling),
[uniform lowpass](../../../examples/numeric_workflow/README.md#uniform-lowpass)
and [nonuniform lowpass](../../../examples/numeric_workflow/README.md#nonuniform-lowpass).
