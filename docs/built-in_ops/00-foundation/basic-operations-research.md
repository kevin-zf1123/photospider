# Basic operation research and implementation contract

Approved scope: twelve basic operation families, CPU, existing ABI/Traits 7.
Research baseline: `ops@2495393c`, 2026-09-11. Foundations PR #298 is merged.
This research commit does not claim implementation or executed product tests.

## Interfaces and numeric policy

All parameters below are required. Examples supply defaults explicitly. Fields
are Float32/Float64 HW; matching binary arrays require identical dtype and shape.
Outputs retain dtype unless stated otherwise. Generic transformations drop
facets; normalized smoothing preserves valid field/coverage interpretation.
Reject nonfinite inputs, unrepresentable intermediates/results and invalid
parameter relationships. Inputs and scratch use logical addressing and the host
allocator; cancellation and allocation failure publish no successful partial
output. Do not extend ABI, implicit broadcasting or GPU support.

| Family | First implementation contract |
| --- | --- |
| curve.sample_linear | Generic Kx2 controls, K>=2, finite values, strictly increasing x. Int64 count 2..1048576; Float64 domain_min<domain_max; String out_of_domain reject/clip. N samples include both domain endpoints, same-dtype generic N output. No y clipping. |
| curve.sample_monotone | Same interface. PCHIP weighted harmonic interior derivatives, limited one-sided end derivatives; two controls give linear interpolation. Turning y values are allowed. |
| field.apply_lut_1d | Field plus same-dtype generic N table, N>=2. Explicit domain_min/max and reject/clip. Endpoint-exact linear interpolation. Generic field output; existing lut.apply_1d remains separate. |
| mask.invert/combine | Canonical Float32 coverage. Invert=1-A. Required operation and/or/xor and algebra fuzzy/independent_coverage. Fuzzy=min/max/abs(A-B); independent=AB/A+B-AB/A+B-2AB. |
| image.mix | Canonical premultiplied RGBA A/B and HW coverage M. (1-M)A+MB on all four channels. Exact M=0/1 endpoints and identical alpha preservation. |
| field.box_mean/gaussian_blur | Int64 radius 1..64, square support, clamp boundary. Box divides by full window size. Gaussian Float64 sigma 0..64, zero identity; normalized separable sampled Gaussian, Float64 accumulation/intermediate. |
| mask.dilate/erode | Int64 radius 0..64, String footprint square/disk. Disk uses dx*dx+dy*dy<=r*r. Gray max/min, outside canvas zero; radius zero identity. Opening/closing are compositions. |
| field.convolve/correlate | Field plus same-dtype KhxKw kernel; required Int64 anchor_y/x within kernel and String boundary clamp/zero. Same output, no normalization/bias. Correlation reads I[p+j-anchor]; convolution reads I[p+anchor-j]. Row-major Float64 accumulation. |
| analysis.histogram | Int64 bins 1..1048576, Float64 range_min<range_max. Int64 bins output; equal-width left-closed intervals, final bin includes upper endpoint. Out-of-range samples excluded from bins and counted by analysis.histogram_out_of_range as Int64 [underflow,overflow]. |
| grade.levels | Finite Float64 black<white, gamma>0, out_min<=out_max. t=clamp((x-black)/(white-black),0,1); out_min+(out_max-out_min)*pow(t,1/gamma). Generic output. |
| numeric.minimum/maximum/abs; field.smoothstep | Finite Float32/64 numeric arrays; min/max zero ties choose negative/positive zero respectively, abs produces positive zero. Smoothstep has finite edge0<edge1; t=clamp((x-edge0)/(edge1-edge0),0,1), t*t*(3-2*t); Float32 coverage output. |
| field.coordinate/constant | No inputs. Positive Int64 height/width, String dtype float32/float64. Coordinate requires axis x/y and space pixel/normalized: i+.5 or (i+.5)/axis length. Constant requires finite Float64 value. Generic HW output. |

## Algorithms and sources

- [SciPy PchipInterpolator](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html):
  with h_i=x_(i+1)-x_i, delta_i=(y_(i+1)-y_i)/h_i, set an interior
  derivative to zero for zero or opposite-sign adjacent secants. Otherwise use
  (w1+w2)/(w1/delta_(i-1)+w2/delta_i), w1=2*h_i+h_(i-1),
  w2=h_i+2*h_(i-1). Endpoint estimates are sign-limited and capped at three
  times their adjacent secant when the neighboring secants change sign.
  Sorted queries permit O(K+N) evaluation and O(K) coefficient scratch.
- [SciPy Gaussian](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.gaussian_filter.html):
  finite support is explicitly radius, not a hidden truncation convention.
  Separable normalized weights cost O(HW*r); temporary Float64 samples avoid
  narrowing between passes. Box uses the same separable support and full
  denominator, O(HW*r). Clamp means repeating the nearest canvas pixel.
- [SciPy convolution](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.convolve.html):
  anchor and kernel reversal distinguish convolution from correlation. Direct
  arbitrary-kernel evaluation costs O(HW*Kh*Kw); a zero-sum derivative kernel
  must never receive automatic sum normalization.
- [SciPy gray dilation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.grey_dilation.html):
  flat-footprint dilation is neighborhood maximum; erosion is minimum.
  Direct square/disk scans cost O(HW*r*r); no continuous-offset claim.
- [W3C compositing](https://www.w3.org/TR/compositing-1/): premultiplied
  coverage provides the algebra underlying the independent-coverage option.
  Fuzzy selection is a separately named product choice. Equal .5 masks give
  fuzzy (.5,.5,0), independent (.25,.75,.5), for AND/OR/XOR.
- [NumPy histogram](https://numpy.org/doc/stable/reference/generated/numpy.histogram.html):
  final-bin endpoint inclusion is adopted; automatic ranges, weights, density
  and multi-output tuples are excluded. Stable Float64 edges and binary search cost O(HW*log(bins)+bins).
- [Khronos smoothstep](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html)
  supplies the clamped cubic formula; invalid/reversed endpoints are explicit
  errors here. Levels is an explicitly chosen power-law interface, with
  gamma>1 lifting midtones, not an implicit color-space conversion.

## Region and composition

Elementwise: numeric min/max/abs, levels, smoothstep, mask Boolean, image mix.
Halo: positive-radius field box/Gaussian. Whole: curves, field LUT, kernel
filters, histograms, generators, and radius-zero-capable morphology. Existing
Axes inference and typed/repeated declarations require Whole; existing dynamic
halo parameters require a positive lower bound. No operation-key inference or
new shared enums are necessary.

Four public workflow scenarios extend examples/foundations_workflow: curves
through extracted channels and mix; coverage Boolean/morphology/feather;
asymmetric filters through numeric error and histograms; generated fields
through smoothstep and local levels. Install-only static/shared builds verify
that examples consume the public package.

## Acceptance

Independent fixtures: linear controls (0,0),(.5,.25),(1,1) produce .125 at
.25; PCHIP of these controls produces .078125 at .25 and .546875 at .75.
Check plateau, turning controls and two-point identity. Four-bin [0,1]
histogram of [0,.25,.5,1] is [1,1,1,1]; bins plus both overflow counts equal
the input sample count. Levels with gamma=2 maps .25 to .5. A radius-one disk
of a central impulse contains five pixels, square nine. Pixel coordinates for
width two are [.5,1.5], normalized [.25,.75].

Also cover endpoint policies, repeated/nonfinite controls, signed/HDR/extreme
values, non-symmetric even kernels and both anchors, single-row/column and
oversized footprints, alpha endpoints, dtype/shape errors, non-contiguous
views, logical origins, ROI versus Whole, cancellation and budget release.
Use ClangFormat 21, cpplint, focused tests and installed consumers. After all
twelve items pass, an independent read-only reviewer examines the complete
diff and actual results, followed by PR CI and Codex bot review.
