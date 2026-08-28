# GitHub Actions CI

Photospider intentionally keeps two GitHub Actions workflows:

- `.github/workflows/ci.yml` handles ordinary pushes with one healthcheck,
  one reusable build, eight independent build-smoke runners, and three parallel
  CTest label shards.
- `.github/workflows/build-ci-image.yml` publishes the Linux CI image when
  `Dockerfile.ci` changes on `main`, or when a maintainer dispatches it
  manually.

There is no separate pull-request-target, sanitizer, scheduler-log, routing,
evidence, provenance, or aggregation workflow. The repository is maintained
as a personal development project, so CI is sized to the useful build and
test signal instead of reproducing enterprise approval or self-authorization
machinery.

## Daily CI flow

`.github/workflows/ci.yml` runs on pushes to `main` and `CI/**`. Every job uses
`ghcr.io/<owner>/<repo>/photospider-ci:latest`, checks out submodules
recursively, and receives only read access to repository contents and packages.
Jobs form one dependency chain with parallel test leaves:

```text
healthcheck -> build -> build-smoke (8 independent matrix jobs)
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

The healthcheck then deliberately performs only three inexpensive checks:

1. `git diff --check` for the pushed commit.
2. A CMake configure with testing enabled and ASan, TSan, and fuzzers disabled.
3. A build of `public_header_self_containment`.

It does not classify changed paths, infer docs-only runs, compare protected
paths, inspect another ref, build the full product, or decide whether later
jobs should exist.

### One cached build

The build job restores `build/ci` with `actions/cache/restore@v6`. Its cache key
contains:

- an explicit cache epoch;
- the runner operating system and build type;
- one hash over `Dockerfile.ci`, every `CMakeLists.txt`, and `cmake/**`;
- the exact Git commit SHA.

The restore prefix stops before the SHA, so a push may reuse the latest
compatible earlier build tree. CMake still configures the restored tree on
every run. Ninja is then invoked exactly once for the primary build. The job
later uses `actions/cache/save@v6` to save the resulting complete tree under the
current SHA key when that exact immutable entry does not already exist. The tree
includes object files and Ninja dependency state. The workflow also prints the
restore action's `cache-hit` output, so a rerun can distinguish an exact-key hit
from a prefix restore or complete miss without changing cache behavior.

Immediately after the build, the job packages the lightweight CTest runtime;
after the explicit cache save, it uploads that archive once. A restored cache
may already contain the fixed
`tests/image_artifact_codec_dependency_disabled` and
`tests/optional_opencv_provider_disabled` work roots from an earlier run, so
the packager excludes those exact transient roots and all descendants without
deleting them from the complete build tree. The build job does not run a build
smoke; saving the current-SHA cache and uploading the single runtime archive
complete the producer boundary.

### Independent build-smoke runners

The workflow has one fixed matrix entry for each of the eight build smokes in
the default CI configuration:

- `DependencyDisabledInstallSmoke`;
- `OpenExrDeepProviderOptionOffSmoke`;
- `StaticProductConsumerSmoke`;
- `PhotospiderdInstallLayoutSmoke`;
- `IpcDisabledInstallSmoke`;
- `ImageArtifactCodecDependencyDisabledBuild`;
- `OpenCvOperationProviderDisabledBuild`;
- `PublicHeaderSelfContainment`.

The matrix uses `fail-fast: false`, so every entry receives its own runner and
container after the producer succeeds. Each runner checks out the same
`github.sha`, restores only the producer's exact current-SHA cache key with
`actions/cache/restore@v6`, and fails on a cache miss instead of falling back to
an earlier commit. It then reconfigures the restored tree with the same source
and binary locations under `$GITHUB_WORKSPACE` before invoking CTest once. The
invocation combines an anchored exact `--tests-regex`, the exact
`--label-regex '^build-smoke$'`, and `--no-tests=error`; a renamed, absent, or
mislabelled entry therefore cannot pass as an empty selection.

These eight jobs run in parallel with the `unit`, `integration`, and
`verification` label jobs. Their local nested build/install outputs are not
saved back to the immutable producer cache and never enter `ctest-runtime`.
Each job writes its report beneath
`CI-results/build-smoke/<matrix-artifact>.junit.xml`; an `always()` step uploads
the unique `ctest-junit-build-smoke-<matrix-artifact>` artifact for seven days
and warns if the report is unavailable. Adding or renaming a durable build
smoke requires coordinated updates to its CTest registration, the fixed
workflow matrix, and this inventory.

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
nested-smoke roots named above. Incremental build inputs remain only in the
complete build-tree cache; the rest of `tests/` is retained as part of the CTest
runtime. The archive is uploaded once with artifact recompression disabled and
downloaded only by the three primary-label test jobs. The eight build-smoke
runners restore the build-tree cache instead and do not create or download
runtime packages. After validating the closure, the packager prints the
physical archive byte count and tar entry count. These diagnostics are
observations for cache and artifact-size experiments; they do not relax the
required roots or forbidden entry checks.

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

`Dockerfile.ci` defines the Linux toolchain and includes Ninja plus the project
build dependencies. `.github/workflows/build-ci-image.yml` runs only for a
`Dockerfile.ci` change on `main` or manual dispatch. It publishes `latest`, a
commit tag, and an optional manual tag to
`ghcr.io/<owner>/<repo>/photospider-ci`.

The ordinary CI workflow consumes that published image. A Dockerfile change
must land and publish successfully before later pushes can rely on the new
toolchain; ordinary CI does not build or compare the image itself.

Both workflows use the maintained Node 24 action majors: `actions/checkout@v7`,
`actions/cache/restore@v6`, `actions/cache/save@v6`, `actions/upload-artifact@v7`,
`actions/download-artifact@v8`, `docker/login-action@v4`,
`docker/metadata-action@v6`, and `docker/build-push-action@v7`. These majors
require Actions Runner 2.327.1 or later; the workflows use GitHub-hosted
runners rather than a repository-owned self-hosted runner.

## Local checks

Use native builds for local validation; do not emulate the Linux image on
macOS merely to mirror GitHub Actions:

```bash
cmake -S . -B build/ci -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DUSE_ASAN=OFF \
  -DUSE_TSAN=OFF \
  -DPHOTOSPIDER_BUILD_FUZZERS=OFF
cmake --build build/ci --parallel 2
bash ci/scripts/package_ctest_runtime.sh \
  build/ci CI-results/ctest-runtime.tar.gz
tar -tzf CI-results/ctest-runtime.tar.gz
ctest --test-dir build/ci --output-on-failure --parallel 2
```

Focused local runs use the same primary labels as CI:

```bash
ctest --test-dir build/ci --output-on-failure -L '^unit$'
ctest --test-dir build/ci --output-on-failure -L '^integration$'
ctest --test-dir build/ci --output-on-failure -L '^verification$'
ctest --test-dir build/ci --output-on-failure -L '^build-smoke$'
```

The maintained `graph_cli_script_test.sh`, `propagation_script_test.sh`,
`plugin_load_test.sh`, `execution_repeat_test.sh`, and `sanitizer_test.sh`
remain available for explicit local product-boundary or sanitizer checks. They
are not extra GitHub workflows and are not part of the ordinary push path.
