# GitHub Actions CI

## Workflows

- `.github/workflows/ci-healthcheck.yml`: static healthcheck on pull requests targeting `main` through `pull_request_target`, pushes to `main` and `CI/**`, and manual dispatch, followed by one stable `healthcheck` result gate.
- `.github/workflows/ci-integration.yml`: documentation-only and image-input routing, published/candidate image identity selection, one build-once candidate call, one shared-suite call, same-digest promotion, and one stable `integration` result gate.
- `.github/workflows/ci-integration-suite.yml`: the typed digest-qualified reusable test DAG shared by published and candidate images, including one same-tree producer, role-specific artifacts, ordinary CTest, default/dedicated/OpenEXR build smokes, scripted runtime shards, and Darwin/Linux security profiles.
- `.github/workflows/ci-sanitizer.yml`: manual ASan or TSan focused checks.
- `.github/workflows/build-ci-image.yml`: reusable trusted-push candidate producer for one event-scoped temporary GHCR tag and its exact digest/attestation; it cannot publish canonical tags or `latest`.

## Branch and Workflow Guards

Push-triggered CI runs only on `main` and branches whose names start with `CI/`. This prevents ordinary feature branches from running workflow files changed on that branch.

Pull requests targeting `main` use `pull_request_target`, which uses the workflow definition from the base branch while checking out the pull request head commit for tests. A `CI/**` pull request is deduplicated in favor of the push trigger only when its head repository is the base repository itself. A fork can use the same branch name without producing a push in the base repository, so a fork `CI/**` pull request fails closed before checkout instead of receiving a synthetic green result or executing untrusted fork code in this privileged event.

The first healthcheck and integration job protects CI workflow inputs before any repository script or local CI image build runs. A same-repository `CI/**` pull request is the only pull-request case skipped in favor of its base-repository push run. A fork `CI/**` pull request enters the job and is rejected before checkout; missing head-repository identity also fails closed. For every other pull request, the job fetches the target branch from the base repository and uses the event's exact base and head commits. Other guarded runs fetch `origin/main` and use `HEAD`. Both diff paths require exactly one merge base and compare that merge-base tree to the selected head with rename detection and Git-status filtering disabled. The changed paths are produced as NUL-delimited records in a parent-shell-visible artifact, read into a Bash array, and matched as exact path values. Human-readable changed and protected inventories use shell-safe `%q` rendering, so an embedded newline cannot split a path or forge a log record. A failed Git producer or inventory read terminates the guard before a success summary. This preserves three-dot semantics for a manually dispatched ref that is behind `main` and includes type changes, uncommon statuses, or unusual valid filenames in the protected-path inventory. The guard fails if the resulting diff changes any of:

- `ci/**`
- `.github/workflows/**`
- `Dockerfile.ci`
- `.dockerignore`

CI workflow changes must therefore be developed on a `CI/**` branch in the base repository. The branch prefix alone is never authorization: a pull request must also have the base repository as its head-repository identity, while push and manual runs already belong to the base repository. The guard also catches non-`CI/**` pull requests that target `main`, so workflow-related files cannot be merged through an ordinary feature branch or a same-named fork branch.

## Documentation-only Routing

The integration workflow runs `change-classification` after the protected-path guard. A change is documentation-only only when every changed path is one of:

- any file under `docs/**`, including every Chinese mirror;
- a root-level `*.md` or `*.markdown` file, including `readme.md`, `manual.md`, and `CONTEXT.md`;
- the root-level extensionless `README`, `LICENSE`, `NOTICE`, `CHANGELOG`, `CONTRIBUTING`, `CODE_OF_CONDUCT`, or `SECURITY` contract, matched case-insensitively.

Every other path requires the complete build and test chain. This includes source and headers, CMake files, tests, plugins, applications, CI scripts, workflows and actions, configuration, dependencies and lockfiles, Docker inputs, assets, nested Markdown outside `docs/**`, and any unknown file. The classifier uses `git diff --no-renames` without a status filter, so a source file renamed into `docs/**` still exposes the deleted source path and cannot be misclassified as documentation-only. Added, copied, deleted, modified, renamed, type-changed (`T`), unmerged, broken-pairing, and unknown-status paths all enter the inventory; an uncommon status is never omitted merely because it was not enumerated in an allowlist.

For `pull_request` and `pull_request_target`, the classifier requires the exact base and head SHAs from the event and exactly one merge base, then evaluates the pull request diff from that merge base to the head. A `main` push compares the exact `before` and head trees. Every `CI/**` push always runs the full chain, even when a later incremental push contains only documentation; this prevents an earlier source or workflow commit on the same branch from escaping current-head integration after pull-request-trigger deduplication. `workflow_dispatch` also always runs the full chain. An unsupported event, an absent or malformed push branch identity, an absent, malformed, all-zero, shallow, or unreachable revision, a missing or ambiguous merge base, a diff failure, or an empty changed-path inventory all fail closed to full integration. The workflow uses `fetch-depth: 0`; it never guesses `origin/main` or `HEAD~1` when event identity is unavailable.

For a documentation-only change, `ci-image-change`, integration planning, all builds, full CTest, build smokes, and the scripted integration shards are intentionally skipped. The always-running `integration` gate verifies those exact skipped conclusions and writes the reason to the GitHub step summary. It fails when classification or an upstream dependency fails, rather than passing because `needs` silently propagated a skip. The workflows remain triggered instead of using `paths-ignore`, so stable required checks receive a conclusion instead of remaining pending. The `healthcheck` gate likewise always concludes and verifies whichever published-image or local-image healthcheck path was selected. Only a same-repository `CI/**` pull request reports intentional push-triggered deduplication; fork or missing repository identity cannot take that shortcut.

## Runtime

`Dockerfile.ci` defines the GitHub Linux test environment. The protected
`photospider-ci:latest` lock value is only a discovery locator. A trusted host
job resolves it and verifies the exact subject's source/signer workflow
attestation in process-private memory before it creates a formal artifact
directory, retains runner identity, or lets Docker pull/expand an image layer.
Failure leaves the always-upload path absent and final workflow output
unchanged. Only after success does it persist attestation evidence, pull the
digest-qualified image, validate OCI revision and canonical manifest labels,
and expose only the resulting
`ghcr.io/<owner>/<repo>/photospider-ci@sha256:...` reference. Published-image
healthcheck and build/test integration jobs execute that verified
digest-qualified reference; none executes the mutable locator directly.
`healthcheck-published-image` is a container job and does not rely on checkout's
temporary HOME-scoped Git trust surviving into later container steps.
Immediately after checkout, its unique `Trust checked-out workspace` step
explicitly selects `shell: bash`, adds only the exact `$GITHUB_WORKSPACE` value
to the job container's persistent global `safe.directory` configuration, and
verifies `HEAD^{commit}` with a read-only Git command. It never configures
`safe.directory=*`, trusts a parent directory, or executes a checked-out
repository script. This boundary completes before both conditional history
fetches and `healthcheck.sh`, so it also covers published-image `main` pushes
and `workflow_dispatch` runs in which neither fetch step executes. The `Fetch
pull request base history` and `Fetch CI branch main history` steps likewise
explicitly set `shell: bash`, so their `set -Eeuo pipefail` prologues run under
Bash instead of relying on the container default shell. Protected-path,
change-classification, and stable result-gate jobs remain lightweight
`ubuntu-24.04` jobs and do not configure or compile the project.

When a pull request or push changes any path in the canonical CI-image lock's
`input_paths`, detection still fetches and verifies the exact base instead of
relying on a possibly absent fork `origin/<base>`. The detector strictly parses
the lock independently at the merge base and head, requires the lock to include
itself, and compares the NUL-delimited Git path inventory with the union of both
validated path sets. Lock additions, removals, duplicates, traversal, malformed
JSON, or a missing revision therefore cannot emit `changed=false`. A fork head
is rejected before checkout, and a same-repository protected `CI/**` pull
request uses its trusted push route. The image-change healthcheck job does not
build an image: it verifies the exact hosted runner, protected locks, canonical
publish-source identity, and generated image-input manifest before running the
static healthcheck. Only an eligible trusted `main` or `CI/**` integration push
may call the one candidate-image producer and shared digest-bound suite.

Hosted labels remain mutable even when their major OS is explicit. GitHub says
runner-image deployment normally takes two to three days, creates a prerelease
while rollout is in progress, and requires the exact job version to be read
from `Set up job` ([official runner-images guidance](https://github.com/actions/runner-images#what-image-version-is-used-in-my-build)).
The finite Linux rollout set therefore contains stable
`ubuntu24/20260816.277.1` and rollout `ubuntu24/20260823.283.1`. The former was
observed in exact-head healthcheck run
[`32997831039`](https://github.com/kevin-zf1123/photospider/actions/runs/32997831039/job/98271915852)
and Integration builder run
[`32997831190`](https://github.com/kevin-zf1123/photospider/actions/runs/32997831190/job/98271974769),
and is the official
[`ubuntu24/20260816.277` release](https://github.com/actions/runner-images/releases/tag/ubuntu24%2F20260816.277).
The latter was observed in run
[`32991073228`](https://github.com/kevin-zf1123/photospider/actions/runs/32991073228/job/98248727299)
and is the official rollout
[`ubuntu24/20260823.283` prerelease](https://github.com/actions/runner-images/releases/tag/ubuntu24%2F20260823.283).
The Darwin set similarly contains stable `macos15/20260727.0256.1` bound to
vcpkg commit
[`6d9d7df564a1ccdaa994e4ad39ccd4a32360867b`](https://github.com/microsoft/vcpkg/commit/6d9d7df564a1ccdaa994e4ad39ccd4a32360867b)
and rollout `macos15/20260824.0311.1` bound to
[`127402f1c75bb3d5ff6bce04b285faa4930a5aca`](https://github.com/microsoft/vcpkg/commit/127402f1c75bb3d5ff6bce04b285faa4930a5aca),
matching the official
[`macos-15-arm64/20260727.0256` release](https://github.com/actions/runner-images/releases/tag/macos-15-arm64%2F20260727.0256)
and
[`macos-15-arm64/20260824.0311` prerelease](https://github.com/actions/runner-images/releases/tag/macos-15-arm64%2F20260824.0311).
Each job measures the environment once and retains one exact resolved runtime
record. The Linux allowlist lock bytes remain an image-manifest input, while
the manifest's `builder_runner` and OCI builder-version label bind the actual
builder member; a verifier on the other approved member reconstructs this
retained provenance instead of substituting its own `ImageVersion`. Darwin
consumers use the retained record's one-to-one vcpkg mapping. Unknown or
tampered records fail before candidate work. Runner lock and retained-record
inputs require `O_NOFOLLOW`, `O_NONBLOCK`, and `O_CLOEXEC`; FIFO and device
paths are rejected without blocking, while fresh retained output uses
`O_EXCL` and refuses every residual path. After rollout completes, removing
the retired member requires a reviewed protected-lock update and new image.
Manifest creation independently opens every canonical image input exactly once
with `O_NOFOLLOW`, `O_NONBLOCK`, and `O_CLOEXEC`, obtains digest and size from
two agreeing reads of that retained descriptor, and reuses the retained lock,
helper, action, and runner bytes for semantic checks without reopening them.

The callable producer itself is a complete parsed-tree contract: it exposes only
the typed `workflow_call`, exact write permissions, and one `ubuntu-24.04` build
job with its reviewed ordered steps. Checkout and the prebuild
`ci_lock_verify.py` invocation precede the unique `docker/build-push-action`;
that action has no environment or condition, pushes only the event-scoped
temporary tag, and accepts exactly the context, Dockerfile, three immutable
labels, and manifest/source-commit build arguments. Extra steps, Docker/Buildx
commands, canonical or `latest` writes, build arguments, fields, or jobs fail
the protected verifier before image construction.

For every `CI/**` push, both healthcheck routes fetch and verify `origin/main` inside their own job, then pass `origin/main` as `CI_BASE_REF`. The published-image job verifies it before `healthcheck.sh`; the image-change job verifies it before canonical manifest generation and `healthcheck.sh`. Static checks therefore cover the cumulative branch diff from the `main` merge base, so a later documentation-only push cannot hide an earlier unformatted C++ commit. An ordinary `main` push retains the exact `github.event.before` value as `CI_BASE_REF` and checks only that push increment. A required fetch or reference verification failure stops the job before `healthcheck.sh` can use fallback base selection. CI-image detection uses the same cumulative `origin/main` basis for `CI/**` pushes, so a later documentation-only push also cannot hide an earlier image-input commit.

The image-input detector uses a no-rename inventory with no Git status filter;
deletions, type changes, and uncommon statuses all remain visible. Its Python
reader consumes Git's NUL-delimited bytes directly and writes a JSON-quoted
diagnostic path log; no Bash whitespace or newline parser classifies paths.
The healthcheck static-scope inventory also disables rename detection but
intentionally applies `--diff-filter=d`: deleted paths are excluded because
formatters and linters require a current file, while type changes and uncommon
non-deletion statuses remain visible. A failed lock read or `git diff` exits
nonzero before any `changed=false` output or “No changed C++ files” summary can
be emitted. This avoids a false route as well as a race with another workflow
that may still be publishing the new `latest` image.

The image includes CMake, a C++ toolchain, OpenCV, yaml-cpp, CURL, OpenSSL,
GTest, nlohmann-json, Python, cpplint, and clang-format. The formatter is
installed from the PyPI wheel at version 21.1.5 so CI and developer formatting
do not drift merely because different formatter releases choose different line
breaks. The maintainer machine currently uses 21.1.3; its formatted output was
verified byte-for-byte equal to 21.1.5 for the changed C++ inventory covered by
this alignment. Developer environments should adopt 21.1.5 going forward.

The clang-format alignment does not add a separate version-detection job. The
CI image independently binds the Ubuntu base by digest and every direct Ubuntu
package by exact version in one signed immutable APT snapshot. Because the
minimal base has no TLS trust bundle, Docker BuildKit first fetches only the
snapshot's exact `openssl` and `ca-certificates` packages through checksum-bound
`ADD` instructions. Before APT can run, the protected
`ci/locks/ubuntu-24.04-snapshot.sources.in` Deb822 template replaces every base
archive, security, and ports source with the timestamp-qualified
`snapshot.ubuntu.com` URI. That one signed archive serves native amd64 and arm64
indices. `dpkg` configures the verified offline bytes, and the first and only APT
update/install sequence then consumes the complete package lock from that
explicit source. No live APT bootstrap or mirror override is a trusted fallback;
checksum failure or an unsatisfiable same-snapshot closure fails the candidate
image build. Every lock row has an exact version and a Debian package name of at
least two characters whose first character is alphanumeric; the remaining name
characters are limited to lowercase alphanumerics plus `+.-`. The installer
places apt's `--` option terminator after all fixed options and before the locked
`name=version` arguments, so an option-shaped row is rejected twice rather than
reinterpreted as an APT flag. Before active instructions are parsed, the
restricted Docker parser models BuildKit's UTF-8 BOM and first-line shebang
removal. Both preambles are forbidden. A hash/C-style directive marker must
begin at byte zero; only after that marker does detection trim the exact Go
`unicode.IsSpace` Unicode White_Space set. It deliberately does not use
Python's wider `str.isspace()` controls. Every resulting `syntax` frontend in
those comment forms or JSON is rejected, including case, Unicode whitespace,
tag, digest, and shebang-hidden variants. Space/tab before a marker remains a
non-active ordinary comment; ordinary comments still close the traditional
Docker directive phase, and the canonical backslash `escape` directive remains
the only permitted parser directive. Frontend detection and active logical
instruction parsing consume the same Go `bufio.ScanLines`-equivalent physical
lines: LF is the only separator and one terminal CR is removed from each token.
CR-only, VT/FF, FS/GS/RS, NEL, and Unicode line/paragraph separators remain
inside the preceding token and cannot expose a hidden instruction. Canonical LF
and CRLF input, continuations, and a terminal CR retain their documented
behavior.

## Integration Test Sharding

For a non-documentation change on the normal published-image path,
`integration-plan` configures the checked-out commit with testing enabled and
parses `ctest --show-only=json-v1` as a non-authoritative preflight. CMake's
default `gtest_discover_tests` mode discovers GoogleTest cases after their
targets build, so configuration-time CTest state may contain unlabelled
`*_NOT_BUILT` placeholders and no labelled entries. The preflight therefore
allows an empty label selection while still rejecting malformed JSON, duplicate
tests or properties, invalid labels, and any labelled entry that is disabled or
commandless. Its preview matrix is retained only as a diagnostic artifact and
is never exposed as a workflow job output.

`build-integrity-default` then builds the complete default tree and repeats the
same JSON query. This post-build invocation is authoritative: it requires a
nonempty exact `build-smoke` selection, validates the `ctestInfo` version,
complete test-name uniqueness, property and label shapes, enabled state,
executable commands, and matrix size, and emits the compact matrix through a
stable job output. Artifact keys are derived from a bounded ASCII slug plus a
SHA-256 name digest, while the exact test name remains a JSON matrix value. The
matrix uses stable case-insensitive name ordering, and no workflow test-name
list is maintained. A focused real CMake fixture configures a
`gtest_discover_tests` target, observes its configuration-time `_NOT_BUILT`
placeholder and empty preview, builds it, and requires the strict post-build
matrix to contain the subsequently discovered labelled case.

The current labelled inventory is:

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

The five default dependency/configuration drivers, three daemon install-layout
cases, and static-product consumer create or validate isolated nested build
profiles;
public-header self-containment invokes its dedicated compile target. These are
durable product, package, configuration, and compile boundaries, not migration
or source-layout checks. `OpenCvOperationProviderBuildSmokeSafety` remains an
ordinary full-CTest safety regression for the OpenCV nested-build driver: its
Python unittest exercises cleanup guards and cache-layout helpers in-process.
It also configures one compiler-free `project(... NONE)` fixture through the
production manifest generator; it never starts a child build, install, compile
target, or generated executable.
This default profile therefore contains ten labelled `build-smoke` entries.
When `PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER=ON`, the conditional
`OpenExrDeepProviderInstallConsumerSmoke` entry is added as the eleventh entry;
it is not part of the default inventory.

Two nested-profile inventories remain exact without workflow-maintained counts.
The static-product producer exports its configured CMake public-header install
allowlist, and the consumer requires the installed include tree to equal those
relative paths. Its CMake writer rejects backslashes and representable ASCII
controls before serialization; its parser independently rejects all ASCII C0
controls including NUL, plus DEL, and requires exact canonical POSIX install
paths while preserving ordinary spaces.
The provider-disabled producer exports its active `gtest_discover_tests`
targets with configuration-specific executable paths through a TSV containing
one exact header and strict two-field data lines. Later comments, blank lines,
controls, extra fields, invalid or duplicate target names, and relative paths
fail closed; absolute POSIX, Windows drive, and Windows UNC paths remain valid.
After the focused build, the driver requires exactly the registered targets
without executable files to appear as unlabelled `*_NOT_BUILT` placeholders.
Both checks reject missing and extra entries, so a new allowlisted header or
registered GoogleTest target changes the expectation through its authoritative
CMake declaration rather than a Python number or future-name special case.

`build-integrity-default` configures once, runs `build_required_targets` and
then `build_all` in the same build tree, and never splits those phases across
runners. It exports four identity-bound role artifacts instead of the former
2.416 GB complete build tree:

- `ci-control-default` contains the exact recursive CTest control graph,
  producer cache/stamp, and generated inventory needed by ordinary fresh-build
  smokes; it contains no producer object files or ordinary test executables.
- `ci-runtime-default` extends that control role with the complete post-build
  ordinary CTest closure. The producer excludes only the exact `build-smoke`
  label, records every command and property, recursively follows every CTest
  include regardless of filename, and resolves relative command, argument,
  `REQUIRED_FILES`, environment, and environment-modification paths from each
  test's effective working directory. That directory is only a resolution
  base and is never copied wholesale. The role selects the referenced
  executables, build-tree data, shared libraries, plugin trees, and trust
  material. After restore, the consumer re-runs JSON discovery and rejects
  `_NOT_BUILT`, a changed inventory, or any missing
  include/executable/data/runtime input.
  A versioned DSO alias is accepted only when it resolves to a regular file
  inside the producer tree; it is materialized as same-byte regular members,
  so the archive itself contains no link.
- `ci-installed-package-default` contains only `ci/installed/**`,
  `ci/producer/CMakeCache.txt`, and
  `ci/producer/generated/ci_inventory/installable_public_headers.txt`.
  Static libraries are allowed only inside the installed prefix; CMakeFiles,
  CTest state, build stamps, objects, dependency files, and unrelated build
  libraries are forbidden.
  Installed DSO aliases use the same in-prefix validation and regular-member
  materialization; dangling, escaping, directory, or special targets fail.
- `ci-openexr-metadata-default` contains exactly
  `ci/producer/CMakeCache.txt`. Its dedicated runner executes the source-tree
  `OpenExrDeepProviderOptionOffSmoke` driver with the original cached Python,
  CMake, CTest, symbol-tool, configuration, and architecture propagation
  inputs. The single-config producer's unique, nonempty, control-free
  `CMAKE_BUILD_TYPE` supplies `--config`; caller environment cannot override
  it, and a multi-config cache is rejected. The role downloads no CTest graph,
  stamp, generated inventory, object, or product library.

Every role has a canonical member/digest/size manifest and is verified before
extraction. A targeted manifest is copied from one no-follow regular-file
snapshot; its exact SHA-256 is passed from attestation into Python's retained
snapshot verification, so pathname replacement cannot switch the validated
identity. `full-ctest` consumes only `ci-runtime-default`, excludes the exact
label, and runs with `--parallel ${CI_JOBS}` while preserving failure logs and
JUnit output. Scripted CLI, propagation, plugin, and execution-repeat shards
consume the same runtime role only where they need the built runtime.

The protected routing lock is versioned and temporary. It sends only
`PublicHeaderSelfContainment` to the original producer,
`StaticProductConsumerSmoke` to the `installed-package` role,
`OpenExrDeepProviderOptionOffSmoke` to the exact `openexr-metadata` role, and
every other discovered smoke to `ctest-control`. The control, installed,
OpenEXR, and producer outputs are pairwise disjoint and exhaust the post-build
CTest inventory. The dedicated installed-package job runs in the
same digest-qualified image as the producer. Its package-input mode skips the
producer build/install but still executes daemon help, install/export/symbol
inspection, every positive consumer compile/link/run probe, and every negative
component check. Prefix, metadata, and job-owned work are pairwise disjoint;
only the work directory is deleted. Before and after this execution, the job
remeasures the exact verified manifest member set, byte sizes, SHA-256 digests,
and executable attributes. Addition, deletion, rewrite, or mode drift fails,
and evidence upload contains only consumer/verifier logs plus any attestation
records—not the restored prefix or producer metadata tree.

The regular `build-smoke` matrix retains `fail-fast: false`. A literal empty
include fallback keeps `fromJSON` valid only when its producer is skipped; a
successful producer cannot emit an empty or incomplete partition. Each item
receives its own 30-minute workflow timeout and preserves the CTest
registration's timeout and `RUN_SERIAL` semantics.

The runner re-queries CTest JSON immediately before execution and requires the
selected exact name to remain unique, enabled, executable, and labelled.
After that label check, execution uses only the validated numeric CTest index;
no test name is interpolated into a shell command or regular expression.
`IpcDisabledInstallSmoke` therefore executes through its maintained
CTest registration and creates its own clean `PHOTOSPIDER_BUILD_IPC=OFF`
producer instead of depending on a separately hard-coded workflow profile.

Published-image and image-input-changing routes call the same typed reusable
suite with one independently expected digest-qualified `image_ref`, candidate,
profile, manifest, source, and workflow identities. Before any candidate
checkout, code, or container starts, its host preflight checks out
`github.repository` at the exact caller `workflow_commit`, binds that value to
the actual `github.workflow_sha`, and runs the protected image verifier. The
verifier independently cross-checks the requested digest/reference, GHCR
attestation signer/source, OCI revision, and canonical manifest digest against
all caller fields. GitHub permits a called workflow to preserve or reduce the
caller's `GITHUB_TOKEN` permissions, but never to elevate them. Permission
compatibility is validated for the reusable call before a later job-level
`if` can make a job skip, so the shared workflow has no workflow-wide
permission declaration. Every job that checks out or executes candidate code,
runs a candidate container, or aggregates results declares an exact read-only
or empty job-level permission map. The sole exception is
`attest-targeted-artifacts`: it declares no local permission map, runs only
when `publish_reusable_attestations` is true and targeted verification has
succeeded, and inherits the required `artifact-metadata`, `attestations`, and
`id-token` writes only from one of the two trusted push callers. The
pull-request caller passes only read permissions and `false`, while all other
jobs remain unable to inherit a trusted caller's writes.

A trusted image-changing push builds once under an event-scoped temporary SHA
tag, attests that exact
digest, and fans it out without also invoking `integration_suite.sh`. Only
after every suite job succeeds may promotion copy that same digest without
rebuilding. Promotion first establishes the immutable `sha-<full-commit>`
reference, then a current run may update its branch tag; only a successful
trusted `main` push may advance `latest`. `main` retains the exact
`branch-main` tag. A `CI/**` branch
tag is `branch-<bounded-readable-slug>-<sha256>`, where the complete 64-hex
SHA-256 suffix hashes the exact full branch-name bytes without an added
newline. Git itself validates `refs/heads/<branch>`; the slug is not a second
ref allowlist. It retains Docker-safe ASCII, deterministically compresses every
other byte—including legal punctuation or UTF-8—into separators, and truncates
only that presentation, keeping the tag Docker-safe and at most 128 characters
without aliasing `CI/a-b` and `CI/a/b`. Pull-request routes remain read-only.

Only the promotion job holds one repository/CI-image-namespace concurrency
group shared by every ref. It retains queued writers, sets
`cancel-in-progress: false`, serializes workflow-owned SHA and mutable writers,
and never cancels the whole Integration workflow or a candidate because a
documentation-only run arrived. Queue order is not trusted. Before any
registry write, the protected manifest helper fetches the exact live branch
twice around an
isolated worktree measurement, requires the tested candidate to be its
ancestor, and reuses the canonical image-input path lock, source-commit
resolver, and manifest digest. A later documentation-only descendant with the
same source/manifest remains promotable. A later image-input identity reports
`superseded`. Force-push, unknown ancestry, manifest failure, or ref drift
during measurement fails with zero registry writes.

After freshness succeeds, promotion strictly parses the SHA tag's unique
top-level manifest digest. An exact digest is reused without a create call. An
absent SHA is created alone only when locked Buildx returns its exact not-found
status and diagnostic, then both creation metadata and immediate registry
resolution are rechecked before any mutable write. A conflicting, missing, or
ambiguous digest, or an authentication, network, or unknown inspect failure,
fails with zero writes. A `superseded` run may create or reuse only that SHA and
cannot touch branch or `latest`. The global lease closes workflow-owned
cross-ref check/create races; GHCR offers no tag compare-and-swap guarantee for
an out-of-band nonworkflow writer, which remains an explicitly governed
residual boundary. The stable gate reports
`promoted` and `superseded` separately rather than presenting a skipped write
as a successful promotion.
The user-observed
2.416 GB archive, 4-minute-19-second compression, and greater-than-31-GB
nominal 13-consumer transfer remain the remote comparison baseline; reduction
and the 30--45 minute image-change target are not claimed until an exact-head
remote run measures them.

CMake 3.16 is the project's compatibility floor, not a workflow-pinned version
for every pull request. Build logic guards policies introduced after that floor,
while current integration exercises the fresh static package consumer on the
supported CI toolchain. A targeted native old-version
producer/install/consumer run is added only when a compatibility-sensitive
change or release check needs it; the regular integration workflow does not
lock Ubuntu or CMake to a dedicated minimum-version job.

## Runtime Architecture Capability Transition

Trusted CI supports exactly two complete runtime validation contracts while the
policy/execution architecture moves through protected `CI/**` files. After
configuration, each runtime-sensitive script captures
`cmake --build <build-dir> --target help` and matches exact target names. The
legacy scheduler contract requires all of `test_scheduler`,
`test_scheduler_plugin_loader`, and `destroy_count_scheduler_plugin`, with no
policy/execution markers. The new contract requires all of
`test_policy_execution`, `test_policy_registry`, and `test_policy_plugin`, with
no legacy markers. A partial, mixed, or marker-free inventory fails before a
build or runtime command; branch names and commit identities never select the
contract.

`build-integrity-default` validates the architecture-neutral
`photospider_kernel`, `graph_cli`, `test_propagation`, and operation-plugin
lifecycle targets, then still builds the complete tree. Full CTest remains the
ordinary-test authority and excludes only the exact `build-smoke` label.
Runtime capability selection preserves coverage semantics by choosing the
applicable legacy or policy/execution targets; it does not preserve the former
orchestration topology. Current post-build routing emits the downstream
`consumer_build_smoke_matrix.json` (`ctest-control`),
`openexr_build_smoke_matrix.json` (`openexr-metadata`), and
`dedicated_build_smoke_matrix.json` (`installed-package`) matrices plus the
producer-local `producer_build_smoke_names.z` list. In particular,
`StaticProductConsumerSmoke` runs through the dedicated installed-prefix
package-input boundary, retaining the full consumer checks without rebuilding
or reinstalling its producer.

Runtime-sensitive shards select behavior without introducing product
compatibility:

- Scripted CLI configuration emits either the legacy `scheduler_*` keys or the
  new `policy_*` and `execution_*` keys. It never mixes or translates them.
- Plugin loading validates the operation surface plus either scheduler plugin
  loading/listing or policy registry, policy/execution tests, policy plugin
  loading/listing, and execution route listing.
- `execution-repeat` runs deterministic scheduler repetitions for the legacy
  contract and policy registry, policy/execution, compute-run routing, and
  resource-admission repetitions for the new contract.
- ASan and TSan retain the shared compute/propagation checks and select the
  matching legacy scheduler or new policy/execution focused tests.

Until the candidate-owned matrix replaces the current-main sanitizer fallback,
that fallback uses one terminal NUL-framed v1 invocation stream. Target,
possibly empty GoogleTest filter, and trust flag remain separate fields on Bash
3.2 and Bash 5; whitespace splitting, shell evaluation, and a legacy text
decoder are forbidden. The producer validates every record before emission;
the shell captures it into one fresh transient file, checks the producer status,
and parses it through a fixed descriptor. A failed NUL read with partial bytes,
missing/duplicate terminal, complete or partial tail, duplicate target, or
nonzero producer fails before configure/build/test and writes no success
evidence. The shell persists only a completely decoded stream as diagnostic
evidence, then `run_gtest_checked` proves every empty or nonempty selection is
nonzero before execution.

Linux and Darwin each schedule ASan, TSan, and bounded fuzz as distinct profile
results. On Darwin, `sanitizer-asan-darwin`, `sanitizer-tsan-darwin`, and
`fuzz-codecs-darwin` are three independent `macos-15` jobs that each depend only
on `integration-plan`, download the same protected profile inventory, and own a
separate timeout and diagnostic artifact. No profile waits for a sibling, so
one failure cannot prevent the other two jobs from being scheduled; the shared
suite gate nevertheless requires all three conclusions to be successful.
The protected lock verifier compares each complete Darwin job mapping, including
its only five ordered steps and every allowed field. The suite gate checks out
the exact protected `workflow_commit` and invokes only the version/hash-bound
`integration_suite_gate.py`; its complete `needs`, result environment, checkout,
permissions, outputs, and helper call are exact mappings. The helper rejects
failed, skipped, missing, or unknown required results, validates attestation
`success` for publishing routes versus `skipped` for read-only routes, validates
the digest, and writes output only after all checks pass. Unknown steps,
`continue-on-error`, extra fields/statements, comments, no-ops, early exit, or a
sibling dependency cannot satisfy the maintained routing contract.
The helper source must also match three independent identities: a
verifier-owned exact byte SHA-256, the protected JSON helper lock, and the
retained regular-file measurement. The measurement uses one
`O_NOFOLLOW`/`O_CLOEXEC` retained descriptor, rejects special files without
blocking, rechecks pathname and descriptor metadata before and after reading,
and requires two reads of that same descriptor to agree. A final symlink swap,
even when it targets a same-inode hardlink, or an in-place mutation therefore
fails rather than changing the authorized helper bytes. It does not depend on
Python-version-specific `ast.dump()` or `ast.unparse()`. Behavior tests still
execute every result and attestation branch, and Python children launched
directly by the security contract use that test process's `sys.executable`.
Canonical manifest input measurement applies the same retained-descriptor
boundary to every self-declared input, not only the two protected helpers. Its
self-including lock, helper hashes, action builder identity, runner rollout
authority, per-input digest, and descriptor size all come from the one
path-to-record measurement map.

This is a protected two-stage transition. The trusted `CI/**` change lands on
`main` first and validates the legacy contract there. The architecture pull
request then incorporates that trusted commit and removes its independent
protected-path delta; its complete marker set selects the policy/execution
contract. Once `main` and every maintained branch use only policy/execution, a
later trusted CI cleanup should remove the legacy profile and capability
switch.

## Plugin-Manager Suite Name Transition

While the plugin manager moves to the pure-C operation ABI,
`plugin_load_test.sh` uses the explicit positive filter
`PluginManagerLifecycleTest.*:PluginManagerPureCAbiTest.*`. GoogleTest treats
the colon-separated patterns as a union, and `run_gtest_checked` applies the
same filter to discovery and execution while rejecting an empty selection.
The legacy suite on `main`, the renamed suite on a migration branch, or both
suites when they coexist are therefore exercised without a broad glob that
could select unrelated tests. Branch names and commit identities never choose
the suite; the built test inventory is authoritative.

This is test-selection compatibility only. It does not add product or ABI
compatibility, and it does not claim that `main` has completed the pure-C ABI
migration. After `main` and every maintained branch expose only
`PluginManagerPureCAbiTest.*`, and the legacy suite is absent from every
supported build inventory, a later trusted `CI/**` cleanup should narrow the
filter to the new suite.

## Scripted CLI Capability Transition

`graph_cli_script_test.sh` selects the explicit-missing-source contract before
it starts any `graph_cli` process. The stable capability marker is the complete
long-lived Graph document error regression registration: the
`tests/integration/test_graph_document_errors.cpp` source, its
`add_ps_test(test_graph_document_errors ...)` target, and its
`gtest_discover_tests(test_graph_document_errors ...)` registration must all be
present. If all three are absent, the tested revision has the legacy
missing-source publication contract. If all three are present, it has the
transactional rejection contract. A partial marker is an inconsistent test
inventory and fails the script.

The marker is evaluated from the checked-out revision, not from a branch name,
commit identity, or observed CLI output. The legacy path therefore positively
requires the warning, published session, current-graph listing, and empty-graph
compute result while rejecting transactional output. The transactional path
requires the classified load failure, empty graph inventory, and absent current
graph while rejecting the legacy warning and publication. Invalid-target
parsing is checked in a separate runtime after loading a maintained fixture, so
it never relies on either missing-source state.

This is a two-stage protected-path transition. First, the `CI/**` change lands
on `main`, where the complete marker is absent and the legacy contract is
verified. Then the architecture pull request must incorporate this same script
unchanged and remove its independent `ci/**` delta; its complete Graph document
error registration selects the transactional contract. Until that second step,
the protected-path guard correctly continues to reject the architecture pull
request's CI-file delta.

After the transactional Graph document contract is present on `main` and every
active pull-request or maintained branch head tested by this protected script
contains the complete registration, a follow-up `CI/**` change must remove the
legacy path and capability switch. The script should then assert transactional
rejection unconditionally.

## Scripts

CI and CTest execute only long-lived software behavior, compile, package-
consumer, performance, concurrency, stability, error-handling, and runtime-
boundary checks. Migration-residue scans, phase-completion checks, stale-term
searches, Doxygen/source-quality audits, issue replay, and evidence/provenance
orchestration are excluded. Issue-specific replay, provenance, helper, and
output artifacts do not enter the primary repository and are not retained as
long-lived personal-overlay content. Explicitly documented general-purpose
manual developer tools are separate; a clean primary checkout never imports
personal development content.

- `ci/scripts/healthcheck.sh`: builds a NUL-delimited changed-path artifact, runs `git diff --check`, the durable change-classification, build-smoke inventory, runtime-capability, and CI-routing regressions, and `clang-format --dry-run --Werror` plus `cpplint` on every nondeleted changed C++ path; inventory failure terminates the script before a no-C++ summary.
- `ci/scripts/change_classification.sh`: classifies exact event revisions as documentation-only or full-integration, records all changed and non-documentation paths, and fails closed on Git uncertainty.
- `ci/scripts/change_classification_test.sh`: exercises the long-lived routing contract across documentation, source, mixed, type-change, workflow, rename, deletion, repeated `CI/**` push, pull-request merge-base, missing branch or revision, zero/unavailable revision, manual, empty-diff, and shallow-clone cases.
- `ci/scripts/ci_routing_test.sh`: whitespace-normalizes and exact-locks both production `protected-ci-paths.if` expressions, then extracts and executes the real stable-gate, pre-checkout fork-rejection, and protected-path shell blocks. It also locks the allow-empty configuration preflight, strict post-build job outputs, empty-output-safe `fromJSON` matrices, per-item artifact/name binding, full-CTest label exclusion, exact runner input, and the pairwise-disjoint four-way build-smoke routing: `ctest-control` consumers, OpenEXR metadata consumers, the dedicated installed-package static consumer, and the producer-local list. It verifies role-specific control/runtime/OpenEXR/installed artifact production, attestation, and consumption ordering; requires the complete shared suite gate; rejects a serial `integration_suite.sh` fallback; and retains the architecture-neutral `execution-repeat` job, environment, artifact, and final-gate routing. Isolated Git fixtures prove that both production guards reject a newline-containing `ci/**` path, safely record it, and fail closed on producer or reader failure. A job/step-scoped production assertion extracts each exact published-image history-fetch step and requires its own top-level `shell: bash`, so metadata on another job or neighboring step cannot satisfy the contract. Another job/step-scoped assertion requires exactly one `Trust checked-out workspace` step with `shell: bash`; its only executable lines must enable strict mode, add the exact `$GITHUB_WORKSPACE` global `safe.directory`, and verify `HEAD^{commit}`. It rejects an entry in another job or adjacent step, any additional or wildcard `safe.directory`, and placement after either fetch or `healthcheck.sh`. The extracted production trust block runs with an isolated HOME and Git repository, where the resulting global configuration must contain exactly that repository path. Job-scoped assertions separately lock the published-image and local-image pull-request exact-base fetch, `CI/**` main fetch/verification, three-way `CI_BASE_REF` route, and execution order. The test executes both extracted production main-fetch blocks; an isolated history proves that cumulative `origin/main` scope retains an early unformatted C++ path while event-before scope contains only the later documentation path. Detector fixtures retain exact/cumulative bases, empty comparisons, newline paths, and changed-path failure propagation. These local source and shell checks deliberately do not claim to execute GitHub's expression evaluator, reproduce cross-UID dubious ownership, or emulate the hosted container runner.
- `ci/scripts/ci_image_install.sh`: performs the only Docker image installation transaction. Its version/full-file SHA-256, verifier-owned active-statement identity, single entrypoint call, snapshot/APT/Pip/GitHub-CLI sequence, download authority, and hash-before-extract boundary are protected; it rejects an alternate APT path, extra downloader, pipe-to-shell command, bypassed hash, or early success.
- `ci/scripts/integration_suite_gate.py`: validates every exact shared-DAG conclusion plus the publish/attestation mode and image digest, then safely appends the sole validated digest output. Direct behavior regressions exercise every required job with failed/skipped/unknown conclusions and both legitimate attestation modes.
- `ci/scripts/runtime_capability_test.sh`: exercises exact Make/Ninja target parsing, both complete contracts, partial/mixed/absent fail-closed behavior, required-target checks, and mutually exclusive CLI configuration output. It also proves that the exact optional `test_plugin_trust_bundle` capability—not the broader policy/execution profile—gates direct-consumer trust export: pre-trust and legacy inventories are no-ops, missing or malformed inventories and incomplete/nonregular material fail closed, and a complete trust-enabled tuple replaces inherited values with canonical paths.
- `ci/scripts/ci_image_changed.sh`: delegates the exact base/head comparison to the canonical manifest helper, which strictly validates both revisions of the self-including `input_paths` lock, compares their union with the NUL-delimited unfiltered diff, and exits without a route output on lock or Git failure.
- `ci/scripts/build_smoke_inventory.py`: strictly parses CTest JSON v1, emits a deterministic matrix and NUL-delimited exact names, and revalidates one matrix selection before index-based execution. Strict post-build mode rejects an empty selection; only explicit preflight mode permits it. Its focused regression covers malformed JSON/schema, duplicate names/properties/label values, invalid or missing labels, disabled/commandless entries, empty strict selection, deterministic ordering, JSON round trips, safe artifact keys, hostile test-name characters, absent/disabled/commandless runner selections that stop before execution, and real configuration-placeholder-to-post-build discovery.
- `ci/scripts/integration_plan.sh`: configures a small testing-enabled tree and validates an allow-empty, non-authoritative configuration-time inventory preview; it emits no workflow matrix output.
- `ci/scripts/build_integrity.sh`: detects one complete runtime contract, runs required-target and complete builds in one tree, writes the ordinary CTest closure, installs a fresh package prefix, and partitions build smokes into the downstream `consumer_build_smoke_matrix.json` (`ctest-control`), `openexr_build_smoke_matrix.json` (`openexr-metadata`), and `dedicated_build_smoke_matrix.json` (`installed-package`) matrices plus the producer-local `producer_build_smoke_names.z` list. It exposes those three downstream matrices and executes the producer-local list only after strict validation.
- `ci/scripts/ctest_runtime_closure.py`: derives the recursive post-build ordinary CTest control/runtime closure and revalidates restored runtime inventory, executables, dynamic libraries, plugins, trust inputs, and build-tree data before execution.
- `ci/scripts/ctest_full.sh`: reuses the runtime role and runs ordinary CTest with the exact `build-smoke` label excluded, controlled `${CI_JOBS}` parallelism, failure output, and JUnit evidence.
- `ci/scripts/build_smoke_test.sh`: revalidates and runs one exact default-role CTest name from `ci-control-default`.
- `ci/scripts/openexr_smoke_test.sh`: runs the exact default OpenEXR option-off source-tree smoke from the verified cache-only metadata role.
- `ci/scripts/static_product_consumer_test.sh`: remeasures exact installed-package content before and after running the complete package consumer, without rebuilding or reinstalling the producer.
- `ci/scripts/targeted_artifact_consume.sh`: verifies archive and manifest attestations with candidate source digest and reusable-workflow signer digest, then verifies and atomically restores one exact role.
- `ci/scripts/graph_cli_script_test.sh`: runs isolated positive, explicit-missing-source, and invalid-target REPL checks using the pre-execution Graph document capability marker described above.
- `ci/scripts/propagation_script_test.sh`: builds `test_propagation` and runs `tiles all` on linear and complex propagation graphs.
- `ci/scripts/plugin_load_test.sh`: checks operation plugins and selects either scheduler plugin loading/listing or policy plugin, registry, policy/execution, and CLI route checks.
- `ci/scripts/execution_repeat_test.sh`: repeats the configured runtime contract's deterministic scheduler or policy/execution behavior tests.
- `ci/scripts/sanitizer_test.sh`: consumes the one retained runner identity produced before profile input, then runs shared and capability-selected focused ASan or TSan tests from an isolated build directory; its temporary fallback transports target/empty-filter/trust records only through the terminal NUL-framed v1 protocol and records the decoded evidence.

## Local Commands

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
# Security runners are reproducible only on an approved hosted runner.
runner_identity="${RUNNER_TEMP:?}/photospider-security-runner-$$.json"
python3 ci/scripts/ci_runner_verify.py --platform Linux \
  --runner-label ubuntu-24.04 --output "$runner_identity"
CI_RUNNER_IDENTITY_FILE="$runner_identity" SANITIZER=asan \
  CI_ARTIFACT_DIR=CI-results/sanitizer-asan \
  bash ci/scripts/sanitizer_test.sh
```

The sanitizer/fuzz commands intentionally have no unverified workstation
fallback. `ci_runner_verify.py` requires the hosted `ImageOS`/`ImageVersion`,
writes a new retained file once, and every downstream platform preparation step
consumes that same path.

Replace `SMOKE_TEST_NAME` with any exact name emitted by
`CI-results/build-integrity-default/consumer_build_smoke_matrix.json`; the runner refuses an
absent, duplicate, disabled, commandless, or unlabelled selection. To run all
labelled smokes directly from a configured tree, use
`ctest --test-dir build/ci-default -L '^build-smoke$' --output-on-failure`.
There is no serial local command equivalent to the shared reusable DAG. Use the
focused role commands above for local diagnosis; build-once digest fan-out,
attestation, aggregation, and same-digest promotion are GitHub Actions gates.

Docker reproduction separates local layer solving from protected publication
provenance. The following local command is intentionally not publish-eligible:

```bash
# Local layer-solver reproduction only; never publish or attest this image.
local_ci_identity=local-layer-solver-only-not-publishable
docker build --no-cache -t photospider-ci:local -f Dockerfile.ci \
  --build-arg CI_IMAGE_INPUT_MANIFEST_SHA256="$local_ci_identity" \
  --build-arg CI_IMAGE_SOURCE_COMMIT="$local_ci_identity" .
docker run --rm -v "$PWD:/workspace" -w /workspace photospider-ci:local \
  bash ci/scripts/build_integrity.sh
```

Only an approved `ubuntu-24.04` GitHub-hosted builder may construct the manifest
used for publication. In that protected job, the genuine retained builder
identity and source equality are obtained as follows:

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

The local marker values above exercise Docker layers and the snapshot solver but
are not a canonical manifest, OCI provenance, or permission to publish. They
must never be passed to promotion or attestation. The protected producer uses
the second block's exact manifest/source values as immutable Buildx inputs and
still applies all workflow, attestation, and digest checks.

The remaining Docker build arguments retain their immutable protected defaults
from `ci/locks/ci-image-lock.json`. Local mirror overrides are not part of the
maintained image contract. The OpenSSL/CA offline bootstrap URLs and SHA-256
values are locked there, both exact versions also occur in the Ubuntu package
lock, and the protected Deb822 template replaces all base archive/security/ports
sources with the exact snapshot URI before APT runs. The same source serves
native amd64 and arm64, and every APT update/install stays inside it. The build
itself is the dependency-solver regression: an unavailable direct or transitive
version fails before the image can be published. Pip separately uses the
hash-locked requirements file. `Dockerfile.ci` has one exact active instruction
stream and invokes only `bash /tmp/ci-image-install.sh`; the helper is copied
from a canonical manifest input and is bound in `ci-image-lock.json` by role,
version, and full-file SHA-256. A separate verifier-owned active-statement
identity and network/install allowlist prevent a helper plus its JSON hash from
being changed together to admit `/usr/bin/apt-get`, an additional download,
`curl | sh`, a skipped GitHub CLI checksum, an uncalled entrypoint, or early
success. The only non-APT download is the exact GitHub CLI release URL whose
architecture-specific hash is checked before extraction and installation.

The local Docker commands above reproduce the maintained current-toolchain layer
and build paths without publication provenance. They are not a claim that CMake
3.16 itself ran. If targeted old-version
evidence is needed and no natively compatible executable exists locally, record
that limitation rather than using architecture emulation to manufacture a
minimum-version PASS.

## Local Artifact Download

Use the personal-overlay script to download GitHub Actions artifacts:

```bash
.codex/skills/personal-overlay-git/scripts/download_ci_results.sh --workflow "CI Integration"
```

It writes to `CI-results/`, which is personal-overlay content and must not be committed to the primary GitHub repository.
