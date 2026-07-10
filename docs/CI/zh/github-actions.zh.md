# GitHub Actions CI

## Workflow

- `.github/workflows/ci-healthcheck.yml`：通过 `pull_request_target` 处理目标为 `main` 的 pull request，并在推送到 `main` 和 `CI/**`、手动触发时运行静态 healthcheck。
- `.github/workflows/ci-integration.yml`：运行构建完整性、主线 CTest、`graph_cli` 脚本测试、propagation 脚本测试、插件加载测试和 scheduler 重复测试。
- `.github/workflows/ci-sanitizer.yml`：手动运行 ASan 或 TSan 聚焦检查。
- `.github/workflows/build-ci-image.yml`：在镜像输入相关 push 和手动触发时发布 GHCR 镜像 `ghcr.io/<owner>/<repo>/photospider-ci`。

## 分支与 workflow 保护

由 push 触发的 CI 只在 `main` 和名称以 `CI/` 开头的分支上运行。这样可以防止普通 feature 分支运行该分支自行修改过的 workflow 文件。

目标为 `main` 的 pull request 使用 `pull_request_target`，即使用 base 分支上的 workflow 定义，同时 checkout pull request 的 head commit 作为被测代码。`CI/**` 分支通过 push 触发验证该分支修改后的 workflow，不再额外启动第二套 pull request run，从而避免同一 commit 重复运行本地镜像 integration。

healthcheck 和 integration 的第一个 job 会在执行任何仓库脚本或本地 CI 镜像构建前保护 CI workflow 输入。对非 `CI/**` 分支，它会比较 `origin/main...HEAD`，并在 diff 修改以下任一路径时失败：

- `ci/**`
- `.github/workflows/**`
- `Dockerfile.ci`

因此，CI workflow 相关修改必须放在 `CI/**` 分支开发。该保护也覆盖目标为 `main` 的非 `CI/**` pull request，避免 workflow 相关文件通过普通 feature 分支合并。

## 运行环境

`Dockerfile.ci` 定义 GitHub Linux 测试环境。常规 healthcheck 和 integration job 会在已发布的 `ghcr.io/<owner>/<repo>/photospider-ci:latest` 镜像中运行。

当 pull request 或 push 修改 CI 镜像输入（`Dockerfile.ci`、`.dockerignore` 或 `.github/workflows/build-ci-image.yml`）时，healthcheck 和 integration workflow 会在当前 workflow 内构建 `photospider-ci:local`，并在该镜像中运行相同脚本。这样可以避免另一个 workflow 尚未发布新 `latest` 镜像时产生竞态。

镜像包含 CMake、C++ 工具链、OpenCV、yaml-cpp、CURL、OpenSSL、GTest、
nlohmann-json、Python、cpplint 和 clang-format。Formatter 通过 PyPI wheel 安装并固定为
21.1.5，避免 CI 与开发机仅因 formatter release 不同而选择不同换行，产生无意义的格式漂移。
维护机当前使用 21.1.3；在本轮对齐覆盖的 changed C++ 清单上，两者的格式化输出已经过逐字节
等价验证。后续建议开发环境统一采用 21.1.5。

本次改动只对齐 clang-format 工具版本。这不是版本检测门禁，不会新增专用 CI job，也不会新增
Ubuntu/CMake 版本锁定；既有 Ubuntu base 与 apt 提供的 CMake 设置保持不变。

## 脚本

- `ci/scripts/healthcheck.sh`：对改动的 C++ 文件运行 `git diff --check`、`clang-format --dry-run --Werror` 和 `cpplint`。
- `ci/scripts/ci_image_changed.sh`：检测当前 diff 是否修改 CI 镜像输入。
- `ci/scripts/integration_suite.sh`：为本地镜像 fallback 路径顺序运行 integration 检查。
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

可选本地镜像源构建：

```bash
docker build -t photospider-ci:local -f Dockerfile.ci \
  --build-arg APT_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/ \
  --build-arg PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple .
```

本机 arm64 构建使用 `ubuntu-ports`。amd64 使用 `http://mirrors.tuna.tsinghua.edu.cn/ubuntu/`。

## 本地 artifact 下载

使用 personal overlay 脚本下载 GitHub Actions artifact：

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

脚本写入 `CI-results/`。该目录属于 personal overlay，不能提交到主 GitHub 仓库。
