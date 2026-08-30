# GitHub Actions CI

Photospider 有意只保留两个 GitHub Actions workflow：

- `.github/workflows/ci.yml` 处理 PR 与日常维护 push，包含一次 preset healthcheck、一次由
  ccache 加速的 legacy-full producer 构建、六个独立 build-smoke runner 和三个并行 CTest label 分片。
- `.github/workflows/build-ci-image.yml` 在 `main` 的 `Dockerfile.ci` 发生变更或维护者手工触发时发布 Linux CI 镜像。

仓库不再设置单独的 pull-request-target、sanitizer、scheduler-log、routing、evidence、provenance 或 aggregator workflow。本仓库按个人开发项目维护，因此 CI 围绕真正有用的构建与测试信号安排，而不是复刻 enterprise 审批或自授权证明机制。

## 日常 CI 流程

`.github/workflows/ci.yml` 在 PR 及向 `main`/`CI/**` push 时运行。每个 job 都使用
`ghcr.io/<owner>/<repo>/photospider-ci:latest`、递归 checkout submodule，并且只拥有仓库内容与
package 的读取权限。Job 形成一条带并行测试叶子的依赖链：

```text
healthcheck -> build -> build-smoke（6 个独立 matrix job）
                     -> unit
                     -> integration
                     -> verification
```

### Healthcheck

在执行项目检查前，Healthcheck 只把 `$GITHUB_WORKSPACE` 加入 job container
用户的全局 Git `safe.directory` 列表，并验证已 checkout 的 `HEAD` 是 commit。
这只是用于跨 checkout 所有权边界保证容器内 Git 可用的设置，不是授权、
protected-path 或 provenance 证明；它绝不使用通配符。

随后 Healthcheck 有意只执行以下低成本检查：

1. 对 exact checked-out commit 运行 `git diff --check`。
2. 显式检查 `ccache` executable 与版本，使未发布或陈旧的 CI 镜像在 producer 前失败。
3. Configure 并构建完整 dependency-neutral `kernel-dev` profile，然后运行其长期 tests。
4. Configure 并构建 `op-dev`。
5. Configure `legacy-full` 并构建 `public_header_self_containment`。
6. Configure `legacy-full-portable` 并构建同一个 public-header target。

这些 path 由 `CMakePresets.json` 持续维护。`kernel-dev`/`op-dev` 默认关闭 Job、CLI、optional
providers/plugins、OpenEXR 和 fuzzers；`legacy-full` 在 Darwin/Linux 显式开启历史
Job/product closure；`legacy-full-portable` 则在 Job 不受支持的 host 上保留完整 portable
closure。完整表格见
[拆仓后开发契约](../../development/zh/Post-Split-Development-Contract.zh.md)。Preset frontend
要求 CMake 3.21；直接配置仍保留项目级 CMake 3.16 最低版本。

它不会对变更路径分类、推断 docs-only 运行、比较 protected path、检查其他 ref、构建完整产品，也不会决定后续 job 是否存在。

### 一次 ccache 加速构建

Producer 不再恢复旧的 `build/ci` tree，而是从全新 binary directory 开始，并仅通过
`actions/cache/restore@v6` 恢复 `.ccache`。Compiler-cache key 包含显式 epoch、runner 操作系统与
架构、build type、`Dockerfile.ci` 镜像配方 hash，以及 workflow 的 `run_id` 与 `run_attempt`。
唯一 restore prefix 在 run identity 前结束，因此 producer 可以使用较早兼容 run 中最新的 cache。
Workflow 会同时报告 `cache-hit` 与实际 matched key；fallback 仍然有用，只是仅有 exact current-run
key 才会产生 `cache-hit == true`。

Cache 配置把 `CCACHE_DIR` 放在 workspace 内，使用 `CCACHE_COMPILERCHECK=content`
拒绝来自不同 compiler 的 entry，同时把本地 cache 限制为 2 GiB。CI 有意不设置
`CCACHE_BASEDIR`，并设置 `CCACHE_NOHASHDIR=true`。Producer 与 smoke job 在同一个绝对
`$GITHUB_WORKSPACE` 路径 checkout 相同源码，因此等价 compiler command 会保留一致的绝对源码
argument，而关闭 directory hashing 后，cache key 不再区分 outer 与 deeper nested 的 working
directory。若在此处设置 `CCACHE_BASEDIR`，ccache 会改为相对于每个 compiler process 的
working directory 重写路径；deeper nested build 的 working directory 不同，重写后的 argument 也会
不同，从而使这些 cache hit 失效。关闭 directory hashing 是仅限 CI 的明确取舍：ccache 返回的
`RelWithDebInfo` object，其 DWARF 可能保留 producer working directory。因此 cached object
绝不会被发布或视为 release/debug 交付物。Runtime 行为仍由测试验证，任何 cache miss 都会正常编译。

恢复后，producer 会清零 ccache statistics，通过显式 C 与 C++ ccache CMake launcher 进行 configure，
并显式把保留的 single-tenant Job 设为 `ON`，
只调用一次 Ninja，再打印得到的 hit/miss statistics。它把 `.ccache` 保存到新的 run-and-attempt key，
供后续 workflow 使用；随后把 hidden `.ccache` directory 封装进不压缩的 tar，并将这个单一文件上传一次，
作为保留一天的 `ccache-handoff` artifact。Tar 会跨 artifact ZIP 层保留 cache directory 内部结构与
mode。Actions Cache 只负责跨 run 优化；artifact 才是同一 run 内 producer 到 smoke 的必需传输。
Actions Cache miss 只表示 producer 合法地冷编译，不是正确性失败。

Key 有意只 hash configure 前后稳定的 `Dockerfile.ci`。Baseline run 已证明，宽泛的 workspace
`hashFiles('**/CMakeLists.txt')` 会把 configure 生成的 dependency CMake file 纳入 hash，使 restore 与
save 的 key 漂移，并导致全部下游 exact restore miss，因此该模式已删除。CI 不再通过 Actions Cache
保存完整 `build/ci` tree，也不把它上传为 artifact。全新构建后，producer 会另行打包轻量 CTest
runtime，并只提供给三个 primary-label job。Packager 会把 compiler-cache 内容，以及
`tests/image_artifact_codec_dependency_disabled` 和
`tests/optional_opencv_provider_disabled` 两个固定临时 root 作为 object-free archive invariant 排除。

### 独立 build-smoke runner

默认 CI 配置中的六个 build smoke 各自对应一个固定 matrix entry：

- `DependencyDisabledInstallSmoke`；
- `OpenExrDeepProviderOptionOffSmoke`；
- `StaticProductConsumerSmoke`；
- `ImageArtifactCodecDependencyDisabledBuild`；
- `OpenCvOperationProviderDisabledBuild`；
- `PublicHeaderSelfContainment`。

Matrix 使用 `fail-fast: false`，因此 producer 成功后，每个 entry 都会获得独立的 runner 和 container。
各 runner checkout 同一个 `github.sha`，把 `ccache-handoff` 下载到固定 artifact directory，再在
`$GITHUB_WORKSPACE` 解开 tar 以重建 `.ccache`。它会验证 cache directory 与 ccache 配置、清零本地
statistics，并保持恢复的 cache 为 read-only，因此一个 consumer 不会改变其他 consumer 使用的
producer snapshot。Artifact 交付本身是必需的，但单次 compiler-cache lookup 可以 miss 并正常编译。

随后六个 runner 都以 producer 的 build type、测试选项和显式 C/C++ ccache launcher，执行同一条显式
Job-enabled legacy 外层
`cmake --fresh -S "$GITHUB_WORKSPACE" -B "$GITHUB_WORKSPACE/build/ci" -G Ninja`。它们会查询
CTest JSON object model，并在选择自身 entry 前要求精确的六项 `build-smoke` inventory。测试本身可以
构建外层 tree，也可以创建更深层 build/install tree；编译 key 匹配时，两者都会使用 read-only
producer cache。每个 runner 都会在 CTest 后打印自己的 ccache statistics。

Smoke job 绝不下载 `ctest-runtime`，也不会收到完整 producer tree。CTest 调用同时使用锚定的精确
`--tests-regex`、精确 `--label-regex '^build-smoke$'` 与 `--no-tests=error`，因此测试被重命名、缺失或
label 错误时，不会把空选择误判为成功。

六个 job 会与 `unit`、`integration` 和 `verification` label job 并行运行。它们在各自 runner 中产生的
外层及嵌套 build/install 输出不会写回 read-only handoff 或跨 run Actions Cache，也绝不会进入
`ctest-runtime`。每个 job 把 report 写到
`CI-results/build-smoke/<matrix-artifact>.junit.xml`；一个 `always()` step 会把它
上传为唯一的 `ctest-junit-build-smoke-<matrix-artifact>` artifact，保留七天，report 不存在时只告警。
新增或重命名长期 build smoke 时，必须同步更新 CTest 注册、固定 workflow matrix 与本清单。

Daemon package、IPC protocol、installed layout/RPATH 与 interoperability test 在外部
[photospider-daemon](https://github.com/kevin-zf1123/photospider-daemon) 仓库运行。这个 kernel
workflow 不配置 daemon ownership，也不重复这些 test。
Daemon downstream gate 只在 installed API/package breaking change 或显式 release check 时请求，
不阻塞每个 kernel PR。

### 轻量 CTest runtime

`ci/scripts/package_ctest_runtime.sh` 会从已经完成完整构建的 `build/ci` producer tree 创建唯一一份物理文件 `ctest-runtime.tar.gz`。归档会保留 CTest 运行所需的 runtime closure：

- 静态与动态库；
- operation、policy 与 scheduler plugin；
- 测试与产品 executable；
- `CTestTestfile.cmake` 与生成的 GoogleTest inventory；
- `CMakeCache.txt`、生成的 package configuration 和其他 runtime data。

归档排除所有 `.o` 与 `.obj`、所有 `CMakeFiles` tree、`.ninja_deps`、`.ninja_log`、既有 `Testing` 输出，
以及上文精确命名的两个临时 nested-smoke root。增量 compiler result 只存在于 ccache；`tests/` 的其余
内容仍作为 CTest runtime 保留。归档只上传一次，关闭 artifact 二次压缩，并且只由三个
primary-label job 下载。Build-smoke runner 会配置各自的外层 tree，不会下载它；任何 runner 都不会
创建第二份 runtime package。Packager 在验证 closure 后会打印物理
archive byte count 与 tar entry count。这些 diagnostic 与 ccache 自身的 hit/miss statistics 只是
cache/artifact 体积实验的 observation，不会放宽 required root 或 forbidden entry 检查。

每个测试 job 都把归档恢复到 producer 使用的相同路径 `build/ci`，随后运行一个精确的 primary label。CI 不会生成重复 runtime package，也不会上传完整 build-tree artifact。每次 label 调用结束后，一个 `always()` step 会单独尝试把 `CI-results/ctest/<label>.junit.xml` 上传为唯一的 `ctest-junit-<label>` artifact。存在的 report 会保留七天；report 缺失时只告警，不会使 job 失败。

## CTest label 与并行约束

CMake 负责测试选择。默认 push 路径中的每个测试都具有一个 primary label：

- `unit`：源码角色位于 `tests/unit/` 的持续维护 GoogleTest；
- `integration`：持续维护的 integration GoogleTest，以及直接 runtime/CLI 检查；
- `verification`：适合日常 CI 的确定性 safety harness；
- `build-smoke`：每项由一个隔离 matrix job 运行的嵌套构建、安装和 package 检查。

测试可以继续保留 `execution`、`security`、`kernel-concurrency` 或 `value-runtime` 等正交 label。仓库 discovery wrapper 会解析并验证 caller 的每组 property pair，把 source-role primary label 与这些正交 label 去重，并只向 `gtest_discover_tests` 传递一个标量 primary `LABELS` property。由于 upstream module 不能传递 list-valued property，生成的 `TEST_INCLUDE_FILES` script 会在 discovery 后使用各自的 `TEST_LIST`，一次性设置完整 merged list 与全部 caller test property；仓库既不依赖重复 property 的隐式合并，也不依赖 module 的 list-value transport。未知 discovery argument、未知 property、奇数长度 property list 与重复的非 label property 会在 configure 阶段失败。共享可变 harness 或不能重叠运行的测试在 CMake 中声明 `RESOURCE_LOCK` 或 `RUN_SERIAL`，需要边界的测试在 CMake 中声明 `TIMEOUT`。三个 runtime shard 只使用 `--label-regex '^<primary-label>$'`；每个固定 build-smoke entry 还会把简短的锚定精确名称 regex 与 `build-smoke` label 结合使用。Workflow 不会编码一条合并后的冗长包含或排除 regex。

重型 benchmark、fuzz target 与 sanitizer build 是 opt-in 开发者工具。日常 push 会配置 `USE_ASAN=OFF`、`USE_TSAN=OFF` 和 `PHOTOSPIDER_BUILD_FUZZERS=OFF`；默认 job 不运行这些工作负载。

## CI 镜像 workflow

`Dockerfile.ci` 定义 Linux toolchain，并包含 ccache、Ninja 和项目构建依赖。
`.github/workflows/build-ci-image.yml` 只在 `main` 的 `Dockerfile.ci` 变更或手工 dispatch 时运行。它向
`ghcr.io/<owner>/<repo>/photospider-ci` 发布 `latest`、commit tag 和可选 manual tag。

日常 CI workflow 消费这份已发布镜像，并会在 configure 前检查 `ccache` 确实存在。Dockerfile 变更
必须先成功发布，日常 CI 才能依赖新 toolchain。对于首次启用 ccache 镜像这类迁移，应当在包含
Dockerfile 变更的 ref 上手工触发 image workflow，等待 `latest` 发布完成，再运行或重跑日常 CI。
`main` 自动触发的 image build 不会自动排在同时启动的 CI run 之前；日常 CI 本身不会构建或比较镜像。

该镜像发布后的第一次日常 workflow 预期会 miss 跨 run Actions Cache，并让 producer 冷编译。它仍会
上传已填充的同 run ccache tar，因此各 smoke 可以在兼容的外层或 nested 编译上获得 hit，并对其余 miss
正常编译。后续 workflow 可以恢复最新兼容 producer cache，预期会以更热状态开始；hit rate 只是
diagnostic，绝不是正确性门禁。

两个 workflow 都使用持续维护的 Node 24 action major：`actions/checkout@v7`、`actions/cache/restore@v6`、`actions/cache/save@v6`、`actions/upload-artifact@v7`、`actions/download-artifact@v8`、`docker/login-action@v4`、`docker/metadata-action@v6` 与 `docker/build-push-action@v7`。这些 major 要求 Actions Runner 2.327.1 或更新版本；当前 workflow 使用 GitHub-hosted runner，而不是仓库自有的 self-hosted runner。

## 本地检查

本地验证应使用 native build；不要仅为复刻 GitHub Actions 而在 macOS 上模拟 Linux 镜像。
在 final legacy-full pass 前 configure 三个持续维护的 presets：

```bash
cmake --preset kernel-dev
cmake --build --preset kernel-dev --parallel 2
ctest --preset kernel-dev --output-on-failure --parallel 2
cmake --preset op-dev
cmake --build --preset op-dev --parallel 2
cmake --preset legacy-full
cmake --build --preset legacy-full --parallel 2
cmake --preset legacy-full-portable
cmake --build --preset legacy-full-portable \
  --target public_header_self_containment --parallel 2
bash ci/scripts/package_ctest_runtime.sh \
  build/legacy-full CI-results/ctest-runtime.tar.gz
tar -tzf CI-results/ctest-runtime.tar.gz
ctest --preset legacy-full --output-on-failure --parallel 2
```

聚焦本地运行使用与 CI 相同的 primary label：

```bash
ctest --test-dir build/legacy-full --output-on-failure -L '^unit$'
ctest --test-dir build/legacy-full --output-on-failure -L '^integration$'
ctest --test-dir build/legacy-full --output-on-failure -L '^verification$'
ctest --test-dir build/legacy-full --output-on-failure -L '^build-smoke$'
```

持续维护的 `graph_cli_script_test.sh`、`propagation_script_test.sh`、`plugin_load_test.sh`、`execution_repeat_test.sh` 和 `sanitizer_test.sh` 仍可用于显式本地 product-boundary 或 sanitizer 检查。它们不是额外的 GitHub workflow，也不属于日常 push 路径。
