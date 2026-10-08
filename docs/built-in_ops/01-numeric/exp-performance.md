# NUM-04 Float32 exp SIMD optimization

The maintained driver now runs IQK only. The subsequent
[adapter/FP64 update](adapter-performance.md) restores certified SLEEF binary64
exp and NUM-01 AST exp, while Float32 NUM-04 retains IQK. No Float64 input is
narrowed to fit the Float32 polynomial. The current Result computation reserves
an arithmetic object, a math batch workspace, and a Float32 SIMD workspace. Specification status remains Proposed. The maintained
`numeric.exp_accelerated_apple_silicon` and `numeric.exp_accelerated_x86_64`
callbacks now use a 64-sample batch and an IQK-derived normal-range SIMD expf.
Strict and other NUM-04/05 functions retain their arithmetic paths. ## Algorithm and correctness boundary

The polynomial is adapted from `ik_llama.cpp`'s `iqk_utils.h`, which attributes
the routine to Justine Tunney and Arm Limited. The original
MIT notice is retained in `third_party/IK_LLAMA_LICENSE.txt` and installed with
the kernel's licenses. Only the normal-range exp polynomial was extracted;
Photospider owns classification and fallback. The reviewed CPU files
`iqk_utils.h`, `iqk_cpu_ops.cpp`, `ggml-impl.h` and `ggml.c` do not provide a
corresponding standalone SIMD sin/pow candidate; relevant ggml CPU call sites
use `sinf`/`powf`. The header's tanh/silu/gelu wrappers are not NUM-04 functions
and were not added to this operator family.

Float32 finite inputs in [-80,80] use explicit FMA range reduction and a degree-5
polynomial, four lanes on NEON or eight on AVX2/FMA. A bit-level input mask
substitutes zero before SIMD arithmetic for every unsupported lane. Those lanes
then execute the existing certified path with original input bits. Zero returns
exactly one; NaN sign/payload, infinities, and subnormal/overflow output cases
retain existing exact handling. Tail lanes duplicate a valid sanitized operand;
all widths/layouts use the same per-lane FMA graph. No global fast-math flag is
introduced and no Float64 argument is narrowed.

`oracle/ops/numeric/exp_bound.py` is a deterministic exact-rational
certificate, not a random accuracy test. It encloses ln(2) with an atanh series,
proves the first reduction FMA exact via Sterbenz, bounds the second FMA, bounds
P-exp using translated Taylor polynomials over 256 complete intervals, and
propagates every binary32 rounding error. Separate negative/positive reduced
intervals and a neighborhood of zero handle the binade crossing. Conservative
reference-distance bounds are <3.123, <2.685 and <3.040 minimum representable
steps respectively. Scaling remains normal for the admitted integer exponents.
This establishes the existing four-step Float32 contract without a per-sample
SLEEF certificate or an unproved reliance on upstream's error comment.

The Result computation phase establishes nearest/gradual floating mode on its
CPU worker and restores that worker's prior environment on exit. The caller
thread's floating-point environment is checked separately around `execute`.

Per-lane indexing and mathematical work are still charged; checks are batched
in groups of at most 64. Fallback owns its own admission/refinement charge,
without double charging. Cancellation and failed work/capacity admission never
publish a partial result. The operation executes as a Whole Result computation
and retains source/build cache identity.

## Result-path acceptance

Run them with:

```sh
ctest --test-dir build/kernel-dev -R '^(test_numeric_unary_result|test_numeric_binary_result|test_numeric_comparisons_result|test_numeric_color_array_result)$' --output-on-failure
```
