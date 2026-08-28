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
时，请求 `COMPONENTS operation_plugin_sdk OPTIONAL_COMPONENTS operation_opencv` 的 consumer 必须让
package 与 `operation_plugin_sdk` 保持 found，将 `operation_opencv` 标记为 not found，导入无依赖的
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
它会在该真实 disabled producer 中运行全部 55 个 dense-image case、全部 4 个 packed FP4 case、
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
分别通过 `Photospider::operation_runtime` 链接进独立的 C++ Host consumer。每个 consumer 都会从
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
symbol 与 dependency。在 marker 分类前，每个精确 audit root 都会通过共享的 Darwin 受信映射
展开，因此工具输出中的 `/tmp/...` 与 `/private/tmp/...` 会作为同一个无语义 prefix 被 scrub。
该过程不会解析 caller-controlled symlink，prefix 之下的 library、header、symbol 与 dependency
名称仍对扫描可见。Default 和 optional-component probe 必须继续可用；required absent component
必须先以 Photospider 自有诊断失败，不得发现 OpenEXR。
`OpenExrDeepProviderInstallConsumerSmoke` 是启用态 companion：它安装显式 component，加载真实
module，解析两个 v3 export，校验 API table，调用 provider destruction，然后卸载 module。
其生成的 imported-provider path 只有在严格解析后的物理文件仍位于 installed prefix 内、且该
prefix 之后的每个 component 都不含 symlink 时，才可以使用任一受信 Darwin tmp 拼写。

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
且必须指向可执行 regular file。在 Darwin 上，只有共享的 root-owned `/tmp` alias 及其物理
`/private/tmp` root 可以为同一个 build-local suffix 提供不同拼写。比较过程绝不会把
caller-controlled path 解析进受信集合，因此后续每个 intermediate component 与 executable leaf
都不得包含任意 symlink。两份 manifest 只有作为本次调用的可丢弃 configure/generate step 的产物
才会被接受；即便如此，reader 仍会在任何 consumer 启动前完成全部 record、identity、set、
filename 与 path 校验。有效 consumer 随后按声明顺序运行；某个 consumer 运行失败时，后续
consumer 不会启动。

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
稳定的 CTest 标签 `build-smoke`。如果 companion 只验证 driver 的 cleanup、argument 或 manifest
逻辑，并且不会启动 compiler、product build、install、external consumer、compile target 或生成的
executable，它仍属于普通 `verification` 测试。

CTest 会保留每个 build smoke，供本机直接运行。日常 GitHub Actions build job 会在可复用主构建后
立即打包 CTest runtime，随后在上传该 archive 前完整运行一次 label。Workflow 不再维护测试名称、
不再解析 matrix inventory，也不再为每个 smoke 创建单独 job。新增长期 build smoke 只需要注册
CTest、添加 `build-smoke` label，并在 CMake 中设置合适的 `RUN_SERIAL`、`RESOURCE_LOCK` 与
`TIMEOUT`。Smoke 失败会阻断 runtime archive 上传和依赖的 test job。单独的 `always()` step 仍会
尝试把 `CI-results/build-smoke.junit.xml` 上传为 `ctest-junit-build-smoke`，将存在的 report 保留
七天，并在 report 缺失时只告警。

Nested driver 必须继续使用互不重叠的 work directory，验证其接受的任何 reusable producer，并且在
cleanup 时不得跟随 symlink 或删除无关 symlink target。因为轻量 runtime artifact 会在该 label
运行前冻结，所以本次调用产生的临时 compiler object 与 `CMakeFiles` tree 只留在 build
job/cache 中。不过，cache restore 仍可能带回较早 label 运行留下的固定 work root：
`tests/image_artifact_codec_dependency_disabled` 与
`tests/optional_opencv_provider_disabled`。因此 packager 会精确排除这两个 root 及其所有后代，但不会
从可复用 build tree 中删除它们；它也不会排除整个 `tests/` runtime root。

GoogleTest discovery 分配 source-role primary label 时不依赖重复 CTest property 行为。仓库 wrapper
会解析持续维护的 `gtest_discover_tests` argument surface，拒绝未知或缺值的 discovery keyword，
验证由已知 test-property pair 构成的偶数长度列表，并只向 discovery 传递一个标量 primary
`LABELS` property。由于 upstream module 不能传递 list-valued property，生成的
`TEST_INCLUDE_FILES` script 会消费每个 post-discovery `TEST_LIST`，并一次性设置完整、去重后的
primary-plus-orthogonal list 与全部 caller test property。其他已接受 property（包括
`ENVIRONMENT`、`RESOURCE_LOCK`、`RUN_SERIAL`、`TIMEOUT` 与 `WORKING_DIRECTORY`）的值会作为
单一 bracket argument 保留；重复的非 label property 会在
configure 阶段失败。

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
Client、纯 C operation ABI v1 与纯 C policy ABI v1 consumer。这些测试不得为该私有变更新增
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
texture/buffer allocation 前拒绝 heap query 给出的对齐后 persistent minimum。Dedicated-heap
admission 测试随后会证明该 query 只是 minimum：一次 ledger root-mutex transaction 会预留当前
全部可用的 persistent-memory ceiling 与精确 scratch；heap 的正值 `currentAllocatedSize` 是唯一
persistent actual；不会再次计入 heap-backed texture；每项 scratch resource 都贡献自己的正值
`allocatedSize`。这些测试还会证明：适配 plan 的 commit 会归还全部未使用 ceiling byte 并拆分
精确 lease；actual heap backing 超过已准入 plan 时，会在 native retention 或 command commit
前以 typed actual-exceeds-reservation category 失败，并让 native owner 与 reservation 各自准确
unwind 一次。预算充足的路径随后让真实仓库 Perlin operation 通过同一个 `ExecutionService`
连续运行两次，并证明 queue 可用、两次 operation submission 与 executor entry、八个 invocation
allocation 已退役、一条 pipeline 被复用、asynchronous pending-Value readback 生成 CPU-owned
output、使用专用 Metal worker id，并且已结算的 Host 与 device reservation 都为零。

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
application。`test_host_adapter` 会加载真实纯 C operation ABI-v1 与纯 C policy ABI-v1 fixture，
配置两种 extension、验证其 snapshot，并通过私有 CPU route 完成 compute。`GraphCliPluginComputeSmoke`
会通过真实 REPL 重复这条纵向路径。`test_ipc_protocol` 与 `test_ipc_daemon` 负责 protocol-v2
routing、process-owned policy state、会改变 generation 的 replacement、scan 与共享 execution
default。`StaticProductConsumerSmoke` 会独立构建已安装的 C11/C++17 operation ABI consumer 与
C11 policy DSO，再执行同一条 external-consumer path。Operation consumer 会断言全部精确 v1
record 布局，并且只导出数字/root 两个纯 C discovery symbol。

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
  same-device contention、minimum-query 校验后由单锁预留当前全部可用的 persistent ceiling、
  精确 scratch admission、plan-to-actual shrink 与未使用 byte 归还、typed underplanning
  rollback、拆分后的精确 memory/scratch lifetime、move-only authority、延迟 asynchronous
  release、bounded Host child grant、deferred Host parent release，以及并发无 overcommit 行为。
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

最终 delivery pass 最多执行一次 clean native configure、一次 full build 和一次完整 CTest/JUnit。
在源码与文档冻结前可以进行 focused validation，但不得重复最终 full gate。GitHub CI 会在 producer
job 中运行 `build-smoke`，并从同一份 packaged runtime 运行 `unit`、`integration` 与
`verification` label。它不会把 lifecycle provenance、stale-term search 或 source-quality audit
注册为产品测试。

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

`test_cpu_dense_tensor_image_operation` 是覆盖已实现 V-2 至 V-12 与 DI-1 至 DI-4 边界的
provider-independent integration binary。它的 55 个长期用例验证：

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
- 在 shared allocation 上受界限约束的正、零与负 immutable stride、彼此不同的 Value
  revision，以及对 reverse-y、broadcast-y 与 planar-channel layout 的直接 `ImageView`
  坐标访问和独立 dense-Value clone；
- immutable Value copy sharing、copy-like DenseTensorView/ImageView move，以及 lvalue/rvalue
  descriptor、layout 与 payload input 的 allocation 隔离；
- 已授权 pending-native 与已校验 opaque-imported Value 的正式发布、正式 HP cache alias
  保留、dirty reseal、replacement identity、disk reload identity 更新、cache path 不变、
  disk-save Value authority，以及 whole-read 与 regionless disk 边界对 exact-partial HP
  state 的拒绝与清理；
- 精确 descriptor-only invert inference、直接复用 sealed input 与精确 result-revision
  publication；
- V-12 浮点矩阵覆盖 1/3/4/8/16 通道 FP32/FP64 图像与 rank-one 至 rank-five
  FP32/FP64 latent，包括具有真实 padding 的 rank-one stride、独立 active-byte/padding-sentinel
  oracle、ImageRect/TensorSlice merge、CPU/external/I-O 边界的精确保留，以及在 Pending
  publication、owner retention 或 provider callback 前拒绝 negative/zero-stride external
  transfer；
- padded multi-channel full 与 ImageRect execution、负原点 ImageRect 选择所需的 signed
  data-window 坐标转换、rank-four TensorSlice、Empty/Whole
  selection、dirty-plan-to-product staging、missing 或 partial intermediate parent
  recomputation、把 selected byte merge 到 existing complete output，以及仅在 Whole commit
  后提升为 reusable authority、callback-free target/upstream Region-route transfer 与
  pre-task-population mutation rejection、在 task population 前拒绝 HP/RT ImageRect
  route switch、device-inventory drift 下由 production pruning 得到的 externally satisfied
  no-work acceptance、exact-cache dirty 与 partial-active drift rejection；execute 返回
  descriptor 与 inference 不一致的合法 Value 时，仍以 `GraphErrc::ComputeError` 拒绝。

`test_region_contracts` 拥有 31 个长期 Region case，覆盖规范 Empty/Whole、key、interval、
normalization、rank-general TensorSlice、overflow-safe clipping/algebra、可表示的单轴与
Tensor-axis union、不可表示 multi-axis union rejection、显式 budget、typed failure、
checked ImageRect/PixelRect conversion、Region propagation、route 选中的 same-key device
replacement rejection、HP/RT intent-sensitive implementation selection、Tensor planning/task
selection/edge mapping 与 Region dirty lifecycle。

`test_packed_fp4_dense_tensor` 拥有 4 个 dependency-neutral V-13 integration case。它们验证
两种 nibble order 与 nonzero bit offset、精确 encoded/scale-dequantized E2M1 access、严格
descriptor/quantization/layout/envelope rejection、block-aligned TensorSlice 对 scale/code 的
投影与 fresh identity、byte-view 与 ordinary-image-view fail-closed 行为、保留表示的 CPU 与注入式
fake-device transfer、精确正式 memory-cache retention，以及在 executor、filesystem 或 codec
副作用前发生的 typed image disk-cache rejection。Malformed matrix 包括错误 quantization
rank/count、zero 或 non-divisible block、nonfinite/nonpositive scale、错误 layout version/
alignment/overlap/size、quantized Strided publication 与 oversized blocked transfer alias。

DI-4 另有专门的 `test_dense_image_value_contracts`、`test_sample_conversion`、
`test_value_artifact` 与 `test_dense_image_processing` unit suite。IPC、Host、worker、durable、
static package-consumer、OpenCV 与普通 OpenEXR integration test 覆盖 named Value delivery、
metadata-only inspection、transactional reconstruction、artifact identity join、adapter lifetime、
彼此独立的 data/display window、精确 HALF promotion、UINT32 code value，以及对不支持 shape 或
隐式 conversion 的 fail-closed 行为。OpenEXR Deep 继续由独立的 provider-defined
variable-sample suite 覆盖。

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
  test_dense_image_value_contracts \
  test_sample_conversion \
  test_value_artifact \
  test_dense_image_processing \
  public_header_self_containment -j 2
ctest --test-dir build --output-on-failure \
  -R '^(RegionContract|RegionImageAdapter|RegionPropagation|RegionRouteSelection|RegionPlanning|RegionLifecycle|CpuDenseTensorImageOperation|PackedFp4DenseTensor|VariableSampleFieldExtensions|DenseImageValueContracts|SampleConversion|ValueArtifact|DenseImageProcessing)\.'
```

`DependencyDisabledInstallSmoke` 会在真实禁用 OpenCV/YAML/OpenEXR discovery 的 product 中构建并
运行全部 55 个 dense 用例、全部 4 个 packed FP4 用例与 17 个 V-14 extension 用例，再证明
installed consumer；
`StaticProductConsumerSmoke` 会证明 operation-SDK-only
installed consumer。`DependencyDisabledInstallSmoke` 还会加载两个独立链接且使用 Value 的
DSO，证明它们从同一个 shared runtime authority mint identity。两个 installed consumer
都会在没有 optional dependency 时构造并计算 Region，并观察同步 Ready Value fence。下述
provider-disabled nested build 也会编译并运行全部 55 个 dense case 与该双 DSO case，因此真实
core operation、fence/transfer proof 与 identity authority 都不依赖 optional OpenCV operation
provider 或 native device SDK。

## 可选 OpenCV Operation Provider 验证

`test_optional_opencv_operation_provider` 是针对两种 provider 配置构建并注册到 CTest 的
integration binary。在普通配置中，它会 seed 仓库 OpenCV provider，执行真实 resize callback，
证明无效 OpenCV matrix shape 会被翻译为 host-owned `GraphErrc::ComputeError`，再加载一个
stdlib-only ABI-v1 provider，使其只替换 resize 的 HP monolithic execution candidate。在
enabled profile 中，其余 OpenCV candidate 与 planning slot 仍保持 active，因此 `op_sources` 与
`combined_sources` 都精确报告 `mixed`；在 disabled profile 中，plugin path 完整拥有当前 active
key。随后测试会执行 replacement sentinel output、卸载该 provider，并在 enabled profile 中执行
已恢复的 OpenCV predecessor。

`test_opencv_operation_provider_exceptions` 在独立进程中运行，因此第一次 provider 初始化尝试
是确定性的。私有 `BUILD_TESTING` hook 会在真实 `std::call_once` body 内、
`cv::setNumThreads(1)` 之前注入一次 `cv::Exception`：第一次注册必须在不发布 callback 的
情况下返回 host-owned `GraphErrc::ComputeError`；下一次注册必须重试、把 OpenCV thread count
设为一并发布 provider。同一个私有且不安装的 test-access 边界会直接驱动真实 monolithic 与
tiled exception wrapper。两次相互独立的 `cv::Error::StsNoMem` 注入都必须分别表现为精确、新建
的 `std::bad_alloc`；tiled 非资源耗尽失败必须表现为 `GraphErrc::ComputeError`。测试不会尝试
真实内存耗尽，也不会修改 public ABI。

同一 binary 中的 `OpenCvOperationProviderMetadataContract.Normalization*` case 会对
`add_weighted` 与 `multiply` 同时运行真实 monolithic callback 和真实 `NodeExecutor` tiled
execution。测试锁定较小 crop secondary 产生的 raw zero padding、三到四通道 normalization 产生
的 raw 浮点 opaque alpha one，以及未改变的算术 output；同时要求排除这些常量的 uniform Sample
Domain 缺席。一个同时包含 zero 与 one 的 `[0,1]` 组合正例必须让 authority 继续进入既有
affine/product closure。`DenseImageProcessing` 还会独立锁定 unsigned-byte opaque alpha 255、
包含/排除 zero 与 255 的声明，以及一到四通道 gray replication，防止 metadata 证明误以为每个
第四通道都是合成 opaque。这些是长期 behavior test，不扫描 source text，也不替换 production
normalization helper。

同一 binary 还包含 `OpenCvRouteNormalization.*`；这些 case 不会把 direct executor helper 当作
route oracle。它们只包裹仓库中精确 selected OpenCV tiled revision 来观察 input identity，随后
驱动真实 `ComputeService` full-parallel、dirty HP 与 dirty RT route。Full case 使用 513x257
macro-tiled multiply，并要求恰好一次 inference、恰好六次 callback，以及无序精确 ROI 集合
`(0,0,256,256)`、`(256,0,256,256)`、`(512,0,1,256)`、`(0,256,256,1)`、
`(256,256,256,1)`、`(512,256,1,1)`。Observer 会复制 `ValueRevisionId`、
`AllocationIdentity` 与一个 inference invocation token；每个 callback 都必须精确匹配这三项事实，
不能只复用裸地址。Dirty blend 与 multiply case 则要求单次 HP 或 RT invocation 及 callback ROI
精确匹配对应的 pre-allocation inference identity。整个矩阵的三条 route 锁定未改变的 raw
zero/opaque 行为与 unsafe
Sample Domain omission；两条 dirty route 还会锁定 `[0,1]` 正例 retention。该 route 层负责回归
ordering/lifetime；较早的 `Normalization*` case 继续作为 direct semantic/provider oracle。

`ExecutionServiceProductResources.FullTiledContext*` 会独立验证 full-plan resource boundary。
较大的 execution-local Node 与 heap-backed static effective parameter 必须增加 retained estimate；
真实 product Run 在精确完整 vector 上 admission，而 retained 少一个 byte 时会在任一 tile entry
前拒绝。逐 owner string observation 要求一个 plan-resolved implementation key，以及每个 tile
各一个 constraint key；settled test-only observation 还会证明 context 指向 plan owner，而不是
保留第二份 implementation copy。TiledInputContext 回归另行要求删除 copy 操作、`noexcept` move，
并保留 self-pointer、Value revision 与 allocation identity。

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
`OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores`、全部 55 个
`CpuDenseTensorImageOperation.*` case、
`ValueIdentityAcrossDsos.MintingAuthorityIsProcessWide`、三个
`DiskCacheDiagnosticConcurrency.*` case，以及两个 `KernelLifecycleConcurrency.*` case。推导出的
sentinel 不得带 label 或 timeout。

DI-1 建立了 49 个用例的 dense-image 子集；Issue #130 的三个回归把它增加到 52 个用例，
Issue #132 的两个 HP/RT ImageRect route-freeze 回归把它增加到 54 个用例，
Issue #131 的 metadata-only device-local planning 回归再把当前子集增加到 55 个用例。
下列计数仍是历史 V-14 checkpoint，不是当前 inventory 算术。在该 V-14 checkpoint 中，
CMake 在该 profile 下精确注册八个
active GoogleTest target。
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

Live install-consumer CTest inventory validator 只会把同一受信映射应用于 command argv index two
处的精确 maintained driver。配置的 Python launcher 与 `-B` flag 仍须精确匹配；普通、
intermediate 或 leaf symlink、parent traversal，以及 driver basename 或 layout drift 都不会进入
可接受候选集合。

同一份受信 root inspector 还服务于三个非破坏性 consumer。
`DependencyDisabledInstallSmoke` 只有在严格 resolution 等于共享的物理拼写时，才会接受位于任一
受信 root 下的 generated manifest target；任意 intermediate 或 leaf symlink 仍会因不相等而失败。
`OpenExrDeepProviderOptionOffSmoke` 在 scrub 工具 evidence 前，只把自身精确 audit root 展开成两种
受信拼写，从而避免 smoke 自身目录名成为 OpenEXR marker，同时保留真实 dependency 名称供扫描。
它的启用态 companion 只有在 physical-prefix confinement 与 prefix 之后的 component 检查拒绝
后续每个 symlink 或 escape 后，才会接受使用任一受信拼写的 generated provider target。
`InstallConsumerArchitecturePropagationSafety` 会注入 synthetic mapping，在不触碰 `/tmp` 的前提下
跨平台锁定双向拼写、manifest acceptance、root 之后的 symlink 与 escape 反例、双拼写 evidence
scrub、真实 `-lOpenEXR` rejection、alias 拼写的 `libImath.dylib`/`libOpenEXR.dylib` rejection，
以及 enabled-provider containment。

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
实现 runner 或 collector。Issues #93 至 #96 现在已经提供各自负责的 source-private I1、I2、
B1 与 M1 mechanism、封闭 inner evidence、correctness test，以及显式 exact-workload 手工
runner。#96 还实现现有 canonical 15-field row/five-field bundle materializer 与
exact-one/DAG validator。本地物化的 M1 row 本身并不是 conformant mixed corpus：每个已命名
isolated/comparison object 与完整 live environment authority 都必须能够解析，任何缺失
prerequisite 都保持 canonical `Invalid`。构建 runner 或通过 correctness test 都不构成机器
画像声明。

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

每个携带 workload 的 field 与 fixed-record component 都使用封闭且区分大小写的
`workload-id-v1` scalar。它只接受上表四个 token，精确 frame 分别为
`16:I1-edit-storm-v1`、`17:I2-progressive-v1`、
`15:B1-immutable-v1` 与 `12:M1-shared-v1`。通用 `identifier` 仍是独立的
lowercase ASCII type，匹配 `[a-z0-9][a-z0-9._+-]*`；两个 validator 都不得借用
对方的 domain。强制 lexical/type oracle 如下：

| 声明的上下文 | 输入 | 预期 |
| --- | --- | --- |
| `workload-id-v1` | 四个精确冻结 token 中的每一个 | 分别接受全部四个。 |
| `workload-id-v1` | `i1-edit-storm-v1` 等 lowercase/case-changed alias | 拒绝；workload token 区分大小写。 |
| `workload-id-v1` | `I3-edit-storm-v1` 等未知 token | 拒绝；domain 是封闭的。 |
| 通用 `identifier` | `abc`、`a0` 或 `a0._+-` | 按未改变的 lowercase grammar 接受。 |
| 通用 `identifier` | `I1-edit-storm-v1`、空输入或 invalid leading byte | 按未改变的 lowercase grammar 拒绝。 |
| Evidence `workload_id` field | 精确 token 与 type frame `10:identifier` | 拒绝；必需 type frame 为 `14:workload-id-v1`。 |
| 非 workload identifier field | `workload-id-v1` type frame 或 workload token | 拒绝 schema/type 混用。 |

Known I1 evidence row 或 bundle 的 workload field 以精确 record
`field=11:workload_id5:known4:none14:workload-id-v116:I1-edit-storm-v1\n`
开始。`job-instance-v1` 与 `row-reference-v1` fixed record 则保留相同的首个
component payload frame，并按专用类型进行校验；其 fixed-record payload 不包含
component type token。聚焦 wire oracle 必须让四个 workload token 通过
job-instance、15-field row、五 field bundle 与 row-reference encoding round-trip；
必须拒绝任何大小写、未知 token、type-frame 或 enclosing-workload mismatch；并从
纠正后的 canonical byte 复算 row/bundle digest。此前的 `identifier` annotation 从未
描述有效的 uppercase-leading v1 object。

每一行使用三个全新的 process/execution-domain replicate。Cold first-use 单独
采集，并排除在 steady-state aggregate 之外。自然 edit ordinal `1..12` 映射为
`edit_index=0..11`；必需 final 是第十二次 edit（`edit_index=11`、`k=1.04`、source
Region `(768,512,256,256)`、preview Region `(192,128,64,64)`）。裸写“edit 12”
不是 v1 evidence identity。

I2 继承的不只是 edit label。对于每个 `edit_index=i`，它使用精确的 I1 第一个 node
序列
`K=[0.82,1.18,0.86,1.14,0.90,1.10,0.94,1.06,0.98,1.02,0.96,1.04]`、
相同的 `edit_index=edit_ordinal-1` lookup，以及相同的第一个 node update，随后按
node one 至 node four 的顺序执行 transform，其 `k` 值为
`[K[i],1.00,1.20,1.40]`。Preview 先从原始 2048 source 逐 channel 计算 4x4 box
average，把该 source 只舍入一次到 binary32，再执行共享 update/transform sequence。
Final 从原始 2048 source 开始，并执行相同的 I1 full-resolution path；它不是通过
upsample、复用或其他方式从 preview 派生的 Value。

强制 I2 coefficient/path scenario oracle 如下：

| 场景 | Oracle |
| --- | --- |
| 精确匹配十二个 index | 枚举 `edit_index=0..11`，要求 I2 第一个 node 的值逐元素等于 I1，即 `[0.82,1.18,0.86,1.14,0.90,1.10,0.94,1.06,0.98,1.02,0.96,1.04]`，且 ordinal、source Region、preview Region 与 generation index 相同。 |
| 单个 coefficient 漂移 | 替换任意一个 `K[i]` 却保留 `I2-progressive-v1` 都是 invalid；有意改变的 fixture 必须使用新的 workload id。 |
| Sequence 重排或 index 偏移 | 交换 entry、直接把 `edit_ordinal` 当作从零开始的 array index、移动/循环 array，或把另一 index 的 coefficient/Region/generation 配在一起，在 v1 下都无效。 |
| Preview rounding order | Preview oracle 对原始 source 逐 channel 执行 4x4 box average，只舍入一次到 binary32，再以 `K[i]` 执行 node one，并依次执行 node two 至 node four。在 transform 后、transform 之间进行舍入，或从已经 transform 的 pixel 计算 preview，都无效。 |
| Full-resolution final path | Final 从原始 2048 source 开始，执行与 I1 相同的 `K[i]` update 与四个 transform，绝不通过 upsample 或复用 preview pixel 生成。 |
| Digest 与 golden linkage | Manifest 绑定完整 coefficient/mapping/order/rounding/path 契约；必需的 `edit_index=11` final logical digest 等于 I1 index 11，preview 等于其自身 fixture golden。任何 mismatch 或 manifest drift 都使 v1 row 无效。 |

强制 I2 cadence scenario oracle 如下：

| 场景 | Oracle |
| --- | --- |
| 连续 phase grid 与 measured origin/index | 为全部 111 个 episode slot 保留唯一 replicate-grid origin `G^I2`：cold 从 `G^I2` 开始，warmup 从 `G^I2+1*1,500,000,000 ns` 开始，measured 从 `E^I2_0=G^I2+11*1,500,000,000 ns` 开始，不启动 episode 的 terminal boundary 为 `T^I2=G^I2+111*1,500,000,000 ns`。在 measured 中，把 `episode_ordinal=1..100` 映射为 `episode_index=0..99`，并按 `E^I2_r=E^I2_0+r*1,500,000,000 ns` 派生每个 origin；拒绝任何新选的 episode/phase origin 或 transition delay。 |
| 区分 phase 的 replicate aggregate | 在全部 111 行上聚合 memory 与 output。对于 latency 与 waste，cold slot zero 与 warmup slot `1..10` 只传播 Invalid；它们的 Pass/Fail verdict、sample 与 service 绝不进入 steady state。Measured slot `11..110` 贡献完整 verdict、精确 100 个 endpoint pair 与精确 100 行 service。Non-measured Fail 在这两项上被忽略，non-measured Invalid 仍为 Invalid，measured Fail 仍为 Fail。 |
| 十二次 edit admission schedule | 对每个 episode 与 `0..11` 中的 `edit_index=i`，要求 `S^I2_{r,i}=E^I2_r+i*16,666,667 ns`，且唯一 preview Host-admission sample `A^I2_{r,i}` 位于封闭区间 `[S^I2_{r,i},S^I2_{r,i}+2,000,000 ns]`。 |
| 单个无效 edit event | 把一个 admission 移到 nominal start 之前或 lateness bound 之后，遗漏/重复/重排/令一个 admission 失败、触发 checked-arithmetic overflow，或插入一个 cadence gap；replicate 无效、publication 被撤销，且任何 edit 或后续 episode 都不得追赶、回填或移动。 |
| Episode spacing 与 quiescence | 要求相邻 origin 精确相差 1,500,000,000 ns，且前一 episode 的全部 work 在下一 origin 前 quiescent；最后一个 measured episode 必须在 `T^I2` 前 quiescent。最晚合法第十二次 final deadline 为 origin 加 1,185,333,337 ns，留下最少 314,666,663 ns 且绝不延长 deadline 的 guard。 |
| Preview 与下一 edit | Edit `0..10` 绝不等待 preview。Preview `i` 只有在 visibility 严格早于 `A^I2_{r,i+1}` 时才可发布；二者相等时，更新 edit acceptance 先排序，该 preview 成为 stale。 |
| 共享 child-deadline anchor | Preview 与 final deadline 分别 checked-add 为 `A^I2_{r,i}+100,000,000 ns` 与 `A^I2_{r,i}+1,000,000,000 ns`。保留较晚的 final trigger/admission，但绝不重新锚定 deadline；第十二次 preview/final 必须在各自 bound 前可见。 |
| 既有 envelope evidence | 在既有 workload-manifest section 中保留 clock/replicate-grid/派生 phase-origin/index/schedule/tie rule，在 measurement evidence 中保留全部 actual admission/deadline/visibility/cancel/drop/gap/quiescence event。复算其 section/verdict digest，同时保持封闭 15-field row 与五 field bundle。 |
| Manifest/golden drift | 即使 image golden 匹配，也拒绝 `I2-progressive-v1` 下的任何 origin/stride/cadence/order/lateness/anchor/tie-rule 漂移；有意改变必须使用新的 workload id 与 manifest/digest/golden lineage。 |

对 I1，baseline 结算后，位于 monotonic origin `E` 的 episode 使用
`S_i=E+i*16,666,667 ns`。`A_i` 是 final Host admission 前立即取得的唯一
monotonic-clock sample；它启动 latency sample，通过 checked addition 得到唯一
absolute Run deadline `D_i=A_i+150,000,000 ns`，并在该 call 成功时作为规范的
admission/acceptance timestamp。Runner 在校验 `A_i` 后、调用 Host 前预留一个唯一且
严格递增的 row-local `event_sequence_i`。成功时产生 accepted 逻辑 event coordinate
`(A_i,event_sequence_i)`。Runner 会在 Host invocation 前通过 private Host/Kernel request
传递 typed proposed coordinate；Kernel 会在 current publication 前把它绑定进 product
`SupersessionIdentity`，current observation 会复制精确 binding。已绑定 replacement 要求
只由 accepted coordinate 推进；generation 保持非零且唯一，但因为记录 preparation 顺序，
其数值可以向后移动。任一侧未绑定的 traffic 仍按 generation 排序。Accepted-row 与
observer-causal sequence allocator 相互独立，并各自从一开始。Host return
timestamp/status 绝不替代该 coordinate。
Failure 不产生 accepted event、current observation 或 accepted product binding，会使
replicate invalid，也不能合成或回填其他 timestamp；proposed coordinate 与 failure/return
事实保留在既有 inner evidence 中，不改变 15/5-field envelope。`A_i` 必须处于
`[S_i,S_i+2,000,000 ns]`；nominal `S_i` 绝不锚定 deadline，允许的 wake lateness
也不消耗 150 ms budget。Overflow、提前启动、迟于 2 ms、miss/drop/gap 或
admission failure 都会使 replicate 无效。Runner 在任何迟到 Host call 前请求
cancellation/supersession 并记录其被接受、撤销 publication，并且不会追赶、回填或
移动后续时刻。已进入的 non-preemptible work 按 waste drain；post-cancel start
为零，missed/expired work 不能发布 output、receipt 或 successful latency。具体而言，
无效 admission result 会同步关闭 Graph，以撤销该 episode 的 publication 并取消、drain
较早 generation。随后 runner 会采集关闭后的 observation/lifecycle/resource 状态，消费
此前已接纳且 ready 的 settlement，并先 flush 一条四项 verdict 均为 Invalid 的 inner row，
再中止全部后续 edit 与 grid slot。如果发生过 Host call，失败 edit 会保留真实 pre-call
sample、reserved sequence、deadline 与 raw Host return，但没有 accepted coordinate、
current observation 或 accepted product。Abort 后未到达的 fixed-width edit 使用
`admission_attempted=false`、null admission sample，且不携带 call facts。

Embedded Host 会在这条 success-only boundary 前关闭更窄的 resource-admission race。Public
与 I1 call 都会在进入 Kernel 前预构造 caller promise/future、成功 result envelope、backend-
delivery bridge、已 join 的 status worker，以及 close-visible tracking。确定性的 source-private
injection 会在最后一个 pre-Kernel point 触发。Test oracle 要求这次失败返回 invalid caller future、
不调用任何 product source callback、不发布任何 current/product/lifecycle observation，并允许下一次
未注入的 I1 request 成为 generation one 且正常 settlement。这是 common Host path 的 correctness
test，不是模拟 I1 collector return。

强制 I1 phase/drain scenario oracle 如下：

| 场景 | Oracle |
| --- | --- |
| 连续 isolated phase grid | 保留唯一 `G^I1`；派生 cold slot zero、warmup slot `1..20`、measured slot `21..220`，并且只把 `T^I1=G^I1+221*750,000,000 ns` 作为 terminal non-start boundary。把每个 phase 的自然 ordinal 映射为从零开始的 `r`；拒绝 fresh phase origin、cooling delay、shifted slot 或迟到的 counter reset。 |
| 成功的 accepted-boundary coordinate | 校验每个 `A_i` 后，在 Host invocation 前预留其唯一 row-local `event_sequence_i`，并通过 private Host/Kernel request seam 传递 `(A_i,event_sequence_i)`。成功时要求 product supersession identity 与 current-generation observation 包含这一精确 coordinate；拒绝把 Host return timestamp/status 或 observer callback time/sequence 用作 deadline、current-generation、supersession、tie-order 或替代 binding coordinate。 |
| 反向 preparation 与 accepted 顺序 | 使用确定性的 Kernel barrier，让较新的 accepted coordinate 准备 generation one 后暂停，较旧的 coordinate 准备 generation two 并先发布 current，然后恢复 generation one。要求较新的 coordinate 成为 current、取消 generation two、保持为唯一 visible output，并保留同 timestamp 的 row-sequence 排序。重复执行反向逻辑断言，证明较旧 coordinate 即使拥有更高 generation 也不能替换 current。要求 coordinator-managed native freshness 保留精确发布的 generation 并拒绝数值更高的 stale transfer；要求 unbound 与 bound/unbound 两个 mixed 方向都保留 generation ordering。 |
| Accepted 与 causal sequence domain | Row-local accepted sequence allocator 与 observation sink causal sequence allocator 各自从一开始。要求二者只在各自 domain 内严格有序；绝不能通过两个 sequence 的数值相等推断 row/product binding。 |
| 失败的 admission 不产生 accepted event | Host failure 时，把预留的 proposed coordinate 与 failure/return observation 保留为 raw inner evidence，使 replicate invalid，并要求不存在 accepted-admission event、current-generation observation、product binding、替代 timestamp、backfill 或 outer schema field。 |
| 已准备的 Host resource failure 不能进入 Kernel | 在所有 caller-side async resource 与 close tracking 已完成 preparation、但即将进入 Kernel 前注入失败。要求 public request 不调用 source callback；要求 I1 request 不暴露 current、cancellation、start、terminal、quiescence、resource-return、visibility 或 Host-settlement observation。关闭 injection 后，要求下一次 request 以 generation one 成功。 |
| 每次 edit 的 expiry 都会关闭 publication | 要求每个 intermediate visible output 不晚于其自身 `D_i`；更晚的 intermediate publication 与冻结的 product/workload contract 冲突，并使 row invalid。必需的 twelfth output 若晚于自身 deadline，仍属于证据完整的 latency-gate failure。 |
| 精确 drain anchor | 每个 episode 要求 `Q_start=S_11=E+183,333,337 ns` 与 `Q_end=Q_start+500,000,000 ns=E+683,333,337 ns`，不受 actual admission 或 deadline 变化影响。Window 可以与 active final Run 重叠，但不会取消它或延长 `D_i`。 |
| Deadline 与 next-origin guard | 在最晚合法 admission 下，要求 `D_11<=E+335,333,337 ns`、从该 deadline 到 `Q_end` 精确 348,000,000 ns，以及从 `Q_end` 到下一 origin 精确 66,666,663 ns。Reset/baseline preparation 必须容纳在该 guard 中；最后一个 measured episode 在 `T^I1` 前使用相同 guard。 |
| Boundary tie 与 settlement | 在 `Q_start`，nominal marker 先于同 timestamp admission；在 `Q_end`，从 product transition 使用的 observation sink causal allocator（而非 accepted-row sequence allocator）中预留首个被排除的 coordinate。只有 timestamp 不晚于 `Q_end` 且 causal sequence 位于 cut 之前的 event 才属于该 history。任何缺失或更晚的 terminal/quiescence/root-resource/Host settlement 都是 invalid；eventual snapshot 不能回填。 |
| 独立最终 golden | 不经过 Host、Kernel、cache、scheduler、YAML 或候选 provider code，独立重算 coordinate-pattern source 与四个显式 binary32-RNE curve stage。要求 version 为 `i1-coordinate-pattern-curve-chain-fp32-v1`、required 零原点 `[0,2048) x [0,2048)` 有符号 data window、缺席的 optional display window（其 presence flag 参与 canonical descriptor digest）、DenseTensor schema/Image facet 结构版本为 2、在非线性 curve chain 后按 operation-specific 规则省略未经证明的 Sample Domain 与 Color 权威，且精确 `Sha256CanonicalV1` digest 为 `18d88b59782daa7ef92b0aa2acc23c7fec5e61baa5e631d9c1c4c8b6abc2eed0`，再交叉校验一个精确 2048 真实产品结果。Source 仍必须声明 FP32 Normalized `[0,1]`。expected evidence 缺失或被替换属于 Invalid；候选不匹配属于 Fail。 |
| Guard-safe evidence finalization | 在 `Q_end` 前对每个 visible output 只计算一次 digest，冻结其类型化 result，并释放其 `Value`。evaluation 与 JSON 不得重新计算 hash。最多允许一个不含 Value 的 evaluator 与下一 baseline preparation 重叠，要求在 admission 前完成，并把有序 JSON/durable I/O 延迟到 `T^I1`，或延迟到撤销 later submission 的 abort。 |
| 逐 Run causal closure | 每个 materialized edit 必须使用唯一 Run id，且精确具有一条 terminal/quiescence/resource/Host chain；cancellation 与 visible publication 各自至多一次，只有 Cancelled 才有 cancellation，只有 Succeeded 才有 visibility，Host status 必须与 terminal 一致。Current generation 必须早于每个 service start，每个 start 必须早于 terminal，visible 必须先于 successful terminal，随后严格为 terminal、quiescence、resource return、Host settlement。不可逆 service-start commit 与 cancellation acceptance 共用 Run-owned terminal arbiter，service-start observation 在 service/Run lock 外投递。`cancellation < start < terminal` 是结构上有效的证据，但会使 Waste 失败；产品路径必须阻止它。 |
| 无缺口 service-start capacity | 从冻结 curve node 的 Macro256 切片派生每个 node 64 个 tile，从一个 monolithic source 加四个 curve node 派生每个完整 Run 257 个 start，并派生每个十二次 edit episode 3,084 个 start。确定性证明 pre-route 两个方向的 start/cancel 顺序、cancellation 获胜时 route/executable 零泄漏、route commit 获胜时 start coordinate 更小、暂存权威可回滚/复用、第 3,084 个 start 仍成功，以及第 3,085 个 start fail closed。 |
| Fail-closed arithmetic/evidence | 拒绝 grid/slot/start/admission/deadline/drain checked overflow、boundary/event evidence 缺失或重复、moved origin、nonquiescence，或同 workload id 下的 manifest rule drift。既有 section/verdict digest 绑定 evidence，不改变 15/5-field envelope。 |

DI-1 改变的是 DenseTensor schema 与 Image facet 结构记录，而不是
`Sha256CanonicalV1` 算法标签或 workload 算术。因此，冻结的 I2 preview logical
digest 为
`2af5a5b2e88646c541a60a7b437194f16d1bc2c34ff20bc571d37bfd3cac3ae2`。
Source Sample Domain 在非线性 chain 前仍具权威，但 output oracle 会省略它。因此当前
I1 digest 与历史 source 修复前的 digest byte 相同，却具有不同且现在显式的原因；
provider/source 与 product-path test 会分别证明修正后的 source 与 output 权威。全部 34 项已编译 B1
logical golden 都按相同 curve-output 省略规则由独立 oracle 重新生成；其 raw-payload
SHA-256 值以及 I1、I2、B1 workload 标识保持不变。

M1 对 `r=0..39` 使用相同逐 episode drain 规则
`E_r=M_0+r*750,000,000 ns`，共精确启动 40 个 measured episode，并持续提供
cap-eight B1。这是可复现的 nominal monotonic time 与 lateness bound，不是精确
OS wake 的声称。

Disk-cache/codec I/O 与跨 episode/job result
reuse 保持禁用。I1/I2 只保留显式重新计算的 baseline/current episode target 与
已声明的 I2 output residency；每个 B1 job 开始时都没有可复用 fixture result。
Warmup B1 job 用独立 identity/directory 执行完整 artifact path；owner 结算后
移除 output，同时保留 process/provider/JIT state。Warmup occurrence-owned
observation 绝不进入 measured aggregate；M1 boundary 在不重启进程或重置 state 的
情况下重置 logical counter。

强制 M1 pre-boundary input-grid scenario oracle 如下：

| 场景 | Oracle |
| --- | --- |
| 精确 phase origin 与 interval | 经过 checked arithmetic 派生 `C^M1=B^M1-6,000,000,000 ns` 与 `W^M1=B^M1-5,000,000,000 ns=C^M1+1,000,000,000 ns`，保留 sequence `c^M1<w^M1<b^M1`，并精确使用 `[(C,c),(W,w))`、`[(W,w),(B,b))` 与 `[(B,b),(U,u))`。Underflow、overflow、boundary 移动、phase 时长不同或 runner 自选 origin 都是 invalid。 |
| Cold origin 与 settlement | 在 `(C^M1,c^M1)` 建立唯一 cold I1 origin，并在 boundary marker 后 offer Graph A seed 252，identity 为 `(phase=cold,cycle=0,attempt=0)`；同 timestamp 的 I1 admission 排在该 offer 之后。要求 I1 `Q_end=C^M1+683,333,337 ns` first-excluded history cut，以及 B252 terminal/owner settlement/output removal 全部位于 `W^M1` 前；固定 316,666,663 ns I1 guard 不移动 `W^M1`，miss 直接 invalid，而不是执行 drain。 |
| Warmup origin 与 count | 在 `(W^M1,w^M1)` 验证 cold 已经 settled，并建立精确 `E^M1_warmup,k=W^M1+k*750,000,000 ns`，其中 `k=0..6`。遗漏/重复 origin、不同 count/index、从 `C^M1` 倒推的跨 phase 连续 grid 或 delayed transition 都应拒绝。 |
| 固定 warmup B1 offer protocol | 在 `W^M1` 先 offer B253、再 offer A254，满足 `w^M1<sequence(B253)<sequence(A254)`，并使用 warmup cycle/attempt zero；同 timestamp 的第一个 I1 admission 排在两者之后。只有 B253 terminal 时才同步 offer B255，并使用更大的 same-time sequence；B255 必须在 `(B^M1,b^M1)` 前已经 offered。Graph A 在 A254 后没有 warmup successor。Offered prefix 由 protocol 固定，只有 incomplete subset 由 terminal history 派生。 |
| 确定性跨 `B^M1` I1 | Warmup origin `k=6` 精确为 `B^M1-500,000,000 ns`，且 `Q_end=B^M1+183,333,337 ns`；要求该 settlement-pending warmup occurrence/generation 及其第十二次 edit publication 出现在 `B^M1` snapshot 中，且该 publication 仍为 current。它持续 current 到首个 measured edit 仅成功时存在的 accepted coordinate `(A_0,event_sequence_0)`。必须在 protocol 提前返回前，通过共享 producer/reader projection 从保留的 final-warmup 与 measured-zero Issue #93 source 推导完整 first-admission/current-hold record；禁止信任 runner 创作的副本。在未改变的 `Q_end`，只要求该旧 occurrence/generation 达到 quiescence 并 settled，不要求并发 measured generation 或整个 shared service 为空。 |
| 不可变 attribution 与 temporal effect | 最后一个 warmup generation 拥有的每个 event/result 都保持 `phase=warmup`，包括 measured latest-wins supersession 后产生的 cancellation 或 settlement。其 occurrence-owned value 不进入 measured aggregate，但 `B^M1` 后每个 start、contention、reservation/grant、Compute I/O 与 high-water effect 都进入按时间 window 归属的 evidence。 |
| 无隐藏 transition | Cold/warmup transition 不得 pause、wait、cool、restart、rebuild queue、release shared resource 或移动 boundary。把全部 origin/count/index、固定 offer、由 terminal 派生的 B255 transition、phase endpoint 与 failure 保留在既有 workload-manifest/measurement section 中并复算 digest，不新增 outer field。 |

强制 M1 phase-boundary scenario oracle 如下：

| 场景 | Oracle |
| --- | --- |
| 精确 boundary 与 interval | 保留 boundary coordinate `(B^M1=M_0,b^M1)`、经过 checked arithmetic 的 terminal-cutoff coordinate `(U^M1=B^M1+30,000,000,000 ns,u^M1)`，以及唯一且严格递增的 row-local event sequence。按 `(monotonic_timestamp,event_sequence)` 排列相等 timestamp；measured interval 是 `[(B^M1,b^M1),(U^M1,u^M1))`。 |
| 有序零时长 transition | 在 `(B^M1,b^M1)`，以 atomic transition 关闭 warmup I1 cadence 与两个 B1 Graph producer，对此前已经 offered 但未完成的每个 warmup I1/B1 occurrence/state 取得 snapshot，只重置 logical measured accumulator，并建立 measured I1 origin。随后在 timestamp `B^M1` 依次 offer measured Graph A job zero 与 Graph B job one；二者均为 producer-local cycle zero，sequence 严格大于 `b^M1`。Snapshot/reset 中不得插入其他 event，也不得 pause/wait/cooling/drain/boundary cancellation/restart/queue rebuild/resource release。 |
| Supersession 顺序 | 把第一次 measured I1 call 绑定到 `edit_index=0`；在 Host invocation 前采样 `A_0` 并预留 `event_sequence_0`。成功时精确产生 `(A_0,event_sequence_0)`，满足 `B^M1<=A_0<=B^M1+2,000,000 ns`；只有该 coordinate 可以让 measured I1 成为 current，并以普通 latest-wins supersede 最后一个 warmup generation。若 `A_0=B^M1`，要求其 accepted-row sequence 排在两次 measured B1 offer 之后。当 product current 与被替换 cancellation 都在 B 被观察到时，使用二者独立的 replicate-wide observer sequence：current `(B,n)` 后接 cancellation `(B,n+1)` 时保持 current hold；cancellation 不晚于 current 时 fail closed。绝不把该 observer sequence 与 `event_sequence_0` 比较。Missing、failed、early 或 late admission 都是 invalid；failure 不产生 accepted event，Host return time/status 保持为 raw evidence。拒绝任何更早 supersession、phase-only cancellation、替代 coordinate 或 snapshot rewrite。保留旧 generation 的固定 `Q_end`，使成功 acceptance 后剩余 `[181,333,337 ns,183,333,337 ns]`，并让之后每个 cancellation/terminal/settlement 保持 warmup attribution，同时把 boundary 后的物理 effect 保留为 measured-window evidence。Issue #93 仍独立拒绝同时具有成功 visibility 与 accepted cancellation 的 Run。 |
| Carryover identity 与 FIFO | 保留 warmup phase/cycle/job/attempt、queue predecessor、admission state、reservation/grant 与 owner settlement。即使仍 queued/running，measured cycle-zero offer 也排在每个 Graph 已经 offered 的 warmup prefix 之后；只有该 transition 可以绕过 predecessor-terminal offer timing。后续 measured offer 恢复普通 per-Graph 规则，绝不推进或改写未完成的 warmup identity。 |
| Occurrence attribution | 按不可变 phase 归属 terminal/completed service、output byte、latency、receipt/golden/digest、determinism、retry/duplicate/discarded service、waste 与 settlement。把 `B^M1` 后的 warmup occurrence-owned quantity 从 measured throughput、Jain service `x`、latency、determinism 与 waste aggregate 排除。 |
| Temporal scheduler/resource effect | 包含 boundary 后每个 phase 的 actual class start、headroom failure、queue contention、reservation/grant、Compute I/O state 与 Host/device/ready-memory high-water。Measured class-start rule 计算 warmup Throughput start，而 Jain completed service 只使用 measured occurrence。 |
| Failure 与 terminal settlement | Warmup carryover failure、event evidence 缺失、event sequence 重复或 coordinate 无法形成全序、phase/identity/FIFO rewrite、boundary-only cancellation、source-derived first-admission/current-hold mismatch、snapshot mismatch 或无法证明 settlement 都是 invalid。在 `(U^M1,u^M1)` 停止新的 measured offer，但不取消 outstanding work；保留排在 cutoff 或其后的 endpoint，但从 30-second numerator 排除，随后要求 exact-zero teardown。`B^M1` 不要求 quiescence。 |
| 既有 envelope evidence | 在既有 manifest/measurement section 与 digest 中保留 `C^M1`、`W^M1`、`B^M1`、`U^M1`、全部 phase interval/origin/count/index、固定 pre-boundary offer、由 actual terminal 派生的 transition、tie/step order、carryover snapshot、phase join、首批 measured offer、per-Graph predecessor/next-cycle counter、counter epoch、queue/start/terminal/receipt event、resource effect、failure 与 final settlement。任何有意规则变化都需要新 workload id；outer row/bundle field 保持 15/5。 |

v1 resource profile 是 32 个 CPU slot、1 GiB Host retained memory、512 MiB
Host scratch、65,536 个 ready entry 与 256 MiB ready byte；Interactive headroom
为 1 个 CPU slot、64 MiB retained memory、32 MiB scratch、1,024 个 ready entry
与 16 MiB ready byte。Compute I/O 准入上限为 64 个 task 与 256 MiB 计划字节总量。
配置 Metal executor 时，device memory 与 device scratch 分别为 512 MiB 与
256 MiB；Metal 缺失属于预定义 `not-applicable`。

对 B1 fairness 证据，只要 Graph producer 仍有未消费 offered demand 且没有暂停
提交，该 Graph 就是 eligible，其中包括 bounded-admission wait。Isolated B1 在其
measured boundary 提供两个有序的 15-job queue。M1 使用上面的 boundary oracle：
首批逐 Graph measured offer 排在保留的 warmup prefix 之后，Graph A 随后重复
`0,2,...,28`，Graph B 重复 `1,3,...,29`。每个 producer 在自己的最后一个 job
terminal 后立即开始自己的下一轮 local cycle，即使另一个仍在上一轮 local cycle。
Cross-Graph barrier 或 producer gap 都是 invalid。两条路径都不会绕过正常 bound
准入全部 30 个 Run。

每个 B1 occurrence 都通过 canonical `job-instance-v1`
`(row_workload_id:workload-id-v1,replicate_ordinal:uint64,
phase:enum(cold|warmup|measured),cycle_ordinal:uint64,job_index:uint64,
run_cap:uint64)` 建立 index。Phase-local cycle zero 覆盖 cold/warmup 与 isolated
measured B1。对 measured M1，未改变的 `cycle_ordinal` component 存储 producer-local
counter，并由 job parity 推导 lane：Graph A 在 job 28 terminal 后递增并立即 offer
下一 local cycle 的 job zero；Graph B 则独立地在 job 29 后递增并 offer job one。
Logical I/O task 增加 stage，完整 task
identity 再增加 `attempt`。Capacity rejection 或幂等 duplicate 保持 attempt zero
与相同 charge；只有 terminal failure 后的显式 retry 才增加 attempt。Cycle 绝不
表示 retry。Charge、admission/status、snapshot、start/terminal、commit id/slot、
receipt、raw trace 与 row evidence 都必须携带完整 job-instance identity。Normalized
semantic trace 继续按 job index 编码，并通过 row job-instance index 把其 digest join
到每个唯一 occurrence。

对每个 B1 output stage，capacity rejection 以不变的 attempt-zero identity 与 charge
重新 offer，总 admission attempt 最多为 64 次。Test oracle 对 attempt 计数，绝不从
time、sleep、polling 或 observed availability 派生该 bound。Non-capacity rejection
或第 64 次 capacity rejection 必须返回 `AdmissionFailed`，删除不完整 slot，追加一条
`Final` observation，并停止 offer 该 stage。

每个 B1 job 在所选且已指纹化的 `OutputStore` root 下的全新可丢弃目录中写入
ADR 0010 规定的精确
`output.rgba32le` payload 与固定顺序 `manifest.txt`。两个有序
`ComputeIoExecutor` task 使用稳定
`(job_instance_id,stage,attempt)` charge identity：payload-stage 的
`planned_bytes=67,108,864`，manifest-commit 的 planned byte 是该 job 的精确
`242 + decimal_digit_count(job)` manifest 长度：job `0..9` 为 243 byte，job
`10..99` 为 244 byte，job `100..255` 为 245 byte。因此 measured job `0..29` 使用
243 或 244 byte，cold/warmup job `252..255` 使用 245 byte。每个 offer/settlement 都
必须保留 executor 在与 charge/release 相同的 mutex 下签发的不可变 event。Event 提供
单调非零 sequence、精确 charged/released task 与 byte delta、适用时关联的 admission
identity，以及结果 process-global snapshot。采样必须证明 active task <=64、active
planned byte <=268,435,456、保留两种 high-water，并证明当前 task 精确 settlement。
Global snapshot 可以包含无关并发 job，也可以在某 job 的 `Final` 时保持非零；row
boundary 仍必须结算到要求的 process baseline。每个 active planned-byte total 都是
真实 admitted charge 的 checked sum。Planned byte 与单 task event 是 Compute I/O
admission、planned-byte high-water 与 task settlement 的强制性权威证据，但仍是
estimate，不证明 physical memory ownership，也不能替代 RSS 或 ledger/device
ownership evidence。

必须把 retained event stream 验证为精确状态机：`Initial` 最先；payload offer/
admission 后接 settlement；manifest offer/admission 后接 settlement；`Final` 最后。
Capacity-rejection row 只能在当前 offer state 重复，并保持 attempt zero 与相同 charge。
检查每条 row 的 job、stage、attempt、planned byte、admission/completion status、精确
event delta/linkage/sequence 与 snapshot limit/phase total，以及 terminal I/O path 与
output status/receipt 的关系。Accepted admission 必须 charge 一个 task 与 offered byte，
其 settlement 必须 release 同一 charge，rejection 必须 charge 零。缺失、重复、重排、
stage/job/status 错误、attempt gap、undercharge、伪造 accepted-zero、settlement linkage
错误、无效 event/snapshot 与 `Final` 后 mutation case，必须使 B1 四个 axis 全部为
`Invalid`。

目标 `OutputStore` 请求并且必须达到 typed `crash-durable`；它结算 payload，最后
以 no-replace 方式发布 canonical manifest，完成全部 leaf-to-root barrier，然后返回
ADR 0009 receipt。较弱、不支持或失败的 durability 都会使结果无效。一个 B1 job
的 commit id、rooted no-replace slot 与 receipt 会绑定完整 job instance 及其 fixture
job index。Store 持有 root 与 slot directory descriptor，并相对于它们执行每个
slot/payload/manifest mutation、barrier、revalidation 与 cleanup。Root pathname
replacement 或 symlink substitution 必须 fail closed，不得产生 redirected artifact。
Store 还会持有 nonblocking advisory exclusive root lock；协作进程/线程必须遵守它，并把
B1 staging 与 occurrence name 保留给单一 store owner。Slot 创建后的 exception guard 必须
先 cancel/wait accepted work，再进行 cleanup，证明精确 charge 已退休，两次检查每个已记录
identity，并检查每次 removal 及随后 absence。由于 POSIX 会把最终 identity recheck 与按 name
删除分开，删除保证仅限该协作式 exclusive-owner contract；不协作 same-UID mutation 不在
threat model 内。Guard 建立前的 anchor handoff failure 必须保留含义不确定的 residue，且不得
声称可重试。确定性 handoff oracle 会从 job 的 commit identity 推导精确 private anchor 与 slot，
绝不扫描 staging prefix。Slot-replacement 测试会先把原 slot rename 到显式 displaced path，再
创建 replacement，从而让原 inode 保持存活，使结果不依赖 Darwin/Linux 的 inode reuse 行为。
在该前提内，只有 checked removal 与观察到 absence 后才允许 exact-identity
retry。只有在该 receipt
和 logical/raw 两种 golden check 后才贡献 throughput。每个 I2
第十二次 edit（`edit_index=11`）preview/final 都通过相同 Host
binding 获取两次。已配置 Metal device 允许每个不同 preview/final revision 的
首次 access 执行一次精确大小的 upload；第二次必须命中相同 residency。该命中不是 broad
revision/device lookup：resident 保留其完整 publication identity，一个
持有 manager lock 的 `PublishedValueAcquisition` lookup 会验证仍存活的 managed lineage、完整
seed/use、source Ready identity、已保存的 publication 与 resident Ready identity。
Lookup-before-lineage-retirement 返回合法的 immutable copy；retirement-before-lookup 即使普通
broad lookup 仍能找到该 entry 也会拒绝。禁止 CPU copy、readback、disk/codec access 或额外
transfer。复制第二次 access、diagnostic、resource 与 no-I/O fact 后，只要 managed lineage 仍
存活，已经 Ready 的 immutable Value 即使在较新 generation 已 current 时仍可被获取；该
verification acquisition 不修改 currentness，也不放宽精确 seed/revision/binding/producer/
fence 检查。Host 会在最终 row snapshot 前，按精确 revision、完整 binding 与 producer identity
只释放该 row 的 resident。错误 identity 不释放任何内容；不得使用 broad clear、capacity-
pressure substitute，也不得改变普通 residency policy。Local acquisition Value 析构后，每个
已配置 device 的完整 memory-and-scratch `reserved` vector 必须等于 row 前 baseline。
同一个排他 absolute capture deadline 也约束 Host-only shape。每次 direct Host ReadLease 后以及
最终 I/O snapshot 后取得的 fresh sample 都必须严格早于该未改变的 point。Conditional Metal 还会在
evidence snapshot 与精确 resident cleanup 后取得最终 sample。Collector 会把完整 Host return 保持
为局部值，直到它自身紧接调用后的 sample 通过；因此 tie 或更晚的返回不存储 acquisition、不冻结
或释放 Pending Value，只能继续由显式 unfrozen cleanup 处理。任何层都不刷新 deadline 或保留
迟到 authority。

I2 使用 ADR 0010 的目标 state machine，而不是虚构当前 API：唯一 replicate-grid
origin 固定连续的 111-slot cold/warmup/measured grid，measured 从 stride 11 开始，
terminal quiescence boundary 位于 stride 111，episode spacing 精确为 1,500 ms；任何
phase transition 都不得新选 origin 或插入 delay。
每个 episode 在相隔 16,666,667 ns 的 nominal schedule 上 admit 十二次 edit，最多允许
迟到 2 ms。Harness 在每个 nominal start 前预先生成 realtime request generation；
成功 Host admission 会令其成为 current、立即提交合法的
`RealTimeUpdate`/`Interactive` preview child，并在共享 realtime request identity 下 arm 合法的
`GlobalHighPrecision`/`Full` final child。Edit `0..10` 绝不等待：下一次 acceptance
遵循冻结 schedule，较早 preview 必须严格在其之前可见，才能保持 current。Final
只在其 preview 可见且仍为 current 时提交。两个 child Run arbiter 绑定同一个 request-
local gate，并在 terminal arbitration 内、发布 `Cancelled` 前 deny 该 gate；cleanup
callback 留在该顺序之外。Final permission 与 observation 是一个 HP Run-owned operation：
它在 HP terminal-arbiter mutex 下检查 Open、消费 gate、预留 causal coordinate，并在解锁前
完成 trigger callback。`ComputeService` 只能在该 operation 成功后提交 HP，因此匹配的 HP
cancellation 不能在 gate consumption 与 trigger observation 之间发布。更新 generation 会撤销
两个较旧 child 的 publication permission。Preview latency 与两个 child deadline 都锚定到
同一个 actual preview admission；final trigger/admission 被保留，但不能重置 1,000 ms deadline。
只有 `edit_index=11` 必须按顺序并在两个 absolute bound 内发布二者。Issue #94 实现
该冻结 cadence、I1 coefficient/update sequence 与 full-resolution final path；它不能
重新定义 cadence，也不能为 edit `0..10` 选择不同 coefficient 后仍保留
`I2-progressive-v1`。

当前确定性验证面由聚焦的 progressive-gate、profile/arithmetic、fail-closed evidence、
真实 product-path 与条件式 native-Metal test 构成。Product-path test 覆盖 preview
visibility 先于 final trigger 与 HP service、trigger 前取消、trigger 后 stale-final 拒绝、
相同时刻较新 edit ordering、精确 child QoS/deadline、不可变 Value acquisition 复用，以及
lifecycle/resource/Host settlement。Progressive-gate test 把真实 Run arbiter 绑定到 final
trigger 使用的同一个 gate。`test_progressive_compute` 的一个确定性 observer barrier 在
Run-owned trigger operation 内暂停，并证明并发 cancellation 在 trigger observation 完成前既
不能发布 cancellation，也不能发布 terminal；反向 case 先发布 cancellation/terminal，并要求
之后的 trigger operation、HP service 与 visibility count 保持为零。较早的 cleanup-delay
regression 继续证明 terminal cancellation 不能被之后的 notification 重新打开。`test_i2_profile`
冻结真实 visible Value，并要求重复 freeze 与 release 在不进行第二次 digest/acquisition 的情况
下保留每项已采集 fact；其 partial-capture failure case 要求 cleanup 保留前缀、保持 acquisition
显式缺失、释放 Value，并禁止之后回填。Host-
settlement case 独立覆盖 preview-only、preview 加
cancelled final、preview 加 successful final 与 no-child terminal shape；它们要求 Host
sequence/time 晚于每个已 materialize child resource，且 status 等于确定性的 progressive
aggregate。Residency case 先发布 generation one，再把同一 managed lineage 推进到 generation
two，随后要求 historical Ready Value 在不改变 currentness 的情况下，以已保存的精确
publication identity 完成 transfer/reuse。它们覆盖错误 Run、task、generation、Graph、intent、
source revision、binding、producer 与 fence rejection；lookup-before-retirement copy survival；
即使普通 broad hit 仍存在也执行 retirement-before-lookup rejection；在只允许一个 allocation
的 device limit 下连续 revision；第二次无 transfer/allocation；精确 release；以及完整 device
reservation 闭合。条件式 native test 保留真实 Metal path。Evidence test 要求
visible successful Run 中每个 `(run_id, local_task_id)` 只有 causal sequence 最早的 start
属于 useful，之后的 duplicate/retry 属于 discarded，不同 task 仍属于 useful，并且独立计算
post-cancel intersection。它们还要求两个 expected endpoint digest 在
candidate 比较前匹配各自 frozen oracle，把 expected/candidate 同步伪造区分为 Invalid、把
candidate-only mismatch 区分为 Fail，并锁定上述区分 phase 的 aggregate 边界。
`i2_progressive_benchmark` 是显式
`EXCLUDE_FROM_ALL` 的手工 target，不注册到 CTest。它只向 absolute、由调用者选择且为空的
output directory 写入闭合的 `execution-profile-i2-inner-row-v1` raw record 与 summary。
该 inner schema 不是 canonical 15-field outer row、bundle 或 reference resolver；仅编译或
通过 deterministic test 都不能建立机器 SLO 结果。精确 111-slot run 仍是显式的手工/release
动作；本文不声明已经执行该运行。

Issue #125 的长期回归会测试 runner mechanism，但不会 replay 111-slot machine workload。
`test_i2_evidence` 会注入 evaluator launch/completion gate 与 serializer seam。它要求先安装
future 再消费 input；baseline work 进行时恰有一个 delayed evaluator；已经过期的固定 handoff
必须失败，但不得丢失 future 或移动 origin；同时覆盖同步 launch recovery、只在 terminal 发生的
有序 serialization、serializer 失败后的 cursor 保留、只 drain 完整 row 的 generic abort，以及
failed-admission 全 Invalid 且 inner-before-outer 的 persistence 与 generic retry 抑制。Workflow
只接受下一个 slot，要求为全部 111 条 row 预留存储，并且最多拥有一个 future。

`test_i2_profile` 会在 Host acquisition 前拒绝已经过期或 exact-tie 的 capture deadline，注入一个
确定性 collector clock，使其在 synthetic Host 返回完整 evidence 的精确时刻到达 deadline，并证明
迟到的局部结果不会存储，而 Value 会保持 Pending 直到显式 unfrozen release。它还证明 timeout
不能改变任何后续 1.5 秒 origin 或 stride-111 terminal。`test_device_residency` 使用真实
`ReadyFence` 与 `ResidencyManager` 状态，覆盖 deadline 前严格 Ready、排他的 pre-poll tie
rejection、无需 wall-clock sleep 的 Ready、Failed 与 ProducerCancelled 确定性 post-poll deadline
crossing、精确 pending-admission discard、拒绝 late publication 与单次 fence failure、rejected
completion 消费 admission 后的 sole-owner settling interval，以及 Ready publication 抢先赢得
timeout race 时的精确 release。同一 suite 会把 deadline tie 精确放在第二次 resident-reuse
precheck 与单次 poll 之间，要求两次 monotonic sample，证明 late rejection 保留精确
revision/binding/producer resident，并单独接受 zero-transfer 的 strict-before Direct reuse。条件式
`test_metal_device_executor` 回归会让一个真实 serialized
callback 跨越另一个 invocation 的 absolute deadline，并注入精确的 upload-preparation、
bounded-copy 与最终 pre-commit monotonic tie。测试要求 admission timeout 后不进入 callback、
不执行 native commit、不发布 destination/resident、精确清除 pending admission、live native
allocation 为零，且 unwind 后的 device-ledger reservation 为零。`test_i2_product_path` 使用
source-private hook 强制真实 EmbeddedHost 走 Metal N/A。一项 case 把所有 sample 保持在 `D-1ns`，
并要求完整的两次读取 Host-only evidence；另一项会在 Host-only 最终 I/O snapshot 后精确推进同一个
injected clock，并要求 tie 不返回 evidence、保留 caller Value，且 Host/device reservation 不变。
它还保留条件式真实产品检查：受 deadline 约束的首次 Metal upload 后，必须以同一个 revision、binding、allocation 与 producer 执行 Direct、
zero-transfer reuse，随后进行精确 row-scoped release。这些测试不会创建 native-cancellation claim，
也不会把 manual runner 或 result orchestration 注册进 CTest/CI。该 hook 及其 compile definition 只
存在于 non-installed internal test product。

必需 logical value 调用 `compute_content_digest(Value)`，并且要求 `Available`、
存在 `ContentDigest`，以及 `CanonicalDigestAlgorithm::Sha256CanonicalV1`。Logical
digest、raw little-endian payload SHA-256、canonical manifest SHA-256、semantic-
trace SHA-256 与 logical/raw golden identity 始终是不同的 evidence family。I2 expected
preview evidence 必须等于 `i2_frozen_preview_content_digest()`，expected final evidence
必须等于 `i1_frozen_final_content_digest()`。Expected evidence 缺失、不受支持或被替换
时，即使 candidate evidence 也同步为该替代值，仍为 Invalid；两个 expected oracle 保持不变
时，candidate-only endpoint mismatch 为 Fail。

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
3. 要求完整 raw proof 保留在 durable evidence 与 JSON 中，只把它视为 expected evidence，
   把每个规范化 field 绑定到该精确 raw observation/proof，并验证每个 field-specific N/A claim、mount
   normalization decision、稳定 instance/endpoint/anchor identity、固定 performance
   configuration、被排除 option 的 no-effect proof 与 root-containment proof；
4. 对完整精确 manifest byte 计算小写 SHA-256，从而复算
   `storage_environment_digest` 与 `base_environment_digest`；
5. 对每一个 required-storage 侧，要求存在独立于 retained file 产生的不透明进程私有 actual
   capability。只有重复的 live held-root descriptor、store 签发的不可变 typed receipt 与可信
   live probe adapter 才能签发其 source；完整 raw probe 是新的 observation result，本身不是
   authority。每次 validation call 都会重新观察 root、receipt、probe 与 unverified-field set。
   任一列出的 unverified external field 都会使该侧 ineligible；复制字段与 diagnostic JSON
   不能恢复 authority。`B1InnerRowInput`/`B1InnerRow` 中保留的副本会共享 capability 并延长
   live-source 生命周期；
6. 解析精确四 field environment-class manifest 并复算
   `environment_class_digest`；独立把其 base-digest payload 绑定到 retained/复算 base，
   把 B1/M1 known `required`/`none` 与 storage-digest payload 绑定到存在的 retained/
   复算 storage，从 retained storage byte 加 raw proof 独立复算完整 eligibility result，
   将其 eligible flag 与有序 reason 同 retained claim 精确比较，并把 I1/I2 known
   `not-applicable`/
   `row-has-no-output-commit` 及精确 N/A state/reason/empty payload 绑定到 storage
   evidence 完全不存在；复算 class self-hash 绝不能替代这些 binding；以及
7. 评估表中每个 canonical-manifest predicate，只输出所有为真的 reason token，每项
   一次并按 unsigned-ASCII 排序；空 list 精确派生 `eligible`，非空 list 派生
   `ineligible`。Reason list 是 retained evidence，但不进入 environment digest。

精确 compatibility 要求 canonical manifest 逐 byte 相同、独立复算 digest 相等，
并在 storage 适用时要求 eligibility。对 storage，该 byte comparison 包含完整 framed
performance record。仅 digest 相等不够。Candidate/reference I1/I2 使用精确 base
compatibility 与固定 storage-N/A environment manifest。Candidate/reference B1/M1、
B1 cap-1/cap-8 与 M1/paired-B1-cap-8 使用精确 base、storage 和完整 environment-
class compatibility。M1/paired-I1 只比较精确 base manifest/digest；二者的
environment manifest 有意不同，但 M1 required-storage 侧仍必须通过自己的 actual-authority
binding。Raw field/proof 缺失或漂移、actual authority 缺失或从 file 重建、retained
eligibility 陈旧、state invalid、byte/digest mismatch 或 containment 失败，都会使受影响
relative verdict 成为 `invalid`。

在比较 peer 前，对 self-validation、cap-one/cap-eight、candidate/reference 与 mixed
relation 执行该四 field binding check。Mechanism test 必须修改内嵌 base 或 storage
digest payload，并在实际 retained manifest 不变时复算 environment-class self-hash；
两种 mutation 仍必须 incompatible。附加 case 会移除或漂移 retained proof，以及修改
canonical storage byte 并复算 storage/class digest、但保留 stale eligibility；每种 case
仍必须 incompatible。
测试还必须在复算 storage digest、class digest 与 retained eligibility 后执行同步 storage-
proof recast：self、cap 两侧、candidate/reference 两侧、M1/B1 两侧，以及 M1/I1 的 M1 侧都
必须因为其独立 actual observation 未改变而保持 invalid。JSON test 必须证明它只暴露
diagnostic authority metadata/digest；type test 还必须证明 actual observation、root authority
与 typed receipt 都不是公开可 default-construct 的 aggregate，并且构造后的 live-source drift
必须让下一次 validation 失败。Runner test 必须证明 canonical input file 绝不会初始化 actual
probe。当 portable runner 无法验证 external mount、performance、hardware-cache、
power-loss-protection 或 transaction-event fact 时，必须报告精确 field，并让 row 为 Invalid，
而不是 machine-conformant。

Issue #95 现已增加长期确定性机制测试，覆盖固定 field/type/enum/cardinality 拒绝；
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

1. 对 candidate，在评估任何 reference-relative row 之前，把
   `comparison_reference_bundle_digest` 解析到恰好一个 retained canonical 五 field
   bundle；独立复算其 bundle digest，要求 workload 相同且 role 为 `reference`，并验证
   其完整 canonical row list；reference 则使用封闭的 N/A encoding；
2. 每个 replicate 启动一个 fresh process，并记录 repository commit、dirty
   state、build/compiler/flag、OS/kernel、CPU/GPU/device inventory、power/thermal
   eligibility、provider/plugin binary 与 generation、process worker、Run cap、
   全部 limit/headroom、fixture hash、seed 和 cache/residency precondition；在
   warmup 前编码并独立验证精确 24-field base manifest；对 B1/M1，还要选择
   `OutputStore` root，通过可信 adapter 采集 warmup 前 storage/capability/configuration
   observation，把精确 21-field storage manifest 与 retained raw proof 编码为 expected
   evidence、冻结固定 performance configuration，并计算 retained eligibility 与 digest；
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
5. 保留 cold first-use 并执行精确且不参与测量的 warmup；对 isolated I1 保持已经
   固定的 221-slot grid；对 M1 则执行精确 `C^M1`/`W^M1` I1-origin 与 B1-offer
   protocol，证明固定 cold settlement 与跨 `B^M1` warmup occurrence，再在不替换或
   暂停冻结环境的情况下执行有序 `B^M1` cutoff/carryover snapshot/counter reset/
   首批 offer/supersession transaction，随后执行到 `U^M1` 与 final settlement 的
   精确 measured interval；
6. 在包含 B1 的 work 前分配并保留 canonical job-instance index，拒绝重复 phase/
   cycle/job coordinate，并验证每个 charge、admission、commit、receipt 与 evidence
   join 使用 occurrence identity 而不是 retry identity；
7. 在各自 owner 边界采集 raw origin/drain/boundary sequence、carryover/FIFO/phase
   attribution、admission、visibility、cancellation/quiescence、start、completion、
   offered-demand eligibility、artifact/receipt、trace、digest、transfer/copy/
   residency 与 resource-lifetime observation；对于 required storage，还要在 initial/final
   row boundary 重新观察 held root，保留实际 typed receipt，并要求独立产生的完整 probe 与
   retained expected proof 精确匹配；任一 unverified external field 都会使 row 为 Invalid；
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
| Fairness | 对两个 B1 Graph 整个一秒 window 都保有未消费 offered demand、且 producer 均未暂停的窗口，`J=(x_A+x_B)^2/(2*(x_A^2+x_B^2))`，其中 `x` 是 completed `work_units + ceil(ready_bytes/4096)`。总 service 为零时 invalid；Jain p05 >=0.95。两个 class 在不预筛 child capacity 的情况下都保持 scheduler-selectable 时，最多三次 Interactive start 后出现 Throughput selection。M1 只统计产品 fact 报告两个 class 都 evidence-startable（包括 child capacity）的 committed start。M1 还要求 headroom 导致的 Interactive admission failure 为零，并独立通过 latency/progress。 |
| Determinism | 对三个 replicate、fresh-process restart 与 Run cap 1/8 中相同的 B1 job index，typed logical `ContentDigest`、raw payload SHA-256、canonical manifest SHA-256、`execution-profile-semantic-trace-v1` SHA-256 与按 job index 区分的 logical/raw golden mismatch count 全部为零。 |
| Waste | `discarded_started_service / all_started_service`，使用 `work_units + ceil(ready_bytes/4096)`。每个无法 commit 结果的已启动 callback 都会被计费；对于 visible successful I1/I2 Run，每个 `(run_id,local_task_id)` 只有 causal sequence 最早的 start 属于 useful，之后的 duplicate/retry 属于 discarded，而不同 task 仍属于 useful。已经进入的不可抢占 work 如实 drain。I1/I2 Interactive 每个 replicate <=0.25，M1 对 Interactive service 单独应用该上限；accepted cancellation/supersession 后才启动的 work 精确为零，并与 discarded work 独立计数。I2 在允许的首次 transfer 规则下，额外 filesystem/codec、CPU-copy、readback、transfer 与 allocation byte 为零。无故障 isolated/mixed B1 的 discarded/duplicate/retry service 为零。 |
| Memory | Host retained、Host scratch、ready byte 与已配置 device memory/scratch 的独立 high-water byte，加上 B1 active Compute I/O task/planned byte。每个 Host component 与 identity 稳定的 configured device 都逐 component 满足 `reserved <= lifetime_high_water <= limit`，且同一 authority 的 lifetime high-water 在有序 capture 间绝不下降。Reserved 高于 high-water 或 high-water 下降属于结构性 Invalid；high-water 超限会使该轴失败。Isolated row-owned delta 与 B1 I/O count 回到 row 前 baseline/零，M1 shutdown 回到零。I2 精确 row-scoped resident release 发生在第二次 reuse evidence 之后、最终 snapshot 之前；每个已配置 device 的完整 memory-and-scratch `reserved` vector 等于 row 前 baseline。Candidate B1/I2 peak <=固定同环境 reference 的 105%。Process RSS 只作为 diagnostic。B1 planned-byte charge 与 event-aligned sample 是 Compute I/O admission、planned-byte high-water 与 final settlement 的强制性权威证据；它们不证明 physical memory ownership，也不能替代 RSS 或 ledger/device ownership evidence。 |

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

只能在执行后，根据实际源码私有 product observation 构造 candidate record set：
ready materialization 提供 local identity、实际 planned dependency、shape/device 与
submission resource declaration；service admission 提供不可逆 start；task execution
提供 terminal outcome。把实际 shape/declaration 归一化为 B1 resource vector，再与
冻结 semantic plan 这一独立 expectation oracle 比较。绝不能把冻结 plan 作为执行前
observed evidence 发出。Ready/start/terminal observation 缺失、重复或存在 gap、
dependency/resource 漂移、causal reorder 或 terminal-outcome 漂移，即使 content/
artifact digest 匹配，也必须使 determinism invalid。

### Evidence Bundle

一个 `execution-profile-slo-v1` bundle 包含：

- 上述全部 provenance 与冻结环境值；
- 精确 base 与 environment-class manifest byte，以及 claimed 和独立复算的
  `base_environment_digest` 与 `environment_class_digest`；对 B1/M1，还包括精确
  storage manifest byte、raw capability/performance-configuration/root-containment
  observation、eligibility/reason，以及 claimed 和复算的
  `storage_environment_digest`；
- workload/fixture/source/graph/payload hash 与全部 seed；
- 相互分离的 warmup、cold 与 measured count/window，包括 I1 grid/drain boundary 与
  全部 M1 cold/warmup/cutoff/terminal coordinate、精确 pre-boundary origin/count/
  offer、event order、carryover snapshot、per-Graph producer-cycle counter、counter
  epoch 及 phase attribution；
- raw sample/event、offered-demand eligibility interval、queue/carryover transition 与
  drop/gap counter；
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
组成：`workload_id:workload-id-v1`、subject/replicate/cap、三个 environment
coordinate、五个 section digest 与两个 pair reference。非 M1 pair field 使用
`not-applicable/row-has-no-isolated-pair` 与 zero payload。Job-instance section 包含
完整 occurrence record 的 canonical list；I1/I2 使用显式 known-empty `0:` list。
每个 section digest 对字面量
`execution-profile-evidence-section-digest-v1\n` domain、framed section name、
schema id 与 retained exact byte 计算 hash。

每个 bundle 由精确 `execution-profile-evidence-bundle-v1\n` header 与五个 record
组成：`workload_id:workload-id-v1`、subject role、provenance-section digest、
comparison reference，以及 item 均通过 `workload-id-v1` 首个 component 使用该
workload id 的非空 canonical row-reference list。Reference bundle 使用
`not-applicable/reference-has-no-comparison-baseline`；任何 optional field 都不能省略
或使用空 digest 表示。`row_digest` 与 `bundle_digest` 对各自不同的 ADR 0010 domain
tag 加上完整 canonical manifest byte 的一个 frame 计算 hash。Claim 存储在 object
旁边，并排除在 hashed byte 之外。

每个 row-reference item 的功能 key 精确为
`(workload_id,run_cap,replicate_ordinal)`。即使两个 item 命名不同 row digest，list
也必须拒绝相同 key。对每个 item，validator 必须解析出恰好一个 retained canonical
row，复算其 digest，解析全部 15 个 field，并让 workload、cap、replicate 与 item
匹配，让 subject role 与 enclosing bundle 匹配。

对 candidate，validator 首先把 `comparison_reference_bundle_digest` 解析到恰好一个
retained bundle object。它必须解析精确 canonical header 与五个 field，独立复算 bundle
digest 并匹配 claim，要求 `subject_role=reference` 且 workload 与 candidate 相同，并验证
完整非空 row list 的 ordering、功能 key 唯一性及 item 到 canonical row 的解析。解析出
零个或多个 retained object（包括多个 object 携带相同 digest claim）、五 field parse/
schema failure、claimed/recomputed mismatch、role/workload 错误，或任一 target row list
无效，都会使全部相关 reference-relative verdict invalid。只有通过这些检查后，精确 target
row 才是与 candidate row 功能 key 相同的唯一 row；comparison bundle digest 本身不选择
row。每个 M1 pair 则命名精确 row digest，并必须在必需 isolated workload 与 cap 8 下
解析到 same-role、same-ordinal target。Item、row、bundle、role 或 key 证据缺失、重复
或不匹配时均为 invalid。

Comparison-bundle resolver 必须覆盖以下 scenario/oracle 矩阵：

| 场景 | Oracle |
| --- | --- |
| Exact-one valid | 一个 retained object 解析为精确 canonical 五 field bundle，全部携带 workload 的 field/component 都按 `workload-id-v1` 解析，其独立 rehash 等于 claim，role 为 `reference`、workload 匹配、完整 row list canonical 且功能唯一，candidate key 选择恰好一个有效 target row；随后可以继续其他 compatibility check。 |
| Zero object | Claim 没有解析出 retained bundle object；全部相关 reference-relative verdict 为 `invalid`。 |
| Multiple objects with the same claim | 同一 digest claim 解析出两个或更多 retained object，即使其 byte 相同；validator 不按 path、insertion order 或 byte 选择，全部相关 verdict 为 `invalid`。 |
| Five-field schema failure | Target 的 header、field count/order/type/state/reason 错误、frame malformed、缺少 final LF 或有 extra byte；它不是 canonical bundle，全部相关 verdict 为 `invalid`。 |
| Independent rehash mismatch | Canonical target byte 复算出的 bundle digest 与 candidate claim 不同；全部相关 verdict 为 `invalid`。 |
| Wrong role | 解析出的 bundle 具有 `subject_role=candidate`；全部相关 verdict 为 `invalid`。 |
| Wrong workload | 解析出的 bundle/candidate/item workload 不同、使用大小写变体或未知 token，或者使用通用 `identifier` type frame；canonical validation 在 equality/key lookup 前失败，全部相关 verdict 为 `invalid`。 |
| Target key missing | 没有 target row-reference item 具有 candidate row 的功能 key；相关 verdict 为 `invalid`。 |
| Target key duplicated | 多个 target row-reference item 具有该 key，包括命名不同 row digest 的情形；相关 verdict 为 `invalid`。 |
| Target row mismatch | Selected item 解析出零个或多个 row、row rehash 或 15-field parsing 失败，或 item/bundle workload、cap、replicate、role 不匹配；相关 verdict 为 `invalid`。 |

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

Issue #93 负责可复用的 I1 accepted-boundary collector：call 前 `A_i` 采样与 row-local
sequence 预留、仅成功时把 `(A_i,event_sequence_i)` 绑定进 product supersession identity、
row 与 current evidence 精确匹配、accepted-row/observer-causal sequence domain 彼此独立、
failure 且不产生 accepted event 或 product binding 的 evidence、连续 isolated-I1 grid，以及
精确 drain/tie/guard 行为。
Issue #95 负责 B1 `OutputStore` 固定 raw probe-to-schema mapping、backend 到固定
schema 的 adapter、mount normalizer、performance-configuration mapping/proof、唯一
canonical encoder/digest、eligibility 与 root-containment evidence，以及 cap-1/
cap-8 和 candidate/reference check。Issue #96 为 M1 原样复用精确 manifest byte，
复用 #93 的 accepted-boundary collector，把第一次 measured edit 绑定到
`edit_index=0`、`A_0` 与其预留 sequence 且不得重新定义 acceptance。完整 admission 与
final-warmup current-hold 必须只从保留的 Issue #93 source 推导，并在任何 protocol 提前返回
前校验。Issue #96 同时实现冻结的
`C^M1`/`W^M1` pre-boundary protocol、独立 producer-local cycle、不得
重新定义的精确 final-warmup current-hold/accepted-admission 例外与
cutoff/carryover/phase-attribution boundary，并强制执行其 same-ordinal 完整 B1 pair，
同时让 I1-only pair 只比较 base。上述 Issue 都不能重定义 v1 grammar、
field 或 sentinel。Issue #92 只定义本 evidence
contract；它不新增当前 probe、serializer、public API、runner 或 runtime result
field。

Issue #93 现已通过 `test_host_adapter`、`test_compute_supersession`、`test_i1_profile`、`test_i1_evidence`、
`test_dense_tensor_content_digest`、`test_resource_ledger`，以及在启用仓库 OpenCV operation
provider 时的 `test_i1_product_path`，注册长期确定性的 I1 行为。它们共同覆盖 checked
grid/admission arithmetic、仅成功时产生的 accepted coordinate、transactional pre-Kernel Host
preparation failure、精确 Host/Kernel/product
identity binding、equal-time row-local ordering、彼此独立的 accepted-row 与 observer-causal
sequence、精确冻结的 graph/request construction、one-based nearest-rank aggregate、彼此独立的
discarded/post-cancel service、Host/device lifetime-high-water observation、final settlement、
canonical DenseTensor logical identity，以及真实 embedded Host latest-wins product path。这些是
correctness test。`test_i1_profile` 会独立重算冻结 mathematical golden；
`test_i1_evidence` 覆盖 `Value` release 后 expected digest 缺失、被替换、invalid 与候选不匹配；
`test_i1_product_path` 交叉校验一个精确 2048 结果。既有
`test_opencv_operation_concurrency` cap-one/cap-eight 断言保护确定性 curve provider 的
bitwise output identity。它们不会断言 timed 221-slot run 中与机器相关的 percentile 或
waste threshold。

精确定时 workload 由手工、`EXCLUDE_FROM_ALL` 的 `i1_edit_storm_benchmark` target 承担；
它不注册到 CTest 或 CI。必须显式构建，并把每个 replicate 分别写入 checkout 外、父目录已存在、
且自身不存在或为空的不同绝对目录：

```shell
cmake --build build --target i1_edit_storm_benchmark -j
./build/tests/i1_edit_storm_benchmark \
  --output-dir /tmp/photospider-i1-r1 \
  --base-manifest /absolute/evidence/base.manifest \
  --environment-class-manifest /absolute/evidence/i1-class.manifest \
  --subject-role reference --replicate-ordinal 1
./build/tests/i1_edit_storm_benchmark \
  --output-dir /tmp/photospider-i1-r2 \
  --base-manifest /absolute/evidence/base.manifest \
  --environment-class-manifest /absolute/evidence/i1-class.manifest \
  --subject-role reference --replicate-ordinal 2
./build/tests/i1_edit_storm_benchmark \
  --output-dir /tmp/photospider-i1-r3 \
  --base-manifest /absolute/evidence/base.manifest \
  --environment-class-manifest /absolute/evidence/i1-class.manifest \
  --subject-role reference --replicate-ordinal 3
```

Runner 会把产品 worker count 固定为 eight，保留唯一连续的 221-slot cold/warmup/measured
grid，绝不移动或 backfill 已错过的 slot，并在 evidence invalid 后停止后续 submission。完整运行
会在候选执行前安装 immutable expected digest，在 `Q_end` 前对每个 visible output 只计算一次
digest，冻结类型化 result，并释放每个 `Value`。正常 `Q_end` 时，一个自有且不含 Value 的
evaluator 可以与下一 baseline preparation 重叠，但必须在下一 admission 前完成。JSON
construction、dump 与 disk flush 位于每个后续 origin guard 之外，并在 `T^I1` 按精确 slot
顺序 drain；bounded live set 是一个 evaluator 加 221 条不含 Value 的 row。完整运行会写出
冻结的 `i1-graph.yaml`、`invocation.json`、raw `episodes.ndjson`、`summary.json` 与
`pair-object.canonical`；
异常会在此前已安全完成的 artifact 之外写出 `failure.json`。特别是 failed/invalid admission
会先向 `episodes.ndjson` 追加并 flush 对应 Invalid inner row，保留 raw admission 事实和关闭后的
observer/resource 状态，然后才写 `failure.json`。JSON/NDJSON 文件仍是封闭的 Issue #93
inner artifact，不声明 outer envelope。Denominator-only pair-object pack 只从完整且尚未
compact 的 221 条 source row 生成：它保留 canonical I1 row、单 row bundle、全部六个 source
section、精确 storage-N/A environment claim，以及用于重算 p99 的 200 个 measured latency
sample；其 output/verdict section 明确拒绝超出该 denominator 的 portable authority。
Candidate 运行还必须提供不可变 comparison-reference bundle digest。Exit zero 表示四项 I1
inner verdict 全部通过；exit two
表示完整 evidence 至少有一项 threshold 失败；exit one 表示 parsing、setup、cadence 或 evidence
invalid。仅构建 target 或运行 `--help` 只是 harness smoke，不是 performance evidence。

Issue #95 现在通过 `test_b1_profile`、`test_b1_environment`、`test_b1_output_store`、
`test_b1_evidence`，以及在启用仓库 OpenCV operation provider 时的
`test_b1_product_path`，注册长期 B1 mechanism。这些测试冻结 34 个 seed/job identity 与独立
binary32-RNE golden、由实际 observation 支撑的 canonical semantic trace 与 dependency/
resource/outcome drift rejection、stable NFC text 与精确 21/24/4-field schema、
scalar/collection/fixed-record/mount/全部 37 个 performance component 与 raw-proof rule、
十一 reason eligibility truth set 与 pair compatibility（包括篡改内嵌 digest 后复算
class self-hash、proof 缺失/漂移，以及 byte/digest 复算后 eligibility 陈旧）、两个带
charge、且位于已验证 private staging anchor 内的 crash-durable output stage、64-attempt
exhaustion/cleanup、atomic no-replace directory publication、mkdir/open 与 public
real-directory replacement race、slot 创建后 fault rollback/retry、root replacement
fail-closed 行为、针对 extra/type/different-identity leaf 与注入 `EIO`/`EROFS` 的严格
cleanup，以及 receipt、不透明 receipt/root/actual-authority construction、retained
descriptor/lock 生命周期、每次重新观察 live source，以及 retained proof/JSON 无法签发
authority；真实并发下
executor 签发的精确 charge/release，以及 undercharge/伪造零值 Compute I/O FSM mutation
matrix、四项相互独立的
inner verdict，以及真实 Host 上精确
Throughput QoS、cap-1/cap-8、Graph A/B predecessor、content/trace、lifecycle、resource 与
Compute I/O closure。它们使用 disposable root，不包含机器相关 throughput 或 candidate/
reference threshold。

精确 B1 corpus 由手工 `b1_immutable_benchmark` target 承担。它为
`EXCLUDE_FROM_ALL`，不属于 CTest/default CI，必须显式构建：

```shell
cmake --build build --target b1_immutable_benchmark -j
./build/tests/b1_immutable_benchmark --help
```

每次 invocation 只接受一个 cap 与一个 fresh-process replicate。`--output-dir` 必须已经是
checkout 外的空绝对目录，并且就是选定的 canonical durability root。三个 manifest input
必须分别是经过独立预验证的精确 `execution-profile-base-environment-v1`、
`execution-profile-storage-environment-v1` 与
`execution-profile-environment-class-v1` byte。`--storage-proof` 必须是精确 canonical
`execution-profile-b1-storage-raw-proof-v1` document，而不是 JSON。它使用共享 manifest
field/frame grammar，并精确包含六个封闭 section：backend、21-field raw observation、mount、
两次 performance cut、transaction/receipt 与 root/destination containment；其中没有任何
derived proof boolean。Proof 的 selected/resolved root 必须等于 `--output-dir`，retained
destination list 必须包含 runner 的 root、Graph、session、cache、invocation、row 与 failure
path，以及 pair-object path。Runner 会严格 parse/re-encode byte，独立重放全部 mapping 与 eligibility predicate，并
拒绝 missing、unknown、duplicate、stale 或 drifting evidence，不会在运行时补写 proof fact。
一条精确 invocation 如下：

```shell
mkdir -m 700 /absolute/durable-root/b1-cap1-r1
./build/tests/b1_immutable_benchmark \
  --output-dir /absolute/durable-root/b1-cap1-r1 \
  --base-manifest /absolute/evidence/base.manifest \
  --storage-manifest /absolute/evidence/storage.manifest \
  --environment-class-manifest /absolute/evidence/b1-class.manifest \
  --storage-proof /absolute/evidence/storage-proof.manifest \
  --run-cap 1 --subject-role reference --replicate-ordinal 1
```

在组合完整 B1 candidate 或 reference 前，必须针对 cap 1 与 8、ordinal 1 至 3，在六个不同
fresh process 与 root 中运行。一次 invocation 会执行 cold seed 252、warmup seed
253/254/255，再由两个并发且有序的 measured producer 处理偶数与奇数 job `0..29`。它会在
所选 root 下写入 `invocation.json`、`row.json`、`pair-object.canonical`、两份冻结 Graph
YAML、session/cache 目录与
34 个 immutable occurrence slot；在安全 root 选择后发生 exception 时，会以 no-replace 方式
额外写入 `failure.json`。仅 payload 就超过 2.2 GiB。Exit zero 表示四项 inner verdict 全部
通过；exit two 表示完整 evidence 至少一项 inner threshold 失败；exit one 表示 parsing、setup、
product、durability 或 evidence invalid。`row.json` 明确不声明 canonical outer row/bundle；
denominator-only pair-object pack 则另行保留 canonical isolated row/bundle 与全部 source
section。它要求 schema version one，以及真实 34-job row 中精确的一个 cold、三个 warmup、
三十个 measured job index，并包含三十个有序 verified-endpoint outcome 与精确 measurement
interval；其 output/verdict section 拒绝超出 B1 rate denominator 的 portable authority。
Pack 会保留 storage claim/proof/eligibility，但不能序列化或签发 process-private actual-storage
authority，因此 portable B1 结果保持为 `Invalid`。Candidate 运行还必须提供不可变
comparison-reference bundle digest。
构建 target 或运行 `--help` 只属于 harness smoke；本文不声明已经执行精确 34-job invocation
或三 replicate B1 机器运行。

Issue #96 通过 `test_m1_profile`、`test_evidence_envelope`，以及在启用仓库 OpenCV
operation provider 时的 `test_m1_product_path` 注册确定性 M1 合同；聚焦的
`test_compute_run` case 还会证明 service-start coordinate reservation/commit/callback-or-abort
fence。Unit suite 覆盖
精确 C/W/B/U arithmetic、1/7/40 origin grid、固定 warmup offer、carryover/current-hold
evidence、独立 producer-local cycle、全部五个轴、未知 enum 的 fail-closed 行为、合法的
zero-based task zero、allocation-free callback publication、并发 reservation 下的 sequence/time
monotonicity、确定性越过历史 4096 次 gate attempt 而不产生伪 `sequence_exhausted`、真实
`UINT64_MAX` fail-closed exhaustion、每个 product callback 都保持 mirror-before-authority 的同坐标 fanout、
不变的 base-only-I1/full-B1 environment delegation、canonical golden digest、functional
key、exact-one/DAG resolution 与不完整 live authority。它们还会拒绝 substituted
isolated-I1 source、遗漏的 isolated-B1 outcome、
遗漏的 M1 raw window、outer/inner claim 篡改、denominator/source 不匹配、缺失/重复/重排/
未知/超限 I/O transition，以及非零 final I/O state。一项专门回归会让全部稀疏 temporal I/O
current value 保持为零，同时证明短 accepted/settled task 仍会提高 event-derived high-water。
Observer-boundary 回归会在 coordinate reserve 后、route commit 后与 slot claim 后暂停，
并证明 copied-record count 不变不能满足 reservation-entry/completion/claim/published cut。
一个集中式 spy 会枚举全部十一种 fanout product callback，并要求每一种都按 mirror/authority
成对排序。Mirror 发布前的确定性 barrier 还会证明：在同坐标 source history 可见之前，M1
reservation-completion frontier 必须保持开放，cut 也必须保持不稳定。一个仅存在于 test
product 的 route-commit rejection 会证明显式 coordinate abort、不发布 start、retry 成功与
final frontier 精确闭合。Static product consumer smoke 禁止该 probe
symbol 出现在 production archive。Lifecycle replay 回归使用一份
producer-faithful multi-Graph history，包含 registration/candidate rollback、group 与 standalone
admission，以及合法 cross-bundle phase interleave。测试会逐一篡改九个 registry-derived
counter，并拒绝因果 phase 重排、错误 Graph/Run identity、跨 Run 拼接、group-child 损坏、
不存在的 rollback identity、缺失/重复/重排/cursor 损坏的 record、stop 后 event 与 final 非零
physical sample。独立负例会强制 ready capacity、ready-plus-entered 对 child grant 的可达性、
child-to-root 与 policy-invocation-to-binding ownership，以及 resource 必须可达 admitted child
或 pending candidate，同时不会虚构 physical event delta。Registry 测试证明 worker-join 与
binding-retirement record 在 lifecycle fence 内取得其九 counter registry cut。
Product suite 使用产品签发的 per-start ready/lifecycle/candidate/resource fact，证明 Throughput
Run 到达 terminal 与 resource settlement 时 Interactive work 仍为 outstanding，并证明精确
31-CPU Throughput/32-CPU shared-headroom boundary。Policy execution 回归还会证明：一个
scheduler-selectable 但 child grant 已耗尽的 candidate 会到达 `GrantUnavailable`，只在当前
worker cycle 被标记，且不能饿死独立 Run，也不能在 cancellation 期间 spin。确定性 service-
start observation 会证明已耗尽 class 的 evidence-startable 为 false、正在 commit 的 class
为 true，并证明两个 capacity-ready class 都为 true。Class-start 正负 case 会区分真实 dual-
evidence-startable 的第四次 Interactive start 与 nominal interval overlap。Timeout 只是
deadlock diagnostic，不是 latency threshold。

Memory 回归会独立拒绝 Host reserved-above-high-water、device reserved-above-high-water 与
逐 device declining lifetime high-water。Source closure 回归会直接修改 first measured
admission/current hold、通过 canonical nested replay 修改，并在同步全部六个 verdict 的完整
重新 hash outer envelope 内修改；每条路径都保留精确 source-derived diagnostic，而不是在
无关 protocol verdict 处停止。
同时间 case 还会通过 direct、canonical-reader 与完整重新 hash 的 outer 路径，覆盖
`(B,n)` measured current 后接 `(B,n+1)` cancellation，以及反向的 `(B,n-1)` sequence。
正例要求 source closure 且没有 current-hold diagnostic，同时保留独立的 Issue #93 Invalid
Run fact；反向 case 要求精确的 current-hold source rejection。

Pair-object 测试还会执行真实 Issue #93/#95 evaluator-to-producer 路径、canonical pack
round trip、精确 section 顺序/数量、source rematerialization、digest/object 不匹配、错误
role/ordinal、重复 corpus 插入与有界绝对 regular-file 读取。I1 pack 必须保留精确 200 个
latency sample；B1 pack 必须保留 schema version one、唯一的 1-cold/3-warmup/30-measured job
index 与 30 个有序 outcome。Loaded validation 会拒绝 portable output authority 或任何超出
denominator-only 的 claim。Relative、空、超限、directory 以及最后一级为 symlink 的输入都会
被拒绝。条件式 Windows source path 也会进行 cross-compile syntax 验证；这不构成 Windows
runtime 结果。

精确 M1 replicate 由手工 `m1_shared_benchmark` target 承担。它为 `EXCLUDE_FROM_ALL`，
没有 `add_test`，也不属于默认构建或 CI：

```shell
cmake --build build --target m1_shared_benchmark -j
./build/tests/m1_shared_benchmark --help
```

一次 invocation 要求 checkout 外的 fresh absolute empty output directory、canonical base/
storage/environment-class claim、retained storage proof、subject role、ordinal，以及两份
canonical、denominator-only isolated pair-object pack。每个 pack path 必须是绝对、有界的
regular file，并同时提供
精确 row/bundle digest。I1 pack 必须来自同 role、同 ordinal、cap-eight I1 producer；B1 pack
必须来自同 role、同 ordinal、cap-eight B1 producer，并使用逐 byte 相同且 eligible 的 storage/
environment-class claim。只有 digest 文本而没有相应 object 时，setup 会直接拒绝；不再接受
numeric p99 或 B1-rate option。Candidate 还必须提供 comparison-reference bundle address。

在推导 timed C/W/B/U boundary 前，runner 会通过一个 opened object 读取每个 pack：POSIX
使用 `O_NOFOLLOW` descriptor，Windows 使用带 `FILE_FLAG_OPEN_REPARSE_POINT` 的
`CreateFileW` handle。Type/reparse check、有界 size、精确 byte、growth check 与 close 都
使用同一个 object。Runner 随后解析封闭 pack schema，重算 row/bundle/section address，
重新物化每个 source object，
强制单 row membership 与 seal order，检查 role/workload/cap/ordinal 以及 base-only I1/full
B1 environment relation，并重算 I1 p99 与 B1 successful-site-operation/interval tuple。只有
这些重算值会填入 M1 evaluator 与 sealed denominator claim。

Runner 通过一个
`EmbeddedHost` 运行 I1 Graph 与两个 B1 Graph，执行精确 cold/warmup/measured cadence，分类
全部 480 个 measured edit，在 U 停止新增 offer，关闭全部 Graph，调用 source-private 且幂等的
M1 evidence-finalization seam，要求终态 `ServiceStopped` snapshot 的全部 15 个 lifecycle
counter 与全部 process resource 为零，并
写入六个 canonical section，以及 `row.canonical`、`bundle.canonical`、复制的 canonical
`paired-i1-object.canonical`/`paired-b1-object.canonical` pack 与 `result.json`。其
nested M1 row 会保留 30 个 raw progress 与 Graph-service window、480 个 raw Host outcome、
产品签发的 class-start cut、具备 evaluator 权威的 I1/B1 source evidence、event-aligned
Compute I/O，以及稀疏 temporal/lifecycle stream。有效
`execution-profile-m1-inner-row-v2` 要求每个 progress duration 精确等于一秒。该 schema
的精确 20-field manifest 会保留 48 个完整 post-freeze I1 episode input，以及每个 protocol
offer 对应的一个完整 B1 physical/output/golden/semantic/I/O observation source。Validator 会
精确 join phase/ordinal/origin 与 job/producer-ordinal/offer/endpoint identity，重新计算 I1
occurrence projection 与 B1 verified-endpoint/waste projection，并使用与 runner 相同的 checked
函数，从 source 推导并精确匹配 first measured admission/current hold、三十个 progress
window、三十个 Graph A/B service/demand window、480 个 measured headroom outcome 及其
attempted/classified/failure aggregate；该 source gate 在 protocol 提前返回前运行。随后再复用
production protocol/fairness/waste/memory/B1-I/O evaluator，并精确匹配五个轴与 overall。即使另一项缺陷已使 row
为 `Invalid`，source mismatch 也不得物化。测试覆盖同 cardinality 的错误 identity/ordinal、
I1 source/projection 与 B1 raw-trace/waste 矛盾、同步 verdict 的 progress/Graph/headroom
projection 矛盾、aggregate 与 checked-overflow failure、source 缺失/重复/重排、半秒、两秒、
混合 duration、ratio-flip、nested schema、raw-value、stale-verdict、outer 全量重 hash 矛盾，以及
producer shape 的正向 byte roundtrip。v2 byte 省略重复的完整 I1/B1 diagnostic JSON；复制的
receipt observation 绝不会变成 portable receipt、storage 或 machine authority。Outer
15-field row 与 five-field bundle 保持 version one。Nested observation snapshot 仍为十个
field，v2 manifest 仍为二十个 field；该修正改变 frontier 语义，但不新增 schema version。
完整 corpus validator 会 exact-one 解析每个具名
isolated row，重新计算 isolated I1 p99 与 B1
successful-site-operation/measurement-interval tuple，并精确
核对两份重复的 M1 claim。证据缺失、歧义、遗漏、替换、篡改或不匹配会在 timing 前被拒绝，
或在 corpus replay 中成为 `Invalid`。两份已加载 pair 的 row、bundle 与 section 会先于 M1
row/bundle，被 exact-once 插入本地 validation corpus。Portable storage observation 仍不会制造
完整 live machine authority，因此即使两个 denominator object 均通过验证，这个独立条件仍可能
使 corpus validation 保持 `Invalid` 并返回 exit status two。构建该 target 或运行 `--help`
只属于 harness smoke；本文不声明已经执行精确
timed replicate 或 three-replicate machine corpus。

## 手工 codec 稳健性 harness

Issue #106 维护两个调用真实产品 decoder 的本地 parser robustness harness：

- `fuzz_worker_protocol_codec` 覆盖有界 worker Assignment 与 Report metadata，包括纯
  Assignment semantic decoder；
- `fuzz_isolated_cpu_invocation_codec` 覆盖 isolated request/response packet 与生产 descriptor
  validator。

它们属于手工 developer mode，不属于 product-test inventory。CMake option
`PHOTOSPIDER_BUILD_FUZZERS` 默认是 `OFF`；启用后，configuration 要求 Darwin 或 Linux、
Clang-family compiler，并要求 libFuzzer、AddressSanitizer 与 UndefinedBehaviorSanitizer 通过真实
compile/link 加有界 native-run probe。缺失或初始化不兼容的 runtime 因而会在 configuration 阶段
失败。与全局 `USE_ASAN` 或 `USE_TSAN` mode 冲突时会默认拒绝。包含 decoder 的生产 private
static library 使用 `fuzzer-no-link,address,undefined` 编译，并只把 `address,undefined` 暴露为
传递到最终链接的要求。这样每个使用其被插桩 object 的普通 consumer（包括
`photospider-worker`）都会闭合 runtime，但不会取得 libFuzzer main；两个手工 executable 则拥有
完整的 `fuzzer,address,undefined` 链接。Configuration-time target-property check 会拒绝未来任何
compile/link ownership 错配。两个 executable 都是 `EXCLUDE_FROM_ALL`，且没有 `add_test()`、
install、export、package 或 CI ownership；被插桩的 private library 同样不会被 install 或
export。

必须显式构建并运行有界本地 campaign：

```bash
cmake -S . -B build-fuzz -DPHOTOSPIDER_BUILD_FUZZERS=ON
cmake --build build-fuzz \
  --target fuzz_worker_protocol_codec \
           fuzz_isolated_cpu_invocation_codec -j
mkdir -p out/fuzz/worker/{corpus,artifacts} \
  out/fuzz/isolated/{corpus,artifacts}
./build-fuzz/fuzzers/fuzz_worker_protocol_codec \
  -runs=1000 -artifact_prefix=out/fuzz/worker/artifacts/ \
  out/fuzz/worker/corpus
./build-fuzz/fuzzers/fuzz_isolated_cpu_invocation_codec \
  -runs=1000 -artifact_prefix=out/fuzz/isolated/artifacts/ \
  out/fuzz/isolated/corpus
```

Harness 内建深层 canonical seed，也接受上面的未跟踪本地 corpus directory。Corpus addition、
生成 finding、log 与 build tree 必须留在已忽略的 `out/`/`build-*` storage 下；不得提交，也不得把
result orchestration 注册到 CTest 或 CI。Finding 只是 parser evidence，不是 session、process、
plugin、quota、artifact、retry 或 publication capability。

确定性且已注册的 GoogleTest 继续负责 canonical re-encoding、严格 prefix/truncation、trailing
byte、worker identity/digest/data-plane/heartbeat validation、isolated enum/count/rank/extent/stride/
range/overflow/phase/overlap validation、optional task identity、page reuse、failure/cancellation/retry/
concurrent Run 行为，以及精确 IPC schema。手工 target 用于探索额外有界输入，绝不能取代这些稳定
regression assertion。

## CTest 注册

所有预期 GoogleTest 二进制都应注册到 CTest。这包括当前可能低置信度的里程碑测试和 `test_propagation_contracts`。

低置信度测试仍应在验证中可见，而不是被静默排除。如果测试不足以可靠地作为开发门禁，应明确记录该状态，并创建后续工作升级或替换它。

里程碑测试、`test_propagation_contracts` 与长期 `test_region_contracts` 行为 suite 已注册到
CTest，因此它们可见；但在后续 pass 将前两类重写为更窄、更清晰 fixture 和断言的回归测试前，
它们仍是低置信度遗留测试。

`test_propagation` 不同：它是脚本式 REPL/tool 目标，不是 GoogleTest 二进制。CMake 保持它可构建，供手工脚本和临时验证使用，但 CTest 不会发现或运行它。不要声称 CTest 覆盖了 `test_propagation`；需要时应单独运行准确的手工命令。

只有 `BUILD_TESTING`、OpenCV、YAML、graph CLI、仓库 OpenCV operation provider 与仓库 OpenCV
operation plugin 全部启用时，才会注册依赖 provider 的默认完整 test suite。该 suite 会注册
`test_dense_image_processing`；即使该 producer 使用 OpenCV，它仍会直接编译 dependency-neutral 实现。
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

GitHub Actions 只保留两个 workflow。`ci.yml` 在向 `main` 与 `CI/**` push 时运行；
`build-ci-image.yml` 在 `main` 的 `Dockerfile.ci` 变更或手工 dispatch 时发布自定义 Linux 镜像。
仓库不再使用 `pull_request_target`、protected-path 授权、docs-only 路由、sanitizer workflow、
scheduler-log workflow、evidence/provenance 层或 result aggregator。

日常 CI 的 healthcheck 会检查 whitespace、configure CMake，并构建
`public_header_self_containment`。唯一 build job 会恢复完整 `build/ci` cache、再次 configure、只调用
一次 Ninja，然后先创建唯一一份 `ctest-runtime.tar.gz`，再运行 `build-smoke` label。Cache 会保留
object、Ninja incremental state 与 smoke 生成的嵌套 build tree，供下一次兼容 push 使用。

Runtime archive 会排除 object file、`CMakeFiles`、Ninja dependency/log database、既有
`Testing` 输出，以及 cache restore 带回的两个临时 smoke root：
`tests/image_artifact_codec_dependency_disabled` 与
`tests/optional_opencv_provider_disabled`。它会保留 `tests/` 的其余内容、library、plugin、executable、
CTest metadata 和 package configuration。Packager 会打印验证后 archive 的精确物理 byte count 与
tar entry count，用于 warm-cache 和 artifact 体积诊断。Producer 只有在 build-smoke 成功后才会上传
该 archive。三个并行 job 会恢复同一份 archive，并分别运行 `unit`、`integration` 或
`verification` label。CMake 负责所有 primary label 以及 `RUN_SERIAL`、`RESOURCE_LOCK` 与
`TIMEOUT` 约束；workflow 不维护冗长 test-name 正则。

JUnit report 与 `ctest-runtime` 保持分离。Build-smoke report 会上传为
`ctest-junit-build-smoke`；每次 labelled CTest 调用后，matrix job 会把
`CI-results/ctest/<label>.junit.xml` 上传为唯一的 `ctest-junit-<label>` artifact。全部四个上传
step 都使用 `always()`，在 report 缺失时告警，并将存在的 report 保留七天。

两个 workflow 使用持续维护的 Node 24 action major：`actions/checkout@v7`、
`actions/cache@v6`、`actions/upload-artifact@v7`、`actions/download-artifact@v8`、
`docker/login-action@v4`、`docker/metadata-action@v6` 与
`docker/build-push-action@v7`。GitHub-hosted runner 满足这些 action 对 Actions Runner 2.327.1
或更新版本的最低要求。Build job 会打印 cache action 的 exact-hit output，因此 warm rerun 可以在
比较 configure、Ninja、package、build-smoke 与 post-job cache 耗时前，先区分 exact hit、prefix
restore 与 miss。

`ci/scripts/build_smoke_inventory.py` 会继续保留，因为长期产品测试
`InstallConsumerArchitecturePropagationSafety` 会导入它来验证已配置 build-smoke entry。手工
product-boundary driver 与 `sanitizer_test.sh` 也继续供本机显式使用。旧 orchestration、routing、
runtime-capability、重复 suite 与 self-proof script 不再作为 CI 入口。精确 workflow、cache、
artifact 与 label 契约记录在 `docs/CI/zh/github-actions.zh.md`。

架构演进目标不会在本测试文档中维护，而是记录在
`docs/roadmap/zh/Kernel-Evolution.zh.md`。每项实现变更分别定义与风险相称的验证和长期回归覆盖。
