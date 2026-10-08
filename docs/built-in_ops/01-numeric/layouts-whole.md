# NUM-09 Whole execution

The nine formal reshape, transpose and slice profile keys execute CPU Whole
Result programs. Each active input is a single-tensor Result, and its complete
sample_shape() includes any batch axes. The output is a
`photospider.tensor`/`samples` Result with the complete transformed shape as
ordinary axes; output facets and batch-axis metadata are empty. The program
publishes the complete output in global coordinates. A requested query `Q`
limits dependency observation and downstream reads, not the shape or coverage
of the published Result.

The planner validates static schemas and parameters before callbacks. For
nonempty work, the program requests each active tensor with Data, Validation and
Descriptor roles (role 13), which supplies its typed-payload validation
obligation. The callback reads through those authorized windows and computes the
whole output. An edit to observed input support invalidates the recorded output
dependency. Empty output has empty coverage and support. Slice ignores a
singleton axis's step value. When every count is one, the step port is excluded
from runtime Need and association; its static schema is still checked.
Otherwise all step entries are requested and validated, even where a particular
entry is numerically unused. Upstream, typed, domain, resource and cancellation
failures remain visible to the Run. The transform preserves selected raw
element bits, including signaling-NaN payloads.

`View` requires one affine source/output owner proven for the complete output.
Compatible fragments can join only when they share an owner and the address
mapping remains valid. Multiple owners make explicit `View` unavailable. `Auto`
uses a view when possible and otherwise materializes a complete packed output;
`Dense` always materializes that complete output. Only `ViewUnavailable`
permits `Auto` to fall back. The operation reads from authorized source windows
and does not need to pack the complete input first.
Resource, validation, upstream and cancellation errors remain errors. A sparse
query cannot make a globally non-affine view valid. Reshape proves contiguous
logical chunks, transpose permutes strides, and slice uses checked widened
stride arithmetic. Views retain source storage and resources after context
retirement. All three operations disable cross-run content caching because a
content key does not prove owner or stride identity; same-run sharing remains
available.

## Public workflow and validation

```sh
cmake --build build/kernel-dev --target photospider_numeric_layouts -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_layouts_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts apple
python3 oracle/ops/numeric/layout_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts strict
python3 oracle/ops/numeric/layout_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts apple
```

 The separate finite-address oracle
checks 6,374 successful View/Auto/Dense mappings per profile plus expected View
rejections. It independently covers negative, zero and singleton strides,
unaligned addresses and non-contiguous chunks. No current x86 execution, native
GPU execution or performance result is claimed.

Public graph fixtures test compatible same-owner fragments, multi-owner
View failure and Auto/Dense output materialization, full source support/dirty, invalid full
slice controls, excluded all-singleton step producers, complete typed rejection,
Empty, work/output/scratch limits and cancellation after admitted copying.
Raw specials in all four fenv modes and escaped storage/resource lifetime pass.
Prepared Whole view fixtures include giant zero-stride producer output,
structured consumption, original external dense owner retention, multi-owner
rejection, same-owner singleton joining and sticky allocator failures.
The separate [`prepared.cpp::whole_views` Result fixture](../../../examples/numeric_workflow/README.md#whole-result-view-preparation)
uses a compiled CPU Whole Tensor Need: it checks affine view proof, strict
multi-owner rejection, Auto collection into Root-owned private backing, an
8-byte source under an 8-byte output cap, and owner lifetime after context
retirement. It documents a Result input path.
The Result operation declares zero fixed workspace
bytes and reserves up to 4096 bytes of Host scratch for rank<=8 coordinate
vectors. Earlier regional checks do not establish Whole behavior.

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
```
