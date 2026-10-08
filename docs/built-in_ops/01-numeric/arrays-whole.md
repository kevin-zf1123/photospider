# NUM-03 Whole execution

All six `numeric.constant` and `numeric.broadcast` profile keys execute CPU
Whole Result operations. Constant View copies one scalar into an independent
zero-stride Result owner; it preserves the whole-array tuple identity without
retaining an oversized scalar backing. Broadcast View preserves the complete
source Result owner, including its original strides. If the source has multiple
owners, View returns `ViewUnavailable`; Dense can collect those owners. Both
Dense operations publish the complete target before consumer projection. An
Empty request reads no samples, while nonempty requests validate all active
source data. A source edit invalidates the complete output observations.

Constant Dense grows an already-filled prefix, then copies blocks of at most
64 KiB without overlap. Cancellation polls between these byte-bounded blocks.
Broadcast Dense retains coordinate mapping and 32-byte Scalar/NEON/AVX2 copying
with exact tails. These are raw bit operations, so exact bit equality applies
to all three profiles, including sNaN payloads, signed zero and integers.
No NUM-14 certificate or floating approximation is involved.

## Public workflow and validation

The current Result behavior is covered by `test_numeric_result_arrays`:

```sh
cmake --build build/kernel-dev --target test_numeric_result_arrays -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_arrays$' --output-on-failure
```

The focused test covers dense output and schema rejection, structured consumption
of giant views, Result cache rebinding, owner lifetime, raw bit patterns and
constant/broadcast boundary cases. It checks all 256 UInt8 values, Int64
extrema, Float32/64 special values, all four floating-point rounding modes, and
negative unaligned axis permutations. A structured input with one batch axis
of extent 2 and cell shape `{3}` verifies that the consumer reads the last
sample using batch coordinate 1 and cell coordinate 2.
Strict and available accelerated profiles run; on the tested Apple Silicon host,
the x86 profile reports `BackendUnavailable`. These checks establish correctness
and ownership behavior, not a current Result performance baseline.

The `photospider_numeric_arrays` executable composes the same Result operations
with the remaining NUM-03 workflow cases. Its full manual run is separate from
the focused CTest above; the test command is the targeted validation entry point.
