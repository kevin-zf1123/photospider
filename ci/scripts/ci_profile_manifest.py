#!/usr/bin/env python3
"""Resolve versioned CI security roles with one bounded mainline fallback.

Future candidates emit ``build_profile_matrix_v1.tsv``, its SHA-256 sidecar,
and ``ci_security_roles_v1.tsv`` under the configured CI inventory directory.
The matrix owns every option, dependency, platform, concrete role selector,
and fuzz bound; the roles TSV contains only profile-to-role references. This
protected reader accepts that complete cross-bound set or, while current
``main`` does not emit it, a protected fallback whose candidate-source hashes
must all match. Partial, stale, duplicate, unknown, or non-canonical inputs fail
closed.
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


def _canonical_object(text: str, context: str) -> dict[str, Any]:
    """Decode one canonical compact JSON object with duplicate-key rejection."""
    try:
        value = json.loads(text, object_pairs_hook=_unique_object)
    except (json.JSONDecodeError, ProfileError) as error:
        raise ProfileError(f"{context}: invalid JSON object: {error}") from error
    if not isinstance(value, dict):
        raise ProfileError(f"{context}: matrix payload must be an object")
    if json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) != text:
        raise ProfileError(f"{context}: matrix payload JSON is not canonical")
    return value


def _exact_fields(value: dict[str, Any], expected: set[str], context: str) -> None:
    """Require one matrix object to contain exactly its versioned fields."""
    if set(value) != expected:
        raise ProfileError(
            f"{context}: fields differ: missing={sorted(expected - set(value))}, "
            f"unknown={sorted(set(value) - expected)}"
        )


def _value_string_array(value: Any, context: str, *, allow_empty: bool = False) -> list[str]:
    """Validate a sorted unique JSON string array already decoded from JSON."""
    if (
        not isinstance(value, list)
        or (not value and not allow_empty)
        or not all(isinstance(item, str) and item for item in value)
    ):
        qualifier = "a string array" if allow_empty else "a nonempty string array"
        raise ProfileError(f"{context}: expected {qualifier}")
    if value != sorted(value) or len(value) != len(set(value)):
        raise ProfileError(f"{context}: array must be sorted and unique")
    return value


def _load_matrix(root: Path, path: Path) -> dict[str, dict[str, dict[str, Any]]]:
    """Parse and cross-validate the complete ``build_profile_matrix_v1`` grammar.

    Records after the schema are bytewise sorted three-field TSV rows:
    ``kind``, canonical identifier, and a compact canonical JSON object. The
    fixed v1 kinds and their exact fields are validated here so a candidate
    cannot smuggle an unknown option, reference, platform, role, or lock into a
    trusted security runner.

    Args:
        root: Candidate repository root used to validate protected lock paths.
        path: Exact generated matrix file.

    Returns:
        Per-kind indexes of validated canonical matrix records.

    Raises:
        ProfileError: If syntax, ordering, fields, values, or cross-references
            differ from the frozen v1 contract.
    """
    rows = _read_canonical_tsv(path)
    if rows[0] != ["schema", MATRIX_SCHEMA]:
        raise ProfileError(f"{path}: missing or unknown first schema record")
    if not rows[1:] or rows[1:] != sorted(rows[1:]):
        raise ProfileError(f"{path}: matrix records are empty or not bytewise sorted")
    schemas = {
        "capability": {"default", "dependencies", "option", "platforms"},
        "component": {"dependencies", "targets"},
        "dependency": {"cmake_package", "lock", "mode", "vcpkg_port"},
        "profile": {"cmake_args", "components", "dependencies", "platforms", "roles"},
        "role": set(),
        "target": {"capabilities", "component"},
    }
    records: dict[str, dict[str, dict[str, Any]]] = {
        kind: {} for kind in sorted(schemas)
    }
    option_owners: dict[str, str] = {}
    for line_number, fields in enumerate(rows[1:], 2):
        context = f"{path}:{line_number}"
        if len(fields) != 3:
            raise ProfileError(f"{context}: matrix record requires three fields")
        kind, raw_identifier, payload = fields
        if kind not in schemas:
            raise ProfileError(f"{context}: unknown matrix record kind {kind!r}")
        identifier = (
            _target_identifier(raw_identifier, context)
            if kind == "target"
            else _identifier(raw_identifier, context)
        )
        if identifier in records[kind]:
            raise ProfileError(f"{context}: duplicate {kind} identifier {identifier}")
        value = _canonical_object(payload, context)
        if kind != "role":
            _exact_fields(value, schemas[kind], context)

        if kind == "capability":
            if not isinstance(value["default"], bool):
                raise ProfileError(f"{context}: capability default must be boolean")
            _value_string_array(value["dependencies"], context, allow_empty=True)
            _value_string_array(value["platforms"], context)
            if not set(value["platforms"]).issubset({"Darwin", "Linux"}):
                raise ProfileError(f"{context}: unsupported capability platform")
            option = value["option"]
            if not isinstance(option, str) or not re.fullmatch(r"[A-Z][A-Z0-9_]*", option):
                raise ProfileError(f"{context}: invalid CMake capability option")
            if option in option_owners:
                raise ProfileError(
                    f"{context}: CMake option {option} is also owned by {option_owners[option]}"
                )
            option_owners[option] = identifier
        elif kind == "component":
            _value_string_array(value["dependencies"], context, allow_empty=True)
            _value_string_array(value["targets"], context)
        elif kind == "dependency":
            if not isinstance(value["cmake_package"], str) or not re.fullmatch(
                r"[A-Za-z][A-Za-z0-9_.+-]*", value["cmake_package"]
            ):
                raise ProfileError(f"{context}: invalid CMake package identity")
            if value["mode"] not in ("optional", "required"):
                raise ProfileError(f"{context}: dependency mode must be optional or required")
            if not isinstance(value["vcpkg_port"], str) or not re.fullmatch(
                r"[a-z0-9][a-z0-9-]*", value["vcpkg_port"]
            ):
                raise ProfileError(f"{context}: invalid vcpkg port identity")
            lock = value["lock"]
            if not isinstance(lock, str) or not re.fullmatch(r"ci/locks/[A-Za-z0-9_.-]+", lock):
                raise ProfileError(f"{context}: dependency lock is not a protected lock path")
            lock_path = root / lock
            if not lock_path.is_file() or lock_path.is_symlink():
                raise ProfileError(f"{context}: protected dependency lock is unavailable: {lock}")
        elif kind == "profile":
            cmake_args = _value_string_array(value["cmake_args"], context)
            if not all(
                re.fullmatch(r"-D[A-Z][A-Z0-9_]*=[A-Za-z0-9_.+-]+", item)
                for item in cmake_args
            ):
                raise ProfileError(f"{context}: invalid profile CMake cache argument")
            cmake_options = [item[2:].split("=", 1)[0] for item in cmake_args]
            if len(cmake_options) != len(set(cmake_options)):
                raise ProfileError(f"{context}: profile repeats a CMake option identity")
            for field in ("components", "dependencies", "platforms", "roles"):
                _value_string_array(value[field], context)
            if not set(value["platforms"]).issubset({"Darwin", "Linux"}):
                raise ProfileError(f"{context}: unsupported profile platform")
        elif kind == "role":
            role_kind = value.get("kind")
            if role_kind not in ("artifact", "ctest-label", "fuzz-target", "target"):
                raise ProfileError(f"{context}: unknown role kind {role_kind!r}")
            role_fields = {"kind", "selector"}
            if role_kind == "fuzz-target":
                role_fields |= {
                    "corpus_sha256",
                    "job_timeout_minutes",
                    "max_len",
                    "production_limit",
                    "production_limit_symbol",
                    "runs",
                    "seed",
                    "timeout_seconds",
                }
            _exact_fields(value, role_fields, context)
            selector = value["selector"]
            if not isinstance(selector, str) or not re.fullmatch(
                r"[A-Za-z][A-Za-z0-9_.+-]*", selector
            ):
                raise ProfileError(f"{context}: invalid role selector")
            if role_kind == "fuzz-target":
                numeric_fields = (
                    "job_timeout_minutes",
                    "max_len",
                    "production_limit",
                    "runs",
                    "seed",
                    "timeout_seconds",
                )
                if not all(
                    isinstance(value[field], int)
                    and not isinstance(value[field], bool)
                    and value[field] > 0
                    for field in numeric_fields
                ):
                    raise ProfileError(f"{context}: fuzz role bounds must be positive integers")
                if value["seed"] != 1 or value["runs"] != 1000:
                    raise ProfileError(f"{context}: fuzz smoke requires seed 1 and exactly 1000 runs")
                if (
                    value["production_limit"] > 16 * 1024 * 1024
                    or value["max_len"] > value["production_limit"]
                ):
                    raise ProfileError(f"{context}: fuzz input bound exceeds its production limit")
                if not isinstance(value["production_limit_symbol"], str) or not re.fullmatch(
                    r"[A-Za-z_][A-Za-z0-9_:]*", value["production_limit_symbol"]
                ):
                    raise ProfileError(f"{context}: invalid production-bound symbol")
                corpus = value["corpus_sha256"]
                if corpus is not None and (
                    not isinstance(corpus, str) or not re.fullmatch(r"[0-9a-f]{64}", corpus)
                ):
                    raise ProfileError(f"{context}: malformed corpus digest")
        else:
            _value_string_array(value["capabilities"], context, allow_empty=True)
            _identifier(value["component"], context)
        records[kind][identifier] = value

    dependencies = records["dependency"]
    capabilities = records["capability"]
    components = records["component"]
    targets = records["target"]
    roles = records["role"]
    for identifier, value in capabilities.items():
        for dependency in value["dependencies"]:
            if dependency not in dependencies:
                raise ProfileError(f"{path}: capability {identifier} references unknown dependency {dependency}")
    for identifier, value in components.items():
        for dependency in value["dependencies"]:
            if dependency not in dependencies:
                raise ProfileError(f"{path}: component {identifier} references unknown dependency {dependency}")
        for target in value["targets"]:
            if target not in targets or targets[target]["component"] != identifier:
                raise ProfileError(f"{path}: component {identifier} has an invalid target reference {target}")
    for identifier, value in targets.items():
        if value["component"] not in components or identifier not in components[value["component"]]["targets"]:
            raise ProfileError(f"{path}: target {identifier} has no reciprocal component reference")
        for capability in value["capabilities"]:
            if capability not in capabilities:
                raise ProfileError(f"{path}: target {identifier} references unknown capability {capability}")
    for identifier, value in roles.items():
        if value["kind"] in ("target", "fuzz-target") and value["selector"] not in targets:
            raise ProfileError(f"{path}: role {identifier} references unknown target {value['selector']}")
    for identifier, value in records["profile"].items():
        for component in value["components"]:
            if component not in components:
                raise ProfileError(f"{path}: profile {identifier} references unknown component {component}")
        for dependency in value["dependencies"]:
            if dependency not in dependencies:
                raise ProfileError(f"{path}: profile {identifier} references unknown dependency {dependency}")
        for role in value["roles"]:
            if role not in roles:
                raise ProfileError(f"{path}: profile {identifier} references unknown role {role}")
        for argument in value["cmake_args"]:
            option = argument[2:].split("=", 1)[0]
            if option not in option_owners:
                raise ProfileError(f"{path}: profile {identifier} references unknown CMake option {option}")
    if not all(records[kind] for kind in schemas):
        raise ProfileError(f"{path}: every v1 matrix record kind must be nonempty")
    return records


def _profile_values(
    matrix: dict[str, dict[str, dict[str, Any]]], profile: str, context: str
) -> tuple[dict[str, Any], dict[str, dict[str, Any]], list[str]]:
    """Resolve one profile plus its exact roles and protected dependency ports."""
    profile_value = matrix["profile"].get(profile)
    if profile_value is None:
        raise ProfileError(f"{context}: security role references unknown profile {profile}")
    role_values = {role: matrix["role"][role] for role in profile_value["roles"]}
    ports = sorted(
        matrix["dependency"][dependency]["vcpkg_port"]
        for dependency in profile_value["dependencies"]
    )
    if len(ports) != len(set(ports)):
        raise ProfileError(f"{context}: profile dependencies resolve duplicate vcpkg ports")
    return profile_value, role_values, ports


def _require_security_cmake_isolation(profile: str, arguments: list[str], context: str) -> None:
    """Enforce mutually exclusive ASan, TSan, and fuzz cache selections."""
    parsed = {item[2:].split("=", 1)[0]: item.split("=", 1)[1] for item in arguments}
    required = {
        "sanitizer-asan": {"USE_ASAN": "ON", "USE_TSAN": "OFF"},
        "sanitizer-tsan": {"USE_ASAN": "OFF", "USE_TSAN": "ON"},
        "fuzz-codecs": {
            "PHOTOSPIDER_BUILD_FUZZERS": "ON",
            "USE_ASAN": "OFF",
            "USE_TSAN": "OFF",
        },
    }[profile]
    if any(parsed.get(option) != expected for option, expected in required.items()):
        raise ProfileError(f"{context}: {profile} CMake isolation is incomplete or contradictory")
    if profile != "fuzz-codecs" and parsed.get("PHOTOSPIDER_BUILD_FUZZERS") == "ON":
        raise ProfileError(f"{context}: sanitizer profile must not enable fuzz targets")


def _load_versioned(root: Path, inventory: Path) -> dict[str, Any]:
    """Load and cross-bind a complete versioned matrix/security-role set."""
    matrix_path = inventory / "build_profile_matrix_v1.tsv"
    digest_path = inventory / "build_profile_matrix_v1.tsv.sha256"
    roles_path = inventory / "ci_security_roles_v1.tsv"
    matrix = _load_matrix(root, matrix_path)
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
            if len(fields) != 4:
                raise ProfileError(f"{context}: sanitizer record requires four fields")
            _, profile, target_roles_json, label_roles_json = fields
            profile = _identifier(profile, context)
            if profile in seen_profiles:
                raise ProfileError(f"{context}: duplicate security profile {profile}")
            seen_profiles.add(profile)
            if profile not in ("sanitizer-asan", "sanitizer-tsan"):
                raise ProfileError(f"{context}: unknown sanitizer profile {profile}")
            profile_value, role_values, dependencies = _profile_values(matrix, profile, context)
            if profile_value["platforms"] != ["Darwin", "Linux"]:
                raise ProfileError(f"{context}: sanitizer platform set must be exact Darwin/Linux")
            _require_security_cmake_isolation(profile, profile_value["cmake_args"], context)
            target_roles = _string_array(target_roles_json, context)
            label_roles = _string_array(label_roles_json, context)
            artifact_roles = [
                role for role, value in role_values.items() if value["kind"] == "artifact"
            ]
            if len(artifact_roles) != 1:
                raise ProfileError(f"{context}: sanitizer profile requires one artifact role")
            if set(target_roles) | set(label_roles) | set(artifact_roles) != set(role_values):
                raise ProfileError(f"{context}: sanitizer role set differs from matrix profile roles")
            if any(role_values.get(role, {}).get("kind") != "target" for role in target_roles):
                raise ProfileError(f"{context}: sanitizer target role kind mismatch")
            if any(role_values.get(role, {}).get("kind") != "ctest-label" for role in label_roles):
                raise ProfileError(f"{context}: sanitizer CTest-label role kind mismatch")
            sanitizers.append(
                {
                    "artifact_role": role_values[artifact_roles[0]]["selector"],
                    "cmake_args": profile_value["cmake_args"],
                    "ctest_labels": sorted(role_values[role]["selector"] for role in label_roles),
                    "platforms": profile_value["platforms"],
                    "profile": profile,
                    "targets": sorted(role_values[role]["selector"] for role in target_roles),
                    "vcpkg_dependencies": dependencies,
                }
            )
        elif fields[0] == "fuzz":
            if len(fields) != 3:
                raise ProfileError(f"{context}: fuzz record requires three fields")
            _, profile, target_role = fields
            profile = _identifier(profile, context)
            if profile != "fuzz-codecs":
                raise ProfileError(f"{context}: unknown fuzz profile {profile}")
            target_role = _identifier(target_role, context)
            profile_value, role_values, dependencies = _profile_values(matrix, profile, context)
            if profile_value["platforms"] != ["Darwin", "Linux"]:
                raise ProfileError(f"{context}: fuzz platform set must be exact Darwin/Linux")
            _require_security_cmake_isolation(profile, profile_value["cmake_args"], context)
            role = role_values.get(target_role)
            if role is None or role["kind"] != "fuzz-target":
                raise ProfileError(f"{context}: fuzz target role is missing or has the wrong kind")
            target = _target_identifier(role["selector"], context)
            key = (profile, target)
            if key in seen_fuzz_targets:
                raise ProfileError(f"{context}: duplicate fuzz target {profile}/{target}")
            seen_fuzz_targets.add(key)
            seed = role["seed"]
            runs = role["runs"]
            timeout = role["timeout_seconds"]
            job_timeout = role["job_timeout_minutes"]
            max_len = role["max_len"]
            production_limit = role["production_limit"]
            symbol = role["production_limit_symbol"]
            corpus_digest = role["corpus_sha256"]
            profile_value = fuzz_profiles.setdefault(
                profile,
                {
                    "artifact_role": "",
                    "cmake_args": profile_value["cmake_args"],
                    "corpus_sha256": corpus_digest,
                    "job_timeout_minutes": job_timeout,
                    "platforms": profile_value["platforms"],
                    "profile": profile,
                    "runs": runs,
                    "seed": seed,
                    "targets": [],
                    "vcpkg_dependencies": dependencies,
                },
            )
            artifact_roles = [
                role_name
                for role_name, role_value in role_values.items()
                if role_value["kind"] == "artifact"
            ]
            fuzz_roles = {
                role_name
                for role_name, role_value in role_values.items()
                if role_value["kind"] == "fuzz-target"
            }
            if len(artifact_roles) != 1 or set(role_values) != fuzz_roles | set(artifact_roles):
                raise ProfileError(f"{context}: fuzz profile roles must be targets plus one artifact")
            artifact_selector = role_values[artifact_roles[0]]["selector"]
            if not profile_value["artifact_role"]:
                profile_value["artifact_role"] = artifact_selector
            identity = (
                profile_value["platforms"], profile_value["seed"], profile_value["runs"],
                profile_value["job_timeout_minutes"], profile_value["corpus_sha256"],
                profile_value["artifact_role"],
                profile_value["vcpkg_dependencies"],
            )
            current = (
                matrix["profile"][profile]["platforms"], seed, runs, job_timeout,
                corpus_digest,
                artifact_selector,
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
    if seen_profiles != {"sanitizer-asan", "sanitizer-tsan"}:
        raise ProfileError(f"{roles_path}: exact ASan and TSan role records are required")
    for profile in fuzz_profiles.values():
        profile["targets"].sort(key=lambda item: item["name"])
        expected_targets = {
            value["selector"]
            for role, value in matrix["role"].items()
            if role in matrix["profile"][profile["profile"]]["roles"]
            and value["kind"] == "fuzz-target"
        }
        if {target["name"] for target in profile["targets"]} != expected_targets:
            raise ProfileError(f"{roles_path}: fuzz role records do not cover the matrix target set")
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
                character in invocation["filter"]
                for character in ("\0", "\t", "\r", "\n")
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
    return _load_versioned(root, inventory) if all(present) else _load_fallback(root)


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
