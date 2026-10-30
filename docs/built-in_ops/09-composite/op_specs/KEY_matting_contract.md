---
spec_schema_version: 1
id: KEY-matting
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# KEY: trimap matting mathematics

This mathematical contract belongs to KEY-05A and inherits [COM common](COM_common_contract.md). Image I is relative linear RGB; trimap 0 fixes alpha=0, 2 fixes alpha=1, and 1 marks an unknown. Epsilon is positive and expressed in squared RGB units.

## Laplacian and hard constraints

For every pixel-centered 3x3 window cropped to the full image, let n be its actual sample count, mu its RGB mean, and Sigma=(1/n) sum_i (I_i-mu)(I_i-mu)^T. Add the following contribution for every pair i,j in that window:

```
L_ij += delta_ij - (1 + (I_i-mu)^T * inverse(Sigma + epsilon/n * Identity)
                         * (I_j-mu)) / n
```

Using U for unknown indices and K for known indices, solve `L_UU * alpha_U = -L_UK * alpha_K`. Known values remain exact endpoints. The final matte is clamp(alpha,0,1) converted to T. These clipped windows, hard constraints and final clamp define the native variant of the [Levin/Lischinski/Weiss model](../research-sources.md#matting).

## Native solve result

Construct numerical matrices and solve in Float64. For a computed raw unknown vector x, let A=L_UU and b=-L_UK*alpha_K, with residual infinity norm R=||A*x-b||_inf. Successful numerical solving requires

```
R <= solve_atol + solve_rtol * (||A||_inf * ||x||_inf + ||b||_inf)
```

Matrix infinity norm is the maximum absolute row sum. Return R in the named raw_residual output, measured before output clamp. A fully known trimap has R=0 and requires no linear-system solve. A nonempty unknown system with no anchors or a singular block has NoSolution. A numerical method unable to meet the criterion returns NotConverged; no partly solved matte is published as a success. Operational cancellation/resource failures use their original categories. The criterion defines observable convergence, without choosing an implementation solver or precision-certification scheme.

The exact rational oracle independently assembles the stated matrix from stored finite inputs and solves small systems, then clamps and rounds each alpha to the requested dtype. Reference raw residual is exactly zero. Compare native alpha as well as its reported residual: this backward-error criterion does not imply a universal forward-error bound for an ill-conditioned system. The small fixture accuracy uses the common native tolerances.

## Support and acceptance

Alpha at unknown locations depends on the complete stated image/trimap problem. The operation returns tiles of the one global solution; tile edges neither create constraints nor crop windows. Known endpoint outputs can be copied from the labels without solving other outputs; raw_residual requires the solve when U is nonempty.

Check a fully known trimap, constant color with opposing anchors and one unknown, nonuniform color fixtures, cropped edge windows, a missing-anchor system, and cases where the raw solution requires final clipping. Separate the raw system residual from the clipped matte's residual.
