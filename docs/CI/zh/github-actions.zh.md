# GitHub Actions CI

## Workflow

- `.github/workflows/ci-healthcheck.yml`：在 pull request、推送到 `main` 和 `CI/workflow-design`、手动触发时运行静态 healthcheck。
- `.github/workflows/ci-integration.yml`：运行构建完整性、主线 CTest、`graph_cli` 脚本测试、propagation 脚本测试、插件加载测试和 scheduler 重复测试。
- `.github/workflows/ci-sanitizer.yml`：手动运行 ASan 或 TSan 聚焦检查。
- `.github/workflows/build-ci-image.yml`：手动发布 GHCR 镜像 `ghcr.io/<owner>/<repo>/photospider-ci`。

## 运行环境

`Dockerfile.ci` 定义 GitHub Linux 测试环境。集成测试 job 会本地构建 `photospider-ci:local`，再运行与本地复现相同的脚本。

镜像包含 CMake、C++ 工具链、OpenCV、yaml-cpp、CURL、OpenSSL、GTest、nlohmann-json、clang-format、Python 和 cpplint。

## 脚本

- `ci/scripts/healthcheck.sh`：对改动的 C++ 文件运行 `git diff --check`、`clang-format --dry-run --Werror` 和 `cpplint`。
- `ci/scripts/build_integrity.sh`：配置 CMake，显式构建 `graph_cli`、`test_propagation`、测试插件和 scheduler 插件，然后运行 `ctest -N`。
- `ci/scripts/ctest_full.sh`：构建全部目标并运行 CTest。默认排除 `SplitComputeServiceRuntimeTrace`，因为该测试会向 personal overlay 的 `tests/results/` 写证据。
- `ci/scripts/graph_cli_script_test.sh`：运行正路径和负路径 REPL 脚本检查。
- `ci/scripts/propagation_script_test.sh`：构建 `test_propagation`，并对线性和复杂 propagation 图运行 `tiles all`。
- `ci/scripts/plugin_load_test.sh`：检查插件产物、plugin manager 测试、scheduler plugin loader 测试和 CLI scheduler 插件列表。
- `ci/scripts/scheduler_repeat_test.sh`：重复运行关键 scheduler 测试。
- `ci/scripts/sanitizer_test.sh`：在独立 build 目录运行聚焦 ASan 或 TSan 测试。

## 本地命令

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

Docker 复现：

```bash
docker build -t photospider-ci:local -f Dockerfile.ci .
docker run --rm -v "$PWD:/workspace" -w /workspace photospider-ci:local \
  bash ci/scripts/build_integrity.sh
```

## 本地 artifact 下载

使用 personal overlay 脚本下载 GitHub Actions artifact：

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

脚本写入 `CI-results/`。该目录属于 personal overlay，不能提交到主 GitHub 仓库。
