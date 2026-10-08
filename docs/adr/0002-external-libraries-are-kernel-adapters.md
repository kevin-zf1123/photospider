# ADR 0002: External Libraries Stay Behind Kernel Contracts

- Status: Accepted

## 1. Core Summary (TL;DR)
The kernel exposes compiler, data, execution, and plugin contracts without exposing third-party library types. Operation implementations may use private dependencies behind those contracts. This keeps the installed kernel usable without optional libraries while allowing focused integrations.

## 2. Mental Model & Intuition

```text
consumer document / values
          |
          v
 Photospider public contracts <----> operation/provider ABI
                                          |
                                          v
                              implementation-private libraries
```

The kernel owns the typed workflow and execution boundary. A plugin owns its internal adapters and translates at the ABI boundary.

## 3. Formal Contracts & APIs

```cpp
struct WorkflowDocument;
class Value;
class Region;
class OperationRegistry;
struct ExecutionContextConfig;
```

These public C++ types define the kernel-facing data model. The Result operation and data-provider C interfaces are declared in `photospider/plugin/result_operation_plugin_api.h` and `photospider/plugin/data_provider_api.h`. Operation plugins pass fixed-width Result records and borrowed buffers; third-party objects and exceptions stay inside the plugin. The installed kernel's required external runtime dependency is C++ and Threads; optional native backends and integrations are build choices.

## 4. Non-Goals & Explicit Boundaries
- The kernel does not define file discovery, document parsing, persistence, codecs, UI, networking, or cryptographic services.
- The ABI validates interoperability. It does not isolate native code or make a plugin trustworthy.
- A plugin cannot transfer ownership of a third-party allocator or library object through the public ABI.

## 5. Consequences
A plugin author must translate data, errors, and lifecycle operations at the boundary and manage any library-global thread settings. This adds adapter work, but prevents library ABI and allocator choices from becoming kernel compatibility requirements. Build configurations that enable optional integrations acquire their corresponding dependencies.
