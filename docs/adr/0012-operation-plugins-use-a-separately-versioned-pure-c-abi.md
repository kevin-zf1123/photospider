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

Library-owned tables remain mapped while their destroy callback runs. The host owns copied metadata and validates generic `Value` callback output. Dependency programs can retain authorized input-owner handles until explicit release or state destruction; their per-poll input/output pointers remain borrowed. Planar callbacks write through host-owned row buffers, and GPU tokens retain allocations until release or callback retirement. These lifetimes differ and are detailed in [Plugin ABI](../kernel-architecture/Plugin-ABI.md).

## 3. Formal Contracts & APIs

```c
#define PS_OPERATION_ABI_VERSION_11 11U
uint32_t ps_operation_plugin_get_abi_version(void);
const ps_operation_plugin_api_v11 *ps_operation_plugin_get_api_v11(void);
#define PS_DATA_PROVIDER_ABI_VERSION_1 1U
#define PS_PLANAR_OPERATION_ABI_VERSION_3 3U
uint32_t ps_data_provider_get_abi_version(void);
const ps_data_provider_api_v1 *ps_data_provider_get_api_v1(void);
```

Operation ABI v11 declares operation traits, parameter and port schemas, callbacks, output contracts, and native GPU services. The independently versioned provider ABI v1 publishes bounded data-schema records. The specialized planar interface is separately versioned as ABI v3. See [Plugin ABI](../kernel-architecture/Plugin-ABI.md) for record layouts and callback order.

The loader checks exact ABI versions and structure sizes, natural pointer/array alignment, pointer/count pairs, bounded counts and key lengths, strict UTF-8 keys, checked arithmetic, closed enum/flag combinations, required callbacks, dense fixed-output representability, callback results, output byte/facet bounds, and exactly-once destroy ownership. It rejects trailing structure bytes. Multi-record updates use copy-then-swap, so allocation failure or a later invalid record cannot publish a valid prefix. A library guard acquires ownership immediately and invokes any safely readable destroy callback before closing each rejected library. Published plugin tables are destroyed before unload.

Generic operation callbacks run synchronously. Their input views and output sink are borrowed for the call, and accepted generic Value output is copied or frozen before return. The first sink publication attempt claims the sink even when validation fails; a second attempt records a sticky violation. Host cancellation is checked first, followed by sink allocation failure, malformed image output, and duplicate publication. A backend-unavailable result after a published sink attempt does not request fallback: an accepted output becomes `OperationFailed`, while a rejected first attempt returns the sink’s typed failure. Unknown nonzero callback results become `OperationFailed`. CPU fallback is available only for an explicit backend-unavailable result from a GPU attempt whose copied traits permit it, before any sink publication attempt.

Planar callbacks use host-owned row buffers and invocation-local scratch services; they do not return a generic Value through the operation sink. GPU allocation tokens retain backing ownership until release or callback retirement. Dependency programs may retain authorized input handles until release or state destruction, while ordinary service pointers expire at the synchronous poll boundary. The registry is frozen before compiler and executor use.

## 4. Non-Goals & Explicit Boundaries
- ABI validation is not a sandbox, signature verification, package admission, or crash isolation.
- The daemon does not load operation plugins as an IPC capability.
- Provider records describe semantic schemas; they do not read files or create stored Values.

## 5. Consequences
A version or layout mismatch rejects loading/registration before publication. Malformed descriptors publish no prefix of a registry update. Callback exceptions are fenced at the boundary, but a crash or hang in native code can still affect the host process. ABI changes require rebuilding external consumers against the matching headers and package.
