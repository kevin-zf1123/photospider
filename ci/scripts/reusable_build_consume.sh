#!/usr/bin/env bash

set -Eeuo pipefail

# @file reusable_build_consume.sh
# @brief Verify GitHub attestations and exact reusable-build identities before
#   traversal-safe extraction into a downstream build directory.
# @note The workflow supplies every expected value from trusted job outputs and
#   GitHub contexts. Missing values, an untrusted signer/source, or any manifest,
#   archive, profile, matrix, image, or commit mismatch fails before extraction.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
REUSABLE_DIR=${CI_REUSABLE_DIR:-$REPO_ROOT/CI-results/reusable-build}
ARCHIVE=$REUSABLE_DIR/ci-build.tar.gz
MANIFEST=$REUSABLE_DIR/ci-build.manifest.json
DESTINATION=${CI_REUSABLE_DESTINATION:-$REPO_ROOT/build}

# @brief Require one environment identity to be present.
# @param $1 Variable name.
# @return Zero when nonempty, otherwise two with a diagnostic.
require_environment() {
  local variable_name=$1
  if [[ -z ${!variable_name:-} ]]; then
    echo "Required reusable-build identity is unset: $variable_name" >&2
    return 2
  fi
}

for variable_name in \
  CI_CANDIDATE_COMMIT \
  CI_BUILD_PROFILE \
  CI_MATRIX_SHA256 \
  CI_IMAGE_DIGEST \
  CI_WORKFLOW_COMMIT \
  GITHUB_REPOSITORY; do
  require_environment "$variable_name"
done
for command_name in gh python3; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required reusable-build verifier command is unavailable: $command_name" >&2
    exit 1
  fi
done
for subject in "$ARCHIVE" "$MANIFEST"; do
  if [[ ! -f "$subject" || -L "$subject" ]]; then
    echo "Reusable-build subject is missing or not a regular file: $subject" >&2
    exit 1
  fi
  gh attestation verify "$subject" \
    --repo "$GITHUB_REPOSITORY" \
    --signer-workflow "$GITHUB_REPOSITORY/.github/workflows/ci-integration.yml" \
    --source-digest "$CI_WORKFLOW_COMMIT" \
    --deny-self-hosted-runners \
    --format json > "$subject.attestation.json"
done

python3 "$SCRIPT_DIR/reusable_build.py" verify-extract \
  --archive "$ARCHIVE" \
  --manifest "$MANIFEST" \
  --candidate-commit "$CI_CANDIDATE_COMMIT" \
  --profile "$CI_BUILD_PROFILE" \
  --matrix-sha256 "$CI_MATRIX_SHA256" \
  --image-digest "$CI_IMAGE_DIGEST" \
  --workflow-commit "$CI_WORKFLOW_COMMIT" \
  --destination "$DESTINATION"
