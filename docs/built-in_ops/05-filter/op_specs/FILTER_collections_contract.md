---
spec_schema_version: 1
id: FILTER-collections
kind: shared_data_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: partially_implemented
---

# Frequency grids and multiscale collections

## FrequencyGrid/v1

Complex values use real/imag Tensor payloads in explicit Result ports with identical shape and Float32/Float64 dtype. The shared caller-supplied static configuration contains schema_revision=1, original_shape, y_axis,
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
a supplied pair's required conjugacy is an unchecked numerical premise. Inverse execution performs no conjugacy scan or repair; an independent oracle may diagnose a malformed pair.

## FilterBands/v1

FilterBands/v1 is a logical SDK/workflow collection of statically enumerated band Results. Each band has a fixed Tensor schema and independent tile allocations. Caller-supplied static configuration records band_id, source_grid, original_shape, level, role, valid_crop, sampling_step/origin, analysis/synthesis profile, rounding_profile and padding_history. Preparation checks that configuration against band descriptors; runtime association payloads, when explicitly represented, are unchecked. Static levels and input descriptors determine all band schemas without image calculation. Compose multiple nodes when necessary to stay within existing member/port limits; this contract adds no dynamic Result directory or kernel collection type.

A workflow may expose the original source as Gaussian level 0; an operator that materializes it uses explicit COPY tasks and a fresh output identity; level l is ceil(H/2^l),ceil(W/2^l).
Laplacian levels 0..L-1 are details plus level L residual; L=0 has residual only.
Wavelet roles LL,LH_y,HL_x,HH and original lengths/crops are explicit per level,
including odd-size duplicate-last padding. Undeclared batch/channel axes are not
subsampled.

## Demand and immutable publication

Band selection occurs during compilation. Request only the selected band's Result members/ports and their mathematical ancestors, not unrelated siblings. A deep low-frequency dependency may require
preceding low-frequency levels. Unless a member explicitly proves Region-local
execution inside a band, each requested band depends on the complete required
planes. This is compatible with band laziness and does not authorize whole-collection
eager materialization.

Lowering declares each selected band task, its fixed reads/writes and any shared private SSA intermediates. Completed tasks enable internal consumers; selected exports become visible when their call retires successfully. Outputs of one call share its operational failure, while already retired calls retain their Results. Requesting more bands requires compiling that selection; published Results do not grow or lazily acquire new payload. The executor has no result cache. Cancellation drains accepted work before releasing computation-held references, and consumer-held owners remain valid after context destruction. Account for outputs, shared ancestors, descriptors and scratch within the Plan's resource bounds; inputs are immutable and output tiles have unique allocations.

Edited coefficients form a new immutable collection retaining compatible reconstruction
metadata. User-constructed bands are legal if shape/role/phase/profile constraints
hold; original producer identity is not required.

The Gabor-bank and Gaussian/Laplacian forms of FilterBands/v1 are implemented by the public types and helpers in `photospider/ops/filter.hpp`, native fixed-schema operations, and explicit COPY composition. Their directories carry declared grid, shape, crop, role, phase, rounding and padding information. Pyramid helpers split deeper collections into blocks that respect the fixed 16-member Result limit. Reconstruction accepts compatible edited or reordered bands without producer-identity checks. These paths use the frozen kernel and ABI 8. FrequencyGrid/v1 has explicit SDK `frequency::Grid`/`Spectrum` and transactional helpers in `photospider/ops/frequency.hpp`, with native FRQ-01..06 operations. The configuration remains caller-owned, never attached to Results. Its functional runtime integration is implemented; overall FRQ acceptance retains the frozen-ABI root cumulative arithmetic/refinement-stage limitation documented by FRQ-01. Wavelet-specific forms remain proposed and unimplemented here. Python/JSON oracles remain independent mathematical references; the C++ SDK and compiled native nodes provide collection structure checks and runtime execution.
