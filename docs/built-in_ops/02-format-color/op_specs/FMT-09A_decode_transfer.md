---
spec_schema_version: 1
id: FMT-09A
parent_id: FMT-09
function: decode_transfer
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_2
clarification_status: complete
registered_operation_keys:
  - color.transfer_decode_strict
  - color.transfer_decode_accelerated_apple_silicon
  - color.transfer_decode_accelerated_x86_64
---

# FMT-09A: decode transfer

Inherit the complete [FMT-09 contract](FMT-09_transfer_contract.md) and
[scalar definitions](FMT-09_transfer_math.md). This direction has strict,
accelerated Apple Silicon and accelerated x86-64 CPU Result ABI 2 keys. The
specification remains Proposed.

## Interface and observable behavior

One input Result with one tensor member and no fields produces one `values`
output Result containing one tensor. The member uses Float32 or Float64, retains
its dtype, shape and axis positions, and preserves Result schema identity, tensor
key and batch axes. Semantic output updates transfer facets; raw output retains
the input facets. Both set `atomic_trailing_axes` to zero. Complete sample rank,
including batch and cell axes, is at most 8; sample count is at most 2^40. Axis
indices refer to cell axes and exclude the Result batch prefix. Respect and
override select one static RGB/Gray group; raw uses explicit component selection.
A may resolve the complete curve and parameters from source metadata. Explicit
assertions must match unless override is selected. Raw always specifies them.
No source interpretation is inferred from primaries, channel count or dtype.

For each requested participating coordinate q, read only input[q], perform the
family's semantic validation or raw evaluation, and output RN_dtype(D(input[q])).
Linear and gamma=1 obey the special bit-copy identity. Copy all requested
nonparticipating coordinates exactly. Alpha=0 causes no hidden-color shortcut;
alpha itself is not read when only a color component is requested.

Semantic output describes the corresponding linear quantity, preserving basis,
white, shape, channel positions and unaffected group/alpha descriptions. PQ/1886
return absolute display-linear cd/m²; OETF and ACES paths return their declared
scene-relative quantity. Raw retains applicable input metadata without asserting
that transfer decoding has produced valid linear color.

## Dependencies, errors and storage

Inherit pointwise Data/Validation/dirty mapping and all static description checks.
Static incoherence can fail even for alpha-only/Empty output; unrelated pixel
values cannot. Source transfer mismatch, unsupported units/model/dtype, selected
invalid samples, output overflow, resource and cancellation outcomes follow the
family table. Whole upstream failures keep their original scope.

Auto/view/materialize and exact produced coverage follow the family. Only
linear/gamma=1 can use a legal whole-map generic or spatial Result view; identity
still validates requested semantic inputs. Nonidentity conversion materializes
requested coverage. Intrinsic zero/cap branches still depend on the requested
source sample and cannot erase validation or dirty witnesses. A forced view of a
statically nonidentity map fails in compile/direct preflight; an identity view
whose physical mapping cannot be proven fails during observation evaluation.

## Independent acceptance fixtures

1. Gamma=2: [-2,-0,0.5,2] -> [-4,-0,0.25,4], exact in both dtypes.
   Invalid gamma (zero, negative or nonfinite) fails statically in every mode.
2. sRGB: compare threshold neighbors around 0.04045 against the independently
   evaluated rational/power formula. Negative magnitudes mirror signs. Do not
   use B(A(x)) as the branch oracle.
3. 709/2020: test both sides and ownership of 4.5*beta for every variant.
   rounded_10bit and 709 produce equal values but different transfer identity.
   Smooth coefficients are checked against independent root isolation.
4. BT.1886 with Lb=1,Lw=100: D(0)=1 and D(1)=100 exactly; finite outside [0,1]
   fails in semantic mode. Raw sufficiently negative V reaches the exact zero
   plateau. Parameters violating 0<=Lb<Lw fail before samples.
5. PQ: D(0)=0 and D(1)=10000; values on the exact zero plateau yield +0.
   Negative or above-one finite values fail semantically; raw evaluates the
   formula. Check an independent high-precision oracle around c1^m2.
6. HLG: D(0)=0; D(0.5)=RN_dtype(1/12). Float64 D(1) is approximately
   1.0000000269348073 and succeeds, but feeding that into semantic B fails its
   domain check. Float32 D(1) rounds to 1. This dtype difference is deliberate.
7. ACEScc/cct: code 2 decodes to exactly 65504. Check neighbors of h and toe
   breakpoints, below-floor encoded values, and negative cct toe values. Do not
   reinterpret the cap as a Float16 output restriction.
8. R-only query with invalid G, alpha and AOV: only R is read and checked.
   Query invalid G later to observe its failure. Alpha-only copies even a NaN
   alpha without running D. Hidden invalid R at alpha=0 still fails R's request.
9. Raw identity copies NaN payloads and signed zeros, including materialize;
   semantic identity rejects requested nonfinite color. Force view of PQ fails
   even for an alpha-only query. Check raw nonidentity NUM special values and
   unchanged-but-unverified metadata separately.

Keep the curve's scalar definition separate from RGB basis conversion and scene/display
reference conversion. An explicit D -> basis/reference stage -> E workflow composes
these operations; the registry does not choose such a chain automatically.
