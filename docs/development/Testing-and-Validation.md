# Testing and Validation

This document defines repository-level testing and validation behavior. It is
development guidance rather than a description of kernel runtime architecture.

## Mainline macOS Architecture

Mainline macOS development targets Apple Silicon `arm64`.

The project does not intend to preserve mainline `x86_64` macOS support. If
future users need `x86_64`, it can be handled by a branch, fork, or dedicated
compatibility effort.

On Apple Silicon, the compiler target, terminal architecture, and dependency
architecture should all agree on `arm64`. Architecture mismatch between an
`x86_64` build and `arm64` Homebrew libraries is not a supported mainline setup.

## Build Configuration Direction

Developer setup should make architecture selection explicit. CMake presets or
bootstrap notes should default macOS to `arm64`.

The repo should also document or provide:

- `compile_commands.json` generation
- lint and formatting commands
- the expected local validation command set

The root CMake configuration exports `compile_commands.json` and defaults
mainline macOS builds to `arm64` when no `CMAKE_OSX_ARCHITECTURES` value is
provided.

On macOS, each install-consumer smoke reads the selected producer's resolved
`CMAKE_OSX_ARCHITECTURES` cache value and passes the exact meaningful value to
every external CMake configure as one argument. Semicolon-separated universal
architecture lists therefore remain intact, and the producer, installed static
archives, and all consumers stay on one architecture profile even when a
Rosetta-launched outer runner would choose another compiler default. This
propagation is Darwin-only; Linux and Windows children never receive the
macOS-specific option. It does not create or preserve a supported mainline
`x86_64` path.

The declared CMake 3.16 minimum is a compatibility floor for the installable
static product's producer path and downstream package consumption; it is not a
fixed toolchain that every pull request must run. Any policy introduced after
that floor, such as `CMP0135`, must be guarded with `if(POLICY <policy>)`.
Compatibility is maintained by that policy guard, the current GitHub
integration package consumer, and a targeted native old-version run only when
a compatibility-sensitive change or release check warrants one.

When a targeted minimum-version run is performed, it starts from a fresh producer
build tree, configures the top-level project with CMake 3.16 and
`BUILD_TESTING=OFF`, builds the real `photospider` target, installs to a fresh
prefix, and only then configures, builds, and runs an external
`find_package(Photospider)` consumer. It must not reuse a producer tree
configured by a newer CMake or substitute an internal helper target. If no
natively compatible old CMake runtime is available locally, skip that targeted
local run; architecture emulation is not required.

The package-consumer smoke recreates its transient install, consumer source,
and consumer build directories without suppressing cleanup failures. It checks
the observed producer/install/consumer behavior in memory and streams commands,
child output, and assertion diagnostics to stdout/stderr for CTest to capture.
All generated files remain in its transient work directory and are discarded
after the run; the repository does not retain per-run reports for this test.

`BUILD_TESTING` controls availability of internal test products, not how the
installed `photospider` archive compiles the Issue #72/#75/#76/#82 observation
seams. The product source inventory is divided into common objects, compiled
once, and production objects for `compute_task_submission.cpp`,
`dirty_update_executor.cpp`, `execution_service.cpp`,
`resource_demand_estimator.cpp`, `graph_cache_service.cpp`,
`graph_state_executor.cpp`, `kernel.cpp`, and `kernel_compute.cpp`. The real
archive always uses the production form of those eight translation units, with
no `PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING`,
`PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING`,
`PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING`,
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING`, or
`PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING` or
`PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING` observer/probe definitions,
globals, branches, or symbols. Focused tests link a non-installed
`photospider_internal_test_product` that reuses the same common objects and
recompiles only those eight translation units with the deterministic seams.
No target links both complete archives, and the test product is absent from
install and export sets. The Issue #75 probe declarations are source-tree-
private free functions, so the macro does not change the production
`ExecutionService` class definition or object layout. The Issue #82 dirty
post-plan observer is likewise a source-tree-private free function backed by
test-product-only thread-local state; it changes no production class
definition or object layout. The sequential-lease admission observer,
retained-operation-string charge observer, and exact direct-resource estimator
follow the same boundary: they are source-private free functions backed by
test-product-only atomic state or authority-free calculation. The direct gate
predicate diagnostic is a private test-access method defined only by the test
product. Its declaration remains token-identical in the source-private class
definition shared by common and seam objects, but adds no object state or
installed surface and grants no authority. The production operation gate and
estimators contain no observer state or notification branch, and no diagnostic
or estimator mints resource or gate ownership. `test_compute_run`,
`test_compute_service_split`,
`test_host_adapter`, `test_kernel_contracts`, and `test_policy_execution` are
the complete direct-consumer set of this internal archive.

`StaticProductConsumerSmoke` enforces that boundary for both
`BUILD_TESTING=ON` and `BUILD_TESTING=OFF` producer configurations. After the
real product is installed to a non-system temporary prefix, the smoke reuses
the daemon capability driver to remove LD/DYLD loader overrides and execute
installed `photospiderd --help`; a missing relocatable operation runtime
therefore fails instead of passing on file existence. Darwin then invokes and validates
`xcrun --find llvm-nm`, then falls back to PATH `llvm-nm` and PATH `nm`;
non-Darwin platforms never invoke `xcrun` and use the two PATH candidates in
that order. Canonically identical executable paths run once. A candidate is
usable only when it starts, exits successfully, emits symbols, and exposes the
nine required anchors spanning all eight production seam objects. Otherwise
the smoke records a path-free failure reason and tries the next candidate; no
candidate or all unusable candidates fail closed. The first usable full symbol
table is authoritative and rejects every hook function/helper/global fragment.
The raw table is used only in memory for that decision; a forbidden symbol in the
first usable table fails the verdict without trying a later candidate. The
retained scan observation has a closed, path-free schema: stable `tool_source`,
ordered structured attempt reasons, status and aggregate line/anchor/prohibited
counts, plus counts keyed only by controlled symbol tokens. It retains no tool,
archive, object, build, install, or workspace path; no raw symbol line or
captured stdout/stderr; and no environment `PATH`. If aggregate package behavior
fails, the JSON diagnostic is a whitelist projection of failed check labels,
command statuses, and that sanitized scan observation rather than the complete
transient observations. The smoke also rejects an installed test product
archive, exported test target, or exported internal seam definition. This
remains a labelled `build-smoke`; ordinary complete CTest selection does not
make package construction part of runtime-test ownership.

`PhotospiderdInstallLayoutSmoke` separately configures three isolated
dependency-disabled producer trees. It builds only the `photospiderd` target
closure, then installs the configured package with nested relative
`libexec/photospider` and `lib64` directories, an absolute libdir, or an
absolute bindir paired with a relative libdir. Every case uses its configured
prefix, removes loader overrides through the shared capability driver, and
executes the installed daemon. The default relative `bin`/`lib` case remains
part of `StaticProductConsumerSmoke`. All matrix build/install directories and
absolute destinations are strict descendants of the CTest work root and are
removed after either success or failure.

The configured producer also serializes
`PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS` into a build-tree
inventory using install-relative `include/photospider/...` paths. Before it
writes any record, the CMake 3.16-compatible writer rejects a backslash, every
CMake-representable ASCII C0 control (codes 1 through 31), and DEL; diagnostics
identify the allowlist position without reproducing the rejected field.
CMake strings cannot represent NUL, so the reader independently rejects all
C0 controls including NUL, plus DEL, in a forged or externally modified
manifest. LF is the only record separator. Ordinary spaces remain legal POSIX
path data.

The smoke rejects a missing, empty, duplicate, control-bearing, backslash-
bearing, noncanonical, or non-header entry. It applies an exact
`PurePosixPath` spelling/root/suffix check before generating the external
consumer's include list, then requires the installed include tree to equal the
configured path set. Missing and unexpected files therefore fail the same
exact comparison; neither the driver nor the documentation maintains a second
public-header count, and an unallowlisted source-tree file cannot silently
widen the package surface. The safety regression round-trips an ordinary-space
path through the production CMake writer and parser, and proves that every
CMake-representable control and both parent-like and ordinary backslash paths
fail before serialization.

The smoke inspects every installed `Photospider*Targets*.cmake` file because
the package separates base, OpenCV-dependent, and embedded-product targets
into distinct export sets. Its dependency classifier recognizes only the exact
OpenCV component target spellings that the producer accepts: bare lowercase
names, lowercase `OpenCV::opencv_*` targets, and component-specific CamelCase
targets such as `OpenCV::Core`; partial-name matches remain rejected. This is
validated through the real exported package/consumer behavior rather than a
synthetic verifier self-test. With OpenCV discovery disabled, a consumer
requesting `COMPONENTS operation_sdk OPTIONAL_COMPONENTS operation_opencv`
must keep the package and `operation_sdk` found, mark `operation_opencv` not
found, import the dependency-free SDK/runtime targets, and omit
`Photospider::operation_opencv`. Requiring `operation_opencv` under the same
condition must fail package discovery. With OpenCV available, the adapter
consumer imports that target through only the OpenCV `core` component and does
not discover unrelated packages.

When IPC is enabled, the package smoke builds and installs `photospider`,
`photospider_ipc_client`, and `photospiderd`. It independently configures a
default embedded consumer of `Photospider::photospider` and an IPC-only project
that requests `COMPONENTS ipc_client`, disables OpenCV/`yaml-cpp` discovery,
and links only `Photospider::photospider_ipc_client`. The latter therefore
resolves only Threads and does not inherit the backend or JSON implementation
target. That IPC-only consumer includes the installed protocol, Client, and Host-adapter headers,
constructs `create_ipc_host()` without contacting a daemon, executes every
safe public Client lifecycle symbol, and links a reference-only branch for all
exact unique inventories of 60 typed Client calls plus all 58 non-destructor
Host virtuals. Package
inspection also requires the IPC archive and exact three-header surface,
permits only `Threads::Threads` in the exported IPC link interface, positively
allows only the current C++ standard-library and installed `photospider/`
public includes, and rejects raw JSON, socket-address/descriptor, file-identity,
file-mapping, and backend declarations. This is the gate's exact boundary, not
an exhaustive promise about every possible POSIX spelling. With backend
discovery disabled, `COMPONENTS ipc_client OPTIONAL_COMPONENTS embedded`
succeeds with only `ipc_client` found, and an unknown optional component
remains not-found without invalidating the package.

The same smoke independently configures a C11 project that requests only
`COMPONENTS policy_sdk`, builds a pure-C ABI-v1 policy DSO against
`Photospider::policy_sdk`, and rejects OpenCV, yaml-cpp, or Threads leakage.
The generated source probes the exact policy ABI constants and layouts. The
external embedded consumer then loads that installed policy DSO and an
installed operation DSO, configures policy and execution defaults, validates
their public snapshots, and computes through both extensions. No generated
consumer receives a source-tree include directory. The operation-SDK-only
factory also uses installed `ValueBuilder`, `WriteLease`, `BufferHandle`,
`ReadLease`, runtime identities, and ImageView to publish and read an immutable
CPU DenseTensor Value. This proves that the V-3 headers and implementation
symbols are complete without OpenCV, yaml-cpp, or Threads discovery.

The durable
`IpcDisabledInstallSmoke` configures a separate clean producer with
`PHOTOSPIDER_BUILD_IPC=OFF` and `BUILD_TESTING=OFF`; it verifies that no IPC
build forwarder, installed header, archive, executable, or exported target is
advertised, a required `ipc_client` component fails discovery, and an external
default embedded Host consumer still links and runs. Required unknown package
components fail as well; optional disabled `ipc_client` and unknown components
remain not-found without failing discovery; omitting components or requesting
`embedded` retains the existing backend dependency resolution.

The durable `DependencyDisabledInstallSmoke` configures a clean producer with
OpenCV and YAML capabilities disabled, disables OpenCV, yaml-cpp, and OpenEXR
package discovery, turns off IPC, enables only the dependency-neutral test
surface, and builds the
real `photospider_kernel` aggregate, `photospider` product,
`test_cpu_dense_tensor_image_operation`, `test_packed_fp4_dense_tensor`,
`test_variable_sample_field_extensions`, and `test_value_identity_across_dsos`
binaries. Before installation it runs all 48 dense-image cases, all four packed
FP4 cases, all seventeen provider-defined VariableSampleField cases, and the
dual-DSO identity case in that actual disabled producer, including the
`register_core_operations -> OpRegistry -> NodeExecutor` invert path and Value
ownership, lease, signed-view, and cache-identity regressions. It verifies the
derived provider/plugin/CLI defaults and the precise diagnostics for three
invalid explicit combinations.
After a clean install it rejects OpenCV headers, targets, export references,
and yaml-cpp link leakage; optional `operation_opencv` remains unavailable
while the required component fails. It also performs a case-insensitive,
surface-aware optional-provider scan over every installed public header,
library/archive, package config, CMake export, generated consumer link script,
and consumer executable dependency/symbol surface. Mach-O uses `otool` and
`nm`; ELF uses `readelf` and `nm`. The bounded markers cover `OpenEXR`, the
`Imf` namespace/library family and named transitive libraries, plus the
deep-scanline, deep-tiled, deep-codec, multipart, and mixed-part vocabulary
reserved for V-15. Broad `exr`, `deep`, and unqualified `half` substring scans
are forbidden because they create unsupported false positives. An external
consumer configures with all three package discoveries disabled,
links/runs `Photospider::photospider`, allocates a neutral
image, uses `ValueBuilder`, write/read leases, runtime identities, ImageView,
and the public FP4/quantization/Blocked/PackedDenseTensorView contracts through
the installed package, loads and closes an empty Host
session, and observes `GraphErrc::Io` from an explicit YAML operation. CI may
reuse a producer only after its cache identity, configuration, complete
capability profile, and already-built dense integration target are validated.
The same external project requests `data_provider_sdk`, verifies that its
interface has no link dependency, builds separate exact-name C11 and C++17 v3
definition producers from the installed header, and links each into a separate
C++ Host consumer through `Photospider::operation_sdk`. Each consumer derives a
three-field Schema/Facet/Layout manifest from the active snapshots, publishes
bounded three-buffer provider-defined Values in compact and repacked forms,
compiles output-sink/diagnostic/property layout assertions, and exercises pure
property, DataSpec, and Region callbacks. Each producer emits a nonempty BYTES
property from callback-local storage so the installed Host proves synchronous
copy-out rather than delayed pointer access. It round-trips
unknown descriptor/Layout bytes and complete or metadata-only artifact
envelopes with the provider visible and absent, never invents an absent
ContentDigest, and checks typed Descriptor, Content, and StorageLayout digests,
including layout-independent content identity. Indexed read and provider-owner
leases keep the exact generation/module alive across unload; active resolution
then reports MissingProvider, retained Value traversal remains valid, and final
owner/provider destruction precedes module release. No source-tree include or
optional provider dependency enters either producer or consumer.

`OpenExrDeepProviderOptionOffSmoke` owns the narrower V-15 option boundary. It
configures a fresh provider-OFF producer with `BUILD_TESTING=ON` while OpenCV,
yaml-cpp, OpenEXR discovery, graph CLI, IPC, and repository operation providers
are disabled. The configure uses an expanded top-level CMake trace plus the
completed cache to require zero executed OpenEXR package lookups and zero
discovery keys. The driver performs the complete producer build, then builds
the exact `test_variable_sample_field_extensions` target and runs a nonempty
`^VariableSampleFieldExtensions\.` CTest selection with `build-smoke` excluded
as an explicit recursion guard. The current selection contains all seventeen
V-14 cases.

After installation, that smoke inventories the neutral public header, package
Config/Targets files, build-tree native products, and installed native
products. It uses the producer's supplied `CMAKE_NM`, the child toolchain's
`CMAKE_NM`, or a validated platform fallback; absence of a symbol inspector
fails closed. Defined and undefined symbol surfaces are inspected separately.
Dynamic dependencies use `otool` on Darwin, `readelf` on ELF, and
`dumpbin /dependents` or `objdump -p` on Windows; Windows cannot pass through an
empty dependency surface. A neutral installed-package consumer then performs a
real verbose compile and executable link. Its verbose output,
`compile_commands.json`, link scripts, response files, evaluated imported-
target properties, native symbols, and dependencies are scanned before the
executable runs. The default and optional-component probes must remain usable,
while a required absent component must fail with the Photospider-owned
diagnostic before OpenEXR discovery. `OpenExrDeepProviderInstallConsumerSmoke`
is the enabled companion: it installs the explicit component, loads the actual
module, resolves both v3 exports, validates the API table, invokes provider
destruction, and unloads the module.

The generated clean consumer project maintains one ordered CMake executable
target list. That same list creates the targets, writes a configure-time exact
target declaration, and supplies a configuration-specific three-field
`target<TAB>$<TARGET_FILE_NAME:target><TAB>$<TARGET_FILE:target>` manifest
through `file(GENERATE)`. The current profile declares
`dependency_disabled_consumer`, `installed_c11_data_provider_consumer`, and
`installed_cpp17_data_provider_consumer`; adding another maintained consumer
extends that CMake list and its paired source list without adding a Python
target name or discovery branch. CMake 3.16's target generator expressions are
the native spelling authority because they describe the selected generator,
target platform, and
configuration. Python's `os.name` and `sys.platform` describe the interpreter
host instead, so they must not infer the executable suffix. In particular, a
POSIX Python running under Cygwin or MSYS2 may legitimately receive a CMake
target filename ending in `.exe`.

The reader requires both manifests to be nonempty, unique, and identical in
target sequence. It rejects malformed field counts, empty fields,
blank/comment records, invalid UTF-8, non-structural ASCII C0 controls or DEL,
noncanonical target names, and missing, unexpected, duplicated, or reordered
targets. A configured filename must contain no POSIX or Windows separator, may
not be `.` or `..`, must be unique, and must equal either the exact target name
or that name plus `.exe`. Those are the only native spellings available because
the generated consumer does not customize `OUTPUT_NAME`, prefix, suffix, or a
configuration postfix. This target-to-filename binding prevents a forged new
field from selecting an arbitrary build-local executable. The full target path
must use canonical native spelling, remain unique, stay at the consumer build
root or its selected configuration directory, have a basename exactly equal to
the CMake-declared filename, avoid symlinks, and identify an executable regular
file. Both manifests are accepted only as products of this invocation's
disposable configure/generate step, and the reader nevertheless completes all
record, identity, set, filename, and path validation before any consumer
starts. Valid consumers then run in declaration order, and a runtime failure
prevents later consumers from starting.

When the selected CMake generator exposes multiple configurations, the smoke
uses that same generator for producer and consumer, checks each
`CMAKE_GENERATOR` and `CMAKE_CONFIGURATION_TYPES` cache value, and resolves the
consumer executable from the configuration-specific `$<TARGET_FILE_NAME:...>`
and `$<TARGET_FILE:...>` manifest fields.

Migration residue, phase completion, stale-term, and source-layout checks are
temporary development checks. They must not be registered with CTest or CI.
Issue-specific replay, provenance, helper, and output artifacts must neither
enter the primary repository nor remain as long-lived personal-overlay
content. Long-lived runtime, public-header, and package-consumer tests own the
durable product boundaries.

## Build-Smoke CI Classification

A build smoke is a durable CTest whose primary boundary delegates to a CMake
configure/build/install, an exported-package or external-consumer build, or a
dedicated compile target. Every such test carries the exact stable CTest label
`build-smoke`. A companion that only calls the driver's Python cleanup or
layout helpers, or configures a compiler-free manifest-generation fixture,
remains an ordinary safety regression in the full CTest shard when it does not
delegate to a product build, install, external consumer, compile target, or
generated executable.

The maintained labelled inventory is
`DependencyDisabledInstallSmoke`,
`ImageArtifactCodecDependencyDisabledBuild`,
`IpcDisabledInstallSmoke`,
`OpenExrDeepProviderInstallConsumerSmoke`,
`OpenExrDeepProviderOptionOffSmoke`,
`OpenCvOperationProviderDisabledBuild`,
`PhotospiderdInstallLayoutSmoke`,
`PublicHeaderSelfContainment`, and
`StaticProductConsumerSmoke`. `PublicHeaderSelfContainment` belongs because its
CTest command builds the dedicated self-containment target; ordinary
GoogleTest binaries, daemon/CLI process tests, and
`PhotospiderdCapabilityHelp` do not create a child build and remain in the main
CTest shard. `OpenCvOperationProviderBuildSmokeSafety` also remains there: it
is the ordinary safety regression for the OpenCV build-smoke driver. Its one
`project(... NONE)` fixture exercises the production manifest generator with
an imported executable, but starts no compiler, product build, CTest, install,
compile target, or generated executable.
`InstallConsumerArchitecturePropagationSafety` likewise remains in the main
shard: it runs the three install-consumer drivers' real command-construction
paths against disposable producer cache fixtures while replacing subprocess
execution, so it verifies cache-to-child-argv propagation without launching a
product configure, build, or install. Its data-driven command recorder also
creates arbitrary 0/1/N dependency-disabled target declarations, target-file
manifests, CMake-authoritative target filenames, and fake executables.
In-process cases cover Linux/macOS extensionless names, Windows `.exe`, and
POSIX-Python Cygwin/MSYS2 `.exe` spelling. They require ordered execution and
pre-runtime failure for empty, duplicate, missing/unexpected, malformed,
control-bearing, separator-bearing, reserved, foreign,
filename/path-drifted, unsafe, noncanonical, unexpected-layout, unbuilt,
non-file, or non-executable inventory records; build and consumer failures lock
fail-fast ordering. A compiler-free `project(... NONE)` fixture exercises the
generated target validator and both target filename/path expressions. A
separate `cmake -P` fixture calls the production public-header writer directly;
neither fixture starts a compiler, product build, install, or generated
executable.
The same process injects executable lookup, validation, and captured-command
callbacks into the static-product driver's production archive-symbol helpers.
It locks Darwin xcrun-first fallback, non-Darwin independence, all-candidate
failure, and canonical path de-duplication without changing process PATH or
replacing the real installed archive scan. When CMake registers the safety
test, it also supplies the current build tree, CMake and CTest executables,
configuration, and Python launcher. The test queries that tree through
`ctest --show-only=json-v1` and the production inventory parser. It requires
`DependencyDisabledInstallSmoke` and `IpcDisabledInstallSmoke` exactly once in
every profile, requires `StaticProductConsumerSmoke` exactly once only when
IPC is enabled and absent otherwise, then requires every expected entry to
remain enabled and labelled and to start with the exact `python -B` driver
path. Commented or inactive CMake source cannot satisfy this
generated-inventory check because it produces no CTest entry. The inventory
query executes none of the real smokes and does not change the nine-test
build-smoke classification.

CTest keeps every labelled test registered for direct local use. CI's
`full-ctest` shard excludes the exact label. Configuration planning parses
`ctest --show-only=json-v1` only as an allow-empty preflight because default
`gtest_discover_tests` entries may still be unlabelled `_NOT_BUILT`
placeholders. After the complete default build, build integrity repeats the
query in strict mode and publishes one independent matrix job per labelled
test. Adding another maintained build smoke therefore requires its CTest
registration and the same label, but no workflow test-name edit. Preflight
fails closed on malformed inventory, duplicates, invalid label shape, or
disabled/commandless labelled entries, but not on an empty selection. The
post-build authority rejects those states and an empty labelled set. Before
execution, the runner re-queries the inventory and rejects a selected name that
is absent, duplicate, disabled, commandless, or no longer labelled. After that
exact label check, it selects only the validated numeric CTest index, so
arbitrary test-name characters are not interpreted by a shell or regular
expression.

The published-image workflow fans out the strict build-integrity output after
restoring the same reusable default producer. An empty include fallback keeps
`fromJSON` well-formed when that producer job is intentionally skipped; a
successful producer cannot publish an empty strict matrix. Each CTest
registration retains its own timeout and `RUN_SERIAL` behavior; each matrix
item also has an independent workflow timeout and result artifact. The
local-image fallback reads the same post-build NUL-delimited names and executes
them sequentially because it has only one Docker-capable runner. Nested drivers
must continue to use disjoint work directories, validate any reusable producer
identity they accept, and clean up without following or deleting unrelated
symlink targets.

## Validation Ownership

Primary-repository CTest and CI entries are reserved for long-lived software
behavior: correctness, performance, stability, multithreaded execution, error
handling, compile boundaries, package consumption, and runtime API boundaries.
`PhotospiderdCapabilityHelp`, `PhotospiderdInstallLayoutSmoke`,
`StaticProductConsumerSmoke`, `GraphCliOptionBadAlloc`, GoogleTest discovery,
and `PublicHeaderSelfContainment` satisfy that rule because they execute or
compile the maintained product. The daemon help test uses a CMake script driver to run
the real configuration-specific `photospiderd --help`, captures stdout and
stderr, requires a numeric zero process result before matching the stable
capability sentence, and diagnoses launch failure separately from nonzero exit.
The driver removes loader override variables and is reused after package
installation, so build-tree and install-tree resolution exercise their own
declared lookup paths.
`IpcDisabledInstallSmoke`, `DependencyDisabledInstallSmoke`, focused
`test_ipc_protocol`/`test_ipc_host` cases, and real-process `test_ipc_daemon`
cases follow the same rule: they exercise
package, framing, typed client, complete IPC Host dispatch/polling/stop/artifact
ownership, daemon lifecycle, concurrency, and cleanup behavior. Daemon tests
use CTest timeouts plus bounded
SIGTERM-to-SIGKILL-to-waitpid cleanup; they do not depend on fixed readiness
sleeps.
`StaticProductConsumerSmoke` is limited to producer configure/build/install,
external `find_package`, public-header compile/link/run, installed export and
dependency boundaries, platform archive/link behavior, and multi-configuration
target discovery. Its behavior verdict must not include Git identity, staged or
unstaged patch hashes, invocation replay, environment fingerprints, or
synthetic verifier self-tests. It uses a transient work directory and emits
commands plus assertion diagnostics directly to CTest's captured streams. A
phase name, migration-residue search, stale-term detector, source-layout
completion check, or issue replay is not a software behavior test and must not
be registered with CTest or invoked by CI.

The CLI/Host Doxygen AST tool is a long-lived manual developer tool, not a
test. Run it explicitly when the corresponding declarations, definitions,
exception contracts, or target source closures change:

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-cli-host-doxygen
```

The CLI/Host audit treats
`apps/graph_cli/src/cli_config.cpp::apply_cli_policy_execution_defaults` as the
canonical policy/execution-default definition and validates its complete
Doxygen in that translation unit. It also audits
`load_configured_policy_plugins`, `run_graph_cli`, the root CLI
resource-exhaustion policy, temporary-then-commit configuration parsing, and
the complete catalog of CLI/benchmark broad catches. Every broad catch must be
preceded on the same chain by an exact `std::bad_alloc` rethrow.

Its file may remain in the primary repository because this document defines
its lasting manual role. It must remain absent from CTest and GitHub CI. Its
`--out` directory is a disposable temporary working directory outside
the repository and must not become a retained result tree.
Issue-specific replay, provenance, helper, and output artifacts must neither
enter the primary repository nor be retained as long-lived personal-overlay
content. A clean primary clone, CMake configuration, CTest inventory, and CI
script must not depend on personal development content.

Validation is proportional. During implementation, run scoped static checks,
affected build targets, and focused regressions. A native clean configure, full
build, or complete CTest/JUnit pass is optional and should be chosen only when
the change's risk warrants it. Local workflow-source, YAML, and shell checks are
developer preflight only; they do not emulate the hosted GitHub Actions runner.
Do not use Docker or local `linux/amd64` emulation as a routine local preflight.
Current-head GitHub Actions remains the authoritative remote integration
environment.

## CLI Option-Action Validation

`test_cli_policy_execution_config` is the CTest-registered integration binary
for the reusable `run_graph_cli` option boundary plus policy/execution
configuration. Its configuration cases enforce transactional YAML/editor
parsing, the zero-through-eight execution-worker range, exact Host values, and
startup failure on Host rejection. Its option cases use a complete
deterministic Host spy and the real ordered parser. Successful load/output and
short-traversal cases preserve the
Host-returned session target and the argument-free `-t` grammar. Failure cases
require load, output, dependency-tree print, traversal-order, and all-cache
clear failures to return recoverable exit code 2 without printing the success
footer or entering the REPL. The load case also captures the REPL banner,
proving that a failed only action wins over the normal no-action fallback.
Each in-process invocation fully reinitializes platform `getopt_long` state
before both the configuration scan and ordered action replay. The cache-clear
case first completes a traversal invocation with a different option shape, so
its second invocation proves that hidden parser state cannot reorder or skip a
later action. Because that parser state is process-global, the reusable
boundary supports repeated serialized calls rather than concurrent calls;
embedders must serialize every complete `run_graph_cli` invocation.

Option replay remains ordered and may expose effects from successful actions
before or after another recoverable action failure; it does not provide a
multi-action rollback transaction. The final result is nevertheless failure
when any action or loaded-graph precondition fails, and that failure wins over
an explicit `--repl`. An invocation with no option action retains normal REPL
entry. Run the focused boundary with:

```bash
cmake --build build --target test_cli_policy_execution_config -j
./build/tests/test_cli_policy_execution_config \
  --gtest_filter='CliOptionActions.*'
```

## Graph Document Error Matrix Validation

`test_graph_document_errors` is a CTest-registered integration binary for the
long-lived Graph document ingestion and save contracts. It exercises the
public embedded Host boundary and the direct `GraphModel::replace_nodes`
transaction boundary. Load/reload cases distinguish omitted source paths from
explicit source paths, require the exact `GraphErrc` category for I/O, YAML,
schema, topology, lifecycle, and unexpected failures, and prove that
`std::bad_alloc` remains an exception. They also prove failed initial loads do
not publish sessions, failed reloads preserve the complete prior Graph state,
successful replacement advances topology generation and authoritative
`GraphRevision`, resets runtime state, and keeps retry possible.

`test_host_adapter` owns the deterministic reload-versus-close lifetime
regression. A real blocking compute and three explicit Host-operation gates
prove that a reload admitted before the close marker remains admitted before
Kernel entry and after public status translation, and that close cannot finish
first. Repeating reload after that marker must return `GraphErrc::NotFound`
without entering Kernel. The companion node-YAML and forward/backward ROI
races prove reload still runs after each required lookup-and-use work item, so
the close admission correction does not weaken graph-state ordering. These
tests use event gates and zero-duration future snapshots, not timing sleeps.

The same binary owns the public save transaction regression. Its private,
destination-scoped `BUILD_TESTING` checkpoint runs on the graph-state worker
immediately before destination open. One case requires recoverable failure to
return `GraphErrc::Io`; another requires exact `std::bad_alloc`. Both require
the existing destination bytes plus the publicly inspected session and node
state to remain unchanged, then require an uninstrumented save retry to
succeed. The const GraphIO boundary and serialized owner path provide the
broader non-mutation guarantee. Production builds compile out the checkpoint
and retain the single real writer.

## Revision-Safe Compute Publication Validation

Issue #72 uses four maintained test binaries to own the long-lived staged
publication boundary. `test_compute_run` validates the checked nonzero strong
`GraphInstanceId` and `GraphRevision` values, non-reused Graph identity,
monotonic mutation revisions, and exact descriptor/snapshot provenance.
`test_compute_service_split` proves `RealtimeProxyGraph` snapshot cloning is a
deep isolation boundary and that complete prepared-state publication uses the
documented no-throw swap path.

`test_kernel_contracts` exercises the product Kernel boundary. Deterministic
event gates hold operation execution outside graph-state while clear, same-label
reload, or same-topology cache clear advances the live revision. Parallel and
sequential stale results must return `GraphErrc::ComputeError`, preserve the
newer visible state, and write no deferred cache artifact. A focused
`PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING` checkpoint pauses after predicate
validation inside the graph-state item, proving mutation cannot enter between
validation and publication. The same checkpoint proves a valid RT proxy commit
remains visible when the independently validated HP sibling later becomes
stale. Together with `PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING` and
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING`, this macro is present only
in the three-translation-unit test-product variant used by
`test_kernel_contracts` and `test_host_adapter`. The installable product uses
the matching production objects even when `BUILD_TESTING=ON`.

The same binary proves the private compute-request lane serializes execution
observation/route replacement with same-Graph compute, accepted async work
survives a dropped caller future, and close drains compute-request work before
graph-state without tearing down process-owned routes. These races use explicit
gates and bounded waits, not timing sleeps. Every discovered
`test_kernel_contracts` case also has a
30-second CTest timeout.

`test_disk_cache_diagnostic_concurrency` is the separate long-lived
multithreaded fault-isolation binary. Production record/snapshot workers run
while `GraphModel` clear, clone, and staged publication repeat; another case
invokes the two-store exchange from both argument orders through an inline,
source-tree-only bridge; a deterministic allocation failure proves a throwing
snapshot copy releases the private scoped guard. Every discovered case carries
the `kernel-concurrency` label and a 20-second CTest timeout. If a lock
regression prevents worker recovery, CTest terminates that dedicated process;
neither a `std::future` destructor nor a thread join can retain the broad
kernel-contract process or wait for the CI job timeout. The sequential
`CacheSemantics.DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics`
case remains in `test_kernel_contracts` to prove failed reload preservation and
successful clear/reload reset without duplicating the deadlock probe.
Run the focused contract with:

```bash
cmake --build build \
  --target test_compute_run test_compute_service_split test_kernel_contracts \
  test_disk_cache_diagnostic_concurrency -j
./build/tests/test_compute_run \
  --gtest_filter='GraphRevision.*:ComputeRunDescriptor.CapturesIdRevisionIntentQualityAndQosWithoutReuse'
./build/tests/test_compute_service_split \
  --gtest_filter='RealtimeProxyGraph.*'
./build/tests/test_kernel_contracts \
  --gtest_filter='ComputeContracts.ParallelStaleComputeCannotOverwriteGraphClear:ComputeContracts.SequentialStaleComputeCannotOverwriteGraphClear:ComputeContracts.ReloadedDocumentRejectsOlderSameLabelCompute:ComputeContracts.SameTopologyCacheClearRejectsStaleMemoryAndDiskPublication:ComputeContracts.CommitPredicateAndPublicationExcludeMutationToctou:ComputeContracts.RealtimeCommitSurvivesStaleHighPrecisionSibling:ComputeContracts.ExecutionObservationAndReplacementWaitForCompute:ComputeContracts.CloseWaitsForAcceptedAsyncComputeRequest:ComputeContracts.DroppedAsyncFutureRemainsOwnedUntilCloseDrain:CacheSemantics.DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics'
ctest --test-dir build --output-on-failure \
  -R '^DiskCacheDiagnosticConcurrency\.'
```

## Cooperative Run Cancellation Validation

Issue #73 keeps cancellation coverage in maintained behavior tests rather than
an issue-specific replay tool. `test_compute_run` owns the private Run source,
stable first reason, injected monotonic deadline, terminal-before-quiescent
state, request fan-out, and cancellation/failure/commit arbitration. The same
binary exercises `ExecutionService` cancellation before active publication,
exact queued-Run purge, the dequeue/pre-callback race, non-preemptible callback
drainage, suppressed dependent re-entry, peer isolation, and exact grant/root
release. Its legacy `A -> B` case proves cancellation after A returns retires
the callback-owned and still-plan-owned units exactly once without entering B
or publishing staged output; its companion exception branch proves a later
provider failure cannot replace the already accepted cancellation.

`test_kernel_contracts` owns the product boundary. Deterministic commit hooks
prove cancellation before claim publishes no Graph/proxy/cache state and a
request after claim cannot undo successful publication. The RT/HP case keeps a
committed proxy visible when the HP sibling later becomes stale, while the
sequential case proves provider return observes cancellation before staged
publication, and the close case proves a logically cancelled request still
drains its running provider and public `ComputeError` translation before Graph
destruction. `test_compute_service_split` cancels from connected preflight on
the private `serial_debug` route and proves that dirty HP and paired HP/RT requests enter
neither the parameter dependent nor phase-two target work.

Public non-expansion remains part of existing durable contracts:
`test_ipc_protocol` locks the exact 60-method protocol-v2 inventory, rejects
`compute.cancel`, round-trips every version-two status label, and requires
`cancellable: false`; `test_compute_request_registry` locks the daemon job
snapshot; `test_policy_registry` locks transactional ABI-v1 load rejection,
binding-held DSO lifetime, and first-fault stability; and
`StaticProductConsumerSmoke` compiles and runs the installed 58-virtual Host,
60-call Client, operation ABI v2, and pure-C policy ABI v1 consumers. These
tests must not gain a compatibility cancellation shim for this private change.

Run the focused cancellation boundary with:

```bash
cmake --build build \
  --target test_compute_run test_compute_service_split \
  test_kernel_contracts test_ipc_protocol test_compute_request_registry \
  test_policy_registry -j
./build/tests/test_compute_run \
  --gtest_filter='ComputeRunCancellation.*:ComputeRunCommitArbiter.LinearizesCancellationBeforeOrAfterCommitClaim:ExecutionServiceCancellation.*'
./build/tests/test_compute_service_split \
  --gtest_filter='ComputeServiceCancellation.ConnectedPreflightCancellationSuppressesDirtyAndSiblingPublication'
./build/tests/test_kernel_contracts \
  --gtest_filter='ComputeContracts.SequentialCancellationAfterProviderReturnSuppressesPublication:ComputeContracts.CancellationBeforeCommitClaimSuppressesPublication:ComputeContracts.CancellationAfterCommitClaimPreservesPublication:ComputeContracts.RealtimeCommitSurvivesStaleHighPrecisionSibling:ComputeContracts.CancelledComputeStillDrainsBeforeGraphClose'
./build/tests/test_ipc_protocol \
  --gtest_filter='ProtocolContract.AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods:EnumCodec.RoundTripsEveryDefinedVersionTwoLabel:HostRoutedGraphStateProtocolTest.ComputeLifecyclePreservesEveryTypedHostRequestFieldAndStableShapes'
./build/tests/test_compute_request_registry \
  --gtest_filter='ComputeRequestRegistrySubmission.PublishesQueuedCommitSnapshot'
./build/tests/test_policy_registry
```

## Latest-Wins Supersession Validation

Issue #74 keeps latest-wins and realtime-group coverage in maintained behavior
tests. `test_compute_supersession` owns canonical absent/explicit HP key
equality, checked nonzero generation overflow, exact 64-total compute-lane
admission, persistent ticket FIFO/wake behavior, concurrent same-key ticket
adoption, cross-target/intent/Graph isolation, close retirement, deterministic
18,000- and 36,000-publication storms, and `RunGroup` cancellation/aggregate
rules. The group cases distinguish a request-level accepted reason from a
reason that actually wins an open child arbiter: late Superseded or
ExplicitRequest after two child successes cannot replace aggregate success,
while a winning cancellation retains the first reason below failure priority.
CMake discovers all 16 cases through CTest with a 60-second per-case timeout.
The stress cases assert one ticket, one logical active owner, at most one
pending owner, exact displaced settlement, and only the final current
generation remaining commit-eligible; they do not create a background runner
or rely on timing sleeps. The current-observer case proves an accepted newer
generation advances external freshness before physical execution while a
prepared older generation that is born stale emits no observer notification.

`test_kernel_contracts` owns the product boundary. It proves missing intent and
explicit HP share one key, failed newest work does not resurrect an older
prepared commit, already committed older output remains visible, and realtime
supersession on both sides of RT publication denies the old HP sibling while
preserving a valid old proxy. A post-commit checkpoint additionally blocks the
old realtime caller after both child successes and visible publication but
before group aggregation; newer generation publication records Superseded
without changing that old caller's success. `test_compute_run` covers immutable
supersession identity and child-local versus group-wide cancellation. Existing
`test_compute_service_split`, `test_host_adapter`, and
`test_bad_alloc_boundaries` remain focused regression companions for service,
Host lifecycle, and allocation-failure boundaries.

Run the focused supersession boundary with:

```bash
cmake --build build \
  --target test_compute_supersession test_kernel_contracts test_compute_run \
  test_compute_service_split test_host_adapter test_bad_alloc_boundaries -j
./build/tests/test_compute_supersession
./build/tests/test_kernel_contracts
./build/tests/test_compute_run
./build/tests/test_compute_service_split
./build/tests/test_host_adapter
./build/tests/test_bad_alloc_boundaries
ctest --test-dir build --output-on-failure \
  -R '^(SupersessionIdentity|GraphStateExecutorContinuation|ComputeRequestCoordinator|ComputeRequestCoordinatorStorm|RunGroup)\.'
```

## Policy Generation and Private Execution Validation

Issue #75 keeps policy-generation and private-route coverage in maintained
behavior tests. `test_policy_registry` owns the exact built-ins and class
support, transactional rejection of a missing API or mismatched ABI, active
binding/DSO lifetime across registry unload, and first-fault stability for one
binding generation. `test_resource_admission` owns the exact closed
`cpu`/`gpu_pipeline`/`serial_debug` route vocabulary, worker-limit rollback,
one fixed pool per Host composition, and validation-first session route
replacement. The `ExecutionServicePolicy.*` cases in `test_compute_run`
continue to own Host-authored cost, class/frontier/fairness, aging, headroom,
three-to-one progress, dependent re-entry, saturation, and exact grant release
through reserved start.

`test_physical_execution_routes` owns allocation-free route/lane state:
CPU/Metal overlap, Metal single-flight, serial worker-zero single-flight,
shutdown rejection, and committed-work drainage. `test_policy_execution`
uses an injected deterministic fake Metal executor to prove the canonical
registry-derived per-route device inventory, rejection before Run publication,
distinct fixed CPU/GPU workers, exact executor entry, Metal exception
publication/recovery, route reuse, cancellation, and reserved-start rollback
without candidate/version ABA or leaked grants. It also proves
that a grant-blocked high-priority Run A cannot starve lower-priority independent
Run B, that A's ready entry later executes exactly once, and that a sole blocked
candidate has bounded policy-selection retries and wakes on cancellation.

The reserved-start rollback probe is fixed-size atomic state compiled only into
the non-installed test product. The Issue #75 probe macro changes no production
class definition or layout, and the production object has no reserved-start-
probe observer typedef, object field, callback, worker hot-path runtime branch,
helper global, or symbol. This statement is limited to that probe; the
pre-existing initial-submission storage observer is baseline behavior and is
not removed or committed for migration in this phase. `Issue75DeviceRouting.*` in
`test_compute_run`
proves that full HP, dirty HP/RT, and connected preflight freeze the chosen
Metal implementation/device and use CPU fallback when Metal is absent.
`test_device_executor_registry` owns fixed-slot validation, exact dispatch,
borrowed TLS context restoration, provider-exception identity, and copied
diagnostics without a platform SDK. Its multi-call case proves submission and
serialized-entry counters advance monotonically across successful and throwing
callbacks. Portable callback tests prove that direct same-executor recursion is
rejected with the stable `std::logic_error` before the nested provider or
either diagnostic counter advances, that the outer context remains current,
that a later invocation recovers, and that a distinct executor may nest while
restoring the outer context. On Apple with the repository operation plugin
enabled, `test_metal_device_executor` directly drives the factory-created real
registry from two controlled threads. While the first callback remains active,
a copied diagnostic must expose two submissions but only one serialized entry;
it releases the first callback only after observing that stable queued state. A
bypassed admission wait instead exposes two entries and fails deterministically
without a sleep, overlap window, or scheduler-timing assumption. The test also
allocates a real texture and shared buffer before throwing, then proves exact
provider-exception identity, same-thread TLS restoration, zero live
allocations, stable queue/pipeline diagnostics, monotonic counters, and
successful later non-nested entry through the same executor. A separate
threadsafe death-test child installs a five-second alarm, attempts synchronous
same-executor callback recursion through the real registry, and exits only
after proving the exact error text, unchanged nested counters/resource
diagnostics, preserved outer TLS context, cleared post-return TLS, and a
successful later invocation. The alarm turns the former self-deadlock into a
bounded test failure without detached threads or lifetime races. After proving
that watchdog path, the test performs one real CPU-to-Metal upload and proves
an exact revision-preserving device replica enters residency. V-9 additionally
proves upload scratch returns only after completion, persistent memory remains
through callback return and residency, capacity-one eviction returns the old
lease, and final manager destruction returns the last lease. A tiny Perlin
device budget rejects the complete native heap-query plan before its first
texture/buffer allocation. The sufficient-budget path runs the real repository
Perlin operation twice through one `ExecutionService` and proves queue
availability, two operation submissions and executor entries, eight retired
invocation allocations, one reused pipeline, asynchronous pending-Value
readback to CPU-owned outputs, the dedicated Metal worker id, and zero settled
Host and device reservations. Native `allocatedSize` is audited before command
commit in both upload and download.

V-8 and V-9 portable cases in `test_device_residency` lock direct
host-read versus transfer planning, exact current completion publication, late
stale rejection before destination Ready, pretracked current publication
rejecting a late older Run admission, failed/discarded nonpublication,
proper-subset identity rejection without consuming a rightful admission,
concurrent exact callbacks, and duplicate-completion rejection. Real
memory-only leases attached to fake native owners further prove creator/Run-
equivalent release does not return bytes early, residency eviction releases
only its own strong owner, an external Value copy extends lifetime, and stale,
rejected, cancelled, or reused identities neither double-release nor consume
another allocation's authority.
`test_compute_run` adds deterministic cases for an early fence callback parked
until original grant retirement, executor lifetime extending Run settlement,
pending Value dependency deferral, cancellation that retires a continuation
without waiting for its producer, and typed stale failure that never releases
dependent work. These cases use gates and futures and contain no timing sleep.

`test_cli_policy_execution_config` locks transactional policy/execution config
parsing and exact Host application. `test_host_adapter` loads real operation
ABI-v2 and pure-C policy ABI-v1 fixtures, configures both extensions, validates
their snapshots, and computes through the private CPU route.
`GraphCliPluginComputeSmoke` repeats that vertical slice through the real REPL.
`test_ipc_protocol` and `test_ipc_daemon` own protocol-v2 routing, process-owned
policy state, generation-changing replacement, scan, and shared execution
defaults. `StaticProductConsumerSmoke` independently builds the installed C11
policy DSO and C++ operation DSO before executing the same external-consumer
path.

The installed Host, CLI, and IPC protocol-v2 surfaces still expose no
cancellation command. IPC continues to reject `compute.cancel` and publish
`cancellable: false`; supersession remains a private embedded-kernel behavior,
not a new public control surface. The worker-owning scheduler ABI has no
compatibility consumer.

Run the focused policy/execution boundary with:

```bash
cmake --build build \
  --target test_policy_registry test_policy_execution \
  test_physical_execution_routes test_device_executor_registry \
  test_device_residency test_compute_run test_resource_ledger \
  test_resource_admission \
  test_cli_policy_execution_config test_host_adapter test_ipc_protocol \
  test_ipc_daemon graph_cli -j
./build/tests/test_policy_registry
./build/tests/test_policy_execution
./build/tests/test_physical_execution_routes
./build/tests/test_device_executor_registry
./build/tests/test_device_residency
./build/tests/test_resource_ledger
./build/tests/test_compute_run --gtest_filter='Issue75DeviceRouting.*'
./build/tests/test_resource_admission
./build/tests/test_cli_policy_execution_config \
  --gtest_filter='CliPolicyExecutionConfigParsing.*:CliPolicyExecutionConfigApply.*'
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.PolicyScanAndOperationPluginUseStatusValues:EmbeddedHostAdapter.ExternalOperationAndPolicyPluginsDriveParallelCompute'
./build/tests/test_ipc_protocol \
  --gtest_filter='ProtocolContract.AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods:HostRoutedGraphStateProtocolTest.PolicyAndExecution*:ClientExecutionDefaults.*'
./build/tests/test_ipc_daemon \
  --gtest_filter='IpcDaemonExecution.*:IpcDaemonPolicy.*'
ctest --test-dir build --output-on-failure \
  -R '^(GraphCliPluginComputeSmoke|StaticProductConsumerSmoke)$'
# Apple with PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=ON:
./build/tests/test_metal_device_executor
```

Focused companion regressions own the remaining boundaries:

- `test_kernel_contracts` drives the real `GraphIOService` stream through
  post-write, post-flush, and post-close failure states. Each phase must return
  `GraphErrc::Io`, and the created destination demonstrates the documented
  non-atomic post-open behavior.
- `test_resource_ledger` proves checked Host and device-vector arithmetic,
  independent saturation and exact recovery for all five Host dimensions,
  CPU/duplicate device configuration rejection, zero and exact-boundary
  device plans, atomic memory-plus-scratch rejection, per-device isolation,
  same-device contention, plan-to-actual shrink, typed underplanning failure,
  split memory/scratch lifetimes, move-only authority, delayed asynchronous
  release, bounded Host child grants, deferred Host parent release, and
  concurrent no-overcommit behavior.
- `test_resource_admission` proves the exact private-route vocabulary,
  worker-limit rollback, one fixed pool per Host with independent Host
  compositions, and validation-first session route replacement that preserves
  the previous copied route after an invalid candidate.
- `test_compute_run` records complete action/node/worker/epoch tuples. It proves
  two concurrent Runs that reuse local task id zero deliver only matching
  Run/node epochs to their separate Hosts; cleanup releases a blocked first Run
  on every assertion path so a serialization regression terminates as a test
  failure. Realtime Full HP and Interactive RT children share one physical Host
  and local task id zero, but distinct trace-node markers map each Host event
  to the matching epoch and callback-retained descriptor/task identity.
  This realtime case intentionally exercises `ExecutionService` directly:
  worker-loop Host/epoch selection and retained callback identity are observable
  at that boundary without adding a test-only GraphRuntime hook. Direct service
  cases also cover whole-vector rejection and recovery for retained Host
  memory, scratch, ready entries, and ready bytes; checked-overflow rejection;
  shared CPU admission across concurrent Runs; initial ready-store backpressure
  and priority ordering; dependent re-entry backpressure; and exact root
  release after success or failure.
- `test_ipc_protocol` proves exact Graph status propagation, one-call mutation
  behavior, and daemon session-name rollback after failed load.
- `test_ipc_daemon` proves the real transport returns save `NotFound` and `Io`
  exactly, leaves the remotely owned graph inspectable after destination
  failure, and accepts a subsequent successful save.

Run the focused validation with:

```bash
cmake --build build --target test_graph_document_errors test_host_adapter \
  test_kernel_contracts test_resource_ledger test_resource_admission \
  test_compute_run test_ipc_protocol test_ipc_daemon -j
./build/tests/test_graph_document_errors
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.*Reload*'
./build/tests/test_kernel_contracts \
  --gtest_filter='GraphIoContract.Save*'
./build/tests/test_resource_ledger
./build/tests/test_resource_admission \
  --gtest_filter='EmbeddedHostExecutionConfiguration.*'
./build/tests/test_compute_run \
  --gtest_filter='ExecutionService.*'
./build/tests/test_ipc_protocol \
  --gtest_filter=ProtocolGraphLoad.FailedHostLoadReleasesNameForRetry
./build/tests/test_ipc_daemon \
  --gtest_filter=IpcDaemonGraphLifecycle.PersistsAcrossClientsAndInspectsCopiedSnapshots
```

These are maintained product-behavior tests. No migration-residue scan,
issue-specific replay script, or retained result artifact belongs to this
validation surface.

The maintained scripted CLI integration check in
`ci/scripts/graph_cli_script_test.sh` owns the corresponding REPL boundary.
Its explicit-missing-source case requires a load failure, an empty `graphs`
inventory, and no current Graph. Its invalid-target case first loads the
maintained propagation fixture before requiring target rejection, so it does
not depend on a failed load publishing state. Each case uses isolated temporary
session and history storage that is removed when the script exits.

## Direct CPU Operation Authority Validation

Issue #82 keeps scalar callback/metadata identity and direct dirty admission in
maintained behavior tests. `test_op_registry_m31` registers monolithic HP and
tiled HP siblings in both orders, invokes both callbacks, and requires each
selected implementation to retain its own identity and complete scheduling
metadata. A later sibling registration therefore cannot silently rewrite the
metadata used with an earlier callback.

The task-planning and runner cases register a SpatialAligned monolithic sibling
before a device-tiled RandomAccess sibling. They require dependency ROI
lowering, tile size, selected callback, and provider input views to consume the
same revisioned route rather than a generic key-level metadata lookup. The
manual `test_propagation` tool likewise filters and retains the exact tiled
implementation for the requested HP or RT diagnostic route.

`test_cpu_dense_tensor_image_operation` also freezes the exact core CPU route
for TensorSlice target-only and target-plus-upstream plans, then appends a
preferred same-key non-core GPU implementation before task population. Both
cases require `NoOperation` at dirty preparation, zero provider entries, zero
lifecycle/gate/grant/reservation/ledger residue, and restoration of the core
registry route. A guard-bypass control continues through the real
`HighPrecisionDirtyNodeExecutor` direct provider lease and reaches the fake GPU
provider, so the regression cannot pass merely because the test stops at
planning.

Three adjacent route-context cases mutate the task-population device inventory
after TensorSlice planning. Only the externally satisfied case prepares as
zero-work without comparing the now-irrelevant frozen intent, device inventory,
or node routes. The exact-cache case installs complete old HP output through
the real Graph boundary but remains active because the TensorSlice is
dirty-selected; like the partial-active control, it must return `NoOperation`
before fake GPU provider entry or execution authority. No test erases an
execution-order node.

`test_compute_service_split` separately proves the outer service boundary. A
real complete target cache remains exact through selection, yet the explicitly
dirty target and its provider cone execute. Two adjacent cases begin with the
same exact planning observation, then remove the output or reduce its formal
Region through an internal test-product observer; all three states retain and
execute the same dirty provider cone. A post-plan registry replacement with
complete old cache must still fail as active route drift before either provider
enters. A reverse mutant that lets exact cache satisfy dirty candidates makes
the exact control and Host ROI fixture fail with empty work and unchanged old
pixels.

Adjacent real-provider cases use a sparse dirty chain
`A(dirty) -> B(externally satisfied, inactive) -> C(dirty)`. The first requires
only C to execute; a shared `A -> D(dirty)` control requires A, C, and D while B
remains inactive. A reverse candidate-only-universe mutant executes A in the
first case and fails, proving demand traversal must retain inactive connectors
and satisfied boundaries. Other planning cases continue to apply ordinary
full-request cache cuts, honor force-recache, and keep RT work executable.

`test_host_adapter` first publishes exact complete HP output, then submits a
non-forced dirty ROI through the public Host boundary. It must execute 16
downstream tiles plus one monolithic source task, update the selected pixel from
3 to 11, preserve an unselected pixel at 3, and expose the local backward
mapping and native PixelRect/tile geometry through Host snapshots. With the
incorrect dirty-cache satisfaction restored, this fixture reports zero active
tasks and leaves the selected pixel unchanged.

`test_compute_run` registers heap-backed exclusive keys for full-plan, dirty HP,
dirty RT, and connected-preflight product paths. The shared string-payload
estimator proves actual capacity plus one terminator and strong overflow
rollback. The internal test product also reports each actual retained owner,
that owner's copied `std::string::capacity()`, and the checked estimator total
immediately before and after its charge. Full-plan, dirty HP, dirty RT, and
connected-preflight cases require every reported delta to equal that actual
capacity plus one terminator and require the exact expected owner counts. This
comparison is independent of the complete admitted vector: the same cases
separately require an identical plan at exact retained capacity and reject one
byte less before provider entry with a zero ledger snapshot. They cover the
allocation transfer from a charged plan/context constraint into its unique
submission without using a migration-residue source scan.

The direct-lease gate regression acquires one heap-backed key, mutates the
caller's still-live allocation in place after acquisition returns, and queries
the real gate predicate through an authority-free test-product diagnostic. The
original key must remain blocked and a different key startable until the lease
retires, proving wait/start/finish borrow the lease-state copy rather than the
caller. The test restores the caller buffer before cleanup so an incorrect
implementation can still unwind deterministically.

`test_compute_service_split` proves that nonparallel dirty HP, dirty RT, and
connected-parameter preflight enter the same process-owned operation gate and
resource ledger used by physical workers. Cross-Graph cases cover
nonreentrancy, exact implementation caps, same/different exclusive keys,
retained-memory and scratch rejection before provider entry, cancellation and
exception cleanup, and successful retry after settlement. Deterministic
post-plan cases replace an HP implementation or unload an RT plugin before
active-operation revalidation. At that observation point the standalone Run or
realtime RunGroup logical lifecycle is intentionally visible. The cases require
typed failure before provider entry and before operation/resource/physical
admission, then require that the logical lifecycle settles with no callback,
grant, root-reservation, gate, or ledger residue and that a retry recovers. An
externally satisfied sibling is intentionally ignored so inactive registry
change cannot invalidate an otherwise valid active dirty target.

A separate cross-Graph case gives two reentrant HP implementations and two
reentrant RT implementations distinct identities, no identity cap, and one
equal heap-backed key. The first provider blocks after the dirty helper has
returned its direct lease and retired its helper-local constraints. The second
provider must remain outside until lease release for both HP and RT, proving
the real helper paths retain the key in direct-lease state rather than on the
helper stack.

The same binary owns two orthogonal sequential provider-boundary regressions.
Both Graphs select one registered callback identity; a node role parameter
distinguishes sequential and peer behavior. The metadata declares
`maximum_parallelism=1`, one nonempty exclusive key, and nonzero
retained/scratch demand. In the physical route case, a test-product-only
observer reports the exact operation-gate denial. The test waits for either
that admission rendezvous or an erroneous provider entry, then requires the
rendezvous and excludes provider overlap. After provider return, an injected
`FakeImageArtifactCodec` blocks disk-cache persistence and then throws
`GraphErrc::Io`; the route-backed provider must enter, exit, and settle while
that Host post-processing remains blocked, leaving no sequential grant in the
resource snapshot.

The resource-capacity case uses the same callback identity, cap, and key but a
second direct contender. A test-product-only authority-free diagnostic reuses
the production direct-lease envelope calculation, and the isolated
`ExecutionService` CPU, retained-memory, and scratch ceilings are set to
exactly one direct callback vector. Its heap-backed key is also compared with an
independent fixed-envelope plus copied-capacity-plus-terminator calculation.
Exact capacity admits; a one-byte-short limit and a declaration that leaves
room for capacity but not its terminator reject before gate/resource ownership
and leave a zero snapshot. The contender then reaches denied admission while
the provider is active and enters/exits before the codec is released. Keeping
this capacity check orthogonal is intentional: a physical Run reserves its
complete root before operation-gate startability, so its root is not a single
direct-lease vector. Neither regression adds a production or installable test
hook.

The post-plan, admission-wait, and retained-string observers, the gate
predicate diagnostic, and the direct-resource diagnostics exist only in the
non-installed internal test product. `StaticProductConsumerSmoke` requires the
nine production anchors spanning all eight seam objects and rejects every
matching state, setter, clearer, notification, helper, and diagnostic symbol
from the installed archive.

Run the focused boundary with:

```bash
cmake --build build --target test_op_registry_m31 test_compute_run \
  test_compute_service_split -j
./build/tests/test_op_registry_m31 \
  --gtest_filter='OpRegistryM31Test.ScalarSlotsStayAtomic*'
./build/tests/test_compute_run \
  --gtest_filter='OperationExecutionGate.DirectLeaseGateIgnoresCallerConstraintMutationAfterAcquisition:RetainedMemoryEstimator.StringPayloadChargesActualCapacityAndTerminatorAtomically:ExecutionServiceProductResources.FullPlanRejectsOneByteShortAndExecutesAtExactLimit:ExecutionServiceProductResources.DirtyHpAndRtUseExactSmallLargeSynchronizationInterval:ExecutionServiceProductResources.ConnectedPreflightUsesOneSharedUmbrellaAtExactThreshold'
./build/tests/test_compute_service_split \
  --gtest_filter='ComputeServiceSequentialAdmission.*:ComputeServiceDirectDirtyAdmission.*:ComputeServiceDirtyIdentity.*:ComputeServiceCancellation.NonparallelConnectedCancellationReleasesDirectAuthorityAndRecovers:ComputeServiceSplit.PreflightFailurePublishesNoHpCacheState'
```

## Graph Close and Process Shutdown Validation

Issue #76 keeps lifecycle correctness in maintained behavior tests rather than
migration scans. `test_run_lifecycle_registry` owns Graph registration,
candidate rollback/install races, atomic standalone/realtime-bundle admission,
Graph-close isolation, process shutdown, and exact final unregistration.
`test_execution_lifecycle_telemetry` owns schema-v1 fixed records, the
65,536-entry ring, 1..4,096 page bounds, atomic cuts, cursor gap/drop/saturation
semantics, all 15 counters, all six physical counter selectors, and the final
`ServiceStopped` zero-counter event.

The existing product-boundary targets carry integration ownership:

- `test_compute_run`, `test_compute_service_split`, and
  `test_kernel_contracts` cover full, dirty, preflight, no-op, realtime child,
  admission-race, visible-commit, exact finalization, and unrelated-Graph
  behavior. `test_kernel_contracts` also fixes the exact close owner/joiner
  generation, throwing-observer claim consumption, and atomic final
  name-removal/success-publication boundaries. Its same-name reload regression
  pauses a real calling-thread diagnostic store after graph-state completion,
  runs no compensating clear, proves the replacement slot is unchanged, and
  releases the final old-runtime owner. A separate regression isolates delayed
  old-runtime clear. A real worker operation invokes `Kernel::shutdown()` and
  proves the exact recoverable preflight leaves telemetry `Accepting`,
  generation zero, and Graph publication open. A watchdog death regression
  proves an injected failure after publication-gate closure terminates.
- `test_kernel_lifecycle_concurrency` links the real production archive, with
  the Kernel lifecycle observer macro rejected at compile time, and repeatedly
  races same-name publication, listing, direct close, and shutdown admission.
  Static archive inspection requires both close and shutdown product anchors
  while rejecting every observer hook symbol.
- `test_resource_ledger` and `test_policy_execution` cover exact root/child
  release, ready/callback/policy/binding counters, route drainage, same-service
  worker/policy-callback shutdown rejection, cross-service shutdown, repeated
  shutdown, and final counter/event order.
- `test_host_adapter` covers coalesced direct Host close, post-marker
  `NotFound`, close isolation, lane retirement order, and one composition-root
  shutdown.
- `test_compute_request_registry`, `test_ipc_protocol`, `test_ipc_host`, and
  `test_ipc_daemon` cover preallocated daemon close generations,
  pre-invocation-only `HostCloseNotStarted`, exactly one Host call, lost
  response without replay/reopen, late `NotFound`, Client/IPC Host local-only
  destruction, accepted-job drainage, signal shutdown, and Host lifetime.

Run the focused lifecycle boundary with:

```bash
cmake --build build --target test_run_lifecycle_registry \
  test_execution_lifecycle_telemetry test_compute_run \
  test_compute_service_split test_kernel_contracts \
  test_kernel_lifecycle_concurrency test_resource_ledger \
  test_policy_execution test_host_adapter test_compute_request_registry \
  test_ipc_protocol test_ipc_host test_ipc_daemon -j
./build/tests/test_run_lifecycle_registry
./build/tests/test_execution_lifecycle_telemetry
./build/tests/test_compute_run
./build/tests/test_compute_service_split
./build/tests/test_kernel_contracts
./build/tests/test_kernel_lifecycle_concurrency
./build/tests/test_resource_ledger
./build/tests/test_policy_execution
./build/tests/test_host_adapter
./build/tests/test_compute_request_registry
./build/tests/test_ipc_protocol
./build/tests/test_ipc_host
./build/tests/test_ipc_daemon
```

The final delivery pass uses one clean native configure, one full build, one
ordinary CTest/JUnit run excluding the exact `build-smoke` label, then strictly
discovers and independently runs every post-build build-smoke entry. It does
not register lifecycle provenance, stale-term searches, or source-quality
audits as product tests.

## Injected Image Artifact Codec Validation

`test_kernel_contracts` owns the long-lived fake-codec cache boundary. Its
`CacheSemantics.InjectedCodec*` cases create `GraphCacheService` or a real
`Kernel` with a shared `FakeImageArtifactCodec` and verify exact decode/encode
paths, service-retained codec lifetime, `int16` precision selection, recoverable
`GraphErrc::Io` diagnostics without HP-cache mutation, and exact
`std::bad_alloc` propagation. The Kernel-lifetime case blocks the real
`GraphStateExecutor`, admits a second cache-save work item that borrows
`Kernel::cache_service_`, releases the caller's only codec owner, and destroys
Kernel on another thread. Executor checkpoints and futures require destruction
to wait, the admitted encode to observe a live codec, and codec release to occur
only after Kernel destruction completes. The fake performs no real image-format
IO, so these tests remain independent of OpenCV codec behavior while exercising
the production runtime and cache service.

`ImageArtifactCodecDependencyDisabledBuild` configures a fresh nested build with
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` and `PHOTOSPIDER_BUILD_IPC=OFF`,
builds the provider-independent focused `test_kernel_contracts` target, and
runs only the injected-codec cases. The target remains available without
registering the complete kernel-contract binary in that profile's CTest
inventory. This proves the Graph/cache injection contract and fake do not
depend on the optional operation provider. The separate
`DependencyDisabledInstallSmoke` covers the complete product profile that omits
OpenCV discovery and selects the unavailable production codec.

Run the focused validation with:

```bash
cmake --build build --target test_kernel_contracts -j 2
./build/tests/test_kernel_contracts \
  --gtest_filter='CacheSemantics.InjectedCodec*'
ctest --test-dir build --output-on-failure \
  -R '^ImageArtifactCodecDependencyDisabledBuild$' -j 2
```

## CPU DenseTensor, Packed FP4, Provider Extensions, Region, ReadyFence, and Transfer Validation

`test_cpu_dense_tensor_image_operation` is a provider-independent integration
binary for the implemented V-2 through V-12 boundary. Its 48 durable cases
verify:

- copyable ReadyFence polling, queued non-inline waits, observer-local waiter
  cancellation, exactly-once Ready/Failed/ProducerCancelled settlement, typed
  failure retention, dropped-completer cancellation, and sole-executor
  retention for pending and already-terminal waits through callback completion;
- deterministic C++17 mutex/condition-variable races between wait registration
  and terminal publication, cancellation and callback entry, and transfer-owner
  destruction and callback entry, with unique terminal settlement and
  at-most-once callback delivery without sleeps or timers;
- pending Value metadata/identity observation, typed rejection of BufferHandle
  and checked-view payload access, and private producer revocation before
  readable Ready publication;
- explicit fake-executor transfer enqueue, distinct allocation binding with a
  preserved logical revision, sole-executor retention through destination completion,
  byte-identical completion, chained readiness without worker blocking,
  unreadable source failure/cancellation propagation, and destination-only
  cancellation when transfer ownership drops;
- explicit injected CPU-to-Metal transfer, checked device-local binding,
  revision preservation, rejection of a claimed host-visible target without a
  host pointer, no implicit host read or readback, typed provider failure, and
  successful later transfer through the same executor;
- malformed facet, stride, byte-offset, and exact-envelope rejection, including
  checked single-axis/cross-axis writable collision and overflow cases plus
  accepted padded, transposed, and singleton-axis layouts;
- exclusive builder write authority, seal revocation, retaining read-lease
  lifetime, BufferHandle subranges, process-local identities, and the
  non-liveness meaning of a nonzero `AllocationIdentity`;
- bounded positive, zero, and negative immutable strides over shared
  allocations, with distinct Value revisions;
- immutable Value copy sharing, copy-like DenseTensorView/ImageView moves, and
  allocation-isolated lvalue/rvalue descriptor, layout, and payload inputs;
- formal HP cache alias preservation, dirty reseal, replacement identity, disk
  reload identity renewal, unchanged cache paths, disk-save Value authority,
  and rejection/purging of exact-partial HP state at whole-read and regionless
  disk boundaries;
- exact descriptor-only invert inference, direct sealed-input reuse, and exact
  result-revision publication;
- the V-12 floating matrix across 1/3/4/8/16-channel FP32/FP64 images and
  rank-one through rank-five FP32/FP64 latents, including a real rank-one
  padded stride with an independent active-byte/padding-sentinel oracle,
  ImageRect/TensorSlice merge, exact CPU/external/I-O boundary preservation,
  and negative/zero-stride external rejection before Pending publication,
  owner retention, or provider callback; and
- padded multi-channel full and ImageRect execution, rank-four TensorSlice,
  Empty/Whole selection, dirty-plan-to-product staging, recomputation of
  missing or partial intermediate parents, selected-byte merge into an
  existing complete output, and promotion to reusable authority only after a
  Whole commit, callback-free target/upstream Region-route transfer and
  pre-task-population mutation rejection, externally satisfied no-work
  acceptance under device-inventory drift, exact-cache dirty and partial-active
  drift rejection, plus `GraphErrc::ComputeError` when execute returns a valid
  Value whose descriptor disagrees with inference.

`test_region_contracts` owns 31 durable Region cases for canonical
Empty/Whole, keys, intervals, normalization, rank-general TensorSlice,
overflow-safe clipping/algebra, representable one-axis and Tensor-axis unions,
nonrepresentable multi-axis union rejection, explicit budgets, typed failures,
checked ImageRect/PixelRect conversion, Region propagation, route-selected
same-key device replacement rejection, HP/RT intent-sensitive implementation
selection, Tensor planning/task selection/edge mapping, and Region dirty
lifecycle.

`test_packed_fp4_dense_tensor` owns four dependency-neutral V-13 integration
cases. They verify both nibble orders and a nonzero bit offset, exact encoded
and scale-dequantized E2M1 access, strict descriptor/quantization/layout/
envelope rejection, block-aligned TensorSlice scale/code projection with fresh
identities, byte-view and ImageBuffer fail-closed behavior, representation-
preserving CPU and injected fake-device transfer, exact formal memory-cache
retention, and typed image disk-cache rejection before executor, filesystem,
or codec effects. The malformed matrix includes wrong quantization rank/count,
zero or non-divisible blocks, nonfinite/nonpositive scales, bad layout version/
alignment/overlap/size, quantized Strided publication, and oversized blocked
transfer aliases.

`test_variable_sample_field_extensions` owns seventeen standard-library-only V-14
integration cases. A synthetic pure-C definition suite publishes versioned
VariableSampleField Schema, Facet, and Layout records with three physical
buffers. The cases verify typed namespaces, candidate conflicts and malformed
record rollback; generic cross-reference rejection before revision minting;
provider semantic rejection; unknown-byte artifact-envelope round-trip without
the provider; property/DataSpec/Region callbacks with every payload pointer
cleared; independent exact SHA-256 descriptor/content/layout vectors; content
identity across physical repacking and padding; incremental ContentDigest for
a fixed-memory generated stream larger than 64 MiB against an independently
calculated exact vector; identity across different provider callback chunk
boundaries; sticky malformed/null and `uint64_t`-overflow sink failures;
measured/hash count-drift rejection and subsequent recovery; old
Value/read/owner lifetime across replacement and unload; final
provider-before-module destroy order;
callback-local diagnostic and nonempty property copy-out with an oversized-
output boundary; checked rank-general Exact TensorSlice site counts, including
wrong nonzero, wrong zero, and `uint64_t` product overflow; and concurrent
replacement without mixed-generation resolution. Concurrent readers sample
callback-local properties from their own output states, and retained old Values
query the same property after replacement to cover thread/generation lifetime
boundaries. Four callback-tail cases additionally require owner destruction to
drain after a successful outer callback but before its worker exits, after a
failing provider callback, and after a foreign-generation destroy request;
they also preserve FIFO owner-destroy order across a cascading cleanup before
module release.

The callback-view case is the structural input-lifetime regression. It enters
validation, property, DataSpec, Region, and content callbacks through one
Value and requires every Schema/Layout record, optional Facet array, buffer
array, Layout-envelope array, metadata payload, and explicit content pointer to
remain valid for its callback. The production adapter keeps move-safe owning
storage separate from the borrowed `ps_data_value_view_v3` and materializes the
view only at the final callback caller address. A scoped no-elide run must
compile `photospider_operation_runtime` itself—not only the test source—with
`-fno-elide-constructors`, then run that case. This is a manual compiler-mode
proof, not another CTest entry or CI phase-completion check:

```bash
cmake -S . -B build-v14-no-elide \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_BUILD_IPC=OFF \
  -DPHOTOSPIDER_ENABLE_OPENCV=OFF \
  -DPHOTOSPIDER_ENABLE_YAML=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON \
  -DCMAKE_CXX_FLAGS=-fno-elide-constructors
cmake --build build-v14-no-elide \
  --target test_variable_sample_field_extensions -j 2
ctest --test-dir build-v14-no-elide --output-on-failure \
  -R '^VariableSampleFieldExtensions\.EveryCallbackReceivesOneStableMaterializedValueView$'
```

Active output bytes must equal `255 - input`; input and output row padding is
not treated as image elements.

Run the focused validation with:

```bash
cmake --build build --target test_region_contracts \
  test_cpu_dense_tensor_image_operation \
  test_packed_fp4_dense_tensor \
  test_variable_sample_field_extensions \
  public_header_self_containment -j 2
ctest --test-dir build --output-on-failure \
  -R '^(RegionContract|RegionImageAdapter|RegionPropagation|RegionRouteSelection|RegionPlanning|RegionLifecycle|CpuDenseTensorImageOperation|PackedFp4DenseTensor|VariableSampleFieldExtensions)\.'
```

`DependencyDisabledInstallSmoke` builds and runs all 48 dense cases plus all
four packed FP4 and seventeen V-14 extension cases in an actual
OpenCV/YAML/OpenEXR-discovery-disabled
product before proving the installed consumers.
`StaticProductConsumerSmoke` proves the operation-SDK-only installed consumer.
`DependencyDisabledInstallSmoke` also loads two independently linked
Value-using DSOs and proves that they mint from one shared runtime authority.
Both installed consumers construct and evaluate Region and observe a
synchronous Ready Value fence without optional dependencies. The
provider-disabled nested build below also compiles and runs all 48 dense cases
plus that dual-DSO case, so the real core operation, fence/transfer proof, and
identity authority do not depend on the optional OpenCV operation provider or
a native device SDK.

## Optional OpenCV Operation Provider Validation

`test_optional_opencv_operation_provider` is a CTest-registered integration
binary built against both provider configurations. In the normal configuration
it seeds the repository OpenCV provider, executes its real resize callback,
proves an invalid OpenCV matrix shape is translated to host-owned
`GraphErrc::ComputeError`, loads a stdlib-only v2 provider that takes complete
ownership of the resize execution/dirty/forward slots, executes the replacement
sentinel output, unloads it, and executes the restored OpenCV predecessor.

`test_opencv_operation_provider_exceptions` runs in its own process so the
first provider initialization attempt is deterministic. A private
`BUILD_TESTING` hook injects one `cv::Exception` inside the real
`std::call_once` body before `cv::setNumThreads(1)`: the first registration must
return host-owned `GraphErrc::ComputeError` without publishing callbacks, and
the next registration must retry, set the OpenCV thread count to one, and
publish the provider. The same private, uninstalled test-access boundary drives
the actual monolithic and tiled exception wrappers directly. Two independent
`cv::Error::StsNoMem` injections must each emerge as an exact, fresh
`std::bad_alloc`, while a tiled non-exhaustion failure must emerge as
`GraphErrc::ComputeError`; no test attempts real memory exhaustion or changes
the public ABI.

`OpenCvOperationProviderDisabledBuild` configures a transient nested build with
`BUILD_TESTING=ON` and
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF`, while OpenCV, YAML, graph
CLI, and operation-plugin defaults remain enabled. The provider-aware broad
suite gate is therefore off. The driver validates the exact CMake cache
profile, builds the provider-independent focused provider binary, its
stdlib-only fixture, the CPU DenseTensor/ImageView integration binary, and the
dedicated disk-cache and kernel-lifecycle concurrency binaries, plus the
provider-independent `test_kernel_contracts` internal-seam consumer, then
queries the machine-readable CTest inventory. `test_kernel_contracts` is built
to exercise the focused-only direct-consumer closure but is deliberately not
discovered in this nested inventory.

During configuration, CMake serializes every active `gtest_discover_tests`
target and its configuration-specific `$<TARGET_FILE:...>` path after
cross-checking the GoogleTest registration metadata. The generated TSV has one
exact first-line header, `# target<TAB>configured executable`, followed only by
nonempty two-field data records. The CMake writer accepts only local executable
target names composed of letters, digits, `_`, `.`, `+`, and `-`, while keeping
`$<TARGET_FILE:...>` and `$<CONFIG>` unevaluated until generation so single-
and multi-config builds select the native executable path. The reader rejects a
missing or repeated header, every later comment or blank line, an extra field,
a duplicate target, `_NOT_BUILT` input, and every non-structural C0 control or
DEL. NUL remains reader-rejected even though CMake cannot represent it. Target
paths must be lexically absolute POSIX paths, Windows drive-rooted paths, or
Windows UNC paths; ordinary spaces and Windows backslashes are valid data.

After the focused build, the driver derives the exact unlabelled
`${target}_NOT_BUILT` set from registered targets whose executable is not a
regular file. This observes the real build closure, including indirect
dependencies, without hard-coding a target count or future target name and
without deriving expectations from CTest's observed sentinels. The exact CTest
inventory is the union of that derived set and
`DependencyDisabledInstallSmoke`,
`OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores`, all 48
`CpuDenseTensorImageOperation.*` cases,
`ValueIdentityAcrossDsos.MintingAuthorityIsProcessWide`, the three
`DiskCacheDiagnosticConcurrency.*` cases, and the two
`KernelLifecycleConcurrency.*` cases.

At the current V-14 checkpoint, CMake registers exactly eight active GoogleTest
targets in this profile. The six-target focused build materializes five of
those registered executables; its sixth target, `test_kernel_contracts`, is
build-only and deliberately undiscovered. CTest discovers 55 runnable focused
cases. The three derived sentinels are exactly
`test_compute_io_executor_NOT_BUILT`,
`test_packed_fp4_dense_tensor_NOT_BUILT`, and
`test_variable_sample_field_extensions_NOT_BUILT`; together with
`DependencyDisabledInstallSmoke`, the exact CTest inventory therefore contains
59 entries. This is a verified result of the dynamic manifest and build closure,
not a target count or sentinel list maintained by the production driver.
Derived sentinels carry no label or timeout. Disk-cache cases retain only the
`kernel-concurrency` label and a 20-second timeout; lifecycle cases retain that
label and a 60-second timeout; dense-image and Value-runtime cases retain their
30-second timeout, with only the latter carrying `value-runtime`. Missing or
extra entries fail, so no provider-dependent broad test may remain registered.
The driver runs every built focused case through CTest. The disabled profile
requires dependency-neutral analyzer/math/dense-invert operations to remain
seeded, OpenCV-backed operation keys to be absent, and the replacement provider
to publish, execute, and fully retire its resize key. The transient build is a
long-lived product configuration check; it emits commands/results to CTest and
retains no per-run report. This stage disables the operation provider, not the
separate OpenCV codec, normalization, adapter, or embedded-product dependencies.

The OpenCV-provider and injected-codec nested-build drivers import the same
destructive work-tree helper from `cmake_build_smoke_support.py`. Before
removing a transient tree, that helper requires a nonempty absolute work
spelling and rejects parent traversal, the repository, every repository
ancestor, filesystem roots, and every untrusted symlink in the final work path
or an existing parent component. On Darwin it recognizes exactly one
platform-owned alias: `lstat("/tmp")` must report a root-owned symlink, strict
canonical resolution must equal `/private/tmp`, and `lstat("/private/tmp")`
must report a root-owned directory. Only then is a leading `/tmp` component
rewritten to physical `/private/tmp`; the temporary root itself is still
protected, and every later component is still inspected with `lstat`.
Linux's ordinary `/tmp` keeps the ordinary path, while a non-Darwin, non-root,
wrong-target, user-controlled, intermediate, or leaf symlink receives no
special trust.

This normalization is required at the driver boundary because CMake on macOS
may serialize a physically selected `/private/tmp/...` binary directory as
`/tmp/...` in `${CMAKE_BINARY_DIR}` and the generated CTest command. The raw
CTest registration therefore remains executable without rewriting the
registration or weakening arbitrary-symlink rejection. Canonical resolution
is otherwise used only for protected-location comparison. The complete checks
are repeated immediately before recursive removal, which receives the physical
trusted-alias spelling or the original non-symlink spelling. Recursive-removal
failures propagate, and an `lstat`-style postcondition verifies that no
directory or dangling link remains. The check/delete sequence is not an atomic
cross-platform filesystem transaction, so these drivers accept only
caller-owned transient subtrees whose components are not concurrently replaced.

`OpenCvOperationProviderBuildSmokeSafety` exercises those destructive guards,
failure propagation, and postcondition only against a synthetic repository,
ancestors, and unrelated symlink targets under a disposable temporary root.
It injects scalar Darwin ownership/type/target facts and a synthetic
logical-to-physical mapping, so every platform covers the trusted-alias
positive case without creating or replacing `/tmp`. It also locks both real
consumer modules to the common remover. Its final-symlink, symlinked-parent,
and post-normalization symlink cases require each unrelated target and marker
to survive; the test never passes the real checkout or its parents to the
remover. The driver also reads the nested
`CMakeCache.txt`: a nonempty `CMAKE_CONFIGURATION_TYPES` selects
`tests/<config>/`, while a single-config cache must contain the exact requested
`CMAKE_BUILD_TYPE`. Missing or contradictory cache state fails explicitly, and
the safety regression covers both layouts independently of the host platform.
It is a fast ordinary full-CTest regression that imports and invokes the
driver's helpers in-process; only `OpenCvOperationProviderDisabledBuild`
launches the child configure/build/CTest profile and carries `build-smoke`.

## OpenCV Operation Concurrency Validation

`test_opencv_operation_concurrency` is a CTest-registered integration binary
for the long-lived operation-provider and benchmark Run-concurrency contracts.
It uses
Host-boundary records and bounded callback gates rather than elapsed-time
thresholds:

- `BenchmarkAutoThreadsPublishRunCapAndPreserveFixedPool` proves that automatic
  selection is resolved once, process execution is prepared with
  `worker_count=0`, Graph load follows preparation, and the resolved nonzero
  Run cap reaches the Host compute request and benchmark result.
- `BenchmarkRunAllSharesPoolAndPreservesMixedSessionCaps` proves that enabled
  `1`, `2`, and automatic sessions share one preparation and retain distinct
  compute caps, while a disabled out-of-range numeric thread value is not
  range-validated or executed and an invalid enabled session is diagnosed and
  skipped.
- `BenchmarkProcessPreparationFailureRetainsDiagnosticAndCanRetry` proves that
  process preparation failure precedes Graph load, preserves the Host
  diagnostic, and leaves once-only preparation retryable.
- `BenchmarkThreadsCapCallbacksOnOneFixedExecutionPool` runs the real
  `BenchmarkService`, one explicitly fixed eight-lane Host pool, Graph load,
  and registered callback path for automatic and explicit `1/2/4/8` Run caps.
  It requires exact cap-sized callback overlap and rejects a cap-plus-one
  callback.
- `BenchmarkThreadsRejectOutOfDomainValuesBeforeGraphLoad` requires signed
  negative and above-eight Run-cap requests to fail before publishing a Graph
  session.
- `HostComputeSurfacesRejectZeroMaximumParallelismAsInvalidParameter` requires
  a present zero public Run cap to fail with `GraphErrc::InvalidParameter`
  across synchronous, asynchronous, and image compute.
- `IpcHostDispatch.MapsEveryCurrentHostVirtualWithoutFallback` and
  `IpcHostCompute.RejectsZeroMaximumParallelismBeforeTransport` prove that the
  IPC Host preserves a positive Run cap through all three compute conveniences
  and rejects zero in the public Graph error domain before transport.
- `BuiltinCurveCallbacksReachRequestedWorkerConcurrency` repeats the built-in
  tiled `curve_transform` path three times at each `1/2/4/8` Run cap on one
  fixed eight-lane pool and requires exact callback overlap through a test-only
  observer.
- `BuiltinCurveOutputMatchesBetweenOneAndEightRunCaps` compares packed pixel
  rows from the public Host result and requires one-cap and eight-cap output on
  one fixed pool to be bitwise equal.

The observer exists only in `BUILD_TESTING` builds, is private to the source
tree, and is never installed. These cases prove reachable concurrency and
deterministic output; they do not claim a machine-independent speedup.

`opencv_operation_concurrency_benchmark` is the corresponding long-lived
manual measurement tool. It is intentionally absent from CTest and CI. The
tool creates and removes a disposable temporary Graph root, executes the real
Host/benchmark/private-execution/built-in-operation path, retains no result artifact,
and prints environment, raw wall-time samples, median wall time, throughput,
speedup, and maximum callback concurrency to stdout. Build and run it with:

```bash
cmake --build build --target opencv_operation_concurrency_benchmark -j
./build/tests/opencv_operation_concurrency_benchmark \
  --size 2048 --warmups 2 --samples 7 --chain-length 4
```

The native snapshot captured on 2026-07-15 used macOS `arm64`, Clang 21.0.0
(`clang-2100.1.1.101`), OpenCV 4.12.0, reported hardware concurrency 10, and
reported `opencv_internal_threads=1`. The workload was a chain of four built-in
`curve_transform` nodes over a 2048-by-2048 FP32 image, with two warmups and
seven samples per Run cap:

| Run cap | Median wall (ms) | Throughput (Mpix/s) | Speedup | Max in flight |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 27.450 | 611.188 | 1.000 | 1 |
| 2 | 19.567 | 857.433 | 1.403 | 2 |
| 4 | 15.688 | 1069.455 | 1.750 | 4 |
| 8 | 15.008 | 1117.910 | 1.829 | 8 |

The raw wall-time samples in milliseconds were:

- cap 1: `27.694|27.134|27.450|27.183|27.869|27.250|28.035`
- cap 2: `19.021|19.567|19.774|19.497|19.435|20.427|20.997`
- cap 4: `16.059|15.688|15.992|15.727|15.600|14.692|14.649`
- cap 8: `16.436|16.610|16.512|15.008|14.859|14.064|14.760`

This snapshot establishes that the requested Run caps reached the real callback
path and that the tested machine benefited from removing outer serialization.
It is not a permanent performance baseline or pass/fail threshold. Rerun the
exact command when evaluating another machine, compiler, OpenCV version, or
operation-concurrency change, and interpret the newly printed raw samples.

## Execution-Profile SLO Manual/Release Protocol

[ADR 0010](../adr/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.md)
defines the normative `execution-profile-slo-v1` contract. Issue #92 freezes
this protocol but does not implement a runner or collector. Until Issues #93
through #96 deliver their assigned rows, no command in the current repository
can produce a conformant bundle and no current profile claim is implied.

The eventual maintained runner is a manual developer/release tool. It may live
in the primary repository because this section defines a lasting product-
measurement role, but machine-dependent latency, throughput, and reference
ratios must remain absent from ordinary CTest and default CI correctness gates.
The runner writes only to an explicit disposable path outside the checkout or
to release-artifact storage; generated bundles are not committed to the primary
or personal-overlay repository. For every B1-bearing run, either destination
must be below the selected fingerprinted `OutputStore` root or rooted namespace
and must not bypass its proven crash-durability path.

### Frozen rows and sample windows

The runner must consume the exact graph/source/edit/preview/job/cadence choices
in ADR 0010; a convenient substitute graph is not a v1 row.

| Row | Required workload and evidence | Cold | Warmup | Measured window |
| --- | --- | ---: | ---: | ---: |
| I1 isolated | `I1-edit-storm-v1`; latency, waste, memory, output correctness | 1 episode | 20 episodes | 200 episodes |
| I2 isolated | `I2-progressive-v1`; twelfth-edit (`edit_index=11`) preview/final latency, Host/conditional-Metal residency and copy waste, memory, output correctness | 1 episode | 10 episodes | 100 episodes |
| B1 cap 1 | `B1-immutable-v1`; throughput, determinism, fault-free waste, memory | seed 252 | seeds 253/254/255 | jobs `0..29` |
| B1 cap 8 | The same B1 corpus and environment except Run cap | seed 252 | seeds 253/254/255 | jobs `0..29` |
| M1 shared | `M1-shared-v1`; latency, progress, fairness, waste, memory | 1 second | 5 seconds | 30 non-overlapping one-second windows |

Every workload-bearing field and fixed-record component uses the closed,
case-sensitive `workload-id-v1` scalar. Its only accepted payloads are the four
table tokens above, whose exact frames are `16:I1-edit-storm-v1`,
`17:I2-progressive-v1`, `15:B1-immutable-v1`, and `12:M1-shared-v1`.
Generic `identifier` remains a distinct lowercase ASCII type matching
`[a-z0-9][a-z0-9._+-]*`; neither validator may borrow the other's domain.
The mandatory lexical/type oracle is:

| Declared context | Input | Expected |
| --- | --- | --- |
| `workload-id-v1` | Each of the four exact frozen tokens | Accept all four independently. |
| `workload-id-v1` | A lowercase/case-changed alias such as `i1-edit-storm-v1` | Reject; workload tokens are case-sensitive. |
| `workload-id-v1` | An unknown token such as `I3-edit-storm-v1` | Reject; the domain is closed. |
| Generic `identifier` | `abc`, `a0`, or `a0._+-` | Accept under the unchanged lowercase grammar. |
| Generic `identifier` | `I1-edit-storm-v1`, empty input, or a leading invalid byte | Reject under the unchanged lowercase grammar. |
| Evidence `workload_id` field | Exact token with type frame `10:identifier` | Reject; the required type frame is `14:workload-id-v1`. |
| Non-workload identifier field | A `workload-id-v1` type frame or workload token | Reject schema/type mixing. |

A known I1 evidence row or bundle starts its workload field with the exact
record `field=11:workload_id5:known4:none14:workload-id-v116:I1-edit-storm-v1\n`.
The `job-instance-v1` and `row-reference-v1` fixed records instead retain the
same first component payload frame and validate it under the dedicated type;
their fixed-record payloads do not contain component type tokens. A focused
wire oracle must round-trip all four workload tokens through job-instance,
15-field row, five-field bundle, and row-reference encodings; must reject any
case, unknown-token, type-frame, or enclosing-workload mismatch; and must
recompute row/bundle digests from the corrected canonical bytes. The former
`identifier` annotation never described a valid uppercase-leading v1 object.

Every row uses three fresh process/execution-domain replicates. Cold first-use
is captured separately and excluded from steady-state aggregates. Natural edit
ordinals `1..12` map to `edit_index=0..11`; the required final is the twelfth
edit (`edit_index=11`, `k=1.04`, source Region `(768,512,256,256)`, preview
Region `(192,128,64,64)`). A bare “edit 12” is not a v1 evidence identity.

I2 inherits more than the edit labels. For every `edit_index=i`, it uses the
exact I1 node-one sequence
`K=[0.82,1.18,0.86,1.14,0.90,1.10,0.94,1.06,0.98,1.02,0.96,1.04]`,
the same `edit_index=edit_ordinal-1` lookup, and the same node-one update followed
by node-one-to-node-four transform order with `k` values
`[K[i],1.00,1.20,1.40]`. Preview first computes the per-channel 4x4 box average
from the original 2048 source and rounds that source once to binary32, then
executes the shared update/transform sequence. Final starts from the original
2048 source and executes the same I1 full-resolution path; it is not an
upsampled, reused, or otherwise preview-derived value.

The mandatory I2 coefficient/path scenario oracle is:

| Scenario | Oracle |
| --- | --- |
| Exact twelve-index match | Enumerate `edit_index=0..11` and require I2 node-one values to equal I1 element-for-element as `[0.82,1.18,0.86,1.14,0.90,1.10,0.94,1.06,0.98,1.02,0.96,1.04]`, with the same ordinal, source Region, preview Region, and generation index. |
| Single coefficient drift | Replacing any one `K[i]` while retaining `I2-progressive-v1` is invalid; the deliberately changed fixture must receive a new workload id. |
| Sequence reorder or index shift | Swapping entries, using `edit_ordinal` directly as the zero-based array index, shifting/wrapping the array, or pairing a coefficient/Region/generation from another index is invalid under v1. |
| Preview rounding order | The preview oracle performs per-channel 4x4 box average on the original source, rounds once to binary32, then applies node one with `K[i]` and nodes two through four in order. Rounding after a transform, between transforms, or from already transformed pixels is invalid. |
| Full-resolution final path | Final starts from the original 2048 source, applies the same `K[i]` update and four transforms as I1, and is never produced by upsampling or reusing preview pixels. |
| Digest and golden linkage | The manifest binds the complete coefficient/mapping/order/rounding/path contract; the required `edit_index=11` final logical digest equals I1 at index 11 and the preview equals its own fixture golden. Any mismatch or manifest drift invalidates the v1 row. |

The mandatory I2 cadence scenario oracle is:

| Scenario | Oracle |
| --- | --- |
| Continuous phase grid and measured origin/index | Retain one replicate-grid origin `G^I2` for all 111 episode slots: cold starts at `G^I2`, warmup at `G^I2+1*1,500,000,000 ns`, measured at `E^I2_0=G^I2+11*1,500,000,000 ns`, and the non-start terminal boundary is `T^I2=G^I2+111*1,500,000,000 ns`. In measured, map `episode_ordinal=1..100` to `episode_index=0..99` and derive every origin as `E^I2_r=E^I2_0+r*1,500,000,000 ns`; reject any fresh episode/phase origin or transition delay. |
| Twelve-edit admission schedule | For every episode and `edit_index=i` in `0..11`, require `S^I2_{r,i}=E^I2_r+i*16,666,667 ns` and one preview Host-admission sample `A^I2_{r,i}` in the closed interval `[S^I2_{r,i},S^I2_{r,i}+2,000,000 ns]`. |
| Single invalid edit event | Move one admission below its nominal start or above its lateness bound, omit/duplicate/reorder/fail one admission, trigger checked-arithmetic overflow, or insert one cadence gap; the replicate is invalid, publication is revoked, and no edit or later episode catches up, backfills, or shifts. |
| Episode spacing and quiescence | Require consecutive origins exactly 1,500,000,000 ns apart and all prior work quiescent before the next origin; require the final measured episode quiescent before `T^I2`. The latest legal twelfth final deadline is origin plus 1,185,333,337 ns, leaving a minimum 314,666,663 ns guard that never extends the deadline. |
| Preview versus next edit | Edits `0..10` never wait for preview. Preview `i` may publish only with visibility strictly before `A^I2_{r,i+1}`; equality orders the newer edit acceptance first and makes the preview stale. |
| Shared child-deadline anchor | Checked-add preview and final deadlines as `A^I2_{r,i}+100,000,000 ns` and `A^I2_{r,i}+1,000,000,000 ns`. Retain the later final trigger/admission but never reanchor the deadline; require the twelfth preview/final visible by their bounds. |
| Existing-envelope evidence | Retain clock/replicate-grid/derived-phase-origin/index/schedule/tie rules in the existing workload-manifest section and all actual admission/deadline/visibility/cancel/drop/gap/quiescence events in measurement evidence. Recompute their section/verdict digests while preserving the closed 15-field row and five-field bundle. |
| Manifest/golden drift | Reject any origin/stride/cadence/order/lateness/anchor/tie-rule drift under `I2-progressive-v1` even when image goldens match; a deliberate change needs a new workload id and manifest/digest/golden lineage. |

For I1, after baseline settlement, an episode at monotonic origin `E` uses
`S_i=E+i*16,666,667 ns`. `A_i` is the one monotonic-clock sample immediately
before final Host admission; it starts latency and checked-adds the sole
absolute deadline `D_i=A_i+150,000,000 ns`. `A_i` must be in
`[S_i,S_i+2,000,000 ns]`; `S_i` never anchors the deadline and permitted wake
lateness never consumes the 150 ms budget. Overflow, early start, more than
2 ms lateness, missed/drop/gap, or admission failure invalidates the replicate.
The runner records accepted cancellation/supersession before any late Host
call, revokes publication, and never catches up, backfills, or shifts later
times. Entered non-preemptible work drains as waste; post-cancel starts are
zero, and missed/expired work cannot publish output, receipt, or successful
latency.

The mandatory I1 phase/drain scenario oracle is:

| Scenario | Oracle |
| --- | --- |
| Continuous isolated phase grid | Retain one `G^I1`; derive cold slot zero, warmup slots `1..20`, measured slots `21..220`, and only `T^I1=G^I1+221*750,000,000 ns` as a terminal non-start boundary. Map each phase's natural ordinal to zero-based `r`; reject a fresh phase origin, cooling delay, shifted slot, or late counter reset. |
| Exact drain anchor | For every episode require `Q_start=S_11=E+183,333,337 ns` and `Q_end=Q_start+500,000,000 ns=E+683,333,337 ns`, independent of actual admission and deadline. The window may overlap an active final Run but does not cancel it or extend `D_i`. |
| Deadline and next-origin guards | With latest legal admission, require `D_11<=E+335,333,337 ns`, exactly 348,000,000 ns from that deadline to `Q_end`, and exactly 66,666,663 ns from `Q_end` to the next origin. Reset/baseline preparation must fit that guard; the last measured episode uses the same guard before `T^I1`. |
| Boundary tie and settlement | At `Q_start`, nominal marker precedes equal-time admission. At `Q_end`, equal-time lifecycle events apply in retained causal order before the quiescence snapshot. Active work at the snapshot or later terminal/settlement is invalid. |
| Fail-closed arithmetic/evidence | Reject checked overflow in grid/slot/start/admission/deadline/drain arithmetic, missing or duplicate boundary/event evidence, a moved origin, nonquiescence, or a workload-manifest rule drift under the same id. Existing section/verdict digests bind the evidence without changing the 15/5-field envelope. |

M1 uses the same per-episode drain rule for
`E_r=M_0+r*750,000,000 ns`, `r=0..39`, starts exactly 40 measured episodes,
and keeps cap-eight B1 offered continuously. These are reproducible nominal
monotonic times and lateness bounds, not exact OS wake claims.

Disk-cache/codec I/O and cross-episode/job result reuse remain disabled. I1/I2
retain only their explicitly recomputed baseline/current episode target and
declared I2 output residency; every B1 job starts without a reusable fixture
result. Warmup B1 jobs execute the complete artifact path under separate
identities/directories; their output is removed after owner settlement while
process/provider/JIT state remains.
Warmup occurrence-owned observations never enter measured aggregates; the M1
boundary resets logical counters without a process restart or state reset.

The mandatory M1 pre-boundary input-grid oracle is:

| Scenario | Oracle |
| --- | --- |
| Exact phase origins and intervals | Checked-derive `C^M1=B^M1-6,000,000,000 ns` and `W^M1=B^M1-5,000,000,000 ns=C^M1+1,000,000,000 ns`, retain sequences `c^M1<w^M1<b^M1`, and use exactly `[(C,c),(W,w))`, `[(W,w),(B,b))`, and `[(B,b),(U,u))`. Underflow, overflow, a shifted boundary, a different phase length, or a runner-selected origin is invalid. |
| Cold origin and settlement | At `(C^M1,c^M1)`, establish the sole cold I1 origin and offer Graph A seed 252 with `(phase=cold,cycle=0,attempt=0)` after the boundary marker; an equal-time I1 admission follows that offer. Require the I1 `Q_end=C^M1+683,333,337 ns` quiescence snapshot and B252 terminal/owner settlement/output removal before `W^M1`; the fixed 316,666,663 ns I1 guard does not move `W^M1`, and a miss is invalid rather than a drain. |
| Warmup origins and count | At `(W^M1,w^M1)`, verify cold already settled and establish exactly `E^M1_warmup,k=W^M1+k*750,000,000 ns`, `k=0..6`. Reject an omitted/duplicate origin, another count/index, a phase-continuous grid back-derived from `C^M1`, or a delayed transition. |
| Fixed warmup B1 offer protocol | At `W^M1`, offer B253 then A254 with `w^M1<sequence(B253)<sequence(A254)` and warmup cycle/attempt zero; an equal-time first I1 admission follows both. Offer B255 synchronously only when B253 becomes terminal, with a greater same-time sequence, and require B255 to have been offered before `(B^M1,b^M1)`. Graph A has no warmup successor. The offered prefix is protocol-fixed; only its incomplete subset is terminal-history-derived. |
| Deterministic cross-`B^M1` I1 | Warmup origin `k=6` is exactly `B^M1-500,000,000 ns` and has `Q_end=B^M1+183,333,337 ns`; require that settlement-pending warmup occurrence/generation in the `B^M1` snapshot. At its `Q_end`, require only that occurrence/generation to be quiescent and settled, not the concurrent measured generation or the whole shared service. |
| Immutable attribution and temporal effect | Keep every event/result owned by the last warmup generation in `phase=warmup`, including cancellation or settlement caused after measured latest-wins supersession. Exclude its occurrence-owned values from measured aggregates, but include every post-`B^M1` start, contention, reservation/grant, Compute I/O, and high-water effect in time-windowed evidence. |
| No hidden transition | Cold/warmup transitions do not pause, wait, cool, restart, rebuild queues, release shared resources, or shift a boundary. Retain all origins/counts/indexes, fixed offers, terminal-derived B255 transition, phase endpoints, and failures in the existing workload-manifest/measurement sections and recompute their digests without adding an outer field. |

The mandatory M1 phase-boundary scenario oracle is:

| Scenario | Oracle |
| --- | --- |
| Exact boundary and interval | Retain boundary coordinate `(B^M1=M_0,b^M1)`, checked terminal-cutoff coordinate `(U^M1=B^M1+30,000,000,000 ns,u^M1)`, and unique strictly increasing row-local event sequences. Order equal timestamps by `(monotonic_timestamp,event_sequence)`; the measured interval is `[(B^M1,b^M1),(U^M1,u^M1))`. |
| Ordered zero-duration transition | At `(B^M1,b^M1)`, atomically close the warmup I1 cadence and both B1 Graph producers, snapshot every previously offered incomplete warmup I1/B1 occurrence and state, reset only logical measured accumulators, and establish measured I1 origin. Then offer measured Graph A job zero followed by Graph B job one at timestamp `B^M1`, both at producer-local cycle zero and with sequence values strictly greater than `b^M1`. Require no event interleaving in the snapshot/reset and no pause/wait/cooling/drain/boundary cancellation/restart/queue rebuild/resource release. |
| Supersession order | If the first measured-I1 actual admission has timestamp `B^M1`, require its sequence after both measured B1 offers. Its accepted latest-wins supersession therefore follows the frozen snapshot; it cannot delete or rewrite the warmup generation, and every causally later cancellation/terminal/settlement event keeps warmup attribution. |
| Carryover identity and FIFO | Preserve warmup phase/cycle/job/attempt, queue predecessor, admission state, reservation/grant, and owner settlement. Measured cycle-zero offers follow each Graph's already-offered warmup prefix even when queued/running; this transition alone bypasses predecessor-terminal offer timing. Subsequent measured offers resume the normal per-Graph rule and never advance or rewrite an incomplete warmup identity. |
| Occurrence attribution | Attribute terminal/completed service, output bytes, latency, receipt/golden/digest, determinism, retry/duplicate/discarded service, waste, and settlement by immutable phase. Exclude warmup occurrence-owned quantities after `B^M1` from measured throughput, Jain service `x`, latency, determinism, and waste aggregates. |
| Temporal scheduler/resource effects | Include every post-boundary phase's actual class starts, headroom failures, queue contention, reservations/grants, Compute I/O state, and Host/device/ready-memory high-water. Count a warmup Throughput start in the measured class-start rule while retaining measured-only Jain completed service. |
| Failure and terminal settlement | Invalidate on warmup carryover failure, missing event evidence, a duplicate event sequence or non-total coordinate, phase/identity/FIFO rewrite, boundary-only cancellation, snapshot mismatch, or unproved settlement. At `(U^M1,u^M1)`, stop new measured offers without cancelling outstanding work; retain endpoints at or after the cutoff but exclude them from 30-second numerators, then require exact-zero teardown. Quiescence is not required at `B^M1`. |
| Existing-envelope evidence | Retain `C^M1`, `W^M1`, `B^M1`, `U^M1`, all phase intervals/origins/counts/indexes, fixed pre-boundary offers, actual terminal-derived transitions, tie/step order, carryover snapshot, phase joins, first measured offers, per-Graph predecessor/next-cycle counters, counter epochs, queue/start/terminal/receipt events, resource effects, failures, and final settlement in existing manifest/measurement sections and digests. Any deliberate rule change needs a new workload id; outer row/bundle fields remain 15/5. |

The v1 resource profile is 32 CPU slots, 1 GiB Host retained memory, 512 MiB
Host scratch, 65,536 ready entries, 256 MiB ready bytes, and Interactive
headroom of one CPU slot, 64 MiB retained memory, 32 MiB scratch, 1,024 ready
entries, and 16 MiB ready bytes. Compute I/O admission is limited to 64 tasks
and 256 MiB of summed planned bytes. A configured Metal executor uses 512 MiB
device memory and 256 MiB device scratch; absent Metal is predefined
`not-applicable`.

For B1 fairness evidence, a Graph is eligible while its producer has
unconsumed offered demand and has not paused submission, including bounded-
admission wait. Isolated B1 offers both ordered 15-job queues at its measured
boundary. M1 uses the boundary oracle above: its first measured per-Graph offers
follow any retained warmup prefix, Graph A then repeats `0,2,...,28`, and Graph
B repeats `1,3,...,29`. Each producer starts its own next local cycle
immediately after its own final job becomes terminal, even if the other remains
in the prior local cycle. A cross-Graph barrier or producer gap is invalid.
Neither path admits all 30 Runs outside normal bounds.

Every B1 occurrence is indexed by canonical `job-instance-v1`
`(row_workload_id:workload-id-v1,replicate_ordinal:uint64,
phase:enum(cold|warmup|measured),cycle_ordinal:uint64,job_index:uint64,
run_cap:uint64)`.
Phase-local cycle zero covers cold/warmup and isolated measured B1. For
measured M1, the unchanged `cycle_ordinal` component stores the producer-local
counter, with lane derived from job parity: Graph A increments after its job 28
terminal and immediately offers job zero of the next local cycle; Graph B
independently increments after job 29 and offers job one. The logical I/O task
adds stage and the full task identity adds `attempt`. Capacity rejection or an
idempotent duplicate keeps attempt zero and the same charge; only an explicit
retry after terminal failure increments it. Cycle is never retry. Charge,
admission/status, snapshot, start/terminal, commit id/slot, receipt, raw trace,
and row evidence must carry the complete job-instance identity. The normalized
semantic trace remains job-index based and its digest is joined to each unique
occurrence through the row job-instance index.

Every B1 job writes the exact ADR 0010 `output.rgba32le` payload and fixed-order
`manifest.txt` in a fresh disposable directory below the selected fingerprinted
`OutputStore` root. Its two ordered
`ComputeIoExecutor` tasks use stable
`(job_instance_id,stage,attempt)` charge identities: payload-stage has
`planned_bytes=67,108,864`, while manifest-commit has that job's exact
`242 + decimal_digit_count(job)` manifest length: 243 bytes for jobs `0..9`,
244 bytes for jobs `10..99`, and 245 bytes for jobs `100..255`. Thus measured
jobs `0..29` use 243 or 244 bytes and cold/warmup jobs `252..255` use 245 bytes.
Samples immediately after every accepted admission/settlement must prove <=64
active tasks, <=268,435,456 active planned bytes, both high-water marks, and
exact zero final settlement. Every active planned-byte total is the checked sum
of the true per-job charges. Planned bytes are mandatory, authoritative
evidence for Compute I/O admission, planned-byte high-water, and final
settlement, but are estimates rather than physical memory ownership evidence;
they do not replace RSS or ledger/device ownership evidence.

The target `OutputStore` requests and must achieve typed `crash-durable`; it
settles the payload, publishes the canonical manifest no-replace and last,
completes all leaf-to-root barriers, then returns the ADR 0009 receipt. Weaker,
unsupported, or failed durability is invalid. The commit id, rooted no-replace
slot, and receipt bind the complete job instance as well as its fixture job
index. A B1 job contributes throughput
only after that receipt and both logical/raw golden checks. Every I2
twelfth-edit (`edit_index=11`) preview/final is
acquired twice through the same Host binding. A configured Metal device permits
one exact-size first upload per distinct preview/final revision; the second
access must hit the same residency. No CPU copy, readback, disk/codec access, or
additional transfer is permitted.

I2 uses the ADR 0010 target state machine, not an invented current API: one
replicate-grid origin fixes a continuous 111-slot cold/warmup/measured grid,
with measured beginning at stride 11, a terminal quiescence boundary at stride
111, and exact 1,500 ms episode spacing; no phase transition chooses a new
origin or inserts a delay. Every episode admits
twelve edits on the 16,666,667 ns nominal schedule with at most 2 ms lateness.
Before each nominal start, the harness pre-mints a realtime request generation;
successful Host admission makes it current, immediately submits the legal
`RealTimeUpdate`/`Interactive` preview child, and arms the legal
`GlobalHighPrecision`/`Full` final child under the shared realtime request
identity. Edits `0..10` never wait: the next acceptance follows the frozen
schedule, and a prior preview must be visible strictly before it to remain
current. The final submits only when its preview becomes visible while still
current. A newer generation revokes both older publication permissions. Preview
latency and both child deadlines anchor to the same actual preview admission;
final trigger/admission is retained but cannot reset the 1,000 ms deadline.
Only `edit_index=11` must publish both, in order and within the two absolute
bounds. #94 implements this frozen cadence and the I1 coefficient/update
sequence and full-resolution final path; it cannot redefine the cadence or
select a different coefficient for edits `0..10` while retaining
`I2-progressive-v1`.

Required logical values call `compute_content_digest(Value)` and require
`Available`, a present `ContentDigest`, and
`CanonicalDigestAlgorithm::Sha256CanonicalV1`. Logical digest, raw little-endian
payload SHA-256, canonical manifest SHA-256, semantic-trace SHA-256, and the
logical/raw golden identity remain separate evidence families.

### Storage environment fingerprint

The closed byte schema is normative in
[ADR 0010](../adr/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.md#storage-base-and-row-environment-manifests-are-closed-v1-schemas).
The runner and an independent validator must reject provider extensions,
alternate “equivalent” objects, omitted records, and best-effort parsing.

The exact storage field order is:

```text
output_store_contract_id
output_store_contract_generation
backend_semantics_id
backend_semantics_generation
backend_instance_id
backend_class
locality
persistence
filesystem_type
mount_identity
mount_effective_options
commit_semantics
durability_capabilities
requested_durability
achieved_durability
durability_endpoint_identity
durability_anchor_identity
storage_class
b1_performance_configuration
hardware_write_cache_policy
power_loss_protection_policy
```

The exact base field order is:

```text
os_family
os_release
kernel_name
kernel_release
architecture
cpu_inventory
gpu_inventory
other_device_inventory
compiler_id
compiler_version
compiler_target
standard_library_id
standard_library_version
build_mode
build_flags
process_worker_count
provider_contracts
plugin_contracts
resource_limits
metal_resource_limits
cache_preconditions
residency_preconditions
power_policy
thermal_eligibility
```

The exact environment-class field/type order is:

```text
base_environment_digest:sha256
storage_environment_applicability:enum
storage_environment_not_applicable_reason:enum
storage_environment_digest:sha256
```

The validator applies the ADR's exact types, enum domains, nested record
layouts, cardinalities, and fixed resource values. Repository/dirty identity,
subject binary hashes, selected absolute path, and fresh job directories stay
in mandatory raw evidence but outside both manifests. The storage adapter maps
every backend into the same `durability_endpoint_identity` and
`durability_anchor_identity` fields; it cannot supply a provider-specific
replacement field.

Each manifest record uses the exact ASCII length-frame form
`field=<frame(name)><frame(state)><frame(reason)><frame(type)><frame(payload)>`
plus LF. `frame(B)` is unpadded decimal byte length, colon, then `B`. The exact
`uint64` lexical language is `0|[1-9][0-9]*` over the complete ASCII value,
with the inclusive numeric range `0..18446744073709551615`; leading-zero forms
such as `00` and `01`, and overflow, are invalid. Text is NFC UTF-8 encoded as
lowercase hexadecimal; identifiers, enums, booleans, SHA-256 values, lists,
maps, sets, and fixed records follow the closed ADR grammar. Headers are exactly
`execution-profile-storage-environment-v1\n`,
`execution-profile-base-environment-v1\n`, and
`execution-profile-environment-class-v1\n`. Missing/extra/reordered/duplicate
records, malformed lengths, BOM/CR/extra whitespace, noncanonical scalars,
or an unsorted or duplicate item in a set, map, record list, or other binding
that requires uniqueness are invalid.

The validator does not infer a concrete collection type from that generic
description. `token-set-v1` is a count plus one frame per exact raw ASCII
token, sorted/unique by the unframed token bytes; empty is `0:` and an unknown
token is invalid. `ordered-text-list-v1` is a count plus one frame around each
canonical lowercase-hex `text` payload in invocation order; duplicates are
allowed and empty is `0:`. CPU/device/contract record lists frame each complete
fixed-record payload, sort/unique by the complete unframed record bytes, and
enforce their respective `>=1`, `>=0`, and provider/plugin cardinalities.
Mount and commit types use the generic map grammar with exactly 7 and 6 sorted
raw-token pairs. Every other named composite uses the generic fixed-record
grammar with exactly the ADR-declared component frames and no component names
on the wire.

The exact `b1-performance-configuration-v1` fixed-record component/type order
is:

```text
compression_mode:enum
compression_algorithm:identifier
compression_level:uint64
compression_profile:identifier
encryption_path:enum
encryption_profile:identifier
checksum_mode:enum
checksum_algorithm:identifier
deduplication_mode:enum
logical_block_bytes:uint64
physical_block_bytes:uint64
record_bytes:uint64
allocation_unit_bytes:uint64
allocation_mode:enum
provisioning_mode:enum
layout_mode:enum
layout_data_units:uint64
layout_parity_units:uint64
layout_replica_count:uint64
layout_stripe_unit_bytes:uint64
layout_profile:identifier
upper_write_cache_mode:enum
upper_write_cache_profile:identifier
io_scheduler:identifier
io_queue_policy:enum
io_queue_depth:uint64
io_concurrency_policy:enum
io_concurrency_limit:uint64
network_path:enum
network_protocol:identifier
network_link_profile:identifier
network_mtu_bytes:uint64
network_qos_profile:identifier
network_region:identifier
backend_service:identifier
backend_performance_tier:identifier
device_performance_profile:identifier
```

The ADR's closed enum/sentinel/cross-component rules are part of validation.
Zero byte units and `not-applicable` identifiers require affirmative proof of
an absent/non-applicable layer; they cannot encode opacity. The configuration
is captured before warmup and remains stable through the replicate. It excludes
disposable paths, subject commits/binaries, and instantaneous load, queue,
cache, autoscaler, free-space, RTT, or jitter samples; those remain raw
preconditions/diagnostics and need not be exactly equal between runs.

For `layout_mode=provider-managed`, all four geometry components remain in the
fixed record. A positive value means the concept exists on the complete path
and the exact effective value was observed. Zero requires the corresponding
retained raw-proof kind: `provider-layout-data-units-absent`,
`provider-layout-parity-units-absent`,
`provider-layout-replica-count-absent`, or
`provider-layout-stripe-unit-absent`. Those closed labels prove concept absence
and do not create a component state, N/A pair, field, or digest input. The
stable non-placeholder `layout_profile`, four values/proofs, and complete path
must come from one frozen observation and satisfy the recorded backend-
semantics generation. Opacity, variability, nondisclosure, or a missing exact
value makes the entire performance field
`unprovable/evidence-chain-incomplete`; contradictory values/proofs make it
`unprovable/conflicting-effective-values`. A partial fixed record and opacity-
encoded zero are both invalid.

The observation state is one of `known`, `not-applicable`, `unknown`,
`unobserved`, `unsupported`, or `unprovable`. Known uses reason `none` and a
canonical payload. Every other state has an empty payload and only its closed
state-specific reason. The only eligible N/A pairs are filesystem absence for
`filesystem_type`, mount absence for `mount_identity` and
`mount_effective_options`, hardware-cache/PLP layer absence for their matching
policy fields, absent configured Metal for `metal_resource_limits`, and
`row-has-no-output-commit` for an I1/I2 storage-digest record. Lack of a probe,
provider opacity, or a remote boundary never proves absence.

For a mounted backend, `mount_effective_options` has exactly seven sorted keys:
`access_mode`, `atime_policy`, `cache_coherence`, `copy_on_write_mode`,
`data_write_mode`, `journal_mode`, and `metadata_write_mode`, with only the ADR
enum values. The adapter emits effective behavior: omitted default and explicit
default inputs canonicalize identically, native order is discarded, and case
folding occurs only for a platform-declared ASCII case-insensitive option
domain. A platform-defined duplicate winner is probed and emitted once; an
unproved/conflicting winner is `unprovable/conflicting-effective-values`.
Unknown native options are excluded only with retained proof that they affect
neither the seven keys, `commit_semantics`, the fixed performance record,
hardware-cache/PLP policy, nor performance/durability anywhere on the complete
B1 write/sync/barrier/provider-commit/revalidation/golden-readback path;
otherwise normalization or the performance record is unprovable. The six
fixed `commit_semantics` keys, eight closed durability capability tokens, and
37 fixed performance components are validated independently. Btrfs
`compress=zstd` and disabled compression must encode different performance
records and cannot compare as one environment.

Eligibility reasons are recomputed as exact predicates rather than accepted as
an arbitrary subset. A failure anywhere in canonical framing, lexical/scalar/
composite validation, field/type/state/reason rules, domain/cardinality,
ordering/uniqueness, fixed-record shape, or cross-field validation produces the
single reason `canonical-schema-invalid` and stops eligibility evaluation. For
a canonical manifest, the validator evaluates every row below and emits all
and only true tokens once, in unsigned-ASCII order. No true token means
`eligible`; one or more means `ineligible`.

| Token | Exact trigger for a canonical manifest |
| --- | --- |
| `commit-semantics-inconsistent` | Known commit-map values and retained transaction/receipt observations cannot form one consistent payload-stage, manifest-last, no-replace, synchronization, and complete barrier/provider-transaction commit. |
| `durability-class-not-crash-durable` | A known requested or achieved durability value is not `crash-durable`; an ineligible state without a known weaker value is handled by the required-observation predicate. |
| `durability-path-inconsistent` | Known contract/backend/instance/mount, endpoint, anchor, commit, and receipt/path facts affirmatively conflict or identify more than one path. Mere missing binding proof is a raw-proof failure. |
| `mount-normalization-unprovable` | A present mount cannot be reduced uniquely because `mount_identity` or `mount_effective_options` is unprovable, normalization resolution is unresolved, or retained native observations contradict the known identity/seven-key map. |
| `not-applicable-proof-invalid` | A permitted N/A pair lacks its exact complete-path layer-absence proof or that proof conflicts with the path. |
| `performance-configuration-unprovable` | The field is not known, a relevant option lacks mapping/no-effect proof, provider geometry is incomplete, or the frozen configuration drifts. Complete observed drift maps here. |
| `raw-observation-proof-incomplete` | Proof required for a known storage value, permitted N/A claim, or raw-to-canonical normalization is missing, incomplete, stale, or conflicting. It does not absorb schema, capability, durability-class, complete-evidence inconsistency/drift, or containment failures. |
| `required-capability-absent` | Known `access_mode` is `read-only`, or any of the eight required durability tokens is absent. |
| `required-observation-ineligible` | A required storage field is `unknown`, `unobserved`, `unsupported`, or `unprovable`; a permitted N/A state is handled by its proof predicate. |
| `root-containment-unproved` | A measured job or retained release artifact lacks a successful unambiguous containment proof, or that proof fails/conflicts. |

The exact possible order is `canonical-schema-invalid`,
`commit-semantics-inconsistent`, `durability-class-not-crash-durable`,
`durability-path-inconsistent`, `mount-normalization-unprovable`,
`not-applicable-proof-invalid`, `performance-configuration-unprovable`,
`raw-observation-proof-incomplete`, `required-capability-absent`,
`required-observation-ineligible`, `root-containment-unproved`. Category and
raw-proof tokens deliberately overlap only when both predicates are true: for
example, an unprovable mount with conflicting raw mapping emits mount,
raw-proof, and required-observation tokens, whereas a fully observed
configuration drift emits only the performance token. The reason list is not
an environment-digest input but must be independently reproducible.

The independent validator performs these steps in order:

1. parse every frame with checked `uint64` length/count arithmetic and require
   exact header, LF, field count, field order, type, state/reason pairing, and
   end of input;
2. validate scalar/composite canonical form, enum domains, list cardinality,
   the concrete token/text/record-list and map/fixed-record bindings,
   ordering/uniqueness, nested record shape, fixed resources, all 37
   performance components, and cross-field consistency; if step 1 or 2 fails,
   return exactly `canonical-schema-invalid` and stop before raw-proof, digest,
   environment-class, or other eligibility evaluation;
3. bind each normalized field to retained raw observation/proof and validate
   every field-specific N/A claim, mount normalization decision, stable
   instance/endpoint/anchor identity, fixed performance configuration,
   excluded-option no-effect proof, and root-containment proof;
4. recompute `storage_environment_digest` and `base_environment_digest` as
   lowercase SHA-256 over their complete exact manifest bytes;
5. parse the exact four-field environment-class manifest and recompute
   `environment_class_digest`; B1/M1 require known `required` plus the storage
   digest, while I1/I2 require known `not-applicable`, reason
   `row-has-no-output-commit`, and a N/A storage-digest record with empty
   payload; and
6. evaluate every canonical-manifest predicate in the table, emit all and only
   true reason tokens once in unsigned-ASCII order, and derive `eligible`
   exactly from an empty list or `ineligible` from a nonempty list. The reason
   list is retained evidence but is not included in an environment digest.

Exact compatibility requires byte-identical canonical manifests, equal
independently recomputed digests, and eligibility where storage applies.
For storage, that byte comparison includes the complete framed performance
record. Digest equality alone is insufficient. Candidate/reference I1/I2 use
exact base compatibility and the fixed storage-N/A environment manifest.
Candidate/reference B1/M1, B1 cap-1/cap-8, and M1/paired-B1-cap-8 use exact
base, storage, and full environment-class compatibility. M1/paired-I1 compares
only exact base manifests/digests; its environment manifests intentionally
differ. A missing raw field/proof, invalid state, byte/digest mismatch, or
failed containment makes the affected relative verdict `invalid`.

Issue #95 must add deterministic mechanism tests covering fixed field/type/
enum/cardinality rejection; every state/reason/payload combination; NFC/text
and scalar encodings, including accepted uint64 `0`, `1`, `2`, `8`, `9`, `10`,
`23`, and `18446744073709551615` plus rejected `00`, `01`, and overflow; the
exact 156-byte durability set and its 221-byte field record; known-empty
ordered text, including repeated flags, versus zero-byte N/A payloads; every CPU/
device/contract record-list cardinality, frame, sort, and duplicate rule;
mount/commit map counts and every fixed-record component order; omitted versus
explicit mount defaults; native option order/case; deterministic and
conflicting duplicates; unknown-option proof; malformed/overflowed frames;
all 37 performance fields, enum/sentinel/zero/cross-component rules, transient-
noise exclusion, positive/absence/opaque/conflicting cases for each of the four
provider-layout components, unmapped-option fail-closed behavior, and the Btrfs compression
mismatch; all three independent digest recomputations; the eleven-reason truth
table, unsigned-ASCII order, canonical-invalid short circuit, exact overlap,
and eligible empty set; and exact B1 candidate/reference plus cap-1/cap-8
compatibility. Issue #96 reuses those fixtures and tests exact same-ordinal
M1/B1 matching plus base-only M1/I1 matching. Issue #92 adds no current test
binary, serializer, probe, runner, API, or runtime field.

### Run procedure

For each candidate or reference bundle:

1. for a candidate, resolve `comparison_reference_bundle_digest` to exactly
   one retained canonical five-field bundle before evaluating any reference-
   relative row; independently recompute its bundle digest, require a same-
   workload `reference` role, and validate its complete canonical row list;
   a reference instead uses the closed N/A encoding;
2. start a fresh process for each replicate and record repository commit,
   dirty state, build/compiler/flags, OS/kernel, CPU/GPU/device inventory,
   power/thermal eligibility, provider/plugin binaries and generations,
   process workers, Run caps, all limits/headroom, fixture hashes, seeds, and
   cache/residency preconditions; encode and independently validate the exact
   24-field base manifest before warmup; for B1/M1 also select the
   `OutputStore` root, capture its raw storage/capability/configuration
   observations, encode the exact 21-field storage manifest, freeze the fixed
   performance configuration, and compute its eligibility and digest;
3. require candidate and reference to have the same evidence schema, workload
   id, row-applicable environment class, limits, and fixture hashes; B1/M1
   comparisons require byte-identical eligible storage/base manifests and a
   matching four-field environment-class manifest, while I1/I2 use the fixed
   storage-N/A environment class and do not acquire an unrelated storage
   requirement;
4. for M1 replicate ordinal `1..3`, pin same-subject, same-ordinal isolated I1
   and isolated B1 cap-8 row/bundle digests; require byte-identical base
   manifests and equal recomputed base digests for both pairs, and an exact
   eligible full environment-class match only for the B1 pair, together with
   compatible resources, fixtures,
   build/providers, and preconditions, while retaining the separate candidate
   `comparison_reference_bundle_digest` semantics;
5. retain cold first use and run the exact non-measured warmup; for isolated I1
   keep its already fixed 221-slot grid, and for M1 execute the exact
   `C^M1`/`W^M1` I1-origin and B1-offer protocol, prove the fixed cold
   settlement and cross-`B^M1` warmup occurrence, then execute the ordered
   `B^M1` cutoff/carryover snapshot/counter reset/first-offer/supersession
   transaction without replacing or pausing the frozen environment, followed
   by the exact measured interval through `U^M1` and final settlement;
6. assign and retain the canonical job-instance index before B1-bearing work,
   reject repeated phase/cycle/job coordinates, and verify that every charge,
   admission, commit, receipt, and evidence join uses occurrence rather than
   retry identity;
7. capture raw origin/drain/boundary sequence, carryover/FIFO/phase attribution,
   admission, visibility, cancellation/quiescence, start, completion,
   offered-demand eligibility, artifact/receipt, trace, digest, transfer/copy/
   residency, and resource-lifetime observations at their owning boundaries;
8. reject any required telemetry cursor gap/drop rather than estimating lost
   observations;
9. compute every replicate aggregate and independent dimension verdict from
   raw evidence using checked arithmetic; and
10. seal external prerequisites, retained sections/provenance, rows, and the
    enclosing bundle in address-dependency topological order; compute their
    distinct domain-separated digests without direct or transitive
    self-reference; enforce functional row-key uniqueness and exact row
    selection for every comparison/pair; and independently recompute every
    section, aggregate, and verdict before reporting conformance.

All durations use a monotonic clock. Percentiles use nearest rank: sort `N`
samples and select one-based rank `ceil(p*N)`. Every replicate must pass; pooled
samples and a median summary cannot hide a failed process.

### Formulas and gates

| Dimension | Required calculation and pass rule |
| --- | --- |
| Latency | I1 starts immediately before final Host admission and ends at matching current visibility. I2 twelfth-edit (`edit_index=11`) preview starts before preview admission and ends at preview visibility; final uses that same start and ends at final visibility. I1 p50/p95/p99 <=50/100/150 ms and 100% final success; I2 preview p50/p95/p99 <=50/75/100 ms and final p95/p99 <=500/1000 ms, with required `ContentDigest` matches; M1 also satisfies I1 absolute bounds and p99 <=2.0x its same-ordinal paired isolated I1. Cancelled intermediates are excluded; accepted-cancel-to-quiescence is separate. |
| Throughput | Successful logical RGBA pixel-site transforms per second, reported as MPix-op/s; one B1 job contributes 16,777,216 site-operations only after Run success + crash-durable receipt + logical/raw golden verification. The interval ends at final golden verification. Pair candidate/reference replicate ordinals under one exactly compatible storage environment: median ratio >=0.95 and every ratio >=0.90. Each M1 one-second B1 rate uses its same-subject, same-ordinal, storage-compatible paired isolated cap-8 B1 rate; p05 >=0.20, and a missing/zero/incompatible denominator or storage fingerprint is invalid. |
| Fairness | For a complete one-second window where both B1 Graphs retain unconsumed offered demand without a producer pause, `J=(x_A+x_B)^2/(2*(x_A^2+x_B^2))`, where `x` is completed `work_units + ceil(ready_bytes/4096)`. Zero total service is invalid; p05 Jain >=0.95. While both classes remain startable, at most three Interactive starts precede Throughput. M1 also has zero headroom-caused Interactive admission failures and independently passes latency/progress. |
| Determinism | For the same B1 job index across three replicates, fresh-process restart, and Run caps 1/8, typed logical `ContentDigest`, raw payload SHA-256, canonical manifest SHA-256, `execution-profile-semantic-trace-v1` SHA-256, and job-indexed logical/raw golden mismatch counts are all zero. |
| Waste | `discarded_started_service / all_started_service`, using `work_units + ceil(ready_bytes/4096)`. Every started callback whose result cannot commit is charged; entered non-preemptible work drains honestly. I1/I2 Interactive <=0.25 per replicate, and M1 applies that bound to Interactive service alone; work starting after accepted cancellation/supersession is exactly zero. I2 extra filesystem/codec, CPU-copy, readback, transfer, and allocation bytes are zero under its permitted first-transfer rule. Fault-free isolated/mixed B1 discarded/duplicate/retry service is zero. |
| Memory | Independent high-water bytes for Host retained, Host scratch, ready bytes, and configured-device memory/scratch, plus B1 active Compute I/O tasks/planned bytes. No absolute limit exceed; isolated row-owned deltas and B1 I/O counts return to the pre-row baseline/zero, and M1 shutdown returns to zero. Candidate B1/I2 peaks are <=105% of the pinned same-environment reference. Process RSS is diagnostic only. B1 planned-byte charge and event-aligned samples are mandatory, authoritative evidence for Compute I/O admission, planned-byte high-water, and final settlement; they do not establish physical memory ownership or replace RSS or ledger/device ownership evidence. |

Each required dimension emits `pass`, `fail`, `invalid`, or a schema-defined
`not-applicable`; there is no composite score. Missing source evidence,
arithmetic overflow, monotonic-clock failure, cursor/drop gaps, fixture or
environment drift, an unpinned/incompatible reference, a zero required
denominator, a missing/ineligible/mismatched required storage fingerprint, or
an unapproved `not-applicable` makes the affected row invalid and
non-conformant. Equal unknown or unobserved storage states do not establish a
compatible environment.

The semantic trace uses exactly three records per deterministic plan task:
`ready`, `start`, and `terminal`. Records carry job/Graph, contiguous
plan-relative task ordinal, sorted dependency ordinals, action/outcome, and the
declared resource vector. The exact ADR 0010 ASCII header/field order, unpadded
decimals, LF termination, numeric job/task and action-rank sort, and lowercase
SHA-256 are mandatory. Duplicate/missing/unknown records or fields, invalid
dependencies/outcomes/encoding, or collector gaps are invalid. Physical time,
worker/queue/global identities, raw sequence, retry, and completion order are
excluded from the canonical bytes but retained in the separate raw trace.

### Evidence bundle

An `execution-profile-slo-v1` bundle contains:

- all provenance and frozen environment values listed above;
- the exact base and environment-class manifest bytes plus claimed and
  independently recomputed `base_environment_digest` and
  `environment_class_digest`; for B1/M1, the exact storage manifest bytes,
  raw capability/performance-configuration/root-containment observations,
  eligibility/reasons, and both claimed and recomputed
  `storage_environment_digest`;
- workload/fixture/source/graph/payload hashes and all seeds;
- warmup, cold, and measured counts/windows kept separately, including I1 grid/
  drain boundaries and all M1 cold/warmup/cutoff/terminal coordinates, exact
  pre-boundary origins/counts/offers, event order, carryover snapshot,
  per-Graph producer-cycle counters, counter epochs, and phase attribution;
- raw samples/events, offered-demand eligibility intervals, queue/carryover
  transitions, and drop/gap counters;
- typed logical output, raw payload, artifact-manifest, semantic-trace, and
  logical/raw golden digests, plus typed requested/achieved durability and
  complete commit receipts;
- transfer/copy/residency identities, bytes, and reuse outcomes;
- authoritative resource samples and high-water/settlement deltas, including
  event-aligned Compute I/O task/planned-byte samples and charge identities;
- units, formulas, denominators, aggregates, invalidation reasons, and one
  verdict per required dimension; and
- `subject_role`, the candidate's immutable
  `comparison_reference_bundle_digest`, and for every M1 replicate the separate
  same-subject/same-ordinal isolated-I1 and isolated-B1-cap-8 row/bundle
  digests, with base-only I1 compatibility and exact storage-compatible B1
  pairing.

The outer wire format is not implementation-defined. Each row is the exact
`execution-profile-evidence-row-v1\n` header followed by the ADR 0010 fixed 15
field records: `workload_id:workload-id-v1`, subject/replicate/cap, three
environment coordinates, five section digests, and two pair references.
Non-M1 pair fields use
`not-applicable/row-has-no-isolated-pair` with zero payload. The job-instance
section contains one canonical list of complete occurrence records; I1/I2 use
an explicit known-empty `0:` list. Each section digest hashes the literal
`execution-profile-evidence-section-digest-v1\n` domain plus framed section
name, schema id, and retained exact bytes.

Each bundle is the exact `execution-profile-evidence-bundle-v1\n` header plus
five records: `workload_id:workload-id-v1`, subject role, provenance-section
digest, comparison reference, and nonempty canonical row-reference list whose
items all use that workload id through a `workload-id-v1` first component.
Reference bundles use
`not-applicable/reference-has-no-comparison-baseline`; no optional field is
omitted or represented by an empty digest. `row_digest` and `bundle_digest`
hash their distinct ADR 0010 domain tags plus one frame around the complete
canonical manifest bytes. The claim is stored beside its object and excluded
from the hashed bytes.

The functional key of every row-reference item is exactly
`(workload_id,run_cap,replicate_ordinal)`. The list rejects two items with the
same key even if they name different row digests. For every item, the validator
resolves exactly one retained canonical row, recomputes its digest, parses all
15 fields, and matches workload, cap, and replicate to the item and subject
role to the enclosing bundle.

For a candidate, the validator first resolves
`comparison_reference_bundle_digest` to exactly one retained bundle object. It
parses the exact canonical header and five fields, independently recomputes the
bundle digest and matches the claim, requires `subject_role=reference` and the
same workload as the candidate, and validates the complete nonempty row list's
ordering, functional-key uniqueness, and item-to-canonical-row resolutions.
Zero or multiple retained objects, including multiple objects carrying the
same digest claim, a five-field parse/schema failure, claimed/recomputed
mismatch, wrong role/workload, or any invalid target row list make every
related reference-relative verdict invalid. Only then is the exact target row
the one and only row with the candidate row's functional key; the comparison
bundle digest does not select a row. Each M1 pair instead names its exact row
digest and must resolve a same-role, same-ordinal target at the required
isolated workload and cap 8. Missing, duplicate, or mismatched item, row,
bundle, role, or key evidence is invalid.

The comparison-bundle resolver has this mandatory scenario/oracle matrix:

| Scenario | Oracle |
| --- | --- |
| Exact-one valid | One retained object parses as the exact canonical five-field bundle, all workload-bearing fields/components parse as `workload-id-v1`, its independent rehash equals the claim, role is `reference`, workload matches, its full row list is canonical and functionally unique, and the candidate key selects exactly one valid target row; resolution may proceed to the remaining compatibility checks. |
| Zero object | No retained bundle object resolves from the claim; every related reference-relative verdict is `invalid`. |
| Multiple objects with the same claim | Two or more retained objects resolve from the same digest claim, even if their bytes are equal; the validator does not choose by path, insertion order, or bytes, and every related verdict is `invalid`. |
| Five-field schema failure | The target has a wrong header, field count/order/type/state/reason, malformed frame, missing final LF, or extra byte; it is not a canonical bundle and every related verdict is `invalid`. |
| Independent rehash mismatch | The canonical target bytes recompute to a bundle digest different from the candidate claim; every related verdict is `invalid`. |
| Wrong role | The resolved bundle has `subject_role=candidate`; every related verdict is `invalid`. |
| Wrong workload | The resolved bundle/candidate/item workload differs, has a case/unknown token, or uses a generic `identifier` type frame; canonical validation fails before equality/key lookup and every related verdict is `invalid`. |
| Target key missing | No target row-reference item has the candidate row's functional key; the related verdict is `invalid`. |
| Target key duplicated | More than one target row-reference item has that key, including different row digests; the related verdict is `invalid`. |
| Target row mismatch | The selected item resolves zero or multiple rows, fails row rehash or 15-field parsing, or mismatches item/bundle workload, cap, replicate, or role; the related verdict is `invalid`. |

Address sealing is also normative. First validate immutable external targets;
then freeze retained sections and bundle provenance in dependency order;
then freeze rows; then freeze the enclosing bundle; finally publish claimed
digests beside the objects. Every versioned section/provenance schema must
expose every address-bearing field or derivation input. With edge `X -> Y`
meaning that `X` depends on `Y`'s address, the complete section/provenance/row/
bundle graph and the external comparison/M1 bundle graph must be finite DAGs,
and every target must be sealed before its source. The validator rejects an
opaque or undeclared address dependency, fixed-point construction, post-seal
rewrite, direct or transitive self/enclosing/later-stage dependency, external
cycle, missing retained bytes, unknown/extra/reordered fields, or any claimed/
recomputed mismatch.

A prose summary, an unrecorded rerun of a “known good” build, or current
`BenchmarkResult` output is not a normative reference. The raw bundle must be
sufficient for an independent reader to reproduce every aggregate and verdict.

### Test ownership

Issues #93 through #96 should register lasting deterministic product behavior
when they add workload semantics, exact starts, cancellation, digest,
resource-limit, or settlement invariants. Machine-dependent performance
thresholds and candidate/reference ratios remain in this manual/release
workflow. No issue-specific replay, provenance/result orchestrator, phase-
completion scan, or performance-result file may be registered with CTest or CI
or retained as repository content.

Issue #93 owns the continuous isolated-I1 grid and exact drain/tie/guard
collector behavior. Issue #95 owns the B1 `OutputStore` fixed raw probe-to-
schema mappings,
backend-to-fixed-schema adapters, mount normalizer, performance-configuration
mapping/proof, the single canonical encoder/digests, eligibility and
root-containment evidence, and cap-1/cap-8 plus candidate/reference checks.
Issue #96 reuses the exact manifest bytes for M1, implements the frozen
`C^M1`/`W^M1` pre-boundary protocol, independent producer-local cycles, and
cutoff/carryover/phase-attribution boundary, and enforces its same-ordinal full
B1 pair while keeping the I1-only pair base-only. None may redefine v1 grammar,
fields, or sentinels. Issue #92 defines only this evidence contract;
it adds no current probe, serializer, public API, runner, or runtime result
field.

## CTest Registration

All intended GoogleTest binaries should be registered with CTest. This includes
milestone tests and `test_propagation_contracts`, which may currently be
low-confidence.

Low-confidence tests should still be visible in validation rather than silently
excluded. If a test is not reliable enough to gate development, document that
status explicitly and create follow-up work to upgrade or replace it.

Milestone tests, `test_propagation_contracts`, and the long-lived
`test_region_contracts` behavior suite are registered with CTest so
they are visible, but they remain low-confidence legacy tests until a follow-up
pass rewrites them as narrower regression tests with clearer fixtures and
assertions.

`test_propagation` is different: it is a scriptable REPL/tool target, not a
GoogleTest binary. CMake keeps it buildable for manual scripts and ad hoc
validation, but CTest does not discover or run it. Do not claim that CTest
covers `test_propagation`; run the exact manual command separately when needed.

The provider-dependent default full test suite is registered only when
`BUILD_TESTING`, OpenCV, YAML, graph CLI, the repository OpenCV operation
provider, and repository OpenCV operation plugins are all enabled. It registers
`test_stdlib_image_buffer_processing` and compiles the standard-library
implementation directly even though that producer uses OpenCV. The test
verifies clone independence, stride-safe deterministic bilinear border
behavior, channel conversions, and ROI copying. The default CTest inventory
also includes `DependencyDisabledInstallSmoke`.

When only `PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` is disabled from that
otherwise default test profile, CMake does not create or discover the broad
suite. It keeps the provider-independent `test_kernel_contracts` target
buildable for both dependency-disabled nested builds and registers exactly the
focused optional-provider GoogleTest, the three dedicated disk-cache
diagnostic concurrency cases, and `DependencyDisabledInstallSmoke`.

The default CTest inventory intentionally contains no phase-completion scan,
migration-residue check, stale-term search, Doxygen audit, or issue-specific
orchestration. The daemon help driver, static package-consumer smoke, and graph
CLI allocation-failure driver remain registered because they exercise real
installed/runtime behavior.

For IPC changes, focused local product validation is:

```bash
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol test_ipc_host \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|ExecutionTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientExecutionDefaults|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonExecution|IpcDaemonPolicy|IpcObservationFixtureDaemon|PhotospiderdCapabilityHelp|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

Temporary daemon processes, sockets, graph sessions, package prefixes, and
consumer trees must be absent after these tests. The mode-`0600` persistent
`${socket}.lock` inode is an intentional product synchronization artifact; a
test-owned temporary root removes it with that root, while the real default
runtime location preserves it. CTest output/JUnit and remote CI artifacts are
the evidence; do not create `tests/results` or an issue-specific replay/
provenance helper.

## Known Test Quality Caveat

Some milestone tests and propagation contract tests were written as development
checks rather than polished regression tests. They should be registered so they
are visible, then upgraded later into clearer, higher-confidence tests.

`test_propagation` remains a manual tool target until it is either converted
into a proper GoogleTest binary or replaced with narrower CTest-registered
fixtures.

## GitHub/CI Integration Status

GitHub Actions and the Linux CI container are maintained validation paths.
Pull requests targeting `main` use the base branch's protected workflow through
`pull_request_target`, while pushes to `main` and `CI/**` also run CI. Ordinary
feature branches cannot change `ci/**`, `.github/workflows/**`, or
`Dockerfile.ci`; those inputs require a `CI/**` branch in the base repository.
Only a same-repository `CI/**` pull request is deduplicated in favor of that
branch's push run. A fork with the same branch prefix is rejected before
checkout, and branch spelling alone never authorizes protected-path changes.
Both production guards write `git diff --name-only -z` to a parent-visible
artifact, read exact NUL records into Bash, match complete path values, and use
`%q` for human-readable changed/protected logs. Producer or reader failure is
fail-closed, and a newline inside a valid `ci/**` filename cannot bypass or
forge the protected-path inventory.

Every triggered run keeps a stable `healthcheck` conclusion. The integration
workflow classifies exact event revisions before configuration: changes limited
to `docs/**`, root Markdown, and the documented root text contracts skip all
build, CTest, and integration shards intentionally, while the stable
`integration` gate verifies and reports that route. Any non-documentation path
or uncertain Git state runs full integration. Type changes and uncommon Git
statuses stay in the unfiltered path inventory. Every `CI/**` push also forces
full current-head integration, including a later incremental push that changes
only documentation. The workflows deliberately avoid `paths-ignore`, which
could leave a configured required check pending. Stable gates take the same
repository-identity decision: only a same-repository `CI/**` pull request can
report intentional deduplication; fork or missing identity fails closed.

During the scheduler-to-policy/execution transition, configured CMake target
help is the validation capability boundary. Trusted CI accepts only the
complete legacy marker set (`test_scheduler`,
`test_scheduler_plugin_loader`, and `destroy_count_scheduler_plugin`) or the
complete new marker set (`test_policy_execution`, `test_policy_registry`, and
`test_policy_plugin`), never a partial or mixed set. Build integrity requires
the stable `photospider_kernel` aggregate rather than architecture-specific
implementation targets and still builds the complete tree. Full CTest remains
the authority for ordinary registered tests, while plugin, CLI,
`execution-repeat`, and sanitizer shards select contract-specific assertions.
The generated CLI configuration is mutually exclusive: CI does not pass
removed scheduler keys to a policy/execution revision or introduce a product
compatibility translation.

`healthcheck-published-image` is a container job, and published-image
healthcheck execution and build/test integration jobs run in
`ghcr.io/<owner>/<repo>/photospider-ci:latest`; lightweight routing and result
gates remain on `ubuntu-latest`. Immediately after checkout, the published
container's unique `Trust checked-out workspace` step binds `shell: bash`,
adds only the exact `$GITHUB_WORKSPACE` to the job-persistent global
`safe.directory`, and verifies `HEAD^{commit}` read-only. It neither configures
`safe.directory=*` nor executes a checked-out repository script. This trust
boundary precedes both conditional history fetches and `healthcheck.sh`,
including `main` push and `workflow_dispatch` routes where neither fetch runs,
instead of relying on checkout's temporary HOME-scoped configuration. The
`Fetch pull request base history` and `Fetch CI branch main history` steps each
also bind `shell: bash`, making their `set -Eeuo pipefail` prologues valid
without relying on the container default shell. If a change modifies an image
input, the workflow builds `photospider-ci:local` and runs the same repository
scripts in that image so validation does not race image publication. For pull
requests, the published-image and local-image healthcheck jobs each fetch the
target branch from the base-repository URL, verify `CI_BASE_SHA` as the event's
exact base commit inside their own job, and supply that exact SHA as `CI_BASE_REF`
independently of the fork checkout's `origin`. For every `CI/**` push, each job
instead fetches and verifies `origin/main`, then supplies it as `CI_BASE_REF`
so the static scope remains cumulative from the `main` merge base across
successive pushes. A later documentation-only push therefore cannot hide an
earlier unformatted C++ commit. An ordinary `main` push retains
`github.event.before` as its incremental `CI_BASE_REF`. Published-image
verification precedes `healthcheck.sh`; local-image verification precedes the
head Dockerfile build and mounted-workspace execution. Required fetch or parse
failure therefore stops before the script's fallback base selection.
`Dockerfile.ci` installs the C++ toolchain, CMake, OpenCV, yaml-cpp, GTest,
nlohmann-json, clang-format, Python, and cpplint required by those scripts.
The image detector uses no Git status filter. The healthcheck static-scope
inventory instead uses `--diff-filter=d` to omit deleted formatter/linter
inputs while retaining type changes and uncommon non-deletion statuses. Both
use NUL-delimited Git output and a parent-visible temporary file. A failing
`git diff` therefore terminates image detection or healthcheck static-scope
detection without emitting a false negative route.

The maintained entry points are:

- `ci/scripts/healthcheck.sh` for fail-closed changed-path inventory, diff,
  format, cpplint, the build-smoke inventory regression, runtime-capability
  regression, and both durable routing shell regressions.
- `ci/scripts/change_classification.sh` and
  `ci/scripts/change_classification_test.sh` for fail-closed documentation-only
  routing and its durable event/path regression matrix.
- `ci/scripts/ci_routing_test.sh` for exact canonical locking of both
  `protected-ci-paths.if` expressions; execution of the real stable-gate,
  fork-rejection, and protected-path blocks; job/step-scoped locking of both
  published-image history-fetch steps' own `shell: bash` metadata;
  job/step-scoped locking of the unique published-image workspace-trust step,
  its exact non-wildcard global `safe.directory`, read-only HEAD verification,
  and checkout-before-trust-before-fetch/healthcheck order;
  published/local job-scoped pull-request exact-base and `CI/**`
  cumulative-main ordering; exact three-way `CI_BASE_REF` source routing;
  allow-empty configuration preflight, strict post-build matrix job output,
  empty-output-safe `fromJSON`, full-CTest/fallback routing,
  architecture-neutral `execution-repeat` routing; newline-path artifacts; and
  detector/reader/producer failure propagation. It
  executes the production trust block with an isolated HOME/repository and
  requires exactly that repository in the resulting global trust list. It also
  executes both production main-fetch blocks, while an isolated Git history
  proves cumulative main scope retains earlier C++ and event-before scope sees
  only the later docs increment. The local source/shell lock does not emulate
  GitHub's expression evaluator, cross-UID dubious ownership, or the hosted
  container runner.
- `ci/scripts/build_smoke_inventory.py` and its focused regression for strict
  CTest JSON parsing, deterministic strict or explicit allow-empty matrix
  generation, duplicate label values, safe artifact keys, NUL-delimited names,
  exact index-based execution, absent/disabled/commandless selections stopping
  before a second subprocess, and a real configure-placeholder-to-post-build
  discovery fixture.
- `ci/scripts/runtime_capability_test.sh` for exact Make/Ninja target parsing,
  complete legacy and policy/execution profiles, partial/mixed/absent
  fail-closed behavior, required-target validation, and mutually exclusive CLI
  configuration.
- `ci/scripts/integration_plan.sh` for allow-empty exact-label configuration
  preflight without authoritative matrix output.
- `ci/scripts/build_integrity.sh` for the default producer profile, including
  runtime-contract detection, architecture-neutral required-target/full builds,
  strict post-build labelled CTest validation, and the authoritative matrix job
  output.
- `ci/scripts/ctest_full.sh` for the main CTest suite with the exact
  `build-smoke` label excluded.
- `ci/scripts/integration_suite.sh` for sequential integration behavior checks,
  running every post-build-discovered build smoke alongside full CTest, CLI,
  propagation, plugin, and capability-selected execution coverage.

CI source inventories and exclusion lists must describe maintained tests and
current source paths. Migration-only harness names must not be retained as
permanent exclusions or treated as product behavior. GitHub job status and
downloadable artifacts report remote integration behavior. The complete
workflow and artifact download boundary are documented in
`docs/CI/github-actions.md`.

Architecture evolution goals are intentionally not maintained in this testing
document. They are recorded in `docs/roadmap/Kernel-Evolution.md`, while each
implementation change defines its own proportional validation and durable
regression coverage.
