# Built-in operation oracles

Independent reference programs for built-in operation specifications live here:

| Area | Reference programs | Usage |
| --- | --- | --- |
| Numeric and curves | [`numeric/`](numeric/) | [Numeric oracle README](numeric/README.md) |
| Ordered numeric reductions/scans | [`numeric/ordered_reference.hpp`](numeric/ordered_reference.hpp) | Strict binary64 left-fold reference |
| Format/color mapping and conversion | [`format_color/`](format_color/) | Independent coordinate, byte, tile and quantization references |
| STMap | [`stmap/reference.hpp`](stmap/reference.hpp) | Scalar boundary/interpolation reference and sample inputs |
| Generation | [`generation/`](generation/) | [Generation oracle README](generation/README.md) |
| Mask and morphology | [`mask_morphology/`](mask_morphology/) | [Mask oracle README](mask_morphology/README.md) |
| Filter and restoration | [`filter/`](filter/) | [Filter oracle README](filter/README.md) |

An oracle checks mathematical references within its documented scope; it does not
establish that an operation is registered or accepted by the runtime.
