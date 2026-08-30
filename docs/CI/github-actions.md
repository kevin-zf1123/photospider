# GitHub Actions CI

Photospider intentionally keeps two GitHub Actions workflows:

- `.github/workflows/ci.yml` handles pull requests and ordinary maintained
  pushes with one preset healthcheck, one ccache-backed legacy-full producer
  build, six independent build-smoke runners, and three parallel CTest label
  shards.
- `.github/workflows/build-ci-image.yml` publishes the Linux CI image when
  `Dockerfile.ci` changes on `main`, or when a maintainer dispatches it
  manually.

There is no separate pull-request-target, sanitizer, scheduler-log, routing,
evidence, provenance, or aggregation workflow. The repository is maintained
as a personal development project, so CI is sized to the useful build and
test signal instead of reproducing enterprise approval or self-authorization
machinery.

## Daily CI flow

`.github/workflows/ci.yml` runs on pull requests and pushes to `main` and
`CI/**`. Every job uses
`ghcr.io/<owner>/<repo>/photospider-ci:latest`, checks out submodules
recursively, and receives only read access to repository contents and packages.
Jobs form one dependency chain with parallel test leaves:

```text
healthcheck -> build -> build-smoke (6 independent matrix jobs)
                     -> unit
                     -> integration
                     -> verification
```

### Healthcheck

Before its project checks, the healthcheck adds only `$GITHUB_WORKSPACE` to
the job container user's global Git `safe.directory` list and verifies that
the checked-out `HEAD` is a commit. This is a container Git availability
setting for the checkout's ownership boundary, not an authorization,
protected-path, or provenance proof; it never uses a wildcard.

The healthcheck deliberately performs only these inexpensive checks:

1. `git diff --check` for the exact checked-out commit.
2. An explicit `ccache` executable and version check, so an unpublished or
   stale CI image fails before the producer.
3. Configure and build the complete dependency-neutral `kernel-dev` profile,
   then run its maintained tests.
4. Configure and build `op-dev`.
5. Configure `legacy-full` and build `public_header_self_containment`.
6. Configure `legacy-full-portable` and build the same public-header target.

These paths are maintained in `CMakePresets.json`. `kernel-dev` and `op-dev`
default-disable Job, CLI, optional providers/plugins, OpenEXR, and fuzzers;
`legacy-full` explicitly enables the historical Job/product closure on
Darwin/Linux. `legacy-full-portable` validates the complete portable closure
with Job disabled and remains usable on hosts where that POSIX product is
unsupported. The
[post-split development contract](../development/Post-Split-Development-Contract.md)
owns the complete preset table. The preset frontend requires CMake 3.21;
direct configuration retains the project-wide CMake 3.16 minimum.

It does not classify changed paths, infer docs-only runs, compare protected
paths, inspect another ref, build the full product, or decide whether later
jobs should exist.

### One ccache-backed build

The producer never restores a previous `build/ci` tree. It starts with a fresh
binary directory and restores only `.ccache` with
`actions/cache/restore@v6`. The compiler-cache key contains an explicit epoch,
runner operating system and architecture, build type, a `Dockerfile.ci` image
recipe hash, and the workflow `run_id` and `run_attempt`. Its only restore
prefix ends before the run identity, so the producer may use the newest cache
from an earlier compatible run. The workflow reports both `cache-hit` and the
matched key: a fallback is useful even though only an exact current-run key
would produce `cache-hit == true`.

The cache configuration sets `CCACHE_DIR` inside the workspace, uses
`CCACHE_COMPILERCHECK=content` to reject entries from a different compiler,
and limits the local cache to 2 GiB. CI intentionally leaves
`CCACHE_BASEDIR` unset and sets `CCACHE_NOHASHDIR=true`. The producer and smoke
jobs check out the same source at the same absolute `$GITHUB_WORKSPACE` path,
so equivalent compiler commands retain matching absolute source arguments
while directory hashing no longer distinguishes outer and deeper nested
working directories. Setting `CCACHE_BASEDIR` here would instead rewrite paths
relative to each compiler process's working directory; deeper nested builds
have a different working directory, so their rewritten arguments would differ
and defeat those cache hits. Disabling directory hashing is a deliberate
CI-only tradeoff: `RelWithDebInfo` objects returned from ccache may retain the
producer's working directory in DWARF. Cached objects are therefore never
published or treated as release/debug deliverables. Runtime behavior remains
under test, and every cache miss compiles normally.

After restore, the producer zeroes ccache statistics, configures C and C++
through explicit `ccache` CMake launchers with the retained single-tenant Job
explicitly `ON`, invokes Ninja exactly once, and
prints the resulting hit/miss statistics. It saves `.ccache` under the new
run-and-attempt key for the next workflow, then creates an uncompressed tar
containing the hidden `.ccache` directory and uploads that single file once as
the one-day `ccache-handoff` artifact. The tar preserves the cache directory's
internal structure and modes across the artifact ZIP layer. The Actions Cache
is only a cross-run optimization; the artifact is the required same-run
producer-to-smoke transport. A missing Actions Cache entry means a valid cold
producer build, not a correctness failure.

The key deliberately hashes only `Dockerfile.ci`, which is stable before and
after configuration. A broad workspace `hashFiles('**/CMakeLists.txt')` key was
removed after a baseline run showed that generated dependency CMake files
changed the hash between restore and save, making every downstream exact
restore miss. No complete `build/ci` tree is now saved to Actions Cache or
uploaded as an artifact. After the fresh build, the producer separately
packages and uploads the lightweight CTest runtime for the three primary-label
jobs. The packager excludes compiler-cache content and the two fixed transient
roots `tests/image_artifact_codec_dependency_disabled` and
`tests/optional_opencv_provider_disabled` as object-free archive invariants.

### Independent build-smoke runners

The workflow has one fixed matrix entry for each of the six build smokes in
the default CI configuration:

- `DependencyDisabledInstallSmoke`;
- `OpenExrDeepProviderOptionOffSmoke`;
- `StaticProductConsumerSmoke`;
- `ImageArtifactCodecDependencyDisabledBuild`;
- `OpenCvOperationProviderDisabledBuild`;
- `PublicHeaderSelfContainment`.

The matrix uses `fail-fast: false`, so every entry receives its own runner and
container after the producer succeeds. Each runner checks out the same
`github.sha`, downloads `ccache-handoff` into a fixed artifact directory, and
extracts its tar at `$GITHUB_WORKSPACE` to recreate `.ccache`. It verifies the
cache directory and ccache configuration, zeroes local statistics, and keeps
the restored cache read-only, so one consumer cannot alter the producer
snapshot used by another. Artifact delivery is required, but an individual
compiler-cache lookup may miss and compile normally.

All six runners then execute the same Job-enabled legacy outer
`cmake --fresh -S "$GITHUB_WORKSPACE" -B "$GITHUB_WORKSPACE/build/ci" -G Ninja`
configuration with the producer's build type, test options, and explicit C and
C++ ccache launchers. They query CTest's JSON object model and require the exact
six-entry `build-smoke` inventory before selecting their own entry. The test
itself may build the outer tree or create deeper build/install trees; both use
the read-only producer cache where compilation keys match. Every runner prints
its local ccache statistics after CTest.

Smoke jobs never download `ctest-runtime` and never receive a full producer
tree. The CTest invocation combines an anchored exact `--tests-regex`, the exact
`--label-regex '^build-smoke$'`, and `--no-tests=error`; a renamed, absent, or
mislabelled entry therefore cannot pass as an empty selection.

These six jobs run in parallel with the `unit`, `integration`, and
`verification` label jobs. Their local outer and nested build/install outputs
are not saved back to the read-only handoff or the cross-run Actions Cache and
never enter `ctest-runtime`.
Each job writes its report beneath
`CI-results/build-smoke/<matrix-artifact>.junit.xml`; an `always()` step uploads
the unique `ctest-junit-build-smoke-<matrix-artifact>` artifact for seven days
and warns if the report is unavailable. Adding or renaming a durable build
smoke requires coordinated updates to its CTest registration, the fixed
workflow matrix, and this inventory.

Daemon package, IPC protocol, installed layout/RPATH, and interoperability
tests run in the external
[photospider-daemon](https://github.com/kevin-zf1123/photospider-daemon)
repository. This kernel workflow neither configures daemon ownership nor
duplicates those tests. A daemon downstream gate is requested only for an
installed API/package break or an explicit release check, not for every kernel
pull request.

### Lightweight CTest runtime

`ci/scripts/package_ctest_runtime.sh` creates one physical
`ctest-runtime.tar.gz` from the completely built `build/ci` producer tree. The
archive retains the runtime closure needed by CTest:

- static and dynamic libraries;
- operation, policy, and scheduler plugins;
- test and product executables;
- `CTestTestfile.cmake` and generated GoogleTest inventories;
- `CMakeCache.txt`, generated package configuration, and other runtime data.

The archive excludes every `.o` and `.obj`, every `CMakeFiles` tree,
`.ninja_deps`, `.ninja_log`, prior `Testing` output, and the two exact transient
nested-smoke roots named above. Incremental compiler results live only in
ccache, while the rest of `tests/` is retained as part of the CTest runtime.
The archive is uploaded once with artifact recompression disabled and is
downloaded only by the three primary-label jobs. Build-smoke runners configure
their own outer trees and do not download it. No runner creates a second
runtime package. After validating the closure, the packager prints the physical
archive byte count and tar entry count. These diagnostics and ccache's own
hit/miss statistics are observations for cache and artifact-size experiments;
they do not relax the required roots or forbidden entry checks.

Each test job restores the archive at `build/ci`, the same path used by the
producer, then runs one exact primary label. There are no duplicate runtime
packages and no full build-tree artifact. After each label invocation, an
`always()` step separately attempts to upload
`CI-results/ctest/<label>.junit.xml` as the unique
`ctest-junit-<label>` artifact. Available reports are retained for seven days,
and a missing report warns instead of failing the job.

## CTest labels and parallelism

CMake owns test selection. Every test in the default push path has one primary
label:

- `unit`: maintained GoogleTests whose source role is `tests/unit/`;
- `integration`: maintained integration GoogleTests and direct runtime/CLI
  checks;
- `verification`: deterministic safety harnesses suitable for ordinary CI;
- `build-smoke`: nested build/install/package checks, each run by one isolated
  matrix job.

Tests may keep orthogonal labels such as `execution`, `security`,
`kernel-concurrency`, or `value-runtime`. The repository discovery wrapper
parses and validates each caller property pair, de-duplicates the source-role
primary label with those orthogonal labels, and passes one scalar primary
`LABELS` property to `gtest_discover_tests`. Because the upstream module cannot
transport a list-valued property, a generated `TEST_INCLUDE_FILES` script uses
each `TEST_LIST` after discovery to assign the complete merged list and every
caller test property once; the repository does not depend on repeated-property
merging or on the module's list-value transport.
Unknown discovery arguments, unknown properties, odd property lists, and
duplicate non-label properties fail during configuration. Tests that share a
mutable harness or cannot overlap declare
`RESOURCE_LOCK` or `RUN_SERIAL`, and bounded tests declare `TIMEOUT`, in CMake.
The three runtime shards therefore select only
`--label-regex '^<primary-label>$'`. Each fixed build-smoke entry additionally
uses its short anchored exact-name regex together with the `build-smoke` label;
the workflow does not encode a combined long inclusion or exclusion regex.

Heavy benchmarks, fuzz targets, and sanitizer builds are opt-in developer
tools. Ordinary pushes configure `USE_ASAN=OFF`, `USE_TSAN=OFF`, and
`PHOTOSPIDER_BUILD_FUZZERS=OFF`; no default job runs them.

## CI image workflow

`Dockerfile.ci` defines the Linux toolchain and includes ccache, Ninja, and the
project build dependencies. `.github/workflows/build-ci-image.yml` runs only
for a `Dockerfile.ci` change on `main` or manual dispatch. It publishes
`latest`, a commit tag, and an optional manual tag to
`ghcr.io/<owner>/<repo>/photospider-ci`.

The ordinary CI workflow consumes that published image and now checks that
`ccache` exists before configuration. A Dockerfile change must publish
successfully before ordinary CI can rely on the new toolchain. For a transition
such as the first ccache-enabled image, dispatch the image workflow for the ref
containing the Dockerfile change, wait for `latest` to publish, and only then
run or rerun the ordinary CI workflow. The automatic `main` image build does
not establish ordering ahead of a concurrently triggered CI run; ordinary CI
does not build or compare the image itself.

The first ordinary workflow after that image publication is expected to miss
the cross-run Actions Cache and compile the producer cold. It still uploads the
populated same-run ccache tar, so each smoke can obtain hits for compatible
outer or nested compilation and simply compiles any remaining misses. Later
workflows can restore the newest compatible producer cache and are expected to
start warmer; hit rate is diagnostic, never a correctness gate.

Both workflows use the maintained Node 24 action majors: `actions/checkout@v7`,
`actions/cache/restore@v6`, `actions/cache/save@v6`, `actions/upload-artifact@v7`,
`actions/download-artifact@v8`, `docker/login-action@v4`,
`docker/metadata-action@v6`, and `docker/build-push-action@v7`. These majors
require Actions Runner 2.327.1 or later; the workflows use GitHub-hosted
runners rather than a repository-owned self-hosted runner.

## Local checks

Use native builds for local validation; do not emulate the Linux image on
macOS merely to mirror GitHub Actions. Configure the three maintained presets
before the final legacy-full pass:

```bash
cmake --preset kernel-dev
cmake --build --preset kernel-dev --parallel 2
ctest --preset kernel-dev --output-on-failure --parallel 2
cmake --preset op-dev
cmake --build --preset op-dev --parallel 2
cmake --preset legacy-full
cmake --build --preset legacy-full --parallel 2
cmake --preset legacy-full-portable
cmake --build --preset legacy-full-portable \
  --target public_header_self_containment --parallel 2
bash ci/scripts/package_ctest_runtime.sh \
  build/legacy-full CI-results/ctest-runtime.tar.gz
tar -tzf CI-results/ctest-runtime.tar.gz
ctest --preset legacy-full --output-on-failure --parallel 2
```

Focused local runs use the same primary labels as CI:

```bash
ctest --test-dir build/legacy-full --output-on-failure -L '^unit$'
ctest --test-dir build/legacy-full --output-on-failure -L '^integration$'
ctest --test-dir build/legacy-full --output-on-failure -L '^verification$'
ctest --test-dir build/legacy-full --output-on-failure -L '^build-smoke$'
```

The maintained `graph_cli_script_test.sh`, `propagation_script_test.sh`,
`plugin_load_test.sh`, `execution_repeat_test.sh`, and `sanitizer_test.sh`
remain available for explicit local product-boundary or sanitizer checks. They
are not extra GitHub workflows and are not part of the ordinary push path.
