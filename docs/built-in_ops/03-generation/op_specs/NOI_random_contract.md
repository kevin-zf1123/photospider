---
spec_schema_version: 1
id: NOI-random
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: packing_and_mapping_pending
---

# Random identity: Philox4x64-10

Inherit [GEN common](GEN_common_contract.md). The core algorithm is Philox4x64-10;
production address packing, seed injection and floating-bit mappings require
specification and validation before any addressed sequence is registered. The
[oracle](../../../../oracle/ops/generation/README.md) tests the core against
independent upstream answers. Its addressed layout is explicitly a candidate.

## Address range and identity

Global integer x/y are signed 32. frame, draw and stream are each 0..2^32-1, carried by
existing Int64 parameters. channel is 0..65535 and internal domain is 8-bit; callers
cannot override a member's domain. Seed retains a 64-bit bit pattern. Directly encode
the 184 address bits into the 256-bit counter without lossy hashing. Specify seed
injection into the 128-bit key, field/word order, unused bits and endianness.
Counter=(packed xy,packed frame/draw,packed stream/channel/domain,0) is a candidate, not
a production sequence. No truncation or wrap on out-of-range input is permitted. Unused
bits cannot silently acquire new meaning.

Use global pixel/cell and logical channel, not rounded coordinates, SIMD lanes, node
addresses, clocks, thread IDs, tile IDs or traversal order. Candidate ordinal, iteration
and attempt mappings require explicit bounds. Random frame is a discrete identifier;
continuous time and rank-volume lookup frame have separate semantics. CPU/GPU integer
core outputs agree; floating transforms follow their named profiles. Address injectivity
does not mean unique individual samples or statistical independence across keys.

## Domain assignments

| Tag | Purpose |
| --- | --- |
| 1 | Uniform white samples |
| 2 | Box-Muller samples |
| 3 | Jittered grid |
| 4 | Finite-candidate rejection |
| 5 | FIFO Bridson |
| 6 | Seeded2D gradient lattice |
| 7 | Cellular sites |
| 8 | Poisson quantile |
| 9 | Integer looks |
| 10 | Artistic grain white source |
| 11 | Explicit random resource displacement |

The purpose partition is fixed; exact word/draw consumption is specified with production
packing. Never borrow adjacent domains after exhausting a draw range.

## Finite floating grids

The mathematical grid targets are halfopen24 j/2^24 for Float32 and halfopen53 j/2^53
for Float64, with the corresponding integer j range. open52 is (2j+1)/2^53 for
j=0..2^52-1, strictly in(0,1). These values are exactly representable. The concrete
choice of Philox output bits remains pending. Native Float32 need not equal a cast of
Float64. Box-Muller uses independent addressed open52 and halfopen53-angle values per
channel, cos branch, without neighbor-pair caching. Finite-tail and statistical-grid
differences from ideal continuous distributions must be reported rather than hidden.

Uniform integer selection uses unbiased rejection. The selected word width, threshold
and draw advancement are part of production mapping. Draw/algorithm limit failure
remains distinct from host ResourceExhausted.

GEN-08/NOI-05 point placement explicitly rounds to Float64 then corrects a rounded upper
cell endpoint with nextDown, rejecting cells with no representable interior. This
lattice rule does not alter the normal final rounding of periodic gradient coordinates.
Rank midpoint mapping has its own specified open-range rule.

## Core and sequence acceptance

Validate the official ten-round algorithm/constants using independent 4x64 known
answers, then separately validate packing injectivity, seed/key mapping and float grids.
Test signed coordinate limits, all 32-bit frame/draw/stream bits, channel/domain
separation, out-of-range rejection, nonzero origin, ROI and order invariance. Cache
identity includes core/layout/grid/member/resource versions and all relevant parameters;
seed alone is insufficient. The old4x32 sequence is not a compatibility reference.
Passing core KATs or candidate-layout tests does not freeze a production sequence or
prove all-domain numerical admission.

Sources: [Random123](https://github.com/DEShawResearch/random123),
[Philox4x64](https://thesalmons.org/john/random123/releases/latest/docs/structr123_1_1Philox4x64__R.html).
