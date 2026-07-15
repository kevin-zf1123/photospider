# GitHub Actions CI

## Workflow

- `.github/workflows/ci-healthcheck.yml`：通过 `pull_request_target` 处理目标为 `main` 的 pull request，并在推送到 `main` 和 `CI/**`、手动触发时运行静态 healthcheck。
- `.github/workflows/ci-integration.yml`：运行动态 integration 规划、并行 build-integrity profile、独立分片的完整 CTest 与 build smoke 测试、`graph_cli` 脚本测试、propagation 脚本测试、插件加载测试和 scheduler 重复测试。
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

## Integration 测试分片

对常规 published-image 路径，`integration-plan` 会启用测试来配置被 checkout 的 commit，并使用 `ctest -N` 精确发现 `StaticProductConsumerSmoke` 和 `IpcDisabledInstallSmoke` 测试名。它仅在对应测试存在时启用各 smoke 及其所需 build。因此，当前 `main` 树只调度 `build-integrity-default`；引入 IPC-disabled install smoke 的 refactor commit 还会调度 `build-integrity-ipc-disabled`。这是针对被测 commit 的能力发现，而不是硬编码的分支名判断，因此以 `main` 为基础的 workflow 定义也能测试 refactor pull request 的 head。如果 smoke runner 已存在却没有对应的精确 CTest 注册，规划会失败，不会静默丢失覆盖。

每个 profile 都有独立 build job，只配置和编译该 profile，再上传独立的可复用构建 artifact：`ci-build-default` 或 `ci-build-ipc-disabled`。IPC-disabled profile 使用 `PHOTOSPIDER_BUILD_IPC=OFF` 配置 producer。Default consumers 只依赖 `build-integrity-default`，IPC-disabled smoke 只依赖 `build-integrity-ipc-disabled`；因此，一个 profile 失败不会抑制另一个 profile 的测试。

测试 job 会复用这些预构建 producer，不再由一个 runner 重新编译全部配置：

- `full-ctest`、`scripted-cli`、`propagation-script`、`plugin-load` 和 `scheduler-repeat` 只下载 `ci-build-default`。
- `full-ctest` 排除 `SplitComputeServiceRuntimeTrace`、`StaticProductConsumerSmoke` 和 `IpcDisabledInstallSmoke`。split trace 继续置于主 CI 之外，两个耗时较长的 build smoke 测试则在各自 job 中运行。
- `static-product-consumer-smoke` 下载 `ci-build-default`，并且只运行 `StaticProductConsumerSmoke`。
- `ipc-disabled-install-smoke` 下载预编译的 `ci-build-ipc-disabled` producer，并且只运行 `IpcDisabledInstallSmoke`。

如果 CI 镜像输入发生变化，workflow 不能使用此前发布的镜像，而会在一个具备 Docker 能力的 runner 上运行 `local-image-integration`。构建 `photospider-ci:local` 后，`integration_suite.sh` 会执行同样的动态规划，构建发现到的每个 profile，再使用各自对应的 build，依次运行相同的完整 CTest、build smoke、CLI、propagation、plugin 和 scheduler 分片。该 fallback 保持相同的测试选择与 producer 配置，但接受单个本地镜像 runner 无法将工作扇出到多个 artifact-consuming job 的限制。

## 脚本式 CLI 能力过渡

`graph_cli_script_test.sh` 会在启动任何 `graph_cli` 进程前选择“显式来源缺失”契约。稳定能力标记是
完整的长期 Graph 文档错误回归注册：必须同时存在
`tests/integration/test_graph_document_errors.cpp` 源文件、对应的
`add_ps_test(test_graph_document_errors ...)` target，以及
`gtest_discover_tests(test_graph_document_errors ...)` 注册。三者全都不存在时，被测 revision 使用旧的
missing-source 发布契约；三者全都存在时，被测 revision 使用事务式拒绝契约；只存在一部分说明测试
清单不一致，脚本会直接失败。

能力标记来自 checkout 的 revision，不依赖分支名、commit identity 或观察到的 CLI 输出。因此，
旧路径会正向要求 warning、已发布 session、current Graph 清单以及空 Graph compute 结果，同时拒绝
事务路径输出；事务路径会要求分类后的 load 失败、空 Graph 清单和不存在 current Graph，同时拒绝
旧 warning 与发布行为。无效 target 解析会在另一个隔离 runtime 中先加载维护中的 fixture，因此
绝不依赖任一种 missing-source 状态。

这是一个分两阶段完成的受保护路径过渡。第一阶段先把 `CI/**` 变更合入 `main`；此时完整标记
不存在，脚本验证旧契约。第二阶段要求架构 pull request 原样采用同一脚本，并消除其独立的
`ci/**` 差异；该分支完整的 Graph 文档错误注册会选择事务契约。在第二阶段完成之前，受保护路径
门禁仍会正确拒绝架构 pull request 中的 CI 文件差异。

当事务式 Graph 文档契约已经进入 `main`，并且所有由该受保护脚本测试的活跃 pull-request 或维护
分支 head 都包含完整注册后，后续 `CI/**` 变更必须删除旧路径与能力切换。此后脚本应无条件断言
事务式拒绝。

## 脚本

- `ci/scripts/healthcheck.sh`：对改动的 C++ 文件运行 `git diff --check`、`clang-format --dry-run --Werror` 和 `cpplint`。
- `ci/scripts/ci_image_changed.sh`：检测当前 diff 是否修改 CI 镜像输入。
- `ci/scripts/integration_plan.sh`：配置一个启用测试的小型规划 build tree，使用 `ctest -N` 发现两个精确 build-smoke 测试名，对照 runner 文件校验注册，并输出 smoke/build 能力标记。
- `ci/scripts/integration_suite.sh`：应用动态规划，并为本地镜像 fallback 路径顺序运行所得 integration 分片。
- `ci/scripts/build_integrity.sh`：构建 `CI_BUILD_PROFILE` 选定的 profile。`default` 会构建 required targets 与完整 build tree，再执行 CTest discovery；`ipc-disabled` 设置 `BUILD_TESTING=OFF` 与 `PHOTOSPIDER_BUILD_IPC=OFF`，校验 cache，并且只构建 `photospider` producer target。
- `ci/scripts/ctest_full.sh`：复用或构建 default producer 并运行 CTest，默认排除 `SplitComputeServiceRuntimeTrace` 和两个已独立分片的 build smoke 测试。
- `ci/scripts/build_smoke_test.sh`：从可复用 producer 运行一个单独选择的 build smoke 测试；将 `SMOKE_TEST` 设置为 `static-product-consumer` 或 `ipc-disabled-install`。
- `ci/scripts/graph_cli_script_test.sh`：使用上述执行前 Graph 文档能力标记，运行相互隔离的正路径、显式来源缺失和无效 target REPL 检查。
- `ci/scripts/propagation_script_test.sh`：构建 `test_propagation`，并对线性和复杂 propagation 图运行 `tiles all`。
- `ci/scripts/plugin_load_test.sh`：检查插件产物、plugin manager 测试、scheduler plugin loader 测试和 CLI scheduler 插件列表。
- `ci/scripts/scheduler_repeat_test.sh`：重复运行关键 scheduler 测试。
- `ci/scripts/sanitizer_test.sh`：在独立 build 目录运行聚焦 ASan 或 TSan 测试。

## 本地命令

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

只有被 checkout commit 的 integration plan 报告了相应能力时，`ipc-disabled` profile 和对应的 smoke 命令才有效。要复现动态选出的完整执行顺序，可使用：

```bash
CI_ARTIFACT_ROOT=CI-results bash ci/scripts/integration_suite.sh
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
