# Generation mathematical oracles

Runnable references for the [generation specifications](../../../docs/built-in_ops/03-generation/generators.md).
These Python tools do not register operators or implement the C++ kernel, public
WorkflowDocument protocol, Result codecs, runtime Region callbacks or GPU execution.

## Run

Python 3.10+; exact arithmetic, RNG core, finite geometry, resource and JSONL tests
use the standard library. Optional Measured tests use mpmath 1.3.0. Install it in
an isolated environment to include those tests; without it they are visibly skipped.
NumPy is optional for non-gating spectrum diagnostics only.

```sh
python3 -m venv build/generation-oracle-venv
build/generation-oracle-venv/bin/python -m pip install -r oracle/ops/generation/requirements-oracle.txt
build/generation-oracle-venv/bin/python oracle/ops/generation/run_oracles.py --self-test --report build/generation-oracle/self-test.json
build/generation-oracle-venv/bin/python oracle/ops/generation/run_oracles.py --input oracle/ops/generation/sample_requests.jsonl --candidate oracle/ops/generation/sample_expected.jsonl --profile strict --report build/generation-oracle/candidate-check.json
```

Reports/logs go under ignored `build/`, not into source fixtures.
Test results are written by the current invocation.

## Evidence and limits

| Module | Implemented reference | Boundary |
| --- | --- | --- |
| exact.py | Fraction mathematics, IEEE RNE/sqrt, basic fields, generic numeric mesh, source-width coordinates, rational-root two-circle subset | Not full shape/metadata validation; irrational two-circle roots explicitly unsupported |
| rng.py | Philox4x64-10 core with three upstream KATs, candidate addressed sequences, fixed Perlin polynomial, point algorithms | Packing/seed/word extraction are **experimental**, not production goldens; Bridson candidates use ordinary math and are Measured |
| geometry.py | Finite rational curves, flatten bounds, polygon area/Boolean boundaries, polyline fixtures | No Result ABI; many arc operations require rational segment lengths; publication helper performs necessary checks only, not general topology certification |
| measured.py | High-precision integral/transcendental/Poisson diagnostics | All outputs are Measured; precision agreement does not certify rounding or global roots |
| resources.py | Raw rank structure/hash/lookup/threshold checks | Synthetic fixtures are not approved blue/STBN assets |
| run_oracles.py | Finite JSONL reference/candidate comparison | Oracle protocol, not kernel workflow JSON; accelerated sample comparison is not admission |

[Per-member coverage](../../../docs/built-in_ops/03-generation/oracle-coverage.md)
lists the available helper subset and remaining limitations for all 95 current member
specifications. No coverage level means the corresponding operator is implemented.
A helper test is not complete validation of every member using it.

## RNG candidate isolation

The core is the selected Philox4x64-10. Address packing and floating bit selection
remain unresolved in the random contract. This suite experiments with `philox4x64-layout-experiment-1`:
`c0=uint32(x)|(uint32(y)<<32)`, `c1=frame|(draw<<32)`,
`c2=stream|(channel<<32)|(domain<<48)`, `c3=0`; key is `(seed_bits,0)`.
Each x/y is signed32; frame/draw/stream are unsigned32 ranges carried as integers.

Experimental float extraction takes 24 high bits of word 0 for Float32; 27 high
bits of word 0 and 26 of word 1 for halfopen53; 26 high bits each of words 0/1
for open52; words 2/3 form the second sample. This is an explicitly recorded
experiment, not a specification amendment. There are no old 4x32 implementations.

The Python stochastic helpers all inherit this candidate status. CLI uniform,
gradient2d and fractal operations reject by default; `--allow-candidate-rng` opts
in and labels results as candidate. Statistics also requires this flag:

```sh
python3 oracle/ops/generation/run_oracles.py --statistics --allow-candidate-rng --report build/generation-oracle/statistics.json
```

Do not use these addressed results as production goldens until packing and mappings
are separately frozen. Philox core KATs do not depend on packing. Fixed Perlin 2002
has its own permutation, no seed, and does not use Philox.

## Mathematical semantics

Repeat/angle final rounding is not followed by nextDown/wrap. Cell point placement
and rank midpoint mappings retain their separately specified endpoint rules.
Generic meshes treat every component numerically, including a fourth component.
Point max_count is a normal target; `with_status=True` returns count/candidate/active
termination reasons. Independent iteration limits fail. Candidate helpers currently
require max_count >= 1; the production zero-count boundary remains a unresolved schema detail.
Width helpers retain source position rather than reapplying the profile to each
fragment. They do not implement snapshot association or arbitrary curve remapping.

## JSONL

Each request has a unique string id, a supported op (`--list`) and args. Floating
outputs carry dtype and IEEE hexadecimal bits. Input bit envelopes preserve binary
values; rational envelopes are mathematical probes, not new runtime dtypes.
Boolean output is the exact rational boundary before Float64 publication.
Candidate-result quality labels are never treated as proofs.

The 14 sample expected results come from hand calculations and the external core
KAT, not by running this oracle and calling the result independent. The same cases
also appear in fixtures.json; these are two validation paths, not 28 independent
fixtures. Fixed-seed crosschecks provide additional finite coverage. Provenance:
[SOURCES](SOURCES.md).

## Actual Perlin runtime comparison

`check_perlin_runtime.py` sends raw IEEE coordinates to a native runner and
compares its returned bits with `rng.perlin2002_fraction` plus independent exact
IEEE rounding. It covers 1566 input/output combinations including Float32/64,
negative coordinates, subnormals, huge finite values and deterministic random
coordinates. Its default seed and case construction are fixed in the script.

```sh
cmake --build build/kernel-dev --target test_perlin_exact test_perlin_workflow -j 8
python3 oracle/ops/generation/check_perlin_runtime.py \
  --runner build/kernel-dev/test_perlin_exact
python3 oracle/ops/generation/check_perlin_runtime.py \
  --runner build/kernel-dev/test_perlin_workflow
python3 oracle/ops/generation/check_perlin_runtime.py \
  --runner build/kernel-dev/test_perlin_workflow --tiled
```

The first runner exercises the C++ integer calculator. The second compiles and
executes actual public CPU Whole workflows, or CPU tiled with `--tiled`.
Each mode checks the same 1566 cases independently.
Worker, layout, ROI and resource behavior is checked separately by
`test_perlin_workflow`, `test_perlin_tiled` and `test_cpu_tiles`; finite oracle
coverage is not exhaustive correctness proof.

## Native strict Perlin GPU comparison

`test_perlin_gpu --stdin` shares one native execution context across requests,
executes the registered GPU-only Whole form, and verifies nonzero native dispatch
counts and absence of fallback before emitting each output's raw bits. It accepts
the same input protocol as the Whole/tiled runners.

```sh
python3 oracle/ops/generation/check_perlin_runtime.py --runner build/kernel-dev/test_perlin_gpu
```

Build the kernel with `-DPHOTOSPIDER_ENABLE_VULKAN=ON` to select its Vulkan backend;
the same GPU runner then reports whether the native Vulkan dispatch succeeded. The
FreeBSD Intel UHD 770 run matched all 1,566 recorded cases. NVIDIA and Linux remain
untested, and the checker does not measure speed.

The checker independently computes the full polynomial with Fraction and rounds
to the requested IEEE dtype. Unsupported devices make the runner fail/skip, never
substitute a CPU result. Both native Metal and Vulkan paths are exercised by this
runner. The Vulkan runner passes all 1566 comparisons on FreeBSD Intel UHD 770;
the Perlin Vulkan path remains untested on NVIDIA and Linux hardware. These
finite oracle checks do not measure performance or prove the full input domain.
