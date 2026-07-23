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

`BUILD_TESTING` 只控制内部 test product 是否可用，不控制已安装 `photospider` archive 如何编译
Issue #72/#75 observation seam。Product source inventory 被拆为只编译一次的 common object，以及
`execution_service.cpp`、`graph_cache_service.cpp`、`graph_state_executor.cpp` 与
`kernel_compute.cpp` 的 production object。真实 archive 始终使用这四个 translation unit 的
production 形式，其中不存在
`PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING`、
`PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING`、
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING` 或
`PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING` 的 declaration、global、branch 或 symbol。Focused
test 会链接不安装的 `photospider_internal_test_product`；它复用同一批 common object，只以
deterministic seam 重新编译这四个 translation unit。没有 target 同时链接两个完整 archive，test
product 也不会进入 install 或 export set。Issue #75 probe declaration 是 source-tree-private 的
free function，因此该宏不会改变 production `ExecutionService` class definition 或 object layout。

`StaticProductConsumerSmoke` 会对 `BUILD_TESTING=ON` 与 `BUILD_TESTING=OFF` 两种 producer
configuration 强制执行这条边界。真实 product 安装后，Darwin 会先调用并验证
`xcrun --find llvm-nm`，然后依次回退到 PATH `llvm-nm` 与 PATH `nm`；非 Darwin 平台绝不会
调用 `xcrun`，只按上述顺序使用两个 PATH candidate。Canonical path 相同的 executable 只运行
一次。Candidate 只有在能启动、成功退出、产生 symbol，并暴露四个 production seam object 的
全部 defined anchor 时才可用；否则 smoke 会记录不含路径的 failure reason 并尝试下一项。没有
candidate 或全部 candidate 都不可用时必须 fail closed。第一个可用的完整 symbol table 是权威
结果，并用于拒绝任何 hook function/helper/global fragment；raw table 只在内存中参与该判定，
因此第一个可用表只要包含 forbidden symbol 就会直接使 verdict 失败，不能通过尝试后续 candidate
隐藏问题。保留的 scan observation 使用闭合且不含路径的 schema：稳定 `tool_source`、按顺序排列
的结构化 attempt reason、status 与聚合 line/anchor/prohibited count，以及只以受控 symbol token
为 key 的 count。它不会保留 tool/archive/object/build/install/workspace path、raw symbol line、
captured stdout/stderr 或环境 `PATH`。若聚合 package behavior 失败，JSON diagnostic 只输出 failed
check label、command status 与该 sanitized scan observation 的白名单投影，不再序列化完整的临时
observations。Smoke 也会拒绝已安装的 test product archive、已导出的 test target 或已导出的内部
seam definition。该测试继续属于带 label 的 `build-smoke`；普通完整 CTest selection 不会让
package construction 混入 runtime-test ownership。

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
Client lifecycle symbol，并链接一个仅用于引用的分支，以精确且唯一的 inventory 覆盖全部 60 个
typed Client call 与全部 58 个非析构 Host virtual。Package 检查还要求 IPC archive 与精确的三个
header surface，导出的
IPC link interface 只允许 `Threads::Threads`；header 正向只允许当前 C++ standard-library include
与已安装的 `photospider/` public include，并拒绝 raw JSON、socket address/descriptor、file
identity、file mapping 与 backend declaration。这是门禁实际保证的精确边界，不声称穷举全部
可能的 POSIX 拼写。在禁用 backend discovery 时，
`COMPONENTS ipc_client OPTIONAL_COMPONENTS embedded` 会只找到 `ipc_client` 并成功；unknown
optional component 会保持 not-found，而不会使 package 无效。

相同 smoke 还会独立 configure 一个只请求 `COMPONENTS policy_sdk` 的 C11 project，针对
`Photospider::policy_sdk` 构建纯 C ABI-v1 policy DSO，并拒绝 OpenCV、yaml-cpp 或 Threads 泄漏。
生成的源码会探测精确 policy ABI constant 与 layout。外部 embedded consumer 随后会加载该已安装
policy DSO 与一个已安装 operation DSO，配置 policy/execution default，验证其 public snapshot，
并通过两种 extension 完成 compute。任何生成的 consumer 都不会获得 source-tree include 目录。

长期 `IpcDisabledInstallSmoke` 会用
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
在不启动 configure、build 或 install 的情况下验证 cache 到 child argv 的传播。同一进程还会向
static-product driver 的 production archive-symbol helper 注入 executable lookup、validation 与
captured-command callback；它会在不改变进程 PATH、也不取代真实 installed archive scan 的前提下，
锁定 Darwin xcrun-first fallback、非 Darwin 独立性、全部 candidate failure 与 canonical path
去重。CMake 注册该 safety test 时，还会传入当前 build tree、CTest executable、configuration 与
Python launcher。测试随后通过 `ctest --show-only=json-v1` 和生产 inventory parser 查询该 build
tree，并要求三个
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

CLI/Host Doxygen AST 工具是长期手工开发工具，不是测试。修改相应声明、定义、异常契约或 target
source closure 时，应显式运行：

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-cli-host-doxygen
```

CLI/Host 审计将
`apps/graph_cli/src/cli_config.cpp::apply_cli_policy_execution_defaults` 视为 policy/execution default
的 canonical 定义，并在该 translation unit 中验证其完整 Doxygen。工具还会审计
`load_configured_policy_plugins`、`run_graph_cli`、根 CLI 的 resource-exhaustion policy、
temporary-then-commit configuration parsing，以及完整的 CLI/benchmark broad-catch catalog。每个
broad catch 都必须在同一条 chain 上由更早的精确 `std::bad_alloc` rethrow 保护。

该文件可以留在 primary repository，因为本文定义了它的长期手工职责；它必须始终不进入 CTest
与 GitHub CI。其 `--out` 目录是仓库外、可丢弃的临时工作目录，不得成为长期 result tree。
Issue 专属 replay、provenance、helper 和 output artifact 既不得进入 primary
repository，也不得作为 personal overlay 的长期内容保留。Clean primary clone、CMake 配置、
CTest inventory 和 CI script 都不能依赖个人开发内容。

验证应与风险成比例。实现期间只运行 scoped static check、受影响 build target 和 focused
regression。是否运行本机原生 clean configure、full build 或完整 CTest/JUnit，应只根据改动风险
决定，而不是常设要求。本机 workflow 源码、YAML 与 shell 检查只属于开发者 preflight；它们不模拟
托管 GitHub Actions runner。不要把 Docker 或本地 `linux/amd64` 模拟作为常规本地 preflight；
current-head GitHub Actions 仍是权威远程 integration 环境。

## CLI option action 验证

`test_cli_policy_execution_config` 是注册到 CTest 的 integration binary，负责可复用
`run_graph_cli` option 边界和 policy/execution 配置。其 configuration case 会强制执行事务型
YAML/editor parsing、零至八 execution-worker 范围、精确 Host value，以及 Host rejection 时的
startup failure。它的 option case 使用完整的确定性 Host spy 与真实有序 parser。成功的
load/output 与短 traversal case 会保留 Host 返回的 session target，
并固定 `-t` 无参数 grammar。失败 case 要求 load、output、dependency-tree print、
traversal-order 与全缓存清理失败返回可恢复 exit code 2，且不得打印成功 footer 或进入 REPL。
Load case 还会捕获 REPL banner，证明唯一 action 失败时，失败结果优先于正常的 no-action
fallback。每次进程内调用都会在 configuration scan 与有序 action replay 前完整重新初始化平台
`getopt_long` 状态。全缓存清理 case 会先完成一次 option shape 不同的 traversal 调用，因此其
第二次调用能够证明：隐藏 parser 状态不能让后续 action 被重排或跳过。由于该 parser 状态是
进程全局的，可复用边界只支持串行的重复调用，而不支持并发调用；embedder 必须串行化每一次
完整的 `run_graph_cli` 调用。

Option replay 仍保持有序；另一个可恢复 action 失败前后已经成功的 action 可能产生可见效果，
该边界不提供多 action rollback transaction。尽管如此，只要任一 action 或 loaded-graph
前置条件失败，最终结果就必须是失败，而且该失败优先于显式 `--repl`。没有 option action 的
调用仍正常进入 REPL。可用以下命令运行聚焦边界：

```bash
cmake --build build --target test_cli_policy_execution_config -j
./build/tests/test_cli_policy_execution_config \
  --gtest_filter='CliOptionActions.*'
```

## Graph 文档错误矩阵验证

`test_graph_document_errors` 是注册到 CTest 的 integration binary，用于验证长期
Graph document ingestion 与 save 契约。它同时覆盖 public embedded Host 边界和直接
`GraphModel::replace_nodes` transaction 边界。Load/reload case 区分“省略 source path”与
“显式 source path”，对 I/O、YAML、schema、topology、lifecycle 与 unexpected failure
要求精确的 `GraphErrc` 分类，并证明 `std::bad_alloc` 仍保持 exception 语义。测试还要求：
initial load 失败不发布 session；reload 失败保留 prior Graph 的完整状态；成功 replacement
推进 topology generation 与 authoritative `GraphRevision`、重置 runtime state，并保持可重试。

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

## Revision-safe compute publication 验证

Issue #72 使用四个维护中的 test binary 负责长期 staged publication 边界。`test_compute_run`
验证 checked nonzero 强类型 `GraphInstanceId` 与 `GraphRevision`、不可复用 Graph identity、单调
mutation revision，以及精确 descriptor/snapshot provenance。`test_compute_service_split` 证明
`RealtimeProxyGraph` snapshot clone 是 deep isolation 边界，并且 complete prepared-state
publication 使用文档所述 no-throw swap path。

`test_kernel_contracts` 覆盖 product Kernel 边界。确定性 event gate 会把 operation execution 保持在
graph-state 之外，同时由 clear、same-label reload 或 same-topology cache clear 推进 live revision。
Parallel 与 sequential stale result 必须返回 `GraphErrc::ComputeError`、保留较新的 visible state，
并且不写入 deferred cache artifact。聚焦的 `PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING`
checkpoint 会在同一个 graph-state item 内完成 predicate validation 后暂停，证明 mutation 无法在
validation 与 publication 之间进入。同一 checkpoint 还证明：有效 RT proxy commit 保持可见，
即使独立校验的 HP sibling 之后变为 stale。该宏与
`PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING`、
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING` 一起，只存在于
`test_kernel_contracts` 与 `test_host_adapter` 使用的三 translation-unit test-product variant 中。
即使 `BUILD_TESTING=ON`，可安装 product 仍使用对应的 production object。

同一个 binary 还证明：私有 compute-request lane 会把 execution observation/route replacement 与
同一 Graph compute 串行化；accepted async work 在 caller future 被丢弃后仍继续存在；close 会在
graph-state 前排空 compute-request work，但不会拆除 process-owned route。这些竞态使用显式 gate
与有界 wait，不使用 timing sleep。每个被发现的 `test_kernel_contracts` case 还拥有 30 秒 CTest
timeout。

`test_disk_cache_diagnostic_concurrency` 是独立的长期多线程故障隔离 binary。Production
record/snapshot worker 会在 `GraphModel` clear、clone 与 staged publication 重复期间运行；另一个
case 通过只存在于 source tree 的 inline bridge，从两个参数顺序调用双 store exchange；确定性
allocation failure 则证明 snapshot copy 抛异常时会释放私有 scoped guard。每个被发现的 case 都
带有 `kernel-concurrency` label 与 20 秒 CTest timeout。如果锁回归阻止 worker recovery，CTest
会终止该专用进程；`std::future` 析构或 thread join 都不能继续占住 broad kernel-contract 进程，
也不必等到 CI job timeout。顺序执行的
`CacheSemantics.DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics` case 仍保留在
`test_kernel_contracts` 中，用于验证失败 reload 保留状态以及成功 clear/reload reset，同时不重复
deadlock probe。可用以下命令运行聚焦契约：

```bash
cmake --build build \
  --target test_compute_run test_compute_service_split test_kernel_contracts \
  test_disk_cache_diagnostic_concurrency -j
./build/tests/test_compute_run \
  --gtest_filter='GraphRevision.*:ComputeRunDescriptor.CapturesIdRevisionIntentQualityAndQosWithoutReuse'
./build/tests/test_compute_service_split \
  --gtest_filter='RealtimeProxyGraph.*'
./build/tests/test_kernel_contracts \
  --gtest_filter='ComputeContracts.ParallelStaleComputeCannotOverwriteGraphClear:ComputeContracts.SequentialStaleComputeCannotOverwriteGraphClear:ComputeContracts.ReloadedDocumentRejectsOlderSameLabelCompute:ComputeContracts.SameTopologyCacheClearRejectsStaleMemoryAndDiskPublication:ComputeContracts.CommitPredicateAndPublicationExcludeMutationToctou:ComputeContracts.RealtimeCommitSurvivesStaleHighPrecisionSibling:ComputeContracts.ExecutionObservationAndReplacementWaitForCompute:ComputeContracts.CloseWaitsForAcceptedAsyncComputeRequest:ComputeContracts.DroppedAsyncFutureRemainsOwnedUntilCloseDrain:CacheSemantics.DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics'
ctest --test-dir build --output-on-failure \
  -R '^DiskCacheDiagnosticConcurrency\.'
```

## Cooperative Run cancellation 验证

Issue #73 把 cancellation coverage 保留在长期维护的行为测试中，而不是 issue-specific replay
tool。`test_compute_run` 负责私有 Run source、稳定 first reason、注入式 monotonic deadline、
terminal-before-quiescent state、request fan-out，以及 cancellation/failure/commit arbitration。同一
binary 还覆盖 `ExecutionService` 在 active publication 前的 cancellation、精确 queued-Run purge、
dequeue/pre-callback race、non-preemptible callback drainage、被抑制的 dependent re-entry、peer
isolation 与精确 grant/root release。其 legacy `A -> B` case 证明：A 返回后发生 cancellation 时，
callback-owned unit 与仍由 plan 拥有的 unit 都恰好一次 retire，B 不会进入，也不会发布 staged
output；其配套 exception 分支还证明后续 provider failure 无法替换已经接受的 cancellation。

`test_kernel_contracts` 负责产品边界。确定性 commit hook 证明 claim 前 cancellation 不会发布任何
Graph/proxy/cache state，而 claim 后的 request 无法撤销成功 publication。RT/HP case 会在 HP
sibling 随后变 stale 时保留已提交 proxy；sequential case 证明 provider 返回后会在 staged
publication 前观察 cancellation；close case 则证明逻辑上已取消的 request 仍会在 Graph 销毁前
排空 running provider，并完成 public `ComputeError` translation。`test_compute_service_split` 会在
私有 `serial_debug` route 的 connected preflight 内触发 cancellation，并证明 dirty HP 与配对 HP/RT
request 都不会进入 parameter dependent 或 phase-two target work。

Public surface 不扩张仍由现有长期契约负责：`test_ipc_protocol` 固定精确的 60-method protocol-v2
inventory、拒绝 `compute.cancel`、round-trip 每个 version-two status label，并要求
`cancellable: false`；`test_compute_request_registry` 固定 daemon job snapshot；
`test_policy_registry` 固定事务型 ABI-v1 load rejection、由 binding 保持的 DSO lifetime 与首个
fault stability；`StaticProductConsumerSmoke` 则会编译并运行已安装的 58-virtual Host、60-call
Client、operation ABI v2 与纯 C policy ABI v1 consumer。这些测试不得为该私有变更新增
compatibility cancellation shim。

可用以下命令执行 focused cancellation boundary：

```bash
cmake --build build \
  --target test_compute_run test_compute_service_split \
  test_kernel_contracts test_ipc_protocol test_compute_request_registry \
  test_policy_registry -j
./build/tests/test_compute_run \
  --gtest_filter='ComputeRunCancellation.*:ComputeRunCommitArbiter.LinearizesCancellationBeforeOrAfterCommitClaim:ExecutionServiceCancellation.*'
./build/tests/test_compute_service_split \
  --gtest_filter='ComputeServiceCancellation.ConnectedPreflightCancellationSuppressesDirtyAndSiblingPublication'
./build/tests/test_kernel_contracts \
  --gtest_filter='ComputeContracts.SequentialCancellationAfterProviderReturnSuppressesPublication:ComputeContracts.CancellationBeforeCommitClaimSuppressesPublication:ComputeContracts.CancellationAfterCommitClaimPreservesPublication:ComputeContracts.RealtimeCommitSurvivesStaleHighPrecisionSibling:ComputeContracts.CancelledComputeStillDrainsBeforeGraphClose'
./build/tests/test_ipc_protocol \
  --gtest_filter='ProtocolContract.AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods:EnumCodec.RoundTripsEveryDefinedVersionTwoLabel:HostRoutedGraphStateProtocolTest.ComputeLifecyclePreservesEveryTypedHostRequestFieldAndStableShapes'
./build/tests/test_compute_request_registry \
  --gtest_filter='ComputeRequestRegistrySubmission.PublishesQueuedCommitSnapshot'
./build/tests/test_policy_registry
```

## Latest-wins supersession 验证

Issue #74 把 latest-wins 与 realtime-group coverage 保留在长期维护的行为测试中。
`test_compute_supersession` 负责缺失/显式 HP 的 canonical key 等价性、checked nonzero generation
overflow、compute lane 精确 64 个总单元的 admission、persistent ticket FIFO/wake 行为、并发
same-key ticket adoption、跨 target/intent/Graph isolation、close retirement、确定性的 18,000 与
36,000 次 publication storm，以及 `RunGroup` cancellation/aggregate 规则。Group case 会区分
request-level accepted reason 与真正赢得开放 child arbiter 的 reason：两个 child 都成功后的迟到
Superseded 或 ExplicitRequest 不能替换 aggregate success；真正赢得取消时，第一个 reason 会在
failure priority 之下保持稳定。CMake 会通过 CTest 发现全部 15 个 case，每个 case 的 timeout 为
60 秒。Stress case 会断言一个 ticket、一个 logical active owner、至多一个 pending owner、被替换
owner 的精确 settlement，并且只有最终 current generation 保持 commit eligibility；它们不创建
background runner，也不依赖 timing sleep。

`test_kernel_contracts` 负责产品边界。它证明缺失 intent 与显式 HP 共用一个 key、最新 work 失败
不会恢复更旧的 prepared commit、已经提交的旧 output 保持可见，以及 RT publication 前后发生的
realtime supersession 都会拒绝旧 HP sibling，同时保留有效的旧 proxy。额外的 post-commit
checkpoint 会在两个 child 都成功且可见 publication 完成后、group aggregation 前阻塞旧 realtime
caller；较新 generation publication 会记录 Superseded，但不能改变旧 caller 的成功结果。
`test_compute_run` 覆盖不可变 supersession identity，以及 child-local 与 group-wide cancellation。现有
`test_compute_service_split`、`test_host_adapter` 与 `test_bad_alloc_boundaries` 继续作为 service、Host
lifecycle 与 allocation-failure 边界的 focused regression companion。

可用以下命令执行 focused supersession boundary：

```bash
cmake --build build \
  --target test_compute_supersession test_kernel_contracts test_compute_run \
  test_compute_service_split test_host_adapter test_bad_alloc_boundaries -j
./build/tests/test_compute_supersession
./build/tests/test_kernel_contracts
./build/tests/test_compute_run
./build/tests/test_compute_service_split
./build/tests/test_host_adapter
./build/tests/test_bad_alloc_boundaries
ctest --test-dir build --output-on-failure \
  -R '^(SupersessionIdentity|GraphStateExecutorContinuation|ComputeRequestCoordinator|ComputeRequestCoordinatorStorm|RunGroup)\.'
```

## Policy generation 与私有 execution 验证

Issue #75 把 policy-generation 与私有 route coverage 保留在长期维护的行为测试中。
`test_policy_registry` 负责精确 built-in 与 class support、missing API 或 ABI mismatch 的事务型
rejection、registry unload 期间 active binding/DSO lifetime，以及一个 binding generation 的首个
fault stability。`test_resource_admission` 负责精确封闭的
`cpu`/`gpu_pipeline`/`serial_debug` route vocabulary、worker-limit rollback、每个 Host composition
一个固定 pool，以及 validation-first session route replacement。`test_compute_run` 中的
`ExecutionServicePolicy.*` case 继续负责 Host 编写的 cost、class/frontier/fairness、aging、
headroom、three-to-one progress、dependent re-entry、saturation，以及 reserved start 期间的精确
grant release。

`test_physical_execution_routes` 负责 allocation-free route/lane state：CPU/Metal overlap、Metal
single-flight、serial worker-zero single-flight、shutdown rejection 与 committed-work drainage。
`test_policy_execution` 使用 deterministic fake-Metal Host，证明规范的逐 route device inventory、
Run 发布前拒绝、彼此独立的固定 CPU/GPU worker、Metal exception publication/recovery、route
reuse、cancellation，以及不会产生 candidate/version ABA 或 grant leak 的 reserved-start rollback。
它还证明：grant-blocked high-priority Run A 不能饿死较低优先级的独立 Run B；A 的 ready entry
随后恰好执行一次；仅一个 candidate 被阻塞时 policy-selection retry 有界，且 cancellation 会
唤醒 worker。

reserved-start rollback probe 是只编译进不安装 test product 的固定大小 atomic state。Issue #75
probe macro 不改变 production class definition 或 layout，production object 不包含 reserved-start-
probe observer typedef、object field、callback、worker hot-path runtime branch、helper global 或
symbol。该声明只限定于这项 probe；既有 initial-submission storage observer 属于 baseline behavior，
本阶段既不移除它，也不承诺迁移它。
`test_compute_run` 中的
`Issue75DeviceRouting.*` 证明 full HP、dirty HP/RT 与 connected preflight 会冻结选中的 Metal
implementation/device，并在 Metal 不存在时使用 CPU fallback。

`test_cli_policy_execution_config` 固定事务型 policy/execution config parsing 与精确 Host
application。`test_host_adapter` 会加载真实 operation ABI-v2 与纯 C policy ABI-v1 fixture，配置两种
extension、验证其 snapshot，并通过私有 CPU route 完成 compute。`GraphCliPluginComputeSmoke`
会通过真实 REPL 重复这条纵向路径。`test_ipc_protocol` 与 `test_ipc_daemon` 负责 protocol-v2
routing、process-owned policy state、会改变 generation 的 replacement、scan 与共享 execution
default。`StaticProductConsumerSmoke` 会独立构建已安装的 C11 policy DSO 与 C++ operation DSO，
再执行同一条 external-consumer path。

Installed Host、CLI 与 IPC protocol-v2 surface 仍不暴露 cancellation command。IPC 继续拒绝
`compute.cancel` 并发布 `cancellable: false`；supersession 仍是私有 embedded-kernel 行为，不是
新的 public control surface。拥有 worker 的 scheduler ABI 不再有 compatibility consumer。

可用以下命令运行 focused policy/execution boundary：

```bash
cmake --build build \
  --target test_policy_registry test_policy_execution \
  test_physical_execution_routes test_compute_run test_resource_admission \
  test_cli_policy_execution_config test_host_adapter test_ipc_protocol \
  test_ipc_daemon graph_cli -j
./build/tests/test_policy_registry
./build/tests/test_policy_execution
./build/tests/test_physical_execution_routes
./build/tests/test_compute_run --gtest_filter='Issue75DeviceRouting.*'
./build/tests/test_resource_admission
./build/tests/test_cli_policy_execution_config \
  --gtest_filter='CliPolicyExecutionConfigParsing.*:CliPolicyExecutionConfigApply.*'
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.PolicyScanAndOperationPluginUseStatusValues:EmbeddedHostAdapter.ExternalOperationAndPolicyPluginsDriveParallelCompute'
./build/tests/test_ipc_protocol \
  --gtest_filter='ProtocolContract.AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods:HostRoutedGraphStateProtocolTest.PolicyAndExecution*:ClientExecutionDefaults.*'
./build/tests/test_ipc_daemon \
  --gtest_filter='IpcDaemonExecution.*:IpcDaemonPolicy.*'
ctest --test-dir build --output-on-failure \
  -R '^(GraphCliPluginComputeSmoke|StaticProductConsumerSmoke)$'
```

以下 focused companion regression 负责其余边界：

- `test_kernel_contracts` 驱动真实 `GraphIOService` stream 进入 post-write、post-flush 与
  post-close failure state。每个 phase 都必须返回 `GraphErrc::Io`；已创建的 destination 证明
  文档所述 non-atomic post-open 行为。
- `test_resource_ledger` 证明 checked vector arithmetic、当前五个维度各自的 saturation 与 exact
  recovery、atomic mixed-vector 与 pair admission、bounded child grant、deferred parent release、
  move-only token contract，以及并发无 overcommit 行为。
- `test_resource_admission` 证明精确私有 route vocabulary、worker-limit rollback、每个 Host 一个固定
  pool 且不同 Host composition 彼此独立，以及 validation-first session route replacement 会在无效
  candidate 后保留先前复制的 route。
- `test_compute_run` 会记录完整的 action/node/worker/epoch tuple。它证明两个复用 local task id
  zero 的并发 Run 只会向各自 Host 交付匹配的 Run/node epoch；cleanup 会在每条 assertion
  路径释放被阻塞的第一个 Run，使序列化回归以测试失败终止而不是挂起。Realtime Full HP 与
  Interactive RT child 共享同一个物理 Host 和 local task id zero，但不同的 trace-node marker
  会把每个 Host event 映射到对应 epoch，以及 callback 保留的 descriptor/task identity。
  该 realtime case 有意直接测试 `ExecutionService`：worker loop 的 Host/epoch 选择和 callback
  保留的 identity 可在这一边界观察，无需增加仅供测试的 GraphRuntime hook。Direct service
  case 还覆盖 retained Host memory、scratch、ready entry 与 ready byte 的 whole-vector rejection
  和 recovery、checked-overflow rejection、并发 Run 的 shared CPU admission、initial
  ready-store backpressure 与 priority ordering、dependent re-entry backpressure，以及 success
  或 failure 后的 exact root release。
- `test_ipc_protocol` 证明精确 Graph status 传递、mutation 只调用一次，以及 failed load 后
  daemon session-name rollback。
- `test_ipc_daemon` 证明真实 transport 精确返回 save `NotFound` 与 `Io`，destination failure 后
  remotely owned graph 仍可 inspect，并接受随后成功的 save。

可用以下命令执行 focused validation：

```bash
cmake --build build --target test_graph_document_errors test_host_adapter \
  test_kernel_contracts test_resource_ledger test_resource_admission \
  test_compute_run test_ipc_protocol test_ipc_daemon -j
./build/tests/test_graph_document_errors
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.*Reload*'
./build/tests/test_kernel_contracts \
  --gtest_filter='GraphIoContract.Save*'
./build/tests/test_resource_ledger
./build/tests/test_resource_admission \
  --gtest_filter='EmbeddedHostExecutionConfiguration.*'
./build/tests/test_compute_run \
  --gtest_filter='ExecutionService.*'
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

## Graph close 与 process shutdown 验证

Issue #76 会把 lifecycle correctness 保留在长期维护的行为测试中，而不是 migration scan。
`test_run_lifecycle_registry` 负责 Graph registration、candidate rollback/install race、原子
standalone/realtime-bundle admission、Graph-close isolation、process shutdown 与精确 final
unregistration。`test_execution_lifecycle_telemetry` 负责 schema-v1 固定 record、65,536-entry ring、
1..4,096 page bound、atomic cut、cursor gap/drop/saturation 语义、全部 15 个 counter、全部六种
physical counter selector，以及最终 `ServiceStopped` zero-counter event。

既有产品边界 target 承担 integration ownership：

- `test_compute_run`、`test_compute_service_split` 与 `test_kernel_contracts` 覆盖 full、dirty、
  preflight、no-op、realtime child、admission race、visible commit、精确 finalization 与无关 Graph
  行为。
- `test_resource_ledger` 与 `test_policy_execution` 覆盖 root/child 精确释放、
  ready/callback/policy/binding counter、route drainage、同一 service 的 worker/policy-callback
  shutdown rejection、跨 service shutdown、重复 shutdown 与最终 counter/event 顺序。
- `test_host_adapter` 覆盖合并的 direct Host close、marker 后 `NotFound`、close isolation、lane
  retirement 顺序与唯一 composition-root shutdown。
- `test_compute_request_registry`、`test_ipc_protocol`、`test_ipc_host` 与 `test_ipc_daemon` 覆盖
  预分配 daemon close generation、只允许 invocation 前的 `HostCloseNotStarted`、恰好一次 Host
  call、丢失 response 后不 replay/reopen、迟到 `NotFound`、Client/IPC Host 仅销毁本地状态、已接受
  job drainage、signal shutdown 与 Host lifetime。

可用以下命令执行 focused lifecycle boundary：

```bash
cmake --build build --target test_run_lifecycle_registry \
  test_execution_lifecycle_telemetry test_compute_run \
  test_compute_service_split test_kernel_contracts test_resource_ledger \
  test_policy_execution test_host_adapter test_compute_request_registry \
  test_ipc_protocol test_ipc_host test_ipc_daemon -j
./build/tests/test_run_lifecycle_registry
./build/tests/test_execution_lifecycle_telemetry
./build/tests/test_compute_run
./build/tests/test_compute_service_split
./build/tests/test_kernel_contracts
./build/tests/test_resource_ledger
./build/tests/test_policy_execution
./build/tests/test_host_adapter
./build/tests/test_compute_request_registry
./build/tests/test_ipc_protocol
./build/tests/test_ipc_host
./build/tests/test_ipc_daemon
```

最终 delivery pass 会执行一次 clean native configure、一次 full build、一次排除精确
`build-smoke` label 的 ordinary CTest/JUnit，然后严格发现并独立运行每个 post-build build-smoke
entry。它不会把 lifecycle provenance、stale-term search 或 source-quality audit 注册为产品测试。

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
因此 provider-aware broad suite gate 为关闭。Driver 会校验精确 CMake cache 画像，构建上述
provider-independent focused binary 与 stdlib-only fixture，并额外构建专用 disk-cache
diagnostic concurrency binary，再查询机器可读的 CTest inventory。该 inventory 必须精确包含
`DependencyDisabledInstallSmoke`、
`OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores` 与三个
`DiskCacheDiagnosticConcurrency.*` case；每个 concurrency case 必须仅保留
`kernel-concurrency` label 与 20 秒 timeout。不得残留任何依赖 provider 的 broad test。
Driver 随后通过 CTest 运行 optional-provider case 与全部三个 concurrency case。禁用
profile 要求依赖中立
analyzer/math operation 仍被 seed、OpenCV-backed operation key 不存在，并要求 replacement
provider 能发布、执行且完整退役其 resize key。该临时 build 是长期 product configuration
检查；它把命令与结果写入 CTest，不保留逐次运行报告。当前阶段禁用的是 operation provider，
不是彼此独立的 OpenCV codec、normalization、adapter 或 embedded-product 依赖。

OpenCV-provider 与注入式 codec 两个嵌套 build driver 都从
`cmake_build_smoke_support.py` 导入同一份破坏性 work-tree helper。移除临时目录前，该 helper
要求 work 拼写非空且为绝对路径，并拒绝 parent traversal、repository、任一 repository
ancestor、filesystem root，以及最终 work 路径或任一现存 parent component 中的任意不受信
symlink。在 Darwin 上，它只识别一项平台拥有的别名：
`lstat("/tmp")` 必须报告 root-owned symlink，严格 canonical resolution 必须精确等于
`/private/tmp`，且 `lstat("/private/tmp")` 必须报告 root-owned directory。只有这些条件全部
满足时，开头的 `/tmp` component 才会改写为物理 `/private/tmp`；系统临时 root 本身仍受保护，
其后每个 component 仍逐项接受 `lstat` 检查。Linux 的普通 `/tmp` 保持普通路径；非 Darwin、
非 root-owned、目标错误、用户控制、中间或 leaf symlink 都不会得到特殊信任。

该规范化必须位于 driver 边界，因为 macOS 上的 CMake 可能把物理选择的
`/private/tmp/...` binary directory 以 `/tmp/...` 序列化到 `${CMAKE_BINARY_DIR}` 与生成的
CTest command。这样，无需改写原始 CTest registration，也无需放宽任意 symlink 拒绝，raw CTest
注册仍可执行。除此之外，canonical resolution 只用于受保护位置比较。Recursive removal 前会
立即重复完整检查；删除函数接收受信别名对应的物理拼写，或不含 symlink 的原始拼写。Recursive
removal 失败会原样传播，`lstat` 风格 postcondition 还会确认目录或 dangling link 都没有残留。
Check/delete 序列不是跨平台原子 filesystem transaction，因此这些 driver 只接受由 caller
独占、且 component 不会被并发替换的临时 subtree。

`OpenCvOperationProviderBuildSmokeSafety` 只针对 disposable temporary root 下的 synthetic
repository、ancestor 和无关 symlink target，验证这些破坏性 guard、失败传播和 postcondition。
它会注入标量形式的 Darwin owner/type/target fact 与 synthetic logical-to-physical mapping，
因此每个平台都能覆盖受信别名 positive case，而无需创建或替换 `/tmp`。它还会锁定两个真实
consumer module 都使用公共 remover。其 final-symlink、symlinked-parent 与规范化后 symlink
case 要求每个无关 target 和 marker 都存活；测试绝不会把真实 checkout 或其 parent 传给
remover。Driver 还会读取嵌套 `CMakeCache.txt`：非空的
`CMAKE_CONFIGURATION_TYPES` 选择 `tests/<config>/`，single-config cache 则必须包含与请求完全
一致的 `CMAKE_BUILD_TYPE`。缺失或互相矛盾的 cache state 会显式失败；safety regression 会在
不依赖 host platform 的情况下覆盖两种 layout。它是快速的普通完整 CTest regression，会在进程内
import 并调用 driver helper；只有 `OpenCvOperationProviderDisabledBuild` 会启动 child
configure/build/CTest profile 并携带 `build-smoke` 标签。

## OpenCV Operation 并发验证

`test_opencv_operation_concurrency` 是注册到 CTest 的 integration binary，用于验证长期
operation-provider 与 benchmark Run-concurrency contract。它使用 Host-boundary record 与
有界 callback gate，而不是 elapsed-time threshold：

- `BenchmarkAutoThreadsPublishRunCapAndPreserveFixedPool` 证明自动选择只解析一次，进程
  execution 以 `worker_count=0` 准备，Graph load 发生在准备之后，并且解析后的非零 Run cap
  同时到达 Host compute request 与 benchmark result。
- `BenchmarkRunAllSharesPoolAndPreservesMixedSessionCaps` 证明 enabled 的 `1`、`2` 与自动
  session 共用一次准备，并保留不同 compute cap；disabled session 的越界数值 thread 值不会
  接受范围校验或执行，enabled 的无效 session 会被诊断并跳过。
- `BenchmarkProcessPreparationFailureRetainsDiagnosticAndCanRetry` 证明进程准备失败发生在
  Graph load 前、保留 Host diagnostic，并让 once-only preparation 可重试。
- `BenchmarkThreadsCapCallbacksOnOneFixedExecutionPool` 会在一个显式固定的八 lane Host pool
  上，对自动和显式 `1/2/4/8` Run cap 运行真实 `BenchmarkService`、Graph load 与已注册
  callback 路径。它要求达到 cap 大小的精确 callback overlap，并拒绝 cap-plus-one callback。
- `BenchmarkThreadsRejectOutOfDomainValuesBeforeGraphLoad` 要求负数与大于八的 Run-cap request
  在发布 Graph session 前失败。
- `HostComputeSurfacesRejectZeroMaximumParallelismAsInvalidParameter` 要求显式为零的 public
  Run cap 在同步、异步与 image compute 上都以 `GraphErrc::InvalidParameter` 失败。
- `IpcHostDispatch.MapsEveryCurrentHostVirtualWithoutFallback` 与
  `IpcHostCompute.RejectsZeroMaximumParallelismBeforeTransport` 证明 IPC Host 会通过三种
  compute convenience 保留正 Run cap，并在 transport 前以 public Graph error domain 拒绝零。
- `BuiltinCurveCallbacksReachRequestedWorkerConcurrency` 会在同一个固定八 lane pool 上、每个
  `1/2/4/8` Run cap 下重复三次 builtin tiled `curve_transform` 路径，并通过仅供测试的
  observer 要求精确 callback overlap。
- `BuiltinCurveOutputMatchesBetweenOneAndEightRunCaps` 会比较 public Host result 中打包后的
  pixel row，并要求同一个固定 pool 上单 cap 与八 cap 输出按位相同。

Observer 只存在于 `BUILD_TESTING` build，是 source tree 私有接口，绝不会安装。这些 case 证明
并发路径可达且输出确定，不承诺与机器无关的 speedup。

`opencv_operation_concurrency_benchmark` 是对应的长期手工 measurement tool，刻意不进入 CTest
或 CI。该工具会创建并清理可丢弃的临时 Graph root，通过真实 Host/benchmark/private-execution/builtin
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
`curve_transform` node，每个 Run cap 先执行两次 warmup，再采集七个 sample：

| Run cap | Median wall（ms） | Throughput（Mpix/s） | Speedup | 最大 in flight |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 27.450 | 611.188 | 1.000 | 1 |
| 2 | 19.567 | 857.433 | 1.403 | 2 |
| 4 | 15.688 | 1069.455 | 1.750 | 4 |
| 8 | 15.008 | 1117.910 | 1.829 | 8 |

原始 wall-time sample（单位：毫秒）为：

- cap 1：`27.694|27.134|27.450|27.183|27.869|27.250|28.035`
- cap 2：`19.021|19.567|19.774|19.497|19.435|20.427|20.997`
- cap 4：`16.059|15.688|15.992|15.727|15.600|14.692|14.649`
- cap 8：`16.436|16.610|16.512|15.008|14.859|14.064|14.760`

该快照证明所请求 Run cap 到达真实 callback 路径，并且测试机器能从移除外层串行化中获益。它不是
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
focused optional-provider GoogleTest、三个专用 disk-cache diagnostic concurrency case 与
`DependencyDisabledInstallSmoke`。

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
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|ExecutionTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientExecutionDefaults|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonExecution|IpcDaemonPolicy|IpcObservationFixtureDaemon|PhotospiderdCapabilityHelp|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
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

在 scheduler 向 policy/execution 过渡期间，configured CMake target help 是验证能力边界。可信
CI 只接受完整的旧标记集合（`test_scheduler`、`test_scheduler_plugin_loader` 和
`destroy_count_scheduler_plugin`），或完整的新标记集合（`test_policy_execution`、
`test_policy_registry` 和 `test_policy_plugin`），绝不接受不完整或混合集合。Build integrity
要求稳定的 `photospider_kernel` aggregate，而不是架构特定的实现 target，并且仍会构建完整
tree。Full CTest 继续作为普通已注册测试的权威入口；plugin、CLI、`execution-repeat` 与
sanitizer 分片会选择对应契约的断言。生成的 CLI 配置严格互斥：CI 不会把已删除的 scheduler key
传给 policy/execution revision，也不会引入产品兼容翻译。

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
  build-smoke inventory 回归、runtime-capability 回归与两项长期 routing shell 回归。
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
  full-CTest/fallback 路由、架构中性 `execution-repeat` 路由、含换行路径 artifact，以及
  detector/reader/producer 失败传播。测试会在隔离 HOME/仓库中执行 production
  trust block，并要求所得 global trust 清单只包含该仓库；同时还会执行两份 production
  main-fetch block。隔离 Git 历史会证明累计 main 范围保留更早的 C++，而 event-before 范围只
  看到较晚的 docs 增量。本机源码/shell 锁定不模拟 GitHub expression evaluator、跨 UID
  dubious ownership 或托管 container runner。
- `ci/scripts/build_smoke_inventory.py` 及其 focused regression：严格解析 CTest JSON，生成确定性的
  严格或显式允许空集合 matrix，覆盖重复 label value、安全 artifact key、NUL 分隔名称、精确索引
  执行、在第二次 subprocess 前停止的 absent/disabled/commandless selection，以及真实配置期
  占位到构建后发现 fixture。
- `ci/scripts/runtime_capability_test.sh`：覆盖精确 Make/Ninja target 解析、完整旧 profile 与
  policy/execution profile、不完整/混合/缺失清单的 fail-closed 行为、required-target 校验和
  互斥 CLI 配置。
- `ci/scripts/integration_plan.sh`：执行允许空集合的精确 label 配置期预检，不输出权威 matrix。
- `ci/scripts/build_integrity.sh`：构建 default producer profile，包括运行时契约检测、架构中性
  required-target/full build、严格构建后带标签 CTest inventory 校验和权威 matrix job output。
- `ci/scripts/ctest_full.sh`：在排除精确 `build-smoke` label 后运行主 CTest suite。
- `ci/scripts/integration_suite.sh`：顺序执行 integration 行为检查，同时运行每个构建后发现的
  build smoke、full CTest、CLI、propagation、plugin 和按能力选择的 execution 覆盖。

CI 源码清单与 exclusion list 必须描述维护中的测试和当前源码路径。迁移专用 harness 名称
不得作为永久 exclusion 保留，也不得被视为产品行为。GitHub job 状态和可下载 artifact 用于
报告远程 integration 行为。完整 workflow 和 artifact 下载边界记录在
`docs/CI/zh/github-actions.zh.md`。

架构演进目标不会在本测试文档中维护，而是记录在
`docs/roadmap/zh/Kernel-Evolution.zh.md`。每项实现变更分别定义与风险相称的验证和长期回归覆盖。
