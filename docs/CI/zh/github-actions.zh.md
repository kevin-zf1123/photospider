# GitHub Actions CI

## Workflow

- `.github/workflows/ci-healthcheck.yml`：通过 `pull_request_target` 处理目标为 `main` 的 pull request，并在推送到 `main` 和 `CI/**`、手动触发时运行静态 healthcheck，最后由一个稳定的 `healthcheck` 结果门禁汇总。
- `.github/workflows/ci-integration.yml`：负责纯文档与镜像输入路由、published/candidate 镜像身份选择、唯一 build-once candidate 调用、唯一 shared-suite 调用、same-digest promotion，以及一个稳定的 `integration` 结果门禁。
- `.github/workflows/ci-integration-suite.yml`：由 published 与 candidate 镜像共用、使用 typed digest-qualified 输入的可复用测试 DAG，包含一个 same-tree producer、角色制品、普通 CTest、default/dedicated/OpenEXR build smoke、脚本式 runtime 分片及 Darwin/Linux security profile。
- `.github/workflows/ci-sanitizer.yml`：手动运行 ASan 或 TSan 聚焦检查。
- `.github/workflows/build-ci-image.yml`：只接受可信 push 的可复用 candidate producer，为一个 event-scoped 临时 GHCR tag 生成精确 digest/attestation；它不能发布 canonical tag 或 `latest`。

## 分支与 workflow 保护

由 push 触发的 CI 只在 `main` 和名称以 `CI/` 开头的分支上运行。这样可以防止普通 feature 分支运行该分支自行修改过的 workflow 文件。

目标为 `main` 的 pull request 使用 `pull_request_target`，即使用 base 分支上的 workflow 定义，同时 checkout pull request 的 head commit 作为被测代码。只有 head repository 就是 base repository 本身时，`CI/**` pull request 才会去重并改由 push trigger 验证。Fork 可以使用同名分支，却不会在 base repository 中产生对应 push；因此 fork `CI/**` pull request 会在 checkout 前明确 fail closed，而不会得到伪造的绿色结论，也不会在这个带权限的 event 中执行不受信任的 fork 代码。

healthcheck 和 integration 的第一个 job 会在执行任何仓库脚本或本地 CI 镜像构建前保护 CI workflow 输入。只有同仓库 `CI/**` pull request 会改由 base-repository push run 处理。Fork `CI/**` pull request 会进入该 job 并在 checkout 前被拒绝；head-repository identity 缺失时也会 fail closed。对于其他 pull request，该 job 会从 base 仓库拉取目标分支，并使用 event 提供的精确 base/head commit；其他受保护 run 会拉取 `origin/main` 并使用 `HEAD`。两条 diff 路径都要求恰好一个 merge base，再在关闭 rename detection 与 Git status 过滤后比较 merge-base tree 和选定的 head。Changed path 会作为 NUL 分隔记录写入父 shell 可见的 artifact，再由 Bash array 按完整路径值读取和匹配。供人阅读的 changed/protected 清单使用 shell-safe `%q` 表示，因此内嵌换行既不能拆分路径，也不能伪造日志记录。Git producer 或清单读取失败时，门禁会在输出成功摘要前终止。这样，手动触发且落后于 `main` 的 ref 仍保持 three-dot 语义，type change、少见 status 或包含特殊字符的合法文件名也都会进入受保护路径清单。所得 diff 修改以下任一路径时，门禁会失败：

- `ci/**`
- `.github/workflows/**`
- `Dockerfile.ci`
- `.dockerignore`

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

`Dockerfile.ci` 定义 GitHub Linux 测试环境。受保护 lock 中的
`photospider-ci:latest` 只用于发现 locator。可信 host job 会解析它，并在创建 formal artifact
directory、保留 runner identity 或允许 Docker 拉取/展开任何 image layer 之前，在 process-private
memory 中校验精确 subject 的 source/signer workflow attestation。失败会使 always-upload path 保持
absent、final workflow output 不变。只有成功后它才持久化 attestation evidence、拉取
digest-qualified image、校验 OCI revision 与 canonical manifest label，并且只输出最终的
`ghcr.io/<owner>/<repo>/photospider-ci@sha256:...` 引用。Published-image healthcheck 与 build/test
integration job 都执行该已校验的 digest-qualified 引用；没有 job 直接执行可变 locator。
`healthcheck-published-image` 是 container job，并不依赖 checkout 的临时 HOME 范围 Git trust 在
后续 container step 中继续存在。Checkout 之后，唯一的 `Trust checked-out workspace` step 会显式
选择 `shell: bash`，只把精确的 `$GITHUB_WORKSPACE` 值加入该 job container 持久的 global
`safe.directory` 配置，并通过只读 Git 命令校验 `HEAD^{commit}`。它绝不会配置
`safe.directory=*`、信任父目录或执行 checkout 得到的仓库脚本。该边界先于两个条件 history fetch
与 `healthcheck.sh` 完成，因此也覆盖两个 fetch 都不执行的 published-image `main` push 与
`workflow_dispatch` run。`Fetch pull request base history` 与 `Fetch CI branch main history` step
同样显式设置 `shell: bash`，因此各自的 `set -Eeuo pipefail` 前导命令会由 Bash 执行，而不是依赖
container 默认 shell。protected-path、change-classification 与稳定结果门禁仍是轻量
`ubuntu-24.04` job，不会 configure 或编译项目。

当 pull request 或 push 修改 canonical CI-image lock 的 `input_paths` 中任一路径时，detector 仍会拉取并校验精确 base，而不依赖 fork 中可能不存在的 `origin/<base>`。Detector 会分别严格解析 merge base 与 head 的 lock，要求 lock 把自身列为输入，并把 NUL 分隔的 Git path inventory 与两个已验证路径集合的并集比较。唯一允许 base 缺失的例外，是 head 在精确 diff path 新增一份 strict、self-including 的 regular lock blob 的真实首次引入；它始终路由 `changed=true`。因此，lock 增加、删除、重复、路径遍历、JSON 畸形、head 缺失或 malformed，或未经证明的 bootstrap，都不能输出 `changed=false`。Fork head 会在 checkout 前被拒绝，同仓库受保护 `CI/**` pull request 则使用其可信 push 路径。Image-change healthcheck job 不构建镜像：它会先校验精确 hosted runner、受保护 lock、canonical publish-source identity 与生成的 image-input manifest，再运行静态 healthcheck。只有符合条件的可信 `main` 或 `CI/**` integration push 可以调用唯一 candidate-image producer 与 shared digest-bound suite。

即使 major OS 已明确，hosted label 仍是可变的。GitHub 说明 runner image deployment 通常需要两到三天，
rollout 期间会创建 prerelease，而且具体 job version 必须从 `Set up job` 读取
（[官方 runner-images 说明](https://github.com/actions/runner-images#what-image-version-is-used-in-my-build)）。
因此，有限 Linux rollout set 同时包含 stable `ubuntu24/20260816.277.1` 与 rollout
`ubuntu24/20260823.283.1`。前者已在 exact-head healthcheck run
[`32997831039`](https://github.com/kevin-zf1123/photospider/actions/runs/32997831039/job/98271915852)
和 Integration builder run
[`32997831190`](https://github.com/kevin-zf1123/photospider/actions/runs/32997831190/job/98271974769)
中观察，并对应官方
[`ubuntu24/20260816.277` release](https://github.com/actions/runner-images/releases/tag/ubuntu24%2F20260816.277)。
后者已在 run
[`32991073228`](https://github.com/kevin-zf1123/photospider/actions/runs/32991073228/job/98248727299)
中观察，并对应官方 rollout
[`ubuntu24/20260823.283` prerelease](https://github.com/actions/runner-images/releases/tag/ubuntu24%2F20260823.283)。
Darwin set 同样包含 stable `macos15/20260727.0256.1`，绑定 vcpkg commit
[`6d9d7df564a1ccdaa994e4ad39ccd4a32360867b`](https://github.com/microsoft/vcpkg/commit/6d9d7df564a1ccdaa994e4ad39ccd4a32360867b)，
以及 rollout `macos15/20260824.0311.1`，绑定
[`127402f1c75bb3d5ff6bce04b285faa4930a5aca`](https://github.com/microsoft/vcpkg/commit/127402f1c75bb3d5ff6bce04b285faa4930a5aca)；
两者分别匹配官方
[`macos-15-arm64/20260727.0256` release](https://github.com/actions/runner-images/releases/tag/macos-15-arm64%2F20260727.0256)
和
[`macos-15-arm64/20260824.0311` prerelease](https://github.com/actions/runner-images/releases/tag/macos-15-arm64%2F20260824.0311)。
每个 job 只测量一次环境，并保留一份精确 resolved runtime record。Linux allowlist lock byte 继续作为
image-manifest input，而 manifest 的 `builder_runner` 与 OCI builder-version label 绑定实际 builder member；
运行在另一个 approved member 上的 verifier 会重建这份 retained provenance，而不会用自身
`ImageVersion` 替换。Darwin consumer 使用 retained record 中一对一的 vcpkg mapping。未知或被篡改的
record 会在 candidate work 前失败。Runner lock 与 retained-record input 强制要求 `O_NOFOLLOW`、
`O_NONBLOCK` 和 `O_CLOEXEC`；FIFO 与 device path 会在不阻塞的情况下被拒绝，fresh retained output
则使用 `O_EXCL` 拒绝任何 residual path。Rollout 完成后，移除 retired member 必须通过 reviewed
protected-lock update 并生成新镜像。
Manifest creation 会独立使用 `O_NOFOLLOW`、`O_NONBLOCK` 与 `O_CLOEXEC` 对每个 canonical image
input 恰好打开一次，从 retained descriptor 的两次一致读取获得 digest 与 size，并复用 retained lock、
helper、action 与 runner byte 完成 semantic check，不再重新打开。

该 callable producer 本身也是一份完整 parsed-tree 合同：它只暴露 typed `workflow_call`、精确 write
permission，以及一个带有受审查有序 step 的 `ubuntu-24.04` build job。Checkout 与 prebuild
`ci_lock_verify.py` 调用必须先于唯一的 `docker/build-push-action`；该 action 不得带 env 或 condition，
只能 push event-scoped temporary tag，并且只能接收精确 context、Dockerfile、三个 immutable label 以及
manifest/source-commit build argument。任何额外 step、Docker/Buildx 命令、canonical 或 `latest` 写入、
build argument、field 或 job 都会在镜像构建前被受保护 verifier 拒绝。

对于每次 `CI/**` push，两条 healthcheck 路径都会在各自 job 内拉取并校验 `origin/main`，再把 `origin/main` 作为 `CI_BASE_REF`。Published-image job 会在 `healthcheck.sh` 前完成校验；image-change job 会在 canonical manifest 生成和 `healthcheck.sh` 前完成校验。因此，静态检查会覆盖从 `main` merge base 开始的累计 branch diff，后续纯文档 push 无法隐藏更早的未格式化 C++ commit。普通 `main` push 则继续把精确的 `github.event.before` 作为 `CI_BASE_REF`，只检查本次 push 增量。任何必需 fetch 或 ref 校验失败都会在 `healthcheck.sh` 使用 fallback base 选择之前终止 job。对于 `CI/**` push，CI 镜像检测同样使用累计 `origin/main` 基线，因此后续纯文档 push 也无法隐藏更早的镜像输入 commit。

镜像输入 detector 使用关闭 rename detection 且不带任何 Git status filter 的清单；删除、type change 与少见 status 都保持可见。它的 Python reader 直接消费 Git 的 NUL 分隔 byte，并写入经 JSON quoting 的诊断路径日志；Bash whitespace 或 newline parser 不参与路径分类。Healthcheck 静态范围清单同样关闭 rename detection，但会有意使用 `--diff-filter=d`：由于 formatter 与 linter 要求当前文件，删除路径会被排除，而 type change 与其他少见的非删除 status 仍保持可见。任何 lock 读取或 `git diff` 失败都会在输出 `changed=false` 或“No changed C++ files”摘要前非零退出。这样既避免假路由，也避免另一个 workflow 尚未发布新 `latest` 镜像时产生竞态。

镜像包含 CMake、C++ 工具链、OpenCV、yaml-cpp、CURL、OpenSSL、GTest、
nlohmann-json、Python、cpplint 和 clang-format。Formatter 通过 PyPI wheel 安装并固定为
21.1.5，避免 CI 与开发机仅因 formatter release 不同而选择不同换行，产生无意义的格式漂移。
维护机当前使用 21.1.3；在本轮对齐覆盖的 changed C++ 清单上，两者的格式化输出已经过逐字节
等价验证。后续建议开发环境统一采用 21.1.5。

clang-format 对齐不会新增独立的 version-detection job。CI image 会另行通过 digest 绑定 Ubuntu
base，并在同一个已签名 immutable APT snapshot 中按精确版本绑定每个直接 Ubuntu package。由于
最小 base 不含 TLS trust bundle，Docker BuildKit 会先通过带 checksum 的 `ADD` instruction，仅获取
该 snapshot 中精确的 `openssl` 与 `ca-certificates` package。在任何 APT 命令运行前，受保护的
`ci/locks/ubuntu-24.04-snapshot.sources.in` Deb822 template 会把 base 的 archive、security 与 ports
source 全部替换为带 timestamp 的 `snapshot.ubuntu.com` URI；同一份已签名 archive 同时服务原生
amd64 与 arm64 index。随后 `dpkg` 配置已验证的离线 byte，第一且唯一的 APT update/install sequence
只从该显式 source 消费完整 package lock。任何 live APT bootstrap 或 mirror override 都不是可信
fallback；checksum 失败或同一 snapshot 的 closure 不可解时，candidate image build 会失败。
每个 lock row 都带精确 version；Debian package name 至少两个字符，首字符必须为字母数字，其余字符
只允许小写字母数字与 `+.-`。Installer 会把 apt 的 `--` option terminator 放在全部固定 option 之后、
locked `name=version` argument 之前，因此 option-shaped row 会被双重拒绝，而不能重新解释成 APT flag。
在解析 active instruction 前，受限 Docker parser 会建模 BuildKit 对 UTF-8 BOM 与首行 shebang 的移除；
这两种 preamble 本身均被禁止。Hash/C-style directive marker 必须从 byte zero 开始；只有移除 marker
以后，detector 才会精确裁剪 Go `unicode.IsSpace` 的 Unicode White_Space 集合，并且刻意不使用 Python
范围更宽的 `str.isspace()` control。随后，无论 `syntax` frontend 使用这些 comment 形式还是 JSON，
也无论采用大小写、Unicode whitespace、tag、digest 或 shebang 隐藏变体，都会被拒绝。Marker 前带
space/tab 时仍是 non-active 普通 comment；普通 comment 仍会关闭传统 Docker directive phase，
canonical backslash `escape` directive 仍是唯一允许的 parser directive。
Frontend detection 与 active logical-instruction parsing 会消费同一份 Go `bufio.ScanLines` 等价物理行：
只有 LF 分隔 token，每个 token 只移除一个尾 CR。CR-only、VT/FF、FS/GS/RS、NEL 与 Unicode
line/paragraph separator 都保留在前一 token 内，不能暴露隐藏 instruction。Canonical LF、CRLF、
continuation 与 terminal CR 保持既有文档化行为。

## Integration 测试分片

对常规 published-image 路径中的非文档变更，`integration-plan` 会启用测试来配置被 checkout 的
commit，并把 `ctest --show-only=json-v1` 解析为非权威预检。CMake 默认的
`gtest_discover_tests` 模式要等 target 构建后才发现 GoogleTest case，因此配置期 CTest 状态可能
只包含未带标签的 `*_NOT_BUILT` 占位项，且没有任何带标签 entry。预检会允许空 label selection，
但仍拒绝 malformed JSON、重复 test 或 property、非法 label，以及任何 disabled 或 commandless
的带标签 entry。其 preview matrix 只作为诊断 artifact 保留，绝不会暴露为 workflow job output。

随后，`build-integrity-default` 会构建完整 default tree 并再次执行相同 JSON 查询，但它明确不拥有
routing authority。它把未经解释的构建后 CTest JSON 与生成的 profile/role 文件装入严格 raw
envelope 并上传，不暴露任何 consumer matrix。Candidate CMake 运行后，该 producer 不会 import
或执行 candidate checkout 中的 route parser 或 routing lock。独立的 `build-smoke-control` job 会把
精确受保护 `workflow_commit` checkout 到 fresh sparse control tree，把 raw envelope 下载到互不重叠
的 untrusted-input tree，并把 candidate source checkout 到第三个不执行代码的 tree。只有受保护 parser
负责校验 `ctestInfo` 版本、完整测试名唯一性、property/label 形状、enabled 状态、可执行 command、
matrix 大小、profile identity 与 routing lock；它会输出四个紧凑 canonical matrix 和一个 route
digest。Artifact key 由有界 ASCII slug 与测试名 SHA-256 digest 派生，精确测试名保留为 JSON matrix
value；排序稳定，workflow 不维护测试名清单。一个聚焦的真实 CMake fixture 会在 configure 时改写
candidate-owned route helper 与 lock，再证明 fresh protected control 仍是权威；raw entry 缺失、重复、
被重新标记或未声明时，会在 matrix output 或 attestation 前失败。

在 targeted artifact attestation 前，`verify-targeted-artifacts` 会把精确 candidate 再次 fresh checkout 到一份不执行代码的 data root。该 root 与精确 protected `workflow_commit` control checkout、raw inventory、control manifest、targeted artifact 和 restored CTest root 互不重叠。只能执行 control checkout 中的 Python。Verifier 会把 control `HEAD` 绑定到 `workflow_commit`、把 candidate-data `HEAD` 绑定到 `candidate_commit`，并在使用后重新检查两个 directory object。随后，它把精确 raw bundle 重新绑定到 route-control digest，并要求 raw CTest JSON、两份 targeted control/runtime closure 与 fresh 恢复后 `ctest --show-only=json-v1` 查询得到同一个普通 test set（只排除精确 `build-smoke`）。Reduction、addition、relabeling、identity/digest mismatch、link、overlap 或 path/commit drift 都会在 attestation 与 readiness 前失败。Container 与 host job 可能使用不同的绝对 workspace root，因此 discovery 在 job-owned 副本内执行，且只能在 CTest control file 与 cache 中替换 completion stamp 记录的精确 producer build-root token。经过验证的 archive、manifest 与 closure 绝不会被改写。

当前带标签的 inventory 为：

- `DependencyDisabledInstallSmoke`
- `ImageArtifactCodecDependencyDisabledBuild`
- `IpcDisabledInstallSmoke`
- `OpenExrDeepProviderOptionOffSmoke`
- `OpenCvOperationProviderDisabledBuild`
- `PhotospiderdInstallLayoutAbsoluteBindirSmoke`
- `PhotospiderdInstallLayoutAbsoluteLibdirSmoke`
- `PhotospiderdInstallLayoutNestedRelativeSmoke`
- `PublicHeaderSelfContainment`
- `StaticProductConsumerSmoke`

五个默认 dependency/configuration driver、三个 daemon install-layout case 与 static-product consumer
会创建或校验隔离的 nested build profile；public-header self-containment 会调用专用 compile
target。它们属于长期 product、package、configuration 与 compile 边界，不是 migration 或
source-layout 检查。
`OpenCvOperationProviderBuildSmokeSafety` 继续作为 OpenCV nested-build driver 的普通完整 CTest
safety regression：其 Python unittest 会在进程内验证 cleanup guard 与 cache-layout helper，
还会通过 production manifest generator 配置一个无需 compiler 的 `project(... NONE)` fixture；
它不会启动 child build、install、compile target 或生成的 executable。
因此，默认 profile 包含十个带 `build-smoke` 标签的 entry。当
`PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER=ON` 时，条件式
`OpenExrDeepProviderInstallConsumerSmoke` 会作为第十一个 entry 加入；它不属于默认 inventory。

两项 nested-profile inventory 无需在 workflow 中维护数量，也仍保持精确。Static-product producer
会导出已配置的 CMake public-header install allowlist，consumer 要求已安装 include tree 与这些相对
路径完全相等。其 CMake writer 会在序列化前拒绝反斜杠与可表达的 ASCII control；parser 会独立
拒绝包括 NUL 在内的全部 ASCII C0 control 以及 DEL，要求精确 canonical POSIX install path，
同时保留普通空格。Provider-disabled producer 会通过只包含一个精确 header 与严格双字段 data
line 的 TSV，导出 active
`gtest_discover_tests` target 及其配置专属 executable 路径；后续 comment、空行、control、额外
字段、非法或重复 target name 与相对路径都会 fail closed，绝对 POSIX path、Windows drive path
与 Windows UNC path 仍然有效。Focused build 完成后，driver 要求恰好只有那些不存在 executable
file 的已注册 target 表现为不带 label 的 `*_NOT_BUILT` 占位项。两项检查都会拒绝缺失和额外
entry，因此新增 allowlisted header 或已注册 GoogleTest target 时，expectation 会通过其权威
CMake declaration 变化，而不是依赖 Python 数字或针对未来名称的特判。

`build-integrity-default` 只配置一次，在同一 build tree 中先运行
`build_required_targets`，再运行 `build_all`，绝不为了表面并行而把两个阶段拆到不同 runner。
它不再向所有消费者发送原先 2.416 GB 的完整 build tree，而会导出四种绑定身份的角色制品：

- `ci-control-default` 只包含普通 fresh-build smoke 所需的精确递归 CTest control graph、producer
  cache/stamp 与 generated inventory；不包含 producer object 或普通测试 executable。
- `ci-runtime-default` 在 control role 上增加完整的构建后普通 CTest 闭包。Producer 只排除精确
  `build-smoke` label，记录每个 command/property，不依赖文件名地递归跟随全部 CTest include，并从
  每项测试的有效 working directory 解析相对 command、argument、`REQUIRED_FILES`、environment 与
  environment-modification 路径。Working directory 只作为解析 base，不会被整体复制。随后选择被
  引用的 executable、build-tree data、shared library、plugin tree 与 trust material。
  恢复后 consumer 会重新运行 JSON discovery，并拒绝 `_NOT_BUILT`、inventory 变化或任何缺失的
  include/executable/data/runtime input。
  Versioned DSO alias 只有在指向 producer tree 内的 regular file 时才会被接受，并会物化为同字节
  regular member，因此 archive 本身不含 link。
- `ci-installed-package-default` 只包含 `ci/installed/**`、
  `ci/producer/CMakeCache.txt` 与
  `ci/producer/generated/ci_inventory/installable_public_headers.txt`。Static library 只允许位于
  installed prefix 中；CMakeFiles、CTest state、build stamp、object、dependency file 与无关 build
  library 均被禁止。
  Installed DSO alias 采用相同的 prefix 内校验与 regular-member 物化；dangling、escape、directory
  或 special target 都会失败。
- `ci-openexr-metadata-default` 精确只包含 `ci/producer/CMakeCache.txt`。专用 runner 使用原注册中
  缓存的 Python、CMake、CTest、symbol tool、configuration 与架构传播输入，直接执行 source-tree
  `OpenExrDeepProviderOptionOffSmoke` driver。单配置 producer 中唯一、非空且不含控制字符的
  `CMAKE_BUILD_TYPE` 会提供 `--config`；caller 环境不能覆盖它，multi-config cache 会被拒绝。该 role
  不下载 CTest graph、stamp、generated inventory、object 或 product library。

每种 role 都有 canonical member/digest/size manifest，并在提取前校验。Targeted manifest 会从一个
拒绝跟随链接的 regular-file snapshot 复制；attestation 得到的精确 SHA-256 会传入 Python 的 retained
snapshot 校验，因此 pathname 替换不能切换已验证 identity。`full-ctest` 只消费
`ci-runtime-default`，排除精确 label，使用 `--parallel ${CI_JOBS}`，同时保留完整失败日志与 JUnit
output。Scripted CLI、propagation、plugin 与 execution-repeat 只在确需 built runtime 时消费同一个
runtime role。

受保护 routing lock 是带版本的临时过渡权威：`PublicHeaderSelfContainment` 会进入下游
producer-designated `ctest-control` matrix job，不在 candidate producer 中执行；
`StaticProductConsumerSmoke` 路由到 `installed-package` role；
`OpenExrDeepProviderOptionOffSmoke` 路由到精确 `openexr-metadata` role；其余发现到的 smoke 全部进入
`ctest-control`。Control、installed、OpenEXR 与 producer-designated output 两两不重叠，并穷尽构建后
CTest inventory。每个 consumer、targeted-artifact verifier 与 attestation、readiness gate 和 suite
gate 都依赖 successful protected control job 及其 route digest，绝不信任 producer 输出的 matrix
text。Dedicated installed-package job 与 producer 使用相同 digest-qualified image；
其 package-input mode 会跳过 producer build/install，但仍完整执行 daemon help、install/export/
symbol 检查、全部正向 consumer compile/link/run probe 和全部负向 component 检查。Prefix、metadata
与 job-owned work 两两隔离，只有 work directory 会被删除。执行前后，job 都会按已校验 manifest
重新测量精确 member set、byte size、SHA-256 digest 与 executable attribute；新增、删除、改写或
mode 漂移都会失败。Evidence upload 只包含 consumer/verifier log 与可用的 attestation record，
不会再次上传恢复后的 prefix 或 producer metadata tree。

普通 `build-smoke` matrix 保留 `fail-fast: false`。只有 producer 被跳过时，字面量空 include
fallback 才用于保持 `fromJSON` 可解析；成功 producer 不能发布空或不完整 partition。每个 item
仍拥有独立的 30 分钟 workflow timeout，并保留 CTest 注册自身的 timeout 与 `RUN_SERIAL` 语义。

Runner 会在执行前立即重新查询 CTest JSON，并要求选定的精确名称仍然唯一、enabled、可执行且
带标签。完成该标签校验后，执行只使用校验过的 CTest 数字索引；测试名不会插入 shell command
或 regular expression。于是 `IpcDisabledInstallSmoke` 会通过长期维护的 CTest 注册执行，
自行创建 clean `PHOTOSPIDER_BUILD_IPC=OFF` producer，不再依赖 workflow 中单独硬编码的 profile。

Published-image 与 image-input-changing 路径会调用同一个 typed reusable suite，并传入独立期望的
digest-qualified `image_ref`、candidate、profile、manifest、source 与 workflow identity。在任何
candidate checkout、code 或 container 启动前，host preflight 会在精确 caller `workflow_commit`
checkout `github.repository`，把该值绑定到实际 `github.workflow_sha`，再运行受保护 image verifier。
Verifier 会独立把请求的 digest/reference、GHCR attestation signer/source、OCI revision 与 canonical
manifest digest 和全部 caller field 交叉校验。GitHub 允许 called workflow 保持或降低 caller 的
`GITHUB_TOKEN` 权限，但绝不允许提升。Reusable call 的权限兼容性会先于后续 job-level `if` 导致的
skip 完成校验，因此 shared workflow 不声明 workflow-wide 权限。每个会 checkout 或执行 candidate
code、运行 candidate container 或聚合结果的 job，都会声明精确的只读或空 job-level permission
map。唯一例外是 `attest-targeted-artifacts`：它不声明本地 permission map，只在
`publish_reusable_attestations` 为 true 且 targeted verification 已成功时运行，并且只从两个可信
push caller 之一继承所需的 `artifact-metadata`、`attestations` 与 `id-token` write 权限。Pull-request
caller 只传入 read 权限与 `false`；其余所有 job 都无法继承可信 caller 的 write 权限。

可信镜像变更 push 只在 event-scoped 临时 SHA tag 下构建一次，attest 该精确 digest，并在不重复调用
`integration_suite.sh` 的前提下扇出。只有全部 suite job 成功后，promotion 才能在不重建的情况下
复制同一 digest。Promotion 会先建立 immutable `sha-<full-commit>` reference，随后 current run 才能
更新其 branch tag；只有可信且成功的 `main` push 可以推进 `latest`。`main` 保持精确的
`branch-main` tag。`CI/**` branch tag 使用
`branch-<bounded-readable-slug>-<sha256>`：完整 64 位十六进制 SHA-256 后缀会对经过校验的完整分支名
字节做 hash，且不会附加换行。Git 自身负责校验 `refs/heads/<branch>`；slug 不是第二套 ref
allowlist。它保留 Docker-safe ASCII，把其余所有字节（包括 Git 允许的标点或 UTF-8）确定性压缩为
分隔符，并且只截断该展示部分，因此 tag 满足 Docker 语法、最长 128 字符，并且不会把
`CI/a-b` 与 `CI/a/b` 映射成同一身份。Pull-request 路径保持只读。

只有 promotion job 持有一个由每个 ref 共享、按 repository/CI-image namespace 分组的 concurrency
group；它会保留排队 writer、设置 `cancel-in-progress: false`、串行化 workflow-owned SHA 与 mutable
writer，绝不会因纯文档 run 到达而取消整个 Integration workflow 或 candidate。Queue 顺序不受信任。
在写入任何 registry state 前，受保护 manifest helper 会围绕一个
隔离 worktree measurement 两次 fetch 精确 live branch，要求已测试 candidate 是其祖先，并复用
canonical image-input path lock、source-commit resolver 与 manifest digest。较新的纯文档 descendant
若保持相同 source/manifest，仍可晋升。若存在更晚 image-input identity，则该 run 报告
`superseded`。Force-push、unknown ancestry、manifest failure 或 measurement 期间 ref drift 都会以
registry 零写入失败。

Freshness 成功后，promotion 会严格解析 SHA tag 唯一的顶层 manifest digest。精确 digest 会直接复用，
且不调用 create。只有 locked Buildx 返回精确的 not-found status 与 diagnostic 时，才会单独创建不
存在的 SHA；随后必须在写入任何 mutable tag 前重新校验 creation metadata 与立即执行的 registry
resolution。digest 冲突、缺失或有歧义，以及认证、网络或未知 inspect failure 都会以零写入失败。
`superseded` run 可以创建或复用该 SHA，但不能触碰 branch 或 `latest`。全局 lease 会闭合
workflow-owned 的跨 ref check/create race；GHCR 不为 out-of-band nonworkflow writer 提供 tag
compare-and-swap 保证，因此该边界仍需显式治理。稳定 gate 会分别报告 `promoted` 与 `superseded`，
不会把跳过写入呈现为晋升成功。

用户观察到的 2.416 GB archive、4 分 19 秒压缩以及 13 个消费者名义传输超过
31 GB 仍是远端比较基线；在 exact-head 远端运行完成量化前，不声称已经实现显著下降或 30--45
分钟镜像变更目标。

CMake 3.16 是项目兼容性下限，不是每个 pull request 都固定运行的 workflow 版本。维护中的
构建逻辑会保护晚于该下限引入的 policy，当前 integration 则在受支持 CI toolchain 上执行 fresh
static package consumer。只有 compatibility-sensitive change 或 release check 确有需要时，才补充
针对性的原生旧版本 producer/install/consumer 运行；常规 integration workflow 不会通过专用最低
版本 job 锁定 Ubuntu 或 CMake。

## 运行时架构能力过渡

在 policy/execution 架构经过受保护的 `CI/**` 文件完成迁移期间，可信 CI 只支持两种完整的运行时
验证契约。配置完成后，每个运行时敏感脚本都会捕获
`cmake --build <build-dir> --target help`，并按精确 target 名匹配。旧 scheduler 契约要求
`test_scheduler`、`test_scheduler_plugin_loader` 和
`destroy_count_scheduler_plugin` 全部存在，且不存在 policy/execution 标记；新契约要求
`test_policy_execution`、`test_policy_registry` 和 `test_policy_plugin` 全部存在，且不存在旧标记。
清单不完整、混合或完全没有标记时，脚本会在 build 或 runtime 命令前失败；分支名和 commit identity
绝不参与契约选择。

`build-integrity-default` 校验架构中性的 `photospider_kernel`、`graph_cli`、
`test_propagation` 与 operation-plugin 生命周期 target，随后仍构建完整 tree。Full CTest 继续作为
普通测试的权威入口，并且只排除精确的 `build-smoke` label。Runtime capability 选择只通过选取适用的
旧 target 或 policy/execution target 来保持覆盖语义，并不保留先前的 orchestration topology。
Fresh protected control 会输出下游 `build_smoke_matrix`（`ctest-control`）、
`openexr_build_smoke_matrix`（`openexr-metadata`）、
`dedicated_build_smoke_matrix`（`installed-package`）和
`producer_build_smoke_matrix`（`ctest-control`）四个 workflow output，以及一个 canonical route
digest。特别是 `StaticProductConsumerSmoke` 会通过专用 installed-prefix
package-input 边界运行，在不重建或重新安装 producer 的前提下保留全部 consumer 检查。

运行时敏感分片会选择对应行为，但不会引入产品兼容层：

- 脚本式 CLI 配置只会输出旧 `scheduler_*` key，或输出新的 `policy_*` 与 `execution_*` key；
  两者不会混合，也不会互相翻译。
- Plugin loading 会校验 operation surface，并选择 scheduler plugin 加载/列举，或 policy registry、
  policy/execution 测试、policy plugin 加载/列举及 execution route 列举。
- `execution-repeat` 在旧契约下重复确定性的 scheduler 测试，在新契约下重复 policy registry、
  policy/execution、compute-run routing 和 resource-admission 测试。
- ASan 与 TSan 保留共享的 compute/propagation 检查，并选择对应的旧 scheduler 或新
  policy/execution focused tests。

在 candidate-owned matrix 取代 current-main sanitizer fallback 之前，该 fallback 只使用一个带终止记录的 NUL-framed v1 invocation stream。Target、可能为空的 GoogleTest filter 与 trust flag 在 Bash 3.2 和 Bash 5 中仍是三个独立 field；禁止 whitespace splitting、shell evaluation 与 legacy text decoder。Producer 会在发出任何 byte 前校验全部 record；shell 把输出捕获到唯一 fresh transient file、显式检查 producer status，再通过固定 descriptor 解析。NUL read 失败且残留 partial byte、terminal 缺失或重复、完整或不完整 tail、target 重复或 producer 非零，都会在 configure/build/test 前失败且不写 success evidence。Shell 只把完整解码后的 stream 保存为诊断 evidence，再由 `run_gtest_checked` 在执行前证明每个空或非空 selection 都非零。

Linux 与 Darwin 都会把 ASan、TSan 和有界 fuzz 调度为不同的 profile result。在 Darwin 上，
`sanitizer-asan-darwin`、`sanitizer-tsan-darwin` 与 `fuzz-codecs-darwin` 是三个独立的
`macos-15` job；每个 job 只依赖 `integration-plan`，下载同一份受保护 profile inventory，并拥有
独立 timeout 与 diagnostic artifact。任何 profile 都不会等待 sibling，因此一个失败不会阻止另外
两个 job 被调度；shared suite gate 仍要求三个 conclusion 全部成功。
受保护 lock verifier 会比较每个 Darwin job 的完整 mapping，包括仅有的五个有序 step 与每个允许
field。Suite gate 会 checkout 精确的受保护 `workflow_commit`，并且只调用带 version/hash 绑定的
`integration_suite_gate.py`；它的完整 `needs`、result environment、checkout、permissions、output
与 helper 调用均为精确 mapping。Helper 会拒绝 failed、skipped、missing 或 unknown required
result，验证 publishing route 的 attestation 为 `success`、read-only route 为 `skipped`，校验
digest，并且仅在全部检查通过后写 output。未知 step、`continue-on-error`、额外 field/statement、
注释、no-op、early exit 或 sibling dependency 均不能满足维护中的 routing contract。
Helper source 还必须同时匹配三个独立 identity：verifier-owned 精确 byte SHA-256、受保护 JSON
helper lock 与 retained regular-file measurement。Measurement 使用单一带 `O_NOFOLLOW`/`O_CLOEXEC`
的 retained descriptor，在不阻塞的情况下拒绝 special file，在读取前后复核 pathname 与 descriptor
metadata，并要求对同一 descriptor 的两次读取一致。因此 final symlink swap 即使指向 same-inode
hardlink 也会失败，in-place mutation 同样不能改变获授权 helper byte。它不依赖随 Python 版本变化的
`ast.dump()` 或 `ast.unparse()`。Behavior test 仍会执行每个 result 与 attestation branch；security
contract 直接启动的 Python child 使用测试进程自己的 `sys.executable`。
Canonical manifest input measurement 会把同一 retained-descriptor boundary 应用于每个
self-declared input，而不仅是两个 protected helper。Self-including lock、helper hash、action builder
identity、runner rollout authority、逐 input digest 与 descriptor size 都来自唯一 path-to-record
measurement map。

这是一个受保护的两阶段过渡。可信 `CI/**` 变更先进入 `main`，并在那里验证旧契约；架构 pull
request 随后纳入该可信 commit，并删除自身独立的受保护路径差异，其完整标记集合会选择
policy/execution 契约。当 `main` 与所有维护分支都只使用 policy/execution 后，后续可信 CI 清理
应删除旧 profile 与能力切换。

## Plugin-Manager 测试套件名称过渡

在 plugin manager 迁移到纯 C operation ABI 期间，`plugin_load_test.sh` 使用显式正向
过滤器 `PluginManagerLifecycleTest.*:PluginManagerPureCAbiTest.*`。GoogleTest 会把冒号
分隔的 pattern 视为并集；`run_gtest_checked` 会把同一过滤器同时用于发现和执行，并拒绝
空 selection。因此，无论 `main` 上的旧测试套件、迁移分支上已改名的测试套件，还是两者
并存，都会被执行，而且不需要使用可能选中无关测试的宽泛 glob。测试套件绝不由分支名或
commit identity 选择；构建后的测试清单才是权威来源。

这只是一层测试选择兼容，不会增加产品或 ABI 兼容层，也不表示 `main` 已经完成纯 C ABI
迁移。只有当 `main` 与所有维护分支都只公开 `PluginManagerPureCAbiTest.*`，并且所有受支持
构建清单中都不再出现旧测试套件后，后续可信 `CI/**` 清理才应把过滤器收窄到新测试套件。

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

- `ci/scripts/healthcheck.sh`：建立 NUL 分隔的 changed-path artifact，运行 `git diff --check`、长期 change-classification、build-smoke inventory、runtime-capability 与 CI-routing 回归，并对每个未删除的 changed C++ 路径运行 `clang-format --dry-run --Werror` 与 `cpplint`；清单失败时会在输出无 C++ 摘要前终止。
- `ci/scripts/change_classification.sh`：把 event 的精确 revision 分类为纯文档或完整 integration，记录所有改动路径与非文档路径，并在 Git 状态不确定时 fail closed。
- `ci/scripts/change_classification_test.sh`：覆盖文档、源码、混合、type change、workflow、重命名、删除、重复 `CI/**` push、pull-request merge-base、branch 或 revision 缺失、全零/不可达 revision、手动触发、空 diff 与浅克隆场景，验证长期路由契约。
- `ci/scripts/ci_routing_test.sh`：对两份生产 `protected-ci-paths.if` 表达式做空白归一化并锁定精确源码，再抽取并执行真实 stable-gate、checkout 前 fork-rejection 与 protected-path shell block。它还会锁定允许空集合的配置期预检、只产出 raw input 的 producer、fresh exact-commit protected-control checkout、互不重叠的 control/candidate/download tree、对空 output 安全的 `fromJSON` matrix、逐项 artifact/name binding、full-CTest label exclusion、精确 runner input，以及四个两两不重叠的下游 build-smoke 分区：普通 `ctest-control`、OpenEXR metadata、专用 installed-package static 与 producer-designated `ctest-control` consumer。它会证明 candidate CMake 运行后 producer 无法调用 route helper 或 lock，验证 role-specific control/runtime/OpenEXR/installed artifact 的生产、绑定 route digest 的 attestation 与消费顺序，要求完整 shared suite gate，拒绝串行 `integration_suite.sh` fallback，并保留架构中性 `execution-repeat` job 的环境变量、artifact 和最终 gate 路由。隔离 Git fixture 会证明两份生产门禁都拒绝含换行的 `ci/**` 路径、安全记录该路径，并在 producer 或 reader 失败时 fail closed。一个 job/step-scoped production 断言会抽取 published-image 中两个精确的 history-fetch step，并要求各自拥有顶层 `shell: bash`，因此其他 job 或相邻 step 的元数据无法满足该契约。另一个 job/step-scoped 断言要求恰好一个使用 `shell: bash` 的 `Trust checked-out workspace` step；它唯一可执行的内容必须是启用 strict mode、把精确 `$GITHUB_WORKSPACE` 加入 global `safe.directory`，以及校验 `HEAD^{commit}`。其他 job 或相邻 step 中的条目、任何额外或通配的 `safe.directory`，以及晚于任一 fetch 或 `healthcheck.sh` 的位置都无法满足断言。抽取出的 production trust block 会在隔离 HOME 与 Git 仓库中运行，并要求所得 global 配置只包含该仓库的精确路径。Job-scoped 断言还分别锁定 published-image 与 local-image 的 pull-request 精确 base fetch、`CI/**` main fetch/校验、三路 `CI_BASE_REF` route 及执行顺序。测试会执行两份抽取出的 production main-fetch block；隔离历史会证明累计 `origin/main` 范围保留较早的未格式化 C++ 路径，而 event-before 范围只包含较晚的文档路径。Detector fixture 继续覆盖精确/累计 base、空比较、含换行路径及 changed-path 失败传播。这些本机源码与 shell 检查明确不声称执行 GitHub expression evaluator、复现跨 UID dubious ownership 或模拟托管 container runner。
- `ci/scripts/ci_image_install.sh`：执行唯一的 Docker image 安装 transaction。它的 version/完整文件 SHA-256、verifier-owned active-statement identity、单一 entrypoint 调用、snapshot/APT/Pip/GitHub-CLI 顺序、下载 authority 与 hash-before-extract boundary 都受保护；alternate APT path、额外 downloader、pipe-to-shell、跳过 hash 或 early success 都会被拒绝。
- `ci/scripts/integration_suite_gate.py`：校验 shared DAG 每项精确 conclusion、publish/attestation mode 与 image digest，随后安全追加唯一 validated digest output。直接行为回归会让每个 required job 分别使用 failed/skipped/unknown conclusion，并覆盖两种合法 attestation mode。
- `ci/scripts/runtime_capability_test.sh`：覆盖精确 Make/Ninja target 解析、两种完整契约、不完整/混合/缺失清单的 fail-closed 行为、required-target 校验及互斥 CLI 配置输出。它还证明 direct-consumer trust export 由精确的可选 `test_plugin_trust_bundle` capability 控制，而不是由更宽泛的 policy/execution profile 控制：pre-trust 与 legacy inventory 保持 no-op，缺失或 malformed inventory 以及不完整/非 regular material 均 fail closed，完整的 trust-enabled tuple 则以 canonical path 覆盖 inherited value。
- `ci/scripts/ci_image_changed.sh`：把精确 base/head 比较委托给 canonical manifest helper；后者严格验证 self-including `input_paths` lock 的必需 revision，把其并集与 NUL 分隔且不带 status 过滤的 diff 比较，对经过证明的 no-lock-base/strict-head 首次引入路由 `true`，并在 head malformed、bootstrap 未证明、后续 lock 缺失或 Git 失败时不输出路由。
- `ci/scripts/build_smoke_inventory.py`：严格解析 CTest JSON v1，输出确定性 matrix 与 NUL 分隔精确名称，并在基于索引执行前重新校验一个 matrix selection。严格构建后模式拒绝空 selection；只有显式 preflight 模式允许为空。Focused regression 会覆盖 malformed JSON/schema、重复名称/property/label value、非法或缺失 label、disabled/commandless entry、严格空 selection、确定性排序、JSON round trip、安全 artifact key、敌意测试名字符、在执行前停止的 absent/disabled/commandless runner selection，以及真实配置期占位到构建后发现过程。
- `ci/scripts/integration_plan.sh`：配置一个启用测试的小型 build tree，并校验允许空集合、非权威的配置期 inventory preview；它不会输出 workflow matrix。
- `ci/scripts/build_integrity.sh`：检测一种完整运行时契约，在同一 tree 中执行 required-target 与完整 build，捕获未经解释的构建后 CTest JSON，写入普通 CTest 闭包并安装 fresh package prefix。Candidate configure 后它不会 import 或执行 route parser/lock，也不输出 consumer matrix；workflow 只把 CTest 与生成的 profile/role byte 打包给独立 protected control job。
- `ci/scripts/build_smoke_route.py`：只从精确 protected control checkout 运行。它校验互不重叠的 candidate/control/raw 边界、两个 checkout commit、raw envelope 与生成的 profile identity，再使用受保护 parser 和 routing lock 输出四个穷尽的 downstream matrix 与一个 canonical route digest；其 verifier 会在 artifact attestation 前把下载的精确 raw byte 重新绑定到该 control digest。
- `ci/scripts/ctest_runtime_closure.py`：派生递归的构建后普通 CTest control/runtime 闭包，为 retained raw/restored JSON 暴露严格普通名称解析，并在执行前重新校验恢复后的 runtime inventory、executable、dynamic library、plugin、trust input 与 build-tree data。
- `ci/scripts/ctest_full.sh`：复用 runtime role，在排除精确 `build-smoke` label 后，以受控 `${CI_JOBS}` 并行度运行普通 CTest，同时保留失败输出与 JUnit 证据。
- `ci/scripts/build_smoke_test.sh`：从 `ci-control-default` 重新校验并运行一个精确 default-role CTest 名称。
- `ci/scripts/openexr_smoke_test.sh`：从已校验、只含 cache 的 metadata role 运行精确 default OpenEXR option-off source-tree smoke。
- `ci/scripts/static_product_consumer_test.sh`：在不重建或重新安装 producer 的前提下，于完整 package consumer 执行前后重新测量精确 installed-package content。
- `ci/scripts/targeted_artifact_consume.sh` 与 `ci/scripts/reusable_build.py`：分别以 candidate source digest 与 reusable-workflow signer digest 校验 archive/manifest attestation，再校验并原子恢复一个精确 role。受保护的 attestation 前 verifier 还会分离 control-code/candidate-data root，并交叉绑定 raw、两份 archived CTest closure 与 restored 普通 inventory。
- `ci/scripts/graph_cli_script_test.sh`：使用上述执行前 Graph 文档能力标记，运行相互隔离的正路径、显式来源缺失和无效 target REPL 检查。
- `ci/scripts/propagation_script_test.sh`：构建 `test_propagation`，并对线性和复杂 propagation 图运行 `tiles all`。
- `ci/scripts/plugin_load_test.sh`：检查 operation plugin，并选择 scheduler plugin 加载/列举，或 policy plugin、registry、policy/execution 与 CLI route 检查。
- `ci/scripts/execution_repeat_test.sh`：重复运行当前运行时契约对应的确定性 scheduler 或 policy/execution 行为测试。
- `ci/scripts/sanitizer_test.sh`：先消费 profile input 前生成的唯一 retained runner identity，再在独立 build 目录运行共享且按能力选择的聚焦 ASan 或 TSan 测试；其 temporary fallback 只通过带终止记录的 NUL-framed v1 protocol 传输 target/empty-filter/trust record，并记录解码后的 evidence。

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
bash ci/scripts/runtime_capability_test.sh
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
  CI_ARTIFACT_ROLE=ctest-control \
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
  CI_ARTIFACT_DIR=CI-results/execution-repeat \
  bash ci/scripts/execution_repeat_test.sh
# Security runner 只能在 approved hosted runner 上复现。
runner_identity="${RUNNER_TEMP:?}/photospider-security-runner-$$.json"
python3 ci/scripts/ci_runner_verify.py --platform Linux \
  --runner-label ubuntu-24.04 --output "$runner_identity"
CI_RUNNER_IDENTITY_FILE="$runner_identity" SANITIZER=asan \
  CI_ARTIFACT_DIR=CI-results/sanitizer-asan \
  bash ci/scripts/sanitizer_test.sh
```

Sanitizer/fuzz 命令有意不提供未经验证的 workstation fallback。`ci_runner_verify.py` 要求 hosted
`ImageOS`/`ImageVersion`，只创建一次新的 retained file，后续每个 platform preparation step 都消费同一路径。

可以把 `SMOKE_TEST_NAME` 替换成 protected `build-smoke-control` job output 中的任一精确 build-smoke
名称（本机诊断时也可使用同一构建后 CTest inventory）；runner 会拒绝 absent、duplicate、disabled、
commandless 或未带标签的选择。要从 configured tree 直接运行全部带标签
smoke，可使用
`ctest --test-dir build/ci-default -L '^build-smoke$' --output-on-failure`。Shared reusable DAG
不存在等价的本机串行命令；本机诊断应使用上文聚焦的 role command，而 build-once digest fan-out、
attestation、聚合与 same-digest promotion 属于 GitHub Actions 门禁。

Docker 复现会把本地 layer solver 与受保护 publication provenance 分开。以下本地命令有意不具备发布资格：

```bash
# Local layer-solver reproduction only; never publish or attest this image.
local_ci_manifest_sentinel=0000000000000000000000000000000000000000000000000000000000000000
local_ci_source_sentinel=0000000000000000000000000000000000000000
docker build --no-cache -t photospider-ci:local -f Dockerfile.ci \
  --build-arg CI_IMAGE_INPUT_MANIFEST_SHA256="$local_ci_manifest_sentinel" \
  --build-arg CI_IMAGE_SOURCE_COMMIT="$local_ci_source_sentinel" .
docker run --rm -v "$PWD:/workspace" -w /workspace photospider-ci:local \
  bash ci/scripts/build_integrity.sh
```

只有 approved `ubuntu-24.04` GitHub-hosted builder 才能构造用于发布的 manifest。在该 protected job 中，
真实 retained builder identity 与 source equality 必须按以下方式取得：

```bash
# Approved Linux hosted runner only; this constructs publish provenance.
mkdir -p CI-results/hosted-ci-image
builder_runner_identity="${RUNNER_TEMP:?}/photospider-builder-runner-${GITHUB_RUN_ID:?}-${GITHUB_RUN_ATTEMPT:?}.json"
python3 ci/scripts/ci_runner_verify.py --platform Linux \
  --runner-label ubuntu-24.04 --output "$builder_runner_identity"
ci_image_source_commit=$(python3 ci/scripts/ci_image_manifest.py \
  publish-source-commit --workflow-commit "${GITHUB_SHA:?}")
ci_image_manifest_digest=$(python3 ci/scripts/ci_image_manifest.py create \
  --source-commit "$ci_image_source_commit" \
  --repository "${GITHUB_REPOSITORY:?}" \
  --builder-runner-identity "$builder_runner_identity" \
  --output CI-results/hosted-ci-image/ci-image-input-v1.json)
```

上面两个不同的全零值只具备 installer 要求的 lowercase 64-hex manifest-digest 与 40-hex Git-commit
形状。它们仅用于执行 Docker layer 与 snapshot solver，是 null sentinel，而不是 canonical manifest、
Git object、OCI provenance 或发布授权；所得本地镜像绝不能 push、promote 或 attest。受保护 producer
会把第二个 block 的真实 manifest/source value 作为 immutable Buildx input，并继续应用全部 workflow、
attestation 与 digest check。

其余 Docker build argument 保持 `ci/locks/ci-image-lock.json` 中受保护的 immutable default。本地
镜像源覆盖不属于维护中的镜像契约。OpenSSL/CA 离线 bootstrap URL 与 SHA-256 值锁定在该文件中；
两个精确版本也同时出现在 Ubuntu package lock 中；受保护 Deb822 template 会在 APT 运行前，把全部
base archive/security/ports source 替换为精确 snapshot URI。同一 source 同时服务原生 amd64 与
arm64，所有 APT update/install 都保持在其中。build 本身就是 dependency-solver 回归：任何不可用的
直接或传递版本都会在 image 发布前失败。Pip 则单独使用带 hash lock 的 requirements 文件。
`Dockerfile.ci` 只有一条精确 active instruction stream，并且只调用
`bash /tmp/ci-image-install.sh`；该 helper 来自 canonical manifest input，且在
`ci-image-lock.json` 中按 role、version 与完整文件 SHA-256 绑定。独立的 verifier-owned
active-statement identity 及 network/install allowlist 会阻止 helper 与其 JSON hash 被同步修改后引入
`/usr/bin/apt-get`、额外下载、`curl | sh`、跳过 GitHub CLI checksum、未调用 entrypoint 或 early
success。唯一的非 APT 下载是精确 GitHub CLI release URL；其架构专用 hash 会在 extract/install
之前完成校验。

上述本地 Docker 命令复现的是维护中的 current-toolchain layer 与 build 路径，不带 publication
provenance，也不代表 CMake 3.16 本身已经
运行。若确实需要针对性旧版本证据，而本机没有原生兼容 executable，应记录该限制，不要用架构
模拟制造最低版本 PASS。

## 本地 artifact 下载

使用 personal overlay 脚本下载 GitHub Actions artifact：

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

脚本写入 `CI-results/`。该目录属于 personal overlay，不能提交到主 GitHub 仓库。
