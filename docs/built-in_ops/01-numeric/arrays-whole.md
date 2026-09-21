# NUM-03 Whole execution

All six formal constant/broadcast profile keys now execute CPU Whole callbacks.
Constant View copies one scalar into an independent zero-stride owner; it retains
its whole-array tuple identity and does not keep an oversized scalar source
backing. Broadcast View preserves one complete source Value, including original
strides and storage. Multiple source owners return ViewUnavailable; Dense may
collect them. Both Dense modes compute and own the complete target before
consumer projection. Empty reads nothing, all active source data is validated,
and any source edit invalidates the selected output's complete observations.

Constant Dense grows an already-filled prefix, then copies blocks of at most
64 KiB without overlap. Cancellation polls between these byte-bounded blocks.
Broadcast Dense retains coordinate mapping and 32-byte Scalar/NEON/AVX2 copying
with exact tails. These are raw bit operations, so exact bit equality applies
to all three profiles, including sNaN payloads, signed zero and integers.
No NUM-14 certificate or floating approximation is involved. Whole numeric
per-atom counters are unavailable.

## Public workflow and validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_arrays -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_arrays _strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_arrays _accelerated_apple_silicon
```

Both runs passed. Independent checks cover all 256 UInt8 values, Int64 extrema,
Float32/64 special/raw bit patterns, all four fenv modes, negative unaligned
axis permutations, ordinary/direct/fragment execution, and exact analytic
broadcast coordinates. `[1048576,1048576]` constant still owns only 8 bytes;
`[274877906944,3]` broadcast retains three Int64 samples. Structured consumption
of giant views, escaped-owner lifetime, scalar-copy source release, NaN cache
identity, Whole dirty/cache invalidation, multi-owner View rejection/Dense
collection, full typed rejection and Empty all passed. Both dense operators
reject insufficient full-output/work budgets and release storage on cancellation
after admitted copying. Existing generic staged allocation/metadata probes remain
in the manual suite and passed separately from the migrated operators.

Five focused numeric/dependency/demand/resources/compiler CTests passed, as did
ClangFormat21/cpplint and scoped independent review. No x86/WSL runtime validation
was performed for this migration. Dated older platform records are pre-Whole.

## Public latency and copy core

Apple M5, macOS27.0 (26A5425a), Clang21.1.3, O2/RelWithDebInfo,
`-fno-fast-math -ffp-contract=off`, package0.18/traits16. Before uses the original
NUM-03 adapter from 18459d2d linked to the same kernel; after uses Whole.
Inputs are frozen before timing, one worker, result/dependency caches off,
1 GiB controlled payload limit, 2^40 work and 512 MiB dependency-state limit.
One warmup and seven measured calls per row, every logical output checked
against raw scalar bits or independent `input[column]` outside timing.

Constant is Int64 scalar=7, output `[N]`. Broadcast is Int64 `[16]` values
0..15, output `[N/16,16]`, map `[1]`. The separately measured copy core is the
actual callback with prepared metadata and prebuilt input, including output
allocation/layout construction, excluding public scheduling/collection and
managed work-ledger overhead. It is not arithmetic-only CPU time.

Milliseconds, median [min,max]:

| Operation/layout, N=16384 | Before public | Whole public | Apple copy core | Scalar copy core median |
| --- | --- | --- | --- | --- |
| constant/View | .0901 [.0819,.1438] | .0443 [.0284,.0616] | .000375 [.000250,.001750] | .000292 |
| constant/Dense | .4693 [.4448,.5239] | .0446 [.0352,.0634] | .005250 [.004333,.007792] | .005375 |
| broadcast/View | .0897 [.0798,.1170] | .0323 [.0296,.0517] | .000292 [.000250,.000583] | .000375 |
| broadcast/Dense | 2.708 [2.637,2.860] | .3740 [.3502,.4364] | .2255 [.2202,.2301] | .2312 |

N=16/256 and all Scalar min/max ranges are retained in the raw CSV. Whole
constant View adds 8 controlled payload bytes; broadcast View adds none but
retains its 128-byte externally supplied source backing. The diagnostic peak
therefore does not describe total retained/RSS memory. At N=16384, Dense
controlled peaks are 131080 bytes for constant and 131200 for broadcast.
Partial Dense requests now require the same complete output ownership.

A 12-second broadcast Dense N=16384 Time Profiler capture contains 11811
execution-chain samples: 9179 (77.72%) include execute_broadcast, 2025 (17.15%)
include ResourceBudget::consume, 492 (4.17%) include byte_address, and 48
(0.41%) include collect. Coordinate/copy and metering dominate the observed
path; inclusive counts overlap.

A separate 12-second constant Dense N=1048576 capture contains 11682 execution
samples. 6809 leaf samples are memmove, mostly beneath the callback dispatcher;
3319 are bzero, including 3309 under MutableValue::allocate. Only 83 (0.71%)
include collect. Inspection of BufferAllocator::allocate confirms value-initialized
byte allocation; the remaining clear-before-overwrite cost belongs to that shared
allocator contract. It was not changed in this operator migration. No claim
assigns every memmove sample to a specific fill or collector instruction.

Raw local files are `build/num-whole-remaining/arrays-timings.csv`,
`arrays-perf.cpp`, `arrays-core.cpp`, `build-arrays-perf.py`, strict/Apple manual
logs, both arrays `.trace` captures, their exported XML/sample JSON and summaries.
