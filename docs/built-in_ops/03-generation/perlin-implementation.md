# Perlin 2002 strict execution

The default registry contains `noise.perlin2002_3d_v1_strict_cpu_whole`,
`noise.perlin2002_3d_v1_strict_cpu_tiled`, and
`noise.perlin2002_3d_v1_strict_gpu`. All three implement [NOI-04A](op_specs/NOI-04A_perlin2002_3d.md)'s fixed permutation and complete exact polynomial, with one final IEEE rounding. The GPU-only entry selects MSL/Metal or SPIR-V/Vulkan from the active GPU service and rejects an unavailable backend without CPU fallback.

Input is one generic Float32/64 tensor `coordinates[S...,3]`. Output port
`values` has shape `S...`, rank 1..7. The optional static String parameter
`dtype` is `float32` or `float64`, default `float64`. Coordinates are finite;
frequency, coordinate construction and units belong to upstream nodes.
Whole reads and validates the complete input and dirties the complete output
when any input sample changes. Returned results own their storage after the
execution context is destroyed. No color interpretation is inferred.

The host assigns CPU ranges with a worker quota. Each active slot owns a fixed
integer workspace; no plugin-created threads or allocation within block callbacks
are used. Source address calculation supports legal negative/broadcast strides,
unaligned storage offsets and logical origins. External workflow bindings retain
the kernel's dense binding contract; strided Values can arrive from producers
or the public direct registry invocation.

Tiled uses Dependency-v1 with an exact static map: each requested observation
reads and validates its three coordinate components. Source edits dirty only
their corresponding observations; nonfinite samples outside the requested region
are not read. Each tile callback stays on one host thread. Normal `execute`
collects bounded tile results; eligible CPU `execute_stream` graphs schedule a
bounded window concurrently on the shared host pool and deliver tiles in order.
Frozen caches/checkpoints, joint operations, structured outputs, and mixed GPU
graphs do not use this concurrent window. Whole itself remains independently
multithreaded.

The tile allocates a 72-byte decoded coordinate record per requested sample, then evaluates the exact polynomial. Both passes use canonical box order and row-major order within each box. Direct sessions accept multiple disjoint boxes. Scratch, output, continuation and publication metadata have managed owners; allocation/work failures return no successful partial result.

The tile makes three bounded scalar reads per sample, one for each coordinate component. Let `N` be the requested sample count, `r` the input rank (`output rank + 1`), `coverage_boxes` the authorized box count, and `fragments` the supplied input-fragment count. The read bound per scalar is

$$
R=2+4r+(coverage\_boxes+fragments)(r+1),\qquad C_{read}=3NR.
$$

A separate index/decode allowance is `N * (3 * r + 3)`. The callback forms the combined bound in 128-bit arithmetic, checks that it fits in `uint64_t`, and prepays it before reading. The implementation computes the same terms as follows:

```cpp
const auto input_rank = static_cast<unsigned __int128>(output_rank) + 1;
const auto coverage_and_fragments =
    static_cast<unsigned __int128>(coverage_boxes) + fragments;
const auto lookup_work = coverage_and_fragments * (input_rank + 1) +
                         4 * input_rank + 2;
const auto lookup_total = 3 * static_cast<unsigned __int128>(count) * lookup_work;
const auto total = lookup_total + count * (3 * input_rank + 3);
```

It passes each bounded read a local credit for `C_read`; the read still enforces the supplied footprint limits and phase cancellation. The callback polls phase cancellation and currentness every 64 samples and checks cancellation for each sample. After coordinate decoding, it prepays the exact limb-tier arithmetic bound separately and debits local credit during calculation.

For the 1-D tiled benchmark, input rank 2 with one coverage box and one fragment gives 16 units per scalar read; three reads add 48 units per sample. At 16,384 samples, that is 786,432 units, matching the issued-work increase from 63,084,919 to 63,871,351.

## Mathematics and bounds

IEEE bit decoding obtains exact floor modulo 256 and dyadic fractions. With
common denominator D=2^q, each fade numerator has denominator D^5. Three fade
weights and a corner dot share denominator D^16. The eight signed terms are
summed exactly. The conservative magnitude bound 16*D^16 needs at most
16q+5 bits, so 8/16/272 uint64 limbs cover q<=31/63/1074. Every multiplication
uses distinct output scratch; negative gradients use sign/magnitude arithmetic.
Integer lattice values are +0; a negative nonzero result underflowing to zero
retains -0. Floating input conversion and intermediate floating arithmetic are
not involved.

Output and all slot storage use the managed allocator. Arithmetic work consumes
the resource root installed for each host block. Address/decode work is charged
before reads. Each sample then prepays a conservative bound determined by q and
its selected limb tier; local credit is checked before each decrement and unused
credit is not refunded. Cancellation checks remain at every multiplication row.
This reservation may reject a finite work budget earlier than actual-limb
accounting, while never performing unadmitted arithmetic. Publication occurs only after every block retires
successfully. The declared workspace currently reserves the maximum 64 slots;
quota-sensitive admission and construction costs remain optimization work.

## Native GPU arithmetic and ownership

GPU has explicit Whole demand/dirty semantics. Host admission decodes the finite
input bits to establish the maximum fractional denominator q and device work
bound. The shader independently decodes coordinates and performs the complete
fade, gradient and eight-corner weighted polynomial. CPU code does not compute
output values. Float64 values are transported as bits; no device floating-point
intermediate is used.

Device scratch contains 17 little-endian base-2^32 integers per active lane.
The same `16*q+5` bit bound admits 16, 32 or 544 words for q<=31, q<=63 or
q<=1074. Each schoolbook product accumulates at most
`(2^32-1)^2 + 2*(2^32-1) = 2^64-1` in a uint64 value. Sign/magnitude accumulation
preserves exact cancellation. Direct guard/sticky/parity extraction from the
integer numerator implements final ties-to-even, subnormal transitions and
signed underflow without a floating conversion.

The fast tiers dispatch at most 256 samples per dispatch and group at most eight
sequential dispatches into one synchronous submission. The full tier uses four
samples and one dispatch per submission. Every lane owns disjoint scratch, reused only after synchronous
device completion. Their maximum scratch sizes are 278528, 557056 and 147968
bytes. Complete output has a separate managed owner. Host scans and every device
batch prepay bounded work; cancellation stops later submissions and publication,
then in-flight commands drain before owners release. Work bounds count modeled
integer operations, not measured device instructions.

Both admission and shader support validated offset, signed strides, logical
origins, padding and unaligned input bytes. Unsigned affine address arithmetic
computes the exact expression modulo 2^64; Value validation proves the final
address is inside the buffer, so cancellation of large intermediate terms does
not require a device int128. Same-device producers retain their native buffers;
CPU/device boundaries use the kernel's explicit transfers. Output remains owned
after the execution context retires.

The Vulkan path compiles `perlin.slang` with Slang 2026.18.2 as SPIR-V 1.5,
validates the module against Vulkan 1.2, and checks descriptor bindings, the
432-byte std140 argument layout, the 64-thread local size, and required device
features. The shader needs 8-bit storage-buffer support as well as 64-bit
integer arithmetic; a device without the required byte-storage features reports
`BackendUnavailable`. The host selects MSL or SPIR-V from
`ps_gpu_service_v11::backend`. `ExecutionMode::NativeGpu` selects placement,
while Perlin retains its Float32/Float64 result contract using integer
arithmetic on either backend.

FreeBSD native Vulkan execution is verified on Intel UHD Graphics 770.
NVIDIA Vulkan execution for this Perlin port and Linux GPU execution remain
untested. The available 64-sample Intel run is an initial timing baseline, not
an optimization result; broader Vulkan performance measurements remain open.

## Reproduction and evidence

The maintained [public workflow example](../../../examples/perlin_workflow/README.md)
executes both registrations; [integration tests](../../../tests/integration/test_perlin_workflow.cpp)
cover additional contracts:
`WorkflowDocument` input binding, `Compiler`, `ExecutionContext` and named output
all use public APIs. At `(1/4,0,0)` the exact result is `0.146484375`.

```sh
cmake --build build/kernel-dev --target test_perlin_exact test_perlin_workflow test_perlin_tiled test_perlin_gpu test_cpu_tiles -j 8
ctest --test-dir build/kernel-dev -R '^test_(perlin_(exact|workflow|tiled|gpu)|cpu_tiles)$' --output-on-failure
python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_workflow
python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_workflow --tiled
python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_gpu
```

macOS validates 1/4/16 host workers, analytic periodic samples, default/explicit
dtype, nonfinite rejection, work exhaustion inside arithmetic, pre-cancellation,
strided public direct invocation and result ownership. The arithmetic unit test
covers q-tier boundaries and scratch reuse after cancellation. The independent
Fraction comparison passes 1566 actual public-workflow output bit checks.
This is finite correctness evidence, not completion of the nine-implementation
performance task. Native FreeBSD passes the focused tests and the 1566 bit comparisons separately
for Whole and tiled. Tiled tests cover an uneven rectangular ROI, exact source
reads, ignored outside-ROI NaN, multi-box Float32 requests, managed scratch
exhaustion, finite work failure and inside-ROI nonfinite rejection. macOS TSAN
passes the scheduler and Perlin tile tests. Scheduler tests force overlap within
one Run and cover shared-pool simultaneous Runs, cancellation/failure/staleness,
ordered publication, multi-node DAGs and indivisible trailing-axis tuples.
Native Metal passes the 1566 Fraction/IEEE comparisons through actual dispatch,
plus GPU-only admission, multi-batch execution, subnormal coordinates, nonfinite
rejection, finite work exhaustion, pre-cancellation, cancellation after the first
real submission cohort with zero retained payload, and same-device strided input
with retained results. The Intel UHD 770 FreeBSD Vulkan workflow also passes the
1566-case Fraction/IEEE comparison and focused GPU tests, including a 201-byte
input whose negative-stride final read reaches the last valid byte, plus the
2,057-sample mixed-dtype case covering q=1074, cancellation, work exhaustion and
no-fallback behavior. The CPU and Metal paths retain their existing math contract.
Every successful GPU test requires native dispatches and no fallback. Broader
Vulkan performance coverage remains pending; raw results and native GPU timing
are in `out/gpu-whole-tiled/PERLIN_GPU_PERFORMANCE.md`.

The initial 64-sample Intel Vulkan benchmark records median 3.99121 ms, p95
4.04772 ms, one native dispatch and one submission, with output bits equal to the
Whole reference. It is a baseline only, not evidence of an improvement. See
[`perlin-vulkan-intel-first.log`](../../../out/gpu-whole-tiled/raw/perlin-vulkan-intel-first.log).

## Measured budget-admission optimization

The budget and tiled timing values in this section describe their recorded benchmark builds. The tiled issued-work figures predate the bounded scalar-read accounting described above and are not current-code work totals.

`test_perlin_workflow --benchmark` constructs deterministic Float64 triples from
`((i*1709+719)%131071)/65536-1`, with 16384 output samples. It performs one warmup
and five measured calls for each of 1/4/8 host workers, checks output bytes against
the one-worker result, and reports execute-only latency. Registry/context creation,
compilation, input construction, root-statistics collection and result comparison
are outside the timer. No profiler or build overlaps these timing runs.

On Apple M5/macOS 27.2, the initial per-row global budget updates measured
27.0622/226.403/432.474 ms median for 1/4/8 workers. A separate native `sample`
trace locates the contention in `ResourceBudget::try_consume` called from the
inner multiply rows. Per-sample precharge measures 17.5412/5.03512/4.18125 ms,
with identical output bytes. The root's issued work rises from 62,844,998 to
63,045,632 because the bound includes unused conservative credit. Peak modeled
host reservation is 3,196,560 bytes for each worker count. This case demonstrates
an actual accounting-contention fix; it does not cover all denominator tiers,
request sizes or sustained mixed-workload throughput.

Raw timings and the separate profiler trace are under
`out/gpu-whole-tiled/raw/perlin-whole-macos-{initial,precharge}.jsonl` and
`perlin-whole-initial.sample.txt`. Sample counts include blocked stacks and are
not CPU percentages. The exact source formula for prepaid work is
`PerlinExact::work_bound`; its maximum is 3,432,111 units per sample at q=1074.


On native FreeBSD 15.1/i9-12900, pinned to the same logical CPU set
`0,2,4,6,8,10,12,14`, initial 1/4/8-worker medians are
47.4365/63.9797/70.0652 ms. The precharge implementation measures
39.3109/10.1356/5.19177 ms. Output bytes, declared work and modeled peak match
the corresponding macOS workload. Timing files are
`out/gpu-whole-tiled/raw/perlin-whole-freebsd-{initial,precharge}.jsonl`.
Fully instrumented macOS TSAN passes both the arithmetic and public workflow
tests without a race report; raw test logs are in `raw/perlin-tsan-tests.txt`.

## Measured tiled scheduling and traversal

The public example times execution and copying the complete result into the
same client-owned byte array for both modes. Setup, compilation and comparison
with a separately executed one-worker Whole reference are outside the timer.
Each trial uses one warmup and five measured calls. Tile size 128 at 16384
samples produces 128 real computation callbacks.

| Platform | Workers | Per-sample traversal/charging, ms | Tile predecode/precharge, ms |
| --- | ---: | ---: | ---: |
| M5/macOS 27.2 | 1 | 29.3731 | 27.5539 |
| M5/macOS 27.2 | 4 | 19.2420 | 10.4321 |
| M5/macOS 27.2 | 8 | 28.7132 | 10.6401 |
| i9-12900/FreeBSD 15.1, pinned as above | 1 | 84.2646 | 47.6296 |
| i9-12900/FreeBSD 15.1, pinned as above | 4 | 18.6298 | 14.6644 |
| i9-12900/FreeBSD 15.1, pinned as above | 8 | 19.8955 | 9.76525 |

All outputs match Whole bytes. Issued work remains 63,084,919. On macOS, modeled
peak host reservation for 1/4/8 workers is 481672/701096/928128 bytes after
predecode, versus 481672/626976/869608 before it. A separate native sample trace
contains shared `DependencySession::Impl::consume` locking and per-point visitor
allocation in the original hot path; blocked-stack counts are not CPU shares.
For a single 128-sample tile, measured 8-worker latency changes from 0.240125 to
0.256375 ms, so the large-input gains do not establish a small-request gain.

In a separate earlier M5 measurement, tile widths 256/512/1024 at 16384 samples gave
4-worker medians 8.36079/7.28896/6.77254 ms and 8-worker medians
6.63575/5.53517/4.61108 ms. The 8-worker modeled peaks grow to
935656/1205456/1785552 bytes. This is a latency/capacity tradeoff, not evidence
that the largest tile is optimal for all workloads. The example keeps tile
width explicit. Raw data are `out/gpu-whole-tiled/raw/perlin-tiled-*.jsonl`;
the separate trace is `perlin-tiled-initial.sample.txt`.
