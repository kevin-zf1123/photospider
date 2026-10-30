# Grading mathematics and research sources

Member equations and the [common contract](op_specs/GRD_common_contract.md) define the Photospider behavior. Public mathematical sources establish the named models; artistic controls, selected defaults, finite neighborhoods, palette selection and composition rules are native definitions. Commercial application output matching is outside this specification.

## Repository color and curve definitions

[FMT model mathematics](../02-format-color/op_specs/FMT-11_model_math.md) and [FMT RGB basis mathematics](../02-format-color/op_specs/FMT-10_basis_math.md) define the model coefficients, white adaptation and coordinate scales. GRD production uses native floating arithmetic for these equations. The independent GRD oracle uses strict rounding at its stated stage boundaries.

[CRV PCHIP](../01-numeric/op_specs/CRV-01B_interpolate_pchip.md) defines the derivative construction and endpoint limiting used by GRD-05B. The [SciPy PchipInterpolator reference](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html) provides the primary project documentation of the shape-preserving interpolation method.

## CLF

The Academy's [Common LUT Format 3.0 specification](https://docs.acescentral.com/clf/specification/) supplies the four ASC CDL styles and the distinction between trilinear and tetrahedral LUT interpolation. GRD specifies its own floating input/output interface and arithmetic accuracy; CLF XML parsing and engine staging are separate concerns.

## CIE observer

CIE 2019, [Colour-matching functions of CIE 1931 standard colorimetric observer](https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer), DOI [10.25039/CIE.DS.xvudnb9b](https://doi.org/10.25039/CIE.DS.xvudnb9b). The dataset contains the 2-degree functions at 1 nm steps from 360 to 830 nm, sourced from CIE 018:2019 Table 6. GRD-11C specifies sampled Planck integration using these functions. The oracle includes the unchanged dataset with its attribution and CC BY-SA 4.0 notice.

## Temperature

Yoshi Ohno, [Practical Use and Calculation of CCT and Duv](https://www.nist.gov/publications/practical-use-and-calculation-cct-and-duv), LEUKOS, DOI [10.1080/15502724.2014.839020](https://doi.org/10.1080/15502724.2014.839020). This supplies the CCT and signed distance-from-locus interpretation. GRD-11C defines its own sampled locus, tangent, units and adaptation direction explicitly.

## OKLab

Björn Ottosson, [A perceptual color space for image processing](https://bottosson.github.io/posts/oklab/), explains the XYZ/LMS and cube-root construction. GRD uses the matrices fixed in the repository FMT mathematics. Vibrance and nearest-segment palette quantization are separate native formulas.

## Local thresholds

Sauvola and Pietikäinen, [Adaptive document image binarization](https://doi.org/10.1016/S0031-3203(99)00055-2), Pattern Recognition 33(2), 225–236, 2000. The [scikit-image filter reference](https://scikit-image.org/docs/stable/api/skimage.filters.html#skimage.filters.threshold_sauvola) states the local mean/standard-deviation threshold equation. GRD-14D fixes population variance, normalized input, finite cropped neighborhoods and threshold polarity. GRD-14C defines its finite Gaussian-weighted mean directly.

## Bilateral base and detail

Durand and Dorsey, [Fast Bilateral Filtering for the Display of High-Dynamic-Range Images](https://people.csail.mit.edu/fredo/PUBLI/Siggraph2002/), SIGGRAPH 2002. The paper motivates edge-preserving base/detail separation. GRD-19 defines its own log2 floor, finite square support, cropped boundary and exposure/detail controls.

## Quantiles

The [NumPy quantile reference](https://numpy.org/doc/stable/reference/generated/numpy.quantile.html) documents linear sample quantiles and weighted inverse-CDF selection. GRD-24 specifies binary linear interpolation and weighted inverse-CDF as distinct estimators, including empty-selection behavior, ordered ties and explicit ROI. The reference does not prescribe an implementation library.

## Histogram equalization

The [OpenCV histogram equalization and CLAHE tutorial](https://docs.opencv.org/4.x/d5/daf/tutorial_py_histogram_equalization.html) explains cumulative-histogram remapping and local contrast limiting. GRD-25 explicitly defines floating feature domains, selection weights, cropped cells, identity cells and binary/weighted redistribution. Those choices define native output behavior.

## Dither and halftone

The ImageMagick project [Color Quantization and Dithering](https://usage.imagemagick.org/quantize/) distinguishes level quantization, ordered patterns, noise and error diffusion. GRD-29 fixes its own thresholds, scan recurrence and palette metric. GRD-30 defines circular area rank, line screens, finite subpixel averaging and artistic coverage composition directly.

## Strict reference arithmetic

The [GNU MPFR manual](https://www.mpfr.org/mpfr-current/mpfr.html) defines directed rounding for the oracle's transcendental enclosures. Rational expressions use exact fractions and direct IEEE nearest/ties-to-even conversion. A reference whose enclosure cannot certify a rounding result is reported as unresolved rather than returned as an approximation.
