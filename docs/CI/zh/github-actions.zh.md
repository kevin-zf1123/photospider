# GitHub Actions CI

Photospider 有意只保留两个 GitHub Actions workflow：

- `.github/workflows/ci.yml` 处理日常 push，包含一次 healthcheck、一次可复用构建、八个独立 build-smoke runner 和三个并行 CTest label 分片。
- `.github/workflows/build-ci-image.yml` 在 `main` 的 `Dockerfile.ci` 发生变更或维护者手工触发时发布 Linux CI 镜像。

仓库不再设置单独的 pull-request-target、sanitizer、scheduler-log、routing、evidence、provenance 或 aggregator workflow。本仓库按个人开发项目维护，因此 CI 围绕真正有用的构建与测试信号安排，而不是复刻 enterprise 审批或自授权证明机制。

## 日常 CI 流程

`.github/workflows/ci.yml` 在向 `main` 和 `CI/**` push 时运行。每个 job 都使用 `ghcr.io/<owner>/<repo>/photospider-ci:latest`、递归 checkout submodule，并且只拥有仓库内容与 package 的读取权限。Job 形成一条带并行测试叶子的依赖链：

```text
healthcheck -> build -> build-smoke（8 个独立 matrix job）
                     -> unit
                     -> integration
                     -> verification
```

### Healthcheck

在执行项目检查前，Healthcheck 只把 `$GITHUB_WORKSPACE` 加入 job container
用户的全局 Git `safe.directory` 列表，并验证已 checkout 的 `HEAD` 是 commit。
这只是用于跨 checkout 所有权边界保证容器内 Git 可用的设置，不是授权、
protected-path 或 provenance 证明；它绝不使用通配符。

随后 Healthcheck 有意只执行四项低成本检查：

1. 对 push 的 commit 运行 `git diff --check`。
2. 显式检查 `ccache` executable 与版本，使未发布或陈旧的 CI 镜像直接失败，而不是静默绕过 launcher 编译。
3. 把 C 与 C++ compiler launcher 设为 `ccache`，启用测试并关闭 ASan、TSan 与 fuzzer，执行一次 CMake configure。
4. 构建 `public_header_self_containment`。

它不会对变更路径分类、推断 docs-only 运行、比较 protected path、检查其他 ref、构建完整产品，也不会决定后续 job 是否存在。

### 一次 ccache 加速构建

Producer 不再恢复旧的 `build/ci` tree，而是从全新 binary directory 开始，并仅通过
`actions/cache/restore@v6` 恢复 `.ccache`。Compiler-cache key 包含显式 epoch、runner 操作系统与
架构、build type、`Dockerfile.ci` 镜像配方 hash，以及 workflow 的 `run_id` 与 `run_attempt`。
唯一 restore prefix 在 run identity 前结束，因此 producer 可以使用较早兼容 run 中最新的 cache。
Workflow 会同时报告 `cache-hit` 与实际 matched key；fallback 仍然有用，只是仅有 exact current-run
key 才会产生 `cache-hit == true`。

Cache 配置把 `CCACHE_DIR` 放在 workspace 内，把 `CCACHE_BASEDIR` 设为绝对 workspace，保留
`CCACHE_HASHDIR=true` 以确保 debug information 对应正确 working directory，并使用
`CCACHE_COMPILERCHECK=content` 拒绝来自不同 compiler 的 entry，同时把本地 cache 限制为 2 GiB。
恢复后，producer 会清零 ccache statistics，通过显式 C 与 C++ ccache CMake launcher 进行 configure，
只调用一次 Ninja，再打印得到的 hit/miss statistics。随后它把 `.ccache` 保存到新的 run-and-attempt
key。由于 Actions cache entry 不可变，必须使用唯一 primary key；restore prefix 负责跨 run 复用，
而每个成功 producer 都能发布更新后的 cache snapshot。

完整 `build/ci` tree 使用独立 namespace 与用途。全新构建结束后，producer 会打包轻量 CTest runtime，
并按包含 epoch、platform、build type、build configuration hash、commit SHA、`run_id` 与
`run_attempt` 的 exact key 保存 tree。Producer 不恢复这份 tree，下游也不提供 restore prefix，因此
object file 与 Ninja dependency state 只用于同一 workflow 内 handoff。Packager 仍把
`tests/image_artifact_codec_dependency_disabled` 与
`tests/optional_opencv_provider_disabled` 两个固定临时 root 作为 object-free archive invariant 精确排除，
而不依赖旧 tree 带回的 residue。保存两个 immutable cache namespace 并上传唯一 runtime archive 后，
producer 边界即告完成。

### 独立 build-smoke runner

默认 CI 配置中的八个 build smoke 各自对应一个固定 matrix entry：

- `DependencyDisabledInstallSmoke`；
- `OpenExrDeepProviderOptionOffSmoke`；
- `StaticProductConsumerSmoke`；
- `PhotospiderdInstallLayoutSmoke`；
- `IpcDisabledInstallSmoke`；
- `ImageArtifactCodecDependencyDisabledBuild`；
- `OpenCvOperationProviderDisabledBuild`；
- `PublicHeaderSelfContainment`。

Matrix 使用 `fail-fast: false`，因此 producer 成功后，每个 entry 都会获得独立的 runner 和 container。
各 runner checkout 同一个 `github.sha`，恢复 producer 为相同 `run_id` 与 `run_attempt` 保存的 exact
ccache key，并要求 `cache-hit == true`。恢复后的 compiler cache 在 smoke job 中为 read-only，且绝不
save，因此一个 consumer 不会改变其他 consumer 使用的 producer snapshot。Fresh nested CMake
configuration 会继承 C 与 C++ launcher environment；每个 runner 在 CTest 后都会打印本地 ccache
statistics。

Matrix flag 会按测试的真实依赖划分外层 CTest handoff：

- `DependencyDisabledInstallSmoke`、`OpenExrDeepProviderOptionOffSmoke`、
  `PhotospiderdInstallLayoutSmoke`、`IpcDisabledInstallSmoke`、
  `ImageArtifactCodecDependencyDisabledBuild` 与
  `OpenCvOperationProviderDisabledBuild` 下载唯一的 object-free runtime。它们会创建 fresh nested
  build tree；OpenEXR driver 还会从保留的 producer `CMakeCache.txt` 读取 platform configuration。
- `StaticProductConsumerSmoke` 与 `PublicHeaderSelfContainment` 会直接构建 producer tree 中的 target。
  它们恢复独立的 exact full-tree key，同时要求非 miss 与 `cache-hit == true`，然后通过显式 ccache
  launcher 重新 configure 该 tree，再执行 CTest。

Smoke runner 的两类 cache restore 都不使用 fallback prefix。CTest 调用同时使用锚定的精确
`--tests-regex`、精确 `--label-regex '^build-smoke$'` 与 `--no-tests=error`，因此测试被重命名、缺失或
label 错误时，不会把空选择误判为成功。

八个 job 会与 `unit`、`integration` 和 `verification` label job 并行运行。它们在各自 runner 中产生的
嵌套 build/install 输出不会写回任一 immutable producer cache，也绝不会进入 `ctest-runtime`。每个
job 把 report 写到 `CI-results/build-smoke/<matrix-artifact>.junit.xml`；一个 `always()` step 会把它
上传为唯一的 `ctest-junit-build-smoke-<matrix-artifact>` artifact，保留七天，report 不存在时只告警。
新增或重命名长期 build smoke 时，必须同步更新 CTest 注册、固定 workflow matrix 与本清单。

### 轻量 CTest runtime

`ci/scripts/package_ctest_runtime.sh` 会从已经完成完整构建的 `build/ci` producer tree 创建唯一一份物理文件 `ctest-runtime.tar.gz`。归档会保留 CTest 运行所需的 runtime closure：

- 静态与动态库；
- operation、policy 与 scheduler plugin；
- 测试与产品 executable；
- `CTestTestfile.cmake` 与生成的 GoogleTest inventory；
- `CMakeCache.txt`、生成的 package configuration 和其他 runtime data。

归档排除所有 `.o` 与 `.obj`、所有 `CMakeFiles` tree、`.ninja_deps`、`.ninja_log`、既有 `Testing` 输出，
以及上文精确命名的两个临时 nested-smoke root。增量 compiler result 只存在于 ccache；`tests/` 的其余
内容仍作为 CTest runtime 保留。归档只上传一次，关闭 artifact 二次压缩，并由三个 primary-label job
与六个创建 fresh nested tree 的 build-smoke runner 下载。另两个 build-smoke runner 恢复 exact
producer tree；任何 runner 都不会创建第二份 runtime package。Packager 在验证 closure 后会打印物理
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

两个 workflow 都使用持续维护的 Node 24 action major：`actions/checkout@v7`、`actions/cache/restore@v6`、`actions/cache/save@v6`、`actions/upload-artifact@v7`、`actions/download-artifact@v8`、`docker/login-action@v4`、`docker/metadata-action@v6` 与 `docker/build-push-action@v7`。这些 major 要求 Actions Runner 2.327.1 或更新版本；当前 workflow 使用 GitHub-hosted runner，而不是仓库自有的 self-hosted runner。

## 本地检查

本地验证应使用 native build；不要仅为复刻 GitHub Actions 而在 macOS 上模拟 Linux 镜像：

```bash
cmake -S . -B build/ci -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DUSE_ASAN=OFF \
  -DUSE_TSAN=OFF \
  -DPHOTOSPIDER_BUILD_FUZZERS=OFF
cmake --build build/ci --parallel 2
bash ci/scripts/package_ctest_runtime.sh \
  build/ci CI-results/ctest-runtime.tar.gz
tar -tzf CI-results/ctest-runtime.tar.gz
ctest --test-dir build/ci --output-on-failure --parallel 2
```

聚焦本地运行使用与 CI 相同的 primary label：

```bash
ctest --test-dir build/ci --output-on-failure -L '^unit$'
ctest --test-dir build/ci --output-on-failure -L '^integration$'
ctest --test-dir build/ci --output-on-failure -L '^verification$'
ctest --test-dir build/ci --output-on-failure -L '^build-smoke$'
```

持续维护的 `graph_cli_script_test.sh`、`propagation_script_test.sh`、`plugin_load_test.sh`、`execution_repeat_test.sh` 和 `sanitizer_test.sh` 仍可用于显式本地 product-boundary 或 sanitizer 检查。它们不是额外的 GitHub workflow，也不属于日常 push 路径。
