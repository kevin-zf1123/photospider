#!/usr/bin/env python3
"""Create and verify identity-bound reusable CI build archives.

The producer writes a deterministic tar archive and canonical external manifest.
The consumer must verify GitHub attestations before invoking ``verify-extract``;
this helper then rechecks all expected identities and the archive digest before
performing traversal-safe extraction. The candidate-owned extended identity is
mandatory once the versioned build-profile matrix exists. Current ``main`` may
use only the exact protected fallback recognized by ``ci_profile_manifest``.
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
    """Validate a bytewise-key-sorted string map."""
    if not isinstance(value, dict) or list(value) != sorted(value):
        raise ReusableBuildError(f"{context} must be a sorted object")
    if not all(isinstance(key, str) and key and isinstance(item, str) and item for key, item in value.items()):
        raise ReusableBuildError(f"{context} must contain nonempty string identities")
    return value


def _validate_string_array(value: Any, context: str) -> list[str]:
    """Validate a sorted unique nonempty-string array (which may itself be empty)."""
    if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
        raise ReusableBuildError(f"{context} must be a string array")
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
    for field in ("cmake", "compiler", "toolchain", "dependencies"):
        _validate_string_map(value[field], f"internal_identity:{field}")
    for field in ("package_components", "package_targets"):
        _validate_string_array(value[field], f"internal_identity:{field}")


def _candidate_identity(
    root: Path,
    inventory: Path,
    candidate_commit: str,
    profile: str,
    matrix_sha256: str,
    fallback: bool,
) -> dict[str, Any]:
    """Load a future candidate identity or construct the bounded current-main form."""
    path = inventory / "reusable_build_identity_v1.json"
    if not path.exists():
        if not fallback:
            raise ReusableBuildError(
                "versioned matrix exists but reusable_build_identity_v1.json is missing"
            )
        return {
            "candidate_commit": candidate_commit,
            "cmake": {},
            "compiler": {},
            "dependencies": {},
            "matrix_sha256": matrix_sha256,
            "package_components": [],
            "package_targets": [],
            "profile": profile,
            "schema": "photospider-current-main-reusable-identity-v1",
            "toolchain": {},
        }
    value = _load_json(path)
    expected = {
        "schema", "candidate_commit", "profile", "matrix_sha256", "cmake", "compiler",
        "toolchain", "dependencies", "package_components", "package_targets",
    }
    if not isinstance(value, dict) or set(value) != expected:
        raise ReusableBuildError(f"{path}: missing or unknown identity fields")
    if value["schema"] != "photospider-reusable-build-identity-v1":
        raise ReusableBuildError(f"{path}: unknown reusable identity schema")
    if value["candidate_commit"] != candidate_commit:
        raise ReusableBuildError(f"{path}: candidate commit mismatch")
    if value["profile"] != profile:
        raise ReusableBuildError(f"{path}: profile mismatch")
    if value["matrix_sha256"] != matrix_sha256:
        raise ReusableBuildError(f"{path}: matrix digest mismatch")
    for field in ("cmake", "compiler", "toolchain", "dependencies"):
        _validate_string_map(value[field], f"{path}:{field}")
    for field in ("package_components", "package_targets"):
        _validate_string_array(value[field], f"{path}:{field}")
    return value


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
    """Create a deterministic archive and its complete external identity manifest."""
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
    internal = _candidate_identity(
        root, inventory, candidate, profile, resolved["matrix_sha256"], resolved["fallback"]
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
    with tarfile.open(arguments.archive, mode="r:gz") as archive:
        _validated_members(archive)
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
    try:
        arguments.handler(arguments)
    except (ReusableBuildError, OSError, tarfile.TarError) as error:
        print(f"reusable build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
