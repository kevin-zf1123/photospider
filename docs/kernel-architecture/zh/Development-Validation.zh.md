# 开发与验证说明

本文档记录影响内核可信度的仓库级验证预期。

## 主线 macOS 架构

主线 macOS 开发目标是 Apple Silicon `arm64`。

项目不打算保留主线 `x86_64` macOS 支持。如果未来用户需要 `x86_64`，可以通过分支、fork 或专门兼容性工作处理。

在 Apple Silicon 上，编译器目标、终端架构和依赖架构应全部同意为 `arm64`。`x86_64` 构建与 `arm64` Homebrew 库之间的架构不匹配不是受支持的主线设置。

## 构建配置方向

开发者设置应明确架构选择。CMake presets 或 bootstrap 说明应默认 macOS 为 `arm64`。

仓库还应记录或提供：

- `compile_commands.json` 生成
- lint 和格式化命令
- 预期本地验证命令集

根 CMake 配置会导出 `compile_commands.json`，并在没有提供 `CMAKE_OSX_ARCHITECTURES` 值时默认主线 macOS 构建为 `arm64`。

声明的 CMake 3.16 最低版本是可安装静态产品 producer 路径与下游 package consumption 的
兼容性下限，不是每个 pull request 都必须运行的固定 toolchain。任何晚于该下限引入的 policy
（例如 `CMP0135`）都必须用 `if(POLICY <policy>)` 保护。兼容性由针对 3.16 command/policy
inventory 的 command/provider/context audit、当前 GitHub integration package consumer，以及在
compatibility-sensitive change 或 release check 确有需要时执行的针对性真实旧版本运行共同维护。

执行针对性最低版本运行时，必须从 fresh producer build tree 开始：使用 CMake 3.16 与
`BUILD_TESTING=OFF` 配置顶层项目，构建真实 `photospider` target，安装到 fresh prefix，然后才
配置、构建并运行外部 `find_package(Photospider)` consumer。不得复用由更新版 CMake 配置的
producer tree，也不得以内部 helper target 替代产品 target。若本机没有原生兼容的旧 CMake
runtime，则跳过该针对性本地运行；不要求进行架构模拟。

Package-consumer smoke 会在不压制清理错误的前提下重新创建临时 install、consumer source 与
consumer build 目录。它在内存中检查观察到的 producer/install/consumer 行为，并把命令、子进程
输出与断言诊断直接写入 stdout/stderr，供 CTest 捕获。所有生成文件都只留在临时工作目录中，
并在运行后丢弃；仓库不会为该测试保留逐次运行报告。

当所选 CMake generator 提供多个 configuration 时，smoke 会为 producer 与 consumer 使用同一个
generator，检查两侧的 `CMAKE_GENERATOR` 和 `CMAKE_CONFIGURATION_TYPES` cache 值，并从
configuration-specific `$<TARGET_FILE:...>` manifest 解析 consumer 可执行文件。

CMake 3.16 command/provider/context audit 是手工兼容性工具，不是 CTest 或 CI 门禁。迁移 residue、
phase 完成度、陈旧术语与源码布局检查同样是临时开发检查。不得把它们注册到 CTest 或 CI，也不得
在 primary repository 中长期保留 issue 专属编排。长期 runtime、public-header 与
package-consumer 测试负责维持产品边界。

## 验证归属

Primary repository 中的 CTest 与 CI entry 只用于长期软件行为：正确性、性能、稳定性、多线程
执行、错误处理、编译边界、package consumption 和运行时 API 边界。
`StaticProductConsumerSmoke`、`GraphCliOptionBadAlloc`、GoogleTest discovery、
`PublicHeaderSelfContainment` 与 `SplitComputeServiceRuntimeTrace` 满足这一规则，因为它们会执行
或编译维护中的产品。`StaticProductConsumerSmoke` 仅覆盖 producer configure/build/install、
外部 `find_package`、public header compile/link/run、安装后的 export 与依赖边界、平台 archive/
link 行为和 multi-configuration target discovery；它的行为判定不得包含 Git identity、staged 或
unstaged patch hash、invocation replay、environment fingerprint 或 synthetic verifier self-test。
它使用临时工作目录，并把命令与断言诊断直接输出到 CTest 捕获的 stream。Phase 名称、迁移
residue 搜索、陈旧术语 detector、源码布局完成度检查或 issue replay 都不是软件行为测试，不能
注册到 CTest 或由 CI 调用。

CLI/Host 与 scheduler Doxygen AST 工具是长期手工开发工具，不是测试。修改相应声明、定义、
异常契约或 target source closure 时，应显式运行：

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json --out <out>
python3 tests/verification/codebase_structure/scheduler_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json --out <out>
```

这些文件可以留在 primary repository，因为本文定义了它们的长期手工职责；它们必须始终不进入
CTest 与 GitHub CI。Issue 专属 replay 与 migration helper 是临时内容，不得进入 primary
repository。Clean primary clone、CMake 配置、CTest inventory 和 CI script 都不能依赖个人开发
内容。

验证应与风险成比例。实现期间只运行 scoped static check、受影响 build target 和 focused
regression。是否运行本机原生 clean configure、full build 或完整 CTest/JUnit，应只根据改动风险
决定，而不是常设要求。不要把 Docker 或本地 `linux/amd64` 模拟作为常规本地 preflight；
current-head GitHub Actions 仍是权威远程 integration 环境。

## CTest 注册

所有预期 GoogleTest 二进制都应注册到 CTest。这包括当前可能低置信度的里程碑测试和 `test_propagation_contracts`。

低置信度测试仍应在验证中可见，而不是被静默排除。如果测试不足以可靠地作为开发门禁，应明确记录该状态，并创建后续工作升级或替换它。

里程碑测试和 `test_propagation_contracts` 已注册到 CTest，因此它们可见；但在后续 pass 将它们重写为更窄、更清晰 fixture 和断言的回归测试前，它们仍是低置信度遗留测试。

`test_propagation` 不同：它是脚本式 REPL/tool 目标，不是 GoogleTest 二进制。CMake 保持它可构建，供手工脚本和临时验证使用，但 CTest 不会发现或运行它。不要声称 CTest 覆盖了 `test_propagation`；需要时应单独运行准确的手工命令。

默认 CTest inventory 刻意不包含 phase 完成度 scan、迁移 residue 检查、陈旧术语搜索、Doxygen
audit 或 issue 专属编排。Static package-consumer smoke 与 graph CLI allocation-failure driver
会继续注册，因为它们执行真实的安装/运行时行为。

## 已知测试质量注意事项

一些里程碑测试和传播契约测试最初是开发检查，而不是精修过的回归测试。它们应被注册以保持可见，然后在后续升级为更清晰、更高置信度的测试。

`test_propagation` 在被转换为合适的 GoogleTest 二进制，或被更窄的 CTest 注册 fixture 替换之前，仍保持为手工工具目标。

## GitHub/CI 集成状态

GitHub Actions 和 Linux CI container 是当前维护中的验证路径。目标为 `main` 的 pull request
通过 `pull_request_target` 使用 base branch 中受保护的 workflow；推送到 `main` 和 `CI/**`
也会运行 CI。普通 feature branch 不能修改 `ci/**`、`.github/workflows/**` 或
`Dockerfile.ci`；这些输入必须通过 `CI/**` branch 修改。

常规 healthcheck 和 integration job 在已发布的
`ghcr.io/<owner>/<repo>/photospider-ci:latest` 镜像中运行。如果某项改动修改 image input，
workflow 会构建 `photospider-ci:local`，并在该镜像中运行同一套仓库脚本，避免验证过程与镜像发布
产生竞态。`Dockerfile.ci` 会安装这些脚本所需的 C++ toolchain、CMake、OpenCV、yaml-cpp、
GTest、nlohmann-json、clang-format、Python 和 cpplint。

当前维护的入口包括：

- `ci/scripts/healthcheck.sh`：执行 diff、format 和 cpplint 检查。
- `ci/scripts/build_integrity.sh`：执行 configure、必需 target 与全量 build，并完成 CTest discovery。
- `ci/scripts/ctest_full.sh`：运行主 CTest suite。
- `ci/scripts/integration_suite.sh`：顺序执行 integration 行为检查，包括 CLI、propagation、plugin
  和 scheduler 覆盖。

`SplitComputeServiceRuntimeTrace` 会把临时输出写入 CMake build tree，不留下仓库持有的逐次运行
输出。当前 `ctest_full.sh` 的排除属于 CI 运行时间策略，不依赖个人开发内容；当其行为与改动相关
时，开发者可以直接运行该测试。GitHub job 状态和可下载 artifact 用于报告远程 integration 行为。
完整 workflow 和 artifact 下载边界记录在 `docs/CI/zh/github-actions.zh.md`。

## 重构边界

以下是已识别的后续重构，不属于当前 kernel-contract 清理：

- 在 frontend 展示契约定义后，添加更丰富的 dirty snapshot 可视化 API。
- Global HP dirty ROI 现在会进入 HP dirty planning，而不是过去无条件的完整重算 fallback。
  非 forced request 应证明局部 dirty work selection；forced HP dirty request 应证明 full-frame
  HP planning 和完整 authoritative HP output，然后才能宣称 covered path 之外的正确性或性能收益。

`ComputeService` 拆分现在已有专门的 `split-compute-service` OpenSpec change，
并在维护文档 `Compute-Service-Split.md` 中记录计划。第一轮拆分已经通过
`src/kernel/services/compute-service/` 下的内部模块实现。边界覆盖位于
`tests/test_compute_service_split.cpp`，并保留 `test_kernel_contracts`、
`test_propagation_contracts`、`test_scheduler`、`test_milestone34` 和
`test_gpu_pipeline_scheduler` 的回归覆盖。

`GraphTraversalService` 拓扑/ROI 拆分已经落地。边界覆盖位于
`tests/test_graph_topology_boundaries.cpp`、`tests/test_propagation_contracts.cpp`，以及消费这些边界的
长期运行时行为测试。
