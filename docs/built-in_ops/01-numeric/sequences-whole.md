# NUM-02 Whole execution

The six formal `numeric.linspace_*` and `numeric.arange_*` keys take Result inputs and publish two independently selectable Result outputs, `values` and `axis`. Each input Result contains exactly one tensor member at any key, with complete `sample_shape()` `[1]`. The values output uses `photospider.tensor` / `samples` and shape `[count]`. Axis uses the same schema and member key with shape `[3]`; `atomic_trailing_axes=1` makes the three components one atomic observation. Both outputs use ordinary axes and drop facets and batch topology.

## Input demand and output selection

For a nonempty selected output, the Whole program requests full active scalar windows with Data, Validation and Descriptor roles (role 13). It reads those authorized windows and computes the entire selected output through a packed Result writer. The published Result retains its complete object and global sample coordinates; a consumer query does not turn it into a packed ROI Result. Selecting a single values index does not reduce the sequence computation. Selecting one axis component closes demand over all three components. Values and axis remain independent: an axis overflow does not invalidate already successful values, and an axis-only request does not allocate values.

Static `count=1` changes the active port set. Both outputs use `start` only; `end` for linspace and `step` for arange receive no runtime Need and do not contribute a Descriptor relation. The declared second input still undergoes complete static schema checks during specialization and seal. For `count>1`, both inputs are required even for endpoint-only output coverage. Empty selected outputs request no payload and perform no sample arithmetic.

The public `numeric::SequenceInput` holds a workflow input reference and an immutable single-tensor Result schema hint, not input payload. The authoring helpers `linspace_node` and `arange_node` create runnable Result workflows with explicit count and dtype parameters. Linspace defaults to Float64; arange chooses Int64 for two Int64 schemas and Float64 otherwise, with explicit dtype available.

## Arithmetic and memory

Linspace computes each sample from exact binary-rational endpoints and its ordinal, then rounds directly to Float32 or Float64. Arange computes exact `start + index * step`; integer mode checks only the final Int64 result, while floating mode rounds once to the selected dtype. Neither operation constructs samples by adding a rounded step. Axis is a three-value atomic tuple: linspace reports `[start,end,step]`; arange reports `[start,last,step]`. Its floating components use Float64 and its integer components use Int64.

The kernels use a bounded `SequenceMath` workspace admitted through the phase allocator. Values own `count * sizeof(dtype)` payload bytes, while axis owns 24 bytes. The host resource ledger charges input windows, output, scratch and work. Cancellation and resource failures release unpublished output; a failure in an unselected output does not revoke an independently successful selected output. These managed payload sizes are not an RSS bound.

## Current behavior checks

Run the focused public Result sequence fixture with:

```sh
ctest --test-dir build/kernel-dev -R '^test_numeric_sequences_result$' --output-on-failure
```

Coverage includes strict and available Apple Silicon workflows, values and axis selection, atomic tuple closure, Int64 precision above 2^53, extreme cancellation, Float32 direct-rounding cases, signed zero, caller floating-environment restoration, axis-only overflow, failure from an unrequested values overflow, Empty and pre-cancellation. Projection tests cover count-one exclusion of a failing second producer, static validation of the excluded input, and count-two required-producer failure. Resource tests cover work-limit rollback. A step change from 0.5 to 1 recomputes all 256 values; the last value is 255 and `computed_elements` is 256. After context retirement, a retained Result keeps at least 2048 Payload bytes live until its final owner is released, after which all Root resource dimensions return to zero. The latest root and installed consumer CTests each passed 1/1; the complete `apple_silicon` workflow also exited successfully. The independent Fraction/IEEE oracle passed 960 cases each for `strict` and `apple_silicon`; each case checks one selected global output index from Whole execution and is not evidence of ROI-local arithmetic. The x86 backend unavailable path is checked, not x86 execution. No performance result is claimed. The configured installed consumer is registered as `installed_numeric_sequences_result`.
