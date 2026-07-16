# GitHub Actions CI

## Workflows

- `.github/workflows/ci-healthcheck.yml`: static healthcheck on pull requests targeting `main` through `pull_request_target`, pushes to `main` and `CI/**`, and manual dispatch, followed by one stable `healthcheck` result gate.
- `.github/workflows/ci-integration.yml`: documentation-only routing, dynamic integration planning, parallel build-integrity profiles, separately sharded full CTest and build smoke tests, scripted `graph_cli`, scripted propagation, plugin loading, scheduler repeat checks, and one stable `integration` result gate.
- `.github/workflows/ci-sanitizer.yml`: manual ASan or TSan focused checks.
- `.github/workflows/build-ci-image.yml`: GHCR image publish for `ghcr.io/<owner>/<repo>/photospider-ci` on image-input pushes and manual dispatch.

## Branch and Workflow Guards

Push-triggered CI runs only on `main` and branches whose names start with `CI/`. This prevents ordinary feature branches from running workflow files changed on that branch.

Pull requests targeting `main` use `pull_request_target`, which uses the workflow definition from the base branch while checking out the pull request head commit for tests. `CI/**` branches validate branch-modified workflows through the push trigger instead of a second pull request run, avoiding duplicate local-image integration runs on the same commit.

The first healthcheck and integration job protects CI workflow inputs before any repository script or local CI image build runs. For a non-`CI/**` pull request, it fetches the target branch from the base repository and uses the event's exact base and head commits. Other guarded runs fetch `origin/main` and use `HEAD`. Both paths require exactly one merge base and compare that merge-base tree to the selected head with rename detection and Git-status filtering disabled. This preserves three-dot semantics for a manually dispatched ref that is behind `main` and includes type changes or any uncommon status in the protected-path inventory. The guard fails if the resulting diff changes any of:

- `ci/**`
- `.github/workflows/**`
- `Dockerfile.ci`

CI workflow changes must therefore be developed on a `CI/**` branch. The guard also catches non-`CI/**` pull requests that target `main`, so workflow-related files cannot be merged through an ordinary feature branch.

## Documentation-only Routing

The integration workflow runs `change-classification` after the protected-path guard. A change is documentation-only only when every changed path is one of:

- any file under `docs/**`, including every Chinese mirror;
- a root-level `*.md` or `*.markdown` file, including `readme.md`, `manual.md`, and `CONTEXT.md`;
- the root-level extensionless `README`, `LICENSE`, `NOTICE`, `CHANGELOG`, `CONTRIBUTING`, `CODE_OF_CONDUCT`, or `SECURITY` contract, matched case-insensitively.

Every other path requires the complete build and test chain. This includes source and headers, CMake files, tests, plugins, applications, CI scripts, workflows and actions, configuration, dependencies and lockfiles, Docker inputs, assets, nested Markdown outside `docs/**`, and any unknown file. The classifier uses `git diff --no-renames` without a status filter, so a source file renamed into `docs/**` still exposes the deleted source path and cannot be misclassified as documentation-only. Added, copied, deleted, modified, renamed, type-changed (`T`), unmerged, broken-pairing, and unknown-status paths all enter the inventory; an uncommon status is never omitted merely because it was not enumerated in an allowlist.

For `pull_request` and `pull_request_target`, the classifier requires the exact base and head SHAs from the event and exactly one merge base, then evaluates the pull request diff from that merge base to the head. A `main` push compares the exact `before` and head trees. Every `CI/**` push always runs the full chain, even when a later incremental push contains only documentation; this prevents an earlier source or workflow commit on the same branch from escaping current-head integration after pull-request-trigger deduplication. `workflow_dispatch` also always runs the full chain. An unsupported event, an absent or malformed push branch identity, an absent, malformed, all-zero, shallow, or unreachable revision, a missing or ambiguous merge base, a diff failure, or an empty changed-path inventory all fail closed to full integration. The workflow uses `fetch-depth: 0`; it never guesses `origin/main` or `HEAD~1` when event identity is unavailable.

For a documentation-only change, `ci-image-change`, integration planning, all builds, full CTest, build smokes, and the scripted integration shards are intentionally skipped. The always-running `integration` gate verifies those exact skipped conclusions and writes the reason to the GitHub step summary. It fails when classification or an upstream dependency fails, rather than passing because `needs` silently propagated a skip. The workflows remain triggered instead of using `paths-ignore`, so stable required checks receive a conclusion instead of remaining pending. The `healthcheck` gate likewise always concludes and verifies whichever published-image or local-image healthcheck path was selected. A `CI/**` pull request run reports its intentional push-triggered deduplication through the same stable gates.

## Runtime

`Dockerfile.ci` defines the GitHub Linux test environment. The published-image healthcheck execution and build/test integration jobs run inside `ghcr.io/<owner>/<repo>/photospider-ci:latest`. Protected-path, change-classification, and stable result-gate jobs remain lightweight `ubuntu-latest` jobs and do not configure or compile the project.

When a pull request or push changes the CI image inputs (`Dockerfile.ci`, `.dockerignore`, or `.github/workflows/build-ci-image.yml`), the healthcheck and integration workflows build `photospider-ci:local` inside the workflow and run the same scripts there. Pull-request image detection fetches the base repository branch, verifies the event's exact base SHA, and compares from that base rather than relying on a possibly absent fork `origin/<base>`. A `CI/**` push fetches `origin/main` and detects image inputs cumulatively from the branch merge base, so a later documentation-only push cannot hide an earlier image-input commit. The unfiltered, no-rename inventory also includes type changes. This avoids both a wrong published-image route and a race with another workflow that may still be publishing the new `latest` image.

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

For a non-documentation change on the normal published-image path, `integration-plan` configures the checked-out commit with testing enabled and uses `ctest -N` to discover the exact `StaticProductConsumerSmoke` and `IpcDisabledInstallSmoke` test names. It enables each smoke and its required build only when that test exists. The current `main` tree therefore schedules only `build-integrity-default`, while a refactor commit that introduces the IPC-disabled install smoke also schedules `build-integrity-ipc-disabled`. This is capability discovery from the tested commit, not a hard-coded branch-name check, so the workflow definition based on `main` can also test the refactor pull request head. If a smoke runner exists without its exact CTest registration, planning fails instead of silently dropping coverage.

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

## Scripted CLI Capability Transition

`graph_cli_script_test.sh` selects the explicit-missing-source contract before
it starts any `graph_cli` process. The stable capability marker is the complete
long-lived Graph document error regression registration: the
`tests/integration/test_graph_document_errors.cpp` source, its
`add_ps_test(test_graph_document_errors ...)` target, and its
`gtest_discover_tests(test_graph_document_errors ...)` registration must all be
present. If all three are absent, the tested revision has the legacy
missing-source publication contract. If all three are present, it has the
transactional rejection contract. A partial marker is an inconsistent test
inventory and fails the script.

The marker is evaluated from the checked-out revision, not from a branch name,
commit identity, or observed CLI output. The legacy path therefore positively
requires the warning, published session, current-graph listing, and empty-graph
compute result while rejecting transactional output. The transactional path
requires the classified load failure, empty graph inventory, and absent current
graph while rejecting the legacy warning and publication. Invalid-target
parsing is checked in a separate runtime after loading a maintained fixture, so
it never relies on either missing-source state.

This is a two-stage protected-path transition. First, the `CI/**` change lands
on `main`, where the complete marker is absent and the legacy contract is
verified. Then the architecture pull request must incorporate this same script
unchanged and remove its independent `ci/**` delta; its complete Graph document
error registration selects the transactional contract. Until that second step,
the protected-path guard correctly continues to reject the architecture pull
request's CI-file delta.

After the transactional Graph document contract is present on `main` and every
active pull-request or maintained branch head tested by this protected script
contains the complete registration, a follow-up `CI/**` change must remove the
legacy path and capability switch. The script should then assert transactional
rejection unconditionally.

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

- `ci/scripts/healthcheck.sh`: runs `git diff --check`, the durable change-classification regression, and `clang-format --dry-run --Werror` plus `cpplint` on every nondeleted changed C++ path; its static-tool inventory retains type changes and uncommon statuses.
- `ci/scripts/change_classification.sh`: classifies exact event revisions as documentation-only or full-integration, records all changed and non-documentation paths, and fails closed on Git uncertainty.
- `ci/scripts/change_classification_test.sh`: exercises the long-lived routing contract across documentation, source, mixed, type-change, workflow, rename, deletion, repeated `CI/**` push, pull-request merge-base, missing branch or revision, zero/unavailable revision, manual, empty-diff, and shallow-clone cases.
- `ci/scripts/ci_image_changed.sh`: detects whether the current unfiltered diff changes CI image inputs; workflows provide an exact fetched pull-request base SHA.
- `ci/scripts/integration_plan.sh`: configures a small testing-enabled planning tree, discovers the two exact build-smoke test names with `ctest -N`, validates registration against the runner files, and emits smoke/build capability flags.
- `ci/scripts/integration_suite.sh`: applies the dynamic plan and runs the resulting integration shards sequentially for the local-image fallback path.
- `ci/scripts/build_integrity.sh`: builds the profile selected by `CI_BUILD_PROFILE`. `default` builds the required targets and the complete tree before CTest discovery; `ipc-disabled` sets `BUILD_TESTING=OFF` and `PHOTOSPIDER_BUILD_IPC=OFF`, validates the cache, and builds only the `photospider` producer target.
- `ci/scripts/ctest_full.sh`: reuses or builds the default producer and runs CTest, excluding the two separately sharded build smoke tests by default. Its protected script also retains a no-op exclusion for the removed `SplitComputeServiceRuntimeTrace`; a follow-up `CI/**` branch from main must remove that token after the source-layout change lands.
- `ci/scripts/build_smoke_test.sh`: runs one separately selected build smoke test from a reusable producer; set `SMOKE_TEST` to `static-product-consumer` or `ipc-disabled-install`.
- `ci/scripts/graph_cli_script_test.sh`: runs isolated positive, explicit-missing-source, and invalid-target REPL checks using the pre-execution Graph document capability marker described above.
- `ci/scripts/propagation_script_test.sh`: builds `test_propagation` and runs `tiles all` on linear and complex propagation graphs.
- `ci/scripts/plugin_load_test.sh`: checks plugin artifacts, plugin manager tests, scheduler plugin loader tests, and CLI scheduler plugin listing.
- `ci/scripts/scheduler_repeat_test.sh`: repeats key scheduler tests.
- `ci/scripts/sanitizer_test.sh`: runs focused ASan or TSan tests from an isolated build directory.

## Local Commands

```bash
CI_ARTIFACT_DIR=CI-results/healthcheck bash ci/scripts/healthcheck.sh
CI_CHANGE_EVENT=push \
  CI_CHANGE_BRANCH=main \
  CI_CHANGE_BASE_SHA="$(git rev-parse HEAD~1)" \
  CI_CHANGE_HEAD_SHA="$(git rev-parse HEAD)" \
  CI_ARTIFACT_DIR=CI-results/change-classification \
  bash ci/scripts/change_classification.sh
bash ci/scripts/change_classification_test.sh
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
