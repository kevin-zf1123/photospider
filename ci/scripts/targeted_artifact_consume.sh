#!/usr/bin/env bash

set -Eeuo pipefail

# @file targeted_artifact_consume.sh
# @brief Verify one role-specific CI artifact and atomically restore only its
#   declared runtime, CTest-control, OpenEXR metadata, or installed-package
#   content.
# @note Trusted pushes require GitHub attestations for both archive and manifest.
#   GitHub source identity binds the tested candidate commit while signer
#   identity binds the protected reusable-workflow commit; these two digests
#   are deliberately independent and both are required.
#   Pull-request and pull_request_target callers are deliberately read-only and
#   therefore perform the same cryptographic manifest/content verification
#   without publishing or requiring a new remote attestation for that run.
#   The manifest is copied from one retained no-follow snapshot. GitHub CLI and
#   Python consume that private copy, and its exact digest is passed through the
#   GitHub environment boundary to the installed-package behavior step.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
ROLE=${CI_ARTIFACT_ROLE:-}
ARTIFACT_DIR=${CI_TARGETED_ARTIFACT_DIR:-$REPO_ROOT/CI-results/targeted-artifact}
ARCHIVE=${CI_TARGETED_ARCHIVE:-$ARTIFACT_DIR/$ROLE.tar.gz}
MANIFEST=${CI_TARGETED_MANIFEST:-$ARTIFACT_DIR/$ROLE.manifest.json}
DESTINATION=${CI_TARGETED_DESTINATION:-$REPO_ROOT/build}
REQUIRE_REMOTE_ATTESTATION=${CI_REQUIRE_REMOTE_ATTESTATION:-true}
SNAPSHOT_PARENT=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
MANIFEST_SNAPSHOT_ROOT=

# @brief Remove only this invocation's private manifest snapshot.
# @return Zero after cleanup, or the first exact removal failure.
# @throws Nothing; cleanup status is returned through the EXIT trap.
# @note Neither the downloaded subjects nor the restored role tree are owned by
#   this trap. A replaced snapshot-root symlink is unlinked, not followed.
cleanup_targeted_manifest_snapshot() {
  local status=$?
  if [[ -n "$MANIFEST_SNAPSHOT_ROOT" &&
    (-e "$MANIFEST_SNAPSHOT_ROOT" || -L "$MANIFEST_SNAPSHOT_ROOT") ]]; then
    rm -rf -- "$MANIFEST_SNAPSHOT_ROOT" || status=$?
  fi
  return "$status"
}
trap cleanup_targeted_manifest_snapshot EXIT

# @brief Require one named environment identity to be nonempty.
# @param $1 Variable name.
# @return Zero for a nonempty value, otherwise two.
require_environment() {
  local variable_name=$1
  if [[ -z ${!variable_name:-} ]]; then
    echo "Required targeted-artifact identity is unset: $variable_name" >&2
    return 2
  fi
}

for variable_name in \
  CI_ARTIFACT_ROLE \
  CI_BUILD_PROFILE \
  CI_CANDIDATE_COMMIT \
  CI_IMAGE_DIGEST \
  CI_MATRIX_SHA256 \
  CI_WORKFLOW_COMMIT \
  GITHUB_REPOSITORY; do
  require_environment "$variable_name"
done
case "$ROLE" in
  ctest-control | ctest-runtime | installed-package | openexr-metadata) ;;
  *)
    echo "Unsupported targeted artifact role: $ROLE" >&2
    exit 2
    ;;
esac
case "$REQUIRE_REMOTE_ATTESTATION" in
  true | false) ;;
  *)
    echo "CI_REQUIRE_REMOTE_ATTESTATION must be literal true or false." >&2
    exit 2
    ;;
esac
for subject in "$ARCHIVE" "$MANIFEST"; do
  if [[ ! -f "$subject" || -L "$subject" ]]; then
    echo "Targeted artifact subject is missing or unsafe: $subject" >&2
    exit 1
  fi
done
if [[ ! -d "$SNAPSHOT_PARENT" || -L "$SNAPSHOT_PARENT" ]]; then
  echo "Targeted manifest snapshot parent is missing or linked." >&2
  exit 1
fi
MANIFEST_SNAPSHOT_ROOT=$(mktemp -d \
  "$SNAPSHOT_PARENT/photospider-targeted-manifest.XXXXXX")
readonly MANIFEST_SNAPSHOT_ROOT
MANIFEST_SNAPSHOT="$MANIFEST_SNAPSHOT_ROOT/$ROLE.manifest.json"
manifest_snapshot_digest=$(
  python3 "$SCRIPT_DIR/reusable_build.py" snapshot-targeted-manifest \
    --manifest "$MANIFEST" \
    --snapshot "$MANIFEST_SNAPSHOT"
)
if [[ ! "$manifest_snapshot_digest" =~ ^[0-9a-f]{64}$ ]]; then
  echo "Targeted manifest snapshot did not produce one SHA-256 digest." >&2
  exit 1
fi
readonly MANIFEST_SNAPSHOT
readonly manifest_snapshot_digest

# @brief Verify one exact artifact subject and persist compact GitHub evidence.
# @param $1 Subject pathname whose bytes GitHub CLI must attest.
# @param $2 Job-owned evidence output kept outside the private snapshot root.
# @return Zero only when repository, signer workflow, source commit, signer
#   commit, and hosted-runner policy all match.
# @throws Nothing; GitHub CLI status is returned to strict shell mode.
verify_remote_attestation() {
  local subject=$1
  local evidence=$2
  gh attestation verify "$subject" \
    --repo "$GITHUB_REPOSITORY" \
    --signer-workflow \
      "$GITHUB_REPOSITORY/.github/workflows/ci-integration-suite.yml" \
    --source-digest "$CI_CANDIDATE_COMMIT" \
    --signer-digest "$CI_WORKFLOW_COMMIT" \
    --deny-self-hosted-runners \
    --format json > "$evidence"
}

if [[ "$REQUIRE_REMOTE_ATTESTATION" == true ]]; then
  if ! command -v gh >/dev/null 2>&1; then
    echo "GitHub CLI is required for targeted artifact attestation." >&2
    exit 1
  fi
  verify_remote_attestation "$ARCHIVE" "$ARCHIVE.attestation.json"
  verify_remote_attestation \
    "$MANIFEST_SNAPSHOT" "$MANIFEST.attestation.json"
fi

python3 "$SCRIPT_DIR/reusable_build.py" verify-targeted-extract \
  --archive "$ARCHIVE" \
  --manifest "$MANIFEST_SNAPSHOT" \
  --expected-manifest-sha256 "$manifest_snapshot_digest" \
  --role "$ROLE" \
  --candidate-commit "$CI_CANDIDATE_COMMIT" \
  --profile "$CI_BUILD_PROFILE" \
  --matrix-sha256 "$CI_MATRIX_SHA256" \
  --image-digest "$CI_IMAGE_DIGEST" \
  --workflow-commit "$CI_WORKFLOW_COMMIT" \
  --destination "$DESTINATION"

if [[ "$ROLE" == installed-package ]]; then
  if [[ -z ${GITHUB_ENV:-} || ! -f "$GITHUB_ENV" || -L "$GITHUB_ENV" ]]; then
    echo "Installed-package consumption requires a regular GitHub environment file." >&2
    exit 1
  fi
  printf 'CI_STATIC_PACKAGE_MANIFEST_SHA256=%s\n' \
    "$manifest_snapshot_digest" >> "$GITHUB_ENV"
fi
