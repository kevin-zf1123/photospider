# External FFT → frequency response → inverse workflow

This example uses only installed public headers and `Photospider::kernel`.
It registers `fft.forward_real`, `fft.import_response`, `fft.multiply` and
`fft.inverse_real`, then executes two sources → compiled DAG → a pixel-checking
sink. The transform always covers the declared original HW image. A source
window is not an independently transformed image tile.

```sh
cmake --build build/issue257-shared --target photospider_fft_workflow test_fft_contract -j 8
ctest --test-dir build/issue257-shared -R '^(test_fft_contract|example_fft_workflow)$' --output-on-failure
```

To build and run against an actual installed package:

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-delivery/install"
cmake -S examples/fft_workflow -B out/phase-a-delivery/fft-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/fft-consumer -j 8
out/phase-a-delivery/fft-consumer/photospider_fft_workflow
```

## Independent reference

Forward uses the negative-sign, unscaled two-dimensional DFT. Inverse uses the
positive sign and divides by HW once. The response is
`exp(-2*pi*i*(u/H+v/W))`; the expected output is the source circularly shifted
by one row and one column. The sink checks every pixel against this exact
indexing reference with an absolute `1e-8` fixture tolerance.

The executable also independently evaluates the direct DFT definition using
long double sine/cosine and summation, without the product's DIF factorization
or bit reversal. It checks every stored frequency for images of at most 1024
samples. For the 8192-sample external-axis fixture, it checks DC, frequency 1,
and the final stored frequency, plus **all** shifted output pixels. Printed DFT
differences and inverse imaginary residuals are Measured, not CertifiedBound.

An independent Python standard-library oracle for a 3×4 case:

```sh
python3 - <<'PY'
import cmath, math
h, w = 3, 4
x = [[(7*(y*w+z)) % 13 - 6 for z in range(w)] for y in range(h)]
def dft(y, sign):
    return [[sum(y[a][b]*cmath.exp(sign*2j*math.pi*(u*a/h+v*b/w))
                 for a in range(h) for b in range(w))
             for v in range(w)] for u in range(h)]
f = dft(x, -1)
g = [[f[u][v]*cmath.exp(-2j*math.pi*(u/h+v/w))
      for v in range(w)] for u in range(h)]
y = dft(g, 1)
expected = [[x[(a-1)%h][(b-1)%w] for b in range(w)] for a in range(h)]
print('expected pixels:', expected)
print('max direct error:', max(abs(y[a][b]/(h*w)-expected[a][b])
                               for a in range(h) for b in range(w)))
assert abs(f[0][0] - sum(map(sum, x))) < 1e-12
assert max(abs(y[a][b]/(h*w)-expected[a][b])
           for a in range(h) for b in range(w)) < 1e-12
PY
```

## What is exercised

- Singleton, odd/even and mixed radix shapes, Full and width-axis R2CHalf.
- W=4 and W=5 retain distinct original identities despite equal packed width.
- A 3×4 impulse has a nonreal Nyquist-column coefficient; that column is not
  incorrectly forced real. Missing half entries reflect both frequency axes.
- Eight-thousand-one-hundred-ninety-two complex axis samples occupy 128 KiB,
  exceeding the operation's **4 KiB workspace**. Actual reads/writes remain
  paged, with two full mandatory disk generations and a 64 KiB managed Host
  limit. The 513-point odd axis also exceeds workspace and exercises streaming
  direct-DFT leaves with an explicit 100-million-work envelope. This is actual
  external-axis execution.
- Cache-off duplicate expressions start once, return one ObjectId, and preserve
  owning Result/window associations after ExecutionContext destruction.
- Bad Hermitian response, nonfinite source, too-small window, work/disk
  exhaustion, cancellation after temporary writes, stale plans, and changed
  Run snapshots have distinct checked outcomes.

`test_fft_contract` exercises the public registry's rejection of mismatched
original shape and multiply identity before state allocation, including equal
half-count shapes. Empty/zero-axis transforms are rejected at the schema API.
Spectrum has fixed positive shape and CompleteBundle publication; this example
does not relabel a partial transform as a complete or dynamically empty one.

The declared acceptance rule is `RealProjectionMeasured(atol=1e-10,rtol=1e-12)`.
Coefficients are not altered or tolerances enlarged to pass it. Inverse emits
the real component and explicitly records max absolute discarded imaginary
component. Managed-capacity peaks exclude allocator/OS/driver overhead and are
not RSS hard bounds. See [the runtime contract](../../docs/kernel-architecture/External-FFT.md)
for the recipe's arithmetic and I/O costs.
