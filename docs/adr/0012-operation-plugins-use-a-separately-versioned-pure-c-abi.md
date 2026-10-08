# ADR 0012: Operation Plugins Use Versioned C Contracts

- Status: Accepted

## 1. Core Summary (TL;DR)
Operation plugins and data providers cross the kernel boundary through versioned C interfaces. The host validates and copies descriptors before making registry entries available. This keeps compiler metadata independent from plugin C++ object layouts while leaving native plugins in the host process and trust domain.

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
#define PS_RESULT_OPERATION_ABI_VERSION_2 2U
const ps_result_operation_plugin_api_v2 *
ps_result_operation_plugin_get_api_v2(void);
#define PS_DATA_PROVIDER_ABI_VERSION_1 1U
const ps_data_provider_api_v1 *ps_data_provider_get_api_v1(void);
```

The operation-plugin C contract is the standalone Result ABI 2 table. It declares Result ports, tensor members, fields, staged callbacks, dependency relations, and Result publication. The data-provider C table remains independently versioned at ABI 1. The former base operation ABI 11 and planar operation C table have been removed; C++ `Value` and dependency APIs remain separate in-process interfaces. See [Plugin ABI](../kernel-architecture/Plugin-ABI.md) for the current table layout and callback contract.

The loader checks exact Result ABI version and structure size, natural pointer/array alignment, pointer/count pairs, bounded counts and key lengths, strict UTF-8 keys, checked arithmetic, closed enum/flag combinations, required callbacks, input/output schema constraints, parameter/facet bounds and exactly-once destroy ownership. Runtime publication checks actual tensor and field coverage. Multi-record updates use copy-then-swap, so allocation failure or a later invalid record cannot publish a valid prefix. A library guard acquires ownership immediately and invokes any safely readable destroy callback before closing each rejected library. Published plugin tables are destroyed before unload.

Result callbacks receive borrowed query and phase-service records. The host checks Needs, typed relations, publication coverage, resource limits, cancellation, and backend fallback before exposing a Result. GPU allocation tokens and owning tensor windows retain backing ownership until release or callback retirement; ordinary service pointers expire when the callback returns. The registry is frozen before compiler and executor use. The former Base C ABI is not loaded as a fallback.

## 4. Non-Goals & Explicit Boundaries
- ABI validation is not a sandbox, signature verification, package admission, or crash isolation.
- The daemon does not load operation plugins as an IPC capability.
- Provider records describe semantic schemas; they do not read files or create stored Values.

## 5. Consequences
A version or layout mismatch rejects loading/registration before publication. Malformed descriptors publish no prefix of a registry update. Callback exceptions are fenced at the boundary, but a crash or hang in native code can still affect the host process. ABI changes require rebuilding external consumers against the matching headers and package.
