# External FFT → frequency response → inverse workflow

This example registers `fft.forward_real`, `fft.import_response`, `fft.multiply`, and `fft.inverse_real` through the public `Photospider::kernel` API. The workflow binds immutable Float64 Result tensors named `pixels` and `response` directly, then publishes Spectrum and spatial Results to a sink that checks every output pixel. The transform covers the declared original HW image; a bounded input window is not an independently transformed image tile. The C++ fixture's `variant` parameter selects test values and backing layout; it is not a workflow input or source operation.

```sh
cmake --build build/kernel-dev --target photospider_fft_workflow test_fft_contract -j 8
ctest --test-dir build/kernel-dev -R '^(test_fft_contract|example_fft_workflow)$' --output-on-failure
```

To build and run against an actual installed package:

```sh
cmake --install build/kernel-dev --prefix "$PWD/out/fft-install"
cmake -S examples/fft_workflow -B out/fft-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/fft-install"
cmake --build out/fft-consumer -j 8
out/fft-consumer/photospider_fft_workflow
out/fft-consumer/photospider_fft_workflow --large
```

The standalone CMake project requires a compatible Photospider 0.32 package.

## Result inputs and ownership

`pixels` is an unbatched Result with one facet-free Float64 tensor of shape HW and no fields. `response` has the same Result form with shape HW2 for Full packing or HK2 for R2CHalf, where `K=floor(W/2)+1`. The operations validate tensor type and exact shape without fixing the input Result schema ID. Spectrum-to-Spectrum edges require complete Spectrum identity. Fixture setup creates backing with `BufferAllocator`, imports it through `root.reference()`, and binds the resulting immutable Results directly. The schema stays fixed across the value and layout variants.

The shared input helper creates ordinary contiguous backing, negative-stride backing, or constant zero-stride broadcast backing. The variants use the same tensor schemas.

The example sink publishes a one-tensor Float64 Result after checking the complete image. Result associations retain source ObjectIds as facts, not source payload. Loaded field windows retain their read plan and Result implementation, so their own bytes remain readable after their Result wrapper and execution context are released; backing is reclaimed when the last window is released.

`BufferAllocator` creates each full caller input backing, and `root.reference()` charges those bytes to Root Referenced. Root Host and Payload peaks exclude these input bytes. Each run caps Referenced at `2*(H*W*8 + H*K*16)` bytes for the two Result inputs; Host and Metadata are 1 MiB when `H*W<=65,536` and 4 MiB otherwise, and Payload is 32 KiB. The executable reports `referenced_peak`, checks Host and Payload peaks against their configured limits, and checks that live Referenced and Disk usage return to zero after the last output window is released. The independent DFT/reference calculations are caller-owned and outside Root accounting. These are fixture budgets, not limits imposed by the FFT operation factory.

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

`--large` configures both Full and R2CHalf at 1024x1024, with a 1024-byte page, 4 MiB each for Host and Metadata, 32 KiB Root Payload, 1 GiB Disk, five billion Run work units and a one-million-stage per-continuation limit. The bound `pixels` Result contains a unit impulse at (1,0). Every stored frequency is checked against the independent closed form `exp(-2*pi*i*u/H)` in bounded reads, and the sink checks every shifted pixel: only (2,1) is one. The default run includes 32x32 Full/Half with a 1100-stage per-continuation limit and an 8192-sample external-axis fixture. `--stage-regression` stops after those transforms and a cross-axis-row singleton case. Printed `issued_stages` totals coordinator actions across the whole DAG, not the poll count of any single producer.

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
  paged, with two full mandatory disk generations and 1 MiB each for configured
  Host and Metadata capacity. The 513-point odd axis also exceeds workspace and
  exercises streaming direct-DFT leaves with an explicit 100-million Run-work
  envelope.
- Cache-off duplicate expressions start once and return the same Result ObjectId.
- Associations record source ObjectIds; they do not retain source payload. A
  loaded inverse field window retains its own read plan and backing after the
  Result wrapper and execution context are released, until the final window is
  released.
- Bad Hermitian response, nonfinite input, too-small window, work/disk
  exhaustion, cancellation after forward publication with Disk backing live,
  stale plans, and changed Run snapshots have distinct checked outcomes. A
  stale plan is rejected before any operation starts.

`test_fft_contract` exercises the public registry's rejection of mismatched
original shape and multiply identity before state allocation, including equal
half-count shapes. Empty/zero-axis transforms are rejected at the schema API.
Spectrum has fixed positive shape and CompleteBundle publication; this example
does not relabel a partial transform as a complete or dynamically empty one.

The sink compares each output pixel with the exact circular-shift reference. The standalone arithmetic oracle reports direct DFT differences and inverse imaginary residuals as Measured values, not CertifiedBounds. Root counters do not represent process RSS; `issued_stages` totals coordinator actions across the DAG and is not a per-continuation stage count. This FFT operation factory is CPU-only; the example makes no GPU claim. The declared acceptance rule is `RealProjectionMeasured(atol=1e-10,rtol=1e-12)`.
Coefficients are not altered or tolerances enlarged to pass it. Inverse emits
the real component and explicitly records max absolute discarded imaginary
component. Managed-capacity peaks exclude allocator/OS/driver overhead and are
not RSS hard bounds. See [the runtime contract](../../docs/kernel-architecture/External-FFT.md)
for the recipe's arithmetic and I/O costs.

The default workflow passed locally and through the installed 0.30 consumer. The local 1024x1024 Full and R2CHalf `--large` runs also passed their direct DFT and shifted-pixel checks; the installed consumer was checked on the default workflow only. Both local packings measured DFT error `7.02167e-16`; inverse imaginary residuals were `1.8744e-16` and `1.25227e-16`. Root Host peaks were 288,997 bytes for each packing, Payload peaks were 8,048 bytes for each, Referenced peaks were 25,165,824 and 16,793,600 bytes, and aggregate `issued_stages` were 1,941,522 and 1,872,002. These fixture counters are not RSS or error certificates.
