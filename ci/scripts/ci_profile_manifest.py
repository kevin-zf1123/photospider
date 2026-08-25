#!/usr/bin/env python3
"""Resolve versioned CI security roles with one bounded mainline fallback.

Future candidates emit ``build_profile_matrix_v1.tsv``, its SHA-256 sidecar,
and ``ci_security_roles_v1.tsv`` under the configured CI inventory directory.
This protected reader accepts that complete set or, while current ``main`` does
not emit it, a protected fallback whose candidate-source hashes must all match.
Partial, stale, duplicate, unknown, or non-canonical inputs fail closed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Any


class ProfileError(ValueError):
    """Report one malformed or mismatched profile identity."""


MATRIX_SCHEMA = "photospider-build-profile-matrix-v1"
ROLES_SCHEMA = "photospider-ci-security-roles-v1"
FALLBACK_SCHEMA = "photospider-current-main-profiles-v1"


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting duplicate members."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProfileError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    """Read strict UTF-8 JSON with duplicate-key rejection."""
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError, ProfileError) as error:
        raise ProfileError(f"cannot read strict JSON {path}: {error}") from error


def _canonical_bytes(value: Any) -> bytes:
    """Return canonical compact JSON bytes with one final newline."""
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def _hash_file(path: Path) -> str:
    """Return the SHA-256 of one regular, non-symlink file."""
    if not path.is_file() or path.is_symlink():
        raise ProfileError(f"profile input is not a regular file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_canonical_tsv(path: Path) -> list[list[str]]:
    """Read canonical LF-delimited TSV without controls, blanks, or duplicates."""
    raw = path.read_bytes()
    if not raw.endswith(b"\n") or b"\r" in raw or b"\0" in raw:
        raise ProfileError(f"{path}: TSV must use canonical LF text with a final newline")
    try:
        lines = raw.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise ProfileError(f"{path}: TSV is not UTF-8: {error}") from error
    if not lines or any(not line for line in lines):
        raise ProfileError(f"{path}: TSV is empty or contains blank records")
    if len(set(lines)) != len(lines):
        raise ProfileError(f"{path}: duplicate TSV record")
    rows = [line.split("\t") for line in lines]
    for line_number, fields in enumerate(rows, 1):
        if any(not field or any(ord(character) < 0x20 for character in field) for field in fields):
            raise ProfileError(f"{path}:{line_number}: empty field or control character")
    return rows


def _string_array(text: str, context: str) -> list[str]:
    """Decode a canonical JSON string array with unique nonempty values."""
    try:
        value = json.loads(text, object_pairs_hook=_unique_object)
    except (json.JSONDecodeError, ProfileError) as error:
        raise ProfileError(f"{context}: invalid JSON array: {error}") from error
    if not isinstance(value, list) or not value or not all(isinstance(item, str) and item for item in value):
        raise ProfileError(f"{context}: expected a nonempty string array")
    if value != sorted(value) or len(value) != len(set(value)):
        raise ProfileError(f"{context}: array must be sorted and unique")
    if json.dumps(value, separators=(",", ":"), ensure_ascii=True) != text:
        raise ProfileError(f"{context}: array JSON is not canonical")
    return value


def _identifier(value: str, context: str) -> str:
    """Require one lowercase canonical CI identifier."""
    if not re.fullmatch(r"[a-z][a-z0-9-]*", value):
        raise ProfileError(f"{context}: invalid identifier {value!r}")
    return value


def _target_identifier(value: str, context: str) -> str:
    """Require one canonical lowercase CMake target identifier."""
    if not re.fullmatch(r"[a-z][a-z0-9_]*", value):
        raise ProfileError(f"{context}: invalid target identifier {value!r}")
    return value


def _positive_integer(value: str, context: str) -> int:
    """Parse one canonical strictly positive decimal integer."""
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise ProfileError(f"{context}: expected a positive canonical integer")
    return int(value)


def _load_versioned(inventory: Path) -> dict[str, Any]:
    """Load and cross-bind a complete versioned matrix/security-role set."""
    matrix_path = inventory / "build_profile_matrix_v1.tsv"
    digest_path = inventory / "build_profile_matrix_v1.tsv.sha256"
    roles_path = inventory / "ci_security_roles_v1.tsv"
    matrix_rows = _read_canonical_tsv(matrix_path)
    if matrix_rows[0] != ["schema", MATRIX_SCHEMA]:
        raise ProfileError(f"{matrix_path}: missing or unknown first schema record")
    matrix_digest = _hash_file(matrix_path)
    expected_sidecar = f"{matrix_digest}  build_profile_matrix_v1.tsv\n"
    if digest_path.read_text(encoding="utf-8") != expected_sidecar:
        raise ProfileError(f"{digest_path}: stale or non-canonical matrix digest")
    rows = _read_canonical_tsv(roles_path)
    if len(rows) < 3 or rows[0] != ["schema", ROLES_SCHEMA]:
        raise ProfileError(f"{roles_path}: missing or unknown schema")
    if rows[1] != ["matrix_sha256", matrix_digest]:
        raise ProfileError(f"{roles_path}: matrix digest does not match exact matrix bytes")
    if rows[2:] != sorted(rows[2:]):
        raise ProfileError(f"{roles_path}: role records are not bytewise sorted")

    sanitizers: list[dict[str, Any]] = []
    fuzz_profiles: dict[str, dict[str, Any]] = {}
    seen_profiles: set[str] = set()
    seen_fuzz_targets: set[tuple[str, str]] = set()
    for line_number, fields in enumerate(rows[2:], 3):
        context = f"{roles_path}:{line_number}"
        if fields[0] == "sanitizer":
            if len(fields) != 8:
                raise ProfileError(f"{context}: sanitizer record requires eight fields")
            (
                _, profile, platforms_json, cmake_json, dependencies_json,
                targets_json, labels_json, artifact_role,
            ) = fields
            profile = _identifier(profile, context)
            if profile in seen_profiles:
                raise ProfileError(f"{context}: duplicate security profile {profile}")
            seen_profiles.add(profile)
            platforms = _string_array(platforms_json, context)
            if not set(platforms).issubset({"Darwin", "Linux"}):
                raise ProfileError(f"{context}: unsupported sanitizer platform")
            cmake_args = _string_array(cmake_json, context)
            if not all(re.fullmatch(r"-D[A-Z0-9_]+=[A-Za-z0-9_.+-]+", item) for item in cmake_args):
                raise ProfileError(f"{context}: invalid CMake cache argument")
            sanitizers.append(
                {
                    "artifact_role": _identifier(artifact_role, context),
                    "cmake_args": cmake_args,
                    "ctest_labels": _string_array(labels_json, context),
                    "platforms": platforms,
                    "profile": profile,
                    "targets": _string_array(targets_json, context),
                    "vcpkg_dependencies": _string_array(dependencies_json, context),
                }
            )
        elif fields[0] == "fuzz":
            if len(fields) != 14:
                raise ProfileError(f"{context}: fuzz record requires fourteen fields")
            (
                _, profile, platforms_json, dependencies_json, target, seed_text, runs_text,
                timeout_text, job_timeout_text, max_len_text, production_limit_text, symbol,
                corpus_digest, artifact_role,
            ) = fields
            profile = _identifier(profile, context)
            target = _target_identifier(target, context)
            key = (profile, target)
            if key in seen_fuzz_targets:
                raise ProfileError(f"{context}: duplicate fuzz target {profile}/{target}")
            seen_fuzz_targets.add(key)
            platforms = _string_array(platforms_json, context)
            if not set(platforms).issubset({"Darwin", "Linux"}):
                raise ProfileError(f"{context}: unsupported fuzz platform")
            dependencies = _string_array(dependencies_json, context)
            seed = _positive_integer(seed_text, context)
            runs = _positive_integer(runs_text, context)
            timeout = _positive_integer(timeout_text, context)
            job_timeout = _positive_integer(job_timeout_text, context)
            max_len = _positive_integer(max_len_text, context)
            production_limit = _positive_integer(production_limit_text, context)
            if seed != 1 or runs != 1000:
                raise ProfileError(f"{context}: fuzz smoke requires seed 1 and exactly 1000 runs")
            if production_limit > 16 * 1024 * 1024 or max_len > production_limit:
                raise ProfileError(f"{context}: fuzz input bound exceeds its production limit")
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_:]*", symbol):
                raise ProfileError(f"{context}: invalid production-bound symbol")
            if corpus_digest != "none" and not re.fullmatch(r"sha256:[0-9a-f]{64}", corpus_digest):
                raise ProfileError(f"{context}: malformed corpus digest")
            profile_value = fuzz_profiles.setdefault(
                profile,
                {
                    "artifact_role": _identifier(artifact_role, context),
                    "corpus_sha256": None if corpus_digest == "none" else corpus_digest.removeprefix("sha256:"),
                    "job_timeout_minutes": job_timeout,
                    "platforms": platforms,
                    "profile": profile,
                    "runs": runs,
                    "seed": seed,
                    "targets": [],
                    "vcpkg_dependencies": dependencies,
                },
            )
            identity = (
                profile_value["platforms"], profile_value["seed"], profile_value["runs"],
                profile_value["job_timeout_minutes"], profile_value["corpus_sha256"],
                profile_value["artifact_role"],
                profile_value["vcpkg_dependencies"],
            )
            current = (
                platforms, seed, runs, job_timeout,
                None if corpus_digest == "none" else corpus_digest.removeprefix("sha256:"),
                artifact_role,
                dependencies,
            )
            if identity != current:
                raise ProfileError(f"{context}: inconsistent values within fuzz profile {profile}")
            profile_value["targets"].append(
                {
                    "max_len": max_len,
                    "name": target,
                    "production_limit": production_limit,
                    "production_limit_symbol": symbol,
                    "timeout_seconds": timeout,
                }
            )
        else:
            raise ProfileError(f"{context}: unknown security role record {fields[0]!r}")
    if not sanitizers or not fuzz_profiles:
        raise ProfileError(f"{roles_path}: sanitizer and fuzz roles must both be nonempty")
    for profile in fuzz_profiles.values():
        profile["targets"].sort(key=lambda item: item["name"])
    return {
        "fallback": False,
        "matrix_sha256": matrix_digest,
        "profiles": sorted(sanitizers + list(fuzz_profiles.values()), key=lambda item: item["profile"]),
        "schema": "photospider-resolved-ci-security-v1",
    }


def _load_fallback(root: Path) -> dict[str, Any]:
    """Load the protected current-main selection after exact source-hash checks."""
    path = root / "ci/locks/current-main-profiles-v1.json"
    value = _load_json(path)
    if not isinstance(value, dict) or set(value) != {"schema", "source_hashes", "sanitizers", "fuzz"}:
        raise ProfileError(f"{path}: malformed fallback root")
    if value["schema"] != FALLBACK_SCHEMA:
        raise ProfileError(f"{path}: unknown fallback schema")
    hashes = value["source_hashes"]
    if not isinstance(hashes, dict) or not hashes:
        raise ProfileError(f"{path}: fallback source hashes are empty")
    if list(hashes) != sorted(hashes):
        raise ProfileError(f"{path}: fallback source hashes are not sorted")
    for relative, expected in hashes.items():
        if not isinstance(relative, str) or not re.fullmatch(r"[0-9a-f]{64}", str(expected)):
            raise ProfileError(f"{path}: malformed fallback source hash")
        actual = _hash_file(root / relative)
        if actual != expected:
            raise ProfileError(
                f"temporary current-main fallback is stale for {relative}: {actual} != {expected}"
            )
    fallback_digest = _hash_file(path)
    sanitizers = value["sanitizers"]
    if not isinstance(sanitizers, list) or len(sanitizers) != 2:
        raise ProfileError(f"{path}: fallback must contain exact ASan and TSan profiles")
    for sanitizer in sanitizers:
        if not isinstance(sanitizer, dict) or set(sanitizer) != {
            "profile", "cmake_args", "runtime_contract", "invocations",
            "platforms", "vcpkg_dependencies",
        }:
            raise ProfileError(f"{path}: fallback sanitizer fields are malformed")
        _identifier(sanitizer["profile"], str(path))
        cmake_args = sanitizer["cmake_args"]
        if (
            not isinstance(cmake_args, list)
            or cmake_args != sorted(cmake_args)
            or not cmake_args
            or not all(
                isinstance(argument, str)
                and re.fullmatch(r"-D[A-Z0-9_]+=[A-Za-z0-9_.+-]+", argument)
                for argument in cmake_args
            )
        ):
            raise ProfileError(f"{path}: fallback sanitizer CMake arguments are malformed")
        if sanitizer["runtime_contract"] != "policy_execution":
            raise ProfileError(f"{path}: fallback runtime contract is unknown")
        if sanitizer["platforms"] != ["Darwin", "Linux"]:
            raise ProfileError(f"{path}: fallback sanitizer platforms are not Darwin/Linux")
        dependencies = sanitizer["vcpkg_dependencies"]
        if (
            not isinstance(dependencies, list)
            or dependencies != sorted(dependencies)
            or len(dependencies) != len(set(dependencies))
            or not dependencies
            or not all(isinstance(item, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", item) for item in dependencies)
        ):
            raise ProfileError(f"{path}: fallback sanitizer dependencies are malformed")
        invocations = sanitizer["invocations"]
        if not isinstance(invocations, list) or not invocations:
            raise ProfileError(f"{path}: fallback sanitizer invocations are empty")
        targets: list[str] = []
        for invocation in invocations:
            if not isinstance(invocation, dict) or set(invocation) != {
                "target", "filter", "trust_environment"
            }:
                raise ProfileError(f"{path}: fallback sanitizer invocation is malformed")
            targets.append(_target_identifier(invocation["target"], str(path)))
            if not isinstance(invocation["filter"], str) or any(
                character in invocation["filter"] for character in ("\t", "\r", "\n")
            ):
                raise ProfileError(f"{path}: fallback GoogleTest filter is malformed")
            if not isinstance(invocation["trust_environment"], bool):
                raise ProfileError(f"{path}: fallback trust flag is malformed")
        if targets != sorted(targets) or len(targets) != len(set(targets)):
            raise ProfileError(f"{path}: fallback sanitizer targets are not sorted and unique")

    fuzz = value["fuzz"]
    if not isinstance(fuzz, dict) or set(fuzz) != {
        "profile", "cmake_args", "seed", "runs", "job_timeout_minutes",
        "corpus_sha256", "targets", "platforms", "vcpkg_dependencies",
    }:
        raise ProfileError(f"{path}: fallback fuzz fields are malformed")
    if fuzz["profile"] != "fuzz-codecs" or fuzz["seed"] != 1 or fuzz["runs"] != 1000:
        raise ProfileError(f"{path}: fallback fuzz deterministic identity is malformed")
    if fuzz["platforms"] != ["Darwin", "Linux"]:
        raise ProfileError(f"{path}: fallback fuzz platforms are not Darwin/Linux")
    fuzz_dependencies = fuzz["vcpkg_dependencies"]
    if (
        not isinstance(fuzz_dependencies, list)
        or fuzz_dependencies != sorted(fuzz_dependencies)
        or len(fuzz_dependencies) != len(set(fuzz_dependencies))
        or not fuzz_dependencies
        or not all(
            isinstance(item, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", item)
            for item in fuzz_dependencies
        )
    ):
        raise ProfileError(f"{path}: fallback fuzz dependencies are malformed")
    if not isinstance(fuzz["job_timeout_minutes"], int) or fuzz["job_timeout_minutes"] <= 0:
        raise ProfileError(f"{path}: fallback fuzz job timeout is invalid")
    if fuzz["corpus_sha256"] is not None and not re.fullmatch(
        r"[0-9a-f]{64}", str(fuzz["corpus_sha256"])
    ):
        raise ProfileError(f"{path}: fallback fuzz corpus digest is malformed")
    fuzz_cmake = fuzz["cmake_args"]
    if (
        not isinstance(fuzz_cmake, list)
        or fuzz_cmake != sorted(fuzz_cmake)
        or not fuzz_cmake
        or not all(isinstance(argument, str) and argument.startswith("-D") for argument in fuzz_cmake)
    ):
        raise ProfileError(f"{path}: fallback fuzz CMake arguments are malformed")
    fuzz_targets = fuzz["targets"]
    if not isinstance(fuzz_targets, list) or not fuzz_targets:
        raise ProfileError(f"{path}: fallback fuzz targets are empty")
    target_names: list[str] = []
    for target in fuzz_targets:
        if not isinstance(target, dict) or set(target) != {
            "name", "max_len", "production_limit", "production_limit_symbol", "timeout_seconds"
        }:
            raise ProfileError(f"{path}: fallback fuzz target fields are malformed")
        target_names.append(_target_identifier(target["name"], str(path)))
        numeric = (target["max_len"], target["production_limit"], target["timeout_seconds"])
        if not all(isinstance(item, int) and not isinstance(item, bool) and item > 0 for item in numeric):
            raise ProfileError(f"{path}: fallback fuzz target bounds are invalid")
        if target["max_len"] > target["production_limit"] or target["production_limit"] > 16 * 1024 * 1024:
            raise ProfileError(f"{path}: fallback fuzz input exceeds its production bound")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_:]*", str(target["production_limit_symbol"])):
            raise ProfileError(f"{path}: fallback production-bound symbol is malformed")
    if target_names != sorted(target_names) or len(target_names) != len(set(target_names)):
        raise ProfileError(f"{path}: fallback fuzz targets are not sorted and unique")

    profiles = list(sanitizers) + [fuzz]
    identifiers = [profile["profile"] for profile in profiles]
    if identifiers != ["sanitizer-asan", "sanitizer-tsan", "fuzz-codecs"]:
        raise ProfileError(f"{path}: fallback profile identities are missing, reordered, or unknown")
    return {
        "fallback": True,
        "matrix_sha256": fallback_digest,
        "profiles": sorted(profiles, key=lambda item: item["profile"]),
        "schema": "photospider-resolved-ci-security-v1",
    }


def resolve(root: Path, inventory: Path) -> dict[str, Any]:
    """Resolve either the complete versioned candidate set or exact fallback."""
    paths = [
        inventory / "build_profile_matrix_v1.tsv",
        inventory / "build_profile_matrix_v1.tsv.sha256",
        inventory / "ci_security_roles_v1.tsv",
    ]
    present = [path.exists() for path in paths]
    if any(present) and not all(present):
        states = ", ".join(f"{path.name}={'present' if exists else 'missing'}" for path, exists in zip(paths, present))
        raise ProfileError(f"partial versioned profile identity: {states}")
    return _load_versioned(inventory) if all(present) else _load_fallback(root)


def _write_output(path: Path, value: bytes) -> None:
    """Atomically write a resolved manifest without following a symlink output."""
    if path.exists() and path.is_symlink():
        raise ProfileError(f"refusing symlink output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    temporary.write_bytes(value)
    temporary.replace(path)


def main() -> int:
    """Resolve the selected profile and emit canonical JSON for trusted runners."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--inventory-dir", type=Path, default=Path("build/generated/ci_inventory"))
    parser.add_argument("--profile", help="emit only this exact profile")
    parser.add_argument("--output", type=Path, help="write canonical JSON to this file")
    arguments = parser.parse_args()
    root = arguments.repo_root.resolve()
    inventory = arguments.inventory_dir
    if not inventory.is_absolute():
        inventory = root / inventory
    try:
        resolved = resolve(root, inventory)
        if arguments.profile:
            matches = [profile for profile in resolved["profiles"] if profile["profile"] == arguments.profile]
            if len(matches) != 1:
                raise ProfileError(f"unknown or ambiguous profile: {arguments.profile}")
            resolved = {
                "fallback": resolved["fallback"],
                "matrix_sha256": resolved["matrix_sha256"],
                "profile": matches[0],
                "schema": resolved["schema"],
            }
        encoded = _canonical_bytes(resolved)
        if arguments.output:
            _write_output(arguments.output, encoded)
        else:
            sys.stdout.buffer.write(encoded)
    except (ProfileError, OSError) as error:
        print(f"ci profile resolution failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
