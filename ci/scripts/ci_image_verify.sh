#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_image_verify.sh
# @brief Resolve a locator tag, verify GitHub attestation and canonical input
#   labels, then emit the exact digest-qualified OCI image for candidate jobs.
# @note Run only in a trusted host job after protected checkout. The mutable tag
#   is merely a discovery locator; no candidate command runs until its resolved
#   digest has passed source-workflow, source-commit, manifest, and label checks.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
OUTPUT_FILE=${CI_IMAGE_OUTPUT_FILE:-${GITHUB_OUTPUT:-}}
ARTIFACT_DIR=${CI_ARTIFACT_DIR:-$REPO_ROOT/CI-results/ci-image-identity}
EXPECTED_REPOSITORY=${CI_IMAGE_REPOSITORY:-${GITHUB_REPOSITORY:-}}

# @brief Print command usage for direct developer and workflow callers.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: CI_IMAGE_REPOSITORY=owner/name ci/scripts/ci_image_verify.sh

Required tools: git, docker with buildx, gh 2.49 or newer, and python3.
Required authentication: GH_TOKEN/GITHUB_TOKEN with package read access.
The exact image, digest, source commit, and manifest digest are appended to
CI_IMAGE_OUTPUT_FILE (or GITHUB_OUTPUT) when provided.
EOF
}

if [[ ${1:-} == --help ]]; then
  usage
  exit 0
fi
if (($# != 0)); then
  usage >&2
  exit 2
fi

for command_name in git docker gh python3; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required CI image verifier command is unavailable: $command_name" >&2
    exit 1
  fi
done
if [[ ! "$EXPECTED_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
  echo "CI_IMAGE_REPOSITORY must be an exact owner/name identity." >&2
  exit 2
fi

mkdir -p "$ARTIFACT_DIR"
python3 "$SCRIPT_DIR/ci_runner_verify.py" --platform Linux \
  > "$ARTIFACT_DIR/linux-runner-identity.json"
manifest_path=$ARTIFACT_DIR/expected-ci-image-input-v1.json
manifest_digest_path=$ARTIFACT_DIR/expected-ci-image-input-v1.sha256
labels_path=$ARTIFACT_DIR/oci-labels.json
attestation_log=$ARTIFACT_DIR/attestation-verification.json

# @brief Read the strict published-image fields without requiring jq.
# @return Three newline-delimited values: locator, workflow path, repository.
# @throws Python exits nonzero for malformed or unexpected protected locks.
read_published_lock() {
  python3 - "$REPO_ROOT/ci/locks/ci-image-lock.json" <<'PY'
import json
import re
import sys

path = sys.argv[1]

def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member: {key}")
        result[key] = value
    return result

with open(path, encoding="utf-8") as handle:
    lock = json.load(handle, object_pairs_hook=unique)
published = lock.get("published_image")
expected = {
    "locator", "source_repository", "source_workflow",
    "input_manifest_label", "source_commit_label",
}
if not isinstance(published, dict) or set(published) != expected:
    raise SystemExit("published image lock is malformed")
if not re.fullmatch(r"ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+:[A-Za-z0-9_.-]+", published["locator"]):
    raise SystemExit("published image locator is malformed")
print(published["locator"])
print(published["source_workflow"])
print(published["source_repository"])
PY
}

mapfile -t published_lock < <(read_published_lock)
if ((${#published_lock[@]} != 3)); then
  echo "Published image lock did not yield exactly three identity fields." >&2
  exit 1
fi
locator=${published_lock[0]}
source_workflow=${published_lock[1]}
locked_repository=${published_lock[2]}
if [[ "$EXPECTED_REPOSITORY" != "$locked_repository" ]]; then
  echo "Requested repository '$EXPECTED_REPOSITORY' differs from protected '$locked_repository'." >&2
  exit 1
fi

source_commit=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" source-commit)
manifest_digest=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" create \
  --source-commit "$source_commit" \
  --repository "$EXPECTED_REPOSITORY" \
  --output "$manifest_path" \
  --digest-output "$manifest_digest_path")

inspect_output=$(docker buildx imagetools inspect "$locator")
mapfile -t image_digests < <(
  awk '$1 == "Digest:" { print $2 }' <<<"$inspect_output" | sort -u
)
if ((${#image_digests[@]} != 1)) ||
  [[ ! ${image_digests[0]} =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo "Locator did not resolve to exactly one canonical OCI digest." >&2
  exit 1
fi
image_digest=${image_digests[0]}
image_repository=${locator%:*}
exact_image=$image_repository@$image_digest

gh attestation verify "oci://$exact_image" \
  --repo "$EXPECTED_REPOSITORY" \
  --signer-workflow "$EXPECTED_REPOSITORY/$source_workflow" \
  --source-digest "$source_commit" \
  --deny-self-hosted-runners \
  --format json > "$attestation_log"

docker pull "$exact_image" >/dev/null
docker image inspect --format '{{json .Config.Labels}}' "$exact_image" > "$labels_path"
verified_manifest_digest=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" verify-labels \
  --manifest "$manifest_path" \
  --labels-json "$labels_path" \
  --source-commit "$source_commit")
if [[ "$verified_manifest_digest" != "$manifest_digest" ]]; then
  echo "OCI label verification returned an unexpected manifest digest." >&2
  exit 1
fi

{
  printf 'image=%s\n' "$exact_image"
  printf 'digest=%s\n' "$image_digest"
  printf 'source_commit=%s\n' "$source_commit"
  printf 'manifest_digest=%s\n' "$manifest_digest"
} | tee "$ARTIFACT_DIR/ci-image-identity.env"
if [[ -n "$OUTPUT_FILE" ]]; then
  cat "$ARTIFACT_DIR/ci-image-identity.env" >> "$OUTPUT_FILE"
fi
