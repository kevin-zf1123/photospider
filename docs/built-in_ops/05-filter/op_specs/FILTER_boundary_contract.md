---
spec_schema_version: 1
id: FILTER-boundary
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# Finite domains, anchors, origins and support

## One-dimensional extension

For length N>=1 and integer index i, mod is the nonnegative remainder.

| Mode | Mapping |
| --- | --- |
| constant | Explicit finite cval outside [0,N); no source read |
| clamp | min(max(i,0),N-1) |
| wrap | i mod N |
| reflect_half | t=i mod (2N); t<N ? t : 2N-1-t |
| reflect_whole | 0 if N=1; otherwise t=i mod (2N-2); t<N ? t : 2N-2-t |
| truncate | Exclude out-of-domain taps, only for members explicitly supporting this mode |

Apply extension separately to y/x; if either axis is outside under constant mode,
use cval once. These closed forms support arbitrarily large kernels and singleton
axes. Unqualified reflect/mirror names are invalid. See [sources](../research-sources.md)
for library terminology; the formulas above define project behavior.

## Convolution and correlation

For K[j,i] of shape [Kh,Kw], the explicit anchor satisfies 0<=ay<Kh, 0<=ax<Kw,
including odd shapes. Output global coordinate r=(y,x) is in input coordinates:

```
convolution: sum K[j,i] * E(input,y+ay-j,x+ax-i)
correlation: sum K[j,i] * E(input,y+j-ay,x+i-ax)
```

These are real-weight correlations without complex conjugation.

| Direction | full origin | valid origin | same origin |
| --- | --- | --- | --- |
| convolution | (-ay,-ax) | (Kh-1-ay,Kw-1-ax) | (0,0) |
| correlation | (ay-(Kh-1),ax-(Kw-1)) | (ay,ax) | (0,0) |

full shape is [H+Kh-1,W+Kw-1]; valid is [H-Kh+1,W-Kw+1], requiring H>=Kh,W>=Kw;
same is [H,W]. Stored index q represents r=q+origin. full/valid allow only constant
zero extension. same allows the non-truncate modes above. Invalid extents fail
preflight; swapping input and kernel is not a remedy. Nominal valid origin is not
the minimum coordinate read from the source.

## Exact support and normalization

Map each contributing tap and deduplicate source reads, retaining mathematical
multiplicity when taps map to the same source. K==+0 or -0 excludes its source and
performs no 0*Inf. The coefficient remains a Control/validation input. A dynamic
kernel reads the entire table to establish support and normalization. Source demand
is otherwise independent of source values; observing NaN does not remove required
reads. Boundary constants participate as ordinary specified operands.

Explicit normalization=none|sum|l1 uses denominator 1, sum(K), sum(abs(K)). sum requires
a nonzero exact denominator (which may be negative); l1 requires a positive one.
Required finite kernel/model constraints belong to the member. Bias follows
normalization. There is no constructor default or implicit epsilon repair. Derivative
profiles that fix normalization=none retain that fixed mathematical definition.

Sample-participation masks differ from output mixing. Normalized convolution uses
sum(K*M*I)/sum(K*M), K>=0, M in [0,1], and its explicit zero-mask exclusion rule.
This exclusion is not the fused-color alpha rule: zero alpha still participates.
An empty denominator follows explicit empty=copy_center|zero|error, with valid=0;
only copy_center consumes the center on that branch. Members may support a subset
of these policies. Constant-border mask weight is 1 where permitted; truncate has
no border tap. Ordinary convolution has no implicit mask port.

Identity/bypass validation is explicit per member. Optimizations must preserve
Control witnesses. Static sparse support may establish Exact; a radius box alone
is Conservative. Configurable boundary and anchors have no constructor defaults.
