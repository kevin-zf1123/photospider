# FreeBSD Clang 22 numeric performance, 2026-09-25

Follow-up: the [FreeBSD/WSL diagnosis](freebsd-wsl-diagnosis.md) reproduced a
hybrid-CPU HWP package-limit issue on this host. These original timings were
recorded at approximately 2.922 GHz; removing the low package cap reached
4.737 GHz and eliminated most of the WSL difference. Preserve this limitation
when interpreting the original absolute times.

This experiment tests the current certified SIMD implementation on `freebsdpc`,
including the uncommitted trigonometric update following `6786cb93`. The
comparison baseline is the complete `6786cb93` batch implementation, built with
its own kernel archive. It uses the same benchmark source, inputs and execution
settings. See the [trig report](trig-performance.md) for algorithms, analytic
certificates and earlier macOS/Linux measurements.

## Explicit compiler and standard library

Host: FreeBSD 15.1-RELEASE amd64, Intel Core i9-12900, 24 logical CPUs,
68,444,450,816 bytes reported physical memory. AVX2 and FMA admission passed.
Both builds explicitly select `/usr/local/bin/clang22` and
`/usr/local/bin/clang++22`, version 22.1.7. System `/usr/bin/clang` is 19.1.7;
no global compiler alias or system library was changed.

Compiler selection alone was insufficient: Clang 22 initially used the base
system libc++ headers, whose charconv header has no floating `from_chars`.
Both trees failed at the same existing expression-literal parser. To preserve
the source implementation, LLVM's `llvmorg-22.1.7` libc++, libc++abi and
libunwind were built and installed only under the experiment directory.
A probe verified `_LIBCPP_VERSION=220107` and floating `from_chars("1.25")`;
`ldd` verified both benchmark executables load these local runtime libraries.
No parser or numeric production source was patched for FreeBSD.

Both builds use RelWithDebInfo, the same headers/libraries and identical compiler
flags. The private numeric object has `-O3 -fno-fast-math -ffp-contract=off
-mavx2 -mfma`; the remaining C++ compilation uses -O2 and the existing strict
floating options. The relevant configuration is:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DCMAKE_C_COMPILER=/usr/local/bin/clang22 \
  -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++22 \
  '-DCMAKE_CXX_FLAGS=-nostdinc++ -isystem /home/alex/photospider-trig-20260925/runtime/include/c++/v1' \
  '-DCMAKE_EXE_LINKER_FLAGS=-L/home/alex/photospider-trig-20260925/runtime/lib -Wl,-rpath,/home/alex/photospider-trig-20260925/runtime/lib'
```

The isolated runtime was configured from `llvm-project/runtimes` with
`LLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind`, Clang 22, Release, tests and
benchmarks disabled, and the local runtime install prefix. Both libcxxabi and
its LLVM unwinder are enabled. Source, configure/build logs and installed files
remain in the experiment directory for reproduction.

## Accuracy and execution checks

Both baseline and current passed 4,742 MPFR cases for each of sin, cos, sinpi,
cospi and sinc, plus 10,333 for sincpi, each through six batch partitions.
Current maximum ordered distances were sin=1, cos=1, sinpi=2, cospi=1, sinc=1,
sincpi=0. Baseline observed distance was zero for this corpus. These are sample
observations; the current analytic four-step certificate remains the guarantee.

Current additionally passed the 20,503-case exp corpus (six partitions, maximum
2 steps), 7,524 public unary oracle cases with MPFR 4.2.2, the generic math
batch fixture, and `test_numeric_operations` / `test_expression_operations`.
Fixtures include negative/unaligned/zero strides, four host rounding modes,
preexisting exception flags, scalar/batch equivalence, work/capacity failure,
cancellation and owner release. No full release or installed-consumer matrix
was run.

## Timings

All measurements use `cpuset -l 2`, one public worker, result cache disabled,
seed 404, one warmup and seven measured iterations. Public compilation/freeze
and output validation remain outside timing. Ordinary trig spans [-1,1];
sinpi/cospi span [-1/4,1/4]. Baseline runs before current, with no concurrent
build or profiler. Raw/core/public results cover N=4,096, 262,144 and 1,048,576.
The public memory/work settings match `trig_measure.py` and the earlier report.

Public medians at N=1,048,576, milliseconds:

| Function | Batch baseline | Polynomial | Speedup | Current core | Current raw |
| --- | ---: | ---: | ---: | ---: | ---: |
| sin | 121.671 | 8.015 | 15.18x | 7.554 | 0.246 |
| cos | 113.407 | 8.071 | 14.05x | 7.570 | 0.278 |
| sinpi | 420.971 | 8.033 | 52.41x | 7.565 | 0.242 |
| cospi | 414.983 | 8.004 | 51.85x | 7.545 | 0.245 |
| sinc | 368.360 | 7.986 | 46.13x | 7.523 | 0.238 |
| sincpi | 501.787 | 9.713 | 51.66x | 9.246 | 2.937 |

Wide sincpi, N=262,144, span [-1024,1024]: 119.289 ms baseline versus
2.494 ms current (47.82x).

The maintained IQK exp implementation, span [-10,10], measured:

| N | Public ms | Core ms | Raw ms |
| ---: | ---: | ---: | ---: |
| 262144 | 1.738 | 1.612 | 0.084 |
| 1048576 | 6.719 | 6.382 | 0.293 |

Exp has no pre-IQK baseline in this FreeBSD experiment. The generic math-batch
driver also completed its scalar-core/callback timing sequence; those different
measurement boundaries are retained in `batch-time.csv`, not treated as
public before/after timings.

These are within-FreeBSD comparisons. Earlier Linux measurements used Clang 18
and different runtime/system conditions, so their absolute difference does not
isolate operating-system performance. No frequency policy was changed. A
separate observation reported CPU 0/2 at 2,922 MHz and HWP EPP=50; that snapshot
is not a measurement of average frequency during each timing interval.

## Hardware counters and memory

The user loaded `hwpmc` through an interactive Terminal.app SSH sudo prompt.
Password input remained in that terminal. Process counters then worked without
root. On this host the generic `cycles` alias was rejected, so collection used
`inst_retired.any` and `cpu_clk_unhalted.thread`, explicitly pinned to CPU 2.
Counter runs were separate from the timings above.

Counter workloads use the core callback, N=262,144, 100 repeats plus one
warmup, span [-1,1], with `profile` mode skipping repeated output smoke checks.
Counts cover the complete process, including startup and the one warmup check;
they are not isolated kernel counts. Cumulative counts were collected with
`pmcstat -C -w 60`; each process exited before the first periodic interval.

| Function | Baseline retired instructions | Current retired instructions | Reduction | Baseline cycles | Current cycles | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| sin | 30,531,146,602 | 3,105,335,383 | 9.83x | 8,777,415,740 | 573,788,936 | 15.30x |
| sincpi | 118,494,198,788 | 3,221,771,098 | 36.78x | 36,605,482,329 | 783,000,205 | 46.75x |

The reduced instruction counts support the removal of per-lane dynamic
certification and scalar reduction overhead. Counter ratios need not equal
public latency ratios because the measurement boundaries and array sizes differ.

A separate `/usr/bin/time -l` run of current public sincpi, N=1,048,576,
reported 31,232 KiB maximum RSS. The public managed Payload peak was 8,597,584
bytes; sin/cos reported 8,599,440 bytes. As in the earlier experiments, the
new four pi/sinc paths add 768 managed bytes and sin/cos retain their existing
maximum scratch size. RSS includes libraries, benchmark inputs and other process
storage and must not be equated with managed Payload.

## Extended analysis

The subsequent [full scoped analysis](freebsd-full-analysis.md) adds size and
layout sweeps, repeated A/B timings, fallback-sensitive inputs, other math
batches, representative CRV workloads, nineteen hardware sampling traces,
source attribution, branch/cache counters, syscall summaries and RSS scaling.
It also reports measured regressions and prioritized follow-up experiments.

## Reproduction and retained artifacts

Remote experiment: `/home/alex/photospider-trig-20260925`. Its `validate.sh`,
`measure.sh` and `counters.sh` record the executed commands. Current and baseline
build directories explicitly retain their Clang 22 compiler paths and isolated
runtime flags. Local copies of results are under ignored
`build/num04-exp/freebsd/`.

```sh
cd /home/alex/photospider-trig-20260925
./validate.sh
./measure.sh
./counters.sh
```

The equivalent primary timing command is:

```sh
cpuset -l 2 /usr/local/bin/python3.12 examples/numeric_workflow/trig_measure.py \
  build/examples/numeric_workflow/photospider_numeric_trig_benchmark \
  baseline-build/examples/numeric_workflow/photospider_numeric_trig_benchmark
```

The runtime installation is local to this experiment. Global aliases are
unchanged; the user-authorized hwpmc module remains loaded. This test does not
add a new selectable numeric backend or change the existing implementation.
