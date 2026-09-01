# Testing and Validation

This document defines maintained repository validation after the breaking
scope reset. Tests validate long-lived software behavior, not migration
completion or provenance.

## Development loop

During implementation, use scoped formatting/lint, affected targets, and
focused tests. After source and documentation freeze, run at most one native
clean configure, one full build, and one complete CTest/JUnit pass. Do not use
Docker or local architecture emulation for that final pass.

## Required behavior areas

Kernel tests cover:

- WorkflowDocument and graph/IR/plan validation;
- typed stage identity and canonical digest separation;
- cross-registry IR/plan rejection even for equal operation keys;
- CPU compile-plan-execute and optional GPU selection/fallback;
- multiple independent graph/execution contexts;
- bounded ready work and `ResourceLedger` settlement;
- cross-backend copy/backend labels, cancellation, stale completion, and
  exception fences;
- Value/Region/strided-layout/facet/buffer negative contracts;
- operation/provider ABI version/size/alignment/pointer/count/bounds/lifetime;
- raw benchmark diagnostics without verdict/evidence output.

Daemon tests live in `photospider-daemon` and cover local frame validation,
nine-method routing, ephemeral Session/Job lifecycle, restart loss,
multi-Session behavior, cancellation, Session close, result release, shutdown,
and the isolated installed-kernel boundary.

## Installed boundary

The package gate configures and installs Photospider to a fresh prefix, then
configures an external C++17 consumer using only
`find_package(Photospider CONFIG REQUIRED)`. It verifies:

- installed headers match the declared public inventory;
- exports contain no source/private paths;
- the embedded compile/execute facade links and runs;
- `kernel`, `operation_sdk`, and `data_provider_sdk` component discovery
  exports exactly `Photospider::kernel`, `Photospider::operation_sdk`, and
  `Photospider::data_provider_sdk`; the SDK targets are header-only;
- removed targets, headers, components, and executables are absent.

Daemon validation must use that isolated prefix, never a sibling checkout or
private include directory.

## Sanitizers and malformed-input validation

ASAN and TSAN are scoped CMake modes where the toolchain supports them. The
ordinary tests exercise malformed Value/Region/layout, graph documents,
operation/provider records, and callback outputs. Malformed local IPC frames
belong to the daemon repository. No fuzz executable is currently maintained;
adding one requires an ordinary long-lived correctness target and corpus
policy rather than migration-specific wiring.

## CTest ownership

CTest/CI entries are reserved for correctness, performance, stability,
multithreading, error handling, package consumption, compilation, and runtime
boundaries. Do not register stale-term searches, source-layout audits,
migration checklists, Doxygen audits, Issue replay, or result/provenance
orchestration. Manual source-quality tools require maintained English and
Chinese documentation and remain outside CTest/CI.

## Final commands

The exact final build directory and optional capability flags are recorded in
the completion report. The normal shape is:

```bash
cmake -S . -B <clean-build> -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON
cmake --build <clean-build> -j
ctest --test-dir <clean-build> --output-on-failure --output-junit <report>
cmake --install <clean-build> --prefix <fresh-prefix>
```

Format changed C/C++ with ClangFormat 21 and lint the same files with
`python3 -m cpplint`. Record unsupported sanitizer/GPU platforms as limitations
rather than successful gates.
