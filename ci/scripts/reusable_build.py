#!/usr/bin/env python3
"""Create and verify identity-bound reusable CI build archives.

The producer refuses cached output, measures CMake/compiler/toolchain/lock,
package, and generated identities from its fresh build tree, cross-checks the
candidate-owned versioned declaration, and only then writes a deterministic
archive plus canonical external manifest. The consumer must verify GitHub
attestations before invoking ``verify-extract``; this helper remeasures safely
staged archive bytes before atomic installation. Current ``main`` may use only
the exact protected fallback recognized by ``ci_profile_manifest``.

The protected pre-attestation mode executes only from an exact workflow-control
checkout and treats a disjoint exact candidate checkout as data. It binds both
commits and directory objects, revalidates the raw/control identity, then
requires one ordinary CTest set across raw JSON, both targeted closures, and a
fresh restored inventory before targeted artifacts can be attested.
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
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Callable


class ReusableBuildError(ValueError):
    """Report an unsafe archive or missing/mismatched reusable-build identity."""


TARGETED_ARTIFACT_ROLES = frozenset(
    {"ctest-control", "ctest-runtime", "installed-package", "openexr-metadata"}
)
"""Exact consumer roles supported by the targeted reusable artifact format."""

_FORBIDDEN_RUNTIME_SUFFIXES = (
    ".a",
    ".d",
    ".gch",
    ".lib",
    ".o",
    ".obj",
    ".pch",
)
"""Build residue that runtime/control artifacts must never fan out."""

_FORBIDDEN_RUNTIME_NAMES = frozenset(
    {
        ".ninja_deps",
        ".ninja_log",
        "DependInfo.cmake",
        "build.make",
        "compiler_depend.make",
        "compiler_depend.ts",
        "depend.make",
        "flags.make",
        "link.txt",
        "progress.make",
    }
)
"""Generated dependency/build records excluded from targeted runtime payloads."""


class _ArchiveSnapshot:
    """Own one immutable-by-path regular archive object for verification.

    The snapshot opens the caller-visible path exactly once with link refusal,
    records type and size from that descriptor, and keeps the descriptor alive
    through digesting, member validation, archived measurement, and final
    extraction. Atomic pathname replacement can therefore change only what a
    later caller sees, never the bytes already accepted by this consumer.

    Args:
        path: Candidate archive path to open without following its final link.

    Raises:
        ReusableBuildError: The platform lacks the required no-follow open flag
            or the opened object is not a regular file.
        OSError: The path cannot be opened or inspected.

    Note:
        Darwin and Linux provide ``O_NOFOLLOW`` and are the maintained CI
        platforms. Each tar reader is sequential and rewinds this same handle;
        ``tarfile`` does not close a caller-owned file object.
    """

    def __init__(self, path: Path) -> None:
        if not hasattr(os, "O_NOFOLLOW"):
            raise ReusableBuildError(
                "archive verification requires an O_NOFOLLOW-capable platform"
            )
        flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_NONBLOCK", 0)
        flags |= getattr(os, "O_CLOEXEC", 0)
        descriptor = os.open(path, flags)
        try:
            metadata = os.fstat(descriptor)
            if not stat.S_ISREG(metadata.st_mode):
                raise ReusableBuildError(f"expected a regular archive file: {path}")
            self._handle = os.fdopen(descriptor, "rb", closefd=True)
        except Exception:
            os.close(descriptor)
            raise
        self.path = path
        self.size = metadata.st_size

    def __enter__(self) -> "_ArchiveSnapshot":
        """Return this live snapshot for one verification scope."""
        return self

    def __exit__(
        self, exception_type: object, exception: object, traceback: object
    ) -> None:
        """Close the retained archive descriptor at scope exit."""
        self._handle.close()

    def sha256(self) -> str:
        """Hash the retained archive bytes and rewind for the next reader."""
        digest = hashlib.sha256()
        self._handle.seek(0)
        for chunk in iter(lambda: self._handle.read(1024 * 1024), b""):
            digest.update(chunk)
        self._handle.seek(0)
        return digest.hexdigest()

    def open_tar(self) -> tarfile.TarFile:
        """Open a gzip tar reader over the retained descriptor.

        Returns:
            A caller-owned ``TarFile`` positioned at the snapshot start.

        Raises:
            tarfile.TarError: The retained bytes are not a readable gzip tar.

        Note:
            Callers must close the returned reader before requesting another;
            all verification phases are deliberately sequential.
        """
        self._handle.seek(0)
        return tarfile.open(fileobj=self._handle, mode="r:gz")


class _ManifestSnapshot:
    """Retain one regular manifest descriptor and its exact immutable bytes.

    The caller-visible pathname is opened once with final-component link
    refusal. JSON decoding, canonical-byte comparison, SHA-256 calculation,
    identity/content validation, and evidence generation all consume the same
    byte string read from that descriptor. A final path-identity check rejects
    atomic replacement instead of silently reporting evidence for an object no
    longer named by the accepted path.

    Args:
        path: Manifest pathname to open without following its final component.

    Raises:
        ReusableBuildError: The platform lacks ``O_NOFOLLOW``, the object is
            nonregular, too large, or changes while its bytes are captured.
        OSError: The pathname cannot be opened or inspected.

    Note:
        Targeted manifests are bounded control data, not payload archives. A
        64 MiB limit keeps a malicious JSON file from becoming an unbounded
        in-memory input while remaining far above the maintained manifests.
    """

    _MAX_BYTES = 64 * 1024 * 1024

    def __init__(self, path: Path) -> None:
        if not hasattr(os, "O_NOFOLLOW"):
            raise ReusableBuildError(
                "manifest verification requires an O_NOFOLLOW-capable platform"
            )
        flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_NONBLOCK", 0)
        flags |= getattr(os, "O_CLOEXEC", 0)
        descriptor = os.open(path, flags)
        try:
            metadata_before = os.fstat(descriptor)
            if not stat.S_ISREG(metadata_before.st_mode):
                raise ReusableBuildError(
                    f"expected a regular targeted manifest file: {path}"
                )
            if metadata_before.st_size > self._MAX_BYTES:
                raise ReusableBuildError("targeted manifest exceeds 64 MiB")
            self._handle = os.fdopen(descriptor, "rb", closefd=True)
            self.bytes = self._handle.read(self._MAX_BYTES + 1)
            metadata_after = os.fstat(self._handle.fileno())
            if len(self.bytes) > self._MAX_BYTES:
                raise ReusableBuildError("targeted manifest exceeds 64 MiB")
            if self._metadata_identity(metadata_before) != self._metadata_identity(
                metadata_after
            ) or len(self.bytes) != metadata_after.st_size:
                raise ReusableBuildError(
                    "targeted manifest changed while its snapshot was captured"
                )
        except Exception:
            if "self" in locals() and hasattr(self, "_handle"):
                self._handle.close()
            else:
                os.close(descriptor)
            raise
        self.path = path
        self._metadata = metadata_after

    @staticmethod
    def _metadata_identity(metadata: os.stat_result) -> tuple[int, ...]:
        """Return stable file/type/change fields used by snapshot checks."""
        return (
            metadata.st_dev,
            metadata.st_ino,
            metadata.st_mode,
            metadata.st_size,
            metadata.st_mtime_ns,
            metadata.st_ctime_ns,
        )

    def __enter__(self) -> "_ManifestSnapshot":
        """Return this live manifest snapshot for one verification scope."""
        return self

    def __exit__(
        self, exception_type: object, exception: object, traceback: object
    ) -> None:
        """Close the retained manifest descriptor at scope exit."""
        self._handle.close()

    def sha256(self) -> str:
        """Hash only the retained bytes captured from the opened descriptor."""
        return hashlib.sha256(self.bytes).hexdigest()

    def json_value(self) -> Any:
        """Decode strict UTF-8 JSON with duplicate-member rejection.

        Returns:
            Decoded JSON value from the retained byte string.

        Raises:
            ReusableBuildError: UTF-8, JSON syntax, or member uniqueness fails.
        """
        try:
            text = self.bytes.decode("utf-8")
            return json.loads(text, object_pairs_hook=_unique_object)
        except (UnicodeError, json.JSONDecodeError, ReusableBuildError) as error:
            raise ReusableBuildError(
                f"cannot read strict targeted manifest JSON {self.path}: {error}"
            ) from error

    def require_path_unchanged(self) -> None:
        """Require the accepted descriptor still owns the caller-visible path.

        Raises:
            ReusableBuildError: The descriptor changed in place or the path was
                deleted, linked, replaced, resized, or rewritten.
            OSError: Descriptor inspection fails unexpectedly.
        """
        descriptor_metadata = os.fstat(self._handle.fileno())
        try:
            path_metadata = os.stat(self.path, follow_symlinks=False)
        except OSError as error:
            raise ReusableBuildError(
                "targeted manifest pathname disappeared during verification"
            ) from error
        expected = self._metadata_identity(self._metadata)
        if (
            self._metadata_identity(descriptor_metadata) != expected
            or self._metadata_identity(path_metadata) != expected
            or not stat.S_ISREG(path_metadata.st_mode)
        ):
            raise ReusableBuildError(
                "targeted manifest pathname changed during verification"
            )


class _CheckoutSnapshot:
    """Bind one real checkout pathname to one exact commit for one operation.

    Args:
        path: Candidate-data or protected-control checkout root.
        context: Stable role used in diagnostics.
        expected_commit: Exact lowercase full Git commit required at ``HEAD``.

    Raises:
        ReusableBuildError: The root is missing, linked, non-directory, cannot
            resolve, or its ``HEAD`` differs from ``expected_commit``.

    Note:
        The directory object and resolved path are rechecked after artifact
        verification. Git metadata is read only; no checkout hook or candidate
        helper is executed.
    """

    def __init__(self, path: Path, context: str, expected_commit: str) -> None:
        self.path = path.absolute()
        self.context = context
        self.expected_commit = _full_sha(expected_commit, f"{context} commit")
        try:
            metadata = self.path.lstat()
            if not stat.S_ISDIR(metadata.st_mode) or self.path.is_symlink():
                raise ReusableBuildError(
                    f"{context} must be one real non-link directory"
                )
            self.resolved = self.path.resolve(strict=True)
        except OSError as error:
            raise ReusableBuildError(f"cannot resolve {context}: {error}") from error
        self._identity = (metadata.st_dev, metadata.st_ino, metadata.st_mode)
        if _git_head(self.resolved, context) != self.expected_commit:
            raise ReusableBuildError(f"{context} HEAD differs from expected commit")

    def require_unchanged(self) -> None:
        """Reject root replacement, aliasing, or ``HEAD`` drift after use.

        Raises:
            ReusableBuildError: Pathname metadata, resolved root, or commit no
                longer equals the retained checkout identity.
        """
        try:
            metadata = self.path.lstat()
            resolved = self.path.resolve(strict=True)
        except OSError as error:
            raise ReusableBuildError(
                f"{self.context} disappeared during verification"
            ) from error
        identity = (metadata.st_dev, metadata.st_ino, metadata.st_mode)
        if (
            not stat.S_ISDIR(metadata.st_mode)
            or self.path.is_symlink()
            or identity != self._identity
            or resolved != self.resolved
            or _git_head(self.resolved, self.context) != self.expected_commit
        ):
            raise ReusableBuildError(
                f"{self.context} changed during verification"
            )


def _load_profile_module(root: Path) -> Any:
    """Load the adjacent protected profile resolver without package assumptions."""
    module_path = root / "ci/scripts/ci_profile_manifest.py"
    specification = importlib.util.spec_from_file_location("photospider_ci_profile_manifest", module_path)
    if specification is None or specification.loader is None:
        raise ReusableBuildError("cannot load protected CI profile reader")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def _load_ctest_closure_module(root: Path) -> Any:
    """Load the adjacent protected ordinary-CTest closure implementation.

    Args:
        root: Exact checked-out repository containing protected CI scripts.

    Returns:
        Loaded module exposing role selection and restored-runtime validation.

    Raises:
        ReusableBuildError: The protected module cannot be located or loaded.

    Note:
        Loading by exact repository path avoids relying on caller-controlled
        ``PYTHONPATH`` or an installed package with a matching module name.
    """
    module_path = root / "ci/scripts/ctest_runtime_closure.py"
    specification = importlib.util.spec_from_file_location(
        "photospider_ctest_runtime_closure", module_path
    )
    if specification is None or specification.loader is None:
        raise ReusableBuildError("cannot load protected CTest closure reader")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def _load_build_smoke_route_module(root: Path) -> Any:
    """Load the protected raw/control routing implementation by exact path.

    Args:
        root: Exact protected control checkout root.

    Returns:
        Loaded module exposing retained raw/control bundle validation.

    Raises:
        ReusableBuildError: The protected module cannot be located or loaded.

    Note:
        Candidate checkout paths never participate in this import boundary.
    """
    module_path = root / "ci/scripts/build_smoke_route.py"
    specification = importlib.util.spec_from_file_location(
        "photospider_build_smoke_route", module_path
    )
    if specification is None or specification.loader is None:
        raise ReusableBuildError("cannot load protected build-smoke route reader")
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


def _git_head(root: Path, context: str = "candidate Git checkout") -> str:
    """Read one exact checkout ``HEAD`` without executing checked-out code.

    Args:
        root: Exact Git checkout root.
        context: Stable candidate/control role used in diagnostics.

    Returns:
        Lowercase full commit identity.

    Raises:
        ReusableBuildError: Git cannot resolve one canonical commit.
    """
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "HEAD^{commit}"],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise ReusableBuildError(
            f"cannot read {context} HEAD: {completed.stderr.strip()}"
        )
    return _full_sha(completed.stdout.strip(), f"{context} HEAD")


def _targeted_checkout_snapshots(
    arguments: argparse.Namespace,
) -> tuple[Path, list[_CheckoutSnapshot]]:
    """Bind protected code and candidate data to disjoint exact checkouts.

    Args:
        arguments: Targeted verifier arguments containing ``repo_root``,
            optional ``candidate_root``, and exact candidate/workflow commits.

    Returns:
        Resolved candidate-data root plus snapshots that must be rechecked
        after artifact use.

    Raises:
        ReusableBuildError: A checkout is linked, overlapping, commit-drifted,
            or a distinct protected control root is not the checkout from which
            this running helper was loaded.

    Note:
        Existing ordinary consumers use the same candidate root for code and
        data. The pre-attestation protected verifier supplies a distinct
        candidate checkout; in that mode only the control root may supply
        executable Python modules.
    """
    control_path = Path(arguments.repo_root).absolute()
    candidate_value = getattr(arguments, "candidate_root", None)
    candidate_path = (
        control_path if candidate_value is None else Path(candidate_value).absolute()
    )
    control_resolved = control_path.resolve(strict=True)
    candidate_resolved = candidate_path.resolve(strict=True)
    if control_resolved == candidate_resolved:
        snapshot = _CheckoutSnapshot(
            candidate_path, "candidate checkout", arguments.candidate_commit
        )
        return snapshot.resolved, [snapshot]
    if (
        control_resolved in candidate_resolved.parents
        or candidate_resolved in control_resolved.parents
    ):
        raise ReusableBuildError(
            "protected control and candidate checkout roots overlap"
        )
    script_root = Path(__file__).resolve().parents[2]
    if script_root != control_resolved:
        raise ReusableBuildError(
            "targeted verifier is not executing from its protected control root"
        )
    control_snapshot = _CheckoutSnapshot(
        control_path, "protected control checkout", arguments.workflow_commit
    )
    candidate_snapshot = _CheckoutSnapshot(
        candidate_path, "candidate data checkout", arguments.candidate_commit
    )
    return candidate_snapshot.resolved, [control_snapshot, candidate_snapshot]


def _require_disjoint_existing_directories(
    paths: dict[str, Path],
) -> dict[str, Path]:
    """Resolve real directories and reject every overlap or final link.

    Args:
        paths: Named existing directory boundaries.

    Returns:
        Resolved directory mapping.

    Raises:
        ReusableBuildError: A directory is missing, linked, non-directory, or
            aliases, contains, or is contained by another boundary.
    """
    resolved: dict[str, Path] = {}
    for name, path in paths.items():
        absolute = Path(path).absolute()
        if not absolute.is_dir() or absolute.is_symlink():
            raise ReusableBuildError(f"{name} is not one real directory")
        resolved[name] = absolute.resolve(strict=True)
    items = list(resolved.items())
    for index, (left_name, left) in enumerate(items):
        for right_name, right in items[index + 1 :]:
            if left == right or left in right.parents or right in left.parents:
                raise ReusableBuildError(
                    f"targeted verifier boundaries overlap: {left_name}={left}, "
                    f"{right_name}={right}"
                )
    return resolved


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


def _runtime_member_is_forbidden(relative: PurePosixPath) -> bool:
    """Return whether one runtime/control member is build residue.

    Args:
        relative: Canonical path below the build tree.

    Returns:
        True for object, dependency, precompiled-header, or static-library
        outputs that no runtime/control consumer may receive.

    Note:
        Installed-package artifacts use a separate role because their static
        product library is intentional package content rather than residue.
    """
    name = relative.name
    return name in _FORBIDDEN_RUNTIME_NAMES or name.endswith(
        _FORBIDDEN_RUNTIME_SUFFIXES
    )


def _targeted_payload_files(
    root: Path, build: Path, role: str, payload_source: Path | None
) -> list[tuple[Path, PurePosixPath]]:
    """Select exact regular files for one targeted consumer role.

    Args:
        root: Exact checkout containing the protected closure reader.
        build: Fresh measured producer build tree.
        role: One value from :data:`TARGETED_ARTIFACT_ROLES`.
        payload_source: Installed prefix for ``installed-package``; absent for
            CTest and OpenEXR metadata roles.

    Returns:
        Sorted ``(source, destination-relative)`` file tuples. Destination
        paths are relative to the archive's canonical ``ci`` root.

    Raises:
        ReusableBuildError: The role/source is invalid, a selected entry is an
            unsafe link or special file, forbidden residue is selected, or the
            role has no complete minimal surface.

    Note:
        CTest membership comes from the producer-written complete ordinary
        closure rather than suffix guesses or a hand-maintained test-name list.
        The installed role carries only its prefix plus the exact producer
        cache and public-header inventory consumed by package-input mode. The
        OpenEXR role carries only the producer cache needed to propagate its
        configured Darwin architecture into the fresh source-tree smoke.
        Versioned shared-library aliases are accepted only when their final
        target is a regular file inside the same role root; the target bytes
        are copied under the alias name so the archive itself contains no link.
    """
    _identifier(role, "targeted artifact role")
    if role not in TARGETED_ARTIFACT_ROLES:
        raise ReusableBuildError(f"unsupported targeted artifact role: {role}")
    if not build.is_dir() or build.is_symlink():
        raise ReusableBuildError("targeted artifact producer build is unsafe")

    selected: list[tuple[Path, PurePosixPath]] = []
    if role == "installed-package":
        if (
            payload_source is None
            or not payload_source.is_dir()
            or payload_source.is_symlink()
        ):
            raise ReusableBuildError("installed-package payload must be a real directory")
        for path in sorted(
            payload_source.rglob("*"),
            key=lambda item: item.relative_to(payload_source).as_posix(),
        ):
            payload_relative = PurePosixPath(
                path.relative_to(payload_source).as_posix()
            )
            if path.is_symlink():
                shared_library = payload_relative.name.endswith(
                    (".dylib", ".so")
                ) or ".so." in payload_relative.name
                if not shared_library:
                    raise ReusableBuildError(
                        f"installed package contains a non-library link: {path}"
                    )
                try:
                    resolved = path.resolve(strict=True)
                    resolved.relative_to(payload_source.resolve())
                except (OSError, ValueError) as error:
                    raise ReusableBuildError(
                        f"installed package link is dangling or escapes: {path}"
                    ) from error
                if not resolved.is_file() or resolved.is_symlink():
                    raise ReusableBuildError(
                        f"installed package link target is not regular: {path}"
                    )
                relative = PurePosixPath("installed") / payload_relative
                selected.append((resolved, relative))
                continue
            if path.is_dir():
                continue
            if not path.is_file():
                raise ReusableBuildError(
                    f"installed package contains a special file: {path}"
                )
            relative = PurePosixPath("installed") / payload_relative
            selected.append((path, relative))
        metadata_files = (
            (
                build / "CMakeCache.txt",
                PurePosixPath("producer/CMakeCache.txt"),
            ),
            (
                build
                / "generated/ci_inventory/installable_public_headers.txt",
                PurePosixPath(
                    "producer/generated/ci_inventory/"
                    "installable_public_headers.txt"
                ),
            ),
        )
        for source, relative in metadata_files:
            if not source.is_file() or source.is_symlink():
                raise ReusableBuildError(
                    f"installed-package producer metadata is unavailable: {source}"
                )
            selected.append((source, relative))
    elif role == "openexr-metadata":
        if payload_source is not None:
            raise ReusableBuildError(
                "openexr-metadata does not accept a payload source"
            )
        cache = build / "CMakeCache.txt"
        if not cache.is_file() or cache.is_symlink():
            raise ReusableBuildError(
                "openexr-metadata producer cache is unavailable"
            )
        selected.append((cache, PurePosixPath("producer/CMakeCache.txt")))
    else:
        if payload_source is not None:
            raise ReusableBuildError(
                "CTest targeted roles do not accept a payload source"
            )
        closure = _load_ctest_closure_module(root)
        try:
            role_paths = closure.expected_role_paths(build, role)
        except Exception as error:
            raise ReusableBuildError(
                f"ordinary CTest closure selection failed: {error}"
            ) from error
        for path_text in role_paths:
            relative = PurePosixPath(path_text)
            path = build.joinpath(*relative.parts)
            if path.is_symlink():
                shared_library = relative.name.endswith(
                    (".dylib", ".so")
                ) or ".so." in relative.name
                if role != "ctest-runtime" or not shared_library:
                    raise ReusableBuildError(
                        f"targeted artifact member is an unsupported link: {path}"
                    )
                try:
                    resolved = path.resolve(strict=True)
                    resolved.relative_to(build.resolve())
                except (OSError, ValueError) as error:
                    raise ReusableBuildError(
                        f"runtime library link is dangling or escapes: {path}"
                    ) from error
                if not resolved.is_file() or resolved.is_symlink():
                    raise ReusableBuildError(
                        f"runtime library link target is not regular: {path}"
                    )
                path = resolved
            if not path.is_file():
                raise ReusableBuildError(
                    f"targeted artifact member is missing or special: {path}"
                )
            if _runtime_member_is_forbidden(relative):
                raise ReusableBuildError(
                    f"ordinary CTest closure includes forbidden residue: {relative}"
                )
            selected.append((path, relative))

    if not selected:
        raise ReusableBuildError(f"targeted artifact role {role} is empty")
    destinations = [relative.as_posix() for _, relative in selected]
    if destinations != sorted(destinations) or len(destinations) != len(
        set(destinations)
    ):
        raise ReusableBuildError("targeted artifact members are not canonical and unique")
    _validate_targeted_role_paths(role, destinations)
    return selected


def _validate_targeted_role_paths(role: str, paths: list[str]) -> None:
    """Require one targeted member inventory to satisfy its role contract.

    Args:
        role: Exact targeted artifact role.
        paths: Sorted file paths below the archive's ``ci`` root.

    Raises:
        ReusableBuildError: Required CTest/package surfaces are absent or a
            runtime/control role contains forbidden build residue.
    """
    if not paths or paths != sorted(paths) or len(paths) != len(set(paths)):
        raise ReusableBuildError("targeted artifact path inventory is invalid")
    if role in {"ctest-control", "ctest-runtime"}:
        relatives = [PurePosixPath(path) for path in paths]
        if any(_runtime_member_is_forbidden(path) for path in relatives):
            raise ReusableBuildError(
                "targeted runtime artifact contains forbidden residue"
            )
        required = {".photospider-ci-build-complete", "CMakeCache.txt"}
        if not required.issubset(paths) or not any(
            PurePosixPath(path).name == "CTestTestfile.cmake" for path in paths
        ):
            raise ReusableBuildError("targeted CTest artifact lacks control metadata")
        closure_path = "generated/ci_inventory/ordinary_ctest_closure_v1.json"
        if closure_path not in paths:
            raise ReusableBuildError("targeted CTest artifact lacks closure metadata")
    elif role == "installed-package":
        metadata = {
            "producer/CMakeCache.txt",
            "producer/generated/ci_inventory/installable_public_headers.txt",
        }
        if not metadata.issubset(paths):
            raise ReusableBuildError("installed package lacks exact producer metadata")
        if any(
            not path.startswith("installed/") and path not in metadata
            for path in paths
        ):
            raise ReusableBuildError("installed-package member escaped its exact roles")
        for path in paths:
            relative = PurePosixPath(path)
            if path.startswith("producer/") and path not in metadata:
                raise ReusableBuildError(
                    "installed package contains undeclared producer state"
                )
            if path.startswith("producer/") and (
                "CMakeFiles" in relative.parts
                or relative.name.endswith(_FORBIDDEN_RUNTIME_SUFFIXES)
                or relative.name.startswith("CTest")
            ):
                raise ReusableBuildError(
                    "installed package contains forbidden producer residue"
                )
        if not any(path.endswith("PhotospiderConfig.cmake") for path in paths):
            raise ReusableBuildError("installed package lacks PhotospiderConfig.cmake")
        if not any(path.endswith((".hpp", ".h")) for path in paths):
            raise ReusableBuildError("installed package lacks public headers")
        if not any(path.endswith((".a", ".dylib", ".so")) for path in paths):
            raise ReusableBuildError("installed package lacks a product library")
    elif role == "openexr-metadata":
        if paths != ["producer/CMakeCache.txt"]:
            raise ReusableBuildError(
                "openexr-metadata must contain only producer/CMakeCache.txt"
            )
    else:
        raise ReusableBuildError(f"unsupported targeted artifact role: {role}")


def _copy_targeted_payload(
    root: Path,
    build: Path,
    role: str,
    payload_source: Path | None,
    stage: Path,
) -> None:
    """Copy one role's exact selected files into a private archive stage."""
    for source, relative in _targeted_payload_files(
        root, build, role, payload_source
    ):
        target = stage.joinpath(*relative.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(0o755 if source.stat().st_mode & 0o111 else 0o644)


def _targeted_content(root: Path) -> dict[str, Any]:
    """Measure exact file members and bytes below one targeted ``ci`` root."""
    members: list[dict[str, Any]] = []
    total_size = 0
    for path in sorted(
        root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()
    ):
        if path.is_symlink():
            raise ReusableBuildError(f"targeted content contains a link: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ReusableBuildError(
                f"targeted content contains a special file: {path}"
            )
        relative = path.relative_to(root).as_posix()
        size = path.stat().st_size
        total_size += size
        members.append(
            {
                "executable": bool(path.stat().st_mode & 0o111),
                "path": relative,
                "sha256": _sha256_file(path),
                "size": size,
            }
        )
    if not members:
        raise ReusableBuildError("targeted content inventory is empty")
    return {"members": members, "uncompressed_size": total_size}


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


def _create_targeted(arguments: argparse.Namespace) -> None:
    """Measure one fresh producer and emit a role-minimal reusable artifact.

    Args:
        arguments: Parsed producer identities, full build source, targeted role,
            optional installed prefix, and output paths.

    Raises:
        ReusableBuildError: Producer identity, role selection, or content
            boundaries are incomplete, unsafe, or mismatched.

    Note:
        Semantic build identity is always measured from the complete fresh
        producer. Only the role-selected private stage is archived, so object
        and dependency residue cannot leak into downstream fan-out.
    """
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
    role = _identifier(arguments.role, "targeted artifact role")
    if role not in TARGETED_ARTIFACT_ROLES:
        raise ReusableBuildError(f"unsupported targeted artifact role: {role}")
    if _git_head(root) != candidate:
        raise ReusableBuildError("candidate commit differs from checked-out Git HEAD")
    try:
        inventory_relative = inventory.resolve().relative_to(arguments.source.resolve())
    except ValueError as error:
        raise ReusableBuildError("generated inventory is outside the producer build") from error
    if inventory_relative != Path("generated/ci_inventory"):
        raise ReusableBuildError(
            "generated inventory is not at generated/ci_inventory in the build"
        )
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
    with tempfile.TemporaryDirectory(prefix="photospider-targeted-producer-") as stage_text:
        stage = Path(stage_text)
        _copy_targeted_payload(
            root, arguments.source, role, arguments.payload_source, stage
        )
        content = _targeted_content(stage)
        _validate_targeted_role_paths(
            role, [member["path"] for member in content["members"]]
        )
        if role in {"ctest-control", "ctest-runtime"}:
            closure = _load_ctest_closure_module(root)
            try:
                closure.validate_staged_role(
                    root,
                    stage,
                    role,
                    [member["path"] for member in content["members"]],
                )
            except Exception as error:
                raise ReusableBuildError(
                    f"targeted CTest producer closure is invalid: {error}"
                ) from error
        _write_archive(stage, arguments.archive)
    manifest = {
        "artifact": {
            "name": arguments.archive.name,
            "sha256": _sha256_file(arguments.archive),
            "size": arguments.archive.stat().st_size,
        },
        "artifact_role": role,
        "candidate_commit": candidate,
        "ci_image_digest": image,
        "content": content,
        "identity_mode": (
            "current-main-fallback" if resolved["fallback"] else "versioned"
        ),
        "internal_identity": internal,
        "matrix_sha256": resolved["matrix_sha256"],
        "producer_workflow_commit": workflow,
        "profile": profile,
        "schema": "photospider-targeted-ci-artifact-manifest-v1",
    }
    _write_json(arguments.manifest, manifest)
    print(_sha256_file(arguments.manifest))


def _validate_targeted_content_value(value: Any, role: str) -> dict[str, Any]:
    """Validate one canonical targeted member/size identity.

    Both member and aggregate sizes require non-boolean, nonnegative integers;
    Python's ``bool`` subclass relationship must never let JSON ``true`` or
    ``false`` become an accepted byte count.
    """
    if not isinstance(value, dict) or set(value) != {"members", "uncompressed_size"}:
        raise ReusableBuildError("targeted content identity fields differ")
    members = value["members"]
    if not isinstance(members, list) or not members:
        raise ReusableBuildError("targeted content member inventory is empty")
    paths: list[str] = []
    measured_size = 0
    for member in members:
        if not isinstance(member, dict) or set(member) != {
            "executable", "path", "sha256", "size"
        }:
            raise ReusableBuildError("targeted content member fields differ")
        path = member["path"]
        pure = PurePosixPath(path) if isinstance(path, str) else PurePosixPath()
        if (
            not isinstance(path, str)
            or not path
            or path.startswith("/")
            or "\\" in path
            or any(part in ("", ".", "..") for part in pure.parts)
            or pure.as_posix() != path
        ):
            raise ReusableBuildError("targeted content member path is unsafe")
        if not isinstance(member["executable"], bool):
            raise ReusableBuildError("targeted content executable flag is not boolean")
        if (
            not isinstance(member["size"], int)
            or isinstance(member["size"], bool)
            or member["size"] < 0
        ):
            raise ReusableBuildError("targeted content member size is invalid")
        if not isinstance(member["sha256"], str) or not re.fullmatch(
            r"[0-9a-f]{64}", member["sha256"]
        ):
            raise ReusableBuildError("targeted content member digest is invalid")
        paths.append(path)
        measured_size += member["size"]
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise ReusableBuildError("targeted content members are not sorted and unique")
    if (
        not isinstance(value["uncompressed_size"], int)
        or isinstance(value["uncompressed_size"], bool)
        or value["uncompressed_size"] < 0
    ):
        raise ReusableBuildError(
            "targeted content uncompressed size is invalid"
        )
    if value["uncompressed_size"] != measured_size:
        raise ReusableBuildError("targeted content uncompressed size mismatch")
    _validate_targeted_role_paths(role, paths)
    return value


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


def _load_targeted_manifest(snapshot: _ManifestSnapshot) -> dict[str, Any]:
    """Load one canonical targeted manifest from retained descriptor bytes.

    Args:
        snapshot: Once-opened regular manifest and its exact retained bytes.

    Returns:
        Canonical decoded manifest. Identity and content values are validated
        by the consuming operation against its explicit expectations.

    Raises:
        ReusableBuildError: The JSON, outer fields, schema, or canonical bytes
            differ from the targeted artifact contract.
    """
    value = snapshot.json_value()
    expected_fields = {
        "schema",
        "artifact",
        "artifact_role",
        "candidate_commit",
        "profile",
        "matrix_sha256",
        "ci_image_digest",
        "producer_workflow_commit",
        "identity_mode",
        "internal_identity",
        "content",
    }
    if not isinstance(value, dict) or set(value) != expected_fields:
        raise ReusableBuildError("targeted artifact manifest fields differ")
    if value["schema"] != "photospider-targeted-ci-artifact-manifest-v1":
        raise ReusableBuildError("unknown targeted artifact manifest schema")
    if snapshot.bytes != _canonical_bytes(value):
        raise ReusableBuildError("targeted artifact manifest is not canonical JSON")
    return value


def _verify_targeted_manifest(
    arguments: argparse.Namespace,
    archive_snapshot: _ArchiveSnapshot,
    manifest_snapshot: _ManifestSnapshot,
    staged_verifier: Callable[[Path, dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """Bind one targeted manifest to expected identity and retained bytes.

    Args:
        arguments: Exact consumer identity, role, archive, and manifest paths.
        archive_snapshot: Once-opened regular archive retained through content
            remeasurement and extraction.
        manifest_snapshot: Once-opened regular manifest whose retained bytes
            own hashing, JSON decoding, validation, and path identity.
        staged_verifier: Optional protected callback invoked against the exact
            already-validated staged ``ci`` tree while both snapshots remain
            retained.

    Returns:
        The validated canonical targeted manifest.

    Raises:
        ReusableBuildError: Any checkout, identity, role, member, size, digest,
            callback, or forbidden-content boundary differs.
        OSError: Safe staging or file measurement fails.
        tarfile.TarError: The retained archive is malformed.
    """
    candidate_root, checkout_snapshots = _targeted_checkout_snapshots(arguments)
    manifest_sha256 = manifest_snapshot.sha256()
    expected_manifest_sha256 = getattr(
        arguments, "expected_manifest_sha256", None
    )
    if expected_manifest_sha256 is not None:
        if not re.fullmatch(r"[0-9a-f]{64}", expected_manifest_sha256):
            raise ReusableBuildError(
                "expected targeted manifest SHA-256 is malformed"
            )
        if manifest_sha256 != expected_manifest_sha256:
            raise ReusableBuildError(
                "targeted manifest differs from the attested snapshot digest"
            )
    value = _load_targeted_manifest(manifest_snapshot)
    expected_values = {
        "artifact_role": _identifier(arguments.role, "targeted artifact role"),
        "candidate_commit": _full_sha(arguments.candidate_commit, "candidate commit"),
        "profile": _identifier(arguments.profile, "profile"),
        "matrix_sha256": arguments.matrix_sha256,
        "ci_image_digest": _digest(arguments.image_digest, "CI image digest"),
        "producer_workflow_commit": _full_sha(
            arguments.workflow_commit, "workflow commit"
        ),
    }
    if expected_values["artifact_role"] not in TARGETED_ARTIFACT_ROLES:
        raise ReusableBuildError("unsupported expected targeted artifact role")
    if not re.fullmatch(r"[0-9a-f]{64}", arguments.matrix_sha256):
        raise ReusableBuildError("matrix SHA-256 is malformed")
    for field, expected in expected_values.items():
        if value[field] != expected:
            raise ReusableBuildError(f"targeted manifest {field} mismatch")
    _validate_internal_identity(
        value["internal_identity"],
        value["identity_mode"],
        value["candidate_commit"],
        value["profile"],
        value["matrix_sha256"],
    )
    content = _validate_targeted_content_value(
        value["content"], value["artifact_role"]
    )
    artifact = value["artifact"]
    if not isinstance(artifact, dict) or set(artifact) != {"name", "sha256", "size"}:
        raise ReusableBuildError("targeted manifest artifact identity is malformed")
    if artifact["name"] != arguments.archive.name:
        raise ReusableBuildError("targeted manifest artifact name mismatch")
    if artifact["sha256"] != archive_snapshot.sha256():
        raise ReusableBuildError("targeted manifest artifact digest mismatch")
    if artifact["size"] != archive_snapshot.size:
        raise ReusableBuildError("targeted manifest artifact size mismatch")
    with tempfile.TemporaryDirectory(prefix="photospider-targeted-verify-") as stage_text:
        stage = Path(stage_text)
        with archive_snapshot.open_tar() as archive:
            members = _validated_members(archive)
            _extract_members(archive, members, stage)
        measured = _targeted_content(stage / "ci")
        if measured != content:
            raise ReusableBuildError("targeted archived content differs from manifest")
        if value["artifact_role"] in {"ctest-control", "ctest-runtime"}:
            closure = _load_ctest_closure_module(arguments.repo_root)
            try:
                closure.validate_staged_role(
                    candidate_root,
                    stage / "ci",
                    value["artifact_role"],
                    [member["path"] for member in content["members"]],
                )
            except Exception as error:
                raise ReusableBuildError(
                    f"targeted archived CTest closure is invalid: {error}"
                ) from error
        if staged_verifier is not None:
            try:
                staged_verifier(stage / "ci", value)
            except ReusableBuildError:
                raise
            except Exception as error:
                raise ReusableBuildError(
                    f"targeted staged coverage verification failed: {error}"
                ) from error
    manifest_snapshot.require_path_unchanged()
    for checkout_snapshot in checkout_snapshots:
        checkout_snapshot.require_unchanged()
    return value


def _snapshot_targeted_manifest(arguments: argparse.Namespace) -> None:
    """Copy one validated manifest snapshot into an exclusive private file.

    Args:
        arguments: Source manifest path and absent destination snapshot path.

    Raises:
        ReusableBuildError: Source validation, destination ownership, copied
            bytes, digest, or pathname identity differs.
        OSError: Secure open, write, sync, mode, or cleanup fails.

    Note:
        The printed SHA-256 is the exact subject passed from attestation to the
        later retained-snapshot Python consumer. The destination is created
        with ``O_EXCL``/``O_NOFOLLOW`` and never overwrites caller state.
    """
    destination = arguments.snapshot
    parent = destination.parent
    if not parent.is_dir() or parent.is_symlink():
        raise ReusableBuildError(
            "targeted manifest snapshot parent is missing or linked"
        )
    created = False
    try:
        with _ManifestSnapshot(arguments.manifest) as source_snapshot:
            _load_targeted_manifest(source_snapshot)
            digest = source_snapshot.sha256()
            retained_bytes = source_snapshot.bytes
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
            flags |= getattr(os, "O_CLOEXEC", 0)
            descriptor = os.open(destination, flags, 0o400)
            created = True
            with os.fdopen(descriptor, "wb", closefd=True) as output:
                output.write(retained_bytes)
                output.flush()
                os.fsync(output.fileno())
            destination.chmod(0o400)
            source_snapshot.require_path_unchanged()
        with _ManifestSnapshot(destination) as copied_snapshot:
            if copied_snapshot.bytes != retained_bytes:
                raise ReusableBuildError(
                    "targeted manifest snapshot copy differs from retained bytes"
                )
            if copied_snapshot.sha256() != digest:
                raise ReusableBuildError(
                    "targeted manifest snapshot copy digest differs"
                )
            copied_snapshot.require_path_unchanged()
    except Exception:
        if created and (destination.exists() or destination.is_symlink()):
            destination.unlink()
        raise
    print(digest)


def _verify_targeted_tree(arguments: argparse.Namespace) -> None:
    """Remeasure one restored role tree against its already verified manifest.

    Args:
        arguments: Exact targeted role, manifest, content root, and optional
            evidence output.

    Raises:
        ReusableBuildError: The manifest role/content contract or any current
            member path, size, digest, or executable attribute differs.
        OSError: The tree or evidence cannot be inspected or written safely.

    Note:
        The archive/attestation consumer must run first. This operation is an
        independent before/after immutability check for a restored tree; it
        neither repairs inputs nor trusts a sentinel or broad directory path.
    """
    role = _identifier(arguments.role, "targeted artifact role")
    if role not in TARGETED_ARTIFACT_ROLES:
        raise ReusableBuildError(f"unsupported targeted artifact role: {role}")
    for expected_name, expected_value in (
        ("content", arguments.expected_content_sha256),
        ("manifest", arguments.expected_manifest_sha256),
    ):
        if expected_value is not None and not re.fullmatch(
            r"[0-9a-f]{64}", expected_value
        ):
            raise ReusableBuildError(
                f"expected targeted {expected_name} SHA-256 is malformed"
            )
    with _ManifestSnapshot(arguments.manifest) as manifest_snapshot:
        manifest_sha256 = manifest_snapshot.sha256()
        if (
            arguments.expected_manifest_sha256 is not None
            and manifest_sha256 != arguments.expected_manifest_sha256
        ):
            raise ReusableBuildError(
                "targeted tree manifest differs from the retained manifest identity"
            )
        value = _load_targeted_manifest(manifest_snapshot)
        if value["artifact_role"] != role:
            raise ReusableBuildError("targeted tree manifest role mismatch")
        expected = _validate_targeted_content_value(value["content"], role)
        if not arguments.content_root.is_dir() or arguments.content_root.is_symlink():
            raise ReusableBuildError("targeted content root is missing or aliased")
        measured = _targeted_content(arguments.content_root)
        if measured != expected:
            raise ReusableBuildError(
                "targeted content tree differs from its verified manifest"
            )
        content_sha256 = hashlib.sha256(_canonical_bytes(measured)).hexdigest()
        if (
            arguments.expected_content_sha256 is not None
            and content_sha256 != arguments.expected_content_sha256
        ):
            raise ReusableBuildError(
                "targeted tree content differs from the retained content identity"
            )
        manifest_snapshot.require_path_unchanged()
        if arguments.evidence_output is not None:
            _write_json(
                arguments.evidence_output,
                {
                    "content_sha256": content_sha256,
                    "member_count": len(measured["members"]),
                    "manifest_sha256": manifest_sha256,
                    "role": role,
                    "schema": "photospider-targeted-tree-verification-v1",
                    "uncompressed_size": measured["uncompressed_size"],
                },
            )
    print(
        "targeted content tree verification passed: "
        f"role={role} content_sha256={content_sha256}"
    )


def _completion_stamp_records(build_root: Path) -> dict[str, str]:
    """Read the strict producer completion stamp from a verified role tree.

    Args:
        build_root: Already verified staged ``ci`` role root.

    Returns:
        Unique nonempty completion fields.

    Raises:
        ReusableBuildError: The stamp is missing, linked, malformed, duplicated,
            or has a field set different from the producer contract.
    """
    path = build_root / ".photospider-ci-build-complete"
    if not path.is_file() or path.is_symlink():
        raise ReusableBuildError("targeted CTest role lacks a safe completion stamp")
    records: dict[str, str] = {}
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if "=" not in line:
            raise ReusableBuildError(
                f"{path}:{line_number}: malformed completion record"
            )
        key, value = line.split("=", 1)
        if not key or not value or key in records:
            raise ReusableBuildError(
                f"{path}:{line_number}: empty or duplicate completion record"
            )
        records[key] = value
    expected = {
        "build_dir",
        "source_dir",
        "profile",
        "build_testing",
        "photospider_build_ipc",
        "candidate_commit",
        "created_at",
    }
    if set(records) != expected:
        raise ReusableBuildError("targeted completion stamp fields differ")
    return records


def _restore_runtime_for_inventory(
    staged_ci: Path,
    restore_root: Path,
    closure: Any,
    ctest_executable: str,
) -> list[dict[str, Any]]:
    """Restore one verified runtime role and rediscover ordinary CTest records.

    Args:
        staged_ci: Exact already-validated runtime role extracted from the
            retained archive descriptor.
        restore_root: Absent job-owned verifier build path.
        closure: Protected CTest closure module loaded from the control root.
        ctest_executable: Trusted host CTest program name or path.

    Returns:
        Sorted exact normalized ordinary-test records from restored JSON-v1
        discovery.

    Raises:
        ReusableBuildError: The recorded producer path is unsafe, residual/link
            state exists, the parent is unsafe/nonempty, protected relocation
            or discovery fails, or restored inventory is malformed.

    Note:
        Container and host jobs may expose different absolute workspace roots.
        After exact archive/closure verification, only the copied CTest control
        files and copied CMake cache have the exact recorded producer build-root
        token replaced with ``restore_root``. No test command executes. The
        derived tree is job-owned and removed in all cases.
    """
    restore = restore_root.absolute().resolve(strict=False)
    records = _completion_stamp_records(staged_ci)
    producer_build_root = records["build_dir"]
    producer_source_root = records["source_dir"]
    if any(
        not value.startswith("/") or "\0" in value
        for value in (producer_build_root, producer_source_root)
    ):
        raise ReusableBuildError("producer CTest root identity is unsafe")
    if restore.exists() or restore.is_symlink():
        raise ReusableBuildError("restored CTest root contains residual state")
    parent = restore.parent
    created_parent = False
    if parent.exists() or parent.is_symlink():
        if not parent.is_dir() or parent.is_symlink():
            raise ReusableBuildError("restored CTest parent is unsafe")
        try:
            with os.scandir(parent) as entries:
                if next(entries, None) is not None:
                    raise ReusableBuildError(
                        "restored CTest parent contains residual state"
                    )
        except OSError as error:
            raise ReusableBuildError(
                f"cannot enumerate restored CTest parent: {error}"
            ) from error
    else:
        grandparent = parent.parent
        if not grandparent.is_dir() or grandparent.is_symlink():
            raise ReusableBuildError("restored CTest grandparent is unsafe")
        parent.mkdir(mode=0o700)
        created_parent = True
    try:
        shutil.copytree(staged_ci, restore, copy_function=shutil.copy2)
        closure_value = closure.load_closure(
            restore.joinpath(*closure.CLOSURE_RELATIVE_PATH.parts)
        )
        old_root = producer_build_root.encode("utf-8")
        new_root = str(restore).encode("utf-8")
        relocation_paths = [
            "CMakeCache.txt",
            *closure_value["control_paths"],
        ]
        for relative in relocation_paths:
            path = restore / relative
            if not path.is_file() or path.is_symlink():
                raise ReusableBuildError(
                    f"restored CTest relocation input is unsafe: {relative}"
                )
            content = path.read_bytes()
            relocated = content.replace(old_root, new_root)
            path.write_bytes(relocated)
            if path.read_bytes() != relocated:
                raise ReusableBuildError(
                    f"restored CTest relocation write differs: {relative}"
                )
        inventory = closure.query_inventory(
            restore, ctest_executable, closure_value["config"]
        )
        return closure.ordinary_test_records(
            inventory,
            "restored targeted CTest runtime inventory",
            Path(producer_source_root),
            restore,
        )
    except ReusableBuildError:
        raise
    except Exception as error:
        raise ReusableBuildError(
            f"restored targeted CTest inventory failed: {error}"
        ) from error
    finally:
        if restore.exists() or restore.is_symlink():
            if restore.is_dir() and not restore.is_symlink():
                shutil.rmtree(restore)
            else:
                restore.unlink()
        if created_parent and parent.exists():
            parent.rmdir()


def _verify_ordinary_coverage(arguments: argparse.Namespace) -> None:
    """Cross-bind raw, control/runtime closure, and restored CTest inventories.

    Args:
        arguments: Explicit protected/candidate roots, raw/control artifacts,
            targeted artifact root, canonical restored build root, and exact
            candidate/workflow/profile/matrix/image/route identities.

    Raises:
        ReusableBuildError: Directory boundaries overlap, protected raw/control
            validation fails, either targeted role differs, or raw, archived,
            and restored ordinary-test sets are not exactly equal.

    Note:
        Routing validation is imported only from the protected control root.
        The candidate checkout supplies data/source bytes but no executable
        helper. Both targeted archives remain descriptor-retained while their
        staged closure is inspected.
    """
    candidate_root, operation_snapshots = _targeted_checkout_snapshots(arguments)
    control_root = Path(arguments.repo_root).absolute().resolve(strict=True)
    boundaries = _require_disjoint_existing_directories(
        {
            "protected control checkout": control_root,
            "candidate data checkout": candidate_root,
            "downloaded raw inventory": arguments.raw_dir,
            "downloaded routing control": arguments.control_manifest.parent,
            "downloaded targeted artifacts": arguments.artifact_root,
        }
    )
    restore_root = arguments.restored_build_root.absolute().resolve(strict=False)
    for name, boundary in boundaries.items():
        if (
            restore_root == boundary
            or restore_root in boundary.parents
            or boundary in restore_root.parents
        ):
            raise ReusableBuildError(
                f"restored CTest root overlaps {name}: {restore_root}"
            )
    route = _load_build_smoke_route_module(control_root)
    try:
        raw_ctest, _ = route.validate_control_bundle(
            raw_dir=boundaries["downloaded raw inventory"],
            manifest_path=arguments.control_manifest,
            route_sha256=arguments.route_sha256,
            candidate_commit=arguments.candidate_commit,
            workflow_commit=arguments.workflow_commit,
            image_digest=arguments.image_digest,
            profile=arguments.profile,
            matrix_sha256=arguments.matrix_sha256,
        )
    except Exception as error:
        raise ReusableBuildError(
            f"protected raw/control identity is invalid: {error}"
        ) from error
    closure = _load_ctest_closure_module(control_root)

    closure_values: dict[str, dict[str, Any]] = {}
    completion_values: dict[str, dict[str, str]] = {}
    restored_records: list[dict[str, Any]] | None = None
    artifact_root = boundaries["downloaded targeted artifacts"]
    for role in ("ctest-control", "ctest-runtime"):
        role_arguments = argparse.Namespace(
            archive=artifact_root / role / f"{role}.tar.gz",
            candidate_commit=arguments.candidate_commit,
            candidate_root=candidate_root,
            expected_manifest_sha256=None,
            image_digest=arguments.image_digest,
            manifest=artifact_root / role / f"{role}.manifest.json",
            matrix_sha256=arguments.matrix_sha256,
            profile=arguments.profile,
            repo_root=control_root,
            role=role,
            workflow_commit=arguments.workflow_commit,
        )

        def inspect_staged(
            staged_ci: Path,
            manifest: dict[str, Any],
            *,
            expected_role: str = role,
        ) -> None:
            """Retain one staged closure and rediscover runtime inventory."""
            nonlocal restored_records
            if manifest["artifact_role"] != expected_role:
                raise ReusableBuildError("targeted coverage role drifted")
            try:
                value = closure.load_closure(
                    staged_ci.joinpath(*closure.CLOSURE_RELATIVE_PATH.parts)
                )
            except Exception as error:
                raise ReusableBuildError(
                    f"{expected_role} ordinary CTest closure is invalid: {error}"
                ) from error
            closure_values[expected_role] = value
            completion_values[expected_role] = _completion_stamp_records(staged_ci)
            if expected_role == "ctest-runtime":
                restored_records = _restore_runtime_for_inventory(
                    staged_ci,
                    restore_root,
                    closure,
                    arguments.ctest_executable,
                )

        with _ArchiveSnapshot(role_arguments.archive) as archive_snapshot:
            with _ManifestSnapshot(role_arguments.manifest) as manifest_snapshot:
                _verify_targeted_manifest(
                    role_arguments,
                    archive_snapshot,
                    manifest_snapshot,
                    inspect_staged,
                )

    if set(closure_values) != {"ctest-control", "ctest-runtime"}:
        raise ReusableBuildError("targeted CTest closure roles are incomplete")
    if closure_values["ctest-control"] != closure_values["ctest-runtime"]:
        raise ReusableBuildError("control/runtime ordinary CTest closures differ")
    if completion_values.get("ctest-control") != completion_values.get(
        "ctest-runtime"
    ):
        raise ReusableBuildError("control/runtime completion identities differ")
    completion = completion_values["ctest-runtime"]
    try:
        raw_records = closure.ordinary_test_records(
            raw_ctest,
            "protected raw producer CTest inventory",
            Path(completion["source_dir"]),
            Path(completion["build_dir"]),
        )
    except Exception as error:
        raise ReusableBuildError(
            f"raw ordinary CTest inventory is invalid: {error}"
        ) from error
    closure_records = closure_values["ctest-runtime"]["ordinary_tests"]
    if restored_records is None:
        raise ReusableBuildError("restored ordinary CTest inventory is absent")
    if raw_records != closure_records or raw_records != restored_records:
        raise ReusableBuildError(
            "raw, targeted closure, and restored ordinary CTest records differ"
        )
    for snapshot in operation_snapshots:
        snapshot.require_unchanged()
    print(
        "ordinary CTest coverage cross-binding passed: "
        f"tests={len(raw_records)} route_sha256={arguments.route_sha256}"
    )


def _verify_targeted_only(arguments: argparse.Namespace) -> None:
    """Verify one targeted archive and manifest without installing it."""
    with _ArchiveSnapshot(arguments.archive) as archive_snapshot:
        with _ManifestSnapshot(arguments.manifest) as manifest_snapshot:
            _verify_targeted_manifest(
                arguments, archive_snapshot, manifest_snapshot
            )
    print("targeted reusable artifact verification passed")


def _verify_targeted_extract(arguments: argparse.Namespace) -> None:
    """Verify and atomically install one retained targeted ``ci`` snapshot.

    The runtime role is re-discovered at its final canonical build path before
    the previous tree is discarded. Any ``NOT_BUILT`` placeholder or closure
    drift removes the rejected tree and restores the previous snapshot.
    """
    with _ArchiveSnapshot(arguments.archive) as archive_snapshot:
        with _ManifestSnapshot(arguments.manifest) as manifest_snapshot:
            manifest = _verify_targeted_manifest(
                arguments, archive_snapshot, manifest_snapshot
            )
        arguments.destination.mkdir(parents=True, exist_ok=True)
        if arguments.destination.is_symlink():
            raise ReusableBuildError("targeted extraction destination is a symlink")
        with archive_snapshot.open_tar() as archive:
            members = _validated_members(archive)
            with tempfile.TemporaryDirectory(
                prefix=".ci-targeted-", dir=arguments.destination
            ) as stage_text:
                stage = Path(stage_text)
                _extract_members(archive, members, stage)
                staged_ci = stage / "ci"
                target_ci = arguments.destination / "ci"
                backup = arguments.destination / f".ci-targeted-backup-{os.getpid()}"
                if backup.exists() or backup.is_symlink():
                    raise ReusableBuildError(
                        f"unexpected targeted extraction backup exists: {backup}"
                    )
                if target_ci.exists() or target_ci.is_symlink():
                    target_ci.replace(backup)
                try:
                    staged_ci.replace(target_ci)
                    if manifest["artifact_role"] == "ctest-runtime":
                        closure = _load_ctest_closure_module(arguments.repo_root)
                        closure.verify_restored_runtime(
                            arguments.repo_root,
                            target_ci,
                            arguments.ctest_executable,
                        )
                except Exception as error:
                    if target_ci.exists() or target_ci.is_symlink():
                        if target_ci.is_dir() and not target_ci.is_symlink():
                            shutil.rmtree(target_ci)
                        else:
                            target_ci.unlink()
                    if backup.exists():
                        backup.replace(target_ci)
                    if isinstance(error, ReusableBuildError):
                        raise
                    raise ReusableBuildError(
                        f"restored targeted role verification failed: {error}"
                    ) from error
                else:
                    if backup.exists():
                        if backup.is_dir() and not backup.is_symlink():
                            shutil.rmtree(backup)
                        else:
                            backup.unlink()
                if backup.exists():
                    raise ReusableBuildError(
                        "targeted extraction left an unexpected backup"
                    )
    print("targeted reusable artifact verification and extraction passed")


def _verify_archived_measurement(
    arguments: argparse.Namespace,
    manifest: dict[str, Any],
    snapshot: _ArchiveSnapshot,
) -> None:
    """Remeasure the retained archive snapshot and match signed identity.

    Args:
        arguments: Exact expected consumer identities and repository root.
        manifest: Canonical external reusable-build manifest.
        snapshot: Once-opened archive object already bound to the manifest
            digest and size.

    Raises:
        ReusableBuildError: Members, profile identity, or measured build state
            differs from the signed manifest.
        OSError: Safe staging or measurement cannot complete.
        tarfile.TarError: The retained archive bytes are malformed.
    """
    with tempfile.TemporaryDirectory(prefix="photospider-reusable-verify-") as stage_text:
        stage = Path(stage_text)
        with snapshot.open_tar() as archive:
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


def _verify_manifest(
    arguments: argparse.Namespace, snapshot: _ArchiveSnapshot
) -> dict[str, Any]:
    """Validate the manifest and bind it to one retained archive object.

    Args:
        arguments: Exact expected consumer identities and manifest path.
        snapshot: Once-opened archive whose digest, size, members, and measured
            build identity must all agree.

    Returns:
        The validated canonical manifest object.

    Raises:
        ReusableBuildError: Any manifest, expected identity, or archive object
            field differs.
    """
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
    if artifact["sha256"] != snapshot.sha256():
        raise ReusableBuildError("manifest artifact digest mismatch")
    if artifact["size"] != snapshot.size:
        raise ReusableBuildError("manifest artifact size mismatch")
    _verify_archived_measurement(arguments, value, snapshot)
    return value


def _verify_extract(arguments: argparse.Namespace) -> None:
    """Verify one retained archive object, then atomically install ``ci``.

    Args:
        arguments: Exact consumer identities, archive path, and destination.

    Raises:
        ReusableBuildError: Identity, archive, member, or destination safety
            checks fail.
        OSError: Snapshot, staging, or atomic installation cannot complete.
        tarfile.TarError: The retained archive bytes are malformed.

    Note:
        The archive pathname is opened once; atomic replacement after digesting
        cannot alter archived measurement or the final extracted bytes.
    """
    with _ArchiveSnapshot(arguments.archive) as snapshot:
        _verify_manifest(arguments, snapshot)
        arguments.destination.mkdir(parents=True, exist_ok=True)
        if arguments.destination.is_symlink():
            raise ReusableBuildError("extraction destination must not be a symlink")
        with snapshot.open_tar() as archive:
            members = _validated_members(archive)
            with tempfile.TemporaryDirectory(
                prefix=".ci-reusable-", dir=arguments.destination
            ) as stage_text:
                stage = Path(stage_text)
                _extract_members(archive, members, stage)
                staged_ci = stage / "ci"
                target_ci = arguments.destination / "ci"
                backup = arguments.destination / f".ci-reusable-backup-{os.getpid()}"
                if backup.exists():
                    raise ReusableBuildError(
                        f"unexpected extraction backup exists: {backup}"
                    )
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
    """Verify identities and every member of one retained archive object."""
    with _ArchiveSnapshot(arguments.archive) as snapshot:
        _verify_manifest(arguments, snapshot)
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


def _add_targeted_expected(parser: argparse.ArgumentParser) -> None:
    """Add targeted role plus the common exact consumer identities."""
    _add_expected(parser)
    parser.add_argument(
        "--candidate-root",
        type=Path,
        help=(
            "separate nonexecuted candidate-data checkout for protected "
            "pre-attestation verification"
        ),
    )
    parser.add_argument("--role", choices=sorted(TARGETED_ARTIFACT_ROLES), required=True)
    parser.add_argument("--expected-manifest-sha256")


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
    create_targeted = subparsers.add_parser(
        "create-targeted", help="create one role-minimal artifact and manifest"
    )
    create_targeted.add_argument("--source", type=Path, required=True)
    create_targeted.add_argument("--payload-source", type=Path)
    create_targeted.add_argument("--archive", type=Path, required=True)
    create_targeted.add_argument("--manifest", type=Path, required=True)
    create_targeted.add_argument("--candidate-commit", required=True)
    create_targeted.add_argument("--profile", required=True)
    create_targeted.add_argument("--image-digest", required=True)
    create_targeted.add_argument("--workflow-commit", required=True)
    create_targeted.add_argument(
        "--role", choices=sorted(TARGETED_ARTIFACT_ROLES), required=True
    )
    create_targeted.set_defaults(handler=_create_targeted)
    verify = subparsers.add_parser("verify-extract", help="verify and safely extract archive")
    _add_expected(verify)
    verify.add_argument("--destination", type=Path, required=True)
    verify.set_defaults(handler=_verify_extract)
    verify_only = subparsers.add_parser("verify-only", help="verify without extracting archive")
    _add_expected(verify_only)
    verify_only.set_defaults(handler=_verify_only)
    verify_targeted = subparsers.add_parser(
        "verify-targeted-extract", help="verify and extract one targeted artifact"
    )
    _add_targeted_expected(verify_targeted)
    verify_targeted.add_argument("--destination", type=Path, required=True)
    verify_targeted.add_argument("--ctest-executable", default="ctest")
    verify_targeted.set_defaults(handler=_verify_targeted_extract)
    verify_targeted_only = subparsers.add_parser(
        "verify-targeted-only", help="verify one targeted artifact without extraction"
    )
    _add_targeted_expected(verify_targeted_only)
    verify_targeted_only.set_defaults(handler=_verify_targeted_only)
    verify_coverage = subparsers.add_parser(
        "verify-ordinary-coverage",
        help="cross-bind raw, archived, and restored ordinary CTest inventory",
    )
    verify_coverage.add_argument("--candidate-root", type=Path, required=True)
    verify_coverage.add_argument("--raw-dir", type=Path, required=True)
    verify_coverage.add_argument("--control-manifest", type=Path, required=True)
    verify_coverage.add_argument("--route-sha256", required=True)
    verify_coverage.add_argument("--artifact-root", type=Path, required=True)
    verify_coverage.add_argument("--restored-build-root", type=Path, required=True)
    verify_coverage.add_argument("--candidate-commit", required=True)
    verify_coverage.add_argument("--profile", required=True)
    verify_coverage.add_argument("--matrix-sha256", required=True)
    verify_coverage.add_argument("--image-digest", required=True)
    verify_coverage.add_argument("--workflow-commit", required=True)
    verify_coverage.add_argument("--ctest-executable", default="ctest")
    verify_coverage.set_defaults(handler=_verify_ordinary_coverage)
    snapshot_targeted_manifest = subparsers.add_parser(
        "snapshot-targeted-manifest",
        help="retain and copy one canonical targeted manifest",
    )
    snapshot_targeted_manifest.add_argument(
        "--manifest", type=Path, required=True
    )
    snapshot_targeted_manifest.add_argument(
        "--snapshot", type=Path, required=True
    )
    snapshot_targeted_manifest.set_defaults(handler=_snapshot_targeted_manifest)
    verify_targeted_tree = subparsers.add_parser(
        "verify-targeted-tree",
        help="remeasure one restored targeted tree against its manifest",
    )
    verify_targeted_tree.add_argument(
        "--manifest", type=Path, required=True
    )
    verify_targeted_tree.add_argument(
        "--role", choices=sorted(TARGETED_ARTIFACT_ROLES), required=True
    )
    verify_targeted_tree.add_argument(
        "--content-root", type=Path, required=True
    )
    verify_targeted_tree.add_argument("--evidence-output", type=Path)
    verify_targeted_tree.add_argument("--expected-content-sha256")
    verify_targeted_tree.add_argument("--expected-manifest-sha256")
    verify_targeted_tree.set_defaults(handler=_verify_targeted_tree)
    return parser


def main() -> int:
    """Run the requested reusable-build operation with stable diagnostics."""
    parser = build_parser()
    arguments = parser.parse_args()
    arguments.repo_root = arguments.repo_root.absolute()
    if not arguments.inventory_dir.is_absolute():
        arguments.inventory_dir = arguments.repo_root / arguments.inventory_dir
    arguments.inventory_dir = arguments.inventory_dir.resolve()
    if hasattr(arguments, "source"):
        arguments.source = arguments.source.resolve()
    if hasattr(arguments, "payload_source") and arguments.payload_source is not None:
        arguments.payload_source = arguments.payload_source.resolve()
    if hasattr(arguments, "content_root"):
        arguments.content_root = arguments.content_root.resolve()
    if hasattr(arguments, "candidate_root") and arguments.candidate_root is not None:
        arguments.candidate_root = arguments.candidate_root.absolute()
    if hasattr(arguments, "restored_build_root"):
        arguments.restored_build_root = arguments.restored_build_root.absolute()
    if (
        hasattr(arguments, "evidence_output")
        and arguments.evidence_output is not None
    ):
        arguments.evidence_output = arguments.evidence_output.resolve()
    try:
        arguments.handler(arguments)
    except (ReusableBuildError, OSError, tarfile.TarError) as error:
        print(f"reusable build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
