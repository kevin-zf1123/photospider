# NUM-10 Whole execution

The eighteen concatenate, gather and scatter profile keys run as CPU Whole
programs over Result tensors. Each input Result supplies one tensor member under
an arbitrary schema/member key. Its complete `sample_shape()` includes batch
axes, has rank 1..8 and contains at most 2^40 elements. The operations support
UInt8, Int64, Float32 and Float64 numeric tensors without requiring image facets
or batch topology. Output Results use `photospider.tensor`/`samples`, publish
the complete output shape as ordinary axes, and drop facets and batch metadata.

The compiler validates static schemas, shape relationships and parameters
during specialization. For nonempty Whole demand, the program requests each
active tensor with Data, Validation and Descriptor roles (role 13), which
triggers typed-payload validation. The program reads through authorized windows
and computes the complete output. Consumer projection limits observed
dependencies and reads; it does not reduce preparation, validation, computation
or publication. Empty output skips input requests and sample arithmetic.
Upstream, typed, resource and cancellation failures retain their status.

## Operation behavior

Concatenate accepts 2..256 ordered inputs with matching dtype, rank and
non-axis extents. It sums the selected axis and rejects an output larger than
2^40 elements. `layout="view"` proves one affine mapping across the complete
inputs. Compatible fragments may join when they share a physical owner, and the
proof can infer unconstrained singleton strides. An independent owner or an
incompatible mapping returns `ViewUnavailable`. `layout="dense"` reads from
authorized input windows and materializes a complete packed output; it does not
pack every complete input into a second buffer. Concatenate has no Auto mode.
The View proof covers all inputs even when the consumer requests only one slab.
Concatenate disables cross-run content caching because content identity does
not establish physical viewability.

Gather accepts a rank-one Int64 index tensor `[M]` and a static axis. It selects
a complete source slice for each index and preserves the index order, including
repeated indices. An invalid index fails even when its output coordinate lies
outside the consumer query.

Scatter accepts `base`, rank-one Int64 `indices`, and shape-compatible `updates`.
Every index is checked against the complete base axis. A coordinate with no hit
copies base bits. `scatter_replace` chooses the matching update with the greatest
input position. The aggregate operations include base first, then matching
updates in increasing position. Sum accumulates exactly and rounds or range
checks once. Minimum and maximum follow NUM-05 NaN and signed-zero ordering. An
integer overflow is attributed to the failing complete output coordinate.
Failed execution publishes no partial output.

## Storage and resources

Gather's index plan uses 16*M metadata bytes. Scatter's stable eight-pass radix
plan and sorting vector peak at 32*M metadata bytes; the second vector is
released after grouping. Coordinate state is bounded by rank eight. The phase
allocator admits fixed state and temporary buffers. The Root accounts retained
input owners, metadata and output payload. Dense concatenate allocates a
complete `N * element_size` output; View retains its source owners without an
output payload. Work is charged while scanning, sorting, locating contributors
and writing. Capacity or cancellation failures release unpublished output and
temporary state.

Concatenate can request at most 64 original ports in one Need envelope. Its
five-stage limit accommodates four input envelopes and the publication for 256
inputs. The Whole continuation retains owning input capabilities in a Root-accounted
map across envelopes; each poll borrows that poll's tensor map only for the
call.

## Workflow and validation

Build and run the public workflow and its focused behavior test with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_indexing -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_indexing_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing strict
python3 oracle/ops/numeric/index_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing strict
python3 oracle/ops/numeric/index_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing apple
```

The workflow checks nonleading-axis concatenation, gather and all four scatter
operations, same-owner view address/association, independent owners, invalid and
repeated indices, input failures outside the selected output, exact integer
cancellation/overflow, typed RGB validation, Empty and pre-cancelled requests,
negative/zero-stride reads, floating-environment preservation, cache
reassociation after source edits, escaped Result/window lifetime, and work,
payload and metadata cleanup. The focused `test_numeric_indexing_result` CTest
passes 1/1; the strict and Apple public workflows exit successfully. No current x86, native GPU
or performance result is claimed.

The existing `test_numeric_result_math_arrays` integration fixture contains additional
indexing cases, including multi-envelope concatenate requests with 65 and 256
repeated references under Dense and View.
