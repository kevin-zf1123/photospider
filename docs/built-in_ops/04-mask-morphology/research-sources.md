# Research sources and compatibility boundaries

Research date: **2026-09-25**. These are primary standards, official project
manuals or author-hosted research. Source support is scoped below; project API,
exact tie/rounding, resource and metadata decisions are not attributed to a
library merely because the library has a similarly named operation.

Documentation versions are the pages actually retrieved, **not** the installed
comparison-library versions. Actual local versions and test outcomes are in
[`test_report.json`](../../../oracle/ops/mask_morphology/test_report.json).
No third-party source code, paper full text or documentation mirror is bundled.

## Authoritative local contracts

[NUM common](../01-numeric/op_specs/NUM_common_contract.md),
[NUM accelerated](../01-numeric/op_specs/NUM_accelerated_contract.md),
[FMT common](../02-format-color/op_specs/FMT_common_contract.md),
[FMT relative scales](../02-format-color/op_specs/FMT_relative_coordinate_scale.md),
[original spec template](../00-foundation/spec-template.md).
All paths below are relative to this document.

<a id="s01"></a>
## S01 — W3C — Compositing and Blending Level 1

[Primary source](https://www.w3.org/TR/compositing-1/). W3C technical report; accessed 2026-09-25. Accessed 2026-09-25.

Alpha/compositing equations supply context for independent coverage and explicit alpha application. This does not infer exact subpixel intersection from two scalar coverages. Fuzzy min/max and the member-specific branch/rounding policies are project choices, not claims of a W3C mask API.

<a id="s02"></a>
## S02 — OpenCV — Miscellaneous Image Transformations

[Primary source](https://docs.opencv.org/4.13.0/d7/d1b/group__imgproc__misc.html). Opened documentation identifies OpenCV 4.13.0. Accessed 2026-09-25.

Threshold and floodFill document fixed-seed versus neighboring-pixel comparison. MASK uses exact symmetric tolerance and explicitly defines multi-seed union; it does not inherit OpenCV buffer mutation, asymmetric differences or mask border conventions. MASK distance validity and nearest ties are independently specified.

<a id="s03"></a>
## S03 — CIE — CIE 1976 L*a*b* colour space

[Primary source](https://cie.co.at/eilvterm/17-23-076). CIE International Lighting Vocabulary entry. Accessed 2026-09-25.

Primary vocabulary for CIELAB meaning. Native MASK/FMT storage remains l=L*/100, with a* and b* unchanged, as required by the repository. CIE coordinate definitions do not themselves establish repository storage normalization or an accepted runtime metadata codec.

<a id="s04"></a>
## S04 — scikit-image — color.deltaE_cie76

[Primary source](https://scikit-image.org/docs/stable/api/skimage.color.html#skimage.color.deltaE_cie76). Opened stable documentation identifies skimage 0.26.0. Accessed 2026-09-25.

Documents Euclidean Lab color difference. MASK-04B substitutes exact 100*(l-l_target) for the lightness difference before one final rounding. DeltaE2000 is a different function and is intentionally not aliased. Raw scaled-coordinate and wrapped-hue selectors are separately defined project members.

<a id="s05"></a>
## S05 — SciPy — ndimage.grey_dilation

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.grey_dilation.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

Flat grayscale dilation is a maximum over the selected neighborhood. MASK restricts footprints to symmetric origin-containing sets and specifies tie provenance. Library reflect/default boundaries are not adopted.

<a id="s06"></a>
## S06 — SciPy — ndimage.grey_erosion

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.grey_erosion.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

Flat grayscale erosion is a minimum over a neighborhood. The draft deliberately chooses infinite-lattice zero extension with final-only cropping for compound opening/closing. It does not assume a library call on a canvas-sized intermediate has that contract.

<a id="s07"></a>
## S07 — SciPy — ndimage.maximum_filter1d

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.maximum_filter1d.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

The documented MAXLIST approach has input-length-linear work independent of window length. This motivates a rectangular-footprint optimization candidate; exact tie handling, sparse support, signed-zero behavior and the repository resource model still require proof.

<a id="s08"></a>
## S08 — Felzenszwalb and Huttenlocher — Distance Transforms of Sampled Functions

[Primary source](https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf). Theory of Computing 8 (2012), 415–428; author-hosted paper; published 2012-09-02. Accessed 2026-09-25.

The lower-envelope formulation supports separable exact sampled-grid distance-transform algorithms. The PDF text and rendered pages 1 and 5 were inspected. This is not a continuous-contour offset algorithm, a certification of arbitrary binary64 comparisons, or a prescribed tie order. MASK-07D exact polygon membership/projection and fixed-grid coverage are project-defined; no area-error theorem is attributed to this paper.

<a id="s09"></a>
## S09 — SciPy — ndimage.distance_transform_edt

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.distance_transform_edt.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

Provides sampled Euclidean distances, anisotropic sampling and nearest-feature indices. MASK separately defines feature polarity, lexicographic ties, exterior sites, signed center convention, valid/within flags and finite no-feature payloads. The external routine is only a differential check of matched nondegenerate distances.

<a id="s10"></a>
## S10 — SciPy — ndimage.gaussian_filter

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.gaussian_filter.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

Documents explicit radius, boundary extension and separable filtering with output-dtype intermediate arrays. MASK instead defines one correctly rounded complete normalized sum. Matching sigma and radius does not make ordinary library arithmetic a strict oracle.

<a id="s11"></a>
## S11 — GNU MPFR — MPFR 4.2.2 Manual

[Primary source](https://www.mpfr.org/mpfr-current/mpfr.html). Opened manual identifies MPFR 4.2.2; local library version is separately recorded. Accessed 2026-09-25.

Directed rounding permits lower/upper enclosures and a destination-rounding certificate. The Gaussian oracle certifies equality of the two destination bit patterns, refining otherwise. MPFR is only a manual reference dependency, not a production dependency or proof of managed memory/cancellation behavior.

<a id="s12"></a>
## S12 — Microsoft Learn — HLSL smoothstep

[Primary source](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-smoothstep). Visible page last-updated date 2026-06-01; formula content accessible. Accessed 2026-09-25.

Documents clamped cubic Hermite interpolation t*t*(3-2*t). MASK uses this mathematical polynomial with exact construction and one RN_T, not the HLSL instruction rounding path. Reversed distance ramp, closed hard-step fallback and zero-width behavior are explicitly project-defined. A Khronos full-page open failed, so it was not used as the normative external citation.

<a id="s13"></a>
## S13 — Tan and collaborators — Parallel Banding Algorithm

[Primary source](https://www.comp.nus.edu.sg/~tants/pba.html). Author-hosted project page. Accessed 2026-09-25.

An exact Euclidean-distance GPU algorithm candidate. Its existence does not prove conformance of a new backend to MASK arbitrary spacings, deterministic ties, output rounding, sparse requests or memory ownership.

<a id="s14"></a>
## S14 — Rong and Tan — Jump Flooding Algorithm

[Primary source](https://www.comp.nus.edu.sg/~tants/jfa.html). Author-hosted project page. Accessed 2026-09-25.

Approximate Voronoi/distance propagation is relevant to fast previews. It is not silently admitted as exact nearest-feature selection; even a small distance error can alter a hard membership decision. Exact repair or a separately specified approximate product is needed.

<a id="s15"></a>
## S15 — scikit-image — segmentation.flood

[Primary source](https://scikit-image.org/docs/stable/api/skimage.segmentation.html#skimage.segmentation.flood). Opened stable documentation identifies skimage 0.26.0. Accessed 2026-09-25.

Seed-relative flood and connectivity provide a comparison point. MASK explicitly distinguishes fixed and neighbor graphs, barriers, multiple seeds, exact comparisons and Whole validation, rather than inheriting undocumented traversal choices.

<a id="s16"></a>
## S16 — SciPy — ndimage.label

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.label.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

Connected-component labeling and centrosymmetric connectivity are relevant. MASK foreground truthiness, compact/MinPixel ID rules, imported-label interpretation, dynamic attribute tables and real ObjectId associations are project contracts, not promises made by SciPy.

<a id="s17"></a>
## S17 — SciPy — ndimage.binary_fill_holes

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.binary_fill_holes.html). Opened manual identifies SciPy 1.18.0. Accessed 2026-09-25.

Explains filling holes via invasion of the complement from the outer boundary. MASK fixes complementary foreground/background connectivity and exact area inequalities. A background component touching the canvas is never an enclosed hole.

<a id="s18"></a>
## S18 — scikit-image — morphology

[Primary source](https://scikit-image.org/docs/stable/api/skimage.morphology.html). Opened stable documentation identifies skimage 0.26.0. Accessed 2026-09-25.

Official reference for remove-small, reconstruction, skeletonization and residual morphology concepts. MASK deliberately does not label its immediate row-major or distance-priority thinning as Zhang–Suen, Guo–Hall or medial axis. Existing library algorithm selection and thresholds are not silently imported.

<a id="s19"></a>
## S19 — SciPy — ndimage.binary_propagation; scikit-image reconstruction

[Primary source](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.binary_propagation.html). SciPy 1.18.0 manual; reconstruction details also inspected in S18. Accessed 2026-09-25.

Propagation until convergence supports the reconstruction/flood distinction. Grayscale marker/limit equations and fixed-point ordering are specified in MASK-15. The deterministic topology thinning schedule is project-defined and not derived from binary_propagation.

<a id="s20"></a>
## S20 — Krita Manual — Fill Tool / Close Gap

[Primary source](https://docs.krita.org/en/reference_manual/tools/fill.html). Page marks Close Gap as added in version 5.3.0. Accessed 2026-09-25.

Establishes the user-facing close-gap fill capability, not a disclosed algorithm or compatibility target. MASK defines two reproducible proposals: original-axis bounded runs, or morphological temporary barriers followed by flood. Neither claims Krita/Photoshop parity.

<a id="s21"></a>
## S21 — Repository NUM/FMT baseline — exact normalization and selected endpoints

[Primary source](../01-numeric/op_specs/NUM-11D_reduce_mean.md). Local uploaded revision 99901466389c27a9c40ccea7256792efc06a9452. Accessed 2026-09-25.

MASK-17 restricted weighted mean is a project-defined extension, constructed as an exact weighted numerator/denominator and rounded once. It is not a claim that existing reduce_mean accepts masks. Selected arithmetic channels consume zero-weight operands under the MASK-17 contract; explicitly bypassed channels follow NUM copy rules. Internal straight alpha and semantic consumption follow FMT.

<a id="s22"></a>
## S22 — Vulkan standard sample locations

[Primary source](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-standard-sample-locations).

The standard locations table defines patterns for 1, 2, 4, 8 and 16 samples when
standardSampleLocations is supported. Coordinates use the pixel's upper-left
origin. There are no standard 32/64-sample tables in this definition. These
tables can inform an explicitly fixed MASK sampling profile; using their point
locations alone does not establish Vulkan rasterization or edge-rule parity.

<a id="s23"></a>
## S23 — PBRT stratified sampling

[Primary source](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Stratified_Sampler).

Stratification partitions the pixel into cells; unjittered samples use cell
centers, while jitter places a sample within each cell. Regular patterns can
reinforce aliasing; jitter changes the error's spatial character toward noise.
A MASK jitter profile would require a fixed generator, seed and global-pixel
mapping so ROI, tiling and thread order cannot change the point set. Neither
pattern supplies an exact geometric pixel-area guarantee.

<a id="s24"></a>
## S24 — Standard two-phase thinning

[Zhang–Suen paper](https://doi.org/10.1145/357994.358023),
[Guo–Hall paper](https://doi.org/10.1145/62065.62074), and
[OpenCV reference predicates](https://github.com/opencv/opencv_contrib/blob/4.x/modules/ximgproc/src/thinning.cpp).

MASK-15E/F explicitly fix the neighborhood numbering, phase order and simultaneous
updates. OpenCV's lookup tables provide an independent local-predicate comparison;
its UInt8/255 interface and protected outer border are not MASK contracts. MASK
uses Float32/Float64 0/1 and virtual zero padding, with eligible canvas-edge pixels.
Standard thinning is not the same contract as MASK-15A's global topology guard.

<a id="s25"></a>
## S25 — Maximal balls in discrete shapes

[Primary research](https://liris.cnrs.fr/Documents/Liris-3479.pdf):
Finding a Minimum Medial Axis of a Discrete Shape.

The maximal digital-ball concept and reconstruction by a union of such balls
motivate MASK-15G. This specification retains all equal-support centers and
makes no minimum-cardinality or one-pixel-width claim. Its radius is the exact
nearest-background-center threshold for an open lattice ball; exported floating
radii alone do not provide an exact reconstruction certificate.

<a id="s26"></a>
## S26 — CIEDE2000 equations and supplementary examples

[Sharma, Wu and Dalal](https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/ciede2000noteCRNA.pdf)
and [author data](https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/).
Equations (2)-(22) specify the distance; examples exercise hue branches and
zero-chroma cases. Rounded decimal answers are formula checks, not an IEEE
correct-rounding oracle. MASK-04D separately certifies complete response bounds.

<a id="s27"></a>
## S27 — Weighted covariance and Cholesky representation

[NumPy covariance definitions](https://numpy.org/doc/2.2/reference/generated/numpy.cov.html)
and [SciPy Cholesky definition](https://docs.scipy.org/doc/scipy/reference/generated/scipy.linalg.cholesky.html).
MASK uses explicit relative weights and population normalization, complete
covariance, diagonal epsilon-squared regularization and a Float64 Cholesky model.
These mathematical references do not establish the rounding of a library
implementation or substitute for the explicit MASK staged model contract.
