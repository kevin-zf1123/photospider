# NUM-08 Result Whole execution

The six `numeric.mix_*` and `numeric.smoothstep_*` operations consume three tensor Results and produce one tensor Result. The coordinator requests complete data, typed-validation, and descriptor support for each input with Need role `13`; the callback writes the complete packed output before the executor serves a consumer projection.

## Data and execution contract

Each input Result supplies one tensor member; its member key is not fixed by these operations. Metadata specialization checks the first member of every input for equal `sample_shape()` and Float32 or Float64 dtype, rank 1 through 8, positive extents, and at most 2^40 samples. `sample_shape()` includes batch axes. The output Result uses schema `photospider.tensor`, tensor key `samples`, output port key `values`, and the full shape as ordinary axes with no facets or batch axes.

```text
Input Result 0 ── Need role 13 ──┐
Input Result 1 ── Need role 13 ──┼─> owning read windows ─> Whole callback
Input Result 2 ── Need role 13 ──┘                         │
                                                          v
                                               packed transactional writer
                                                          │
                                                          v
                                        complete Result -> requested projection
```

The coordinator satisfies the three full-shape Tensor Needs before starting the synchronous callback. The callback acquires owning read windows over each input's `sample_shape()` and receives one packed writer. It computes coordinates in logical row-major order and writes through that writer. The writer is borrowed for the callback; each read window retains the authorized input backing while it is in use. The sealed output owns its backing and remains readable after its execution context retires.

The published Result retains the full output shape and global sample coordinates. Query coverage `Q` selects the observed dependency roots and the coordinates served to the consumer; it does not narrow the publication to a packed ROI Result.

The callback validates every mix factor and every smoothstep edge across the full logical shape. A failure outside the consumer's requested region still fails the run. Mix checks `t` for finiteness and membership in [0,1]. Smoothstep checks finite edges with `edge0 < edge1` before handling the input sample, so an invalid edge takes precedence over an input NaN. Invalid dynamic values report `InvalidArgument` with `FailureReason::InvalidDomain`, `FailureScope::Run`, the input port, offending bit pattern, and global coordinate. No partial output is published.

Empty output support uses metadata only: it requests no input payload, creates no sample support, and skips sample arithmetic. For nonempty output, every input has Whole support. An input edit dirties the observed output footprint; it does not change the complete-output publication rule.

## Memory, work, and failure handling

Source Results retain the original typed backing under the execution Root, and the callback's read windows keep those authorized owners alive while computing. The Whole path also allocates the complete packed output of `N × dtype width` bytes and a fixed interpolation workspace. A sparse consumer projection can therefore require work and output storage proportional to the full tensor. Resource accounting or allocation failure aborts publication and releases unpublished scratch and output storage. Cancellation is checked during input-window operations, per sample, during exact arithmetic, and before publication; the callback never publishes a partial tensor. Successful output storage remains owned by the Result.

## Current behavior evidence

Temporary acceptance cases should include batch-axis input/output shape, negative
strides, infinity clamping, NaN payload quieting, pre-cancellation, and factor/edge
boundaries. Use independent expected values for the interpolation formula.

These checks do not establish x86 execution, native GPU support or a performance result.
