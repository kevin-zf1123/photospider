# 测试与验证

本文档定义仓库级测试与验证行为，属于开发指引，而不是内核运行时架构说明。

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
（例如 `CMP0135`）都必须用 `if(POLICY <policy>)` 保护。兼容性由这项 policy 保护、当前 GitHub
integration package consumer，以及只在 compatibility-sensitive change 或 release check 确有需要时
执行的针对性原生旧版本运行共同维护。

执行针对性最低版本运行时，必须从 fresh producer build tree 开始：使用 CMake 3.16 与
`BUILD_TESTING=OFF` 配置顶层项目，构建真实 `photospider` target，安装到 fresh prefix，然后才
配置、构建并运行外部 `find_package(Photospider)` consumer。不得复用由更新版 CMake 配置的
producer tree，也不得以内部 helper target 替代产品 target。若本机没有原生兼容的旧 CMake
runtime，则跳过该针对性本地运行；不要求进行架构模拟。

Package-consumer smoke 会在不压制清理错误的前提下重新创建临时 install、consumer source 与
consumer build 目录。它在内存中检查观察到的 producer/install/consumer 行为，并把命令、子进程
输出与断言诊断直接写入 stdout/stderr，供 CTest 捕获。所有生成文件都只留在临时工作目录中，
并在运行后丢弃；仓库不会为该测试保留逐次运行报告。

该 smoke 会检查每个已安装的 `Photospider*Targets*.cmake` 文件，因为 package 将基础 target、
依赖 OpenCV 的 target 与 embedded-product target 分到不同 export set 中。禁用 OpenCV discovery
时，请求 `COMPONENTS operation_sdk OPTIONAL_COMPONENTS operation_opencv` 的 consumer 必须让
package 与 `operation_sdk` 保持 found，将 `operation_opencv` 标记为 not found，导入无依赖的
SDK/runtime target，并且不导入 `Photospider::operation_opencv`。在相同条件下 required
`operation_opencv` 必须使 package discovery 失败。OpenCV 可用时，adapter consumer 仅通过 OpenCV
`core` component 导入该 target，并且不会发现无关 package。

IPC enabled 时，package smoke 会构建并安装 `photospider`、
`photospider_ipc_client` 与 `photospiderd`。它会独立 configure 一个默认使用
`Photospider::photospider` 的 embedded consumer，以及一个请求 `COMPONENTS ipc_client`、禁用
OpenCV/`yaml-cpp` discovery、且只链接 `Photospider::photospider_ipc_client` 的 IPC-only project。
后者因此只解析 Threads，不继承 backend 或 JSON implementation target。该 IPC-only consumer
会 include 已安装的 protocol、Client
与 Host-adapter header，在不连接 daemon 的情况下构造 `create_ipc_host()`，执行全部安全 public
Client lifecycle symbol，并链接一个仅用于引用的分支，以精确且唯一的 inventory 覆盖全部 55 个 typed Client call 与全部
53 个非析构 Host virtual。Package 检查还要求 IPC archive 与精确的三个 header surface，导出的
IPC link interface 只允许 `Threads::Threads`；header 正向只允许当前 C++ standard-library include
与已安装的 `photospider/` public include，并拒绝 raw JSON、socket address/descriptor、file
identity、file mapping 与 backend declaration。这是门禁实际保证的精确边界，不声称穷举全部
可能的 POSIX 拼写。在禁用 backend discovery 时，
`COMPONENTS ipc_client OPTIONAL_COMPONENTS embedded` 会只找到 `ipc_client` 并成功；unknown
optional component 会保持 not-found，而不会使 package 无效。长期 `IpcDisabledInstallSmoke` 会用
`PHOTOSPIDER_BUILD_IPC=OFF` 与 `BUILD_TESTING=OFF` 配置另一个 clean producer；它验证不会
advertise IPC build forwarder、installed header、archive、executable 或 exported target，required
`ipc_client` component discovery 会失败，同时 external default embedded Host consumer 仍能
link/run。Required unknown component 也会失败；optional disabled `ipc_client` 与 unknown
component 会保持 not-found 而不使 discovery 失败；省略 component 或请求 `embedded` 时继续
解析既有 backend dependency。

当所选 CMake generator 提供多个 configuration 时，smoke 会为 producer 与 consumer 使用同一个
generator，检查两侧的 `CMAKE_GENERATOR` 和 `CMAKE_CONFIGURATION_TYPES` cache 值，并从
configuration-specific `$<TARGET_FILE:...>` manifest 解析 consumer 可执行文件。

迁移 residue、phase 完成度、陈旧术语与源码布局检查是临时开发检查，不得注册到 CTest 或 CI。
Issue 专属 replay、provenance、helper 和 output artifact 既不得进入 primary repository，也不得
作为 personal overlay 的长期内容保留。长期 runtime、public-header 与 package-consumer 测试负责
维持产品边界。

## 验证归属

Primary repository 中的 CTest 与 CI entry 只用于长期软件行为：正确性、性能、稳定性、多线程
执行、错误处理、编译边界、package consumption 和运行时 API 边界。
`StaticProductConsumerSmoke`、`GraphCliOptionBadAlloc`、GoogleTest discovery、
`PublicHeaderSelfContainment` 满足这一规则，因为它们会执行或编译维护中的产品。
`IpcDisabledInstallSmoke`、focused `test_ipc_protocol`/`test_ipc_host` case 与 real-process
`test_ipc_daemon` case 同样符合该规则：它们验证 package、framing、typed client、完整 IPC Host
dispatch/polling/stop/artifact ownership、daemon lifecycle、concurrency 与 cleanup 行为。Daemon
test 使用 CTest timeout 与 bounded
SIGTERM-to-SIGKILL-to-waitpid cleanup，不依赖固定 readiness sleep。
`StaticProductConsumerSmoke` 仅覆盖 producer configure/build/install、
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
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-cli-host-doxygen
python3 tests/verification/codebase_structure/scheduler_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-scheduler-doxygen
```

这些文件可以留在 primary repository，因为本文定义了它们的长期手工职责；它们必须始终不进入
CTest 与 GitHub CI。其 `--out` 目录是仓库外、可丢弃的临时工作目录，不得成为长期 result tree。
Issue 专属 replay、provenance、helper 和 output artifact 既不得进入 primary
repository，也不得作为 personal overlay 的长期内容保留。Clean primary clone、CMake 配置、
CTest inventory 和 CI script 都不能依赖个人开发内容。

验证应与风险成比例。实现期间只运行 scoped static check、受影响 build target 和 focused
regression。是否运行本机原生 clean configure、full build 或完整 CTest/JUnit，应只根据改动风险
决定，而不是常设要求。不要把 Docker 或本地 `linux/amd64` 模拟作为常规本地 preflight；
current-head GitHub Actions 仍是权威远程 integration 环境。

## OpenCV Operation 并发验证

`test_opencv_operation_concurrency` 是注册到 CTest 的 integration binary，用于验证长期
operation-provider 与 benchmark-worker contract。它使用有界 callback gate，而不是 elapsed-time
threshold：

- `BenchmarkThreadsConfigureExactHostSchedulerWorkers` 会对自动 request 与显式 `1/2/4/8`
  request 运行真实 `BenchmarkService`、Host scheduler 配置、Graph load 与已注册 callback 路径。
  它要求达到精确的解析后 callback 数量，并拒绝出现 grant-plus-one callback。
- `BenchmarkThreadsRejectOutOfDomainValuesBeforeGraphLoad` 要求负数与大于八的 worker request
  在发布 Graph session 前失败。
- `BuiltinCurveCallbacksReachRequestedWorkerConcurrency` 会在每个 `1/2/4/8` grant 下重复三次
  builtin tiled `curve_transform` 路径，并通过仅供测试的 observer 要求精确 callback overlap。
- `BuiltinCurveOutputMatchesBetweenOneAndEightWorkers` 会比较 public Host result 中打包后的
  pixel row，并要求单 worker 与八 worker 输出按位相同。

Observer 只存在于 `BUILD_TESTING` build，是 source tree 私有接口，绝不会安装。这些 case 证明
并发路径可达且输出确定，不承诺与机器无关的 speedup。

`opencv_operation_concurrency_benchmark` 是对应的长期手工 measurement tool，刻意不进入 CTest
或 CI。该工具会创建并清理可丢弃的临时 Graph root，通过真实 Host/benchmark/scheduler/builtin
operation 路径执行，不保留 result artifact，并把环境、原始 wall-time sample、median wall time、
throughput、speedup 与 callback 最大并发度输出到 stdout。构建与运行命令为：

```bash
cmake --build build --target opencv_operation_concurrency_benchmark -j
./build/tests/opencv_operation_concurrency_benchmark \
  --size 2048 --warmups 2 --samples 7 --chain-length 4
```

2026-07-15 采集的原生快照使用 macOS `arm64`、Clang 21.0.0
（`clang-2100.1.1.101`）、OpenCV 4.12.0；报告 hardware concurrency 为 10，且
`opencv_internal_threads=1`。Workload 是在 2048×2048 FP32 image 上串联四个 builtin
`curve_transform` node，每个 grant 先执行两次 warmup，再采集七个 sample：

| Worker | Median wall（ms） | Throughput（Mpix/s） | Speedup | 最大 in flight |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 27.450 | 611.188 | 1.000 | 1 |
| 2 | 19.567 | 857.433 | 1.403 | 2 |
| 4 | 15.688 | 1069.455 | 1.750 | 4 |
| 8 | 15.008 | 1117.910 | 1.829 | 8 |

原始 wall-time sample（单位：毫秒）为：

- 1 worker：`27.694|27.134|27.450|27.183|27.869|27.250|28.035`
- 2 worker：`19.021|19.567|19.774|19.497|19.435|20.427|20.997`
- 4 worker：`16.059|15.688|15.992|15.727|15.600|14.692|14.649`
- 8 worker：`16.436|16.610|16.512|15.008|14.859|14.064|14.760`

该快照证明所请求 grant 到达真实 callback 路径，并且测试机器能从移除外层串行化中获益。它不是
永久性能 baseline 或 pass/fail threshold。在评估另一台机器、compiler、OpenCV version 或
operation-concurrency 变更时，应重新运行准确命令，并解释新输出的原始 sample。

## CTest 注册

所有预期 GoogleTest 二进制都应注册到 CTest。这包括当前可能低置信度的里程碑测试和 `test_propagation_contracts`。

低置信度测试仍应在验证中可见，而不是被静默排除。如果测试不足以可靠地作为开发门禁，应明确记录该状态，并创建后续工作升级或替换它。

里程碑测试和 `test_propagation_contracts` 已注册到 CTest，因此它们可见；但在后续 pass 将它们重写为更窄、更清晰 fixture 和断言的回归测试前，它们仍是低置信度遗留测试。

`test_propagation` 不同：它是脚本式 REPL/tool 目标，不是 GoogleTest 二进制。CMake 保持它可构建，供手工脚本和临时验证使用，但 CTest 不会发现或运行它。不要声称 CTest 覆盖了 `test_propagation`；需要时应单独运行准确的手工命令。

默认 CTest inventory 刻意不包含 phase 完成度 scan、迁移 residue 检查、陈旧术语搜索、Doxygen
audit 或 issue 专属编排。Static package-consumer smoke 与 graph CLI allocation-failure driver
会继续注册，因为它们执行真实的安装/运行时行为。

IPC change 的 focused local product validation 为：

```bash
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol test_ipc_host \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|SchedulerTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonSchedulers|IpcObservationFixtureDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

这些测试结束后，不得遗留 temporary daemon process、socket、graph session、package prefix 或
consumer tree。Mode-`0600` 持久 `${socket}.lock` inode 是有意保留的产品同步 artifact；test-owned
temporary root 会随 root 一起删除它，而真实 default runtime location 会保留它。CTest
output/JUnit 与 remote CI artifact 是证据；不要创建 `tests/results` 或 issue 专属
replay/provenance helper。

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

CI 源码清单与 exclusion list 必须描述维护中的测试和当前源码路径。迁移专用 harness 名称
不得作为永久 exclusion 保留，也不得被视为产品行为。GitHub job 状态和可下载 artifact 用于
报告远程 integration 行为。完整 workflow 和 artifact 下载边界记录在
`docs/CI/zh/github-actions.zh.md`。

架构演进目标不会在本测试文档中维护，而是记录在
`docs/roadmap/zh/Kernel-Evolution.zh.md`。每项实现变更分别定义与风险相称的验证和长期回归覆盖。
