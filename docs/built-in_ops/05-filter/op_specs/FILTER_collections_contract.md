---
spec_schema_version: 1
id: FILTER-collections
kind: shared_data_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# Frequency grids and multiscale collections

## FrequencyGrid/v1

Complex values use real/imag Values with identical shape and Float32/Float64 dtype.
The shared logical descriptor contains schema_revision=1, original_shape, y_axis,
x_axis, packing=full|r2c_x, forward_sign=-1, norm=backward|ortho,
index_order=unshifted|shifted, spacing_y/x, unit, spatial_origin and grid identity.
Equal shape does not prove equal grids. A frequency-response multiplier is
dimensionless; it shares sampling/original dimensions/index order without inheriting
spectrum amplitude normalization.

r2c_x changes only x extent to floor(W/2)+1. W comes from the original descriptor or
explicit orig_width; never infer 2*(K-1) for potentially odd widths. Uncompressed
axes retain negative frequencies. The k_x=0 and even-width Nyquist columns contain
conjugate y pairs; their entire imaginary columns need not vanish. Only intersections
self-conjugate in both axes have imaginary zero.

irfft accepts explicitly constructed pairs whose redundant boundary pairs are
exactly conjugate (signed zeros compare equal); no silent imaginary discard.
FRQ-02C is the explicit full-pair projection. Native rfft derives paired entries
from one strict value and its conjugate. Nonfinite samples obey numerical propagation;
a supplied pair that cannot satisfy the required conjugacy check fails that explicit
structural validation.

## FilterBands/v1

The immutable Result contains stable band_id, tensor descriptor and separately
owned payload for each band. Association fields include source_grid, original_shape,
level, role, valid_crop, sampling_step/origin, analysis/synthesis profile,
rounding_profile and padding_history. Inverses validate these fields, not array order.
Static levels and input descriptors determine every band descriptor without image
calculation. CompleteBundle describes complete association, not eager payload readiness.

Gaussian level 0 references source ownership; level l is ceil(H/2^l),ceil(W/2^l).
Laplacian levels 0..L-1 are details plus level L residual; L=0 has residual only.
Wavelet roles LL,LH_y,HL_x,HH and original lengths/crops are explicit per level,
including odd-size duplicate-last padding. Undeclared batch/channel axes are not
subsampled.

## Demand and immutable publication

Payload execution is lazy by band. Request only the band and its mathematical
ancestors, not all sibling bands. A deep low-frequency dependency may require
preceding low-frequency levels. Unless a member explicitly proves Region-local
execution inside a band, each requested band depends on the complete required
planes. This is compatible with band laziness and does not authorize whole-collection
eager materialization.

Publish the immutable descriptor association after all structural checks, then
publish individual owned payloads atomically when ready. Never expose a partial
successful band. Shared ancestors may be reused within their valid invocation/cache
identity. Identity includes source values/descriptors, requested band, profile,
parameters, phase, rounding and sampling metadata. A payload failure affects its
actual dependent bands; descriptor-association failure prevents observing the
association. Cancellation releases computation-held references after safe completion;
consumer-held owners remain valid across context destruction. Cache-off still
retains active ancestors and requested results. Account for all retained owners,
working layers and descriptors; no writable aliases or implicit disk paging.

Edited coefficients form a new immutable collection retaining compatible reconstruction
metadata. User-constructed bands are legal if shape/role/phase/profile constraints
hold; original producer identity is not required.

These are logical schema requirements. Public ABI encoding, compiler inference,
Result observations, lazy membership, owner accounting and cancellation must be
implemented and validated before registration. JSON/Python oracle representations
do not provide that runtime capability.
