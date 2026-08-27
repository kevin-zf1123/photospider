# Protected CI input locks

These files are trusted control inputs. CI readers reject malformed, duplicate,
unknown, stale, or mismatched records rather than resolving a mutable fallback.
The temporary `current-main-profiles-v1.json` lock is accepted only when both
candidate-owned versioned manifests are absent and every listed source hash
still matches. It must be removed by the protected cleanup after matrix adoption.

The dependency/image values were refreshed on 2026-08-25 and the hosted-runner
identities on 2026-08-27 from authoritative upstream services:

- GitHub action release refs were resolved with `git ls-remote` against each
  action's official GitHub repository and confirmed through the GitHub API.
- `ubuntu:24.04` was resolved through the official Docker Hub registry. The
  locked digest is the multi-platform OCI index, not a mutable tag or one local
  architecture's child manifest.
- Ubuntu packages were selected from Canonical's signed immutable snapshot
  `20260825T000000Z` at `https://snapshot.ubuntu.com/ubuntu/`. Exact top-level
  versions are installed through the protected
  `ubuntu-24.04-snapshot.sources.in` Deb822 template. Before the first APT
  command, Docker replaces the locked base image's archive, security, and ports
  sources with the template's timestamp-qualified snapshot URI. The same signed
  archive serves the native amd64 and arm64 indices, so neither architecture can
  fall back to a live host. Because the minimal base image has no TLS trust
  bundle, `openssl=3.0.13-0ubuntu3.12` and
  `ca-certificates=20260601~24.04.1` form the complete offline bootstrap. Their
  architecture-specific snapshot URLs and SHA-256 values are locked in
  `ci-image-lock.json`; `dpkg` configures those verified bytes before the first
  APT command. The first and only APT update/install sequence consumes only that
  explicit snapshot source, and the main package transaction consumes the same
  two exact versions again. Lock rows use Debian's package-name grammar with a
  minimum two-byte name, and the installer places apt's `--` terminator after
  all fixed options and before every locked `name=version` argument. An
  option-shaped row therefore fails validation and cannot become an APT flag.
- `ci-image-lock.json` also binds the version, path, and full-file SHA-256 of
  `ci_image_install.sh` and `integration_suite_gate.py`. Both paths remain in
  the canonical image-input manifest. The installer owns the complete image
  network/install transaction: Docker has one exact helper invocation, while a
  verifier-owned active-statement identity and explicit command allowlist reject
  extra APT aliases, downloads, pipe-to-shell paths, bypassed hashes, and early
  exit. The suite-gate helper instead requires three-way equality between a
  verifier-owned exact source-byte SHA-256, the protected JSON helper hash, and
  the retained regular-file measurement. This identity is independent of Python
  AST serialization changes; exhaustive behavior tests still execute every
  required result and attestation publish/skip mode, and updating a JSON helper
  hash alone cannot authorize source drift.
- The callable image producer is decoded through the restricted YAML parser and
  compared as one complete mapping: its sole `workflow_call`, write permissions,
  only build job, ordered steps, environments, commands, outputs, and every
  action `with` field are exact. Checkout and `ci_lock_verify.py` must precede
  the unique Buildx build/push action; only the manifest and source-commit build
  arguments and the event-scoped temporary tag are admitted. The Dockerfile
  parser separately models BuildKit's BOM removal and first-line shebang removal
  before frontend detection. A UTF-8 BOM and shebang are themselves forbidden.
  Hash/C-style directive markers must start at byte zero; after that marker the
  verifier trims exactly Go `unicode.IsSpace` (the Unicode White_Space set), not
  Python's wider control-character set. Every active `syntax` frontend in those
  comment forms or JSON is rejected, including case, Unicode whitespace, tag,
  digest, and shebang-hidden variants, before the full instruction stream is
  compared. Marker-leading space/tab remains a non-active ordinary comment.
  Frontend detection and active logical-instruction parsing share one exact Go
  `bufio.ScanLines` model: only LF separates tokens and one terminal CR is
  removed. CR-only, VT/FF, FS/GS/RS, NEL, and Unicode line/paragraph separators
  therefore cannot reveal a new instruction after an ordinary comment.
- Python hashes are the official PyPI release-file SHA-256 values for
  `clang-format==21.1.5` (Linux x86-64 and AArch64 wheels) and
  `cpplint==2.0.0` (universal wheel).
- GitHub CLI `2.98.0` archive hashes are from the official release
  `gh_2.98.0_checksums.txt`. The CLI is needed to verify GitHub artifact and OCI
  attestations; Ubuntu's older package does not provide `gh attestation`.
  `published_image.attestation_fetch_limit` fixes the reviewed finite fetch
  window as the exact JSON integer 30; boolean, float, string, or any other
  integer fails the prebuild lock verifier. GitHub CLI 2.98 applies its
  download predicate filter after the API limit, so published discovery does
  not pass `--predicate-type` while fetching. It first downloads every
  exact-subject, repository-scoped predicate bundle, rejects a raw count of 30
  as possibly truncated, snapshots the unsaturated bytes, and applies the SLSA
  predicate, signer workflow, and hosted-runner policy only while verifying
  that same snapshot offline. The shorter filtered or verified result is never
  treated as fetch-count evidence. Known producers retain exact
  `--source-digest` and `--signer-digest` constraints.
- Every Git command used for image source, complete-DAG ancestry, attestation
  source selection, or promotion freshness passes through one protected
  process boundary. It removes caller-supplied `GIT_*` authority, forces
  `--no-replace-objects`, and rejects canonical `refs/replace/**`, legacy
  `.git/info/grafts`, or common-directory drift before semantic output is
  consumed.
- GitHub documents a two-to-three-day image deployment window and directs exact
  job diagnosis to `Set up job`. The finite Linux rollout set records stable
  `ubuntu24/20260816.277.1`, observed in exact-head runs `32997831039` and
  `32997831190`, plus rollout `ubuntu24/20260823.283.1`, observed in run
  [`32991073228`](https://github.com/kevin-zf1123/photospider/actions/runs/32991073228/job/98248727299).
  The official runner-images records are
  [`ubuntu24/20260816.277`](https://github.com/actions/runner-images/releases/tag/ubuntu24%2F20260816.277)
  and
  [`ubuntu24/20260823.283`](https://github.com/actions/runner-images/releases/tag/ubuntu24%2F20260823.283).
  The allowlist bytes are a canonical image input; the resolved runtime record,
  manifest `builder_runner`, and OCI builder label bind the actual build member.
  Linux ASan, TSan, bounded-fuzz, and manual sanitizer jobs measure that record
  on the real host from an exact protected sparse checkout before candidate
  checkout. They then pull the exact digest and use the protected host wrapper
  to mount candidate source, protected `ci`, profile inventory, and the same
  identity read-only into a network-disabled container. No registry credential
  enters that container.
- The finite Darwin set binds `macos-15`/`arm64`/`arm64-osx`. Stable
  `20260727.0256.1` maps to full vcpkg commit
  [`6d9d7df564a1ccdaa994e4ad39ccd4a32360867b`](https://github.com/microsoft/vcpkg/commit/6d9d7df564a1ccdaa994e4ad39ccd4a32360867b),
  while rollout `20260824.0311.1` maps to
  [`127402f1c75bb3d5ff6bce04b285faa4930a5aca`](https://github.com/microsoft/vcpkg/commit/127402f1c75bb3d5ff6bce04b285faa4930a5aca).
  Their official image records are
  [`macos-15-arm64/20260727.0256`](https://github.com/actions/runner-images/releases/tag/macos-15-arm64%2F20260727.0256)
  and
  [`macos-15-arm64/20260824.0311`](https://github.com/actions/runner-images/releases/tag/macos-15-arm64%2F20260824.0311).
  Both release records include Android SDK CMake `3.31.5` and Homebrew LLVM
  `18.1.8`. The lock binds the exact CMake executable, the official 3.31.5
  `Modules/GoogleTest.cmake` SHA-256
  `b5a2546c8cea1d5f9a366c6983261c621c0b34a40d8494caefdc0fa4c78862c4`,
  and the exact LLVM C/C++ paths. The
  [official Android repository metadata](https://dl.google.com/android/repository/repository2-1.xml)
  binds `cmake-3.31.5-darwin.zip` to SHA-1
  `4c28c635f08638494ce563ffa7d26d1c43a50190`; the module hash above was
  measured from that exact archive. Platform preparation verifies the real
  version/module bytes; fuzz also builds and executes a bounded combined
  libFuzzer/ASan/UBSan probe before configure. CMake 3.31.5 is the protected
  compatibility handoff for current-main's counter-suffix inventory helper;
  direct hash-suffix support remains an unchecked candidate-owned task.
  Each Darwin profile first retains its real host identity from an exact
  protected `workflow_commit` sparse checkout, then admits candidate source in
  a disjoint HEAD-bound data root. The protected Darwin wrapper executes every
  profile/parser/platform helper from control and rejects candidate helper
  selection, overlap, replacement, dirty state, or HEAD/root drift.
  Security jobs use the preinstalled tree only as an image-bound binary and
  locked Git-object source. Each profile creates an unseedable checkout below
  `runner.temp` (fetching the same exact commit from the official repository if
  the local object is absent), verifies its commit, clean state, and real-file
  tree, then disables binary/asset caches so registry source hashes remain
  authoritative.

Any lock refresh is a protected CI change. Run `python3 ci/scripts/ci_lock_verify.py`
and the durable security contract tests before accepting it.
