# GitHub Actions CI

## Workflow

- `.github/workflows/ci-healthcheck.yml`：通过 `pull_request_target` 处理目标为 `main` 的 pull request，并在推送到 `main` 和 `CI/**`、手动触发时运行静态 healthcheck，最后由一个稳定的 `healthcheck` 结果门禁汇总。
- `.github/workflows/ci-integration.yml`：运行纯文档路由、配置期 inventory 预检、一个发布构建后 label-driven matrix 的可复用 default build、独立分片的完整 CTest 与逐测试 build-smoke job、`graph_cli` 脚本测试、propagation 脚本测试、插件加载测试、scheduler 重复测试，并由一个稳定的 `integration` 结果门禁汇总。
- `.github/workflows/ci-sanitizer.yml`：手动运行 ASan 或 TSan 聚焦检查。
- `.github/workflows/build-ci-image.yml`：在镜像输入相关 push 和手动触发时发布 GHCR 镜像 `ghcr.io/<owner>/<repo>/photospider-ci`。

## 分支与 workflow 保护

由 push 触发的 CI 只在 `main` 和名称以 `CI/` 开头的分支上运行。这样可以防止普通 feature 分支运行该分支自行修改过的 workflow 文件。

目标为 `main` 的 pull request 使用 `pull_request_target`，即使用 base 分支上的 workflow 定义，同时 checkout pull request 的 head commit 作为被测代码。只有 head repository 就是 base repository 本身时，`CI/**` pull request 才会去重并改由 push trigger 验证。Fork 可以使用同名分支，却不会在 base repository 中产生对应 push；因此 fork `CI/**` pull request 会在 checkout 前明确 fail closed，而不会得到伪造的绿色结论，也不会在这个带权限的 event 中执行不受信任的 fork 代码。

healthcheck 和 integration 的第一个 job 会在执行任何仓库脚本或本地 CI 镜像构建前保护 CI workflow 输入。只有同仓库 `CI/**` pull request 会改由 base-repository push run 处理。Fork `CI/**` pull request 会进入该 job 并在 checkout 前被拒绝；head-repository identity 缺失时也会 fail closed。对于其他 pull request，该 job 会从 base 仓库拉取目标分支，并使用 event 提供的精确 base/head commit；其他受保护 run 会拉取 `origin/main` 并使用 `HEAD`。两条 diff 路径都要求恰好一个 merge base，再在关闭 rename detection 与 Git status 过滤后比较 merge-base tree 和选定的 head。Changed path 会作为 NUL 分隔记录写入父 shell 可见的 artifact，再由 Bash array 按完整路径值读取和匹配。供人阅读的 changed/protected 清单使用 shell-safe `%q` 表示，因此内嵌换行既不能拆分路径，也不能伪造日志记录。Git producer 或清单读取失败时，门禁会在输出成功摘要前终止。这样，手动触发且落后于 `main` 的 ref 仍保持 three-dot 语义，type change、少见 status 或包含特殊字符的合法文件名也都会进入受保护路径清单。所得 diff 修改以下任一路径时，门禁会失败：

- `ci/**`
- `.github/workflows/**`
- `Dockerfile.ci`

因此，CI workflow 相关修改必须放在 base repository 的 `CI/**` 分支开发。分支前缀本身绝不是授权：pull request 还必须以 base repository 作为 head-repository identity，而 push 和手动 run 本来就属于 base repository。该保护也覆盖目标为 `main` 的非 `CI/**` pull request，避免 workflow 相关文件通过普通 feature 分支或 fork 中的同名分支合并。

## 纯文档路由

integration workflow 会在受保护路径门禁之后运行 `change-classification`。只有当每个改动路径都属于以下范围时，变更才是纯文档变更：

- `docs/**` 下的任意文件，包括所有中文镜像；
- 根目录的 `*.md` 或 `*.markdown` 文件，包括 `readme.md`、`manual.md` 和 `CONTEXT.md`；
- 根目录无扩展名的 `README`、`LICENSE`、`NOTICE`、`CHANGELOG`、`CONTRIBUTING`、`CODE_OF_CONDUCT` 或 `SECURITY` 契约，匹配时不区分大小写。

所有其他路径都要求执行完整构建与测试链。这包括源码和头文件、CMake 文件、测试、插件、应用、CI 脚本、workflow 与 action、配置、依赖与 lockfile、Docker 输入、asset、`docs/**` 之外的嵌套 Markdown，以及任何未知文件。分类器使用不带 status filter 的 `git diff --no-renames`，因此把源码文件重命名到 `docs/**` 后仍会暴露被删除的源码路径，不会被误判为纯文档。新增、复制、删除、修改、重命名、type change（`T`）、未合并、broken-pairing 和 unknown-status 路径都会进入清单；少见 status 不会仅仅因为未列入 allowlist 而被遗漏。

对于 `pull_request` 和 `pull_request_target`，分类器要求 event 提供精确的 base/head SHA 和唯一 merge base，再评估从该 merge base 到 head 的 pull request diff。`main` push 会比较精确的 `before` 与 head tree。每次 `CI/**` push 都始终执行完整链，即使后续一次增量 push 只包含文档也不例外；这样可以避免同一分支上更早的源码或 workflow commit 在 pull-request-trigger 去重后逃过 current-head integration。`workflow_dispatch` 也始终执行完整链。event 不受支持，push branch identity 缺失或格式错误，revision 缺失、格式错误、全零、来自浅克隆或不可达，merge base 缺失或不唯一，diff 失败，或者 changed-path 清单为空时，都会 fail closed 到完整 integration。workflow 使用 `fetch-depth: 0`；event identity 不可用时绝不会猜测 `origin/main` 或 `HEAD~1`。

对于纯文档变更，`ci-image-change`、integration 规划、所有 build、完整 CTest、build smoke 与脚本式 integration 分片都会被有意跳过。始终运行的 `integration` 门禁会校验这些 job 的确得到 `skipped` 结论，并把原因写入 GitHub step summary。分类或上游依赖失败时，该门禁会失败，不会因为 `needs` 静默传播 skip 而通过。workflow 保持触发，不使用 `paths-ignore`，因此稳定 required check 会得到结论，而不会一直 pending。`healthcheck` 门禁也始终给出结论，并校验实际选中的 published-image 或 local-image healthcheck 路径。只有同仓库 `CI/**` pull request 会报告有意采用 push-triggered 去重；fork 或缺失 repository identity 时不能走该捷径。

## 运行环境

`Dockerfile.ci` 定义 GitHub Linux 测试环境。`healthcheck-published-image` 是 container job，published-image healthcheck 执行 job 与 build/test integration job 会在 `ghcr.io/<owner>/<repo>/photospider-ci:latest` 中运行。Published-image job 不依赖 checkout 的临时 HOME 范围 Git trust 在后续 container step 中继续存在。Checkout 之后，唯一的 `Trust checked-out workspace` step 会显式选择 `shell: bash`，只把精确的 `$GITHUB_WORKSPACE` 值加入该 job container 持久的 global `safe.directory` 配置，并通过只读 Git 命令校验 `HEAD^{commit}`。它绝不会配置 `safe.directory=*`、信任父目录或执行 checkout 得到的仓库脚本。该边界先于两个条件 history fetch 与 `healthcheck.sh` 完成，因此也覆盖两个 fetch 都不执行的 published-image `main` push 与 `workflow_dispatch` run。`Fetch pull request base history` 与 `Fetch CI branch main history` step 同样显式设置 `shell: bash`，因此各自的 `set -Eeuo pipefail` 前导命令会由 Bash 执行，而不是依赖 container 默认 shell。protected-path、change-classification 与稳定结果门禁仍是轻量 `ubuntu-latest` job，不会 configure 或编译项目。

当 pull request 或 push 修改 CI 镜像输入（`Dockerfile.ci`、`.dockerignore` 或 `.github/workflows/build-ci-image.yml`）时，healthcheck 和 integration workflow 会在当前 workflow 内构建 `photospider-ci:local`，并在该镜像中运行相同脚本。Pull-request 镜像检测会拉取 base 仓库分支、校验 event 的精确 base SHA，再从该 base 开始比较，而不依赖 fork 中可能不存在的 `origin/<base>`。对于 pull request，published-image 与 local-image healthcheck job 都会在各自 job 内把 `CI_BASE_SHA` 绑定到 event base，从 base-repository URL 拉取目标分支，校验该精确 commit，并把 event 的精确 SHA 作为 `CI_BASE_REF`。Published-image 的校验先于 `healthcheck.sh`；local-image 的校验先于构建 head Dockerfile 或执行挂载 workspace。因此，即使 fork checkout 的 `origin` 不包含 base tip，event 的精确 SHA 仍然可解析。

对于每次 `CI/**` push，两条 healthcheck 路径都会改为在各自 job 内拉取并校验 `origin/main`，再把 `origin/main` 作为 `CI_BASE_REF`。Published-image job 会在 `healthcheck.sh` 前完成校验；local-image job 会在 Docker build 与执行挂载 workspace 前完成校验。因此，静态检查会覆盖从 `main` merge base 开始的累计 branch diff，后续纯文档 push 无法隐藏更早的未格式化 C++ commit。普通 `main` push 则继续把精确的 `github.event.before` 作为 `CI_BASE_REF`，只检查本次 push 增量。任何必需 fetch 或 ref 校验失败都会在 `healthcheck.sh` 使用 fallback base 选择之前终止 job。对于 `CI/**` push，CI 镜像检测同样使用累计 `origin/main` 基线，因此后续纯文档 push 也无法隐藏更早的镜像输入 commit。

镜像输入 detector 使用关闭 rename detection 且不带任何 Git status filter 的清单；删除、type change 与少见 status 都保持可见。Healthcheck 静态范围清单同样关闭 rename detection，但会有意使用 `--diff-filter=d`：由于 formatter 与 linter 要求当前文件，删除路径会被排除，而 type change 与其他少见的非删除 status 仍保持可见。两份清单都使用 NUL 分隔的 Git path，因此含换行的文件名仍保持为一个精确路径，并且都会先把清单写入父 shell 可观察的文件。`git diff` 失败时，脚本会在输出任何 `changed=false` 或“No changed C++ files”摘要前非零退出。这样既避免假路由，也避免另一个 workflow 尚未发布新 `latest` 镜像时产生竞态。

镜像包含 CMake、C++ 工具链、OpenCV、yaml-cpp、CURL、OpenSSL、GTest、
nlohmann-json、Python、cpplint 和 clang-format。Formatter 通过 PyPI wheel 安装并固定为
21.1.5，避免 CI 与开发机仅因 formatter release 不同而选择不同换行，产生无意义的格式漂移。
维护机当前使用 21.1.3；在本轮对齐覆盖的 changed C++ 清单上，两者的格式化输出已经过逐字节
等价验证。后续建议开发环境统一采用 21.1.5。

本次改动只对齐 clang-format 工具版本。这不是版本检测门禁，不会新增专用 CI job，也不会新增
Ubuntu/CMake 版本锁定；既有 Ubuntu base 与 apt 提供的 CMake 设置保持不变。

## Integration 测试分片

对常规 published-image 路径中的非文档变更，`integration-plan` 会启用测试来配置被 checkout 的
commit，并把 `ctest --show-only=json-v1` 解析为非权威预检。CMake 默认的
`gtest_discover_tests` 模式要等 target 构建后才发现 GoogleTest case，因此配置期 CTest 状态可能
只包含未带标签的 `*_NOT_BUILT` 占位项，且没有任何带标签 entry。预检会允许空 label selection，
但仍拒绝 malformed JSON、重复 test 或 property、非法 label，以及任何 disabled 或 commandless
的带标签 entry。其 preview matrix 只作为诊断 artifact 保留，绝不会暴露为 workflow job output。

随后，`build-integrity-default` 会构建完整 default tree，并再次执行相同 JSON 查询。这次构建后
调用才是权威来源：它要求精确 `build-smoke` selection 非空，校验 `ctestInfo` 版本、完整测试名
唯一性、property/label 形状、enabled 状态、可执行 command 与 matrix 大小，并通过稳定 job
output 输出紧凑 matrix。Artifact key 由有界 ASCII slug 与测试名 SHA-256 digest 派生，精确测试名
保留为 JSON matrix value；matrix 按不区分大小写的测试名稳定排序，workflow 不维护测试名清单。
一个聚焦的真实 CMake fixture 会配置 `gtest_discover_tests` target，观察配置期 `_NOT_BUILT`
占位项与空 preview，构建 target，再要求严格构建后 matrix 包含随后发现的带标签 case。

当前带标签的 inventory 为：

- `DependencyDisabledInstallSmoke`
- `ImageArtifactCodecDependencyDisabledBuild`
- `IpcDisabledInstallSmoke`
- `OpenCvOperationProviderDisabledBuild`
- `PublicHeaderSelfContainment`
- `StaticProductConsumerSmoke`

四个 dependency/configuration driver 与 static-product consumer 会创建或校验隔离的 nested
build profile；public-header self-containment 会调用专用 compile target。它们属于长期
product、package、configuration 与 compile 边界，不是 migration 或 source-layout 检查。
`OpenCvOperationProviderBuildSmokeSafety` 继续作为 OpenCV nested-build driver 的普通完整 CTest
safety regression：其 Python unittest 会在进程内验证 cleanup guard 与 cache-layout helper，
但不会启动 child configure、build、install 或 compile target。

`build-integrity-default` 会构建一次完整 default profile，并上传 `ci-build-default`。普通测试 job
会复用该 artifact：

- `full-ctest`、`scripted-cli`、`propagation-script`、`plugin-load` 和 `scheduler-repeat` 只下载 `ci-build-default`。
- `full-ctest` 使用 CTest label filter 排除每个精确 `build-smoke` 标签，不再维护按名称排除的
  build-smoke 清单。
- `build-smoke` 通过 `fail-fast: false` 消费 build-integrity job 输出的构建后 JSON include
  matrix。当 producer job 被有意跳过或没有 output 时，字面量空 include fallback 会保证
  `fromJSON` 仍有效；job-level 路由仍要求预检与 build-integrity 成功，而后者的严格 matrix
  不可能为空。每个 matrix entry 都会显示为独立的 `Build smoke (<精确 CTest 名>)` job，下载 `ci-build-default`，获得独立
  20 分钟 job timeout 与 artifact 路径，并且只运行所选 CTest entry。CTest 注册继续保留各自
  300 到 900 秒 timeout 与既有 `RUN_SERIAL` 语义。

Runner 会在执行前立即重新查询 CTest JSON，并要求选定的精确名称仍然唯一、enabled、可执行且
带标签。完成该标签校验后，执行只使用校验过的 CTest 数字索引；测试名不会插入 shell command
或 regular expression。于是 `IpcDisabledInstallSmoke` 会通过长期维护的 CTest 注册执行，
自行创建 clean `PHOTOSPIDER_BUILD_IPC=OFF` producer，不再依赖 workflow 中单独硬编码的 profile。

如果 CI 镜像输入发生变化，workflow 不能使用此前发布的镜像，而会在一个具备 Docker 能力的 runner
上运行 `local-image-integration`。构建 `photospider-ci:local` 后，`integration_suite.sh` 会构建
default profile，读取同一个构建后 NUL 分隔精确名称清单，在 full CTest 中排除该标签，
再顺序运行每个带标签 smoke，随后运行 CLI、propagation、plugin 与 scheduler 分片。该 fallback
保持相同的发现与选择契约，但接受单个 local-image runner 无法扇出 matrix job 的限制。

CMake 3.16 是项目兼容性下限，不是每个 pull request 都固定运行的 workflow 版本。维护中的
构建逻辑会保护晚于该下限引入的 policy，当前 integration 则在受支持 CI toolchain 上执行 fresh
static package consumer。只有 compatibility-sensitive change 或 release check 确有需要时，才补充
针对性的原生旧版本 producer/install/consumer 运行；常规 integration workflow 不会通过专用最低
版本 job 锁定 Ubuntu 或 CMake。

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

CI 与 CTest 只执行长期软件行为、编译、package-consumer、性能、并发、稳定性、错误处理和运行时
边界检查。迁移 residue scan、phase 完成度检查、陈旧术语搜索、Doxygen/source-quality audit、
issue replay 与 evidence/provenance orchestration 都必须排除。Issue 专属 replay、provenance、
helper 和 output artifact 不得进入 primary repository，也不得作为 personal overlay 的长期内容
保留。明确记录的通用手工开发工具属于另一类内容；clean primary checkout 绝不能 import 个人开发
内容。

- `ci/scripts/healthcheck.sh`：建立 NUL 分隔的 changed-path artifact，运行 `git diff --check`、长期 change-classification、build-smoke inventory 与 CI-routing 回归，并对每个未删除的 changed C++ 路径运行 `clang-format --dry-run --Werror` 与 `cpplint`；清单失败时会在输出无 C++ 摘要前终止。
- `ci/scripts/change_classification.sh`：把 event 的精确 revision 分类为纯文档或完整 integration，记录所有改动路径与非文档路径，并在 Git 状态不确定时 fail closed。
- `ci/scripts/change_classification_test.sh`：覆盖文档、源码、混合、type change、workflow、重命名、删除、重复 `CI/**` push、pull-request merge-base、branch 或 revision 缺失、全零/不可达 revision、手动触发、空 diff 与浅克隆场景，验证长期路由契约。
- `ci/scripts/ci_routing_test.sh`：对两份生产 `protected-ci-paths.if` 表达式做空白归一化并锁定精确源码，再抽取并执行真实 stable-gate、checkout 前 fork-rejection 与 protected-path shell block。它还会锁定允许空集合的配置期预检、严格构建后 job output、对空 output 安全的 `fromJSON` matrix、逐项 artifact/name binding、full-CTest label exclusion、精确 runner input、local fallback inventory 与聚合 build-smoke gate。隔离 Git fixture 会证明两份生产门禁都拒绝含换行的 `ci/**` 路径、安全记录该路径，并在 producer 或 reader 失败时 fail closed。一个 job/step-scoped production 断言会抽取 published-image 中两个精确的 history-fetch step，并要求各自拥有顶层 `shell: bash`，因此其他 job 或相邻 step 的元数据无法满足该契约。另一个 job/step-scoped 断言要求恰好一个使用 `shell: bash` 的 `Trust checked-out workspace` step；它唯一可执行的内容必须是启用严格模式、把精确 `$GITHUB_WORKSPACE` 加入 global `safe.directory`，以及校验 `HEAD^{commit}`。其他 job 或相邻 step 中的条目、任何额外或通配的 `safe.directory`，以及晚于任一 fetch 或 `healthcheck.sh` 的位置都无法满足断言。抽取出的 production trust block 会在隔离 HOME 与 Git 仓库中运行，并要求所得 global 配置只包含该仓库的精确路径。Job-scoped 断言还分别锁定 published-image 与 local-image 的 pull-request 精确 base fetch、`CI/**` main fetch/校验、三路 `CI_BASE_REF` 路由及执行顺序。测试会执行两份抽取出的 production main-fetch block；隔离历史会证明累计 `origin/main` 范围保留较早的未格式化 C++ 路径，而 event-before 范围只包含较晚的文档路径。Detector fixture 继续覆盖精确/累计 base、空比较、含换行路径及 changed-path 失败传播。这些本机源码与 shell 检查明确不声称执行 GitHub expression evaluator、复现跨 UID dubious ownership 或模拟托管 container runner。
- `ci/scripts/ci_image_changed.sh`：检测当前 NUL 分隔且不带 status 过滤的 diff 是否修改 CI 镜像输入；workflow 会向它提供已拉取并验证的 pull-request 精确 base SHA，diff 失败时不会输出路由。
- `ci/scripts/build_smoke_inventory.py`：严格解析 CTest JSON v1，输出确定性 matrix 与 NUL 分隔精确名称，并在基于索引执行前重新校验一个 matrix selection。严格构建后模式拒绝空 selection；只有显式 preflight 模式允许为空。Focused regression 会覆盖 malformed JSON/schema、重复名称/property/label value、非法或缺失 label、disabled/commandless entry、严格空 selection、确定性排序、JSON round trip、安全 artifact key、敌意测试名字符、在执行前停止的 absent/disabled/commandless runner selection，以及真实配置期占位到构建后发现过程。
- `ci/scripts/integration_plan.sh`：配置一个启用测试的小型 build tree，并校验允许空集合、非权威的配置期 inventory preview；它不会输出 workflow matrix。
- `ci/scripts/integration_suite.sh`：消费严格构建后输出的精确名称，为 local-image fallback 顺序运行所得 integration 分片。
- `ci/scripts/build_integrity.sh`：构建 default profile 的 required targets 与完整 tree，严格校验构建后带标签 CTest inventory，将 matrix 暴露为 workflow job output，并为 build 加可复用 stamp。
- `ci/scripts/ctest_full.sh`：复用或构建 default producer，并在排除精确 `build-smoke` label 后运行 CTest。
- `ci/scripts/build_smoke_test.sh`：从可复用 default producer 重新校验并运行 `SMOKE_TEST_NAME` 指定的精确 CTest 名称。
- `ci/scripts/graph_cli_script_test.sh`：使用上述执行前 Graph 文档能力标记，运行相互隔离的正路径、显式来源缺失和无效 target REPL 检查。
- `ci/scripts/propagation_script_test.sh`：构建 `test_propagation`，并对线性和复杂 propagation 图运行 `tiles all`。
- `ci/scripts/plugin_load_test.sh`：检查插件产物、plugin manager 测试、scheduler plugin loader 测试和 CLI scheduler 插件列表。
- `ci/scripts/scheduler_repeat_test.sh`：重复运行关键 scheduler 测试。
- `ci/scripts/sanitizer_test.sh`：在独立 build 目录运行聚焦 ASan 或 TSan 测试。

## 本地命令

```bash
CI_ARTIFACT_DIR=CI-results/healthcheck bash ci/scripts/healthcheck.sh
CI_CHANGE_EVENT=push \
  CI_CHANGE_BRANCH=main \
  CI_CHANGE_BASE_SHA="$(git rev-parse HEAD~1)" \
  CI_CHANGE_HEAD_SHA="$(git rev-parse HEAD)" \
  CI_ARTIFACT_DIR=CI-results/change-classification \
  bash ci/scripts/change_classification.sh
bash ci/scripts/change_classification_test.sh
python3 -B ci/scripts/build_smoke_inventory_test.py
bash ci/scripts/ci_routing_test.sh
CI_ARTIFACT_DIR=CI-results/integration-plan \
  bash ci/scripts/integration_plan.sh
GITHUB_OUTPUT=/tmp/photospider-build-integrity.out \
  BUILD_DIR="$PWD/build/ci-default" CI_BUILD_PROFILE=default \
  CI_ARTIFACT_DIR=CI-results/build-integrity-default \
  bash ci/scripts/build_integrity.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR=CI-results/ctest-full bash ci/scripts/ctest_full.sh
BUILD_DIR="$PWD/build/ci-default" CI_REUSE_BUILD=ON \
  SMOKE_TEST_NAME=DependencyDisabledInstallSmoke \
  CI_ARTIFACT_DIR=CI-results/build-smoke/dependency-disabled \
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

可以把 `SMOKE_TEST_NAME` 替换成
`CI-results/build-integrity-default/build_smoke_matrix.json` 输出的任一精确名称；runner 会拒绝 absent、
duplicate、disabled、commandless 或未带标签的选择。要从 configured tree 直接运行全部带标签
smoke，可使用
`ctest --test-dir build/ci-default -L '^build-smoke$' --output-on-failure`。要复现动态选择后的
完整执行顺序，可使用：

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

上述本地 Docker 命令复现的是维护中的 current-toolchain CI 路径，不代表 CMake 3.16 本身已经
运行。若确实需要针对性旧版本证据，而本机没有原生兼容 executable，应记录该限制，不要用架构
模拟制造最低版本 PASS。

## 本地 artifact 下载

使用 personal overlay 脚本下载 GitHub Actions artifact：

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

脚本写入 `CI-results/`。该目录属于 personal overlay，不能提交到主 GitHub 仓库。
