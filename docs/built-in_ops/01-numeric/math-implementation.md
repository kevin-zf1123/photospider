# Numeric mathematical implementation

This document records the implemented numerical machinery for NUM-04 and its
shared NUM-05 facilities. Specification acceptance remains independent: the
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
