#!/usr/bin/env bash

set -Eeuo pipefail

# @file linux_security_profile.sh
# @brief Verify one Linux hosted runner on the real host, authenticate/pull an
#   exact CI image, and execute one protected security profile inside it.
# @note The caller must create the retained runner identity before checking out
#   candidate data. This protected helper receives that same regular file and
#   mounts it read-only beside a read-only candidate source tree, a nested
#   read-only protected ``ci`` control tree, and read-only profile inventory.
#   Only the fresh job-owned ``/work`` mount is writable. The container runs as
#   the same strictly measured positive numeric UID/GID that owns that mode-0700
#   host directory. Measurement opens the directory without following links,
#   retains its device/inode/mode/owner tuple, and repeats that measurement
#   immediately before Docker runs. A protected entrypoint then proves the
#   identity plus a real create/read/remove cycle before candidate work. The
#   GHCR token is used by the host login command and is never forwarded into
#   the container.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)

# @brief Print the exact environment-owned Linux profile interface.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: CI_SECURITY_PROFILE=<sanitizer-asan|sanitizer-tsan|fuzz-codecs> \
  CI_CONTROL_ROOT=<protected checkout> CI_CANDIDATE_ROOT=<candidate checkout> \
  CI_INVENTORY_DIR=<downloaded inventory> CI_RUNNER_IDENTITY_FILE=<retained json> \
  CI_WORK_ROOT=<fresh runner.temp child> CI_IMAGE_REF=<digest-qualified image> \
  CI_IMAGE_DIGEST=<sha256 digest> CI_GHCR_USERNAME=<actor> CI_GHCR_TOKEN=<token> \
  CI_CANDIDATE_COMMIT=<sha> CI_WORKFLOW_COMMIT=<sha> \
  CI_RUNNER_TEMP=<runner.temp> GITHUB_REPOSITORY=<owner/name> \
  ci/scripts/linux_security_profile.sh
EOF
}

if (($# != 0)); then
  usage >&2
  exit 2
fi

required_environment=(
  CI_CANDIDATE_COMMIT
  CI_CANDIDATE_ROOT
  CI_CONTROL_ROOT
  CI_GHCR_TOKEN
  CI_GHCR_USERNAME
  CI_IMAGE_DIGEST
  CI_IMAGE_REF
  CI_INVENTORY_DIR
  CI_JOBS
  CI_RUNNER_IDENTITY_FILE
  CI_RUNNER_TEMP
  CI_SECURITY_PROFILE
  CI_WORKFLOW_COMMIT
  CI_WORK_ROOT
  GITHUB_REPOSITORY
)
for required_name in "${required_environment[@]}"; do
  if [[ -z ${!required_name:-} ]]; then
    echo "Linux security wrapper lacks required environment: $required_name" >&2
    usage >&2
    exit 2
  fi
done

case "$CI_SECURITY_PROFILE" in
  sanitizer-asan)
    profile_script=ci/scripts/sanitizer_test.sh
    sanitizer=asan
    ;;
  sanitizer-tsan)
    profile_script=ci/scripts/sanitizer_test.sh
    sanitizer=tsan
    ;;
  fuzz-codecs)
    profile_script=ci/scripts/fuzz_smoke.sh
    sanitizer=
    ;;
  *)
    echo "Unsupported Linux security profile: $CI_SECURITY_PROFILE" >&2
    exit 2
    ;;
esac

if [[ ! "$CI_CANDIDATE_COMMIT" =~ ^[0-9a-f]{40}$ ||
  ! "$CI_WORKFLOW_COMMIT" =~ ^[0-9a-f]{40}$ ||
  ! "$CI_IMAGE_DIGEST" =~ ^sha256:[0-9a-f]{64}$ ||
  ! "$GITHUB_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ||
  ! "$CI_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Linux security wrapper received a malformed identity." >&2
  exit 1
fi
expected_image_ref="ghcr.io/$GITHUB_REPOSITORY/photospider-ci@$CI_IMAGE_DIGEST"
if [[ "$CI_IMAGE_REF" != "$expected_image_ref" ]]; then
  echo "Linux security image reference differs from its exact digest identity." >&2
  exit 1
fi

# @brief Resolve one real directory and reject commas/control bytes that would
#   make Docker's structured bind-mount grammar ambiguous.
# @param $1 Existing directory.
# @param $2 Stable diagnostic role.
# @return Physical directory path on stdout.
# @throws Nothing; missing, linked, comma-bearing, control-bearing, or
#   unresolvable directories return nonzero before login/pull/candidate work.
real_mount_directory() {
  local path=$1
  local role=$2
  if [[ ! -d "$path" || -L "$path" || "$path" == *','* ||
    "$path" == *$'\n'* || "$path" == *$'\r'* || "$path" == *$'\t'* ]]; then
    echo "$role is not an unambiguous real directory: $path" >&2
    return 1
  fi
  (cd -- "$path" && pwd -P)
}

# @brief Measure one work directory through a no-follow descriptor.
# @param $1 Fresh job-owned work-root pathname.
# @return A canonical ``device:inode:mode:uid:gid`` identity on stdout.
# @throws Nothing; unavailable safe-open flags, a link/special/replaced path,
#   non-0700 mode, nonpositive ownership, or metadata drift returns nonzero.
# @note The protected inline reader imports no candidate module. It opens with
#   O_NOFOLLOW, O_NONBLOCK, O_CLOEXEC, and O_DIRECTORY, then compares pathname
#   and descriptor metadata before emitting the retained identity. The caller
#   repeats this measurement immediately before Docker consumes the bind.
measure_work_root_identity() {
  local path=$1
  /usr/bin/python3 - "$path" <<'PY'
import os
import stat
import sys


def fail(message: str) -> "NoReturn":
    """Abort one protected work-root measurement with a bounded diagnostic."""
    raise SystemExit(f"Linux security work root {message}.")


path = sys.argv[1]
required_flags = ("O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC", "O_DIRECTORY")
if any(not hasattr(os, name) for name in required_flags):
    fail("cannot be opened with all required safe descriptor flags")
flags = os.O_RDONLY
for name in required_flags:
    flags |= getattr(os, name)
try:
    descriptor = os.open(path, flags)
except OSError as error:
    fail(f"cannot be opened safely: {error.strerror or error.__class__.__name__}")
try:
    opened = os.fstat(descriptor)
    pathname = os.lstat(path)
    opened_identity = (
        opened.st_dev,
        opened.st_ino,
        opened.st_mode,
        opened.st_uid,
        opened.st_gid,
    )
    pathname_identity = (
        pathname.st_dev,
        pathname.st_ino,
        pathname.st_mode,
        pathname.st_uid,
        pathname.st_gid,
    )
    if opened_identity != pathname_identity:
        fail("pathname differs from its opened descriptor")
    if not stat.S_ISDIR(opened.st_mode):
        fail("is not a directory")
    mode = stat.S_IMODE(opened.st_mode)
    if mode != 0o700:
        fail("mode is not exactly 0700")
    if opened.st_ino <= 0 or opened.st_uid <= 0 or opened.st_gid <= 0:
        fail("has no positive inode/owner identity")
    final = os.fstat(descriptor)
    final_identity = (
        final.st_dev,
        final.st_ino,
        final.st_mode,
        final.st_uid,
        final.st_gid,
    )
    if final_identity != opened_identity:
        fail("descriptor metadata changed during measurement")
    print(
        f"{opened.st_dev}:{opened.st_ino}:{mode:o}:"
        f"{opened.st_uid}:{opened.st_gid}"
    )
finally:
    os.close(descriptor)
PY
}

control_root=$(real_mount_directory "$CI_CONTROL_ROOT" "Protected control root")
candidate_root=$(real_mount_directory "$CI_CANDIDATE_ROOT" "Candidate source root")
inventory_root=$(real_mount_directory "$CI_INVENTORY_DIR" "Security inventory root")
runner_temp=$(real_mount_directory "$CI_RUNNER_TEMP" "Runner temporary root")
if [[ "$control_root" != "$SCRIPT_ROOT" ]]; then
  echo "Linux security wrapper is not executing from its declared protected checkout." >&2
  exit 1
fi
for left in "$control_root" "$candidate_root" "$inventory_root"; do
  for right in "$control_root" "$candidate_root" "$inventory_root"; do
    [[ "$left" == "$right" ]] && continue
    if [[ "$left" == "$right"/* || "$right" == "$left"/* ]]; then
      echo "Linux security control, candidate, and inventory roots overlap." >&2
      exit 1
    fi
  done
done
if [[ ! -f "$CI_RUNNER_IDENTITY_FILE" || -L "$CI_RUNNER_IDENTITY_FILE" ||
  "$CI_RUNNER_IDENTITY_FILE" == *','* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\n'* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\r'* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\t'* ]]; then
  echo "Linux retained runner identity is not a safe regular file." >&2
  exit 1
fi
identity_file=$(cd -- "$(dirname -- "$CI_RUNNER_IDENTITY_FILE")" && pwd -P)/$(basename -- "$CI_RUNNER_IDENTITY_FILE")

candidate_head=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
  git -C "$candidate_root" rev-parse --verify HEAD^{commit})
workflow_head=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
  git -C "$control_root" rev-parse --verify HEAD^{commit})
if [[ "$candidate_head" != "$CI_CANDIDATE_COMMIT" ||
  "$workflow_head" != "$CI_WORKFLOW_COMMIT" ]]; then
  echo "Linux security checkout commit differs from its protected identity." >&2
  exit 1
fi

work_parent=${CI_WORK_ROOT%/*}
if [[ "$work_parent" == "$CI_WORK_ROOT" || -z "$work_parent" ||
  -e "$CI_WORK_ROOT" || -L "$CI_WORK_ROOT" ]]; then
  echo "Linux security work root is residual or has no explicit parent." >&2
  exit 1
fi
work_parent=$(real_mount_directory "$work_parent" "Linux security work parent")
if [[ "$work_parent" != "$runner_temp" ]]; then
  echo "Linux security work root is not a direct runner.temp child." >&2
  exit 1
fi
mkdir -m 700 -- "$CI_WORK_ROOT"
# A setgid runner.temp parent may correctly transfer its group to this fresh
# child; clear only inherited special mode bits while retaining that real GID.
/bin/chmod 700 "$CI_WORK_ROOT"
work_root=$(real_mount_directory "$CI_WORK_ROOT" "Linux security work root")
mkdir -m 700 -- "$work_root/build" "$work_root/home" "$work_root/results" \
  "$work_root/runner-temp" "$work_root/tmp"
work_identity=$(measure_work_root_identity "$work_root")
if [[ ! "$work_identity" =~ ^[0-9]+:[1-9][0-9]*:700:[1-9][0-9]*:[1-9][0-9]*$ ]]; then
  echo "Linux security work-root measurement is not canonical." >&2
  exit 1
fi
IFS=: read -r work_device work_inode work_mode container_uid container_gid \
  <<< "$work_identity"

# Authenticate on the host only. The protected command uses password-stdin;
# the secret is removed from this process before pull/run argv are constructed.
printf '%s' "$CI_GHCR_TOKEN" |
  docker login ghcr.io --username "$CI_GHCR_USERNAME" --password-stdin
unset CI_GHCR_TOKEN
docker pull "$CI_IMAGE_REF"

container_arguments=(
  run
  --rm
  --read-only
  --network none
  --cap-drop ALL
  --security-opt no-new-privileges
  --pids-limit 4096
  --user "$container_uid:$container_gid"
  --tmpfs /tmp:rw,nosuid,nodev
  --mount "type=bind,src=$candidate_root,dst=/workspace/photospider,readonly"
  --mount "type=bind,src=$control_root/ci,dst=/workspace/photospider/ci,readonly"
  --mount "type=bind,src=$inventory_root,dst=/inputs/profile,readonly"
  --mount "type=bind,src=$identity_file,dst=/inputs/runner-identity.json,readonly"
  --mount "type=bind,src=$work_root,dst=/work"
  --workdir /workspace/photospider
  --env BUILD_DIR=/work/build
  --env CMAKE_BUILD_TYPE=RelWithDebInfo
  --env CI_ARTIFACT_DIR=/work/results
  --env "CI_CONTAINER_GID=$container_gid"
  --env "CI_CONTAINER_UID=$container_uid"
  --env CI_CONTAINER_WORK_ROOT=/work
  --env CI_INVENTORY_DIR=/inputs/profile
  --env "CI_JOBS=$CI_JOBS"
  --env CI_RUNNER_IDENTITY_FILE=/inputs/runner-identity.json
  --env CI_RUNNER_TEMP=/work/runner-temp
  --env HOME=/work/home
  --env TMPDIR=/work/tmp
)
if [[ -n "$sanitizer" ]]; then
  container_arguments+=(--env "SANITIZER=$sanitizer")
fi
container_arguments+=(
  "$CI_IMAGE_REF"
  bash
  ci/scripts/linux_security_container_entrypoint.sh
  "$profile_script"
)

{
  printf 'schema=photospider-linux-security-host-wrapper-v1\n'
  printf 'profile=%s\n' "$CI_SECURITY_PROFILE"
  printf 'candidate_commit=%s\n' "$CI_CANDIDATE_COMMIT"
  printf 'workflow_commit=%s\n' "$CI_WORKFLOW_COMMIT"
  printf 'image_digest=%s\n' "$CI_IMAGE_DIGEST"
  printf 'work_device=%s\n' "$work_device"
  printf 'work_inode=%s\n' "$work_inode"
  printf 'work_mode=%s\n' "$work_mode"
  printf 'container_uid=%s\n' "$container_uid"
  printf 'container_gid=%s\n' "$container_gid"
} > "$work_root/results/linux-security-host-wrapper.env"

current_work_identity=$(measure_work_root_identity "$work_root")
if [[ "$current_work_identity" != "$work_identity" ]]; then
  echo "Linux security work root changed before container execution." >&2
  exit 1
fi
docker "${container_arguments[@]}"
if [[ ! -s "$work_root/results/summary.log" ]]; then
  echo "Linux security container produced no profile success evidence." >&2
  exit 1
fi
