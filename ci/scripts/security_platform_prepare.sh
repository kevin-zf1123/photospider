#!/usr/bin/env bash

set -Eeuo pipefail

# @file security_platform_prepare.sh
# @brief Consume one retained Darwin/Linux runner identity and materialize exact
#   platform CMake/toolchain commands without re-reading mutable environment.
# @note Linux dependencies are supplied by the attested CI image. Darwin uses
#   the exact vcpkg commit selected for the measured image version in the
#   retained identity. It copies the image-bound vcpkg binary into an
#   unseedable fresh checkout created below runner.temp, while the checkout is
#   populated from the locked preinstalled Git object or the explicit official
#   microsoft/vcpkg source. Each Darwin member also binds the exact compatible
#   CMake path/version/GoogleTest module and the exact LLVM 18 libFuzzer
#   compiler pair. Windows and unknown platforms fail rather than receiving
#   reduced coverage.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
PROFILE_JSON=${1:-}
OUTPUT_FILE=${CI_PLATFORM_CMAKE_ARGS_FILE:-}
CMAKE_COMMAND_FILE=${CI_PLATFORM_CMAKE_COMMAND_FILE:-}
RUNNER_IDENTITY_FILE=${CI_RUNNER_IDENTITY_FILE:-}
if [[ -z "$PROFILE_JSON" || -z "$OUTPUT_FILE" || -z "$CMAKE_COMMAND_FILE" ||
  -z "$RUNNER_IDENTITY_FILE" || $# -ne 1 ]]; then
  echo "Usage: CI_RUNNER_IDENTITY_FILE=<path> CI_PLATFORM_CMAKE_ARGS_FILE=<path> CI_PLATFORM_CMAKE_COMMAND_FILE=<path> $0 <resolved-profile.json>" >&2
  exit 2
fi
if [[ ! -f "$PROFILE_JSON" || -L "$PROFILE_JSON" ]]; then
  echo "Resolved security profile is missing or not a regular file." >&2
  exit 1
fi
for platform_output in "$OUTPUT_FILE" "$CMAKE_COMMAND_FILE"; do
  if [[ -e "$platform_output" && -L "$platform_output" ]]; then
    echo "Refusing symlink platform output: $platform_output" >&2
    exit 1
  fi
  mkdir -p "$(dirname -- "$platform_output")"
done
: > "$OUTPUT_FILE"
: > "$CMAKE_COMMAND_FILE"

platform=$(uname -s)
architecture=$(uname -m)

# @brief Load one canonical runtime record retained by ci_runner_verify.py.
# @return Twelve nonempty lines: architecture, image OS/version, runner label,
#   triplet/vcpkg, CMake path/version/module hash, and fuzz C/C++
#   compiler path/version. Inapplicable Linux fields use ``-``.
# @throws Python exits nonzero for non-canonical bytes, a stale allowlist
#   member, a mismatched platform, a link/special file, or unknown fields.
# @note The runner environment is deliberately not read here. Every platform
#   decision is derived from the one file produced before candidate execution.
read_retained_runner_identity() {
  python3 - "$SCRIPT_DIR" "$REPO_ROOT" "$RUNNER_IDENTITY_FILE" "$platform" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
from ci_runner_verify import load_resolved_identity

identity = load_resolved_identity(Path(sys.argv[3]), Path(sys.argv[2]), sys.argv[4])
print(identity["architecture"])
print(identity["image_os"])
print(identity["image_version"])
print(identity["runner_label"])
print(identity.get("triplet", "-"))
print(identity.get("vcpkg_commit", "-"))
print(identity.get("cmake_path", "-"))
print(identity.get("cmake_version", "-"))
print(identity.get("cmake_gtest_module_sha256", "-"))
print(identity.get("fuzz_c_compiler_path", "-"))
print(identity.get("fuzz_cxx_compiler_path", "-"))
print(identity.get("fuzz_compiler_version", "-"))
PY
}

runtime_identity=()
while IFS= read -r identity_value; do
  runtime_identity+=("$identity_value")
done < <(read_retained_runner_identity)
if ((${#runtime_identity[@]} != 12)); then
  echo "Retained runner identity did not yield twelve exact fields." >&2
  exit 1
fi
expected_architecture=${runtime_identity[0]}
expected_image_os=${runtime_identity[1]}
expected_image_version=${runtime_identity[2]}
expected_runner_label=${runtime_identity[3]}
triplet=${runtime_identity[4]}
expected_vcpkg_commit=${runtime_identity[5]}
expected_cmake_path=${runtime_identity[6]}
expected_cmake_version=${runtime_identity[7]}
expected_cmake_module_sha256=${runtime_identity[8]}
expected_fuzz_c_compiler=${runtime_identity[9]}
expected_fuzz_cxx_compiler=${runtime_identity[10]}
expected_fuzz_compiler_version=${runtime_identity[11]}
if [[ "$architecture" != "$expected_architecture" ]]; then
  echo "Runtime architecture '$architecture' differs from retained '$expected_architecture'." >&2
  exit 1
fi

# @brief Read and validate eligibility/dependencies from the resolved profile.
# @param $1 `eligible`, `dependencies`, or `profile`.
# @return The platform name or newline-delimited exact vcpkg port identities.
# @throws Python exits nonzero for malformed, unsupported, or duplicate data.
read_profile_platform() {
  local operation=$1
  python3 - "$PROFILE_JSON" "$operation" "$platform" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    root = json.load(handle)
profile = root.get("profile")
if not isinstance(profile, dict):
    raise SystemExit("resolved security profile is malformed")
platforms = profile.get("platforms")
if (
    not isinstance(platforms, list)
    or platforms != sorted(platforms)
    or len(platforms) != len(set(platforms))
    or not set(platforms).issubset({"Darwin", "Linux"})
):
    raise SystemExit("security profile platform eligibility is malformed")
platform = sys.argv[3]
if platform not in platforms:
    raise SystemExit(f"security profile is not eligible on {platform}")
if sys.argv[2] == "eligible":
    print(platform)
elif sys.argv[2] == "profile":
    profile_name = profile.get("profile")
    if profile_name not in {"sanitizer-asan", "sanitizer-tsan", "fuzz-codecs"}:
        raise SystemExit("security profile identity is unsupported")
    print(profile_name)
elif sys.argv[2] == "dependencies":
    dependencies = profile.get("vcpkg_dependencies")
    if (
        not isinstance(dependencies, list)
        or dependencies != sorted(dependencies)
        or len(dependencies) != len(set(dependencies))
        or not dependencies
        or not all(isinstance(item, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", item) for item in dependencies)
    ):
        raise SystemExit("security profile vcpkg dependency identities are malformed")
    print(*dependencies, sep="\n")
else:
    raise SystemExit("unknown platform profile operation")
PY
}

read_profile_platform eligible >/dev/null
profile_name=$(read_profile_platform profile)

# @brief Reject links, special files, or unreadable entries in a fresh tree.
# @param $1 Existing root that must be a real directory containing only real
#   directories and regular files.
# @return Zero only when every entry can be inspected without following links.
# @throws Python exits nonzero for a missing root, a symlink, a FIFO, a socket,
#   a device, or an entry that cannot be inspected.
validate_fresh_real_tree() {
  python3 - "$1" <<'PY'
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
try:
    root_mode = os.lstat(root).st_mode
except OSError as error:
    raise SystemExit(f"cannot inspect fresh vcpkg root: {error}") from error
if not stat.S_ISDIR(root_mode) or stat.S_ISLNK(root_mode):
    raise SystemExit("fresh vcpkg root is not a real directory")
for current, directories, files in os.walk(root, topdown=True, followlinks=False):
    for name in directories + files:
        candidate = Path(current) / name
        try:
            mode = os.lstat(candidate).st_mode
        except OSError as error:
            raise SystemExit(f"cannot inspect fresh vcpkg entry {candidate}: {error}") from error
        if stat.S_ISLNK(mode):
            raise SystemExit(f"fresh vcpkg checkout contains a link: {candidate}")
        if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
            raise SystemExit(f"fresh vcpkg checkout contains a special entry: {candidate}")
PY
}

# @brief Verify the exact runner-bound CMake executable and GoogleTest module.
# @return Zero only when path, version, CMAKE_ROOT, module type, and module
#   SHA-256 equal the retained runner member. The version query may carry only
#   the reviewed Android SDK ``-g<7 lowercase hex>`` vendor suffix; its parsed
#   semantic version must still equal the lock exactly.
# @throws Python reports bounded subprocess/file diagnostics and exits nonzero
#   before vcpkg, configure, build, or test execution.
# @note The selected CMake 3.31.5 module uses counter-suffixed GoogleTest
#   includes understood by current-main's candidate-owned inventory helper.
#   This protected stage does not broaden or modify that helper.
verify_darwin_cmake_contract() {
  python3 - "$expected_cmake_path" "$expected_cmake_version" \
    "$expected_cmake_module_sha256" <<'PY'
import hashlib
import os
import re
import stat
import subprocess
import sys
from pathlib import Path

command = Path(sys.argv[1])
version = sys.argv[2]
module_digest = sys.argv[3]
if not command.is_absolute() or not re.fullmatch(r"[1-9][0-9]*(?:\.[0-9]+){2}", version):
    raise SystemExit("retained Darwin CMake identity is malformed")
if not re.fullmatch(r"[0-9a-f]{64}", module_digest):
    raise SystemExit("retained Darwin GoogleTest module digest is malformed")
try:
    mode = os.lstat(command).st_mode
except OSError as error:
    raise SystemExit(f"cannot inspect retained Darwin CMake path: {error}") from error
if not stat.S_ISREG(mode) or stat.S_ISLNK(mode) or not os.access(command, os.X_OK):
    raise SystemExit("retained Darwin CMake path is not a real executable")

def run(arguments: list[str], label: str) -> str:
    """Run one bounded tool identity query and return strict UTF-8 stdout."""
    try:
        completed = subprocess.run(
            arguments,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"{label} failed: {error}") from error
    try:
        output = completed.stdout[:65536].decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise SystemExit(f"{label} returned non-UTF-8 output: {error}") from error
    if completed.returncode != 0 or len(completed.stdout) > 65536:
        raise SystemExit(f"{label} failed ({completed.returncode}):\n{output}")
    return output

version_output = run([str(command), "--version"], "Darwin CMake version query")
if (
    not version_output.endswith("\n")
    or any(
        ord(character) < 32 and character != "\n" or ord(character) == 127
        for character in version_output
    )
    or not version_output.isascii()
):
    raise SystemExit("Darwin CMake version output has a non-canonical byte shape")
version_lines = version_output[:-1].split("\n")
if len(version_lines) == 1:
    first_line = version_lines[0]
elif version_lines[1:] == [
    "",
    "CMake suite maintained and supported by Kitware (kitware.com/cmake).",
]:
    first_line = version_lines[0]
else:
    raise SystemExit("Darwin CMake version output has unexpected extra lines")
version_match = re.fullmatch(
    r"cmake version ([1-9][0-9]*(?:\.[0-9]+){2})(?:-g[0-9a-f]{7})?",
    first_line,
)
if version_match is None:
    raise SystemExit(
        f"Darwin CMake version line has an unsupported shape: {first_line!r}"
    )
if version_match.group(1) != version:
    raise SystemExit(
        "Darwin CMake semantic version differs: "
        f"expected {version!r}, observed {version_match.group(1)!r}"
    )
system_information = run(
    [str(command), "--system-information"], "Darwin CMake system-information query"
)
roots = re.findall(r'(?m)^CMAKE_ROOT "([^"\r\n]+)"$', system_information)
if len(roots) != 1:
    raise SystemExit("Darwin CMake reported no unique CMAKE_ROOT")
module = Path(roots[0]) / "Modules/GoogleTest.cmake"
try:
    module_mode = os.lstat(module).st_mode
    content = module.read_bytes()
except OSError as error:
    raise SystemExit(f"cannot inspect Darwin GoogleTest module: {error}") from error
if not stat.S_ISREG(module_mode) or stat.S_ISLNK(module_mode):
    raise SystemExit("Darwin GoogleTest module is not a real regular file")
if hashlib.sha256(content).hexdigest() != module_digest:
    raise SystemExit("Darwin GoogleTest module differs from its protected digest")
PY
}

# @brief Compile, link, and run a real bounded libFuzzer/ASan/UBSan probe.
# @return Zero only when both exact compilers report the retained version and
#   the C++ compiler builds and executes the combined sanitizer harness.
# @throws Python writes bounded compile/run logs beside the platform outputs,
#   prints at most 240 lines of real diagnostics, and exits nonzero on any
#   path/version/compile/link/run/timeout failure.
# @note Probe source/output live below a fresh runner.temp child and are removed
#   on success. Failure logs remain diagnostic data, never selector authority.
verify_darwin_fuzz_toolchain() {
  python3 - "$expected_fuzz_c_compiler" "$expected_fuzz_cxx_compiler" \
    "$expected_fuzz_compiler_version" "$CI_RUNNER_TEMP" \
    "$(dirname -- "$OUTPUT_FILE")" <<'PY'
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

c_compiler = Path(sys.argv[1])
cxx_compiler = Path(sys.argv[2])
version = sys.argv[3]
runner_temp = Path(sys.argv[4]).resolve()
diagnostic_root = Path(sys.argv[5])
if not re.fullmatch(r"[1-9][0-9]*(?:\.[0-9]+){2}", version):
    raise SystemExit("retained Darwin fuzz compiler version is malformed")
for compiler in (c_compiler, cxx_compiler):
    if not compiler.is_absolute() or not compiler.exists() or not os.access(compiler, os.X_OK):
        raise SystemExit(f"retained Darwin fuzz compiler is unavailable: {compiler}")
    try:
        completed = subprocess.run(
            [str(compiler), "--version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"Darwin fuzz compiler version query failed: {error}") from error
    output = completed.stdout[:65536].decode("utf-8", errors="replace")
    match = re.search(r"(?m)^(?:Homebrew )?clang version ([0-9]+(?:\.[0-9]+){2})\b", output)
    if completed.returncode != 0 or match is None or match.group(1) != version:
        raise SystemExit(
            f"Darwin fuzz compiler version differs for {compiler}:\n{output}"
        )

probe_root = Path(tempfile.mkdtemp(prefix="photospider-fuzz-toolchain.", dir=runner_temp))
source = probe_root / "probe.cpp"
binary = probe_root / "probe"
source.write_text(
    "#include <cstddef>\n"
    "#include <cstdint>\n"
    "extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }\n",
    encoding="utf-8",
)
compile_command = [
    str(cxx_compiler),
    "-std=c++17",
    "-fno-omit-frame-pointer",
    "-fsanitize=fuzzer,address,undefined",
    str(source),
    "-o",
    str(binary),
]
run_command = [
    str(binary),
    "-seed=1",
    "-runs=1",
    "-timeout=1",
    "-max_len=8",
]

def execute(arguments: list[str], timeout: int, label: str) -> subprocess.CompletedProcess[bytes]:
    """Run one real bounded probe phase and persist exact argv/output."""
    try:
        completed = subprocess.run(
            arguments,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        (diagnostic_root / f"darwin-fuzz-toolchain-{label}.log").write_text(
            json.dumps(arguments) + "\n" + str(error) + "\n", encoding="utf-8"
        )
        raise SystemExit(f"Darwin fuzz {label} probe failed: {error}") from error
    output = completed.stdout[:262144]
    (diagnostic_root / f"darwin-fuzz-toolchain-{label}.log").write_bytes(
        (json.dumps(arguments) + "\n").encode("utf-8") + output
    )
    if completed.returncode != 0 or len(completed.stdout) > 262144:
        diagnostic = output.decode("utf-8", errors="replace")
        raise SystemExit(
            f"Darwin fuzz {label} probe failed ({completed.returncode}):\n{diagnostic}"
        )
    return completed

try:
    execute(compile_command, 90, "compile-link")
    if not binary.is_file() or binary.is_symlink() or not os.access(binary, os.X_OK):
        raise SystemExit("Darwin fuzz compile/link probe produced no real executable")
    execute(run_command, 30, "run")
except BaseException:
    raise
else:
    shutil.rmtree(probe_root)
PY
}

case "$platform" in
  Linux)
    if [[ "$expected_image_os" != ubuntu24 ||
      "$expected_runner_label" != ubuntu-24.04 ||
      "$triplet" != - || "$expected_vcpkg_commit" != - ||
      "$expected_cmake_path" != - || "$expected_cmake_version" != - ||
      "$expected_cmake_module_sha256" != - ||
      "$expected_fuzz_c_compiler" != - ||
      "$expected_fuzz_cxx_compiler" != - ||
      "$expected_fuzz_compiler_version" != - ]]; then
      echo "Retained Linux runner identity has inconsistent platform fields." >&2
      exit 1
    fi
    # The locked image supplies clang-18. Fuzz selection is profile-driven;
    # harmless compiler cache entries are accepted by non-fuzz profiles.
    {
      printf '%s\n' '-DCMAKE_C_COMPILER=clang-18'
      printf '%s\n' '-DCMAKE_CXX_COMPILER=clang++-18'
    } > "$OUTPUT_FILE"
    printf '%s\n' cmake > "$CMAKE_COMMAND_FILE"
    ;;
  Darwin)
    if [[ "$expected_image_os" != macos15 ||
      "$expected_runner_label" != macos-15 ||
      "$triplet" != arm64-osx ||
      ! "$expected_vcpkg_commit" =~ ^[0-9a-f]{40}$ ||
      ! "$expected_cmake_path" =~ ^/[^[:cntrl:]]+$ ||
      ! "$expected_cmake_version" =~ ^[1-9][0-9]*(\.[0-9]+){2}$ ||
      ! "$expected_cmake_module_sha256" =~ ^[0-9a-f]{64}$ ||
      ! "$expected_fuzz_c_compiler" =~ ^/[^[:cntrl:]]+$ ||
      ! "$expected_fuzz_cxx_compiler" =~ ^/[^[:cntrl:]]+$ ||
      ! "$expected_fuzz_compiler_version" =~ ^[1-9][0-9]*(\.[0-9]+){2}$ ]]; then
      echo "Retained Darwin runner identity has inconsistent platform fields." >&2
      exit 1
    fi
    verify_darwin_cmake_contract
    printf '%s\n' "$expected_cmake_path" > "$CMAKE_COMMAND_FILE"
    if [[ "$profile_name" == fuzz-codecs ]]; then
      verify_darwin_fuzz_toolchain
    fi
    vcpkg_source_root=${VCPKG_INSTALLATION_ROOT:-}
    runner_temp=${CI_RUNNER_TEMP:-}
    if [[ -z "$vcpkg_source_root" || -L "$vcpkg_source_root" ||
      ! -d "$vcpkg_source_root" || -L "$vcpkg_source_root/.git" ||
      ! -d "$vcpkg_source_root/.git" || -L "$vcpkg_source_root/vcpkg" ||
      ! -f "$vcpkg_source_root/vcpkg" || ! -x "$vcpkg_source_root/vcpkg" ]]; then
      echo "GitHub-hosted vcpkg Git object source or binary is unavailable or unsafe." >&2
      exit 1
    fi
    if [[ -z "$runner_temp" || -L "$runner_temp" || ! -d "$runner_temp" ]]; then
      echo "Darwin security preparation requires a real CI_RUNNER_TEMP directory." >&2
      exit 1
    fi
    vcpkg_source_root=$(cd -- "$vcpkg_source_root" && pwd -P)
    runner_temp_real=$(cd -- "$runner_temp" && pwd -P)
    scratch_root=$(mktemp -d "$runner_temp_real/photospider-vcpkg.XXXXXX")
    if [[ -L "$scratch_root" || ! -d "$scratch_root" ]]; then
      echo "Darwin vcpkg scratch root is not a fresh real directory." >&2
      exit 1
    fi
    scratch_root=$(cd -- "$scratch_root" && pwd -P)
    if [[ ${scratch_root%/*} != "$runner_temp_real" ||
      ${scratch_root##*/} != photospider-vcpkg.* ]]; then
      echo "Darwin vcpkg scratch root is not a direct runner.temp child." >&2
      exit 1
    fi
    if [[ -n $(find "$scratch_root" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
      echo "Darwin vcpkg scratch root contains residual state." >&2
      exit 1
    fi
    fresh_vcpkg_root=$scratch_root/vcpkg
    if [[ -e "$fresh_vcpkg_root" || -L "$fresh_vcpkg_root" ]]; then
      echo "Darwin fresh vcpkg checkout target contains residual state." >&2
      exit 1
    fi

    GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git init --quiet --template= "$fresh_vcpkg_root"
    fetch_source=https://github.com/microsoft/vcpkg.git
    if GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$vcpkg_source_root" cat-file -e "$expected_vcpkg_commit^{commit}"; then
      fetch_source=$vcpkg_source_root
    fi
    GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" -c protocol.file.allow=always fetch \
      --force --no-tags --no-recurse-submodules --depth=1 \
      "$fetch_source" "$expected_vcpkg_commit"
    GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" checkout --quiet --detach \
      "$expected_vcpkg_commit"
    if ! actual_vcpkg_commit=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" rev-parse --verify HEAD^{commit}); then
      echo "Darwin fresh vcpkg checkout has no readable commit identity." >&2
      exit 1
    fi
    if [[ "$actual_vcpkg_commit" != "$expected_vcpkg_commit" ]]; then
      echo "Darwin fresh vcpkg commit '$actual_vcpkg_commit' differs from protected '$expected_vcpkg_commit'." >&2
      exit 1
    fi
    if ! checkout_status=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" status --porcelain=v1 \
      --untracked-files=all --ignore-submodules=none); then
      echo "Darwin fresh vcpkg checkout cleanliness cannot be verified." >&2
      exit 1
    fi
    if [[ -n "$checkout_status" ]]; then
      echo "Darwin fresh vcpkg checkout contains residual or modified state." >&2
      exit 1
    fi
    validate_fresh_real_tree "$fresh_vcpkg_root"
    toolchain_file=$fresh_vcpkg_root/scripts/buildsystems/vcpkg.cmake
    if [[ -L "$toolchain_file" || ! -f "$toolchain_file" ]]; then
      echo "Darwin fresh vcpkg checkout lacks a real toolchain file." >&2
      exit 1
    fi
    if [[ -e "$fresh_vcpkg_root/vcpkg" || -L "$fresh_vcpkg_root/vcpkg" ]]; then
      echo "Darwin fresh vcpkg binary target contains residual state." >&2
      exit 1
    fi
    cp -- "$vcpkg_source_root/vcpkg" "$fresh_vcpkg_root/vcpkg"
    chmod 700 "$fresh_vcpkg_root/vcpkg"
    if [[ -L "$fresh_vcpkg_root/vcpkg" || ! -f "$fresh_vcpkg_root/vcpkg" ||
      ! -x "$fresh_vcpkg_root/vcpkg" ]] ||
      ! cmp -s -- "$vcpkg_source_root/vcpkg" "$fresh_vcpkg_root/vcpkg"; then
      echo "Darwin image-bound vcpkg binary was not copied exactly." >&2
      exit 1
    fi
    validate_fresh_real_tree "$fresh_vcpkg_root"
    if ! checkout_status=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" status --porcelain=v1 \
      --untracked-files=all --ignore-submodules=none); then
      echo "Darwin fresh vcpkg checkout cleanliness cannot be reverified." >&2
      exit 1
    fi
    if [[ -n "$checkout_status" ]]; then
      echo "Darwin fresh vcpkg checkout changed while binding the runner binary." >&2
      exit 1
    fi
    dependencies=()
    while IFS= read -r dependency; do
      dependencies+=("$dependency")
    done < <(read_profile_platform dependencies)
    if ((${#dependencies[@]} == 0)); then
      echo "Darwin security profile resolved no dependencies." >&2
      exit 1
    fi
    install_root=$fresh_vcpkg_root/installed
    VCPKG_ROOT="$fresh_vcpkg_root" \
      VCPKG_BINARY_SOURCES=clear X_VCPKG_ASSET_SOURCES=clear \
      "$fresh_vcpkg_root/vcpkg" install \
      --triplet "$triplet" \
      --x-install-root "$install_root" \
      --clean-after-build \
      "${dependencies[@]}"
    {
      printf '%s\n' "-DCMAKE_TOOLCHAIN_FILE=$toolchain_file"
      printf '%s\n' "-DVCPKG_INSTALLED_DIR=$install_root"
      printf '%s\n' "-DVCPKG_TARGET_TRIPLET=$triplet"
      if [[ "$profile_name" == fuzz-codecs ]]; then
        printf '%s\n' "-DCMAKE_C_COMPILER=$expected_fuzz_c_compiler"
        printf '%s\n' "-DCMAKE_CXX_COMPILER=$expected_fuzz_cxx_compiler"
      fi
    } > "$OUTPUT_FILE"
    ;;
  *)
    echo "Unsupported security platform: $platform" >&2
    exit 1
    ;;
esac
