# Built-in operation oracles

Independent reference programs for built-in operation specifications live here:

| Area | Reference programs | Usage |
| --- | --- | --- |
| Numeric and curves | [`numeric/`](numeric/) | [Numeric workflow and oracle commands](../../examples/numeric_workflow/README.md) |
| Generation | [`generation/`](generation/) | [Generation oracle README](generation/README.md) |
| Mask and morphology | [`mask_morphology/`](mask_morphology/) | [Mask oracle README](mask_morphology/README.md) |
| Filter and restoration | [`filter/`](filter/) | [Filter oracle README](filter/README.md) |

The C++ public workflow examples remain under [`examples/`](../../examples/).
An oracle checks mathematical references within its documented scope; it does not
establish that an operation is registered or accepted by the runtime.
