# GitHub Actions CI

## Workflows

- `.github/workflows/ci-healthcheck.yml`: static healthcheck on pull requests targeting `main` through `pull_request_target`, pushes to `main` and `CI/**`, and manual dispatch.
- `.github/workflows/ci-integration.yml`: build integrity, mainline CTest, scripted `graph_cli`, scripted propagation, plugin loading, and scheduler repeat checks.
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

The image includes CMake, a C++ toolchain, OpenCV, yaml-cpp, CURL, OpenSSL, GTest, nlohmann-json, clang-format, Python, and cpplint.

CMake 3.16 is the project's compatibility floor, not a workflow-pinned version
for every pull request. The maintained command/provider/context audit checks
the floor's command and policy inventory, while current integration exercises
the fresh static package consumer on the supported CI toolchain. A targeted
real old-version producer/install/consumer run is added when a compatibility-
sensitive change or release check needs it; the regular integration workflow
does not lock Ubuntu or CMake to a dedicated minimum-version job.

## Scripts

CI and CTest execute only long-lived software behavior, compile, package-
consumer, performance, concurrency, stability, error-handling, and runtime-
boundary checks. Migration-residue scans, phase-completion checks, stale-term
searches, Doxygen/source-quality audits, issue replay, and evidence/provenance
orchestration are excluded. The latter belong to issue-specific personal-
overlay evidence under `tests/results/...`, or to explicitly documented manual
developer tools; a clean primary checkout never imports that content.

- `ci/scripts/healthcheck.sh`: runs `git diff --check`, `clang-format --dry-run --Werror`, and `cpplint` on changed C++ files.
- `ci/scripts/ci_image_changed.sh`: detects whether the current diff changes CI image inputs.
- `ci/scripts/integration_suite.sh`: runs the integration checks sequentially for the local-image fallback path.
- `ci/scripts/build_integrity.sh`: configures CMake, explicitly builds `graph_cli`, `test_propagation`, test plugins, scheduler plugins, then runs `ctest -N`.
- `ci/scripts/ctest_full.sh`: builds all targets and runs CTest. Its current default excludes the resource-intensive `SplitComputeServiceRuntimeTrace`; that runtime trace now writes only below the CMake build tree and has no personal-overlay dependency.
- `ci/scripts/graph_cli_script_test.sh`: runs positive and negative scripted REPL checks.
- `ci/scripts/propagation_script_test.sh`: builds `test_propagation` and runs `tiles all` on linear and complex propagation graphs.
- `ci/scripts/plugin_load_test.sh`: checks plugin artifacts, plugin manager tests, scheduler plugin loader tests, and CLI scheduler plugin listing.
- `ci/scripts/scheduler_repeat_test.sh`: repeats key scheduler tests.
- `ci/scripts/sanitizer_test.sh`: runs focused ASan or TSan tests from an isolated build directory.

## Local Commands

```bash
CI_ARTIFACT_DIR=CI-results/healthcheck bash ci/scripts/healthcheck.sh
CI_ARTIFACT_DIR=CI-results/build-integrity bash ci/scripts/build_integrity.sh
CI_ARTIFACT_DIR=CI-results/ctest-full bash ci/scripts/ctest_full.sh
CI_ARTIFACT_DIR=CI-results/graph-cli bash ci/scripts/graph_cli_script_test.sh
CI_ARTIFACT_DIR=CI-results/propagation bash ci/scripts/propagation_script_test.sh
CI_ARTIFACT_DIR=CI-results/plugin-load bash ci/scripts/plugin_load_test.sh
CI_ARTIFACT_DIR=CI-results/scheduler-repeat bash ci/scripts/scheduler_repeat_test.sh
SANITIZER=asan CI_ARTIFACT_DIR=CI-results/sanitizer-asan bash ci/scripts/sanitizer_test.sh
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

For issue #34 review 15, local validation does not use these Docker commands or
`linux/amd64` emulation. Implementation iterations use affected targets and
focused tests; after final source freeze, one native clean configure/full
build/CTest-JUnit pass is reused by all formal local gates. Current-head GitHub
Actions supplies the separate remote integration result.

## Local Artifact Download

Use the personal-overlay script to download GitHub Actions artifacts:

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

It writes to `CI-results/`, which is personal-overlay content and must not be committed to the primary GitHub repository.
