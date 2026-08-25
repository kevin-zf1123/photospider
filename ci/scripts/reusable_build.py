#!/usr/bin/env python3
"""Create and verify identity-bound reusable CI build archives.

The producer refuses cached output, measures CMake/compiler/toolchain/lock,
package, and generated identities from its fresh build tree, cross-checks the
candidate-owned versioned declaration, and only then writes a deterministic
archive plus canonical external manifest. The consumer must verify GitHub
attestations before invoking ``verify-extract``; this helper remeasures safely
staged archive bytes before atomic installation. Current ``main`` may use only
the exact protected fallback recognized by ``ci_profile_manifest``.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any


class ReusableBuildError(ValueError):
    """Report an unsafe archive or missing/mismatched reusable-build identity."""


def _load_profile_module(root: Path) -> Any:
    """Load the adjacent protected profile resolver without package assumptions."""
    module_path = root / "ci/scripts/ci_profile_manifest.py"
    specification = importlib.util.spec_from_file_location("photospider_ci_profile_manifest", module_path)
    if specification is None or specification.loader is None:
        raise ReusableBuildError("cannot load protected CI profile reader")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting duplicate members."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReusableBuildError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    """Read strict UTF-8 JSON with duplicate-key rejection."""
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError, ReusableBuildError) as error:
        raise ReusableBuildError(f"cannot read strict JSON {path}: {error}") from error


def _canonical_bytes(value: Any) -> bytes:
    """Return canonical compact JSON with one final newline."""
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def _sha256_file(path: Path) -> str:
    """Hash one regular file."""
    if not path.is_file() or path.is_symlink():
        raise ReusableBuildError(f"expected a regular file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _full_sha(value: str, context: str) -> str:
    """Require a lowercase forty-character Git commit identity."""
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ReusableBuildError(f"{context} must be a lowercase full Git SHA")
    return value


def _digest(value: str, context: str) -> str:
    """Require a canonical SHA-256 subject identity."""
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", value):
        raise ReusableBuildError(f"{context} must be an exact sha256 identity")
    return value


def _identifier(value: str, context: str) -> str:
    """Require one canonical profile or artifact-role identifier."""
    if not re.fullmatch(r"[a-z][a-z0-9-]*", value):
        raise ReusableBuildError(f"{context} is not a canonical identifier")
    return value


def _validate_string_map(value: Any, context: str) -> dict[str, str]:
    """Validate a nonempty bytewise-key-sorted string identity map."""
    if not isinstance(value, dict) or not value or list(value) != sorted(value):
        raise ReusableBuildError(f"{context} must be a nonempty sorted object")
    if not all(isinstance(key, str) and key and isinstance(item, str) and item for key, item in value.items()):
        raise ReusableBuildError(f"{context} must contain nonempty string identities")
    return value


def _validate_string_array(value: Any, context: str) -> list[str]:
    """Validate a sorted unique array containing at least one identity."""
    if not isinstance(value, list) or not value or not all(
        isinstance(item, str) and item for item in value
    ):
        raise ReusableBuildError(f"{context} must be a nonempty string array")
    if value != sorted(value) or len(value) != len(set(value)):
        raise ReusableBuildError(f"{context} must be sorted and unique")
    return value


def _validate_internal_identity(
    value: Any,
    identity_mode: str,
    candidate_commit: str,
    profile: str,
    matrix_sha256: str,
) -> None:
    """Validate the embedded candidate identity against external manifest fields."""
    expected_fields = {
        "schema", "candidate_commit", "profile", "matrix_sha256", "cmake", "compiler",
        "toolchain", "dependencies", "package_components", "package_targets",
        "generated",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        raise ReusableBuildError("internal reusable identity has missing or unknown fields")
    expected_schema = {
        "current-main-fallback": "photospider-current-main-reusable-identity-v1",
        "versioned": "photospider-reusable-build-identity-v1",
    }.get(identity_mode)
    if expected_schema is None or value["schema"] != expected_schema:
        raise ReusableBuildError("internal reusable identity mode/schema mismatch")
    expected_values = {
        "candidate_commit": candidate_commit,
        "profile": profile,
        "matrix_sha256": matrix_sha256,
    }
    for field, expected in expected_values.items():
        if value[field] != expected:
            raise ReusableBuildError(f"internal reusable identity {field} mismatch")
    for field in ("cmake", "compiler", "toolchain", "dependencies", "generated"):
        _validate_string_map(value[field], f"internal_identity:{field}")
    for field in ("package_components", "package_targets"):
        _validate_string_array(value[field], f"internal_identity:{field}")


def _git_head(root: Path) -> str:
    """Read the exact candidate HEAD used by the protected producer/consumer."""
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "HEAD^{commit}"],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise ReusableBuildError(f"cannot read candidate Git HEAD: {completed.stderr.strip()}")
    return _full_sha(completed.stdout.strip(), "candidate Git HEAD")


def _parse_cmake_set(path: Path, key: str) -> str:
    """Read exactly one quoted or unquoted scalar from a generated CMake module."""
    if not path.is_file() or path.is_symlink():
        raise ReusableBuildError(f"generated CMake identity is not a regular file: {path}")
    text = path.read_text(encoding="utf-8")
    matches = re.findall(
        rf"(?m)^set\({re.escape(key)}\s+(?:\"([^\"]*)\"|([^\s\)]+))\s*\)$", text
    )
    if len(matches) != 1:
        raise ReusableBuildError(f"{path}: expected exactly one {key} identity")
    value = matches[0][0] or matches[0][1]
    if not value:
        raise ReusableBuildError(f"{path}: {key} identity is empty")
    return value


def _cmake_cache(build: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Measure all maintained configuration inputs from one CMake cache.

    Returns both the complete parsed cache and the canonical identity subset.
    Empty cache values are represented explicitly as ``<empty>`` so absence and
    an intentionally empty setting cannot alias each other.
    """
    path = build / "CMakeCache.txt"
    if not path.is_file() or path.is_symlink():
        raise ReusableBuildError(f"CMake cache is missing or unsafe: {path}")
    complete: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith(("#", "//")):
            continue
        match = re.fullmatch(r"([^:=]+):([^=]+)=(.*)", line)
        if match is None:
            continue
        key, _, value = match.groups()
        if key in complete:
            raise ReusableBuildError(f"{path}:{line_number}: duplicate CMake cache key {key}")
        complete[key] = value
    maintained = {
        key: value if value else "<empty>"
        for key, value in complete.items()
        if key
        in {
            "BUILD_TESTING",
            "CMAKE_BUILD_TYPE",
            "CMAKE_GENERATOR",
            "CMAKE_GENERATOR_PLATFORM",
            "CMAKE_GENERATOR_TOOLSET",
            "CMAKE_PROJECT_NAME",
            "CMAKE_PROJECT_VERSION",
            "USE_ASAN",
            "USE_TSAN",
        }
        or re.fullmatch(r"PHOTOSPIDER_[A-Z0-9_]+", key)
    }
    mandatory = {
        "BUILD_TESTING",
        "CMAKE_BUILD_TYPE",
        "CMAKE_GENERATOR",
        "CMAKE_PROJECT_NAME",
        "USE_ASAN",
        "USE_TSAN",
    }
    missing = mandatory - set(maintained)
    if missing:
        raise ReusableBuildError(f"{path}: missing maintained CMake identities {sorted(missing)}")
    return complete, dict(sorted(maintained.items()))


def _single_generated_module(build: Path, name: str) -> Path:
    """Select exactly one generated CMake platform/compiler module."""
    matches = sorted(build.glob(f"CMakeFiles/*/{name}"))
    if len(matches) != 1 or matches[0].is_symlink():
        raise ReusableBuildError(f"expected exactly one regular generated {name} module")
    return matches[0]


def _measure_compiler(build: Path, cache: dict[str, str]) -> dict[str, str]:
    """Measure compiler path, ID, version, and generated detector bytes."""
    module = _single_generated_module(build, "CMakeCXXCompiler.cmake")
    compiler = _parse_cmake_set(module, "CMAKE_CXX_COMPILER")
    if cache.get("CMAKE_CXX_COMPILER") != compiler:
        raise ReusableBuildError("CMake cache and generated compiler path differ")
    return {
        "cxx_compiler": compiler,
        "cxx_compiler_id": _parse_cmake_set(module, "CMAKE_CXX_COMPILER_ID"),
        "cxx_compiler_module_sha256": _sha256_file(module),
        "cxx_compiler_version": _parse_cmake_set(module, "CMAKE_CXX_COMPILER_VERSION"),
    }


def _measure_toolchain(build: Path, cache: dict[str, str]) -> dict[str, str]:
    """Measure generated host platform plus the configured toolchain bytes."""
    module = _single_generated_module(build, "CMakeSystem.cmake")
    toolchain_text = cache.get("CMAKE_TOOLCHAIN_FILE", "")
    result = {
        "cmake_system_module_sha256": _sha256_file(module),
        "system_name": _parse_cmake_set(module, "CMAKE_SYSTEM_NAME"),
        "system_processor": _parse_cmake_set(module, "CMAKE_SYSTEM_PROCESSOR"),
        "toolchain_file": toolchain_text or "none",
        "toolchain_sha256": "none",
    }
    if toolchain_text:
        toolchain = Path(toolchain_text)
        if not toolchain.is_absolute():
            toolchain = build / toolchain
        result["toolchain_sha256"] = _sha256_file(toolchain)
    return result


def _package_inventory(build: Path) -> tuple[Path, list[str], list[str]]:
    """Measure the configured package component and exported-target surface."""
    config = build / "PhotospiderConfig.cmake"
    if not config.is_file() or config.is_symlink():
        raise ReusableBuildError(f"configured package file is missing or unsafe: {config}")
    text = config.read_text(encoding="utf-8")
    components = sorted(
        set(
            re.findall(
                r"(?m)^set\(Photospider_([a-z][a-z0-9_]*)_FOUND\s+(?:TRUE|ON|1)\s*\)$",
                text,
            )
        )
    )
    exports = sorted((build / "CMakeFiles/Export").glob("**/*.cmake"))
    if not exports or any(not path.is_file() or path.is_symlink() for path in exports):
        raise ReusableBuildError("configured package export inventory is empty or unsafe")
    targets: set[str] = set()
    for path in exports:
        targets.update(
            re.findall(
                r"(?m)^add_(?:library|executable)\(Photospider::([A-Za-z][A-Za-z0-9_]*)\b",
                path.read_text(encoding="utf-8"),
            )
        )
    if not components or not targets:
        raise ReusableBuildError("configured package component/target inventory is empty")
    return config, components, sorted(targets)


def _measure_generated(inventory: Path) -> dict[str, str]:
    """Hash every generated CI inventory file except its self-describing identity."""
    if not inventory.is_dir() or inventory.is_symlink():
        raise ReusableBuildError(f"generated CI inventory is missing or unsafe: {inventory}")
    result: dict[str, str] = {}
    for path in sorted(inventory.rglob("*")):
        if path.is_symlink() or not path.is_file():
            raise ReusableBuildError(f"generated CI inventory contains an unsafe entry: {path}")
        relative = path.relative_to(inventory).as_posix()
        if relative == "reusable_build_identity_v1.json":
            continue
        result[relative] = _sha256_file(path)
    if not result:
        raise ReusableBuildError("generated CI inventory identity is empty")
    return result


def _measure_dependencies(root: Path, package_config: Path) -> dict[str, str]:
    """Hash every protected dependency lock plus configured package contract.

    The completion stamp is validated separately as protected producer state
    and is already covered by the final archive digest. Keeping it outside the
    candidate-declared semantic identity avoids a timestamp/self-order cycle.
    """
    result: dict[str, str] = {}
    lock_root = root / "ci/locks"
    for path in sorted(lock_root.iterdir()):
        if path.name == "README.md":
            continue
        if not path.is_file() or path.is_symlink():
            raise ReusableBuildError(f"protected lock surface contains an unsafe entry: {path}")
        result[f"protected-lock:{path.name}"] = _sha256_file(path)
    result["generated-package-config:PhotospiderConfig.cmake"] = _sha256_file(package_config)
    return dict(sorted(result.items()))


def _validate_fresh_stamp(
    root: Path,
    build: Path,
    candidate: str,
    profile: str,
    cache: dict[str, str],
) -> None:
    """Cross-bind a fresh producer stamp to repository, profile, and cache."""
    path = build / ".photospider-ci-build-complete"
    if not path.is_file() or path.is_symlink():
        raise ReusableBuildError("fresh producer build has no regular completion stamp")
    records: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if "=" not in line:
            raise ReusableBuildError(f"{path}:{line_number}: malformed completion record")
        key, value = line.split("=", 1)
        if not key or not value or key in records:
            raise ReusableBuildError(f"{path}:{line_number}: empty or duplicate completion record")
        records[key] = value
    expected_keys = {
        "build_dir", "source_dir", "profile", "build_testing",
        "photospider_build_ipc", "candidate_commit", "created_at",
    }
    if set(records) != expected_keys:
        raise ReusableBuildError("fresh producer completion stamp fields differ")
    expected = {
        "build_dir": str(build.resolve()),
        "source_dir": str(root.resolve()),
        "profile": profile,
        "candidate_commit": candidate,
    }
    for key, value in expected.items():
        if records[key] != value:
            raise ReusableBuildError(f"fresh producer completion stamp {key} mismatch")
    expected_cache = {
        "build_testing": cache.get("BUILD_TESTING", ""),
        "photospider_build_ipc": cache.get("PHOTOSPIDER_BUILD_IPC", "not-defined"),
    }
    for key, value in expected_cache.items():
        if not value or records[key] != value:
            raise ReusableBuildError(f"fresh producer completion stamp {key} differs from CMake cache")
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", records["created_at"]):
        raise ReusableBuildError("fresh producer completion timestamp is malformed")


def _measure_internal_identity(
    root: Path,
    build: Path,
    inventory: Path,
    candidate_commit: str,
    profile: str,
    matrix_sha256: str,
    fallback: bool,
    *,
    require_fresh_origin: bool,
) -> dict[str, Any]:
    """Measure semantic identity and cross-check the candidate-declared copy."""
    if not build.is_dir() or build.is_symlink():
        raise ReusableBuildError(f"reusable source is not a regular build directory: {build}")
    cache, cmake = _cmake_cache(build)
    if require_fresh_origin:
        _validate_fresh_stamp(root, build, candidate_commit, profile, cache)
    package_config, package_components, package_targets = _package_inventory(build)
    measured = _measured_identity_value(
        root,
        build,
        inventory,
        candidate_commit,
        profile,
        matrix_sha256,
        fallback,
        cache,
        cmake,
        package_config,
        package_components,
        package_targets,
    )
    identity_path = inventory / "reusable_build_identity_v1.json"
    if fallback:
        if identity_path.exists() or identity_path.is_symlink():
            raise ReusableBuildError("current-main fallback rejects caller-provided reusable identity")
    else:
        candidate_identity = _load_json(identity_path)
        if identity_path.read_bytes() != _canonical_bytes(candidate_identity):
            raise ReusableBuildError("candidate reusable identity is not canonical JSON")
        if candidate_identity != measured:
            raise ReusableBuildError("candidate reusable identity differs from protected measurement")
    return measured


def _measured_identity_value(
    root: Path,
    build: Path,
    inventory: Path,
    candidate_commit: str,
    profile: str,
    matrix_sha256: str,
    fallback: bool,
    cache: dict[str, str] | None = None,
    cmake: dict[str, str] | None = None,
    package_config: Path | None = None,
    package_components: list[str] | None = None,
    package_targets: list[str] | None = None,
) -> dict[str, Any]:
    """Return the build-owned identity without consulting caller JSON.

    Optional already-measured values keep the producer path single-pass. Tests
    and future candidate generators may call the same deterministic routine to
    materialize the versioned declaration that the protected producer then
    compares byte-for-byte.
    """
    if cache is None or cmake is None:
        cache, cmake = _cmake_cache(build)
    if package_config is None or package_components is None or package_targets is None:
        package_config, package_components, package_targets = _package_inventory(build)
    measured = {
        "candidate_commit": candidate_commit,
        "cmake": cmake,
        "compiler": _measure_compiler(build, cache),
        "dependencies": _measure_dependencies(root, package_config),
        "generated": _measure_generated(inventory),
        "matrix_sha256": matrix_sha256,
        "package_components": package_components,
        "package_targets": package_targets,
        "profile": profile,
        "schema": (
            "photospider-current-main-reusable-identity-v1"
            if fallback
            else "photospider-reusable-build-identity-v1"
        ),
        "toolchain": _measure_toolchain(build, cache),
    }
    _validate_internal_identity(
        measured,
        "current-main-fallback" if fallback else "versioned",
        candidate_commit,
        profile,
        matrix_sha256,
    )
    return measured


def _archive_entries(source: Path) -> list[Path]:
    """Return sorted regular directories/files and reject links or special entries."""
    if not source.is_dir() or source.is_symlink():
        raise ReusableBuildError(f"build source is not a regular directory: {source}")
    entries = [source]
    entries.extend(sorted(source.rglob("*"), key=lambda path: path.relative_to(source).as_posix()))
    if len(entries) > 100_000:
        raise ReusableBuildError("reusable build contains too many entries")
    for entry in entries:
        if entry.is_symlink() or not (entry.is_dir() or entry.is_file()):
            raise ReusableBuildError(f"reusable build contains a link or special entry: {entry}")
        relative = entry.relative_to(source).as_posix()
        if any(ord(character) < 0x20 for character in relative):
            raise ReusableBuildError(f"reusable build contains a control-character path: {relative!r}")
    return entries


def _write_archive(source: Path, output: Path) -> None:
    """Write a deterministic gzip-compressed POSIX tar rooted at ``ci``."""
    if output.exists() and output.is_symlink():
        raise ReusableBuildError(f"refusing symlink archive output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
    entries = _archive_entries(source)
    with temporary.open("wb") as raw_handle:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_handle, mtime=0) as gzip_handle:
            with tarfile.open(fileobj=gzip_handle, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for entry in entries:
                    relative = entry.relative_to(source)
                    archive_name = "ci" if not relative.parts else f"ci/{relative.as_posix()}"
                    information = archive.gettarinfo(str(entry), arcname=archive_name)
                    information.uid = 0
                    information.gid = 0
                    information.uname = ""
                    information.gname = ""
                    information.mtime = 0
                    information.pax_headers = {}
                    information.mode = 0o755 if entry.is_dir() or entry.stat().st_mode & 0o111 else 0o644
                    if entry.is_file():
                        with entry.open("rb") as input_handle:
                            archive.addfile(information, input_handle)
                    else:
                        archive.addfile(information)
    temporary.replace(output)


def _write_json(path: Path, value: dict[str, Any]) -> None:
    """Atomically write one canonical manifest without following a symlink."""
    if path.exists() and path.is_symlink():
        raise ReusableBuildError(f"refusing symlink manifest output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    temporary.write_bytes(_canonical_bytes(value))
    temporary.replace(path)


def _create(arguments: argparse.Namespace) -> None:
    """Measure a fresh build, cross-check candidate identity, then archive it."""
    root = arguments.repo_root
    inventory = arguments.inventory_dir
    module = _load_profile_module(root)
    try:
        resolved = module.resolve(root, inventory)
    except Exception as error:  # The loaded module owns its detailed diagnostics.
        raise ReusableBuildError(f"profile identity is invalid: {error}") from error
    candidate = _full_sha(arguments.candidate_commit, "candidate commit")
    workflow = _full_sha(arguments.workflow_commit, "producer workflow commit")
    profile = _identifier(arguments.profile, "profile")
    image = _digest(arguments.image_digest, "CI image digest")
    if _git_head(root) != candidate:
        raise ReusableBuildError("candidate commit differs from checked-out Git HEAD")
    try:
        inventory_relative = inventory.resolve().relative_to(arguments.source.resolve())
    except ValueError as error:
        raise ReusableBuildError("generated inventory is outside the reusable build source") from error
    if inventory_relative != Path("generated/ci_inventory"):
        raise ReusableBuildError("generated inventory is not at generated/ci_inventory in the build")
    internal = _measure_internal_identity(
        root,
        arguments.source,
        inventory,
        candidate,
        profile,
        resolved["matrix_sha256"],
        resolved["fallback"],
        require_fresh_origin=True,
    )
    _write_archive(arguments.source, arguments.archive)
    manifest = {
        "artifact": {
            "name": arguments.archive.name,
            "sha256": _sha256_file(arguments.archive),
            "size": arguments.archive.stat().st_size,
        },
        "candidate_commit": candidate,
        "ci_image_digest": image,
        "identity_mode": "current-main-fallback" if resolved["fallback"] else "versioned",
        "internal_identity": internal,
        "matrix_sha256": resolved["matrix_sha256"],
        "producer_workflow_commit": workflow,
        "profile": profile,
        "schema": "photospider-reusable-build-manifest-v1",
    }
    _write_json(arguments.manifest, manifest)
    print(_sha256_file(arguments.manifest))


def _validated_members(archive: tarfile.TarFile) -> list[tarfile.TarInfo]:
    """Validate every member before extraction and return the safe inventory."""
    members = archive.getmembers()
    if not members or len(members) > 100_000:
        raise ReusableBuildError("archive is empty or exceeds the entry limit")
    seen: set[str] = set()
    total_size = 0
    for member in members:
        name = member.name
        pure = PurePosixPath(name)
        if (
            not name
            or name.startswith("/")
            or "\\" in name
            or any(part in ("", ".", "..") for part in pure.parts)
            or pure.parts[0] != "ci"
        ):
            raise ReusableBuildError(f"unsafe archive member path: {name!r}")
        canonical = pure.as_posix()
        if canonical != name or any(ord(character) < 0x20 for character in name):
            raise ReusableBuildError(f"non-canonical archive member path: {name!r}")
        if canonical in seen:
            raise ReusableBuildError(f"duplicate archive member: {canonical}")
        seen.add(canonical)
        if not (member.isfile() or member.isdir()):
            raise ReusableBuildError(f"link or special archive member: {canonical}")
        total_size += member.size
        if total_size > 8 * 1024 * 1024 * 1024:
            raise ReusableBuildError("archive uncompressed size exceeds 8 GiB")
    if "ci" not in seen:
        raise ReusableBuildError("archive does not contain the required ci root")
    return members


def _extract_members(
    archive: tarfile.TarFile, members: list[tarfile.TarInfo], stage: Path
) -> None:
    """Extract prevalidated regular members without tar traversal helpers."""
    for member in members:
        pure = PurePosixPath(member.name)
        target = stage.joinpath(*pure.parts)
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            target.chmod(member.mode & 0o777)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        extracted = archive.extractfile(member)
        if extracted is None:
            raise ReusableBuildError(f"cannot read archive member: {member.name}")
        with target.open("wb") as output:
            shutil.copyfileobj(extracted, output)
        target.chmod(member.mode & 0o777)


def _verify_archived_measurement(
    arguments: argparse.Namespace, manifest: dict[str, Any]
) -> None:
    """Remeasure safely staged archive bytes and match the signed identity."""
    with tempfile.TemporaryDirectory(prefix="photospider-reusable-verify-") as stage_text:
        stage = Path(stage_text)
        with tarfile.open(arguments.archive, mode="r:gz") as archive:
            members = _validated_members(archive)
            _extract_members(archive, members, stage)
        build = stage / "ci"
        inventory = build / "generated/ci_inventory"
        module = _load_profile_module(arguments.repo_root)
        try:
            resolved = module.resolve(arguments.repo_root, inventory)
        except Exception as error:  # The loaded module owns its detailed diagnostics.
            raise ReusableBuildError(f"archived profile identity is invalid: {error}") from error
        fallback = manifest["identity_mode"] == "current-main-fallback"
        if resolved["fallback"] != fallback:
            raise ReusableBuildError("archived profile mode differs from reusable manifest")
        if resolved["matrix_sha256"] != manifest["matrix_sha256"]:
            raise ReusableBuildError("archived matrix digest differs from reusable manifest")
        measured = _measure_internal_identity(
            arguments.repo_root,
            build,
            inventory,
            manifest["candidate_commit"],
            manifest["profile"],
            manifest["matrix_sha256"],
            fallback,
            require_fresh_origin=False,
        )
        if measured != manifest["internal_identity"]:
            raise ReusableBuildError("archived build identity differs from protected measurement")


def _verify_manifest(arguments: argparse.Namespace) -> dict[str, Any]:
    """Validate canonical manifest structure, expected identities, and artifact bytes."""
    value = _load_json(arguments.manifest)
    expected_fields = {
        "schema", "artifact", "candidate_commit", "profile", "matrix_sha256",
        "ci_image_digest", "producer_workflow_commit", "identity_mode", "internal_identity",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        raise ReusableBuildError("reusable build manifest has missing or unknown fields")
    if value["schema"] != "photospider-reusable-build-manifest-v1":
        raise ReusableBuildError("unknown reusable build manifest schema")
    if arguments.manifest.read_bytes() != _canonical_bytes(value):
        raise ReusableBuildError("reusable build manifest is not canonical JSON")
    expected_values = {
        "candidate_commit": _full_sha(arguments.candidate_commit, "candidate commit"),
        "profile": _identifier(arguments.profile, "profile"),
        "matrix_sha256": arguments.matrix_sha256,
        "ci_image_digest": _digest(arguments.image_digest, "CI image digest"),
        "producer_workflow_commit": _full_sha(arguments.workflow_commit, "workflow commit"),
    }
    if not re.fullmatch(r"[0-9a-f]{64}", arguments.matrix_sha256):
        raise ReusableBuildError("matrix SHA-256 is malformed")
    for field, expected in expected_values.items():
        if value[field] != expected:
            raise ReusableBuildError(f"manifest {field} mismatch")
    if _git_head(arguments.repo_root) != expected_values["candidate_commit"]:
        raise ReusableBuildError("expected candidate differs from checked-out Git HEAD")
    _validate_internal_identity(
        value["internal_identity"],
        value["identity_mode"],
        value["candidate_commit"],
        value["profile"],
        value["matrix_sha256"],
    )
    artifact = value["artifact"]
    if not isinstance(artifact, dict) or set(artifact) != {"name", "sha256", "size"}:
        raise ReusableBuildError("manifest artifact identity is malformed")
    if artifact["name"] != arguments.archive.name:
        raise ReusableBuildError("manifest artifact name mismatch")
    if artifact["sha256"] != _sha256_file(arguments.archive):
        raise ReusableBuildError("manifest artifact digest mismatch")
    if artifact["size"] != arguments.archive.stat().st_size:
        raise ReusableBuildError("manifest artifact size mismatch")
    _verify_archived_measurement(arguments, value)
    return value


def _verify_extract(arguments: argparse.Namespace) -> None:
    """Verify all identities and archive members, then atomically install ``ci``."""
    _verify_manifest(arguments)
    arguments.destination.mkdir(parents=True, exist_ok=True)
    if arguments.destination.is_symlink():
        raise ReusableBuildError("extraction destination must not be a symlink")
    with tarfile.open(arguments.archive, mode="r:gz") as archive:
        members = _validated_members(archive)
        with tempfile.TemporaryDirectory(prefix=".ci-reusable-", dir=arguments.destination) as stage_text:
            stage = Path(stage_text)
            _extract_members(archive, members, stage)
            staged_ci = stage / "ci"
            target_ci = arguments.destination / "ci"
            backup = arguments.destination / f".ci-reusable-backup-{os.getpid()}"
            if backup.exists():
                raise ReusableBuildError(f"unexpected extraction backup exists: {backup}")
            if target_ci.exists() or target_ci.is_symlink():
                target_ci.replace(backup)
            try:
                staged_ci.replace(target_ci)
            except Exception:
                if backup.exists() and not target_ci.exists():
                    backup.replace(target_ci)
                raise
            if backup.exists():
                if backup.is_dir() and not backup.is_symlink():
                    shutil.rmtree(backup)
                else:
                    backup.unlink()
    print("reusable build verification and safe extraction passed")


def _verify_only(arguments: argparse.Namespace) -> None:
    """Verify identities and every archive member without changing a build tree."""
    _verify_manifest(arguments)
    print("reusable build identity and archive verification passed")


def _add_expected(parser: argparse.ArgumentParser) -> None:
    """Add common exact consumer identity arguments."""
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--candidate-commit", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--matrix-sha256", required=True)
    parser.add_argument("--image-digest", required=True)
    parser.add_argument("--workflow-commit", required=True)


def build_parser() -> argparse.ArgumentParser:
    """Build the producer/consumer command-line interface."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--inventory-dir", type=Path, default=Path("build/generated/ci_inventory"))
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create", help="create archive and external manifest")
    create.add_argument("--source", type=Path, required=True)
    create.add_argument("--archive", type=Path, required=True)
    create.add_argument("--manifest", type=Path, required=True)
    create.add_argument("--candidate-commit", required=True)
    create.add_argument("--profile", required=True)
    create.add_argument("--image-digest", required=True)
    create.add_argument("--workflow-commit", required=True)
    create.set_defaults(handler=_create)
    verify = subparsers.add_parser("verify-extract", help="verify and safely extract archive")
    _add_expected(verify)
    verify.add_argument("--destination", type=Path, required=True)
    verify.set_defaults(handler=_verify_extract)
    verify_only = subparsers.add_parser("verify-only", help="verify without extracting archive")
    _add_expected(verify_only)
    verify_only.set_defaults(handler=_verify_only)
    return parser


def main() -> int:
    """Run the requested reusable-build operation with stable diagnostics."""
    parser = build_parser()
    arguments = parser.parse_args()
    arguments.repo_root = arguments.repo_root.resolve()
    if not arguments.inventory_dir.is_absolute():
        arguments.inventory_dir = arguments.repo_root / arguments.inventory_dir
    arguments.inventory_dir = arguments.inventory_dir.resolve()
    if hasattr(arguments, "source"):
        arguments.source = arguments.source.resolve()
    try:
        arguments.handler(arguments)
    except (ReusableBuildError, OSError, tarfile.TarError) as error:
        print(f"reusable build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
