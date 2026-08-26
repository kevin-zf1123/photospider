#!/usr/bin/env bash

set -Eeuo pipefail

# @file static_product_consumer_test.sh
# @brief Execute the complete static-product package consumer from one verified
#   installed-package role artifact.
# @note The artifact verifier must run first. This driver remeasures the exact
#   verified manifest before and after product/consumer execution, so any added,
#   removed, rewritten, or executable-bit-drifted package input fails. It never
#   configures, builds, installs, deletes, or rewrites producer state; only its
#   job-owned consumer work directory may be removed by the Python driver.
#   CI_STATIC_PACKAGE_MANIFEST_SHA256 is the exact digest retained before the
#   manifest attestation and exported by the protected extraction step.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
PACKAGE_ROOT=${CI_STATIC_PACKAGE_ROOT:-$REPO_ROOT/CI-results/installed-package-consumer/ci}
PACKAGE_MANIFEST=${CI_STATIC_PACKAGE_MANIFEST:-$REPO_ROOT/CI-results/targeted-artifact/installed-package.manifest.json}
ATTESTED_MANIFEST_SHA256=${CI_STATIC_PACKAGE_MANIFEST_SHA256:-}

if [[ "${CI_ARTIFACT_ROLE:-}" != installed-package ]]; then
  echo "Static product consumer requires the installed-package artifact role." >&2
  exit 2
fi
if [[ ! -d "$PACKAGE_ROOT" || -L "$PACKAGE_ROOT" ]]; then
  echo "Static product consumer package root is missing or aliased." >&2
  exit 1
fi
if [[ ! -f "$PACKAGE_MANIFEST" || -L "$PACKAGE_MANIFEST" ]]; then
  echo "Static product consumer manifest is missing or aliased." >&2
  exit 1
fi
if [[ ! "$ATTESTED_MANIFEST_SHA256" =~ ^[0-9a-f]{64}$ ]]; then
  echo "Static product consumer attested manifest digest is malformed." >&2
  exit 1
fi
readonly ATTESTED_MANIFEST_SHA256
installed_prefix="$PACKAGE_ROOT/installed"
producer_metadata="$PACKAGE_ROOT/producer"
for input in "$installed_prefix" "$producer_metadata"; do
  if [[ ! -d "$input" || -L "$input" ]]; then
    echo "Static product consumer input is missing or aliased: $input" >&2
    exit 1
  fi
done
if [[ "$installed_prefix" == "$producer_metadata" ||
  "$installed_prefix" == "$producer_metadata"/* ||
  "$producer_metadata" == "$installed_prefix"/* ]]; then
  echo "Installed prefix and producer metadata must be disjoint." >&2
  exit 1
fi

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"
cd "$REPO_ROOT"

# @brief Remeasure every restored package/metadata member against the signed
#   manifest and persist a compact phase-specific verifier record.
# @param $1 Stable phase identity: before or after.
# @param $@ Optional retained manifest/content digest arguments for the after
#   phase.
# @return Zero only when member paths, sizes, digests, and executable bits are
#   exactly unchanged.
# @throws Nothing; reusable_build.py diagnostics and status are propagated.
# @note Evidence is written outside PACKAGE_ROOT so measurement never changes
#   the tree it is proving immutable.
verify_package_content() {
  local phase=$1
  shift
  run_logged "package_input_$phase" \
    python3 "$SCRIPT_DIR/reusable_build.py" verify-targeted-tree \
      --manifest "$PACKAGE_MANIFEST" \
      --role installed-package \
      --content-root "$PACKAGE_ROOT" \
      --evidence-output \
        "$CI_ARTIFACT_DIR/package-input-$phase-verification.json" \
      "$@"
}

verify_package_content before \
  --expected-manifest-sha256 "$ATTESTED_MANIFEST_SHA256"
package_input_identity=$(
  python3 - "$CI_ARTIFACT_DIR/package-input-before-verification.json" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle)
expected = {
    "content_sha256",
    "manifest_sha256",
    "member_count",
    "role",
    "schema",
    "uncompressed_size",
}
if not isinstance(value, dict) or set(value) != expected:
    raise SystemExit("package-input before-verification evidence fields differ")
for field in ("content_sha256", "manifest_sha256"):
    if not isinstance(value[field], str) or re.fullmatch(r"[0-9a-f]{64}", value[field]) is None:
        raise SystemExit(f"package-input {field} is malformed")
print(f'{value["content_sha256"]}\t{value["manifest_sha256"]}')
PY
)
IFS=$'\t' read -r expected_content_sha256 expected_manifest_sha256 \
  unexpected_identity <<< "$package_input_identity"
if [[ ! "$expected_content_sha256" =~ ^[0-9a-f]{64}$ ||
  ! "$expected_manifest_sha256" =~ ^[0-9a-f]{64}$ ||
  -n "$unexpected_identity" ]]; then
  echo "Static package input did not retain two exact identities." >&2
  exit 1
fi
readonly expected_content_sha256
readonly expected_manifest_sha256

# @var consumer_work
# @brief Job-owned transient source/build/probe root for the package consumer.
# @note It is deliberately outside both restored inputs. The Python driver
#   validates the same pairwise-disjoint boundary before deleting this path.
consumer_work="$CI_ARTIFACT_DIR/static-product-consumer-work"
set +e
run_logged static_product_consumer \
  python3 -B "$REPO_ROOT/tests/integration/static_product_consumer_smoke.py" \
    --repo "$REPO_ROOT" \
    --work "$consumer_work" \
    --installed-prefix "$installed_prefix" \
    --producer-metadata "$producer_metadata" \
    --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
consumer_status=$?
set -e

set +e
verify_package_content after \
  --expected-content-sha256 "$expected_content_sha256" \
  --expected-manifest-sha256 "$expected_manifest_sha256"
content_status=$?
set -e
if ((content_status != 0)); then
  echo "Static product consumer changed verified package input." >&2
  exit "$content_status"
fi
if ((consumer_status != 0)); then
  echo "Static product consumer failed with status $consumer_status." >&2
  exit "$consumer_status"
fi
printf 'Verified installed-package consumer completed.\n' |
  tee "$CI_ARTIFACT_DIR/summary.log"
