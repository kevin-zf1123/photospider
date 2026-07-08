# GitHub Actions CI

## Workflows

- `.github/workflows/ci-healthcheck.yml`: static healthcheck on pull requests, pushes to `main` and `CI/workflow-design`, and manual dispatch.
- `.github/workflows/ci-integration.yml`: build integrity, mainline CTest, scripted `graph_cli`, scripted propagation, plugin loading, and scheduler repeat checks.
- `.github/workflows/ci-sanitizer.yml`: manual ASan or TSan focused checks.
- `.github/workflows/build-ci-image.yml`: manual GHCR image publish for `ghcr.io/<owner>/<repo>/photospider-ci`.

## Runtime

`Dockerfile.ci` defines the GitHub Linux test environment. Integration jobs build it locally as `photospider-ci:local`, then run the same scripts used for local reproduction.

The image includes CMake, a C++ toolchain, OpenCV, yaml-cpp, CURL, OpenSSL, GTest, nlohmann-json, clang-format, Python, and cpplint.

## Scripts

- `ci/scripts/healthcheck.sh`: runs `git diff --check`, `clang-format --dry-run --Werror`, and `cpplint` on changed C++ files.
- `ci/scripts/build_integrity.sh`: configures CMake, explicitly builds `graph_cli`, `test_propagation`, test plugins, scheduler plugins, then runs `ctest -N`.
- `ci/scripts/ctest_full.sh`: builds all targets and runs CTest. It excludes `SplitComputeServiceRuntimeTrace` by default because that test writes personal-overlay evidence under `tests/results/`.
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

## Local Artifact Download

Use the personal-overlay script to download GitHub Actions artifacts:

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

It writes to `CI-results/`, which is personal-overlay content and must not be committed to the primary GitHub repository.
