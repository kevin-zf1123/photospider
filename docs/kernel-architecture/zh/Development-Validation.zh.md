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
runtime，Review 应记录该针对性运行没有执行，并且不得声称 CMake 3.16 runtime PASS；不要求进行
架构模拟。

freshness 验证必须 fail-closed。producer build tree、安装 prefix、consumer source tree
和 consumer build tree 都必须在不压制错误的前提下删除；证据必须记录每个路径在清理前是否
存在，证明清理后路径及配置前的 `CMakeCache.txt` 均不存在，并从这些真实文件系统观察派生
`freshness_verified`。仅复制命令行中的“fresh”开关不能作为证据。

每次 producer/consumer 证据运行都必须根据同一次运行的 `actual` 观察和进程 invocation
生成 `environment.md`。该文件记录有序的 UTC 开始/结束时间、source HEAD、staged/unstaged
patch hash、普通未跟踪源码的路径/内容 hash 清单、请求和解析后的 CMake
executable/version、producer/install/consumer 路径，以及精确的进程内重放命令。compare
必须将这些字段与 `actual.json`、command log header、producer
freshness 和 producer `CMakeCache.txt` 对照；从旧运行手工保留的 environment 说明不构成有效
证据。维护 command/provider audit 是独立运行，必须保留自己的命令和时间窗口，不能混入
producer/consumer 的起止时间。

正式验证根在启动任何将被消费的 producer、consumer、test、structural 或 quality 运行前，
必须先冻结一个内容寻址的最终源码身份。每个子 artifact 都要从自己的 invocation 记录同一组
base commit/tree、final snapshot tree、final patch hash 和普通未跟踪源码 inventory。消费
personal-overlay 输入的检查还必须记录独立的 overlay commit/tree、snapshot 与 patch identity，
同时从 identity 排除生成的 evidence output，避免自引用。根 `compare.log` 必须在全部运行后
重新计算两套适用身份，并逐项断言每个子身份都与重算身份相等，否则 fail-closed。冻结后的
任何源码编辑都会使本轮运行失效；artifact 时间戳、目录名以及从旧 review 复制的 hash 都不能
替代这项相等性证明。

当所选 CMake generator 提供多个 configuration 时，package 证据必须为 producer 与 consumer
选择同一个 generator，记录两侧的 `CMAKE_GENERATOR` 和 `CMAKE_CONFIGURATION_TYPES` cache
值，并实际 build/install/run `RelWithDebInfo` 以及另一个可用 configuration。consumer
可执行文件仍必须从 configuration-specific `$<TARGET_FILE:...>` manifest 解析；仅有 synthetic
路径布局检查不能充当 multi-configuration 运行证据。

CMake 3.16 command/provider/context 审计把 root producer 与 package-config template 视为互相独立的执行
上下文。项目命令只有在本地声明之后，或在某个 `include()` 实际引入且真实 CMake 3.16
module source 确实提供它之后才可用；`@PACKAGE_INIT@` 与
`CMakeFindDependencyMacro` 都是显式且对位置敏感的 provider。所有维护中的
`cmake_policy(SET CMPxxxx value)` 调用都必须与真实 3.16 policy list 比较；每个未知
policy 使用点都必须位于精确的 `if(POLICY <same-id>)` enclosing scope 中。真实
`--help-command-list` 只证明 command name 存在；subcommand/keyword 兼容性结论只覆盖
证据中列明的有限敏感 token，不代表完整 CMake grammar。该 audit 与当前 GitHub CI package
consumer 会共同保护维护路径，而不会把每个 PR 锁定到某个 Ubuntu 或 CMake release。确有需要时，
针对性旧版本证据再补充严格 fresh active-path 运行。在真实 runner 执行 Apple 与 Windows 分支前，
这两个平台只具备 command/policy/context 与已记录敏感 token 的静态覆盖。

Issue 专属的 phase 与 residue guard 只作为 personal overlay 证据保护已经落地的
public-boundary 语义。第 15 轮副本位于
`tests/results/codebase-refactor/phase-4-static-product/review15/migration_gates/`，
不属于 primary repository 的 CTest 或 CI 输入。它们可以检查权威 OpenSpec 及中文镜像、架构
文档、utility diagram、Kernel/embedded-adapter source 和 CLI source，以证明某一迁移 snapshot。
这些证据确保 frontend 只经过 `ps::Host`，将 Kernel 和 InteractionService 归为 internal，
把 `Kernel::ComputeRequest` 的构造权限定在 adapter，并拒绝 GraphRuntime 中已废弃的通用
worker queue。Issue 专属 scan 归档后，长期运行时测试、public-header 编译和 package-consumer
测试负责维持产品边界。

## 验证与证据归属

Primary repository 中的 CTest 与 CI entry 只用于长期软件行为：正确性、性能、稳定性、多线程
执行、错误处理、编译边界、package consumption 和运行时 API 边界。
`StaticProductConsumerSmoke`、`GraphCliOptionBadAlloc`、GoogleTest discovery、
`PublicHeaderSelfContainment` 与 `SplitComputeServiceRuntimeTrace` 满足这一规则，因为它们会执行
或编译维护中的产品。Phase 名称、迁移 residue 搜索、陈旧术语 detector、源码布局完成度检查、
issue replay 或 evidence/provenance report 都不是软件行为测试，不能注册到 CTest 或由 CI 调用。

CLI/Host 与 scheduler Doxygen AST 工具是长期手工开发工具，不是测试。修改相应声明、定义、
异常契约或 target source closure 时，应显式运行：

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json --out <out>
python3 tests/verification/codebase_structure/scheduler_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json --out <out>
```

这些文件可以留在 primary repository，因为本文定义了它们的长期手工职责；它们必须始终不进入
CTest 与 GitHub CI。Issue 专属 replay、migration、formal-report、source-identity、provenance
与 evidence helper 应放在 personal overlay 的
`tests/results/<change-or-feature>/...` 下。Clean primary clone、CMake 配置、CTest inventory 和
CI script 都不能读取或 import 这些 overlay 内容。

验证应与风险成比例。实现期间只运行 scoped static check、受影响 build target 和 focused
regression。源码与文档冻结后，最多执行一次本机原生 clean configure、一次 full build 和一次带
JUnit 输出的完整 CTest。正式本地证据复用同一个 build tree 做 focused stress 与手工检查，不为
每个 gate 创建独立 build tree。本轮最终本地验证禁止使用 Docker 或本地 `linux/amd64` 模拟；
current-head GitHub Actions 仍是权威远程 integration 环境。

## CTest 注册

所有预期 GoogleTest 二进制都应注册到 CTest。这包括当前可能低置信度的里程碑测试和 `test_propagation_contracts`。

低置信度测试仍应在验证中可见，而不是被静默排除。如果测试不足以可靠地作为开发门禁，应明确记录该状态，并创建后续工作升级或替换它。

里程碑测试和 `test_propagation_contracts` 已注册到 CTest，因此它们可见；但在后续 pass 将它们重写为更窄、更清晰 fixture 和断言的回归测试前，它们仍是低置信度遗留测试。

`test_propagation` 不同：它是脚本式 REPL/tool 目标，不是 GoogleTest 二进制。CMake 保持它可构建，供手工脚本和临时验证使用，但 CTest 不会发现或运行它。不要把 `ctest` 证据写成覆盖了 `test_propagation`；如果使用该目标，应引用具体的手工命令 transcript。

默认 CTest inventory 刻意不包含 phase 完成度 scan、迁移 residue 检查、陈旧术语搜索、Doxygen
audit 或 personal-overlay helper。Static package-consumer smoke 与 graph CLI allocation-failure
driver 会继续注册，因为它们执行真实的安装/运行时行为。

## 已知测试质量注意事项

一些里程碑测试和传播契约测试最初是开发检查，而不是精修过的回归测试。它们应被注册以保持可见，然后在后续升级为更清晰、更高置信度的测试。

`test_propagation` 在被转换为合适的 GoogleTest 二进制，或被更窄的 CTest 注册 fixture 替换之前，仍保持为手工工具目标。

## Issue #34 第 15 轮审查本地验证

第 15 轮证据位于
`tests/results/codebase-refactor/phase-4-static-product/review15/`。正式 driver 会分别冻结 primary
与 personal-overlay 内容 identity，创建一个本机原生 clean `BUILD_TESTING=ON` tree，只执行一次
full build 和一次完整 CTest/JUnit，并让 focused scheduler/plugin/CLI/runtime stress、手工 AST 与
migration evidence 复用该 tree。它刻意不执行此前的第二个 `BUILD_TESTING=OFF` producer、
multi-configuration rebuild、Docker 或本地 `linux/amd64` 模拟。

长期 CTest inventory 包含真实 package consumption、CLI process error handling、public-header
编译、runtime trace behavior 和全部 GoogleTest binary。Review 专属 phase/residue/replay tool 只
位于 `review15/migration_gates/`；正式 driver 会直接调用选定工具，而 clean CMake、CTest 与 CI
不包含 personal-overlay 依赖。机器可读分类与理由位于
`review15/script_ownership_inventory.json` 及对应 Markdown 文件。

Focused coverage 新增 CPU/GPU 精确 exception-state rollback、跨 epoch publication gate、两阶段
scheduler running publication、scheduler plugin owner 完整 forwarding、GraphRuntime shutdown
sweep，以及可复用 CLI startup/option exception handling。正式本地 PASS 后，intentional 双仓
commit/push、current-head GitHub Actions 与 artifact review、fresh Codex/Copilot review 且 unresolved
thread 为 0，以及确有需要的真实 Windows/MSVC run 仍是外部门禁。Issue #34 与 active CLI layout
change 在本地继续保持未完成。

## Issue #34 第 14 轮审查本地验证

第 14 轮证据位于
`tests/results/codebase-refactor/phase-4-static-product/review14/`，由单一 fail-closed
orchestration 针对冻结且内容寻址的源码 snapshot 生成。根 compare 会消费每个子门禁自己的
identity，而不是信任目录名或旧 review 的 hash。

聚焦行为门禁覆盖：operation-plugin 精确 key unload、unload-all 与 destruction 不依赖分配、
相对路径 normalization 失败、按成功加载序号逆序恢复、
scheduler-plugin raw instance 在 host owner 分配/type-name copy 失败时的所有权、CPU/GPU batch
exception fencing、紧接失败后的干净复用、GPU HP-CPU idle-wait handshake，以及活跃 benchmark/CLI
`std::bad_alloc` 路径。Phase-4 differential fixture 会跟踪 reference、pointer/dereference、chained、
conditional 与 mutator-call alias，并要求同一未污染的 `exception_ptr` 完成 transport 与 rethrow；
CLI Clang AST audit 会检查全部维护中的 Host-facing 声明/定义及每个内部 broad catch。

正式产品门禁使用 `current_consumer/`、同一个 Ninja Multi-Config producer 的
`multiconfig/relwithdebinfo/` 与 `multiconfig/debug/`、clean `BUILD_TESTING=OFF` 产品/ABI 检查、
以及 clean `BUILD_TESTING=ON` 全目标构建与 CTest/JUnit。结构和质量门禁包括 phase-4/phase-7
scan、Host/member/dirty/GraphRuntime guard、OpenSpec/task audit、format/lint、Python static
check、Doxygen AST、整个授权源码树的 `__pycache__`/artifact 检查、两个 Git repository 的
structured diff、structure 参数 flat/pairs/invalid schema 差分，以及本机真实/缺失 CMake
provider 解析差分。缺失的 optional provider 会明确记录为不适用，且不会导入任何定义；缺失的
required provider 会形成结构化失败，而不是异常 traceback。

每个子结果都必须携带 `source_identity/` 冻结的适用 primary 与 overlay identity。
`provenance/` 与根 formal report 会在运行结束后重新核对全部身份；任何缺失或不相等的 identity
都会使完整本地结果失效。

按用户本轮决定，本地 `linux/amd64` 相同 CI 容器重放不再是第 14 轮完成门禁。先通过本机原生
门禁；当前 head 的 GitHub Actions 是权威 integration 环境，任何失败 artifact 都必须在那里
复核并修复。这不会替代已有的 CMake 3.16 最低版本证据，也不声称本轮重新执行了本地 CMake
3.16。本地 PASS 仍不能关闭 Issue #34 或归档 active change：intentional commit/push、权威
GitHub CI 与 artifact 复核、review-bot/thread resolution 和真实 Windows runner 仍是外部完成
门禁。

## Issue #34 第 13 轮审查本地验证

第 13 轮审查针对内容寻址的修复前源码关闭了本地可复现问题；该源码记录位于
`tests/results/codebase-refactor/phase-4-static-product/review13/pre_fix/`。
保存的失败二进制、运行 transcript、parser fixture、patch hash 和派生 compare，无需在当前
worktree 重新构建历史源码，即可复现本轮审查的全部六项缺陷。

对应的修复后证据按行为拆分，而不是只给出单一 pass/fail 声明：

- `post_fix/` 将 operation-plugin transaction 回归重复十次，将 CPU/GPU
  exception-publication 回归重复五十次。它证明：registrar 返回后的任一 allocation failure
  都不会改变 live registry、source map、result map 或 retained handle set；失败 callback
  会先于 library unload 析构；普通 registrar error 仍作为结果记录；`std::bad_alloc`
  会以原对象不变地重新抛出；scheduler 的首个 publisher 会先发布精确的
  `exception_ptr`，再让 flag 可见；并且两个 scheduler 在 worker 与 concurrent-publisher
  压力后都可复用。
- `phase4_static_product_scan/`、`phase7_plugin_registration/` 和
  `cli_host_doxygen_ast/` 分别保护 `std::bad_alloc` 控制流边界、版本化 operation-plugin
  ABI 及 transaction 语义，以及通过 Clang AST 验证的完整 Host-facing CLI Doxygen 契约。
- `consumer_smoke_current/`、两个 `multiconfig_*` 目录、`binary_abi_checks/` 和
  `product_off/` 覆盖 clean `BUILD_TESTING=OFF` static product、public-header consumption、
  runtime package 使用、同一个 multi-config producer 的两个 configuration，以及安装产品中
  不含 test seam。
- `final_clean/` 记录 clean `BUILD_TESTING=ON` 的 all-target build 和完整 CTest JUnit
  结果。`quality/` 记录格式化、lint、Python 编译/静态分析、Doxygen AST、diff integrity
  与 structured evidence 检查。`tasks_audit/` 证明中英文 task checkbox 保持同步，且
  Issue #34 在本地仍保持 open。

这些 artifact 只建立本地源码与运行时闭环，不能据此勾选 Issue #34 tracking item，也不能
归档其 active OpenSpec change。same-CI-container 运行、commit/push、remote CI 与 review-bot
门禁、review thread resolution 和真实 Windows runner 仍是外部完成门禁。

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
- `ci/scripts/integration_suite.sh`：顺序复现本地 container 验证，包括 CLI、propagation、plugin
  和 scheduler 检查。

`SplitComputeServiceRuntimeTrace` 会把临时输出写入 CMake build tree，而不会写到 personal
overlay 的 `tests/results`。当前 `ctest_full.sh` 的排除属于 CI 运行时间策略，而不是 overlay
依赖；focused 或完整本机 CTest 证据可以直接运行它。GitHub job 状态和
下载的 artifact 只能补充、不能替代 focused test 与任务专属的
`tests/results/...` expected/actual/compare 证据。完整 workflow、Docker 复现命令和 artifact 下载边界
记录在 `docs/CI/zh/github-actions.zh.md`。

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
`tests/test_graph_topology_boundaries.cpp`、`tests/test_propagation_contracts.cpp`，
以及 `tests/results/split-graph-traversal-service/` 下的可复现证据。
