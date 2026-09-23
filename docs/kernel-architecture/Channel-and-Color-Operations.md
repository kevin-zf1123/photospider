# Channel and color operations

Package 0.20.0 retires the old channel, alpha and color operations, together with
`numeric.cast` and `numeric.encode_range`. Their source, default registrations
and dedicated old tests are removed. The 13 exact keys and ownership boundaries
are listed in the [retirement record](../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md).
Lookup, invocation and compilation of those keys return `NotFound`; there are
no aliases or registered compatibility stubs.

The [format/color catalog](../built-in_ops/02-format-color/representation.md) and
[FMT common contract](../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
define the replacement direction. FMT-01..08 remain Proposed and unimplemented;
FMT-07 is retired. Complete images use planar storage and straight color with
alpha inside the tensor. New operators must implement their own exact demand,
metadata, numerical and layout contracts. Historical typed HWC behavior does
not constitute a subset implementation of these new specifications.

Shared ColorArray descriptions, profile ownership and mathematics used by
maintained NUM/CRV operations remain. Their presence does not register a format
conversion or imply support for the new FMT families.

The [public retirement regression](../../tests/integration/test_format_color_retirement.cpp)
checks all removed keys through lookup, direct invocation and compilation,
and executes `numeric.add_strict` with an independently checked result of 0.5.
The installed consumer builds and runs the same source:

```sh
cmake --build build --target test_format_color_retirement -j 8
ctest --test-dir build -R '^(test_format_color_retirement|test_installed_consumer)$' --output-on-failure
```

The [Chinese mirror](zh/Channel-and-Color-Operations.zh.md) describes the same boundary.
