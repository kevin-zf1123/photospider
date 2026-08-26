#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_image_promote.sh
# @brief Promote one fully tested CI-image digest to canonical GHCR tags without
#   rebuilding or changing its attested OCI subject.
# @note The caller must already be authenticated to GHCR with package-write
#   authority. Only a trusted push of ``main`` or ``CI/**`` is accepted;
#   pull-request, pull_request_target, dispatch, and arbitrary branch routes
#   fail before ``imagetools create``. ``latest`` is emitted only for ``main``.
# @note A CI branch tag combines a bounded readable slug with SHA-256 of the
#   exact validated full branch name. The digest input has no added newline,
#   so distinct names that slug identically remain distinct and deterministic.
# @note Git itself validates ``refs/heads/<branch>``. The readable slug is not
#   an alternate ref validator; unsafe Docker-tag bytes are only presentation.
# @note Before any registry tag is written, the protected image
#   manifest helper fetches the live branch twice around one isolated worktree
#   measurement. A newer image-input identity makes this run ``superseded``;
#   force-push, unknown ancestry, manifest failure, or ref drift fails closed.
# @note Freshness also receives the producer's exact resolved builder image
#   version. It reconstructs the candidate manifest with that approved member;
#   the promotion host's own mutable ImageVersion never replaces provenance.
# @note One job-scoped repository/image promotion lease serializes every
#   workflow-owned SHA and mutable writer across refs. After freshness succeeds,
#   the SHA tag is inspected before any write: an exact digest is reused, an
#   exact not-found result is created once, and every conflict or
#   ambiguous/unknown inspection fails with zero writes. GHCR provides no tag
#   compare-and-swap against a writer outside that maintained lease.
# @note ``CI_IMAGE_PROMOTION_REPO_ROOT`` and scratch-root overrides are trusted
#   direct-developer/test seams. The maintained workflow does not populate them
#   from candidate or repository-variable contexts.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=${CI_IMAGE_PROMOTION_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}
ARTIFACT_DIR=${CI_IMAGE_PROMOTION_ARTIFACT_DIR:-$REPO_ROOT/CI-results/ci-image-promotion}
OUTPUT_FILE=${CI_IMAGE_PROMOTION_OUTPUT_FILE:-${GITHUB_OUTPUT:-}}
SCRATCH_ROOT=${CI_IMAGE_PROMOTION_SCRATCH_ROOT:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}}

# @brief Print the exact protected promotion interface.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: ci_image_promote.sh \
  --repository OWNER/REPOSITORY \
  --image-ref ghcr.io/OWNER/REPOSITORY/photospider-ci@sha256:DIGEST \
  --expected-digest sha256:DIGEST \
  --candidate-commit FULL_SHA \
  --source-commit FULL_SHA \
  --manifest-digest SHA256 \
  --builder-image-version EXACT_IMAGE_VERSION \
  --event-name push \
  --branch main|CI/NAME

The command creates branch-main or
branch-<bounded-slug>-<sha256-of-full-branch-name>, plus immutable
sha-<full-sha>, from the already tested digest. An existing exact SHA tag is
reused and never rewritten. A main push additionally updates latest only while
its live image-input identity remains current. A superseded run may create its
previously absent SHA tag but never writes branch/latest. It never builds.
EOF
}

# @brief Derive one deterministic collision-resistant OCI tag for a CI branch.
# @param $1 Exact Git-validated full branch name, including any legal UTF-8.
# @return Zero after printing one Docker-safe tag no longer than 128 ASCII
#   characters; Python encoding/hash failures return nonzero.
# @throws Nothing; helper failures propagate through the caller's strict mode.
# @note ``os.fsencode`` round-trips the exact branch-name bytes received in
#   argv, including surrogate-escaped bytes, without adding a newline. Only a
#   Docker-safe ASCII presentation slug is compressed/truncated; the complete
#   64-hex digest is retained and is independent of locale presentation.
canonical_ci_branch_tag() {
  python3 - "$1" <<'PY'
import hashlib
import os
import re
import sys

branch_name = sys.argv[1]
branch_identity = os.fsencode(branch_name)
digest = hashlib.sha256(branch_identity).hexdigest()
safe_slug_bytes = frozenset(
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-"
)
slug_characters = []
previous_separator = False
for byte in branch_identity:
    character = chr(byte) if byte in safe_slug_bytes else "-"
    if character == "-":
        if previous_separator:
            continue
        previous_separator = True
    else:
        previous_separator = False
    slug_characters.append(character)
slug = "".join(slug_characters).strip("-")
if not slug:
    raise SystemExit("CI branch identity produced an empty readable slug")
prefix = "branch-"
separator = "-"
maximum_tag_length = 128
slug_limit = maximum_tag_length - len(prefix) - len(separator) - len(digest)
if slug_limit <= 0:
    raise SystemExit("CI branch tag components exceed the OCI length boundary")
tag = f"{prefix}{slug[:slug_limit]}{separator}{digest}"
if len(tag) > maximum_tag_length or re.fullmatch(
    r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", tag
) is None:
    raise SystemExit("derived CI branch tag is not a canonical OCI tag")
print(tag)
PY
}

# @brief Parse one Buildx ``.Manifest`` JSON document to its top-level digest.
# @param $1 Captured successful ``imagetools inspect --format`` stdout path.
# @return Zero after printing one canonical SHA-256 digest; nonzero for missing,
#   duplicate, malformed, concatenated, or non-object JSON.
# @throws Nothing; Python parse diagnostics propagate through strict mode.
# @note Nested platform descriptors are not tag identity. Only the unique
#   top-level descriptor returned by Buildx's documented ``.Manifest`` format
#   may satisfy immutable or mutable tag verification.
read_inspected_digest() {
  python3 - "$1" <<'PY'
import json
import re
import sys

def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member: {key}")
        result[key] = value
    return result

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle, object_pairs_hook=unique)
digest = value.get("digest") if isinstance(value, dict) else None
if not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
    raise SystemExit("inspected manifest lacks one canonical top-level digest")
print(digest)
PY
}

# @brief Accept only the locked Buildx/GHCR not-found diagnostic for one ref.
# @param $1 Captured failed inspect stdout path, which must be byte-empty.
# @param $2 Captured failed inspect stderr path.
# @param $3 Exact immutable SHA destination reference.
# @return Zero only for empty stdout and one exact newline-terminated not-found
#   diagnostic; nonzero for auth, network, warning, status, or text drift.
# @throws Nothing; Python comparison status is returned to the caller.
# @note The caller separately requires exit status one. This narrow classifier
#   intentionally fails closed when the locked runner/Buildx error contract
#   changes instead of treating an unknown registry failure as absence.
is_exact_missing_sha_inspection() {
  python3 - "$1" "$2" "$3" <<'PY'
from pathlib import Path
import sys

stdout = Path(sys.argv[1]).read_bytes()
stderr = Path(sys.argv[2]).read_bytes()
expected = f"ERROR: {sys.argv[3]}: not found\n".encode()
raise SystemExit(0 if stdout == b"" and stderr == expected else 1)
PY
}

# @brief Read one Buildx create metadata file's unique descriptor digest.
# @param $1 Exact regular metadata JSON path.
# @return Zero after printing one canonical SHA-256 descriptor digest.
# @throws Nothing; malformed, duplicated, missing, or aliased metadata fails.
# @note This verifies the create response independently from subsequent tag
#   resolution; both identities must equal the tested digest.
read_created_metadata_digest() {
  local metadata_file=$1
  if [[ ! -f "$metadata_file" || -L "$metadata_file" ]]; then
    echo "Promotion metadata is missing or is not a regular file." >&2
    return 1
  fi
  python3 - "$metadata_file" <<'PY'
import json
import re
import sys

def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member: {key}")
        result[key] = value
    return result

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle, object_pairs_hook=unique)
descriptor = value.get("containerimage.descriptor") if isinstance(value, dict) else None
digest = descriptor.get("digest") if isinstance(descriptor, dict) else None
if not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
    raise SystemExit("promotion metadata lacks one canonical descriptor digest")
print(digest)
PY
}

repository=
image_ref=
expected_digest=
candidate_commit=
source_commit=
manifest_digest=
builder_image_version=
event_name=
branch_name=
while (($#)); do
  case "$1" in
    --repository)
      repository=${2:-}
      shift 2
      ;;
    --image-ref)
      image_ref=${2:-}
      shift 2
      ;;
    --expected-digest)
      expected_digest=${2:-}
      shift 2
      ;;
    --candidate-commit)
      candidate_commit=${2:-}
      shift 2
      ;;
    --source-commit)
      source_commit=${2:-}
      shift 2
      ;;
    --manifest-digest)
      manifest_digest=${2:-}
      shift 2
      ;;
    --builder-image-version)
      builder_image_version=${2:-}
      shift 2
      ;;
    --event-name)
      event_name=${2:-}
      shift 2
      ;;
    --branch)
      branch_name=${2:-}
      shift 2
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

for command_name in docker git python3; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required CI image promotion command is unavailable: $command_name" >&2
    exit 1
  fi
done
if [[ ! "$repository" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
  echo "Promotion repository must be an exact owner/name identity." >&2
  exit 2
fi
if [[ ! "$expected_digest" =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo "Promotion digest must be one canonical SHA-256 identity." >&2
  exit 2
fi
expected_image="ghcr.io/$repository/photospider-ci@$expected_digest"
if [[ "$image_ref" != "$expected_image" ]]; then
  echo "Promotion image_ref does not equal the protected repository and digest." >&2
  exit 1
fi
if [[ ! "$candidate_commit" =~ ^[0-9a-f]{40}$ ||
  ! "$source_commit" =~ ^[0-9a-f]{40}$ ||
  "$candidate_commit" != "$source_commit" ]]; then
  echo "Promotion candidate/source commits must be one identical full SHA." >&2
  exit 1
fi
if [[ ! "$manifest_digest" =~ ^[0-9a-f]{64}$ ]]; then
  echo "Promotion manifest digest must be one lowercase SHA-256 value." >&2
  exit 2
fi
if [[ ! "$builder_image_version" =~ ^20[0-9]{6}\.[0-9]{3,4}\.[0-9]+$ ]]; then
  echo "Promotion builder image version must be one exact hosted image identity." >&2
  exit 2
fi
if [[ "$event_name" != push ]]; then
  echo "Only a trusted push may promote a CI image." >&2
  exit 1
fi
if [[ "$branch_name" == main ]]; then
  branch_tag=branch-main
elif [[ "$branch_name" == CI/* ]] &&
  git check-ref-format "refs/heads/$branch_name" >/dev/null 2>&1; then
  branch_tag=$(canonical_ci_branch_tag "$branch_name")
else
  echo "Only main or one canonical CI/** branch may promote a CI image." >&2
  exit 1
fi
if ((${#branch_tag} > 128)) ||
  [[ ! "$branch_tag" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]]; then
  echo "Canonical branch tag violates the OCI tag syntax or length boundary." >&2
  exit 1
fi

image_repository="ghcr.io/$repository/photospider-ci"
sha_tag="sha-$candidate_commit"

if [[ -e "$ARTIFACT_DIR" || -L "$ARTIFACT_DIR" ]]; then
  echo "Promotion artifact directory contains residual or aliased state." >&2
  exit 1
fi
mkdir -m 700 -p -- "$ARTIFACT_DIR"
mutable_metadata_path=$ARTIFACT_DIR/promotion-metadata.json
sha_metadata_path=$ARTIFACT_DIR/sha-promotion-metadata.json
sha_preflight_stdout=$ARTIFACT_DIR/sha-preflight-manifest.json
sha_preflight_stderr=$ARTIFACT_DIR/sha-preflight.stderr
sha_verify_stdout=$ARTIFACT_DIR/sha-verified-manifest.json
sha_verify_stderr=$ARTIFACT_DIR/sha-verified.stderr
tags_path=$ARTIFACT_DIR/promoted-tags.txt
freshness_path=$ARTIFACT_DIR/promotion-freshness.json

freshness_status=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  --repo-root "$REPO_ROOT" promotion-freshness \
  --candidate-commit "$candidate_commit" \
  --candidate-source-commit "$source_commit" \
  --candidate-manifest-digest "$manifest_digest" \
  --candidate-builder-image-version "$builder_image_version" \
  --repository "$repository" \
  --branch "$branch_name" \
  --scratch-root "$SCRATCH_ROOT" \
  --output "$freshness_path")
case "$freshness_status" in
  current)
    promotion_status=promoted
    mutable_tags=("$image_repository:$branch_tag")
    if [[ "$branch_name" == main ]]; then
      mutable_tags+=("$image_repository:latest")
    fi
    ;;
  superseded)
    promotion_status=superseded
    mutable_tags=()
    ;;
  *)
    echo "Promotion freshness returned an unknown status: '$freshness_status'." >&2
    exit 1
    ;;
esac

sha_destination="$image_repository:$sha_tag"
: > "$sha_preflight_stdout"
: > "$sha_preflight_stderr"
if docker buildx imagetools inspect \
  --format '{{json .Manifest}}' "$sha_destination" \
  > "$sha_preflight_stdout" 2> "$sha_preflight_stderr"; then
  if [[ -s "$sha_preflight_stderr" ]]; then
    echo "Immutable SHA inspection emitted unexpected stderr." >&2
    exit 1
  fi
  sha_resolved_digest=$(read_inspected_digest "$sha_preflight_stdout")
  if [[ "$sha_resolved_digest" != "$expected_digest" ]]; then
    echo "Immutable SHA tag already resolves to a different digest." >&2
    exit 1
  fi
  sha_action=reused
else
  sha_inspect_status=$?
  if ((sha_inspect_status != 1)) ||
    ! is_exact_missing_sha_inspection \
      "$sha_preflight_stdout" "$sha_preflight_stderr" "$sha_destination"; then
    echo "Immutable SHA preflight failed without one exact not-found result." >&2
    exit 1
  fi
  sha_action=created
fi

: > "$tags_path"
if [[ "$sha_action" == created ]]; then
  docker buildx imagetools create --prefer-index=false \
    --tag "$sha_destination" \
    --metadata-file "$sha_metadata_path" \
    "$image_ref"
  sha_metadata_digest=$(read_created_metadata_digest "$sha_metadata_path")
  if [[ "$sha_metadata_digest" != "$expected_digest" ]]; then
    echo "Immutable SHA creation metadata differs from the tested digest." >&2
    exit 1
  fi
  : > "$sha_verify_stdout"
  : > "$sha_verify_stderr"
  if ! docker buildx imagetools inspect \
    --format '{{json .Manifest}}' "$sha_destination" \
    > "$sha_verify_stdout" 2> "$sha_verify_stderr"; then
    echo "New immutable SHA tag could not be verified after creation." >&2
    exit 1
  fi
  if [[ -s "$sha_verify_stderr" ]]; then
    echo "Immutable SHA verification emitted unexpected stderr." >&2
    exit 1
  fi
  sha_resolved_digest=$(read_inspected_digest "$sha_verify_stdout")
  if [[ "$sha_resolved_digest" != "$expected_digest" ]]; then
    echo "New immutable SHA tag differs from the tested digest." >&2
    exit 1
  fi
fi
printf '%s\t%s\t%s\n' \
  "$sha_destination" "$expected_digest" "$sha_action" >> "$tags_path"

if [[ "$promotion_status" == promoted ]]; then
  create_arguments=(docker buildx imagetools create --prefer-index=false)
  for destination in "${mutable_tags[@]}"; do
    create_arguments+=(--tag "$destination")
  done
  create_arguments+=(--metadata-file "$mutable_metadata_path" "$image_ref")
  "${create_arguments[@]}"

  mutable_metadata_digest=$(
    read_created_metadata_digest "$mutable_metadata_path"
  )
  if [[ "$mutable_metadata_digest" != "$expected_digest" ]]; then
    echo "Mutable promotion metadata differs from the tested digest." >&2
    exit 1
  fi

  mutable_index=0
  for destination in "${mutable_tags[@]}"; do
    mutable_stdout=$ARTIFACT_DIR/mutable-$mutable_index-manifest.json
    mutable_stderr=$ARTIFACT_DIR/mutable-$mutable_index.stderr
    : > "$mutable_stdout"
    : > "$mutable_stderr"
    if ! docker buildx imagetools inspect \
      --format '{{json .Manifest}}' "$destination" \
      > "$mutable_stdout" 2> "$mutable_stderr"; then
      echo "Promoted mutable tag '$destination' could not be inspected." >&2
      exit 1
    fi
    if [[ -s "$mutable_stderr" ]]; then
      echo "Promoted mutable tag '$destination' emitted unexpected stderr." >&2
      exit 1
    fi
    resolved_digest=$(read_inspected_digest "$mutable_stdout")
    if [[ "$resolved_digest" != "$expected_digest" ]]; then
      echo "Promoted tag '$destination' does not resolve to the tested digest." >&2
      exit 1
    fi
    printf '%s\t%s\tpromoted\n' \
      "$destination" "$expected_digest" >> "$tags_path"
    mutable_index=$((mutable_index + 1))
  done
fi

if [[ -n "$OUTPUT_FILE" ]]; then
  {
    printf 'status=%s\n' "$promotion_status"
    printf 'digest=%s\n' "$expected_digest"
    printf 'branch_tag=%s\n' "$branch_tag"
    printf 'sha_tag=%s\n' "$sha_tag"
    printf 'sha_action=%s\n' "$sha_action"
  } >> "$OUTPUT_FILE"
fi
if [[ "$promotion_status" == superseded ]]; then
  printf 'Candidate %s was superseded; immutable tag %s was %s.\n' \
    "$candidate_commit" "$sha_tag" "$sha_action"
else
  printf 'Promoted %s to %s mutable tag(s); immutable tag %s was %s.\n' \
    "$expected_digest" "${#mutable_tags[@]}" "$sha_tag" "$sha_action"
fi
