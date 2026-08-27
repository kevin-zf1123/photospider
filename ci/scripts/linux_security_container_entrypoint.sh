#!/usr/bin/env bash

set -Eeuo pipefail

# @file linux_security_container_entrypoint.sh
# @brief Prove the exact numeric Linux container identity and writable work
#   mount before executing one protected sanitizer or fuzz profile.
# @note The host wrapper supplies the UID/GID measured on the same hosted
#   runner that owns the mode-0700 bind source. Candidate data cannot select
#   this helper, its environment, or the protected profile path. The probe is
#   confined to the fresh job-owned work root and is removed before candidate
#   configure/build/test execution.

# @brief Print the exact protected container-entrypoint interface.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: CI_CONTAINER_UID=<positive decimal> CI_CONTAINER_GID=<positive decimal> \
  CI_CONTAINER_WORK_ROOT=<fresh writable mount> \
  ci/scripts/linux_security_container_entrypoint.sh \
  <ci/scripts/sanitizer_test.sh|ci/scripts/fuzz_smoke.sh>
EOF
}

if (($# != 1)); then
  usage >&2
  exit 2
fi

profile_script=$1
case "$profile_script" in
  ci/scripts/sanitizer_test.sh | ci/scripts/fuzz_smoke.sh) ;;
  *)
    echo "Unsupported protected Linux security profile script: $profile_script" >&2
    exit 2
    ;;
esac

container_uid=${CI_CONTAINER_UID:-}
container_gid=${CI_CONTAINER_GID:-}
work_root=${CI_CONTAINER_WORK_ROOT:-}
if [[ ! "$container_uid" =~ ^[1-9][0-9]*$ ||
  ! "$container_gid" =~ ^[1-9][0-9]*$ ||
  "$work_root" != /* || "$work_root" == */ ||
  "$work_root" == *$'\n'* || "$work_root" == *$'\r'* ||
  "$work_root" == *$'\t'* || ! -d "$work_root" || -L "$work_root" ]]; then
  echo "Linux security container received a malformed execution identity." >&2
  exit 1
fi

actual_uid=$(/usr/bin/id -u)
actual_gid=$(/usr/bin/id -g)
if [[ "$actual_uid" != "$container_uid" || "$actual_gid" != "$container_gid" ]]; then
  echo "Linux security container UID/GID differs from the bound host owner." >&2
  exit 1
fi

probe_path=$work_root/.photospider-ci-write-probe
probe=

# @brief Remove only the entrypoint-owned probe after an interrupted check.
# @return Zero; cleanup never broadens beyond the exact fresh work root.
cleanup_probe() {
  if [[ -n ${probe:-} && -f "$probe" && ! -L "$probe" ]]; then
    rm -- "$probe"
  fi
}
trap cleanup_probe EXIT HUP INT TERM

if [[ -e "$probe_path" || -L "$probe_path" ]]; then
  echo "Linux security work probe contains residual state." >&2
  exit 1
fi
probe=$probe_path
(
  umask 077
  printf '%s\n' photospider-linux-security-write-v1 > "$probe"
)
if [[ ! -f "$probe" || -L "$probe" ||
  $(<"$probe") != photospider-linux-security-write-v1 ]]; then
  echo "Linux security work mount is not exactly writable by the bound identity." >&2
  exit 1
fi
rm -- "$probe"
probe=
trap - EXIT HUP INT TERM

exec bash "$profile_script"
