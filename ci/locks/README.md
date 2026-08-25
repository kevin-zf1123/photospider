# Protected CI input locks

These files are trusted control inputs. CI readers reject malformed, duplicate,
unknown, stale, or mismatched records rather than resolving a mutable fallback.
The temporary `current-main-profiles-v1.json` lock is accepted only when both
candidate-owned versioned manifests are absent and every listed source hash
still matches. It must be removed by the protected cleanup after matrix adoption.

The values were refreshed on 2026-08-25 from authoritative upstream services:

- GitHub action release refs were resolved with `git ls-remote` against each
  action's official GitHub repository and confirmed through the GitHub API.
- `ubuntu:24.04` was resolved through the official Docker Hub registry. The
  locked digest is the multi-platform OCI index, not a mutable tag or one local
  architecture's child manifest.
- Ubuntu packages were selected from Canonical's signed immutable snapshot
  `20260825T000000Z` at `https://snapshot.ubuntu.com/ubuntu/`. Exact top-level
  versions are installed with APT snapshot mode, so transitive resolution is
  constrained by that signed snapshot.
- Python hashes are the official PyPI release-file SHA-256 values for
  `clang-format==21.1.5` (Linux x86-64 and AArch64 wheels) and
  `cpplint==2.0.0` (universal wheel).
- GitHub CLI `2.98.0` archive hashes are from the official release
  `gh_2.98.0_checksums.txt`. The CLI is needed to verify GitHub artifact and OCI
  attestations; Ubuntu's older package does not provide `gh attestation`.
- The Darwin host lock records the observed `macos-15-arm64` runner image
  version `20260727.0256.1` published by `actions/runner-images`, binds the
  workflow label `macos-15` to its `arm64` architecture and `arm64-osx`
  triplet, and resolves its documented vcpkg commit prefix to the full commit
  in the official `microsoft/vcpkg` repository.
  Security jobs reject runner-image or registry drift and disable binary/asset
  caches so the locked port registry's source hashes remain authoritative.

Any lock refresh is a protected CI change. Run `python3 ci/scripts/ci_lock_verify.py`
and the durable security contract tests before accepting it.
