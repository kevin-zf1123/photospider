# GitHub Actions CI

Kernel CI validates useful build, test, runtime, and installed-package signals.
It does not implement enterprise approval, migration-provenance, or evidence
aggregation.

## Kernel workflow

The maintained kernel workflow performs:

1. CMake/Ninja configure on current Ubuntu and macOS runners for both the
   default static kernel and the supported shared kernel;
2. full build of the dependency-neutral kernel and maintained tests in each
   library mode;
3. complete CTest in each mode, including the isolated install/package
   consumer that builds and runs linked C and C++ code;
4. separate Linux ASAN and TSAN configure/build/test jobs.

Optional GPU compilation/runtime runs only on a compatible runner. CPU tests
remain required and GPU-unavailable paths validate planner fallback. ASAN/TSAN
may run as separate supported-toolchain jobs without changing product APIs.

## Cross-repository boundary

Kernel CI does not checkout the daemon. Daemon CI checks out a matching kernel
feature branch when one exists, otherwise kernel main, builds and installs it
to an isolated prefix, then configures only against that public package. A
private header, copied IR, or daemon-to-kernel source-tree include is a failure.

Daemon CI validates local IPC v3, Session/Job lifecycle, cancellation, restart
loss, result release, shutdown, package consumer, public dependency inventory,
and malformed frames. It contains no IPC v2 four-cell compatibility gate.

## Test ownership

CTest/CI runs long-lived product behavior only. Migration residue, stale-term
searches, source-layout completion, Doxygen audits, Issue replay, result
orchestration, and provenance reports remain manual development checks and are
not registered.

## Test output

The normal jobs emit CTest JUnit plus runner logs. Those files are ordinary CI
diagnostics and never become runtime product objects or release authority.

## Local parity

Developers run native focused checks and one final native clean pass described
in [Testing and Validation](../development/Testing-and-Validation.md). Local
Docker or architecture emulation is not required to mimic hosted CI.
