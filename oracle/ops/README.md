# Built-in operation oracles

Independent reference programs for built-in operation specifications live here.
Each area groups existing references by specification family (`num01/`, `crv01/`,
`fmt09/`, `gen01/`, `noi01/`, `pth01/`, `mask01/`, `fil01/`, `frq01/`,
`res01/`, `geo06/`). Shared helpers and cross-family fixtures live in `common/`.

| Area | Reference programs | Usage |
| --- | --- | --- |
| Shared spatial samples | [`common/spatial_samples.py`](common/spatial_samples.py) | 5×7 mask/RGBA samples, row-major indexing and the Float32 absolute-error predicate |
| Numeric and curves | [`numeric/`](numeric/) | [Numeric oracle README](numeric/README.md) |
| Ordered numeric reductions/scans | [`numeric/common/ordered.hpp`](numeric/common/ordered.hpp) | Strict binary64 left-fold reference |
| Format/color mapping and conversion | [`format_color/`](format_color/) | Independent coordinate, byte, tile and quantization references; FMT/alpha goldens and generators |
| STMap | [`transform/geo06/reference.hpp`](transform/geo06/reference.hpp) | Scalar boundary/interpolation reference and sample inputs |
| Generation | [`generation/`](generation/) | [Generation oracle README](generation/README.md) |
| Mask and morphology | [`mask_morphology/`](mask_morphology/) | [Mask oracle README](mask_morphology/README.md) |
| Filter and restoration | [`filter/`](filter/) | [Filter oracle README](filter/README.md) |

An oracle checks mathematical references within its documented scope; it does not
establish that an operation is registered or accepted by the runtime.
