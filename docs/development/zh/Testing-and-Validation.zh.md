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
Issue #72/#75/#76/#82 observation seam。Product source inventory 被拆为只编译一次的 common
object，以及 `compute_task_submission.cpp`、`dirty_update_executor.cpp`、
`execution_service.cpp`、`resource_demand_estimator.cpp`、
`graph_cache_service.cpp`、`graph_state_executor.cpp`、`kernel.cpp` 与
`kernel_compute.cpp` 的 production object。真实 archive 始终使用这八个 translation unit 的
production 形式，其中不存在
`PHOTOSPIDER_INTERNAL_DIRTY_UPDATE_TESTING`、
`PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING`、
`PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING`、
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING`、
`PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING` 或
`PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING` 的 observer/probe definition、global、branch 或
symbol。Focused
test 会链接不安装的 `photospider_internal_test_product`；它复用同一批 common object，只以
deterministic seam 重新编译这八个 translation unit。没有 target 同时链接两个完整 archive，test
product 也不会进入 install 或 export set。Issue #75 probe declaration 是 source-tree-private 的
free function，因此该宏不会改变 production `ExecutionService` class definition 或 object layout。
Issue #82 dirty post-plan observer 同样是 source-tree-private free function，并由仅存在于 test
product 的 thread-local state 支撑；它不会改变任何 production class definition 或 object layout。
Sequential lease admission observer、retained-operation-string charge observer 与精确
direct resource estimator 遵守同一边界：它们是 source-tree-private free function，只依赖
test-product-only atomic state 或不授予 authority 的计算。Direct gate predicate diagnostic
是只由 test product 定义的 private test-access method。其 declaration 会在 common object 与
seam object 共用的 source-private class definition 中保持 token-identical，但它不新增 object
state 或 installed surface，也不授予 authority。Production operation gate 与 estimator
不含 observer state 或 notification branch，任何 diagnostic 或 estimator 也不会授予 resource
或 gate ownership。
`test_compute_run`、`test_compute_service_split`、`test_host_adapter`、
`test_kernel_contracts` 与 `test_policy_execution` 构成该内部 archive 的完整 direct-consumer
集合。

`StaticProductConsumerSmoke` 会对 `BUILD_TESTING=ON` 与 `BUILD_TESTING=OFF` 两种 producer
configuration 强制执行这条边界。真实 product 安装到非系统临时 prefix 后，smoke 会复用
daemon capability driver，移除 LD/DYLD loader override，并执行 installed
`photospiderd --help`；这样，缺失可重定位 operation runtime 时不会因只检查文件存在而通过。
随后 Darwin 会先调用并验证
`xcrun --find llvm-nm`，然后依次回退到 PATH `llvm-nm` 与 PATH `nm`；非 Darwin 平台绝不会
调用 `xcrun`，只按上述顺序使用两个 PATH candidate。Canonical path 相同的 executable 只运行
一次。Candidate 只有在能启动、成功退出、产生 symbol，并暴露覆盖八个 production seam
object 的全部九个 required anchor 时才可用；否则 smoke 会记录不含路径的 failure reason
并尝试下一项。没有
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

`PhotospiderdInstallLayoutSmoke` 会另行配置三个隔离、dependency-disabled 的 producer tree。
它只构建 `photospiderd` target closure，随后安装已配置 package，分别覆盖嵌套相对目录
`libexec/photospider` 与 `lib64`、absolute libdir，以及配合相对 libdir 的 absolute
bindir。每个 case 都使用自身配置的 prefix，通过共享 capability driver 移除 loader override，
并执行 installed daemon。默认相对 `bin`/`lib` case 仍由 `StaticProductConsumerSmoke`
覆盖。全部 matrix build/install directory 与 absolute destination 都必须严格位于 CTest work
root 之下，并在成功或失败后清理。

配置后的 producer 还会把 `PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS` 序列化为
build-tree inventory，其中使用安装相对路径 `include/photospider/...`。写入任何 record 前，
兼容 CMake 3.16 的 writer 会拒绝反斜杠、CMake 可以表达的全部 ASCII C0 control（code 1 至 31）
与 DEL；diagnostic 只标识 allowlist 位置，不复述被拒绝字段。CMake string 无法表达 NUL，因此
reader 会独立拒绝伪造或被外部修改的 manifest 中包括 NUL 在内的全部 C0 control 以及 DEL。
LF 是唯一的 record separator；普通空格仍是合法的 POSIX path 数据。

Smoke 会拒绝缺失、空、重复、含 control、含反斜杠、非 canonical 或非 header 的 entry。
它先执行精确的 `PurePosixPath` 拼写/root/suffix 检查，再从该 inventory 生成 external consumer
的 include 清单，并要求已安装 include tree 与配置得到的路径集合完全相等。因此缺失文件与
意外文件都会在同一项精确比较中失败；driver 和文档均不维护第二份 public-header 数量，
未进入 allowlist 的 source-tree 文件也无法静默扩大 package surface。Safety regression 会让
带普通空格的路径经过真实 CMake writer 与 parser 往返，并证明 CMake 可表达的每个 control、
形似 parent 的反斜杠路径与普通反斜杠路径都会在序列化前失败。

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
operation-SDK-only factory 还会使用 installed `ValueBuilder`、`WriteLease`、
`BufferHandle`、`ReadLease`、runtime identity 与 ImageView 发布并读取 immutable CPU
DenseTensor Value。这证明 V-3 header 与 implementation symbol 在不发现 OpenCV、yaml-cpp
或 Threads 时仍完整。

长期 `IpcDisabledInstallSmoke` 会用
`PHOTOSPIDER_BUILD_IPC=OFF` 与 `BUILD_TESTING=OFF` 配置另一个 clean producer；它验证不会
advertise IPC build forwarder、installed header、archive、executable 或 exported target，required
`ipc_client` component discovery 会失败，同时 external default embedded Host consumer 仍能
link/run。Required unknown component 也会失败；optional disabled `ipc_client` 与 unknown
component 会保持 not-found 而不使 discovery 失败；省略 component 或请求 `embedded` 时继续
解析既有 backend dependency。

长期 `DependencyDisabledInstallSmoke` 会配置一个 OpenCV 与 YAML capability 均禁用的 clean
producer，禁用 OpenCV、yaml-cpp 与 OpenEXR 三个 package discovery，关闭 IPC，只启用
dependency-neutral test surface，
并构建真实 `photospider_kernel` aggregate、`photospider` product 与
`test_cpu_dense_tensor_image_operation`、`test_packed_fp4_dense_tensor` 与
`test_variable_sample_field_extensions`、`test_value_identity_across_dsos` binary。安装前，
它会在该真实 disabled producer 中运行全部 48 个 dense-image case、全部 4 个 packed FP4 case、
全部 17 个 provider-defined VariableSampleField case 与一个双 DSO identity case，包括
`register_core_operations -> OpRegistry -> NodeExecutor` invert path，以及 Value allocation
ownership、lease、signed-view 与 cache-identity 回归。它会验证派生的 provider/plugin/CLI
默认值，以及三类无效显式组合的精确诊断。
Clean install 后，它会拒绝 OpenCV header、target、export reference 与 yaml-cpp link 泄漏；
optional `operation_opencv` 保持 unavailable，required component 则失败。它还会对全部 installed
public header、library/archive、package config、CMake export、生成的 consumer link script，以及
consumer executable 的 dependency/symbol surface 执行大小写不敏感且区分 surface 的可选 provider
扫描。Mach-O 使用 `otool` 与 `nm`；ELF 使用 `readelf` 与 `nm`。有界 marker 覆盖
`OpenEXR`、`Imf` namespace/library family 与具名 transitive library，以及为 V-15 保留的
deep-scanline、deep-tiled、deep-codec、multipart 和 mixed-part 词汇。禁止使用宽泛的 `exr`、
`deep` 与无限定 `half` 子串扫描，因为它们会制造没有依据的 false positive。外部 consumer 会在
三个 discovery 均禁用时配置，链接并运行 `Photospider::photospider`，分配中立 image，并通过
installed package 使用 `ValueBuilder`、write/read lease、runtime identity、ImageView，以及
public FP4/quantization/Blocked/PackedDenseTensorView contract，
加载并关闭 empty Host session，并观察显式 YAML operation 返回 `GraphErrc::Io`。CI 只有在校验
producer cache identity、configuration、完整 capability profile 与已构建 dense integration
target 后才可复用该 producer。
同一个 external project 还会请求 `data_provider_sdk`，验证其 interface 不含 link dependency，
并根据安装后的 header 分别构建采用精确名称的 C11 与 C++17 v3 definition producer，再将二者
分别通过 `Photospider::operation_sdk` 链接进独立的 C++ Host consumer。每个 consumer 都会从
active snapshot 派生一份 Schema/Facet/Layout 三字段 manifest，发布 compact 与 repacked 两种
形式的有界三 buffer provider-defined Value，编译 output-sink/diagnostic/property layout
assertion，并执行纯 property、DataSpec 与 Region callback。每个 producer 都会从
callback-local storage 发出非空 BYTES property，使 installed Host 证明同步 copy-out，而不是
延迟 pointer access。
它会在 provider 可见和缺席时往返保留未知 descriptor/Layout byte 与完整或 metadata-only artifact
envelope，绝不为缺失的 ContentDigest 伪造值；同时检查 typed Descriptor、Content 与
StorageLayout digest，包括与 layout 无关的 content identity。Indexed read 与 provider-owner
lease 会在 unload 后继续保留精确 generation/module；active resolution 随后报告 MissingProvider，
retained Value traversal 仍保持有效，最终 owner/provider destroy 先于 module release。
任一 producer 或 consumer 都不会引入 source-tree include 或可选 provider dependency。

`OpenExrDeepProviderOptionOffSmoke` 负责更窄的 V-15 option 边界。它会在禁用 OpenCV、
yaml-cpp、OpenEXR discovery、graph CLI、IPC 与仓库 operation provider 的同时，用
`BUILD_TESTING=ON` 配置一个全新的 provider-OFF producer。Configure 会结合展开后的顶层 CMake
trace 与最终 cache，要求实际执行的 OpenEXR package lookup 和 discovery key 都为零。Driver
先完成 producer 的完整 build，再构建精确的 `test_variable_sample_field_extensions` target，随后
运行非空的 `^VariableSampleFieldExtensions\.` CTest selection，并排除 `build-smoke` 作为显式
递归保护。当前 selection 包含全部 17 个 V-14 case。

安装后，该 smoke 会盘点中立 public header、package Config/Targets 文件、build-tree native
product 与 installed native product。它依次采用 producer 传入的 `CMAKE_NM`、child toolchain 的
`CMAKE_NM` 或经过验证的平台 fallback；缺少 symbol inspector 时会 fail closed。Defined 与
undefined symbol surface 分别检查。Dynamic dependency 在 Darwin 使用 `otool`，在 ELF 使用
`readelf`，在 Windows 使用 `dumpbin /dependents` 或 `objdump -p`；Windows 不得以空 dependency
surface 通过。随后，一个中立 installed-package consumer 会执行真实的 verbose compile 与
executable link；在 executable 运行前，smoke 会扫描其 verbose output、
`compile_commands.json`、link script、response file、求值后的 imported-target property、native
symbol 与 dependency。Default 和 optional-component probe 必须继续可用；required absent
component 必须先以 Photospider 自有诊断失败，不得发现 OpenEXR。
`OpenExrDeepProviderInstallConsumerSmoke` 是启用态 companion：它安装显式 component，加载真实
module，解析两个 v3 export，校验 API table，调用 provider destruction，然后卸载 module。

生成的 clean consumer project 会维护一份有序的 CMake executable target list。同一份 list
负责创建 target、写出 configure-time 精确 target declaration，并通过 `file(GENERATE)` 提供
configuration-specific 的三字段
`target<TAB>$<TARGET_FILE_NAME:target><TAB>$<TARGET_FILE:target>` manifest。当前 profile
声明 `dependency_disabled_consumer`、`installed_c11_data_provider_consumer` 与
`installed_cpp17_data_provider_consumer`；新增另一个长期 consumer 时，只需扩展该 CMake list 与
配对源码列表，无需在 Python 中增加 target name 或 discovery branch。CMake 3.16 的 target generator
expression 是 native 拼写的权威来源，因为它描述所选 generator、target platform 与
configuration；Python 的 `os.name` 与 `sys.platform` 描述的是 interpreter host，不能据此推断
executable suffix。尤其是，在 Cygwin 或 MSYS2 下运行的 POSIX Python 完全可能合法收到以
`.exe` 结尾的 CMake target filename。

Reader 要求两份 manifest 都非空、唯一，并且 target 顺序完全相同。它会拒绝字段数量 malformed、
empty field、blank/comment record、无效 UTF-8、非结构性的 ASCII C0 control 或 DEL、非 canonical
target name，以及缺失、意外、重复或乱序的 target。Configured filename 不得包含 POSIX 或
Windows separator，不得为 `.` 或 `..`，必须唯一，并且只能精确等于 target name 或该名称加
`.exe`。这是全部可用的 native 拼写，因为生成的 consumer 不会定制 `OUTPUT_NAME`、prefix、
suffix 或 configuration postfix。这项 target-to-filename 绑定会阻止伪造的新字段选择任意
build-local executable。完整 target path 必须使用 canonical native 拼写、保持唯一、位于
consumer build root 或所选 configuration 目录，其 basename 与 CMake 声明的 filename 精确相等，
同时不得是 symlink，且必须指向可执行 regular file。两份 manifest 只有作为本次调用的可丢弃
configure/generate step 的产物才会被接受；即便如此，reader 仍会在任何 consumer 启动前完成
全部 record、identity、set、filename 与 path 校验。有效 consumer 随后按声明顺序运行；某个
consumer 运行失败时，后续 consumer 不会启动。

当所选 CMake generator 提供多个 configuration 时，smoke 会为 producer 与 consumer 使用同一个
generator，检查两侧的 `CMAKE_GENERATOR` 和 `CMAKE_CONFIGURATION_TYPES` cache 值，并从
configuration-specific manifest 的 `$<TARGET_FILE_NAME:...>` 与 `$<TARGET_FILE:...>` 字段
解析 consumer 可执行文件。

迁移 residue、phase 完成度、陈旧术语与源码布局检查是临时开发检查，不得注册到 CTest 或 CI。
Issue 专属 replay、provenance、helper 和 output artifact 既不得进入 primary repository，也不得
作为 personal overlay 的长期内容保留。长期 runtime、public-header 与 package-consumer 测试负责
维持产品边界。

## Build-smoke CI 分类

Build smoke 是一种长期维护的 CTest，其主要边界会委托执行 CMake configure/build/install、
exported package 或 external consumer build，或者专用 compile target。所有此类测试都携带精确且
稳定的 CTest 标签 `build-smoke`。如果 companion 只在进程内调用 driver 的 Python cleanup 或 layout
helper，或者配置一个不需要 compiler 的 manifest-generation fixture，并且没有委托执行 product
build、install、external consumer、compile target 或生成的 executable，它仍会作为普通 safety
regression 留在完整 CTest 分片。

当前带标签的 inventory 是
`DependencyDisabledInstallSmoke`、
`ImageArtifactCodecDependencyDisabledBuild`、
`IpcDisabledInstallSmoke`、
`OpenExrDeepProviderInstallConsumerSmoke`、
`OpenExrDeepProviderOptionOffSmoke`、
`OpenCvOperationProviderDisabledBuild`、
`PhotospiderdInstallLayoutSmoke`、
`PublicHeaderSelfContainment` 和
`StaticProductConsumerSmoke`。`PublicHeaderSelfContainment` 属于该分类，因为它的 CTest command
会构建专用 self-containment target；普通 GoogleTest binary、daemon/CLI process test 与
`PhotospiderdCapabilityHelp` 不会创建 child build，因此继续留在主 CTest 分片。
`OpenCvOperationProviderBuildSmokeSafety` 也留在该分片：它是 OpenCV build-smoke driver 的普通
safety regression。其唯一的 `project(... NONE)` fixture 会使用 imported executable 执行 production
manifest generator，但不会启动 compiler、product build、CTest、install、compile target 或生成的
executable。
`InstallConsumerArchitecturePropagationSafety` 同样留在主分片：它使用可丢弃的 producer cache
fixture 执行三个 install-consumer driver 的真实命令构造路径，同时替换 subprocess 执行，因此能
在不启动 product configure、build 或 install 的情况下验证 cache 到 child argv 的传播。其
data-driven command recorder 还会创建任意 0/1/N dependency-disabled target declaration、
target-file manifest、由 CMake 提供权威值的 target filename 与 fake executable。进程内 case
覆盖 Linux/macOS 的无后缀名称、Windows `.exe`，以及 POSIX Python 下 Cygwin/MSYS2 的 `.exe`
拼写。它们要求按序执行，并要求 empty、duplicate、missing/unexpected、malformed、含 control、
含 separator、reserved、foreign、filename/path drift、unsafe、noncanonical、unexpected-layout、
unbuilt、non-file 或 non-executable inventory record 在 runtime 前失败；build 与 consumer
failure 还会锁定 fail-fast 顺序。一项 compiler-free `project(... NONE)` fixture 会执行生成的
target validator 与两种 target filename/path expression；另一项 `cmake -P` fixture 会直接调用
production public-header writer。两者都不会启动 compiler、product build、install 或生成的
executable。
同一进程还会向 static-product driver 的 production archive-symbol helper 注入 executable lookup、
validation 与 captured-command callback；它会在不改变进程 PATH、也不取代真实 installed archive
scan 的前提下，锁定 Darwin xcrun-first fallback、非 Darwin 独立性、全部 candidate failure 与
canonical path 去重。CMake 注册该 safety test 时，还会传入当前 build tree、CMake 与 CTest
executable、configuration 与 Python launcher。测试随后通过 `ctest --show-only=json-v1` 和生产
inventory parser 查询该 build
tree，并要求三个
真实 smoke 遵循配置相关的精确集合：所有 profile 都必须各自只注册一次
`DependencyDisabledInstallSmoke` 与 `IpcDisabledInstallSmoke`；
`StaticProductConsumerSmoke` 只在 IPC enabled 时必须精确注册一次，在 IPC disabled 时必须缺席。
每个预期 entry 还必须保持 enabled、带正确 label，并以精确的 `python -B` driver path 开头。被
注释或处于 inactive CMake 分支中的源码不会生成 CTest entry，因此无法通过这项生成后 inventory
检查。该查询不会执行任何真实 smoke，也不会改变现有九项 build-smoke 分类。

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
`PhotospiderdCapabilityHelp`、`PhotospiderdInstallLayoutSmoke`、
`StaticProductConsumerSmoke`、`GraphCliOptionBadAlloc`、GoogleTest discovery
与 `PublicHeaderSelfContainment` 满足这一规则，
因为它们会执行或编译维护中的产品。Daemon help 测试通过 CMake script driver 运行当前
configuration 对应的真实 `photospiderd --help`，分别捕获 stdout 与 stderr，先要求进程结果是
数值零，再匹配稳定 capability sentence；启动失败与非零退出会得到不同诊断。
该 driver 会移除 loader override variable，并在 package 安装后复用，因此 build-tree 与
install-tree resolution 都会验证自身声明的 lookup path。
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
failure priority 之下保持稳定。CMake 会通过 CTest 发现全部 16 个 case，每个 case 的 timeout 为
60 秒。Stress case 会断言一个 ticket、一个 logical active owner、至多一个 pending owner、被替换
owner 的精确 settlement，并且只有最终 current generation 保持 commit eligibility；它们不创建
background runner，也不依赖 timing sleep。Current-observer case 会证明 accepted 较新
generation 在物理执行前推进 external freshness，而一个 prepared 但 born-stale 的旧
generation 不会发出 observer notification。

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
`test_policy_execution` 使用注入的 deterministic fake Metal executor，证明规范的 registry-derived
逐 route device inventory、Run 发布前拒绝、彼此独立的固定 CPU/GPU worker、精确 executor
entry、Metal exception publication/recovery、route reuse、cancellation，以及不会产生
candidate/version ABA 或 grant leak 的 reserved-start rollback。
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
`test_device_executor_registry` 不依赖 platform SDK，负责 fixed-slot validation、精确 dispatch、
借用 TLS context restoration、provider-exception identity 与复制的 diagnostics；其中多调用
case 证明 submission 与 serialized-entry 计数在成功及抛异常 callback 后都会单调推进。可移植
callback tests 证明：直接同 executor 递归会在 nested provider 或任一 diagnostics counter 推进
之前以稳定的 `std::logic_error` 被拒绝；外层 context 保持 current；后续 invocation 可以恢复；
不同 executor 则可以嵌套并恢复外层 context。在 Apple 上且仓库 operation plugin 已启用时，
`test_metal_device_executor` 会由两个受控线程直接驱动工厂创建的真实 registry。第一个 callback
保持活跃时，复制出的 diagnostics 必须稳定显示两次
submission、但只有一次 serialized entry；测试只在观察到该排队状态后才释放第一个 callback。
若 admission wait 被旁路，diagnostics 会直接显示两次 entry，测试无需 sleep、重叠观察窗口或
scheduler timing 假设即可确定失败。该测试还会先分配真实 texture 与 shared buffer 再抛出异常，
然后证明精确 provider exception identity、同线程 TLS 恢复、存活 allocation 为零、
queue/pipeline diagnostics 稳定、计数单调推进，并且同一个 executor 可以在后续非嵌套调用中
成功进入。另一个 threadsafe death-test child 会安装五秒 alarm，通过真实 registry 尝试同步的
同 executor callback 递归，并且只有在证明精确错误文本、nested counter/resource diagnostics
不变、外层 TLS context 保持、返回后 TLS 清空以及后续 invocation 成功后才退出。alarm 会把
此前的自死锁转化为有界测试失败，而不使用 detached thread 或制造生命周期竞态。验证该
watchdog path 后，测试会执行一次真实 CPU-to-Metal upload，并证明精确保留 revision 的 device
replica 进入 residency。V-9 还会证明 upload scratch 仅在 completion 后归还、persistent
memory 会跨 callback return 与 residency 保留、capacity-one eviction 会归还旧 lease，并且
最终 manager destruction 会归还最后一个 lease。极小的 Perlin device budget 会在首个
texture/buffer allocation 前拒绝完整 native heap-query plan。预算充足的路径随后让真实仓库
Perlin operation 通过同一个 `ExecutionService` 连续运行两次，并证明 queue 可用、两次
operation submission 与 executor entry、八个 invocation allocation 已退役、一条 pipeline 被
复用、asynchronous pending-Value readback 生成 CPU-owned output、使用专用 Metal worker id，
并且已结算的 Host 与 device reservation 都为零。Upload 与 download 都会在 command commit 前
审计 native `allocatedSize`。

V-8 与 V-9 在 `test_device_residency` 中的可移植 case 会固定 direct host-read 与 transfer
planning、精确 current completion publication、destination Ready 前的 late stale rejection、
pretracked current publication 对晚启动旧 Run admission 的拒绝、failed/discarded
nonpublication、不会消耗正确 admission 的 proper-subset identity rejection、
concurrent exact callback 与 duplicate-completion rejection。附加真实 memory-only lease 的
fake native owner 还会证明：creator/Run-equivalent release 不会提前归还 byte、residency eviction
只释放自己的 strong owner、外部 Value 副本会延长生命周期，而且 stale、rejected、cancelled
或复用 identity 不会 double-release，也不会消费另一个 allocation 的 authority。
`test_compute_run` 新增确定性 case，
覆盖 early fence callback 在原 grant 退役前保持 parked、executor lifetime 延长 Run settlement、
pending Value dependency deferral、无需等待 producer 即可退役 continuation 的 cancellation，以及
绝不释放 dependent work 的 typed stale failure。这些 case 使用 gate 和 future，不含 timing sleep。

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
  test_physical_execution_routes test_device_executor_registry \
  test_device_residency test_compute_run test_resource_ledger \
  test_resource_admission \
  test_cli_policy_execution_config test_host_adapter test_ipc_protocol \
  test_ipc_daemon graph_cli -j
./build/tests/test_policy_registry
./build/tests/test_policy_execution
./build/tests/test_physical_execution_routes
./build/tests/test_device_executor_registry
./build/tests/test_device_residency
./build/tests/test_resource_ledger
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
# Apple 且 PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=ON：
./build/tests/test_metal_device_executor
```

以下 focused companion regression 负责其余边界：

- `test_kernel_contracts` 驱动真实 `GraphIOService` stream 进入 post-write、post-flush 与
  post-close failure state。每个 phase 都必须返回 `GraphErrc::Io`；已创建的 destination 证明
  文档所述 non-atomic post-open 行为。
- `test_resource_ledger` 证明 checked Host/device vector arithmetic、当前五个 Host dimension
  各自的 saturation 与 exact recovery、CPU/重复 device configuration rejection、zero 与
  exact-boundary device plan、atomic memory-plus-scratch rejection、per-device isolation、
  same-device contention、plan-to-actual shrink、typed underplanning failure、拆分的
  memory/scratch lifetime、move-only authority、延迟 asynchronous release、bounded Host child
  grant、deferred Host parent release，以及并发无 overcommit 行为。
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

## Direct CPU operation authority 验证

Issue #82 将 scalar callback/metadata identity 与 direct dirty admission 保留在长期行为测试中。
`test_op_registry_m31` 会按两种顺序注册 monolithic HP 与 tiled HP sibling，调用两种 callback，
并要求每个选中 implementation 保留自身 identity 与完整 scheduling metadata。因此，后注册的
sibling 不能静默改写与先前 callback 配对使用的 metadata。

Task planning 与 runner case 会先注册 SpatialAligned monolithic sibling，再注册 device-tiled
RandomAccess sibling。它们要求 dependency ROI lowering、tile size、选中 callback 与 provider
input view 都消费同一个带 revision 的 route，不得退回通用 key-level metadata 查询。手工
`test_propagation` 工具同样会为请求的 HP 或 RT diagnostic route 过滤并保留精确 tiled
implementation。

`test_cpu_dense_tensor_image_operation` 还会为 TensorSlice 的 target-only 与
target-plus-upstream plan 冻结精确 core CPU route，再在 task population 前追加优先级更高的
same-key non-core GPU implementation。两个 case 都要求 dirty preparation 返回
`NoOperation`、provider entry 为零、lifecycle/gate/grant/reservation/ledger 残留为零，并恢复
core registry route。Guard-bypass 对照会继续通过真实 `HighPrecisionDirtyNodeExecutor` direct
provider lease 并进入 fake GPU provider，因此该回归不能只因测试停在 planning 而通过。

相邻的三项 route-context case 会在 TensorSlice planning 后改变 task-population device
inventory。只有 externally satisfied case 会作为 zero-work 完成 preparation，不比较此时已无关
的 frozen intent、device inventory 或 node route。Exact-cache case 会通过真实 Graph boundary
安装 complete 旧 HP output，但由于 TensorSlice 已被 dirty-selected，它仍保持 active；与
partial-active 对照一样，它必须在 fake GPU provider entry 或 execution authority 之前返回
`NoOperation`。测试不会自行删除 execution-order node。

`test_compute_service_split` 会独立验证外层 service boundary。真实 complete target cache 会一直
保持 exact 到 selection，但明确标脏的 target 与其 provider cone 仍会执行。相邻两项 case 从相同
exact planning observation 开始，再通过 internal test-product observer 删除 output 或缩减其
formal Region；三种状态都会保留并执行同一个 dirty provider cone。Complete 旧 cache 下的
post-plan registry replacement 仍必须作为 active route drift 失败，且两个 provider 均不能进入。
若用反向 mutant 让 exact cache 满足 dirty candidate，则 exact 对照与 Host ROI fixture 都会因
work 为空、旧 pixel 不变而失败。

相邻的 real-provider case 使用 sparse dirty chain
`A(dirty) -> B(externally satisfied, inactive) -> C(dirty)`。第一项要求只执行 C；带 shared
`A -> D(dirty)` 的对照要求执行 A、C 与 D，而 B 保持 inactive。反向 candidate-only-universe
mutant 会在第一项中错误执行 A 并失败，从而证明 demand traversal 必须保留 inactive connector 与
satisfied boundary。其他 planning case 继续应用普通 full-request cache cut、遵守
force-recache，并保持 RT work 可执行。

`test_host_adapter` 会先发布 exact complete HP output，再通过 public Host boundary 提交非 forced
dirty ROI。它必须执行 16 个 downstream tile 与一个 monolithic source task，把选中 pixel 从 3
更新为 11，把未选中的 pixel 保持为 3，并通过 Host snapshot 暴露局部 backward mapping 与
native PixelRect/tile geometry。恢复错误的 dirty-cache satisfaction 后，该 fixture 会报告零个
active task，且选中 pixel 保持旧值。

`test_compute_run` 会为 full-plan、dirty HP、dirty RT 与 connected-preflight 产品路径注册
heap-backed exclusive key。共享 string-payload estimator 证明实际 capacity 加一个终止符，
并证明 overflow rollback 具有 strong guarantee。Internal test product 还会报告每个实际
retained owner、该 owner 所复制 `std::string` 的 `capacity()`，以及该次计费前后的 checked
estimator total。Full-plan、dirty HP、dirty RT 与 connected-preflight case 会要求每个上报
delta 都等于实际 capacity 加一个终止符，并要求精确的 owner 数量。该比较独立于完整 admitted
vector；同一批 case 会另外要求相同 plan 在精确 retained capacity 下成功，并要求少一个 byte
在 provider entry 前拒绝且 ledger snapshot 为零。它们覆盖已计费的 plan/context constraint
allocation 移入唯一 submission 的 ownership transfer，不使用 migration-residue source scan。

Direct-lease gate 回归会取得一个 heap-backed key，在 acquisition 返回后原地修改 caller
仍存活的 allocation，并通过不授予 authority 的 test-product diagnostic 查询真实 gate
predicate。在 lease 退出前，原始 key 必须仍受阻，而不同 key 必须可启动；这证明
wait/start/finish 借用 lease-state 副本，而不是 caller。测试会在 cleanup 前恢复 caller
buffer，使错误实现也能确定性地完成 unwind。

`test_compute_service_split` 证明 nonparallel dirty HP、dirty RT 与 connected-parameter
preflight 会进入 physical worker 所使用的同一个 process-owned operation gate 与 resource
ledger。Cross-Graph case 覆盖 nonreentrancy、精确 implementation cap、相同/不同 exclusive key、
provider 进入前的 retained-memory 与 scratch rejection、cancellation/exception cleanup，以及
settlement 后成功重试。确定性 post-plan case 会在 active-operation revalidation 前替换 HP
implementation 或卸载 RT plugin。此时 standalone Run 或 realtime RunGroup 的逻辑生命周期会被
有意观测为可见；case 要求在 provider entry 与 operation/resource/physical admission 前以 typed
failure 停止，随后要求逻辑生命周期完成 settlement，不留下 callback、grant、root reservation、
gate 或 ledger 残留，并证明重试能够恢复。Externally satisfied sibling 会被有意忽略，因此
inactive registry 变化不能使原本有效的 active dirty target 失效。

另一项 cross-Graph case 会让两种 reentrant HP implementation 与两种 reentrant RT
implementation 使用不同 identity、无 identity cap 以及同一个 heap-backed key。第一个
provider 会在 dirty helper 已经返回 direct lease、helper-local constraints 已退出后保持阻塞。
HP 与 RT 的第二个 provider 都必须等到 lease 释放后才能进入，从而证明真实 helper 路径把 key
保留在 direct-lease state，而不是 helper stack。

同一个 binary 还负责两项正交的 sequential provider 边界回归。两个 Graph 都选择同一个已注册
callback identity，并通过 node role 参数区分 sequential 与 peer 行为。Metadata 声明
`maximum_parallelism=1`、一个 nonempty exclusive key，以及非零 retained/scratch demand。
在 physical route case 中，仅存在于 test product 的 observer 会报告精确的 operation-gate
denial。测试等待 admission rendezvous 或 provider 错误进入二选一事件，随后要求前者发生并排除
provider overlap。Provider 返回后，注入的 `FakeImageArtifactCodec` 会阻塞磁盘缓存持久化，
随后抛出 `GraphErrc::Io`；route-backed provider 必须在这段 Host 后处理仍被阻塞时进入、退出并
完成 settlement，resource snapshot 中不得留下 sequential grant。

Resource-capacity case 使用同一个 callback identity、cap 与 key，但采用第二个 direct
contender。仅存在于 test product 且不授予 authority 的 diagnostic 会复用 production
direct-lease envelope 计算，并把隔离 `ExecutionService` 的 CPU、retained-memory 与 scratch
上限精确设为一个 direct callback vector。它还会把 heap-backed key 与独立的 fixed-envelope
加 copied-capacity-plus-terminator 计算比较。精确 capacity 会成功；少一个 byte 的上限，以及
只容得下 capacity、容不下 terminator 的声明，都会在取得 gate/resource ownership 前拒绝并留下
零 snapshot。Contender 随后会在 provider 活跃时抵达被拒绝的 admission，并在 codec 释放前进入
和退出。把这项 capacity 检查保持为正交场景是有意设计：physical Run 会在 operation-gate
startability 前预留完整 root，因此该 root 不是一个 direct-lease vector。两项回归都不会新增
production 或 installable test hook。

Post-plan、admission-wait 与 retained-string observer、gate predicate diagnostic 以及
direct-resource diagnostic 只存在于不安装的 internal test product 中。
`StaticProductConsumerSmoke` 要求覆盖八个 seam object 的九个 production anchor，并拒绝
installed archive 中出现任何匹配的 state、setter、clearer、notification、helper 或
diagnostic symbol。

聚焦验证命令为：

```bash
cmake --build build --target test_op_registry_m31 test_compute_run \
  test_compute_service_split -j
./build/tests/test_op_registry_m31 \
  --gtest_filter='OpRegistryM31Test.ScalarSlotsStayAtomic*'
./build/tests/test_compute_run \
  --gtest_filter='OperationExecutionGate.DirectLeaseGateIgnoresCallerConstraintMutationAfterAcquisition:RetainedMemoryEstimator.StringPayloadChargesActualCapacityAndTerminatorAtomically:ExecutionServiceProductResources.FullPlanRejectsOneByteShortAndExecutesAtExactLimit:ExecutionServiceProductResources.DirtyHpAndRtUseExactSmallLargeSynchronizationInterval:ExecutionServiceProductResources.ConnectedPreflightUsesOneSharedUmbrellaAtExactThreshold'
./build/tests/test_compute_service_split \
  --gtest_filter='ComputeServiceSequentialAdmission.*:ComputeServiceDirectDirtyAdmission.*:ComputeServiceDirtyIdentity.*:ComputeServiceCancellation.NonparallelConnectedCancellationReleasesDirectAuthorityAndRecovers:ComputeServiceSplit.PreflightFailurePublishesNoHpCacheState'
```

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
  行为。`test_kernel_contracts` 还固定精确 close owner/joiner generation、抛异常 observer 的 claim
  消费，以及最终 name removal/success publication 的原子边界。其 same-name reload
  回归会在 graph-state completion 后暂停真实 calling-thread diagnostic store，不运行
  compensating clear，证明 replacement slot 不变，再释放旧 runtime 的最后一个 owner。
  另一个回归单独隔离迟到 old-runtime clear。一个真实 worker operation 会调用
  `Kernel::shutdown()`，证明精确 recoverable preflight 让 telemetry 保持 `Accepting`、
  generation 为零且 Graph publication 保持 open。watchdog death 回归则证明 publication
  gate 关闭后的注入 failure 会终止进程。
- `test_kernel_lifecycle_concurrency` 链接真实 production archive，在编译期拒绝 Kernel
  lifecycle observer macro，并重复并发 same-name publication、listing、direct close 与
  shutdown admission。静态 archive inspection 同时要求 close 与 shutdown product anchor，
  并拒绝全部 observer hook symbol。
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
  test_compute_service_split test_kernel_contracts \
  test_kernel_lifecycle_concurrency test_resource_ledger \
  test_policy_execution test_host_adapter test_compute_request_registry \
  test_ipc_protocol test_ipc_host test_ipc_daemon -j
./build/tests/test_run_lifecycle_registry
./build/tests/test_execution_lifecycle_telemetry
./build/tests/test_compute_run
./build/tests/test_compute_service_split
./build/tests/test_kernel_contracts
./build/tests/test_kernel_lifecycle_concurrency
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

## CPU DenseTensor、Packed FP4、Provider Extension、Region、ReadyFence 与 Transfer 验证

`test_cpu_dense_tensor_image_operation` 是已实现 V-2 至 V-12 边界的 provider-independent
integration binary。它的 48 个长期用例验证：

- copyable ReadyFence poll、queued non-inline wait、observer-local waiter cancellation、
  exactly-once Ready/Failed/ProducerCancelled settlement、typed failure retention 与
  dropped-completer cancellation，以及 pending 与 already-terminal wait 使用唯一 executor
  时，executor 会存活到 callback 完成；
- 使用确定性 C++17 mutex/condition-variable，在没有 sleep 或 timer 的情况下，验证 wait
  registration 与 terminal publication、cancellation 与 callback entry，以及 transfer-owner
  destruction 与 callback entry 的真实竞争、唯一 terminal settlement 与 callback 至多交付一次；
- pending Value metadata/identity observation、对 BufferHandle 与 checked-view payload
  access 的 typed rejection，以及 private producer 在 readable Ready publication 前撤权；
- 显式 fake-executor transfer enqueue、独立 allocation binding 与保留的逻辑 revision、
  byte-identical completion、唯一 executor 会存活到 destination 完成、无需阻塞 worker 的 chained
  readiness、保持不可读的 source failure/cancellation propagation，以及 transfer ownership
  被丢弃时只取消 destination；
- 显式注入的 CPU-to-Metal transfer、经过检查的 device-local binding、revision preservation、
  对“host-visible 但没有 host pointer”目标的拒绝、没有隐式 host read/readback、typed provider
  failure，以及同一 executor 上后续 transfer 成功恢复；
- malformed facet、stride、byte offset 与 exact-envelope rejection，包括受检的单轴/跨轴
  writable collision 与 overflow case，以及可接受的 padded、transposed 和 singleton-axis
  layout；
- exclusive builder write authority、seal revocation、retaining read-lease lifetime、
  BufferHandle subrange、process-local identity，以及非零 `AllocationIdentity` 不表示
  allocation liveness；
- 在 shared allocation 上受界限约束的正、零与负 immutable stride，以及彼此不同的 Value
  revision；
- immutable Value copy sharing、copy-like DenseTensorView/ImageView move，以及 lvalue/rvalue
  descriptor、layout 与 payload input 的 allocation 隔离；
- 正式 HP cache alias 保留、dirty reseal、replacement identity、disk reload identity 更新、
  cache path 不变、disk-save Value authority，以及 whole-read 与 regionless disk 边界对
  exact-partial HP state 的拒绝与清理；
- 精确 descriptor-only invert inference、直接复用 sealed input 与精确 result-revision
  publication；
- V-12 浮点矩阵覆盖 1/3/4/8/16 通道 FP32/FP64 图像与 rank-one 至 rank-five
  FP32/FP64 latent，包括具有真实 padding 的 rank-one stride、独立 active-byte/padding-sentinel
  oracle、ImageRect/TensorSlice merge、CPU/external/I-O 边界的精确保留，以及在 Pending
  publication、owner retention 或 provider callback 前拒绝 negative/zero-stride external
  transfer；
- padded multi-channel full 与 ImageRect execution、rank-four TensorSlice、Empty/Whole
  selection、dirty-plan-to-product staging、missing 或 partial intermediate parent
  recomputation、把 selected byte merge 到 existing complete output，以及仅在 Whole commit
  后提升为 reusable authority、callback-free target/upstream Region-route transfer 与
  pre-task-population mutation rejection、device-inventory drift 下由 production pruning
  得到的 externally satisfied no-work acceptance、exact-cache dirty 与 partial-active
  drift rejection；execute 返回 descriptor 与 inference 不一致的合法 Value 时，仍以
  `GraphErrc::ComputeError` 拒绝。

`test_region_contracts` 拥有 31 个长期 Region case，覆盖规范 Empty/Whole、key、interval、
normalization、rank-general TensorSlice、overflow-safe clipping/algebra、可表示的单轴与
Tensor-axis union、不可表示 multi-axis union rejection、显式 budget、typed failure、
checked ImageRect/PixelRect conversion、Region propagation、route 选中的 same-key device
replacement rejection、HP/RT intent-sensitive implementation selection、Tensor planning/task
selection/edge mapping 与 Region dirty lifecycle。

`test_packed_fp4_dense_tensor` 拥有 4 个 dependency-neutral V-13 integration case。它们验证
两种 nibble order 与 nonzero bit offset、精确 encoded/scale-dequantized E2M1 access、严格
descriptor/quantization/layout/envelope rejection、block-aligned TensorSlice 对 scale/code 的
投影与 fresh identity、byte-view 与 ImageBuffer fail-closed 行为、保留表示的 CPU 与注入式
fake-device transfer、精确正式 memory-cache retention，以及在 executor、filesystem 或 codec
副作用前发生的 typed image disk-cache rejection。Malformed matrix 包括错误 quantization
rank/count、zero 或 non-divisible block、nonfinite/nonpositive scale、错误 layout version/
alignment/overlap/size、quantized Strided publication 与 oversized blocked transfer alias。

`test_variable_sample_field_extensions` 拥有 17 个只使用标准库的 V-14 integration case。
一个合成纯 C definition suite 会发布带版本的 VariableSampleField Schema、Facet 和 Layout record，
并使用三个 physical buffer。这些用例会验证 typed namespace、candidate conflict 与 malformed
record rollback；在 revision minting 前拒绝通用 cross-reference 错误；provider semantic rejection；
在没有 provider 时保留未知 byte 的 artifact-envelope round-trip；property/DataSpec/Region callback
中的每个 payload pointer 均被清除；独立且精确的 SHA-256 descriptor/content/layout vector；
physical repacking 与 padding 不改变 content identity；固定内存生成式 stream 超过 64 MiB 时的
增量 ContentDigest 及其独立计算的精确 vector；不同 provider callback chunk 边界保持相同
identity；sticky malformed/null 与 `uint64_t` overflow sink failure；measured/hash
count-drift 拒绝及后续正常计算恢复；旧 Value/read/owner 跨 replacement 和 unload 的
lifetime；最终 provider-before-module destroy 顺序；callback-local diagnostic 与
非空 property copy-out、oversized-output boundary；rank-general Exact TensorSlice 的 checked
site count，包括错误非零 count、错误零 count 与 `uint64_t` product overflow；以及 concurrent
replacement 不存在 mixed-generation resolution。并发 reader 会从各自 output state 抽样
callback-local property；保留的旧 Value 则在 replacement 后查询同一 property，以覆盖
thread/generation lifetime boundary。新增的 4 个 callback-tail case 还要求 owner destruction
在成功的外层 callback 返回后、worker 退出前完成 drain，在失败的 provider callback 之后完成
drain，并在 foreign-generation destroy request 之后延迟处理；同时要求 cascading cleanup 在
module release 前保持 FIFO owner-destroy 顺序。

Callback-view case 是 input 生命周期结构化回归。它通过同一个 Value 进入 validation、property、
DataSpec、Region 与 content callback，并要求每个 Schema/Layout record、可选 Facet array、buffer
array、Layout-envelope array、metadata payload 与显式 content pointer 在对应 callback 期间持续
有效。生产 adapter 把 move-safe owning storage 与借用的 `ps_data_value_view_v3` 分离，只在最终
callback caller 地址即时 materialize view。Scoped no-elide 运行必须以
`-fno-elide-constructors` 编译 `photospider_operation_runtime` 本身，而不能只编译 test source，
随后运行该 case。这是一项手工 compiler-mode 证明，不是新增 CTest entry 或 CI phase-completion
检查：

```bash
cmake -S . -B build-v14-no-elide \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_BUILD_IPC=OFF \
  -DPHOTOSPIDER_ENABLE_OPENCV=OFF \
  -DPHOTOSPIDER_ENABLE_YAML=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON \
  -DCMAKE_CXX_FLAGS=-fno-elide-constructors
cmake --build build-v14-no-elide \
  --target test_variable_sample_field_extensions -j 2
ctest --test-dir build-v14-no-elide --output-on-failure \
  -R '^VariableSampleFieldExtensions\.EveryCallbackReceivesOneStableMaterializedValueView$'
```

Active output byte 必须等于 `255 - input`；input/output row padding 不被当作 image element。

聚焦验证命令为：

```bash
cmake --build build --target test_region_contracts \
  test_cpu_dense_tensor_image_operation \
  test_packed_fp4_dense_tensor \
  test_variable_sample_field_extensions \
  public_header_self_containment -j 2
ctest --test-dir build --output-on-failure \
  -R '^(RegionContract|RegionImageAdapter|RegionPropagation|RegionRouteSelection|RegionPlanning|RegionLifecycle|CpuDenseTensorImageOperation|PackedFp4DenseTensor|VariableSampleFieldExtensions)\.'
```

`DependencyDisabledInstallSmoke` 会在真实禁用 OpenCV/YAML/OpenEXR discovery 的 product 中构建并
运行全部 48 个 dense 用例、全部 4 个 packed FP4 用例与 17 个 V-14 extension 用例，再证明
installed consumer；
`StaticProductConsumerSmoke` 会证明 operation-SDK-only
installed consumer。`DependencyDisabledInstallSmoke` 还会加载两个独立链接且使用 Value 的
DSO，证明它们从同一个 shared runtime authority mint identity。两个 installed consumer
都会在没有 optional dependency 时构造并计算 Region，并观察同步 Ready Value fence。下述
provider-disabled nested build 也会编译并运行全部 48 个 dense case 与该双 DSO case，因此真实
core operation、fence/transfer proof 与 identity authority 都不依赖 optional OpenCV operation
provider 或 native device SDK。

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
provider-independent focused provider binary 及其 stdlib-only fixture、CPU
DenseTensor/ImageView integration binary、专用 disk-cache 与 kernel-lifecycle concurrency
binary，以及 provider-independent `test_kernel_contracts` internal-seam consumer，再查询机器
可读的 CTest inventory。`test_kernel_contracts` 的构建用于覆盖 focused-only direct-consumer
closure，但不会在该嵌套 inventory 中被 discover。

配置期间，CMake 会在交叉核验 GoogleTest registration metadata 后，序列化每个 active
`gtest_discover_tests` target 及其配置专属 `$<TARGET_FILE:...>` 路径。生成的 TSV 只允许一个
精确首行 header `# target<TAB>configured executable`，其后只能出现非空的双字段 data record。
CMake writer 只接受由字母、数字、`_`、`.`、`+` 与 `-` 组成的 local executable target name，
同时保留 `$<TARGET_FILE:...>` 与 `$<CONFIG>`，直到 generation 阶段才求值，因此 single-config
与 multi-config build 都会选择原生 executable path。Reader 会拒绝缺失或重复的 header、
后续任何 comment 或空行、额外字段、重复 target、输入的 `_NOT_BUILT`，以及所有非结构性 C0
control 与 DEL；即使 CMake 无法表达 NUL，reader 仍会拒绝它。Target path 必须在词法上是绝对
POSIX path、Windows drive-rooted path 或 Windows UNC path；普通空格与 Windows 反斜杠是合法数据。

Focused build 完成后，driver 会从 executable 不是 regular file 的已注册 target 推导精确且
不带 label 的 `${target}_NOT_BUILT` 集合。该过程会观察真实 build closure（包括间接
dependency），无需硬编码 target 数量或未来 target 名，也不会从 CTest 实际观察到的 sentinel
反推 expectation。精确 CTest
inventory 等于该推导集合与以下条目的并集：`DependencyDisabledInstallSmoke`、
`OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores`、全部 48 个
`CpuDenseTensorImageOperation.*` case、
`ValueIdentityAcrossDsos.MintingAuthorityIsProcessWide`、三个
`DiskCacheDiagnosticConcurrency.*` case，以及两个 `KernelLifecycleConcurrency.*` case。推导出的
sentinel 不得带 label 或 timeout。

在当前 V-14 checkpoint 中，CMake 在该 profile 下精确注册八个 active GoogleTest target。
包含六个 target 的 focused build 会具现其中五个已注册 executable；第六个 target
`test_kernel_contracts` 只参与构建，且特意不被 discover。CTest discover 出 55 个可运行
focused case；三个动态推导出的 sentinel 精确为
`test_compute_io_executor_NOT_BUILT`、`test_packed_fp4_dense_tensor_NOT_BUILT` 与
`test_variable_sample_field_extensions_NOT_BUILT`。再加上 `DependencyDisabledInstallSmoke`，精确
CTest inventory 因而包含 59 项。这是 dynamic manifest 与真实 build closure 的已验证结果，不是
production driver 维护的 target 数量或 sentinel 名单。推导出的 sentinel 不带 label 或 timeout；
disk-cache case 只保留 `kernel-concurrency` label 与 20 秒 timeout，lifecycle case 保留同一 label
与 60 秒 timeout；dense-image 与 Value-runtime case 保留 30 秒 timeout，且只有后者携带
`value-runtime` label。缺失或额外 entry 都会失败，因此不得残留依赖 provider 的 broad test。
Driver 随后通过 CTest 运行全部已构建 focused case。禁用 profile 要求 dependency-neutral
analyzer/math/dense-invert operation 仍被 seed、OpenCV-backed operation key 不存在，并要求
replacement provider 能发布、执行且完整退役其 resize key。该临时 build 是长期 product
configuration 检查；它把命令与结果写入 CTest，不保留逐次运行报告。当前阶段禁用的是
operation provider，不是彼此独立的 OpenCV codec、normalization、adapter 或
embedded-product 依赖。

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

## 执行画像 SLO 手工/Release Protocol

[ADR 0010](../../adr/zh/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.zh.md)
定义规范的 `execution-profile-slo-v1` 契约。Issue #92 会冻结本 protocol，但不
实现 runner 或 collector。在 Issues #93 至 #96 交付各自负责的证据行之前，
当前仓库没有任何命令能够生成符合要求的 bundle，也不暗示当前画像已经达标。

最终长期维护的 runner 是手工 developer/release 工具。由于本节定义其长期产品
测量职责，它可以留在 primary repository；但与机器相关的 latency、throughput
与 reference ratio 必须始终不进入普通 CTest 和默认 CI correctness gate。Runner
只能写入 checkout 外的显式可丢弃路径或 release-artifact storage；生成的 bundle
不得提交到 primary 或 personal-overlay repository。对每个包含 B1 的运行，两种
destination 都必须位于已选且已指纹化的 `OutputStore` root 或 rooted namespace
之下，不能绕过其已证明的 crash-durability path。

### 冻结的证据行与采样窗口

Runner 必须使用 ADR 0010 中精确的 graph/source/edit/preview/job/cadence 选择；
方便的替代 graph 不是 v1 证据行。

| 证据行 | 必需 workload 与证据 | Cold | Warmup | Measured window |
| --- | --- | ---: | ---: | ---: |
| I1 isolated | `I1-edit-storm-v1`；latency、waste、memory、output correctness | 1 个 episode | 20 个 episode | 200 个 episode |
| I2 isolated | `I2-progressive-v1`；第十二次 edit（`edit_index=11`）preview/final latency、Host/条件式 Metal residency 与 copy waste、memory、output correctness | 1 个 episode | 10 个 episode | 100 个 episode |
| B1 cap 1 | `B1-immutable-v1`；throughput、determinism、fault-free waste、memory | seed 252 | seed 253/254/255 | job `0..29` |
| B1 cap 8 | 相同 B1 corpus 与环境，仅 Run cap 不同 | seed 252 | seed 253/254/255 | job `0..29` |
| M1 shared | `M1-shared-v1`；latency、progress、fairness、waste、memory | 1 秒 | 5 秒 | 30 个互不重叠的一秒 window |

每一行使用三个全新的 process/execution-domain replicate。Cold first-use 单独
采集，并排除在 steady-state aggregate 之外。自然 edit ordinal `1..12` 映射为
`edit_index=0..11`；必需 final 是第十二次 edit（`edit_index=11`、`k=1.04`、source
Region `(768,512,256,256)`、preview Region `(192,128,64,64)`）。裸写“edit 12”
不是 v1 evidence identity。

Baseline 结算后，episode 选择 monotonic origin `E`，并使用
`S_i=E+i*16,666,667 ns`。`A_i` 是 final Host admission 前立即取得的唯一
monotonic-clock sample；它启动 latency sample，并通过 checked addition 得到唯一
absolute Run deadline `D_i=A_i+150,000,000 ns`。`A_i` 必须处于
`[S_i,S_i+2,000,000 ns]`；nominal `S_i` 绝不锚定 deadline，允许的 wake lateness
也不消耗 150 ms budget。Overflow、提前启动、迟于 2 ms、miss/drop/gap 或
admission failure 都会使 replicate 无效。Runner 在任何迟到 Host call 前请求
cancellation/supersession 并记录其被接受、撤销 publication，并且不会追赶、回填或
移动后续时刻。已进入的 non-preemptible work 按 waste drain；post-cancel start
为零，missed/expired work 不能发布 output、receipt 或 successful latency。Final
500 ms drain 只观察 quiescence，绝不延长 `D_i`。Episode origin 精确间隔
750,000,000 ns；
baseline 准备必须在每个 origin 前完成。M1 在 measured time zero 重启该
schedule，共精确启动 40 个 episode，并
持续提供 cap-8 B1。这是可复现的 nominal monotonic time 与 lateness bound，不是
精确 OS wake 的声称。

Disk-cache/codec I/O 与跨 episode/job result
reuse 保持禁用。I1/I2 只保留显式重新计算的 baseline/current episode target 与
已声明的 I2 output residency；每个 B1 job 开始时都没有可复用 fixture result。
Warmup B1 job 用独立 identity/directory 执行完整 artifact path；owner 结算后
移除 output，同时保留 process/provider/JIT state。Warmup observation 绝不进入
measured aggregate；测量边界在不重启进程的情况下重置 counter。

v1 resource profile 是 32 个 CPU slot、1 GiB Host retained memory、512 MiB
Host scratch、65,536 个 ready entry 与 256 MiB ready byte；Interactive headroom
为 1 个 CPU slot、64 MiB retained memory、32 MiB scratch、1,024 个 ready entry
与 16 MiB ready byte。Compute I/O 准入上限为 64 个 task 与 256 MiB 计划字节总量。
配置 Metal executor 时，device memory 与 device scratch 分别为 512 MiB 与
256 MiB；Metal 缺失属于预定义 `not-applicable`。

对 B1 fairness 证据，只要 Graph producer 仍有未消费 offered demand 且没有暂停
提交，该 Graph 就是 eligible，其中包括 bounded-admission wait。Harness 在测量
边界同时提供两个有序的 15-job queue，按递增 job index 推进每个 Graph，并在 M1
中无 producer gap 地开始新 cycle；它不会绕过正常 bound 准入全部 30 个 Run。

每个 B1 occurrence 都通过 canonical `job-instance-v1`
`(row_workload_id,replicate_ordinal,phase,cycle_ordinal,job_index,run_cap)` 建立
index。Phase-local cycle zero 覆盖 cold/warmup 与 isolated measured B1；M1 使用
当前 phase-local cycle，并且只在同 phase 的完整 `0..29` corpus 后增加 cycle。
Logical I/O task 增加 stage，完整 task
identity 再增加 `attempt`。Capacity rejection 或幂等 duplicate 保持 attempt zero
与相同 charge；只有 terminal failure 后的显式 retry 才增加 attempt。Cycle 绝不
表示 retry。Charge、admission/status、snapshot、start/terminal、commit id/slot、
receipt、raw trace 与 row evidence 都必须携带完整 job-instance identity。Normalized
semantic trace 继续按 job index 编码，并通过 row job-instance index 把其 digest join
到每个唯一 occurrence。

每个 B1 job 在所选且已指纹化的 `OutputStore` root 下的全新可丢弃目录中写入
ADR 0010 规定的精确
`output.rgba32le` payload 与固定顺序 `manifest.txt`。两个有序
`ComputeIoExecutor` task 使用稳定
`(job_instance_id,stage,attempt)` charge identity：payload-stage 的
`planned_bytes=67,108,864`，manifest-commit 的 planned byte 是该 job 的精确
`242 + decimal_digit_count(job)` manifest 长度：job `0..9` 为 243 byte，job
`10..99` 为 244 byte，job `100..255` 为 245 byte。因此 measured job `0..29` 使用
243 或 244 byte，cold/warmup job `252..255` 使用 245 byte。每次 accepted admission/
settlement 后立即采样，必须证明 active task <=64、active planned byte
<=268,435,456，保留两种 high-water，并最终精确结算为零。每个 active planned-byte
total 都是对真实 per-job charge 的 checked sum。Planned byte 是 Compute I/O
admission、planned-byte high-water 与 final settlement 的强制性权威证据，但它仍是
estimate，不证明 physical memory ownership，也不能替代 RSS 或 ledger/device
ownership evidence。

目标 `OutputStore` 请求并且必须达到 typed `crash-durable`；它结算 payload，最后
以 no-replace 方式发布 canonical manifest，完成全部 leaf-to-root barrier，然后返回
ADR 0009 receipt。较弱、不支持或失败的 durability 都会使结果无效。一个 B1 job
的 commit id、rooted no-replace slot 与 receipt 会绑定完整 job instance 及其 fixture
job index。只有在该 receipt 和 logical/raw 两种 golden check 后才贡献 throughput。每个 I2
第十二次 edit（`edit_index=11`）preview/final 都通过相同 Host
binding 获取两次。已配置 Metal device 允许每个不同 preview/final revision 的
首次 access 执行一次精确大小的 upload；第二次必须命中相同 residency。禁止
CPU copy、readback、disk/codec access 或额外 transfer。

I2 使用 ADR 0010 的目标 state machine，而不是虚构当前 API：每次 edit 生成一个
realtime request generation，立即提交合法的 `RealTimeUpdate`/`Interactive` preview
child，并在共享 realtime request identity 下 arm 合法的
`GlobalHighPrecision`/`Full` final child。Final 只在其 preview 可见且仍为 current
时提交。更新 generation 会撤销两个较旧 child 的 publication permission。Preview
latency 从 preview admission 前立即开始，到 preview visibility 结束；final latency
使用同一起点，到 final visibility 结束。只有 `edit_index=11` 必须依次发布二者。

必需 logical value 调用 `compute_content_digest(Value)`，并且要求 `Available`、
存在 `ContentDigest`，以及 `CanonicalDigestAlgorithm::Sha256CanonicalV1`。Logical
digest、raw little-endian payload SHA-256、canonical manifest SHA-256、semantic-
trace SHA-256 与 logical/raw golden identity 始终是不同的 evidence family。

### 存储环境指纹

封闭的 byte schema 以
[ADR 0010](../../adr/zh/0010-execution-profile-slos-are-six-independent-benchmark-verdicts.zh.md#storagebase-与逐行-environment-manifest-是封闭的-v1-schema)
为规范权威。Runner 与独立 validator 必须拒绝 provider extension、替代的“等价”
object、遗漏 record 与 best-effort parsing。

Storage 的精确 field 顺序是：

```text
output_store_contract_id
output_store_contract_generation
backend_semantics_id
backend_semantics_generation
backend_instance_id
backend_class
locality
persistence
filesystem_type
mount_identity
mount_effective_options
commit_semantics
durability_capabilities
requested_durability
achieved_durability
durability_endpoint_identity
durability_anchor_identity
storage_class
b1_performance_configuration
hardware_write_cache_policy
power_loss_protection_policy
```

Base 的精确 field 顺序是：

```text
os_family
os_release
kernel_name
kernel_release
architecture
cpu_inventory
gpu_inventory
other_device_inventory
compiler_id
compiler_version
compiler_target
standard_library_id
standard_library_version
build_mode
build_flags
process_worker_count
provider_contracts
plugin_contracts
resource_limits
metal_resource_limits
cache_preconditions
residency_preconditions
power_policy
thermal_eligibility
```

Environment-class 的精确 field/type 顺序是：

```text
base_environment_digest:sha256
storage_environment_applicability:enum
storage_environment_not_applicable_reason:enum
storage_environment_digest:sha256
```

Validator 应用 ADR 的精确 type、enum domain、nested record layout、cardinality 与
固定 resource value。Repository/dirty identity、subject binary hash、所选 absolute
path 与 fresh job directory 保持为必需 raw evidence，但位于两个 manifest 之外。
Storage adapter 把每个 backend 都映射到相同的 `durability_endpoint_identity` 与
`durability_anchor_identity` field；不能提供 provider 特有替代 field。

每个 manifest record 使用精确 ASCII length-frame 形式
`field=<frame(name)><frame(state)><frame(reason)><frame(type)><frame(payload)>`
并追加 LF。`frame(B)` 是不补零的十进制 byte length、冒号与 `B`。完整 ASCII
`uint64` 的精确 lexical language 是 `0|[1-9][0-9]*`，数值闭区间为
`0..18446744073709551615`；`00`、`01` 等前导零形式和溢出均为 invalid。Text 是
编码为小写十六进制的 NFC UTF-8；identifier、enum、boolean、SHA-256、list、map、
set 与 fixed record 遵循 ADR 的封闭 grammar。Header 精确为
`execution-profile-storage-environment-v1\n`、
`execution-profile-base-environment-v1\n` 与
`execution-profile-environment-class-v1\n`。缺失/额外/重排/重复 record、malformed
length、BOM/CR/额外 whitespace、非 canonical scalar，或 set、map、record list
及其他要求唯一的 binding 中未排序或重复的 item 都是 invalid。

Validator 不能依据上述通用说明自行推断具体 collection type。`token-set-v1` 是
count 加每个精确 raw ASCII token 的一个 frame，按未加 frame 的 token byte 排序并
保持唯一；空 set 为 `0:`，未知 token 为 invalid。`ordered-text-list-v1` 是 count 加
每个 canonical lowercase-hex `text` payload 的一个 frame，保留 invocation order；
允许 duplicate，空 list 为 `0:`。CPU/device/contract record list 为每个完整 fixed-
record payload 加 frame，按未加外层 frame 的完整 record byte 排序并保持唯一，同时
执行各自的 `>=1`、`>=0` 与 provider/plugin cardinality。Mount 与 commit type 使用
通用 map grammar，分别精确包含 7 与 6 个已排序 raw-token pair。其他每个 named
composite 都使用通用 fixed-record grammar，按 ADR 声明顺序精确包含 component frame，
wire 中没有 component name。

`b1-performance-configuration-v1` 的精确 fixed-record component/type 顺序是：

```text
compression_mode:enum
compression_algorithm:identifier
compression_level:uint64
compression_profile:identifier
encryption_path:enum
encryption_profile:identifier
checksum_mode:enum
checksum_algorithm:identifier
deduplication_mode:enum
logical_block_bytes:uint64
physical_block_bytes:uint64
record_bytes:uint64
allocation_unit_bytes:uint64
allocation_mode:enum
provisioning_mode:enum
layout_mode:enum
layout_data_units:uint64
layout_parity_units:uint64
layout_replica_count:uint64
layout_stripe_unit_bytes:uint64
layout_profile:identifier
upper_write_cache_mode:enum
upper_write_cache_profile:identifier
io_scheduler:identifier
io_queue_policy:enum
io_queue_depth:uint64
io_concurrency_policy:enum
io_concurrency_limit:uint64
network_path:enum
network_protocol:identifier
network_link_profile:identifier
network_mtu_bytes:uint64
network_qos_profile:identifier
network_region:identifier
backend_service:identifier
backend_performance_tier:identifier
device_performance_profile:identifier
```

ADR 的封闭 enum/sentinel/cross-component rule 都属于 validation 范围。Zero byte
unit 与 `not-applicable` identifier 要求缺失/non-applicable layer 的肯定证明，不能
表示 opacity。Configuration 在 warmup 前采集，并在整个 replicate 中保持稳定。
它排除 disposable path、subject commit/binary 与瞬时 load、queue、cache、autoscaler、
free-space、RTT 或 jitter sample；这些保持为 raw precondition/diagnostic，两个运行
之间无需精确相等。

当 `layout_mode=provider-managed` 时，四个 geometry component 仍留在 fixed
record 中。正值表示该概念存在于完整 path，并已观测到精确 effective value。零值
要求相应 retained raw-proof kind：`provider-layout-data-units-absent`、
`provider-layout-parity-units-absent`、
`provider-layout-replica-count-absent` 或
`provider-layout-stripe-unit-absent`。这些封闭 label 用来证明概念不存在，不会创建
component state、N/A pair、field 或 digest 输入。稳定且非 placeholder 的
`layout_profile`、四项 value/proof 与完整 path 必须来自同一次冻结 observation，并
满足已记录 backend-semantics generation。Opacity、variability、nondisclosure 或精确
value 缺失会使整个 performance field 成为
`unprovable/evidence-chain-incomplete`；value/proof 冲突会使其成为
`unprovable/conflicting-effective-values`。部分 fixed record 与用零编码 opacity 都是
invalid。

Observation state 是 `known`、`not-applicable`、`unknown`、`unobserved`、
`unsupported` 或 `unprovable` 之一。Known 使用 reason `none` 与 canonical payload。
其他 state 都使用空 payload，且只能携带其封闭的 state-specific reason。唯一 eligible
的 N/A pair 是：`filesystem_type` 的 filesystem absence；`mount_identity` 与
`mount_effective_options` 的 mount absence；两个匹配 policy field 的 hardware-cache/
PLP layer absence；`metal_resource_limits` 的 configured Metal absence；以及 I1/I2
storage-digest record 的 `row-has-no-output-commit`。缺少 probe、provider opacity 或
remote boundary 永远不能证明 absence。

对于 mounted backend，`mount_effective_options` 精确拥有七个已排序 key：
`access_mode`、`atime_policy`、`cache_coherence`、`copy_on_write_mode`、
`data_write_mode`、`journal_mode` 与 `metadata_write_mode`，value 只能来自 ADR enum。
Adapter 发出 effective behavior：omitted default 与 explicit default 输入 canonicalize
为相同值，丢弃 native order；只有 platform 声明 option domain 对 ASCII 大小写不
敏感时才 fold case。Platform 已定义的 duplicate winner 要 probe 后只发出一次；
winner 不可证明或冲突时为 `unprovable/conflicting-effective-values`。只有保留 proof
表明未知 native option 既不影响七个 key、`commit_semantics`、固定 performance
record、hardware-cache/PLP policy，也不影响完整 B1 write/sync/barrier/provider-
commit/revalidation/golden-readback path 上的 performance/durability 时，才可排除它；
否则 normalization 或 performance record 为 unprovable。六个固定
`commit_semantics` key、八个封闭 durability capability token 与 37 个固定
performance component 要独立验证。Btrfs `compress=zstd` 与 disabled compression
必须编码为不同 performance record，不能比较为同一个 environment。

Eligibility reason 必须作为精确 predicate 重新计算，不能接受任意 subset。Canonical
framing、lexical/scalar/composite validation、field/type/state/reason rule、domain/
cardinality、ordering/uniqueness、fixed-record shape 或 cross-field validation 任一
失败，都只产生 `canonical-schema-invalid`，并停止 eligibility evaluation。对于
canonical manifest，validator 评估下表每一行，并只输出所有为真的 token，每项一次，
按 unsigned-ASCII 排序。没有 true token 表示 `eligible`；一个或多个表示
`ineligible`。

| Token | Canonical manifest 的精确触发条件 |
| --- | --- |
| `commit-semantics-inconsistent` | Known commit-map value 与 retained transaction/receipt observation 无法形成一个一致的 payload-stage、manifest-last、no-replace、synchronization 与完整 barrier/provider-transaction commit。 |
| `durability-class-not-crash-durable` | Known requested 或 achieved durability value 不是 `crash-durable`；没有 known 较弱 value 的 ineligible state 由 required-observation predicate 处理。 |
| `durability-path-inconsistent` | Known contract/backend/instance/mount、endpoint、anchor、commit 与 receipt/path fact 明确冲突或标识多条 path。仅缺少 binding proof 属于 raw-proof failure。 |
| `mount-normalization-unprovable` | Present mount 因 `mount_identity` 或 `mount_effective_options` unprovable、normalization resolution 未解决，或 retained native observation 与 known identity/七 key map 冲突而无法唯一归约。 |
| `not-applicable-proof-invalid` | 允许的 N/A pair 缺少精确完整路径 layer-absence proof，或该 proof 与 path 冲突。 |
| `performance-configuration-unprovable` | Field 不是 known，相关 option 缺少 mapping/no-effect proof，provider geometry 不完整，或冻结 configuration drift。完整观测到的 drift 归此项。 |
| `raw-observation-proof-incomplete` | 用于 known storage value、允许的 N/A claim 或 raw-to-canonical normalization 的 proof 缺失、不完整、陈旧或冲突。它不吸收 schema、capability、durability-class、证据完整的 inconsistency/drift 或 containment failure。 |
| `required-capability-absent` | Known `access_mode` 为 `read-only`，或八项 required durability token 中任意一项缺失。 |
| `required-observation-ineligible` | Required storage field 为 `unknown`、`unobserved`、`unsupported` 或 `unprovable`；允许的 N/A state 由其 proof predicate 处理。 |
| `root-containment-unproved` | Measured job 或 retained release artifact 缺少成功且无歧义的 containment proof，或该 proof 失败/冲突。 |

完整可能顺序精确为 `canonical-schema-invalid`、
`commit-semantics-inconsistent`、`durability-class-not-crash-durable`、
`durability-path-inconsistent`、`mount-normalization-unprovable`、
`not-applicable-proof-invalid`、`performance-configuration-unprovable`、
`raw-observation-proof-incomplete`、`required-capability-absent`、
`required-observation-ineligible`、`root-containment-unproved`。只有两个 predicate
都为真时，category token 与 raw-proof token 才刻意重叠：例如，raw mapping 冲突的
unprovable mount 会输出 mount、raw-proof 与 required-observation token；raw stream
完整的 configuration drift 则只输出 performance token。Reason list 不是
environment-digest 输入，但必须可以独立复现。

独立 validator 按顺序执行：

1. 使用 checked `uint64` length/count arithmetic 解析每个 frame，并要求精确 header、
   LF、field count、field order、type、state/reason pair 与 end of input；
2. 验证 scalar/composite canonical form、enum domain、list cardinality、order/unique、
   具体 token/text/record-list 与 map/fixed-record binding、nested record shape、固定
   resource、全部 37 个 performance component 与 cross-field consistency；如果步骤
   1 或 2 失败，则精确返回 `canonical-schema-invalid`，并在 raw-proof、digest、
   environment-class 或其他 eligibility evaluation 之前停止；
3. 把每个规范化 field 绑定到保留的 raw observation/proof，并验证每个 field-specific
   N/A claim、mount normalization decision、稳定 instance/endpoint/anchor identity、
   固定 performance configuration、被排除 option 的 no-effect proof 与 root-
   containment proof；
4. 对完整精确 manifest byte 计算小写 SHA-256，从而复算
   `storage_environment_digest` 与 `base_environment_digest`；
5. 解析精确四 field environment-class manifest 并复算
   `environment_class_digest`；B1/M1 要求 known `required` 与 storage digest，I1/I2
   要求 known `not-applicable`、reason `row-has-no-output-commit`，以及 payload 为空的
   N/A storage-digest record；以及
6. 评估表中每个 canonical-manifest predicate，只输出所有为真的 reason token，每项
   一次并按 unsigned-ASCII 排序；空 list 精确派生 `eligible`，非空 list 派生
   `ineligible`。Reason list 是 retained evidence，但不进入 environment digest。

精确 compatibility 要求 canonical manifest 逐 byte 相同、独立复算 digest 相等，
并在 storage 适用时要求 eligibility。对 storage，该 byte comparison 包含完整 framed
performance record。仅 digest 相等不够。Candidate/reference I1/I2 使用精确 base
compatibility 与固定 storage-N/A environment manifest。Candidate/reference B1/M1、
B1 cap-1/cap-8 与 M1/paired-B1-cap-8 使用精确 base、storage 和完整 environment-
class compatibility。M1/paired-I1 只比较精确 base manifest/digest；二者的
environment manifest 有意不同。Raw field/proof 缺失、state invalid、byte/digest
mismatch 或 containment 失败，都会使受影响 relative verdict 成为 `invalid`。

Issue #95 必须增加长期确定性机制测试，覆盖固定 field/type/enum/cardinality 拒绝；
每种 state/reason/payload 组合；NFC/text 与 scalar encoding，包括接受 uint64 `0`、
`1`、`2`、`8`、`9`、`10`、`23`、`18446744073709551615`，拒绝 `00`、`01` 与
overflow；精确 156-byte durability set 与 221-byte field record；known-empty ordered
text（包括重复 flag）与 zero-byte N/A payload；每种 CPU/device/contract record-list
cardinality、frame、sort 与 duplicate rule；mount/commit map count 与每个
fixed-record component order；omitted 对 explicit
mount default；native option order/case；确定与冲突的 duplicate；unknown-option
proof；malformed/overflow frame；全部 37 个 performance field、enum/sentinel/zero/
cross-component rule、transient-noise exclusion、四个 provider-layout component
各自的 positive/absence/opaque/conflicting case、unmapped-option fail-closed 与 Btrfs compression
mismatch；三层 digest 独立复算；十一 reason truth table、unsigned-ASCII order、
canonical-invalid short circuit、精确 overlap 与 eligible empty set；以及精确 B1
candidate/reference 和 cap-1/cap-8 compatibility。Issue #96 复用这些
fixture，并测试精确 same-ordinal M1/B1 matching 与 base-only M1/I1 matching。
Issue #92 不新增当前 test binary、serializer、probe、runner、API 或 runtime field。

### 运行流程

对每个 candidate 或 reference bundle：

1. 在评估依赖 reference 的行之前，按 content digest 选择一个 immutable
   reference；
2. 每个 replicate 启动一个 fresh process，并记录 repository commit、dirty
   state、build/compiler/flag、OS/kernel、CPU/GPU/device inventory、power/thermal
   eligibility、provider/plugin binary 与 generation、process worker、Run cap、
   全部 limit/headroom、fixture hash、seed 和 cache/residency precondition；在
   warmup 前编码并独立验证精确 24-field base manifest；对 B1/M1，还要选择
   `OutputStore` root、采集 raw storage/capability/configuration observation、编码
   精确 21-field storage manifest、冻结固定 performance configuration，并计算其
   eligibility 与 digest；
3. 要求 candidate 与 reference 的 evidence schema、workload id、environment
   class、limit 与 fixture hash 相同；B1/M1 比较要求逐 byte 相同且 eligible 的
   storage/base manifest 与匹配的四 field environment-class manifest，I1/I2 使用
   固定 storage-N/A environment class，不会获得无关 storage 要求；
4. 对 ordinal 为 `1..3` 的 M1 replicate，分别固定 same-subject、same-ordinal 的
   isolated I1 与 isolated B1 cap-8 row/bundle digest；要求两个 pair 都具有逐 byte
   相同的 base manifest 与相等的复算 base digest，只对 B1 pair 要求精确且 eligible
   的完整 environment-class 匹配，同时要求 resource、fixture、build/provider 与
   precondition 兼容，并保留
   独立的 candidate `comparison_reference_bundle_digest` 语义；
5. 保留 cold first-use，执行精确且不参与测量的 warmup，在不替换冻结环境的情况下
   重置测量 counter，然后执行精确 measured window；
6. 在包含 B1 的 work 前分配并保留 canonical job-instance index，拒绝重复 phase/
   cycle/job coordinate，并验证每个 charge、admission、commit、receipt 与 evidence
   join 使用 occurrence identity 而不是 retry identity；
7. 在各自 owner 边界采集 raw admission、visibility、cancellation/quiescence、
   start、completion、offered-demand eligibility、artifact/receipt、trace、digest、
   transfer/copy/residency 与 resource-lifetime observation；
8. 拒绝任何必需的 telemetry cursor gap/drop，不估算缺失 observation；
9. 使用 checked arithmetic 从 raw evidence 计算每个 replicate aggregate 与各项
   独立 dimension verdict；以及
10. 按 address-dependency 拓扑顺序封存 external prerequisite、retained section/
    provenance、row 与 enclosing bundle；在不存在直接或传递 self-reference 的情况下
    计算彼此不同的 domain-separated digest；对每个 comparison/pair 强制执行功能行 key
    唯一与精确 row 选择；并在报告 conformance 前独立复算每个 section、aggregate 与
    verdict。

全部 duration 使用 monotonic clock。Percentile 使用 nearest rank：排序 `N` 个
sample，并选择从一开始的 rank `ceil(p*N)`。每个 replicate 必须通过；pooling
sample 或 median summary 不能隐藏失败进程。

### 公式与门禁

| 维度 | 必需计算与通过规则 |
| --- | --- |
| Latency | I1 从 final Host admission 前立即开始，到匹配的 current visibility 结束。I2 第十二次 edit（`edit_index=11`）preview 从 preview admission 前开始，到 preview visibility 结束；final 使用同一起点，到 final visibility 结束。I1 p50/p95/p99 <=50/100/150 ms 且 final success 为 100%；I2 preview p50/p95/p99 <=50/75/100 ms，final p95/p99 <=500/1000 ms，并匹配必需 `ContentDigest`；M1 还要满足 I1 绝对上界，且 p99 <=其 same-ordinal paired isolated I1 的 2.0 倍。被取消的 intermediate 不进入 percentile；accepted-cancel-to-quiescence 单独报告。 |
| Throughput | 每秒成功的 logical RGBA pixel-site transform，以 MPix-op/s 报告；一个 B1 job 只有在 Run success + crash-durable receipt + logical/raw golden verification 后才贡献 16,777,216 个 site-operation。Interval 在 final golden verification 结束。Candidate/reference replicate 按 ordinal 在一个精确兼容 storage environment 下配对：ratio 中位数 >=0.95，且每个 ratio >=0.90。每个 M1 一秒 B1 rate 使用 same-subject、same-ordinal 且 storage-compatible 的 paired isolated cap-8 B1 rate；p05 >=0.20，denominator 或 storage fingerprint 缺失、为零或不兼容时 invalid。 |
| Fairness | 对两个 B1 Graph 整个一秒 window 都保有未消费 offered demand、且 producer 均未暂停的窗口，`J=(x_A+x_B)^2/(2*(x_A^2+x_B^2))`，其中 `x` 是 completed `work_units + ceil(ready_bytes/4096)`。总 service 为零时 invalid；Jain p05 >=0.95。两个 class 都保持 startable 时，最多三次 Interactive start 后出现 Throughput。M1 还要求 headroom 导致的 Interactive admission failure 为零，并独立通过 latency/progress。 |
| Determinism | 对三个 replicate、fresh-process restart 与 Run cap 1/8 中相同的 B1 job index，typed logical `ContentDigest`、raw payload SHA-256、canonical manifest SHA-256、`execution-profile-semantic-trace-v1` SHA-256 与按 job index 区分的 logical/raw golden mismatch count 全部为零。 |
| Waste | `discarded_started_service / all_started_service`，使用 `work_units + ceil(ready_bytes/4096)`。每个无法 commit 结果的已启动 callback 都会被计费；已经进入的不可抢占 work 如实 drain。I1/I2 Interactive 每个 replicate <=0.25，M1 对 Interactive service 单独应用该上限；accepted cancellation/supersession 后才启动的 work 精确为零。I2 在允许的首次 transfer 规则下，额外 filesystem/codec、CPU-copy、readback、transfer 与 allocation byte 为零。无故障 isolated/mixed B1 的 discarded/duplicate/retry service 为零。 |
| Memory | Host retained、Host scratch、ready byte 与已配置 device memory/scratch 的独立 high-water byte，加上 B1 active Compute I/O task/planned byte。不得超过绝对 limit；isolated row-owned delta 与 B1 I/O count 回到 row 前 baseline/零，M1 shutdown 回到零。Candidate B1/I2 peak <=固定同环境 reference 的 105%。Process RSS 只作为 diagnostic。B1 planned-byte charge 与 event-aligned sample 是 Compute I/O admission、planned-byte high-water 与 final settlement 的强制性权威证据；它们不证明 physical memory ownership，也不能替代 RSS 或 ledger/device ownership evidence。 |

每个必需维度输出 `pass`、`fail`、`invalid` 或 schema 预定义的
`not-applicable`；不存在 composite score。缺少源证据、算术 overflow、monotonic-
clock failure、cursor/drop gap、fixture 或 environment drift、未固定/不兼容的
reference、必需 denominator 为零、必需 storage fingerprint 缺失/ineligible/
mismatched，或未经批准的 `not-applicable`，都会使受影响行成为 invalid 且不符合
要求。相同 unknown 或 unobserved storage state 不能证明环境兼容。

Semantic trace 对每个 deterministic plan task 精确使用三条 record：`ready`、
`start` 与 `terminal`。Record 携带 job/Graph、连续的 plan-relative task ordinal、
sorted dependency ordinal、action/outcome 与 declared resource vector。ADR 0010
规定的精确 ASCII header/field order、不补零 decimal、LF termination、numeric
job/task 与 action-rank sort，以及 lowercase SHA-256 都是必需项。Duplicate/missing/
unknown record 或 field、非法 dependency/outcome/encoding 或 collector gap 都会使
结果无效。Physical time、worker/queue/global identity、raw sequence、retry 与
completion order 不进入 canonical byte，但保留在独立 raw trace 中。

### Evidence Bundle

一个 `execution-profile-slo-v1` bundle 包含：

- 上述全部 provenance 与冻结环境值；
- 精确 base 与 environment-class manifest byte，以及 claimed 和独立复算的
  `base_environment_digest` 与 `environment_class_digest`；对 B1/M1，还包括精确
  storage manifest byte、raw capability/performance-configuration/root-containment
  observation、eligibility/reason，以及 claimed 和复算的
  `storage_environment_digest`；
- workload/fixture/source/graph/payload hash 与全部 seed；
- 相互分离的 warmup、cold 与 measured count/window；
- raw sample/event、offered-demand eligibility interval 与 drop/gap counter；
- typed logical output、raw payload、artifact-manifest、semantic-trace 与 logical/raw
  golden digest，加上 typed requested/achieved durability 与完整 commit receipt；
- transfer/copy/residency identity、byte 与 reuse outcome；
- 权威 resource sample 与 high-water/settlement delta，包括 event-aligned Compute I/O
  task/planned-byte sample 与 charge identity；
- 单位、公式、denominator、aggregate、invalidation reason 与每个必需维度的一项
  verdict；以及
- `subject_role`、candidate 的 immutable
  `comparison_reference_bundle_digest`，以及每个 M1 replicate 分离的 same-subject/
  same-ordinal isolated-I1 与 isolated-B1-cap-8 row/bundle digest，其中 I1 使用
  base-only compatibility，B1 使用精确 storage-compatible pairing。

Outer wire format 不是 implementation-defined。每个 row 都由精确
`execution-profile-evidence-row-v1\n` header 与 ADR 0010 的固定 15 个 field record
组成：workload/subject/replicate/cap、三个 environment coordinate、五个 section
digest 与两个 pair reference。非 M1 pair field 使用
`not-applicable/row-has-no-isolated-pair` 与 zero payload。Job-instance section 包含
完整 occurrence record 的 canonical list；I1/I2 使用显式 known-empty `0:` list。
每个 section digest 对字面量
`execution-profile-evidence-section-digest-v1\n` domain、framed section name、
schema id 与 retained exact byte 计算 hash。

每个 bundle 由精确 `execution-profile-evidence-bundle-v1\n` header 与五个 record
组成：workload id、subject role、provenance-section digest、comparison reference，
以及 item 均使用该 workload id 的非空 canonical row-reference list。Reference
bundle 使用
`not-applicable/reference-has-no-comparison-baseline`；任何 optional field 都不能省略
或使用空 digest 表示。`row_digest` 与 `bundle_digest` 对各自不同的 ADR 0010 domain
tag 加上完整 canonical manifest byte 的一个 frame 计算 hash。Claim 存储在 object
旁边，并排除在 hashed byte 之外。

每个 row-reference item 的功能 key 精确为
`(workload_id,run_cap,replicate_ordinal)`。即使两个 item 命名不同 row digest，list
也必须拒绝相同 key。对每个 item，validator 必须解析出恰好一个 retained canonical
row，复算其 digest，解析全部 15 个 field，并让 workload、cap、replicate 与 item
匹配，让 subject role 与 enclosing bundle 匹配。Candidate comparison target 必须是
workload 相同的 `reference` bundle；精确 target row 是与 candidate row 功能 key
相同的唯一 row。Comparison bundle digest 本身不选择 row。每个 M1 pair 则命名精确
row digest，并必须在必需 isolated workload 与 cap 8 下解析到 same-role、same-ordinal
target。Item、row、bundle、role 或 key 证据缺失、重复或不匹配时均为 invalid。

Address sealing 同样是规范规则。首先验证 immutable external target；随后按 dependency
顺序冻结 retained section 与 bundle provenance；再冻结 row；再冻结 enclosing bundle；
最后把 claimed digest 发布在 object 旁边。每个 versioned section/provenance schema 必须
暴露每个 address-bearing field 或 derivation input。Edge `X -> Y` 表示 `X` 依赖 `Y`
的 address；完整 section/provenance/row/bundle graph 与 external comparison/M1 bundle
graph 都必须是有限 DAG，且每个 target 必须先于 source 封存。Validator 会拒绝 opaque
或 undeclared address dependency、fixed-point construction、post-seal rewrite、直接或
传递的 self/enclosing/later-stage dependency、external cycle、missing retained byte、
unknown/extra/reordered field，或任何 claimed/recomputed mismatch。

Prose summary、未记录的“known good” build 重跑或当前 `BenchmarkResult` output
都不是规范 reference。Raw bundle 必须足以让独立 reader 复算每个 aggregate 与
verdict。

### 测试归属

Issues #93 至 #96 在新增 workload 语义、精确 start、cancellation、digest、
resource-limit 或 settlement invariant 时，应注册长期确定性产品行为。与机器相关
的性能 threshold 与 candidate/reference ratio 保留在本手工/release workflow。
任何 Issue 专属 replay、provenance/result orchestrator、phase-completion scan 或
performance-result file 都不得注册到 CTest/CI，也不得作为仓库内容长期保留。

Issue #95 负责 B1 `OutputStore` 固定 raw probe-to-schema mapping、backend 到固定
schema 的 adapter、mount normalizer、performance-configuration mapping/proof、唯一
canonical encoder/digest、eligibility 与 root-containment evidence，以及 cap-1/
cap-8 和 candidate/reference check。Issue #96 为 M1 原样复用精确 manifest byte，
并强制执行其 same-ordinal 完整 B1 pair，同时让 I1-only pair 只比较 base。两个
Issue 都不能重定义 v1 grammar、field 或 sentinel。Issue #92 只定义本 evidence
contract；它不新增当前 probe、serializer、public API、runner 或 runtime result
field。

## CTest 注册

所有预期 GoogleTest 二进制都应注册到 CTest。这包括当前可能低置信度的里程碑测试和 `test_propagation_contracts`。

低置信度测试仍应在验证中可见，而不是被静默排除。如果测试不足以可靠地作为开发门禁，应明确记录该状态，并创建后续工作升级或替换它。

里程碑测试、`test_propagation_contracts` 与长期 `test_region_contracts` 行为 suite 已注册到
CTest，因此它们可见；但在后续 pass 将前两类重写为更窄、更清晰 fixture 和断言的回归测试前，
它们仍是低置信度遗留测试。

`test_propagation` 不同：它是脚本式 REPL/tool 目标，不是 GoogleTest 二进制。CMake 保持它可构建，供手工脚本和临时验证使用，但 CTest 不会发现或运行它。不要声称 CTest 覆盖了 `test_propagation`；需要时应单独运行准确的手工命令。

只有 `BUILD_TESTING`、OpenCV、YAML、graph CLI、仓库 OpenCV operation provider 与仓库 OpenCV
operation plugin 全部启用时，才会注册依赖 provider 的默认完整 test suite。该 suite 会注册
`test_stdlib_image_buffer_processing`；即使该 producer 使用 OpenCV，它仍会直接编译标准库实现。
该测试验证 clone independence、stride-safe 且确定性的 bilinear border 行为、channel
conversion 与 ROI copy。默认 CTest inventory 也包含 `DependencyDisabledInstallSmoke`。

若在其他默认 test profile 选项不变时只禁用
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER`，CMake 不会创建或发现 broad suite。它会为注入式
codec smoke 与两个 dependency-disabled nested build 保留可构建的 provider-independent
`test_kernel_contracts` target，并且只注册
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
