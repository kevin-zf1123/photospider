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

在 macOS 上，每个 install-consumer smoke 都会读取所选 producer 已解析的
`CMAKE_OSX_ARCHITECTURES` cache 值，并把精确且有意义的值作为一个参数传给每个外部 CMake
configure。因此，以分号分隔的 universal architecture 列表会保持完整；即使由 Rosetta 启动的
外层 runner 会选择另一种 compiler 默认值，producer、已安装 static archive 与全部 consumer
仍保持同一 architecture profile。该传播只在 Darwin 上生效；Linux 和 Windows child 绝不会
收到这个 macOS 专属选项。这不会创建或保留受支持的主线 `x86_64` 路径。

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
依赖 OpenCV 的 target 与 embedded-product target 分到不同 export set 中。它的 dependency
classifier 只识别 producer 接受的精确 OpenCV component target 拼写：裸 lowercase name、lowercase
`OpenCV::opencv_*` target，以及 `OpenCV::Core` 这类 component-specific CamelCase target；partial-name
match 仍会被拒绝。验证证据来自真实 exported package/consumer 行为，而不是 synthetic verifier
self-test。禁用 OpenCV discovery
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

长期 `DependencyDisabledInstallSmoke` 会配置一个 OpenCV 与 YAML capability 均禁用的 clean
producer，禁用这两个 package discovery，关闭 IPC/testing，并构建真实 `photospider_kernel`
aggregate 与 `photospider` product。它会验证派生的 provider/plugin/CLI 默认值，以及三类无效
显式组合的精确诊断。Clean install 后，它会拒绝 OpenCV header、target、export reference 与
yaml-cpp link 泄漏；optional `operation_opencv` 保持 unavailable，required component 则失败。
外部 consumer 会在两个 discovery 均禁用时配置，链接并运行
`Photospider::photospider`，分配中立 image，加载并关闭 empty Host session，并观察显式 YAML
operation 返回 `GraphErrc::Io`。CI 只有在校验 producer cache identity、configuration 与完整
capability profile 后才可复用该 producer。

当所选 CMake generator 提供多个 configuration 时，smoke 会为 producer 与 consumer 使用同一个
generator，检查两侧的 `CMAKE_GENERATOR` 和 `CMAKE_CONFIGURATION_TYPES` cache 值，并从
configuration-specific `$<TARGET_FILE:...>` manifest 解析 consumer 可执行文件。

迁移 residue、phase 完成度、陈旧术语与源码布局检查是临时开发检查，不得注册到 CTest 或 CI。
Issue 专属 replay、provenance、helper 和 output artifact 既不得进入 primary repository，也不得
作为 personal overlay 的长期内容保留。长期 runtime、public-header 与 package-consumer 测试负责
维持产品边界。

## Build-smoke CI 分类

Build smoke 是一种长期维护的 CTest，其主要边界会委托执行 CMake configure/build/install、
exported package 或 external consumer build，或者专用 compile target。所有此类测试都携带精确且
稳定的 CTest 标签 `build-smoke`。如果 companion 只在进程内调用 driver 的 Python cleanup 或 layout
helper，它会作为普通 safety regression 留在完整 CTest 分片。

当前带标签的 inventory 是
`DependencyDisabledInstallSmoke`、
`ImageArtifactCodecDependencyDisabledBuild`、
`IpcDisabledInstallSmoke`、
`OpenCvOperationProviderDisabledBuild`、
`PublicHeaderSelfContainment` 和
`StaticProductConsumerSmoke`。`PublicHeaderSelfContainment` 属于该分类，因为它的 CTest command
会构建专用 self-containment target；普通 GoogleTest binary、daemon/CLI process test 与
`PhotospiderdCapabilityHelp` 不会创建 child build，因此继续留在主 CTest 分片。
`OpenCvOperationProviderBuildSmokeSafety` 也留在该分片：它是 OpenCV build-smoke driver 的普通
safety regression，本身不会启动 CMake、CTest、install 或 compile target。
`InstallConsumerArchitecturePropagationSafety` 同样留在主分片：它使用可丢弃的 producer cache
fixture 执行三个 install-consumer driver 的真实命令构造路径，同时替换 subprocess 执行，因此能
在不启动 configure、build 或 install 的情况下验证 cache 到 child argv 的传播。CMake 注册该
safety test 时，还会传入当前 build tree、CTest executable、configuration 与 Python launcher。
测试随后通过 `ctest --show-only=json-v1` 和生产 inventory parser 查询该 build tree，并要求三个
真实 smoke 遵循配置相关的精确集合：所有 profile 都必须各自只注册一次
`DependencyDisabledInstallSmoke` 与 `IpcDisabledInstallSmoke`；
`StaticProductConsumerSmoke` 只在 IPC enabled 时必须精确注册一次，在 IPC disabled 时必须缺席。
每个预期 entry 还必须保持 enabled、带正确 label，并以精确的 `python -B` driver path 开头。被
注释或处于 inactive CMake 分支中的源码不会生成 CTest entry，因此无法通过这项生成后 inventory
检查。该查询不会执行任何真实 smoke，也不会改变现有六项 build-smoke 分类。

CTest 会保留每个带标签测试的注册，供本机直接运行。CI 的 `full-ctest` 分片会排除该精确标签；
配置规划只会把 `ctest --show-only=json-v1` 解析为允许空集合的预检，因为默认
`gtest_discover_tests` entry 此时可能仍是未带标签的 `_NOT_BUILT` 占位项。完整 default build
结束后，build-integrity 会以严格模式再次查询，并为每个带标签测试发布一个独立 matrix job。因此，
新增长期 build smoke 只需要注册 CTest 并添加相同 label，不需要修改 workflow 中的测试名。
Preflight 会对 malformed inventory、duplicate、非法 label 形状或 disabled/commandless 的带标签
entry fail closed，但不会因空 selection 失败；构建后权威查询还会拒绝空 label set。执行前 runner
会重新查询 inventory；所选名称 absent、duplicate、disabled、commandless 或不再带标签时都会被
拒绝。完成该精确 label 校验后，它只使用经过校验的 CTest 数字索引选择测试，因此任意测试名字符
都不会被 shell 或 regular expression 解释。

Published-image workflow 会在恢复同一份可复用 default producer 后扇出严格 build-integrity
output。当 producer job 被有意跳过时，空 include fallback 会让 `fromJSON` 保持有效；成功的
producer 不可能发布空的严格 matrix。每个 CTest 注册保留自身 timeout 与 `RUN_SERIAL` 行为；每个
matrix item 另有独立 workflow timeout 与 result artifact。Local-image fallback 只有一个
Docker-capable runner，因此读取同一份构建后 NUL 分隔名称并顺序执行。Nested driver 必须继续使用
彼此不重叠的 work directory，校验其接受的任何可复用 producer identity，并且在 cleanup 时不得
跟随 symlink 或删除无关 symlink target。

## 验证归属

Primary repository 中的 CTest 与 CI entry 只用于长期软件行为：正确性、性能、稳定性、多线程
执行、错误处理、编译边界、package consumption 和运行时 API 边界。
`PhotospiderdCapabilityHelp`、`StaticProductConsumerSmoke`、
`GraphCliOptionBadAlloc`、GoogleTest discovery 与 `PublicHeaderSelfContainment` 满足这一规则，
因为它们会执行或编译维护中的产品。Daemon help 测试通过 CMake script driver 运行当前
configuration 对应的真实 `photospiderd --help`，分别捕获 stdout 与 stderr，先要求进程结果是
数值零，再匹配稳定 capability sentence；启动失败与非零退出会得到不同诊断。
`IpcDisabledInstallSmoke`、`DependencyDisabledInstallSmoke`、focused
`test_ipc_protocol`/`test_ipc_host` case 与 real-process `test_ipc_daemon` case 同样符合该规则：
它们验证 package、framing、typed client、完整 IPC Host
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

CLI/Host 审计将
`apps/graph_cli/src/cli_config.cpp::apply_cli_scheduler_defaults` 视为 scheduler
默认值的 canonical 定义，并在该 translation unit 中验证其完整 Doxygen。工具会另外审计
`run_graph_cli` 及其资源耗尽策略。对于 `ConfigEditor::SyncUiStateToModel`，两个解析链都必须
重新抛出 `std::bad_alloc`。Scheduler worker 校验会单独处理 `std::invalid_argument`，而
history-size 的 `std::stoi` 只通过一个 broad `catch (...)` 忽略其他错误；因此 catalog 对该函数
只期望一个 broad catch，且其前面必须有 `std::bad_alloc` handler 保护。

这些文件可以留在 primary repository，因为本文定义了它们的长期手工职责；它们必须始终不进入
CTest 与 GitHub CI。其 `--out` 目录是仓库外、可丢弃的临时工作目录，不得成为长期 result tree。
Issue 专属 replay、provenance、helper 和 output artifact 既不得进入 primary
repository，也不得作为 personal overlay 的长期内容保留。Clean primary clone、CMake 配置、
CTest inventory 和 CI script 都不能依赖个人开发内容。

验证应与风险成比例。实现期间只运行 scoped static check、受影响 build target 和 focused
regression。是否运行本机原生 clean configure、full build 或完整 CTest/JUnit，应只根据改动风险
决定，而不是常设要求。本机 workflow 源码、YAML 与 shell 检查只属于开发者 preflight；它们不模拟
托管 GitHub Actions runner。不要把 Docker 或本地 `linux/amd64` 模拟作为常规本地 preflight；
current-head GitHub Actions 仍是权威远程 integration 环境。

## CLI option action 验证

`test_cli_scheduler_config` 是注册到 CTest 的 integration binary，负责可复用
`run_graph_cli` option 边界和 scheduler 配置。它的 option case 使用完整的确定性 Host spy 与
真实有序 parser。成功的 load/output 与短 traversal case 会保留 Host 返回的 session target，
并固定 `-t` 无参数 grammar。失败 case 要求 load、output、dependency-tree print、
traversal-order 与全缓存清理失败返回可恢复 exit code 2，且不得打印成功 footer 或进入 REPL。
Load case 还会捕获 REPL banner，证明唯一 action 失败时，失败结果优先于正常的 no-action
fallback。

Option replay 仍保持有序；另一个可恢复 action 失败前后已经成功的 action 可能产生可见效果，
该边界不提供多 action rollback transaction。尽管如此，只要任一 action 或 loaded-graph
前置条件失败，最终结果就必须是失败，而且该失败优先于显式 `--repl`。没有 option action 的
调用仍正常进入 REPL。可用以下命令运行聚焦边界：

```bash
cmake --build build --target test_cli_scheduler_config -j
./build/tests/test_cli_scheduler_config \
  --gtest_filter='CliOptionActions.*'
```

## Graph 文档错误矩阵验证

`test_graph_document_errors` 是注册到 CTest 的 integration binary，用于验证长期
Graph document ingestion 与 save 契约。它同时覆盖 public embedded Host 边界和直接
`GraphModel::replace_nodes` transaction 边界。Load/reload case 区分“省略 source path”与
“显式 source path”，对 I/O、YAML、schema、topology、lifecycle 与 unexpected failure
要求精确的 `GraphErrc` 分类，并证明 `std::bad_alloc` 仍保持 exception 语义。测试还要求：
initial load 失败不发布 session；reload 失败保留 prior Graph 的完整状态；成功 replacement
推进 topology generation 并重置 runtime state；失败后仍可重试。

`test_host_adapter` 负责确定性的 reload 与 close 生命周期回归。真实 blocking compute 与三个显式
Host-operation gate 会证明：close marker 之前已准入的 reload 在进入 Kernel 前以及 public status
转换后仍保持 admission，close 不能先完成；该 marker 之后重复 reload 必须在不进入 Kernel 的
情况下返回 `GraphErrc::NotFound`。Node-YAML 与 forward/backward ROI 的配套竞态会证明 reload
仍在每个 required lookup-and-use work item 之后运行，因此 close admission 修正不会削弱
graph-state ordering。这些测试使用 event gate 与零时长 future snapshot，不使用 timing sleep。

同一个 binary 负责 public save transaction regression。其仅供测试、按 destination 限定的
`BUILD_TESTING` checkpoint 会在 graph-state worker 上、destination open 前立即运行。一个 case
要求可恢复失败返回 `GraphErrc::Io`，另一个要求精确传播 `std::bad_alloc`。两者都要求 existing
destination bytes，以及通过 public inspection 观察的 session 与 node state 保持不变，然后要求
未注入故障的 save retry 成功。Const GraphIO boundary 与串行化 owner path 提供更广泛的
non-mutation 保证。Production build 会编译掉该 checkpoint，并保留唯一的真实 writer。

以下 focused companion regression 负责其余边界：

- `test_kernel_contracts` 驱动真实 `GraphIOService` stream 进入 post-write、post-flush 与
  post-close failure state。每个 phase 都必须返回 `GraphErrc::Io`；已创建的 destination 证明
  文档所述 non-atomic post-open 行为。
- `test_scheduler_worker_budget` 证明无效 input document 会释放两个已启动的 scheduler
  reservation，且不发布 session。
- `test_ipc_protocol` 证明精确 Graph status 传递、mutation 只调用一次，以及 failed load 后
  daemon session-name rollback。
- `test_ipc_daemon` 证明真实 transport 精确返回 save `NotFound` 与 `Io`，destination failure 后
  remotely owned graph 仍可 inspect，并接受随后成功的 save。

可用以下命令执行 focused validation：

```bash
cmake --build build --target test_graph_document_errors test_host_adapter \
  test_kernel_contracts test_scheduler_worker_budget test_ipc_protocol \
  test_ipc_daemon -j
./build/tests/test_graph_document_errors
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.*Reload*'
./build/tests/test_kernel_contracts \
  --gtest_filter='GraphIoContract.Save*'
./build/tests/test_scheduler_worker_budget \
  --gtest_filter=EmbeddedHostSchedulerBudget.InvalidYamlAfterSchedulerStartDoesNotPublishAndReturnsPairExactlyOnce
./build/tests/test_ipc_protocol \
  --gtest_filter=ProtocolGraphLoad.FailedHostLoadReleasesNameForRetry
./build/tests/test_ipc_daemon \
  --gtest_filter=IpcDaemonGraphLifecycle.PersistsAcrossClientsAndInspectsCopiedSnapshots
```

这些是长期维护的产品行为测试。该验证面不应包含 migration-residue scan、Issue
专属 replay script 或长期保留的 result artifact。

持续维护的 CLI 脚本式集成检查 `ci/scripts/graph_cli_script_test.sh` 负责对应的 REPL
边界。它的“显式来源缺失”场景要求 load 失败、`graphs` 清单为空且不存在当前 Graph；
它的“无效 target”场景会先加载维护中的 propagation fixture，再要求 target 被拒绝，
因此不会依赖失败 load 发布状态。每个场景都使用相互隔离的临时 session 与 history
存储，并在脚本退出时删除。

## 注入式图像 Artifact Codec 验证

`test_kernel_contracts` 负责长期 fake-codec cache 边界。它的
`CacheSemantics.InjectedCodec*` 用例会用共享 `FakeImageArtifactCodec` 构造
`GraphCacheService` 或真实 `Kernel`，并验证精确 decode/encode path、service 保持的 codec
生命周期、`int16` 精度选择、不会变更 HP cache 的可恢复 `GraphErrc::Io` diagnostic，以及精确的
`std::bad_alloc` 传播。Kernel 生命周期用例会阻塞真实 `GraphStateExecutor`，再准入第二个借用
`Kernel::cache_service_` 的 cache-save work item，释放 caller 的唯一 codec owner，并在另一线程
销毁 Kernel。Executor checkpoint 与 future 会要求析构保持等待、已准入 encode 观察到仍存活的
codec，并且 codec 只能在 Kernel 析构完成后释放。Fake 不执行真实图像格式 IO，因此这些测试不
依赖 OpenCV codec 行为，但会执行生产 runtime 与 cache service。

`ImageArtifactCodecDependencyDisabledBuild` 会用
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` 与 `PHOTOSPIDER_BUILD_IPC=OFF`
配置 fresh nested build，构建 provider-independent focused
`test_kernel_contracts` target，并只运行注入式 codec 用例。该 target 在不把完整 kernel-contract
binary 注册到此画像 CTest inventory 的前提下保持可用。这证明 Graph/cache 注入契约与 fake
不依赖可选 operation provider。独立的 `DependencyDisabledInstallSmoke` 覆盖完全省略 OpenCV
discovery、并选择 unavailable production codec 的完整 product profile。

聚焦验证命令为：

```bash
cmake --build build --target test_kernel_contracts -j 2
./build/tests/test_kernel_contracts \
  --gtest_filter='CacheSemantics.InjectedCodec*'
ctest --test-dir build --output-on-failure \
  -R '^ImageArtifactCodecDependencyDisabledBuild$' -j 2
```

## 可选 OpenCV Operation Provider 验证

`test_optional_opencv_operation_provider` 是针对两种 provider 配置构建并注册到 CTest 的
integration binary。在普通配置中，它会 seed 仓库 OpenCV provider，执行真实 resize callback，
证明无效 OpenCV matrix shape 会被翻译为 host-owned `GraphErrc::ComputeError`，再加载一个
stdlib-only v2 provider，使其完整拥有 resize 的 execution/dirty/forward slot，执行 replacement
sentinel output，卸载该 provider，最后执行已恢复的 OpenCV predecessor。

`test_opencv_operation_provider_exceptions` 在独立进程中运行，因此第一次 provider 初始化尝试
是确定性的。私有 `BUILD_TESTING` hook 会在真实 `std::call_once` body 内、
`cv::setNumThreads(1)` 之前注入一次 `cv::Exception`：第一次注册必须在不发布 callback 的
情况下返回 host-owned `GraphErrc::ComputeError`；下一次注册必须重试、把 OpenCV thread count
设为一并发布 provider。同一个私有且不安装的 test-access 边界会直接驱动真实 monolithic 与
tiled exception wrapper。两次相互独立的 `cv::Error::StsNoMem` 注入都必须分别表现为精确、新建
的 `std::bad_alloc`；tiled 非资源耗尽失败必须表现为 `GraphErrc::ComputeError`。测试不会尝试
真实内存耗尽，也不会修改 public ABI。

`OpenCvOperationProviderDisabledBuild` 会使用
`BUILD_TESTING=ON` 与 `PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF`
配置一个临时嵌套 build，同时保留 OpenCV、YAML、graph CLI 与 operation-plugin 的默认启用值。
因此 provider-aware broad suite gate 为关闭。Driver 会校验精确 CMake cache 画像，只构建上述
provider-independent focused binary 与 stdlib-only fixture，再查询机器可读的 CTest
inventory。该 inventory 必须精确包含 `DependencyDisabledInstallSmoke` 与
`OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores`，不得残留任何依赖 provider
的 broad test。Driver 随后通过 CTest 运行该 focused case。禁用 profile 要求依赖中立
analyzer/math operation 仍被 seed、OpenCV-backed operation key 不存在，并要求 replacement
provider 能发布、执行且完整退役其 resize key。该临时 build 是长期 product configuration
检查；它把命令与结果写入 CTest，不保留逐次运行报告。当前阶段禁用的是 operation provider，
不是彼此独立的 OpenCV codec、normalization、adapter 或 embedded-product 依赖。

嵌套 build driver 在移除临时目录前，会得到绝对的 work 路径拼写，但不会把它解析成 symlink
目标后再删除。它会拒绝 parent traversal、repository、任一 repository ancestor、filesystem
root，以及最终 work 路径或任一现存 parent component 中的 symlink。Canonical resolution
只用于受保护位置比较；recursive removal 前会立即重复同一组检查，并且始终把通过验证的绝对
路径拼写而不是 symlink 目标交给删除函数。Recursive removal 失败会原样传播，lstat 风格的
postcondition 还会确认目录或 dangling link 都没有残留。
`OpenCvOperationProviderBuildSmokeSafety` 只针对 disposable temporary root 下的 synthetic
repository、ancestor 和无关 symlink target，验证这些破坏性 guard、失败传播和 postcondition。
它的 final-symlink 与 symlinked-parent case 要求每个无关 target 和 marker 都存活；测试绝不会把
真实 checkout 或其 parent 传给 remover。Driver 还会读取嵌套 `CMakeCache.txt`：非空的
`CMAKE_CONFIGURATION_TYPES` 选择 `tests/<config>/`，single-config cache 则必须包含与请求完全
一致的 `CMAKE_BUILD_TYPE`。缺失或互相矛盾的 cache state 会显式失败；safety regression 会在
不依赖 host platform 的情况下覆盖两种 layout。它是快速的普通完整 CTest regression，会在进程内
import 并调用 driver helper；只有 `OpenCvOperationProviderDisabledBuild` 会启动 child
configure/build/CTest profile 并携带 `build-smoke` 标签。

## OpenCV Operation 并发验证

`test_opencv_operation_concurrency` 是注册到 CTest 的 integration binary，用于验证长期
operation-provider 与 benchmark-worker contract。它使用 Host-boundary record 与有界 callback
gate，而不是 elapsed-time threshold：

- `BenchmarkAutoThreadsPublishResolvedGrantToHost` 证明自动选择会在 Host 配置前只解析一次，
  在 Graph load 前发布非零 grant，并报告完全相同的 grant；判定过程不会重复硬件探测。
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

只有 `BUILD_TESTING`、OpenCV、YAML、graph CLI、仓库 OpenCV operation provider 与仓库 OpenCV
operation plugin 全部启用时，才会注册依赖 provider 的默认完整 test suite。该 suite 会注册
`test_stdlib_image_buffer_processing`；即使该 producer 使用 OpenCV，它仍会直接编译标准库实现。
该测试验证 clone independence、stride-safe 且确定性的 bilinear border 行为、channel
conversion 与 ROI copy。默认 CTest inventory 也包含 `DependencyDisabledInstallSmoke`。

若在其他默认 test profile 选项不变时只禁用
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER`，CMake 不会创建或发现 broad suite。它会为注入式
codec smoke 保留可构建的 provider-independent `test_kernel_contracts` target，并且只注册
focused optional-provider GoogleTest 与 `DependencyDisabledInstallSmoke`。

默认 CTest inventory 刻意不包含 phase 完成度 scan、迁移 residue 检查、陈旧术语搜索、Doxygen
audit 或 issue 专属编排。Daemon help driver、static package-consumer smoke 与 graph CLI
allocation-failure driver 会继续注册，因为它们执行真实的安装/运行时行为。

IPC change 的 focused local product validation 为：

```bash
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol test_ipc_host \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|SchedulerTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonSchedulers|IpcObservationFixtureDaemon|PhotospiderdCapabilityHelp|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
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
`Dockerfile.ci`；这些输入必须通过 base repository 中的 `CI/**` branch 修改。只有同仓库
`CI/**` pull request 会改由该分支的 push run 处理。Fork 使用相同分支前缀时会在 checkout
前被拒绝，分支拼写本身绝不授权 protected-path 修改。
两份生产门禁都会把 `git diff --name-only -z` 写入父 shell 可见的 artifact，由 Bash 读取精确
NUL record、按完整路径值匹配，并用 `%q` 生成供人阅读的 changed/protected 日志。Producer 或
reader 失败时会 fail closed；合法 `ci/**` 文件名中的换行既不能绕过门禁，也不能伪造清单记录。

每次触发的 run 都会保留稳定的 `healthcheck` 结论。integration workflow 会在 configure 前对
event 的精确 revision 分类：仅修改 `docs/**`、根目录 Markdown 和已记录根目录文本契约的变更会
有意跳过所有 build、CTest 与 integration 分片，再由稳定的 `integration` 门禁校验并报告该路由。
任何非文档路径或不确定 Git 状态都会执行完整 integration。Type change 与少见 Git status 会保留在
不带过滤的路径清单中。每次 `CI/**` push 也都会强制执行 current-head 完整 integration，包括后续
仅修改文档的增量 push。workflow 刻意不使用 `paths-ignore`，因为它可能让已配置的 required check
一直 pending。稳定门禁采用相同的 repository-identity 决策：只有同仓库 `CI/**` pull request
可以报告有意去重；fork 或 identity 缺失时会 fail closed。

`healthcheck-published-image` 是 container job，published-image healthcheck 执行 job 与
build/test integration job 会在 `ghcr.io/<owner>/<repo>/photospider-ci:latest` 中运行；轻量路由与
结果门禁仍在 `ubuntu-latest` 上运行。Checkout 后，published container 中唯一的
`Trust checked-out workspace` step 会绑定 `shell: bash`，只把精确的 `$GITHUB_WORKSPACE` 加入
该 job 持久的 global `safe.directory`，并以只读方式校验 `HEAD^{commit}`。它既不会配置
`safe.directory=*`，也不会执行 checkout 得到的仓库脚本。该 trust boundary 先于两个条件
history fetch 与 `healthcheck.sh`，也覆盖两个 fetch 都不会运行的 `main` push 和
`workflow_dispatch` 路由，而不依赖 checkout 的临时 HOME 范围配置。`Fetch pull request base
history` 与 `Fetch CI branch main history` step 同样绑定 `shell: bash`，使各自的
`set -Eeuo pipefail` 前导命令无需依赖 container 默认 shell 即可正确执行。如果某项改动修改
image input，workflow 会构建
`photospider-ci:local`，并在该镜像中运行同一套仓库脚本，避免验证过程与镜像发布产生竞态。
对于 pull request，published-image 与 local-image healthcheck job 都会在各自 job 内从
base-repository URL 拉取目标分支，把 `CI_BASE_SHA` 校验为 event 的精确 base commit，并把该精确
SHA 作为 `CI_BASE_REF` 传入，不依赖 fork checkout 的 `origin`。对于每次 `CI/**` push，两条路径
都会改为在各自 job 内拉取并校验 `origin/main`，再把它作为 `CI_BASE_REF`，使静态检查范围在连续
push 之间始终从 `main` merge base 开始累计。因此，后续纯文档 push 无法隐藏更早的未格式化 C++
commit。普通 `main` push 则继续使用 `github.event.before` 作为增量 `CI_BASE_REF`。
Published-image 校验先于 `healthcheck.sh`；local-image 校验先于构建 head Dockerfile 与执行挂载
workspace。任何必需 fetch 或解析失败都会在脚本使用 fallback base 选择前停止。
`Dockerfile.ci` 会安装这些脚本所需的 C++ toolchain、CMake、OpenCV、yaml-cpp、
GTest、nlohmann-json、clang-format、Python 和 cpplint。
镜像 detector 不使用 Git status filter；healthcheck 静态范围清单则使用 `--diff-filter=d` 排除
无法交给 formatter/linter 的删除路径，同时保留 type change 与少见的非删除 status。两者都使用
NUL 分隔的 Git 输出与父 shell 可见的临时文件。因此 `git diff` 失败时，镜像检测或 healthcheck
静态范围检测会直接终止，不会输出假阴性路由。

当前维护的入口包括：

- `ci/scripts/healthcheck.sh`：执行 fail-closed changed-path 清单、diff、format、cpplint、
  build-smoke inventory 回归与两项长期 shell 回归。
- `ci/scripts/change_classification.sh` 与
  `ci/scripts/change_classification_test.sh`：执行 fail-closed 纯文档路由及其长期 event/path
  回归矩阵。
- `ci/scripts/ci_routing_test.sh`：精确锁定两份 canonical `protected-ci-paths.if` 表达式；执行真实
  stable-gate、fork-rejection 与 protected-path block；以 job/step 作用域锁定 published-image
  两个 history-fetch step 各自的 `shell: bash` 元数据；以 job/step 作用域锁定唯一的
  published-image workspace-trust step、精确且不含通配符的 global `safe.directory`、只读 HEAD
  校验，以及 checkout < trust < fetch/healthcheck 的顺序；校验 published/local job-scoped
  pull-request 精确 base、`CI/**` 累计 main 顺序、三路 `CI_BASE_REF` 精确源码路由、
  允许空集合的配置期预检、严格构建后 matrix job output、对空 output 安全的 `fromJSON`、
  full-CTest/fallback 路由、含换行路径 artifact，以及
  detector/reader/producer 失败传播。测试会在隔离 HOME/仓库中执行 production
  trust block，并要求所得 global trust 清单只包含该仓库；同时还会执行两份 production
  main-fetch block。隔离 Git 历史会证明累计 main 范围保留更早的 C++，而 event-before 范围只
  看到较晚的 docs 增量。本机源码/shell 锁定不模拟 GitHub expression evaluator、跨 UID
  dubious ownership 或托管 container runner。
- `ci/scripts/build_smoke_inventory.py` 及其 focused regression：严格解析 CTest JSON，生成确定性的
  严格或显式允许空集合 matrix，覆盖重复 label value、安全 artifact key、NUL 分隔名称、精确索引
  执行、在第二次 subprocess 前停止的 absent/disabled/commandless selection，以及真实配置期
  占位到构建后发现 fixture。
- `ci/scripts/integration_plan.sh`：执行允许空集合的精确 label 配置期预检，不输出权威 matrix。
- `ci/scripts/build_integrity.sh`：构建 default producer profile，包括 required-target/full build、
  严格构建后带标签 CTest inventory 校验和权威 matrix job output。
- `ci/scripts/ctest_full.sh`：在排除精确 `build-smoke` label 后运行主 CTest suite。
- `ci/scripts/integration_suite.sh`：顺序执行 integration 行为检查，同时运行每个构建后发现的
  build smoke、full CTest、CLI、propagation、plugin 和 scheduler 覆盖。

CI 源码清单与 exclusion list 必须描述维护中的测试和当前源码路径。迁移专用 harness 名称
不得作为永久 exclusion 保留，也不得被视为产品行为。GitHub job 状态和可下载 artifact 用于
报告远程 integration 行为。完整 workflow 和 artifact 下载边界记录在
`docs/CI/zh/github-actions.zh.md`。

架构演进目标不会在本测试文档中维护，而是记录在
`docs/roadmap/zh/Kernel-Evolution.zh.md`。每项实现变更分别定义与风险相称的验证和长期回归覆盖。
