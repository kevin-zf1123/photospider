# NUM-11 Whole execution

All21 formal reduction keys use Whole. Numeric reducers collect/validate complete
input, compute all keepdims groups and publish complete dense output before
consumer projection. Any active source edit invalidates every recorded output
observation. Unrequested group overflow or upstream/typed failure affects the
Run; no partial successful groups are published. Empty reads nothing. Numerical
group order, dtype domains, ddof, first-NaN payload mapping and exact final
rounding/root rules are unchanged. Unsuffixed mean/variance remain distinct
legacy implementations.

Count has an empty static runtime-input projection. It validates metadata only,
skips source producers and owns one8-byte zero-stride complete output. Numeric
reducers reuse one4048-byte exact workspace per callback (this arm64 build),
reset before each group; full output and collected input storage are additional.
Rank<=8 coordinate vectors are bounded host containers. The former64-sample
streaming memory guarantee has been replaced by full-input storage. Exact limb
work and every input are metered/cancellable; no per-group descriptor, staged
publication or numeric atom diagnostic remains.

## Public workflow and validation

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_reductions -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_reductions strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_reductions apple
python3 examples/numeric_workflow/reduction_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_reductions strict
python3 examples/numeric_workflow/reduction_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_reductions apple
```

Both profiles pass all public fixtures and4740 independent Fraction/midpoint-square
oracle cases each. On[[1,2,3],[4,5,6]], axes1 produces sum[6,15], min[1,4],
max[3,6], mean[2,5], count[3,3], variance[RN(2/3),RN(2/3)] and
std[RN(sqrt(2/3)),RN(sqrt(2/3))]. Four rounding modes, exact integer overflow
cancellation, dtype conversion, subnormal/extreme/NaN/Inf/zero cases, multiple
axes and exact root boundaries are covered.

Whole fixtures exercise a4096-value RegionalSource with complete ordered input,
2^40 logical count with2^20 outputs on an8-byte owner and zero failed-producer
calls, unrequested group overflow, all-recorded-output dirty propagation,
full typed rejection (Count remains payload-free), Empty, pre-cancel, ddof,
strided logical NaN priority, unaligned negative/zero strides, work/full-output/
scratch failure and cancellation after admitted arithmetic with ownership cleanup.
Required later source failure cannot be suppressed by a NaN. Five focused
numeric/dependency/demand/resources/compiler CTests, format/lint and scoped
read-only review pass. No x86 runtime test in this migration; older regional
WSL/installed records are not current Whole acceptance.

## Public latency and exact core

Apple M5/macOS27.0 (26A5425a), Clang21.1.3 O2/RelWithDebInfo,
-fno-fast-math -ffp-contract=off, package0.18/traits16. Before adapter from483888c8
linked to the same current kernel; this isolates adapter behavior. One worker,
result/dependency caches off,1 GiB payload,2^40 dependency work,512 MiB dependency
state. Compile/freeze precedes timing; one warmup plus seven measured calls.
Every result is checked outside timing. Float64 input[4,N/4] repeats0,1,2,3;
axis1, Float64 numerical outputs and Int64 count, ddof0. Each group sum is
1.5*N/4, mean1.5, min0, max3, variance1.25, std RN(sqrt(1.25)).

Core invokes the actual callback with prepared metadata and prebuilt input,
including output/workspace allocation, grouping and arithmetic, excluding public
scheduling/collection and managed work-ledger overhead. Accelerated results
retain their own contract; this migration adds no NUM-14 certificate. Scalar
and platform exact limbs remain, and NUM-14 Accelerate/SME comparisons are untouched.

Milliseconds median [min,max], N=16384:

| Operation | Before public | Whole public | Apple callback core | Scalar core median |
| --- | --- | --- | --- | --- |
| sum |10.2019 [10.0645,10.5703]|1.4464 [1.4330,1.4650]|1.2350 [1.1808,1.2972]|1.2391|
| minimum |9.4563 [9.1555,9.9980]|.5823 [.5628,.5904]|.3969 [.3945,.4129]|.3739|
| maximum |9.2947 [9.0740,9.9615]|.5578 [.5510,.6290]|.4025 [.3964,.4187]|.4058|
| mean |10.7454 [10.2437,11.1065]|1.4989 [1.4135,1.6455]|1.2350 [1.2225,1.2506]|1.2295|
| count |.0804 [.0772,.1078]|.0329 [.0295,.0648]|.000375 [.000334,.001667]|.000417|
| variance |11.8831 [11.7301,12.4280]|3.2991 [3.1992,3.3291]|2.6865 [2.5985,2.7345]|2.7290|
| std |12.0718 [11.8338,12.6387]|3.2903 [3.2725,3.3460]|2.7428 [2.6993,2.7856]|2.8883|

N=256 and all Scalar sample ranges are retained in raw CSV. Numerical controlled
payload peak rises from4200 to135152 bytes at N=16384 (full collected input,
4048-byte state and32-byte output). Count decreases from64 to8 bytes. External
source backing/RSS is not represented by these peaks. Partial numeric output
still incurs complete input/group computation and storage; sparse requests can
regress.

A12-second variance capture has11972 execution-chain samples:11639 include
execute_reduction,10341 ExactMoments,10311 ExactAggregate,720
ResourceBudget::consume and12 collect. Inclusive counts overlap. Largest leaves
are FixedInteger<68>::add4735, set_product2597 and set940; exact accumulation
and product construction dominate this fixture. Final multiply_fixed appears in
12 samples and round in2. The remaining cost is directly observed in the exact
core, without inferring an I/O bottleneck.

Raw local files: build/num-whole-remaining/reductions-timings.csv,
reductions-perf.cpp/build-reductions-perf.py, reductions-whole.trace, exported
XML/sample JSON/profile summary, public/oracle logs and reductions-focused.log.
