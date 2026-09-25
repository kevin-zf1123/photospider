# NUM-01 Whole execution

`sample_expression_node` still authors the three formal
`numeric.sample_expression_{profile}` keys. Compilation prepares one immutable
AST; CPU Whole invocations reuse its sealed owner. Values collect all active
scalars once, evaluate all count samples and publish one complete packed output.
Axis independently evaluates only the interval, retaining its Atomic tuple.
Count=1 statically excludes end; axis excludes all coefficients. Legacy
`numeric.sample_expression` remains a different operator and was not migrated.

Whole values failure has Domain/Run scope, no Atom, and retains the failing
global sample, x and source span. Any invalid coordinate/expression in the full
domain fails a partial consumer request. Active inputs invalidate all selected
output observations. Empty does no payload work. Values retain count*4/8 bytes;
axis retains 24 bytes. On ARM64 the actual values scratch is 240816 bytes and
axis scratch is 1144 bytes; the common traits admission reserves the larger
workspace for either output. Owners release at their last reference, while
failure/cancellation retires unpublished output and scratch.

The RN64 coordinate and step rules, adjacent-coordinate checks, postorder strict
AST evaluation and one final conversion remain. Accelerated evaluation preserves
the final FP32-scaled four-ULP contract with strict replay of uncertified samples.
It is not a bitwise-exact claim for all accelerated expressions. Scalar,
Apple/NEON and x86/AVX2 keys remain; NUM-14's 8uA certificate is not used.

## Public validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_expression -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_expression strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_expression apple
python3 examples/numeric_workflow/expression_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_expression strict
python3 examples/numeric_workflow/expression_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_expression apple
```

Strict and Apple passed 715 independent coordinate/stepwise Fraction/MPFR
4.2.0-p12 cases each. The oracle evaluates all count samples before projecting
requested indices, preserving an independent reference for Whole failures.
Public examples verify ascending/descending grids, mixed named coefficients,
compile-once dynamic replacement, count=1 failed end exclusion, axis-only failed
coefficient exclusion, interval validation for constant expressions, first
failing subtree spans, Float32 conversion, tail/partition identity, all four
rounding modes with unaligned negative-stride scalars, and fenv restoration on
success and numeric failure. Work/output/scratch limits, pre-cancel, cancellation
after admitted arithmetic, unpublished-owner release and retry also pass.

Five focused numeric/dependency/demand/resources/compiler CTests passed.
The strict curve, Bezier and parametric public examples passed after the shared
sampling helper changed; callers that do not hold an outer floating environment
retain the existing scoped environment. ClangFormat21/cpplint passed. No x86
runtime validation was performed for this migration.

## Public latency and numerical core

Apple M5, macOS 27.0 (26A5425a), Clang21.1.3, O2/RelWithDebInfo,
`-fno-fast-math -ffp-contract=off`; Apple profile, Float64, start=0/end=1,
N=65536. Both adapters link the same package0.17/traits15 kernel; before adapter
and public driver are from 8996f526. One CPU worker, cache off, 32 MiB payload,
2^50 work limits, compiled/frozen inputs before timing. One warmup plus seven
samples; public values are checked at seven independently generated checkpoints.
The core driver calls actual coordinate and AST engines with preallocated
storage and no public execution/collection. Core checkpoint checks use the same
independent Fraction/MPFR constants; full numerical correctness is separately
gated by the oracle above.

Times are milliseconds, public median (maximum); the existing public driver
does not retain individual samples/minimum. Core is median [minimum,maximum].

| Expression / requested indices | Before public | Whole public | Core, all N |
| --- | --- | --- | --- |
| `2*x+1`, all | 27.483 (28.015) | 5.037 (5.411) | 3.248 [3.222,3.520] |
| `2*x+1`, three | .146 (.180) | 5.269 (5.505) | same full-domain work |
| `exp(x)`, all | 27.905 (28.298) | 6.397 (6.469) | 4.790 [4.743,4.910] |
| `exp(x)`, three | .136 (.144) | 6.398 (6.439) | same full-domain work |

Whole improves these full-output cases and makes sparse requests slower.
Controlled peak payload changes from 240880 to 765120 bytes for three-point
ROI and from 765144 to 765120 for full output. The operation timing now records
one callback instead of Need/Evaluate's two. Public per-atom numeric counters
are unavailable, represented by zero fields in the existing CSV rather than
claimed zero mathematical work.

## Observed bottleneck and optimization

A first 12-second Whole Time Profiler capture had 11953 execution-chain samples;
6312 leaf samples were `fegetenv/fesetenv`, 5727 beneath coordinate weighting.
Whole now establishes the floating environment once for the callback and tells
its coordinate/accelerated AST helpers to reuse that scope. Other helper callers
retain the default scoped behavior. This does not change mathematical rounding
or fallback rules. Both numerical oracles and success/failure fenv checks passed
after this change. Before this adjustment, Whole full-output medians were
10.365 ms (`2*x+1`) and 11.209 ms (`exp(x)`).

The second 12-second capture has 11814 execution-chain samples: zero fenv leaf
samples, 4220 (35.72%) including coordinate weighting, 5461 (46.22%) including
accelerated AST evaluation, 5 (0.042%) including collect, and zero preparation
samples. `nextafter` appears in 1678 (14.20%) inclusive samples. Inclusive stacks
overlap; these are sampled observations, not an assertion that fenv work never
occurs. No replacement of the interval bounds or attribution of all memmove
samples to input collection is claimed.

Raw local files are under `build/num-whole-remaining/`: expression before/after
and core CSVs, `expression-core.cpp`, `expression-profile.cpp`, both `.trace`
captures and exported XML/sample JSON, oracle/manual logs and focused test logs.
The public `benchmark_quick` command reproduces N=65536 full/three-point timing;
other sizes in `benchmark` are not claimed as measured for this change.

## Exp backend update (2026-09-24)

Direct SLEEF exp has been removed. The accelerated AST rejects `Exp` from its
fast interval evaluator and replays that sample through the original strict
RN64 AST evaluator, preserving input precision and success/failure behavior.
The NUM-04 IQK exp certificate only covers binary32 primitive arguments; it is
not an enclosure for these binary64 intermediates. Expressions containing exp
can therefore be substantially slower than the historical timings above.
Other admitted mathematical functions retain their SLEEF paths.
