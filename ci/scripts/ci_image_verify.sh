#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_image_verify.sh
# @brief Resolve a protected or explicitly supplied locator, verify GitHub
#   attestation before any image-layer pull, then validate canonical input
#   labels and retained builder runtime before emitting the exact
#   digest-qualified OCI image for candidate jobs.
# @note Run only in a trusted host job after protected checkout. The mutable tag
#   is merely a discovery locator; no candidate command runs until its resolved
#   digest has passed source-workflow, source-commit, manifest, and label checks.
# @note ``CI_IMAGE_LOCATOR`` is reserved for a protected caller and must name
#   either an exact temporary tag or digest-qualified reference in the locked
#   repository. Optional expected digest, manifest, source, and workflow values
#   independently bind reusable-workflow inputs to registry and checkout state.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
OUTPUT_FILE=${CI_IMAGE_OUTPUT_FILE:-${GITHUB_OUTPUT:-}}
ARTIFACT_DIR=${CI_ARTIFACT_DIR:-$REPO_ROOT/CI-results/ci-image-identity}
EXPECTED_REPOSITORY=${CI_IMAGE_REPOSITORY:-${GITHUB_REPOSITORY:-}}
LOCATOR_OVERRIDE=${CI_IMAGE_LOCATOR:-}
EXPECTED_DIGEST=${CI_IMAGE_EXPECTED_DIGEST:-}
EXPECTED_MANIFEST_DIGEST=${CI_IMAGE_EXPECTED_MANIFEST_DIGEST:-}
EXPECTED_SOURCE_COMMIT=${CI_IMAGE_EXPECTED_SOURCE_COMMIT:-}
EXPECTED_WORKFLOW_COMMIT=${CI_IMAGE_EXPECTED_WORKFLOW_COMMIT:-}

# @brief Print command usage for direct developer and workflow callers.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: CI_IMAGE_REPOSITORY=owner/name ci/scripts/ci_image_verify.sh

Required tools: git, docker with buildx, gh 2.49 or newer, and python3.
Required authentication: GH_TOKEN/GITHUB_TOKEN with package read access.
Protected callers may set CI_IMAGE_LOCATOR to an exact temporary tag or
digest-qualified reference in the locked repository. CI_IMAGE_EXPECTED_DIGEST,
CI_IMAGE_EXPECTED_MANIFEST_DIGEST, CI_IMAGE_EXPECTED_SOURCE_COMMIT, and
CI_IMAGE_EXPECTED_WORKFLOW_COMMIT bind caller fields to measured state.
The exact image, digest, source commit, manifest digest, and builder image
version are appended to CI_IMAGE_OUTPUT_FILE (or GITHUB_OUTPUT) when provided.
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
if [[ -n "$EXPECTED_DIGEST" && ! "$EXPECTED_DIGEST" =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo "CI_IMAGE_EXPECTED_DIGEST must be one canonical SHA-256 digest." >&2
  exit 2
fi
if [[ -n "$EXPECTED_MANIFEST_DIGEST" &&
  ! "$EXPECTED_MANIFEST_DIGEST" =~ ^[0-9a-f]{64}$ ]]; then
  echo "CI_IMAGE_EXPECTED_MANIFEST_DIGEST must be one lowercase SHA-256 value." >&2
  exit 2
fi
for expected_commit_name in \
  CI_IMAGE_EXPECTED_SOURCE_COMMIT \
  CI_IMAGE_EXPECTED_WORKFLOW_COMMIT; do
  expected_commit=${!expected_commit_name:-}
  if [[ -n "$expected_commit" && ! "$expected_commit" =~ ^[0-9a-f]{40}$ ]]; then
    echo "$expected_commit_name must be one lowercase full Git SHA." >&2
    exit 2
  fi
done
if [[ -n "$EXPECTED_WORKFLOW_COMMIT" ]]; then
  actual_workflow_commit=$(git -C "$REPO_ROOT" rev-parse --verify 'HEAD^{commit}')
  if [[ "$actual_workflow_commit" != "$EXPECTED_WORKFLOW_COMMIT" ]]; then
    echo "Protected checkout differs from CI_IMAGE_EXPECTED_WORKFLOW_COMMIT." >&2
    exit 1
  fi
fi

# @brief Reject residual formal evidence before remote attestation begins.
# @return Zero only when the final artifact pathname is completely absent.
# @throws Nothing; a regular directory, link, dangling link, or other residual
#   final entry returns nonzero before network or identity work.
# @note This preflight creates nothing. A second atomic mkdir after successful
#   attestation closes the check/create race for the workflow-owned final name.
require_fresh_artifact_path() {
  if [[ -e "$ARTIFACT_DIR" || -L "$ARTIFACT_DIR" ]]; then
    echo "CI image identity artifact path must be fresh: $ARTIFACT_DIR" >&2
    return 1
  fi
}

# @brief Create the formal artifact directory only after attestation succeeds.
# @return Zero after creating one fresh non-link mode-0700 directory.
# @throws Nothing; an aliased parent, residual final path, or mkdir/type failure
#   returns nonzero before retained identities or layer pulls are produced.
prepare_artifact_directory() {
  local artifact_parent=${ARTIFACT_DIR%/*}
  if [[ "$artifact_parent" == "$ARTIFACT_DIR" ]]; then
    artifact_parent=.
  fi
  if [[ ! -e "$artifact_parent" ]]; then
    mkdir -p -- "$artifact_parent"
  fi
  if [[ ! -d "$artifact_parent" || -L "$artifact_parent" ]]; then
    echo "CI image identity artifact parent is not a real directory." >&2
    return 1
  fi
  mkdir -m 0700 -- "$ARTIFACT_DIR"
  if [[ ! -d "$ARTIFACT_DIR" || -L "$ARTIFACT_DIR" ]]; then
    echo "CI image identity artifact directory was not created safely." >&2
    return 1
  fi
}

require_fresh_artifact_path

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
    "builder_image_version_label", "input_manifest_label",
    "source_commit_label",
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

published_lock=()
while IFS= read -r published_value; do
  published_lock+=("$published_value")
done < <(read_published_lock)
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
expected_image_repository="ghcr.io/$EXPECTED_REPOSITORY/photospider-ci"
locator_declared_digest=
if [[ -n "$LOCATOR_OVERRIDE" ]]; then
  if [[ "$LOCATOR_OVERRIDE" =~ ^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+:[A-Za-z0-9_.-]+$ ]]; then
    override_image_repository=${LOCATOR_OVERRIDE%:*}
  elif [[ "$LOCATOR_OVERRIDE" =~ ^ghcr\.io/[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@sha256:[0-9a-f]{64}$ ]]; then
    override_image_repository=${LOCATOR_OVERRIDE%@*}
    locator_declared_digest=${LOCATOR_OVERRIDE##*@}
  else
    echo "CI_IMAGE_LOCATOR must be one exact GHCR tag or digest locator." >&2
    exit 2
  fi
  if [[ "$override_image_repository" != "$expected_image_repository" ]]; then
    echo "Candidate locator repository differs from the protected repository." >&2
    exit 1
  fi
  locator=$LOCATOR_OVERRIDE
fi

inspect_output=$(docker buildx imagetools inspect "$locator")
image_digests=()
while IFS= read -r resolved_digest; do
  image_digests+=("$resolved_digest")
done < <(awk '$1 == "Digest:" { print $2 }' <<<"$inspect_output" | sort -u)
if ((${#image_digests[@]} != 1)) ||
  [[ ! ${image_digests[0]} =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo "Locator did not resolve to exactly one canonical OCI digest." >&2
  exit 1
fi
image_digest=${image_digests[0]}
if [[ -n "$locator_declared_digest" &&
  "$image_digest" != "$locator_declared_digest" ]]; then
  echo "Digest-qualified locator resolved to a different OCI digest." >&2
  exit 1
fi
if [[ -n "$EXPECTED_DIGEST" && "$image_digest" != "$EXPECTED_DIGEST" ]]; then
  echo "Locator digest '$image_digest' differs from expected '$EXPECTED_DIGEST'." >&2
  exit 1
fi
exact_image=$expected_image_repository@$image_digest

source_commit=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" source-commit)
if [[ -n "$EXPECTED_SOURCE_COMMIT" &&
  "$source_commit" != "$EXPECTED_SOURCE_COMMIT" ]]; then
  echo "Measured CI image source commit differs from expected source commit." >&2
  exit 1
fi

# Authenticate the exact registry subject into process-private memory before
# creating formal artifacts, retaining runner identity, or letting Docker pull
# and expand a layer. Failure therefore leaves the upload path absent.
attestation_json=$(gh attestation verify "oci://$exact_image" \
  --repo "$EXPECTED_REPOSITORY" \
  --signer-workflow "$EXPECTED_REPOSITORY/$source_workflow" \
  --source-digest "$source_commit" \
  --signer-digest "$source_commit" \
  --deny-self-hosted-runners \
  --format json)
if [[ -z "$attestation_json" ]]; then
  echo "GitHub attestation verification returned empty evidence." >&2
  exit 1
fi

prepare_artifact_directory
verifier_runner_path=$ARTIFACT_DIR/verifier-linux-runner-identity.json
manifest_path=$ARTIFACT_DIR/expected-ci-image-input-v1.json
manifest_digest_path=$ARTIFACT_DIR/expected-ci-image-input-v1.sha256
labels_path=$ARTIFACT_DIR/oci-labels.json
builder_runner_path=$ARTIFACT_DIR/builder-linux-runner-identity.json
attestation_log=$ARTIFACT_DIR/attestation-verification.json
printf '%s\n' "$attestation_json" > "$attestation_log"
python3 "$SCRIPT_DIR/ci_runner_verify.py" \
  --platform Linux \
  --runner-label ubuntu-24.04 \
  --output "$verifier_runner_path" >/dev/null

# The image is not executed. Only after attestation succeeds are its config
# labels parsed as untrusted discovery data, reduced to one approved builder
# version, and retained before the canonical manifest is reconstructed. This
# allows an approved rollout host to verify an image built by the other
# approved rollout member without substituting the verifier's own mutable
# ImageVersion.
docker pull "$exact_image" >/dev/null
docker image inspect --format '{{json .Config.Labels}}' "$exact_image" > "$labels_path"
builder_image_version=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" builder-from-labels \
  --labels-json "$labels_path" \
  --output "$builder_runner_path")

manifest_digest=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" create \
  --source-commit "$source_commit" \
  --repository "$EXPECTED_REPOSITORY" \
  --builder-runner-identity "$builder_runner_path" \
  --output "$manifest_path" \
  --digest-output "$manifest_digest_path")
if [[ -n "$EXPECTED_MANIFEST_DIGEST" &&
  "$manifest_digest" != "$EXPECTED_MANIFEST_DIGEST" ]]; then
  echo "Measured CI image manifest digest differs from expected manifest digest." >&2
  exit 1
fi

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
  printf 'builder_image_version=%s\n' "$builder_image_version"
} | tee "$ARTIFACT_DIR/ci-image-identity.env"
if [[ -n "$OUTPUT_FILE" ]]; then
  cat "$ARTIFACT_DIR/ci-image-identity.env" >> "$OUTPUT_FILE"
fi
