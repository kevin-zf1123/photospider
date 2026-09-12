# External-axis FFT runtime

Package 0.10 provides the C++ factories in `plugin/fft_operation.hpp`; the C
operation ABI remains 9. They execute through structured protocol 2 and public
managed Result/temporary-storage APIs. No external FFT library, GPU provider or
whole-axis RAM allocation is required.

## Identity and stages

`fft_spectrum_spec(H,W,packing,atol,rtol)` constructs a closed HW profile:
transformed axes and storage order `[0,1]`, zero shifts/origin, unit sample step,
coordinate unit `sample`, negative forward sign and Unscaled normalization.
Packing is Full or width-axis R2CHalf. The policy is RealProjectionMeasured with
finite nonnegative tolerances. Factories reject unsupported rank, axes/order,
shift, sign, normalization, origin/step/unit or policy rather than silently
reinterpreting them. H/W are positive and `H*W <= (INT64_MAX-4095)/16`.

| Operation | Inputs | Output |
| --- | --- | --- |
| `fft.forward_real` | Facet-free finite Float64 HW | Complete Spectrum |
| `fft.import_response` | Facet-free finite Float64 HW2 or HK2 | Complete Spectrum on the explicitly assigned basis |
| `fft.multiply` | Two complete identical Spectrum schemas | Complete pointwise complex product |
| `fft.inverse_real` | Complete Spectrum | Complete real projection and measured imaginary residual |

Static validation compares complete schemas, including original dimensions,
before source reads or continuation allocation. Equal packed sample count is
insufficient: W=4 and W=5 both store K=3. The public registry also compares
inferred output metadata with caller-supplied output metadata before starting
the producer. Response coefficients are explicit numeric frequency multipliers;
there is no implicit spatial resampling, shift or normalization conversion.

Forward computes the unscaled negative-sign DFT over the entire HW domain.
Inverse uses positive sign along both axes, then divides each component by the
binary64-converted HW count exactly once. Arithmetic uses binary64 nearest
ties-to-even, gradual underflow and no FMA contraction; caller floating state
is restored. Complex multiply checks its four products and two additions.
Nonfinite input is InvalidDomain; nonfinite arithmetic is ArithmeticOverflow.

`photospider.fft_real_output` contains H*W Float64 `pixels` and one Float64
`imaginary_residual`. Its `fft_inverse_identity_v1` facet retains the complete
forward Spectrum identity. Pixels are the real inverse projection; the residual
is max(abs(imag(inverse/HW))). It is a finite measured diagnostic, not a
certified error bound. The registered recipe does not normalize signed zeros.

## External DIF recipe

Let each axis length be L=2^s*m with m odd. Work on one logical contiguous axis
at a time in two full complex temporary arrays A and B, with fixed-width
16-byte addressing and no per-page resident directory.

1. Spool finite source samples into A, or expand an input half spectrum into A.
2. For span=L,L/2,...,2*m, apply in-place DIF butterflies. Read both original
   halves before writing `(a+b)` and `(a-b)*exp(sign*2*pi*i*j/span)`.
   Large spans use two bounded windows; small spans batch several whole groups
   into one contiguous window.
3. For natural frequency k, read odd leaf
   `bitreverse(k mod 2^s,s)` and direct-DFT frequency `floor(k/2^s)`, accumulating
   in increasing leaf-sample order across page boundaries. Write to B in natural
   frequency order. All leaf reads use unchanged A; B writes cannot overwrite
   future inputs. For m=1 this is a permutation, not another full DFT.
4. Transpose B into A using bounded gather batches, then transform the other
   axis. Gather the final transposed storage into declared HW order when
   writing the final Result. H=1 omits the unnecessary second axis.

The relation `k=bitreverse(leaf,s)+2^s*q` follows by selecting low frequency bits
at successive DIF levels. For L=12, physical leaf/frequency enumeration yields
`0,4,8,2,6,10,1,5,9,3,7,11`. Pure odd axes use direct DFT and are quadratic;
the implementation does not claim universal O(L log L).

Both temporary files are created, then extended by checked 16*HW bytes before
dependent writes. Extensions are separate coordinator actions, with real
4096-byte zero staging and aligned disk admission. The files remain writable
private scratch and are not sealed between generations. Stage completion drains
all writes before role changes. Each data window is at most
`min(user_page_bytes,1024)` bytes; complex records require at least 16 bytes.
The declared callback workspace is 4096 bytes, with temporary read replies,
owning window metadata and retained buffers additionally charged to the root.
Odd-leaf output buffers and gather batches are explicitly owned and bounded.

Arithmetic work is O(HW*(log2(W/mw)+mw+log2(H/mh)+mh)); odd-leaf direct summation
may dominate. Every pass, butterfly, accumulation, initialization and I/O is
charged. Mandatory scratch occupies two separately admitted full generations,
plus any final Result and live predecessor owners. Stage, work, I/O or capacity
exhaustion fails without a complete result; already submitted work is not
refunded. The example selects a finite 200000-stage envelope; the general
default 4096-stage setting does not promise all large transforms will fit.

## Real policy and publication

For K=floor(W/2)+1, omitted frequency values are
`F[u,v]=conj(C[(-u) mod H,W-v])`. This reflects both frequency axes. Nyquist
columns need not be wholly real; only simultaneously self-conjugate points are
real. No coefficient projection is inserted to hide a Hermitian defect.

Spectrum retains its current CompleteBundle contract. The runtime checks every
finite component and applicable Hermitian pair against the declared atol/rtol
before observers or consumers see it. Multiplication is validated again; two
accepted inputs do not guarantee an accepted product under the same absolute
tolerance. A rejected Spectrum is TypeMismatch with Association scope. No
Exact or CertifiedBound is inferred from passing the floating acceptance rule.

Private scratch is released after its final copied page is written, before the
additional Spectrum validation windows are needed. Validation is paged, but
reflected accesses can reload windows; its I/O cost is not represented as one
sequential scan. Conservative whole-transform relations retain full source and
descriptor support. Result associations, shared cache-off owners and escaped
read windows retain mandatory backing until the last owner releases it.

The [public workflow](../../examples/fft_workflow/README.md) provides executable
commands, independent direct-DFT and circular-shift references, half/full and
Nyquist cases, an axis exceeding workspace, bounded failures and lifetime tests.
Reported numerical differences are measured fixture agreement. Resource peaks
refer to the product managed-capacity ledger, not a process RSS hard bound.
