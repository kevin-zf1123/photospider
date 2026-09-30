# External-axis FFT runtime

[Chinese reader version](zh/External-FFT.zh.md).

## 1. Scope and ownership

The installed CPU operation factory implements full-domain two-dimensional real FFT operations using mandatory temporary storage and bounded windows. `fft_operation.hpp` exposes the public factory; the coordinator owns Result publication, root resource admission, and input associations. The operation owns two temporary complex generations while transforming; output Results retain their backing and upstream associations independently of the execution context and optional shared cache.

## 2. Data layout and memory

```cpp
Result<SpectrumSpec> fft_spectrum_spec(
    std::uint64_t height, std::uint64_t width,
    SpectrumPacking packing = SpectrumPacking::R2CHalf,
    double atol = 1e-10, double rtol = 1e-12);
Result<OperationDefinition> make_fft_operation(
    FftOperation operation, const SpectrumSpec& spectrum);
```

The factory accepts positive H and W with checked `H*W <= (INT64_MAX-4095)/16`. It freezes both transformed axes and their stored order `[0,1]`, zero shifts and origin, unit sample steps, `sample` units, negative unscaled forward sign, and either full storage or width-axis R2CHalf packing. The real policy is `RealProjectionMeasured` with finite nonnegative Hermitian tolerances. A descriptor that differs in rank, axes, order, shift, sign, normalization, origin, step, unit, packing, or policy fails instead of being reinterpreted.

Spectrum samples are Float64 real/imaginary pairs in the declared slow-to-fast axis order. The four registered keys are:

| Key | Input | Output |
| --- | --- | --- |
| `fft.forward_real` | Facet-free finite Float64 HW | Complete Spectrum |
| `fft.import_response` | Facet-free Float64 HW2 or HK2 | Complete Spectrum on the explicitly assigned frequency basis |
| `fft.multiply` | Two identically described complete Spectra | Complete pointwise complex product |
| `fft.inverse_real` | Complete Spectrum | Real projection and measured imaginary residual |

`photospider.fft_real_output` stores H*W Float64 pixels and one Float64 `imaginary_residual`. Its `fft_inverse_identity_v1` facet preserves the full Spectrum contract. The residual is `max(abs(imag(inverse/HW)))`; it is a measured diagnostic, not an error certificate.

## 3. Execution and state machine

The producer checks the complete Spectrum identity before source reads or continuation allocation. Widths 4 and 5 both pack to three columns, so packed sample count alone cannot establish identity. The registry also checks inferred output metadata before starting the producer. Whole-transform dependency support is Conservative; an input edit can dirty every output.

```text
real source --forward: spool A -> transform -> transpose A/B -> second axis--> Spectrum
response Value --import_response: direct bounded copy -----------------------> Spectrum
Spectrum + Spectrum --multiply: bounded pointwise product -------------------> Spectrum
Spectrum --inverse: expand half -> transform -> transpose A/B -> project ----> spatial Result
Spectrum/Result validation ------------------------------------------+-------> publish
                                                                     +-------> reject
```

Forward and inverse create two full complex temporary generations, each extended by checked `16*H*W` bytes. Import and multiply do not run the DIF recipe. Create, extend, dependent writes, and draining work are separate coordinator stages. Windows are bounded by `min(user_page_bytes,1024)` and must hold at least one 16-byte complex record. Callback workspace is 4096 bytes; read replies, retained window metadata, output batches, and gather buffers are additionally charged to the root. Transpose, odd-leaf output, and final gathers are batched without overwriting values still needed from the source generation. Each producer is capped by its declared maximum of 1,000,000 dependency stages, `DependencyLimits::maximum_stages` (default 4096), and the root stage budget. An incomplete run publishes no complete Result. Mandatory scratch is released after its last required copy; Result associations and escaped read windows retain backing until their final owner releases it.

## 4. Algorithms and math

For each transformed axis length `L=2^s*m` with odd `m`, forward/inverse use DIF radix-2 spans `L,L/2,...,2*m`, then evaluate the odd-length leaf transform in increasing sample order. For natural frequency `k`, the leaf index is `bitreverse(k mod 2^s,s)` and the odd frequency is `floor(k/2^s)`. The producer reads unchanged generation A, writes natural-order values to B, then transposes B into A for the other axis. H=1 skips the second axis. Pure odd lengths use direct DFT and are quadratic; this is not universally `O(L log L)`.

For a real input `x[y,x]`, forward uses the negative-sign unscaled transform:

$$
F[u,v]=\sum_{y=0}^{H-1}\sum_{x=0}^{W-1}x[y,x]e^{-2\pi i(uy/H+vx/W)}.
$$

Inverse uses the positive sign and divides each component by the binary64-converted `H*W` exactly once. Its `photospider.fft_real_output` Result stores H*W Float64 pixels and one Float64 `imaginary_residual=max(abs(imag(inverse/HW)))`; the `fft_inverse_identity_v1` facet retains the complete Spectrum contract. The residual is a measurement, not an error certificate.

Arithmetic uses binary64 nearest ties-to-even, gradual underflow, and no FMA contraction; the caller's floating-point environment is restored. Complex multiplication checks all four products and both additions. Nonfinite input returns `InvalidDomain`; nonfinite arithmetic returns `ArithmeticOverflow`.

For R2CHalf, `K=floor(W/2)+1` and omitted values satisfy `F[u,v]=conj(C[(-u) mod H,W-v])`, reflecting both axes. A Nyquist column need not be wholly real; points self-conjugate on both axes must be real. The runtime does not project coefficients to hide a Hermitian defect. ExactHermitian compares finite real/imaginary components directly. RealProjectionMeasured compares a stored value `a` with the conjugate of its stored mirror `m` and accepts the pair under

$$
|a-\overline{m}| \le atol + rtol\max(|a|,|m|).
$$

The relative comparison uses exponent-separated binary64 fractions to avoid overflow. CompleteBundle checks finite components and applicable pairs before consumers see a Spectrum. Two accepted inputs can still produce a rejected product. Neither acceptance mode certifies transform error.

## 5. Limitations and non-goals

- The implementation is a paged CPU recipe; it does not require an external FFT library or GPU provider.
- Odd-axis direct transforms may dominate runtime. The algorithm is not universally `O(N log N)`.
- Work, I/O, window, disk, and root-capacity limits can also fail a run. Passing schema admission does not reserve all required resources.
- Spectrum tolerances and inverse residual are measured acceptance/diagnostic values, not error bounds.
- Validation can revisit reflected windows, so its I/O is not necessarily one sequential scan.
- Managed-capacity accounting is not a process RSS guarantee.

See [the FFT workflow](../../examples/fft_workflow/README.md) for the public entry point and runnable usage.
