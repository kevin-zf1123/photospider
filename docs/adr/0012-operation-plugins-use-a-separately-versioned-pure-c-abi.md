# ADR 0012: Operation Plugins Use Versioned C Contracts

- Status: Accepted
- Proposed successor: [ADR 0022](0022-freeze-operation-c-abi-v3.md) replaces the exact-size table rule with prefix negotiation and moves built-in operations onto the same C import path. This ADR describes ABI 6 until ADR 0022 is accepted and implemented.

## 1. Core Summary (TL;DR)
Operation plugins cross the kernel boundary through versioned C interfaces. The host validates and copies descriptors before making registry entries available. This keeps compiler metadata independent from plugin C++ object layouts while leaving native plugins in the host process and trust domain.

## 2. Mental Model & Intuition

```text
startup configuration --> load library --> validate exact table --> copy traits/schemas
                                                   |
                                      registry frozen for compiler and runs
                                                   |
                                          synchronous callback
                                                   |
                                      host validates/copies output
```

Library-owned tables remain mapped while their destroy callback runs. The host owns copied metadata and validates Result schemas and publication. C++ dependency programs can retain authorized input-owner handles until explicit release or state destruction; per-poll pointers remain borrowed. Result tensor windows and native GPU tokens retain backing owners until release or callback retirement. These lifetimes are detailed in [Plugin ABI](../kernel-architecture/Plugin-ABI.md).

## 3. Formal Contracts & APIs

```c
#define PS_RESULT_OPERATION_ABI_VERSION_6 6U
const ps_result_operation_plugin_api_v6 *
ps_result_operation_plugin_get_api_v6(void);
```

The operation-plugin C contract is the standalone Result ABI 6 table. It declares Result ports, static Tensor members, member-interval Needs, shared-stage callbacks, and Result publication. Its `publish_tensor_view` service requires a non-NULL explicit transform, while `publish_tensor_view_pieces` accepts 1..16 exact-size piece envelopes, forbids reshape in pieces, and commits a complete disjoint target partition atomically. `reshape=1, source_rank=0, axes=NULL` remains valid for a single-source view. These forms retain source Results across Roots without duplicating source Payload charges in the publishing Root. There is no current data-provider C table. See [Plugin ABI](../kernel-architecture/Plugin-ABI.md) for the current table layout and callback contract.

The loader checks exact Result ABI version and structure size, natural pointer/array alignment, pointer/count pairs, bounded counts and key lengths, strict UTF-8 keys, checked arithmetic, closed enum/flag combinations, required callbacks, input/output schema constraints, parameter bounds and exactly-once destroy ownership. Runtime publication checks actual member tile coverage. Multi-record updates use copy-then-swap, so allocation failure or a later invalid record cannot publish a valid prefix. A library guard acquires ownership immediately and invokes any safely readable destroy callback before closing each rejected library. Published plugin tables are destroyed before unload.

Result callbacks receive borrowed query and phase-service records. The host checks Needs, frozen evidence, publication coverage, resource limits, cancellation, and backend fallback before exposing a Result. GPU allocation tokens and owning tensor windows retain backing ownership until release or callback retirement; ordinary service pointers expire when the callback returns. The registry is frozen before compiler and executor use.

## 4. Non-Goals & Explicit Boundaries
- ABI validation is not a sandbox, signature verification, package admission, or crash isolation.
- The daemon does not load operation plugins as an IPC capability.

## 5. Consequences
A version or layout mismatch rejects loading/registration before publication. Malformed descriptors publish no prefix of a registry update. Callback exceptions are fenced at the boundary, but a crash or hang in native code can still affect the host process. ABI changes require rebuilding external consumers against the matching headers and package. ABI 6 has no prefix-size compatibility with ABI 5, no old getter shim, and no automatic interpretation of a missing view transform; ADR 0022 remains an unimplemented Proposed successor.
