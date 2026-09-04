# Compiler and Execution Slice Definition of Done

A compiler/execution change is complete only when all applicable items below
are true.

## Design and identity

- The change names the affected stage: document, semantic IR, optimized IR,
  physical plan, runtime execution, or public result.
- Stage identities remain distinct and canonical digest inputs are explicit.
- Operation semantic traits cover every new type/shape/Region/layout/backend
  influence.

## Correctness

- Duplicate/missing/cyclic graph errors and malformed IR/plan input fail
  before publication.
- Integer/byte-count overflow, bounds, alignment, pointer/count, shape, Region,
  layout, facet, and buffer validation is present where applicable.
- Cancellation and stale completion cannot publish.
- Exceptions are fenced and every resource/lease is released exactly once.
- CPU is functional; optional GPU selection and permitted fallback are tested.

## Product boundary

- The kernel contains no daemon Session/Job registry or result identity.
- The daemon consumer uses only an isolated installed public package.
- Internal IR, plugin paths, and native handles do not cross local IPC.
- No removed service, durable-work, worker-process, policy DSO, plugin-security,
  durable-result, or evidence product returns through an option or stub.

## Verification and documentation

- Focused unit/integration/negative/concurrency tests pass.
- Installed public header/export/consumer inventory passes when affected.
- English public documents, Chinese mirrors, GitHub Issues/Projects, and the
  checked-in delivery snapshot agree.
- Private OpenSpec working notes are outside the public completion gate.
- Actual commands and limitations are recorded; unrun gates are not claimed.
