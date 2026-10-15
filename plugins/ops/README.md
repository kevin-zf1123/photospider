# Repository-owned operations

`plugins/ops/` contains bundled operations, workflow-authoring helpers and the
shared domain SDK. `registry.cpp` aggregates registration; callbacks and schemas
remain near their implementations. Operations and SDK code use kernel public
interfaces, with no dependency on `src/lib` headers.

| Directory | Responsibility |
| --- | --- |
| `01-numeric` | Arithmetic, curves, expressions, LUT application/baking, numeric reports and quality helpers |
| `02-format-color` | Channel, alpha, numeric-format, transfer, RGB basis and model conversion operations |
| `09-composite` | Layer value types and pure arithmetic implementation |
| `common/domain` | Explicit descriptions, codecs, ICC/OCIO resource wrappers and exact domain mathematics |
| `include/photospider/ops` | Public workflow-authoring and registry headers |

## Libraries and public interfaces

`Photospider::ops` implements the bundled operations and authoring functions. It
links `Photospider::ops_sdk` and the header-only `Photospider::ops_headers` target.
The SDK links only kernel public interfaces and may be built with ops disabled.
Its public headers live in the repository's main `include/photospider/ops/` tree;
both include roots use the installed `photospider/ops/` namespace.

Link `Photospider::ops` to call `make_default_operation_registry` or the compiled
authoring helpers. Link `Photospider::ops_sdk` for standalone configuration,
resource, report, LUT bake and Layer helpers. `PHOTOSPIDER_BUILD_OPS` requires
`PHOTOSPIDER_BUILD_OPS_SDK`; kernel-only builds may disable both.

Results and Values carry structure, data and storage owners. Domain descriptions
are explicit static parameters, independently supplied to every consumer.
Description editing uses the SDK's pure `format::edit_description` function;
there are no metadata assignment/removal workflow nodes. Domain validators and
report codecs are registered explicitly with the generic kernel services.

Operation behavior and examples are documented in
[`docs/built-in_ops/`](../../docs/built-in_ops/README.md) and the
[kernel architecture guide](../../docs/kernel-architecture/README.md).
