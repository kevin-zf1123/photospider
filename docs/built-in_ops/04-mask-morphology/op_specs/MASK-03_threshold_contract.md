---
spec_schema_version: 1
id: MASK-03
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-03: Hard and soft scalar selection

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-03A](MASK-03A_threshold.md) | threshold | Hard scalar threshold |
| [MASK-03B](MASK-03B_range_mask.md) | range_mask | Hard interval mask |
| [MASK-03C](MASK-03C_soft_threshold.md) | soft_threshold | Soft centered threshold |
| [MASK-03D](MASK-03D_soft_range.md) | soft_range | Soft interval selector |
| [MASK-03E](MASK-03E_nonzero_to_binary.md) | nonzero_to_binary | Explicit UInt8 truthiness adapter |

## Normative shared mathematics and interface

A-D consume finite Float32/Float64 Field, preserve its floating dtype and produce
coverage, with binary meaning for A/B. They do not consume color descriptions.
E explicitly adapts raw UInt8 truthiness to Float32/Float64 Binary using required
output_dtype; raw UInt8 is not an admitted mask storage type. Color-to-gray
and integer interval conversion remain explicit FMT stages.

Define clip01(t)=max(0,min(1,t)); L(t)=clip01(t), S(t)=u*u*(3-2*u), u=clip01(t).
`curve` is the required static enum linear|smoothstep for C/D. `F` denotes that
selected exact real function, not a rounded intermediate. A comparator is ge,
gt, le or lt. B accepts independent lower_closed/upper_closed Bool parameters;
lo<=hi; a degenerate interval contains a point only if both endpoints are closed.
C uses full transition width w>=0, centered at threshold t: w=0 takes x>=t;
otherwise F((x-(t-w/2))/w). D uses lo<=hi and two full edge widths wl,wu>=0:
`min(Ledge,Uedge)`, with Ledge=C(x,lo,wl) and Uedge=1-F((x-(hi-wu/2))/wu)
when wu>0, else `[x<=hi]`. At zero lower width use `[x>=lo]`.
Overlapping transitions remain min, not multiply, renormalize or reorder.

Construct endpoint expressions exactly even when t±w/2 would overflow a
floating intermediate; only the final output is rounded. All hard comparisons
are exact with actual stored inputs and Float64 thresholds. No NaN->false raw
exception is introduced here. Zero-width endpoints precede division. Cost O(Q),
constant scratch. Conceptual workflow: FMT gray/selected scalar -> selector ->
MASK-05/08/17. Future public workflows must not use the old Whole threshold as
an exact-demand implementation of A.

## Sources, independent evidence and review boundary

[S02](../research-sources.md#s02)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
