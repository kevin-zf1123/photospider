# GitHub Actions CI

## Workflows

- `.github/workflows/ci-healthcheck.yml`: static healthcheck on pull requests targeting `main` through `pull_request_target`, pushes to `main` and `CI/**`, and manual dispatch.
- `.github/workflows/ci-integration.yml`: dynamic integration planning, parallel build-integrity profiles, separately sharded full CTest and build smoke tests, scripted `graph_cli`, scripted propagation, plugin loading, and scheduler repeat checks.
- `.github/workflows/ci-sanitizer.yml`: manual ASan or TSan focused checks.
- `.github/workflows/build-ci-image.yml`: GHCR image publish for `ghcr.io/<owner>/<repo>/photospider-ci` on image-input pushes and manual dispatch.

## Branch and Workflow Guards

Push-triggered CI runs only on `main` and branches whose names start with `CI/`. This prevents ordinary feature branches from running workflow files changed on that branch.

Pull requests targeting `main` use `pull_request_target`, which uses the workflow definition from the base branch while checking out the pull request head commit for tests. `CI/**` branches validate branch-modified workflows through the push trigger instead of a second pull request run, avoiding duplicate local-image integration runs on the same commit.

The first healthcheck and integration job protects CI workflow inputs before any repository script or local CI image build runs. For non-`CI/**` branches it compares `origin/main...HEAD` and fails if the diff changes any of:

- `ci/**`
- `.github/workflows/**`
- `Dockerfile.ci`

CI workflow changes must therefore be developed on a `CI/**` branch. The guard also catches non-`CI/**` pull requests that target `main`, so workflow-related files cannot be merged through an ordinary feature branch.

## Runtime

`Dockerfile.ci` defines the GitHub Linux test environment. Normal healthcheck and integration jobs run inside the published `ghcr.io/<owner>/<repo>/photospider-ci:latest` image.

When a pull request or push changes the CI image inputs (`Dockerfile.ci`, `.dockerignore`, or `.github/workflows/build-ci-image.yml`), the healthcheck and integration workflows build `photospider-ci:local` inside the workflow and run the same scripts there. This avoids racing another workflow that may still be publishing the new `latest` image.

The image includes CMake, a C++ toolchain, OpenCV, yaml-cpp, CURL, OpenSSL,
GTest, nlohmann-json, Python, cpplint, and clang-format. The formatter is
installed from the PyPI wheel at version 21.1.5 so CI and developer formatting
do not drift merely because different formatter releases choose different line
breaks. The maintainer machine currently uses 21.1.3; its formatted output was
verified byte-for-byte equal to 21.1.5 for the changed C++ inventory covered by
this alignment. Developer environments should adopt 21.1.5 going forward.

clang-format is the only tool version aligned by this change. This is not a
version-detection gate and adds no dedicated CI job or new Ubuntu/CMake version
lock; the existing Ubuntu base and apt-provided CMake setup remain unchanged.

## Integration Test Sharding

For the normal published-image path, `integration-plan` configures the checked-out commit with testing enabled and uses `ctest -N` to discover the exact `StaticProductConsumerSmoke` and `IpcDisabledInstallSmoke` test names. It enables each smoke and its required build only when that test exists. The current `main` tree therefore schedules only `build-integrity-default`, while a refactor commit that introduces the IPC-disabled install smoke also schedules `build-integrity-ipc-disabled`. This is capability discovery from the tested commit, not a hard-coded branch-name check, so the workflow definition based on `main` can also test the refactor pull request head. If a smoke runner exists without its exact CTest registration, planning fails instead of silently dropping coverage.

Each profile has an independent build job that configures and compiles only that profile, then uploads a separate reusable build artifact: `ci-build-default` or `ci-build-ipc-disabled`. The IPC-disabled profile configures the producer with `PHOTOSPIDER_BUILD_IPC=OFF`. Default consumers depend only on `build-integrity-default`, while the IPC-disabled smoke depends only on `build-integrity-ipc-disabled`; a failure in one profile therefore does not suppress tests for the other profile.

The test jobs reuse those prebuilt producers rather than recompiling every configuration on one runner:

- `full-ctest`, `scripted-cli`, `propagation-script`, `plugin-load`, and `scheduler-repeat` download only `ci-build-default`.
- `full-ctest` excludes `SplitComputeServiceRuntimeTrace`, `StaticProductConsumerSmoke`, and `IpcDisabledInstallSmoke`. The split trace remains outside primary CI, and the two long build smoke tests run in their own jobs.
- `static-product-consumer-smoke` downloads `ci-build-default` and runs only `StaticProductConsumerSmoke`.
- `ipc-disabled-install-smoke` downloads the precompiled `ci-build-ipc-disabled` producer and runs only `IpcDisabledInstallSmoke`.

If CI image inputs change, the workflow cannot use the previously published image and instead runs `local-image-integration` on one Docker-capable runner. After building `photospider-ci:local`, `integration_suite.sh` performs the same dynamic plan, builds each discovered profile, and runs the same full-CTest, build-smoke, CLI, propagation, plugin, and scheduler shards sequentially with their corresponding build. This fallback preserves test selection and producer configuration while accepting that a single local-image runner cannot fan out into artifact-consuming jobs.

CMake 3.16 is the project's compatibility floor, not a workflow-pinned version
for every pull request. Build logic guards policies introduced after that floor,
while current integration exercises the fresh static package consumer on the
supported CI toolchain. A targeted native old-version
producer/install/consumer run is added only when a compatibility-sensitive
change or release check needs it; the regular integration workflow does not
lock Ubuntu or CMake to a dedicated minimum-version job.

## Scripts

CI and CTest execute only long-lived software behavior, compile, package-
consumer, performance, concurrency, stability, error-handling, and runtime-
boundary checks. Migration-residue scans, phase-completion checks, stale-term
searches, Doxygen/source-quality audits, issue replay, and evidence/provenance
orchestration are excluded. Issue-specific replay, provenance, helper, and
output artifacts do not enter the primary repository and are not retained as
long-lived personal-overlay content. Explicitly documented general-purpose
manual developer tools are separate; a clean primary checkout never imports
personal development content.

- `ci/scripts/healthcheck.sh`: runs `git diff --check`, `clang-format --dry-run --Werror`, and `cpplint` on changed C++ files.
- `ci/scripts/ci_image_changed.sh`: detects whether the current diff changes CI image inputs.
- `ci/scripts/integration_plan.sh`: configures a small testing-enabled planning tree, discovers the two exact build-smoke test names with `ctest -N`, validates registration against the runner files, and emits smoke/build capability flags.
- `ci/scripts/integration_suite.sh`: applies the dynamic plan and runs the resulting integration shards sequentially for the local-image fallback path.
- `ci/scripts/build_integrity.sh`: builds the profile selected by `CI_BUILD_PROFILE`. `default` builds the required targets and the complete tree before CTest discovery; `ipc-disabled` sets `BUILD_TESTING=OFF` and `PHOTOSPIDER_BUILD_IPC=OFF`, validates the cache, and builds only the `photospider` producer target.
- `ci/scripts/ctest_full.sh`: reuses or builds the default producer and runs CTest, excluding the two separately sharded build smoke tests by default. Its protected script also retains a no-op exclusion for the removed `SplitComputeServiceRuntimeTrace`; a follow-up `CI/**` branch from main must remove that token after the source-layout change lands.
- `ci/scripts/build_smoke_test.sh`: runs one separately selected build smoke test from a reusable producer; set `SMOKE_TEST` to `static-product-consumer` or `ipc-disabled-install`.
- `ci/scripts/graph_cli_script_test.sh`: runs positive and negative scripted REPL checks.
- `ci/scripts/propagation_script_test.sh`: builds `test_propagation` and runs `tiles all` on linear and complex propagation graphs.
- `ci/scripts/plugin_load_test.sh`: checks plugin artifacts, plugin manager tests, scheduler plugin loader tests, and CLI scheduler plugin listing.
- `ci/scripts/scheduler_repeat_test.sh`: repeats key scheduler tests.
- `ci/scripts/sanitizer_test.sh`: runs focused ASan or TSan tests from an isolated build directory.

## Local Commands

```bash
CI_ARTIFACT_DIR=CI-results/healthcheck bash ci/scripts/healthcheck.sh
GITHUB_OUTPUT=/tmp/photospider-integration-plan.out \
  CI_ARTIFACT_DIR=CI-results/integration-plan \
  bash ci/scripts/integration_plan.sh
BUILD_DIR="$PWD/build/ci-default" CI_BUILD_PROFILE=default \
  CI_ARTIFACT_DIR=CI-results/build-integrity-default \
  bash ci/scripts/build_integrity.sh
BUILD_DIR="$PWD/build/ci-ipc-disabled" CI_BUILD_PROFILE=ipc-disabled \
  CI_ARTIFACT_DIR=CI-results/build-integrity-ipc-disabled \
  bash ci/scripts/build_integrity.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR=CI-results/ctest-full bash ci/scripts/ctest_full.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  SMOKE_TEST=static-product-consumer \
  CI_ARTIFACT_DIR=CI-results/static-product-consumer-smoke \
  bash ci/scripts/build_smoke_test.sh
BUILD_DIR="$PWD/build/ci-ipc-disabled" CI_REUSE_BUILD=ON \
  SMOKE_TEST=ipc-disabled-install \
  CI_ARTIFACT_DIR=CI-results/ipc-disabled-install-smoke \
  bash ci/scripts/build_smoke_test.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR=CI-results/graph-cli \
  bash ci/scripts/graph_cli_script_test.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR=CI-results/propagation \
  bash ci/scripts/propagation_script_test.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR=CI-results/plugin-load \
  bash ci/scripts/plugin_load_test.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR=CI-results/scheduler-repeat \
  bash ci/scripts/scheduler_repeat_test.sh
SANITIZER=asan CI_ARTIFACT_DIR=CI-results/sanitizer-asan bash ci/scripts/sanitizer_test.sh
```

The `ipc-disabled` profile and either smoke command are valid only when the checked-out commit's integration plan reports them. To reproduce the entire dynamically selected sequence, use:

```bash
CI_ARTIFACT_ROOT=CI-results bash ci/scripts/integration_suite.sh
```

Docker reproduction:

```bash
docker build -t photospider-ci:local -f Dockerfile.ci .
docker run --rm -v "$PWD:/workspace" -w /workspace photospider-ci:local \
  bash ci/scripts/build_integrity.sh
```

Optional local mirror build:

```bash
docker build -t photospider-ci:local -f Dockerfile.ci \
  --build-arg APT_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ \
  --build-arg PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple .
```

Use `ubuntu-ports` for local arm64 builds. Use `http://mirrors.tuna.tsinghua.edu.cn/ubuntu/` for amd64.

The local Docker commands above reproduce the maintained current-toolchain CI
paths. They are not a claim that CMake 3.16 itself ran. If targeted old-version
evidence is needed and no natively compatible executable exists locally, record
that limitation rather than using architecture emulation to manufacture a
minimum-version PASS.

## Local Artifact Download

Use the personal-overlay script to download GitHub Actions artifacts:

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

It writes to `CI-results/`, which is personal-overlay content and must not be committed to the primary GitHub repository.
