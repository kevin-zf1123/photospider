# NUM-03 Whole execution

All six `numeric.constant` and `numeric.broadcast` profile keys execute CPU
Whole Result operations. Constant View copies one scalar into an independent
zero-stride Result owner; it preserves the whole-array tuple identity without
retaining an oversized scalar backing. Broadcast View preserves the complete
source Result owner, including its original strides. If the source has multiple
owners, View returns `ViewUnavailable`; Dense can collect those owners. Both
Dense operations publish the complete target before consumer projection. An
Empty request reads no samples, while nonempty requests validate all active
source data. A source edit invalidates the complete output observations.

Constant Dense grows an already-filled prefix, then copies blocks of at most
64 KiB without overlap. Cancellation polls between these byte-bounded blocks.
Broadcast Dense retains coordinate mapping and 32-byte Scalar/NEON/AVX2 copying
with exact tails. These are raw bit operations, so exact bit equality applies
to all three profiles, including sNaN payloads, signed zero and integers.
No NUM-14 certificate or floating approximation is involved.

## Public workflow and validation

The current Result behavior is covered by `test_numeric_result_arrays`:

```sh
cmake --build build/kernel-dev --target test_numeric_result_arrays -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_arrays$' --output-on-failure
```

The focused test covers dense output and schema rejection, structured consumption
of giant views, Result cache rebinding, owner lifetime, raw bit patterns and
constant/broadcast boundary cases. It checks all 256 UInt8 values, Int64
extrema, Float32/64 special values, all four floating-point rounding modes, and
negative unaligned axis permutations. A structured input with one batch axis
of extent 2 and cell shape `{3}` verifies that the consumer reads the last
sample using batch coordinate 1 and cell coordinate 2.
Strict and available accelerated profiles run; on the tested Apple Silicon host,
the x86 profile reports `BackendUnavailable`. These checks establish correctness
and ownership behavior, not a current Result performance baseline.

The `photospider_numeric_arrays` executable composes the same Result operations
with the remaining NUM-03 workflow cases. Its full manual run is separate from
the focused CTest above; the test command is the targeted validation entry point.

## Historical Value Whole latency and copy core

These measurements describe the earlier Value-backed Whole implementation and
its Value adapter at package 0.18 / traits 16. They do not measure the current
Result implementation. The recorded host was Apple M5, macOS 27.0 (26A5425a),
Clang 21.1.3, O2/RelWithDebInfo, with `-fno-fast-math -ffp-contract=off`.
The Before column uses the original NUM-03 adapter with the same kernel;
the Whole column uses the earlier Value Whole path.
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

| Operation/layout, N=16384 | Before Value adapter | Value Whole public | Apple copy core | Scalar copy core median |
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
