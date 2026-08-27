#!/usr/bin/env python3
"""Create and verify the ordinary CTest runtime-closure contract.

The producer consumes the complete post-build ``ctest --show-only=json-v1``
inventory, excludes only tests carrying the exact ``build-smoke`` label, and
records every CTest control include plus the build/source files referenced by
ordinary commands and properties. Relative command, argument,
``REQUIRED_FILES``, ``ENVIRONMENT``, and ``ENVIRONMENT_MODIFICATION`` paths are
resolved against each test's effective ``WORKING_DIRECTORY``. The directory is
only a resolution base; its unrelated contents are never selected. The runtime
role also carries every built shared library and maintained plugin/trust tree
needed by those commands.

The consumer re-runs CTest discovery only after attestation and restoration at
its canonical build path. A ``*_NOT_BUILT`` placeholder, changed ordinary
inventory, missing include, executable, data, shared library, plugin, or trust
input fails before ordinary CTest can execute.

The pre-attestation protected verifier never invokes CTest or interprets a
candidate CMake program. It parses the retained generated control files through
one strict, side-effect-free command allowlist, compares their complete test
records with the retained raw JSON and both archived closures, and rejects any
unknown command or ambiguous path spelling. Root tokens are applied only to
structured absolute path fields that equal a maintained root or one of its
component descendants; arbitrary substring replacement is forbidden.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Mapping, NamedTuple, Sequence


BUILD_SMOKE_LABEL = "build-smoke"
"""Exact label excluded from the ordinary CTest runtime closure."""

CLOSURE_RELATIVE_PATH = PurePosixPath(
    "generated/ci_inventory/ordinary_ctest_closure_v1.json"
)
"""Canonical producer-build path of the ordinary CTest closure manifest."""

_RUNTIME_ROOTS = frozenset(
    {
        "plugins",
        "policies",
        "schedulers",
        "test_plugins",
        "test_policies",
        "test_schedulers",
    }
)
"""Build roots whose regular contents are runtime-loaded rather than linked."""

_SHARED_LIBRARY_ROOTS = _RUNTIME_ROOTS | {"bin", "lib", "tests"}
"""Top-level output roots eligible for conservative shared-library closure."""

_FORBIDDEN_RUNTIME_SUFFIXES = (
    ".a",
    ".d",
    ".gch",
    ".lib",
    ".o",
    ".obj",
    ".pch",
)
"""Build residue that cannot satisfy an ordinary runtime dependency."""

_CONTROL_DIRECTIVE = re.compile(
    r"(?m)^[ \t]*(include|subdirs)[ \t]*\([ \t]*" r'(?:"([^"\r\n]*)"|([^\s\)]+))'
)
"""Generated CTest include/subdirectory directive and its first argument."""

_BOOLEAN_PROPERTIES = frozenset(
    {"DISABLED", "PROCESSOR_AFFINITY", "RUN_SERIAL", "WILL_FAIL"}
)
"""CTest properties serialized as booleans by JSON-v1."""

_FLOAT_PROPERTIES = frozenset({"COST", "TIMEOUT"})
"""CTest properties serialized as numbers by JSON-v1."""

_INTEGER_PROPERTIES = frozenset({"PROCESSORS", "SKIP_RETURN_CODE"})
"""CTest properties serialized as integers by JSON-v1."""

_LIST_PROPERTIES = frozenset(
    {
        "ATTACHED_FILES",
        "ATTACHED_FILES_ON_FAIL",
        "DEPENDS",
        "ENVIRONMENT",
        "ENVIRONMENT_MODIFICATION",
        "FAIL_REGULAR_EXPRESSION",
        "FIXTURES_CLEANUP",
        "FIXTURES_REQUIRED",
        "FIXTURES_SETUP",
        "LABELS",
        "PASS_REGULAR_EXPRESSION",
        "REQUIRED_FILES",
        "RESOURCE_GROUPS",
        "RESOURCE_LOCK",
        "SKIP_REGULAR_EXPRESSION",
    }
)
"""CTest properties serialized as string arrays by JSON-v1."""

_STRING_PROPERTIES = frozenset({"GENERATED_RESOURCE_SPEC_FILE", "WORKING_DIRECTORY"})
"""CTest properties serialized as strings by JSON-v1."""

_IGNORED_CONTROL_PROPERTIES = frozenset({"_BACKTRACE_TRIPLES"})
"""Generated CMake-only metadata intentionally omitted by CTest JSON-v1."""


class CTestClosureError(ValueError):
    """Report malformed inventory, unsafe paths, or an incomplete closure."""


class _CMakeCommand(NamedTuple):
    """Represent one decoded generated CTest control command.

    Attributes:
        name: Lowercase command identity.
        arguments: Decoded argument vector without CMake evaluation.
        line: One-based physical start line used in diagnostics.
    """

    name: str
    arguments: tuple[str, ...]
    line: int


def _is_shared_library_name(name: str) -> bool:
    """Return whether one filename is a maintained Darwin/Linux DSO spelling."""
    return name.endswith((".dylib", ".so")) or ".so." in name


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build one JSON object while rejecting duplicate member names."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CTestClosureError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    """Read strict UTF-8 JSON from one regular, non-link file."""
    if not path.is_file() or path.is_symlink():
        raise CTestClosureError(f"expected a regular JSON file: {path}")
    try:
        return _load_json_bytes(path.read_bytes(), str(path))
    except (OSError, CTestClosureError) as error:
        raise CTestClosureError(f"cannot read strict JSON {path}: {error}") from error


def _load_json_bytes(value: bytes, context: str) -> Any:
    """Decode one retained UTF-8 JSON object with duplicate rejection.

    Args:
        value: Exact bytes already retained by the protected caller.
        context: Stable artifact/path identity used in diagnostics.

    Returns:
        Detached decoded JSON value.

    Raises:
        CTestClosureError: UTF-8, JSON syntax, or member uniqueness fails.

    Note:
        This function performs no pathname I/O. Cross-job coverage validation
        passes it bytes measured by the protected raw-bundle descriptor reader.
    """
    try:
        return json.loads(
            value.decode("utf-8"), object_pairs_hook=_unique_object
        )
    except (UnicodeError, json.JSONDecodeError, CTestClosureError) as error:
        raise CTestClosureError(
            f"cannot decode strict CTest JSON {context}: {error}"
        ) from error


def _canonical_bytes(value: Any) -> bytes:
    """Serialize one closure value as canonical ASCII JSON plus newline."""
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        + "\n"
    ).encode("ascii")


def _sha256_file(path: Path) -> str:
    """Hash one regular, non-link file without retaining its bytes."""
    if not path.is_file() or path.is_symlink():
        raise CTestClosureError(f"expected a regular closure input: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _relative_regular(path: Path, root: Path, context: str) -> PurePosixPath:
    """Return one canonical regular-file path below a real root.

    Args:
        path: Candidate file whose parents and final component are inspected.
        root: Maintained build or source root.
        context: Stable diagnostic label.

    Returns:
        Canonical POSIX path relative to ``root``.

    Raises:
        CTestClosureError: The path escapes, aliases, or is not a regular file.

    Note:
        ``resolve`` detects parent-link escapes; the explicit final-link check
        prevents a symlink that happens to resolve back below the same root.
    """
    if path.is_symlink() or not path.is_file():
        raise CTestClosureError(f"{context} is missing, linked, or nonregular: {path}")
    try:
        relative = path.resolve().relative_to(root.resolve())
    except (OSError, ValueError) as error:
        raise CTestClosureError(
            f"{context} escapes its maintained root: {path}"
        ) from error
    pure = PurePosixPath(relative.as_posix())
    if not pure.parts or any(part in ("", ".", "..") for part in pure.parts):
        raise CTestClosureError(f"{context} is non-canonical: {path}")
    return pure


def _root_identities(
    source_root: Path, build_root: Path
) -> tuple[tuple[PurePosixPath, str], ...]:
    """Return deterministic component-aware maintained-root identities.

    Args:
        source_root: Exact producer source root recorded by the build.
        build_root: Exact producer build root recorded by the build.

    Returns:
        Build then source roots paired with their stable closure tokens. The
        more specific build root is intentionally considered first because the
        maintained build tree is normally a component descendant of source.

    Raises:
        CTestClosureError: A root is relative/noncanonical, both roots are the
            same path, or source is nested below build and would make source
            data indistinguishable from generated build data.
    """
    roots: list[PurePosixPath] = []
    for context, root in (("source", source_root), ("build", build_root)):
        # Producer calls may enter Darwin's ``/var`` alias while CTest emits
        # the physical ``/private/var`` spelling. Resolve only an existing
        # root object; pre-attestation comparison intentionally keeps deleted
        # producer roots as their retained lexical identity.
        try:
            canonical_root = root.resolve(strict=True)
        except OSError:
            canonical_root = root
        text = str(canonical_root)
        pure = PurePosixPath(text)
        if (
            not pure.is_absolute()
            or pure.as_posix() != text
            or any(part in ("", ".", "..") for part in pure.parts[1:])
        ):
            raise CTestClosureError(
                f"CTest {context} root is not a canonical absolute path"
            )
        roots.append(pure)
    source, build = roots
    if source == build or source.is_relative_to(build):
        raise CTestClosureError("CTest source/build root topology is ambiguous")
    return ((build, "${BUILD_ROOT}"), (source, "${SOURCE_ROOT}"))


def _contains_root_text(value: str, roots: Sequence[tuple[PurePosixPath, str]]) -> bool:
    """Return whether a scalar lexically contains any maintained-root bytes."""
    return any(root.as_posix() in value for root, _ in roots)


def _normalize_absolute_path(
    value: str,
    roots: Sequence[tuple[PurePosixPath, str]],
    context: str,
) -> str:
    """Normalize one complete absolute path at component boundaries only.

    Args:
        value: A decoded scalar already known to be one structured path field.
        roots: Maintained roots returned by :func:`_root_identities`.
        context: Stable diagnostic identity.

    Returns:
        The original external/nonabsolute value, or one root-token spelling for
        an exact maintained root or component descendant.

    Raises:
        CTestClosureError: Maintained-root bytes occur in a root-prefix sibling,
            embedded string, quoted value, noncanonical path, or other shape
            that cannot be proven to name one exact path.
    """
    if not value.startswith("/"):
        if _contains_root_text(value, roots):
            raise CTestClosureError(
                f"{context} embeds maintained-root bytes ambiguously"
            )
        return value
    pure = PurePosixPath(value)
    if pure.as_posix() != value or any(
        part in ("", ".", "..") for part in pure.parts[1:]
    ):
        if _contains_root_text(value, roots):
            raise CTestClosureError(f"{context} path is non-canonical")
        return value
    for root, token in roots:
        if pure == root:
            return token
        if pure.is_relative_to(root):
            relative = pure.relative_to(root).as_posix()
            return f"{token}/{relative}"
    if _contains_root_text(value, roots):
        raise CTestClosureError(
            f"{context} is a maintained-root prefix sibling or embedded path"
        )
    return value


def _normalize_path_list(
    value: str,
    roots: Sequence[tuple[PurePosixPath, str]],
    context: str,
    delimiter: str,
) -> str:
    """Normalize a nonempty structured path list without guessing boundaries."""
    items = value.split(delimiter)
    if any(not item for item in items):
        if _contains_root_text(value, roots):
            raise CTestClosureError(f"{context} has an ambiguous empty list item")
        return value
    return delimiter.join(
        _normalize_absolute_path(item, roots, context) for item in items
    )


def _normalize_assignment_value(
    value: str,
    roots: Sequence[tuple[PurePosixPath, str]],
    context: str,
    *,
    path_list: bool = False,
) -> str:
    """Normalize one explicit ``KEY=value`` or option assignment.

    Unknown assignment keys containing maintained roots fail instead of
    allowing a new quoting/list convention to become an implicit authority.
    """
    if "=" not in value:
        return _normalize_absolute_path(value, roots, context)
    key, assigned = value.split("=", 1)
    if re.fullmatch(
        r"(?:[A-Za-z_][A-Za-z0-9_]*|--?[A-Za-z0-9_.+-]+|"
        r"-D[A-Za-z0-9_]+(?::[A-Za-z0-9_]+)?)",
        key,
    ) is None:
        if _contains_root_text(value, roots):
            raise CTestClosureError(f"{context} has an ambiguous assignment key")
        return value
    if not assigned and _contains_root_text(value, roots):
        raise CTestClosureError(f"{context} has an empty path assignment")
    if path_list and os.pathsep in assigned:
        normalized = _normalize_path_list(
            assigned, roots, context, os.pathsep
        )
    elif ";" in assigned:
        normalized = _normalize_path_list(assigned, roots, context, ";")
    else:
        normalized = _normalize_absolute_path(assigned, roots, context)
    return f"{key}={normalized}"


def _normalize_command(
    command: Sequence[str], source_root: Path, build_root: Path, test_name: str
) -> list[str]:
    """Normalize only structured command arguments for one CTest record."""
    roots = _root_identities(source_root, build_root)
    return [
        _normalize_assignment_value(
            argument,
            roots,
            f"CTest {test_name!r} command argument {index}",
        )
        for index, argument in enumerate(command)
    ]


def _normalize_property_value(
    name: str,
    value: Any,
    source_root: Path,
    build_root: Path,
    test_name: str,
) -> Any:
    """Normalize only property fields whose schema explicitly carries paths."""
    roots = _root_identities(source_root, build_root)
    context = f"CTest {test_name!r} property {name}"
    if name == "WORKING_DIRECTORY":
        if not isinstance(value, str):
            raise CTestClosureError(f"{context} is not a string")
        return _normalize_absolute_path(value, roots, context)
    if name == "REQUIRED_FILES":
        values = value if isinstance(value, list) else [value]
        if not all(isinstance(item, str) for item in values):
            raise CTestClosureError(f"{context} is not a string array")
        return [
            _normalize_absolute_path(item, roots, context) for item in values
        ]
    if name == "ENVIRONMENT":
        values = value if isinstance(value, list) else [value]
        if not all(isinstance(item, str) for item in values):
            raise CTestClosureError(f"{context} is not a string array")
        return [
            _normalize_assignment_value(
                item, roots, context, path_list=True
            )
            for item in values
        ]
    if name == "ENVIRONMENT_MODIFICATION":
        values = value if isinstance(value, list) else [value]
        normalized: list[str] = []
        for item in values:
            if not isinstance(item, str) or "=" not in item or ":" not in item:
                raise CTestClosureError(f"{context} entry is malformed")
            variable, modification = item.split("=", 1)
            operation, modified = modification.split(":", 1)
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", variable) is None:
                raise CTestClosureError(f"{context} variable is malformed")
            if operation in {"reset", "unset"}:
                if modified:
                    raise CTestClosureError(f"{context} reset/unset has a value")
                normalized.append(item)
                continue
            if operation not in {
                "cmake_list_append",
                "cmake_list_prepend",
                "path_list_append",
                "path_list_prepend",
                "set",
                "string_append",
                "string_prepend",
            }:
                raise CTestClosureError(f"{context} operation is unsupported")
            if operation.startswith("path_list_") and os.pathsep in modified:
                modified = _normalize_path_list(
                    modified, roots, context, os.pathsep
                )
            elif operation.startswith("cmake_list_") and ";" in modified:
                modified = _normalize_path_list(modified, roots, context, ";")
            else:
                modified = _normalize_absolute_path(modified, roots, context)
            normalized.append(f"{variable}={operation}:{modified}")
        return normalized
    if isinstance(value, str):
        if _contains_root_text(value, roots):
            raise CTestClosureError(
                f"{context} contains a root outside an allowlisted path field"
            )
        return value
    if isinstance(value, list):
        return [
            _normalize_property_value(
                name, item, source_root, build_root, test_name
            )
            for item in value
        ]
    if isinstance(value, dict):
        return {
            key: _normalize_property_value(
                name, item, source_root, build_root, test_name
            )
            for key, item in sorted(value.items())
        }
    if value is None or isinstance(value, (bool, int, float)):
        return value
    raise CTestClosureError("CTest inventory contains a non-JSON property value")


def _properties(test_name: str, value: Any) -> dict[str, Any]:
    """Convert one CTest property array to a unique sorted mapping."""
    if value is None:
        return {}
    if not isinstance(value, list):
        raise CTestClosureError(f"CTest properties for {test_name!r} are not an array")
    result: dict[str, Any] = {}
    for index, item in enumerate(value):
        if not isinstance(item, dict) or set(item) != {"name", "value"}:
            raise CTestClosureError(
                f"CTest property {index} for {test_name!r} is malformed"
            )
        name = item["name"]
        if not isinstance(name, str) or not name or name in result:
            raise CTestClosureError(
                f"CTest property {index} for {test_name!r} is empty or duplicate"
            )
        result[name] = item["value"]
    return dict(sorted(result.items()))


def _labels(test_name: str, properties: Mapping[str, Any]) -> list[str]:
    """Return one sorted unique exact CTest label array."""
    value = properties.get("LABELS", [])
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise CTestClosureError(f"CTest LABELS for {test_name!r} are malformed")
    if len(value) != len(set(value)):
        raise CTestClosureError(f"CTest LABELS for {test_name!r} are duplicated")
    return sorted(value)


def _test_records(
    payload: Any,
    source_root: Path,
    build_root: Path,
    *,
    exclude_build_smoke: bool,
) -> list[dict[str, Any]]:
    """Normalize complete CTest records through structured path fields only.

    Args:
        payload: Strict decoded ``ctestInfo`` JSON-v1 value.
        source_root: Exact producer source-root identity.
        build_root: Exact producer build-root identity.
        exclude_build_smoke: Whether exact ``build-smoke`` records are omitted
            for the ordinary runtime closure.

    Returns:
        Sorted unique normalized records.

    Raises:
        CTestClosureError: Schema, identity, command, property, label, or
            structured root normalization is malformed or ambiguous.
    """
    if not isinstance(payload, dict) or payload.get("kind") != "ctestInfo":
        raise CTestClosureError("CTest inventory kind must be ctestInfo")
    version = payload.get("version")
    if (
        not isinstance(version, dict)
        or version.get("major") != 1
        or not isinstance(version.get("minor"), int)
    ):
        raise CTestClosureError("CTest inventory must use json-v1")
    tests = payload.get("tests")
    if not isinstance(tests, list) or not tests:
        raise CTestClosureError("CTest inventory is empty")
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, item in enumerate(tests, 1):
        if not isinstance(item, dict):
            raise CTestClosureError(f"CTest test {index} is not an object")
        name = item.get("name")
        if not isinstance(name, str) or not name or name in seen or "\0" in name:
            raise CTestClosureError(f"CTest test {index} has an invalid identity")
        seen.add(name)
        properties = _properties(name, item.get("properties"))
        labels = _labels(name, properties)
        if exclude_build_smoke and BUILD_SMOKE_LABEL in labels:
            continue
        command = item.get("command")
        if (
            not isinstance(command, list)
            or not command
            or not all(
                isinstance(argument, str) and "\0" not in argument
                for argument in command
            )
        ):
            raise CTestClosureError(f"ordinary CTest {name!r} has no valid command")
        disabled = properties.get("DISABLED", False)
        if not isinstance(disabled, bool):
            raise CTestClosureError(f"CTest DISABLED for {name!r} is not boolean")
        if name.endswith("_NOT_BUILT") or Path(command[0]).name.endswith("_NOT_BUILT"):
            raise CTestClosureError(
                f"ordinary CTest inventory contains NOT_BUILT: {name}"
            )
        records.append(
            {
                "command": _normalize_command(
                    command, source_root, build_root, name
                ),
                "disabled": disabled,
                "labels": labels,
                "name": name,
                "properties": {
                    property_name: _normalize_property_value(
                        property_name,
                        property_value,
                        source_root,
                        build_root,
                        name,
                    )
                    for property_name, property_value in properties.items()
                },
            }
        )
    if not records:
        raise CTestClosureError("ordinary CTest inventory is empty")
    records.sort(key=lambda record: record["name"])
    return records


def _ordinary_tests(
    payload: Any, source_root: Path, build_root: Path
) -> list[dict[str, Any]]:
    """Normalize every non-build-smoke test from one complete CTest payload."""
    return _test_records(
        payload,
        source_root,
        build_root,
        exclude_build_smoke=True,
    )


def ordinary_test_records(
    raw_inventory: bytes,
    context: str,
    source_root: Path,
    build_root: Path,
) -> list[dict[str, Any]]:
    """Normalize exact ordinary records from retained complete CTest JSON.

    Args:
        raw_inventory: Exact protected-measured ``ctestInfo`` JSON-v1 bytes.
        context: Stable raw/restored inventory identity used in diagnostics.
        source_root: Producer source root represented as ``${SOURCE_ROOT}``.
        build_root: Producer or restored build root represented as
            ``${BUILD_ROOT}``.

    Returns:
        Sorted unique normalized ordinary records after excluding only the
        exact :data:`BUILD_SMOKE_LABEL`.

    Raises:
        CTestClosureError: JSON, schema, duplicate identity, labels, command,
            disabled state, or ``NOT_BUILT`` representation is malformed.

    Note:
        This boundary never resolves or opens either root. Path/content closure
        validation remains the role-artifact verifier's separate duty.
    """
    payload = _load_json_bytes(raw_inventory, context)
    return _ordinary_tests(payload, source_root, build_root)


def complete_test_records(
    raw_inventory: bytes,
    context: str,
    source_root: Path,
    build_root: Path,
) -> list[dict[str, Any]]:
    """Return every exact CTest record, including build-smoke registrations.

    Args:
        raw_inventory: Exact retained complete CTest JSON-v1 bytes.
        context: Stable retained-input identity.
        source_root: Exact producer source root.
        build_root: Exact producer build root.

    Returns:
        Sorted normalized complete test records.

    Raises:
        CTestClosureError: The same strict schema/path checks as
            :func:`ordinary_test_records` fail.

    Note:
        The pre-attestation pure-data verifier uses this complete view to prove
        that control files cannot add, remove, relabel, or rewrite even a
        separately routed build-smoke registration.
    """
    payload = _load_json_bytes(raw_inventory, context)
    return _test_records(
        payload,
        source_root,
        build_root,
        exclude_build_smoke=False,
    )


def ordinary_test_names(raw_inventory: bytes, context: str) -> list[str]:
    """Return the exact ordinary-test name set from retained CTest JSON.

    Args:
        raw_inventory: Exact protected-measured ``ctestInfo`` JSON-v1 bytes.
        context: Stable raw/restored inventory identity used in diagnostics.

    Returns:
        Sorted unique names after excluding only exact ``build-smoke``.

    Raises:
        CTestClosureError: The same strict record/schema checks as
            :func:`ordinary_test_records` fail.

    Note:
        Impossible inert roots are safe because this convenience function
        returns names only and performs no path I/O.
    """
    records = ordinary_test_records(
        raw_inventory,
        context,
        Path("/__photospider_retained_source_root__"),
        Path("/__photospider_retained_build_root__"),
    )
    return [record["name"] for record in records]


def _directive_target(
    control: Path, command: str, raw_target: str, build_root: Path
) -> Path:
    """Resolve one generated CTest include/subdirectory target safely."""
    if not raw_target or "$" in raw_target or "\\" in raw_target:
        raise CTestClosureError(
            f"unsupported generated CTest {command} target in {control}: {raw_target!r}"
        )
    target = Path(raw_target)
    if not target.is_absolute():
        target = control.parent / target
    if command == "subdirs":
        target = target / "CTestTestfile.cmake"
    try:
        target.resolve().relative_to(build_root.resolve())
    except (OSError, ValueError) as error:
        raise CTestClosureError(
            f"generated CTest {command} target escapes the build root: {target}"
        ) from error
    return target


def _control_paths(build_root: Path) -> list[str]:
    """Recursively enumerate the exact acyclic generated CTest control graph.

    The iterative depth-first stack retains every entered control file until
    all of its children have exited. A back edge therefore encounters an
    active canonical path and fails instead of being hidden by ordinary
    visited-node de-duplication.
    """
    root_control = build_root / "CTestTestfile.cmake"
    pending: list[tuple[str, Path]] = [("enter", root_control)]
    visiting: set[Path] = set()
    visited: set[Path] = set()
    result: set[str] = set()
    while pending:
        phase, control = pending.pop()
        canonical = control.resolve()
        if phase == "exit":
            if canonical not in visiting:
                raise CTestClosureError(
                    f"generated CTest traversal state is inconsistent: {control}"
                )
            visiting.remove(canonical)
            visited.add(canonical)
            continue
        if canonical in visited:
            continue
        if canonical in visiting:
            raise CTestClosureError(f"generated CTest include cycle: {control}")
        visiting.add(canonical)
        relative = _relative_regular(control, build_root, "CTest control file")
        result.add(relative.as_posix())
        text = control.read_text(encoding="utf-8")
        matched_starts: set[int] = set()
        children: list[Path] = []
        for match in _CONTROL_DIRECTIVE.finditer(text):
            matched_starts.add(match.start())
            raw_target = match.group(2) or match.group(3)
            children.append(
                _directive_target(control, match.group(1), raw_target, build_root)
            )
        for suspicious in re.finditer(r"(?m)^[ \t]*(?:include|subdirs)[ \t]*\(", text):
            if suspicious.start() not in matched_starts:
                raise CTestClosureError(
                    f"cannot parse generated CTest directive in {control}"
                )
        pending.append(("exit", control))
        pending.extend(("enter", child) for child in reversed(children))
    if not result:
        raise CTestClosureError("CTest control graph is empty")
    return sorted(result)


def _parse_cmake_control(content: bytes, context: str) -> list[_CMakeCommand]:
    """Parse the side-effect-free generated CMake command syntax we allow.

    Args:
        content: Exact retained control-file bytes from a targeted archive.
        context: Stable relative file identity for diagnostics.

    Returns:
        Ordered decoded command records. This parser performs no expansion,
        include, condition, variable, filesystem, or subprocess operation.

    Raises:
        CTestClosureError: UTF-8, comments, quoting, bracket arguments, command
            shape, nesting, expansion syntax, or trailing bytes are ambiguous.

    Note:
        This is intentionally not a general CMake interpreter. It accepts the
        generated CTest subset needed below and leaves semantic allowlisting to
        :func:`control_test_records`. Unknown syntax fails before attestation.
    """
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError as error:
        raise CTestClosureError(
            f"generated CTest control is not UTF-8: {context}: {error}"
        ) from error
    if "\0" in text:
        raise CTestClosureError(f"generated CTest control contains NUL: {context}")

    commands: list[_CMakeCommand] = []
    index = 0
    line = 1
    length = len(text)

    def advance() -> str:
        """Consume one code point while maintaining the physical line."""
        nonlocal index, line
        character = text[index]
        index += 1
        if character == "\n":
            line += 1
        return character

    def skip_space_and_comments() -> None:
        """Consume whitespace and ordinary line comments outside arguments."""
        nonlocal index
        while index < length:
            if text[index].isspace():
                advance()
                continue
            if text[index] == "#":
                if text.startswith("#[", index):
                    raise CTestClosureError(
                        f"bracket comments are unsupported in {context}:{line}"
                    )
                while index < length and advance() != "\n":
                    pass
                continue
            break

    def decoded_escape() -> str:
        """Decode one bounded CMake escape without evaluating variables."""
        if index >= length:
            raise CTestClosureError(
                f"trailing CMake escape in {context}:{line}"
            )
        escaped = advance()
        return {"n": "\n", "r": "\r", "t": "\t"}.get(escaped, escaped)

    while True:
        skip_space_and_comments()
        if index >= length:
            break
        command_line = line
        name_match = re.match(r"[A-Za-z_][A-Za-z0-9_]*", text[index:])
        if name_match is None:
            raise CTestClosureError(
                f"cannot parse generated CTest command in {context}:{line}"
            )
        name = name_match.group(0).lower()
        for _ in name_match.group(0):
            advance()
        while index < length and text[index].isspace():
            advance()
        if index >= length or advance() != "(":
            raise CTestClosureError(
                f"generated CTest command lacks '(' in {context}:{command_line}"
            )
        arguments: list[str] = []
        while True:
            skip_space_and_comments()
            if index >= length:
                raise CTestClosureError(
                    f"unterminated generated CTest command in {context}:{command_line}"
                )
            if text[index] == ")":
                advance()
                break
            argument = ""
            explicit_empty = False
            if text[index] == '"':
                explicit_empty = True
                advance()
                while index < length and text[index] != '"':
                    if text[index] == "\\":
                        advance()
                        argument += decoded_escape()
                    else:
                        argument += advance()
                if index >= length:
                    raise CTestClosureError(
                        f"unterminated quoted CMake argument in {context}:{line}"
                    )
                advance()
            else:
                bracket = re.match(r"\[(=*)\[", text[index:])
                if bracket is not None:
                    explicit_empty = True
                    delimiter = "]" + bracket.group(1) + "]"
                    for _ in bracket.group(0):
                        advance()
                    end = text.find(delimiter, index)
                    if end < 0:
                        raise CTestClosureError(
                            f"unterminated bracket argument in {context}:{line}"
                        )
                    while index < end:
                        argument += advance()
                    for _ in delimiter:
                        advance()
                else:
                    while index < length and not text[index].isspace() and text[index] != ")":
                        if text[index] == "(":
                            raise CTestClosureError(
                                f"nested unquoted '(' is unsupported in {context}:{line}"
                            )
                        if text[index] == "\\":
                            advance()
                            argument += decoded_escape()
                        else:
                            argument += advance()
            if not argument and not explicit_empty:
                raise CTestClosureError(
                    f"ambiguous empty generated CTest argument in {context}:{line}"
                )
            if "$" in argument:
                raise CTestClosureError(
                    f"generated CTest expansion is forbidden in {context}:{line}"
                )
            arguments.append(argument)
        commands.append(_CMakeCommand(name, tuple(arguments), command_line))
    if not commands:
        raise CTestClosureError(f"generated CTest control file is empty: {context}")
    return commands


def _control_relative_target(
    raw_target: str,
    owner: PurePosixPath,
    build_root: PurePosixPath,
    *,
    subdirectory: bool,
) -> str:
    """Resolve one retained include/subdirectory target by path components.

    Args:
        raw_target: Decoded CMake argument.
        owner: Relative control file containing the directive.
        build_root: Exact producer build-root identity.
        subdirectory: Whether ``CTestTestfile.cmake`` is appended.

    Returns:
        Canonical build-relative target.

    Raises:
        CTestClosureError: The target escapes, uses a root-prefix sibling,
            contains ambiguous components, or names the build root itself.
    """
    if (
        not raw_target
        or raw_target.startswith("//")
        or "\\" in raw_target
        or any(
            ord(character) < 32 or ord(character) == 127
            for character in raw_target
        )
    ):
        raise CTestClosureError(
            f"generated CTest target is non-canonical: {raw_target!r}"
        )
    target = PurePosixPath(raw_target)
    if target.as_posix() != raw_target:
        # PurePosixPath intentionally erases ``./``, repeated separators, dot
        # components, and trailing separators. Compare the decoded spelling
        # before using that normalized object so candidate control bytes cannot
        # acquire two textual identities for one retained member.
        raise CTestClosureError(
            f"generated CTest target is non-canonical: {raw_target!r}"
        )
    if target.is_absolute():
        if target == build_root:
            relative = PurePosixPath()
        elif target.is_relative_to(build_root):
            relative = target.relative_to(build_root)
        else:
            raise CTestClosureError(
                f"generated CTest target escapes its producer build root: {raw_target}"
            )
    else:
        relative = owner.parent / target
    if subdirectory:
        relative /= "CTestTestfile.cmake"
    normalized = PurePosixPath(relative.as_posix())
    if (
        not normalized.parts
        or normalized.as_posix() != relative.as_posix()
        or any(part in ("", ".", "..") for part in normalized.parts)
    ):
        raise CTestClosureError(
            f"generated CTest target is non-canonical: {raw_target}"
        )
    return normalized.as_posix()


def _explicit_control_include_target(
    raw_target: str,
    owner: PurePosixPath,
    build_root: PurePosixPath,
) -> str:
    """Resolve one explicit generated CTest include without module lookup.

    Args:
        raw_target: Decoded single include argument.
        owner: Relative control file containing the include.
        build_root: Exact producer build-root identity.

    Returns:
        Canonical build-relative ``.cmake`` control-file identity.

    Raises:
        CTestClosureError: The argument is a module-mode name, contains a
            backslash/control byte, lacks the explicit ``.cmake`` suffix, or
            fails the component-aware build-root boundary.

    Note:
        CMake module-mode ``include(name)`` consults ``CMAKE_MODULE_PATH`` and
        built-in modules. The pre-attestation parser deliberately models no
        search path: every include must name one declared control file by an
        explicit canonical relative or absolute path.
    """
    if PurePosixPath(raw_target).suffix != ".cmake":
        raise CTestClosureError(
            "generated CTest include is not an explicit canonical .cmake "
            f"path: {raw_target!r}"
        )
    return _control_relative_target(
        raw_target, owner, build_root, subdirectory=False
    )


def _validate_inert_generated_test_list(
    command: _CMakeCommand,
    local_test_names: Sequence[str],
    seen_variable: str | None,
    context: str,
) -> str:
    """Validate the sole inert GoogleTest discovery-list assignment.

    Args:
        command: Parsed ``set`` command.
        local_test_names: Tests registered earlier in the same generated
            control file, in exact registration order.
        seen_variable: Previously accepted list variable in this file.
        context: Stable file/line diagnostic.

    Returns:
        The unique accepted ``*_TESTS`` variable name.

    Raises:
        CTestClosureError: The assignment is duplicate, names a search/cache/
            parent-scope variable, or its values do not exactly equal the
            local registrations.

    Note:
        ``gtest_discover_tests`` emits one final ``set(<target>_TESTS ...)`` as
        inert generated bookkeeping. No other CMake assignment form is needed
        to reconstruct CTest records, so accepting one would silently model
        candidate-controlled interpreter or module-search state.
    """
    if not command.arguments:
        raise CTestClosureError(f"{context}: generated set shape differs")
    variable = command.arguments[0]
    values = command.arguments[1:]
    if any(
        value in {"CACHE", "FORCE", "PARENT_SCOPE"}
        for value in values
    ):
        # These tokens select or complete CMake's cache/parent-scope ``set``
        # signatures. Reject them even when candidate test names happen to make
        # the decoded value list equal: bytewise list equality does not make a
        # stateful CMake signature inert.
        raise CTestClosureError(
            f"{context}: alternate generated set signature is forbidden"
        )
    if (
        seen_variable is not None
        or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*_TESTS", variable) is None
        or tuple(values) != tuple(local_test_names)
    ):
        raise CTestClosureError(
            f"{context}: generated set is not the unique inert local test list"
        )
    return variable


def _control_property_value(name: str, raw: str, context: str) -> Any:
    """Convert one allowlisted generated CTest property to JSON-v1 shape."""
    if name in _BOOLEAN_PROPERTIES:
        normalized = raw.upper()
        if normalized in {"1", "ON", "TRUE", "YES"}:
            return True
        if normalized in {"0", "FALSE", "NO", "OFF"}:
            return False
        raise CTestClosureError(f"{context}: boolean property {name} is invalid")
    if name in _FLOAT_PROPERTIES:
        if re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", raw) is None:
            raise CTestClosureError(f"{context}: numeric property {name} is invalid")
        return float(raw)
    if name in _INTEGER_PROPERTIES:
        if re.fullmatch(r"-?[0-9]+", raw) is None:
            raise CTestClosureError(f"{context}: integer property {name} is invalid")
        return int(raw)
    if name in _LIST_PROPERTIES:
        values = raw.split(";")
        if any(not value for value in values):
            raise CTestClosureError(f"{context}: list property {name} is ambiguous")
        return values
    if name in _STRING_PROPERTIES:
        if not raw:
            raise CTestClosureError(f"{context}: string property {name} is empty")
        return raw
    if name in _IGNORED_CONTROL_PROPERTIES:
        return None
    raise CTestClosureError(f"{context}: unknown CTest property {name}")


def control_test_records(
    staged_build_root: Path,
    closure_value: Mapping[str, Any],
    source_root: Path,
    producer_build_root: Path,
) -> tuple[list[dict[str, Any]], dict[str, str]]:
    """Reconstruct complete CTest records from retained control bytes only.

    Args:
        staged_build_root: Private extracted targeted-role ``ci`` directory.
        closure_value: Already validated archived closure object.
        source_root: Exact producer source-root identity from the completion
            stamp; it is data and is never opened by this parser.
        producer_build_root: Exact producer build-root identity from the same
            completion stamp.

    Returns:
        Complete sorted normalized records and a path-to-SHA-256 control-byte
        mapping used to prove control/runtime roles are byte-identical.

    Raises:
        CTestClosureError: A control path/member is unsafe, graph membership is
            incomplete, syntax or command semantics exceed the allowlist,
            wrapper structure is ambiguous, or test/property coverage differs.

    Note:
        No CMake/CTest command is invoked. ``include`` and ``subdirs`` are
        resolved as pure path records, and only the exact GoogleTest
        ``if(EXISTS)/include/else/add_test(..._NOT_BUILT)/endif`` wrapper is
        accepted as conditional syntax. Includes must be explicit canonical
        ``.cmake`` paths, and ``set`` is limited to the one inert
        ``<target>_TESTS`` list that exactly repeats local registrations.
    """
    control_paths = closure_value.get("control_paths")
    if (
        not isinstance(control_paths, list)
        or not control_paths
        or control_paths != sorted(control_paths)
        or len(control_paths) != len(set(control_paths))
        or "CTestTestfile.cmake" not in control_paths
    ):
        raise CTestClosureError("archived CTest control path set is invalid")
    expected = set(control_paths)
    contents: dict[str, bytes] = {}
    digests: dict[str, str] = {}
    for relative_text in control_paths:
        relative = PurePosixPath(relative_text)
        if (
            not relative.parts
            or relative.as_posix() != relative_text
            or any(part in ("", ".", "..") for part in relative.parts)
        ):
            raise CTestClosureError(
                f"archived CTest control path is unsafe: {relative_text}"
            )
        path = staged_build_root.joinpath(*relative.parts)
        if not path.is_file() or path.is_symlink():
            raise CTestClosureError(
                f"archived CTest control file is missing or unsafe: {relative_text}"
            )
        content = path.read_bytes()
        contents[relative_text] = content
        digests[relative_text] = hashlib.sha256(content).hexdigest()

    producer_root = _root_identities(source_root, producer_build_root)[0][0]
    pending = ["CTestTestfile.cmake"]
    visited: set[str] = set()
    tests: dict[str, tuple[str, ...]] = {}
    test_directories: dict[str, PurePosixPath] = {}
    property_values: dict[str, dict[str, Any]] = {}

    def add_test(
        name: str,
        command: Sequence[str],
        context: str,
        directory: PurePosixPath,
    ) -> None:
        """Add one unique real test command to the reconstructed inventory."""
        if not name or "\0" in name or name.endswith("_NOT_BUILT"):
            raise CTestClosureError(f"{context}: invalid CTest identity {name!r}")
        if name in tests or not command:
            raise CTestClosureError(f"{context}: duplicate or empty CTest {name!r}")
        tests[name] = tuple(command)
        test_directories[name] = directory

    while pending:
        relative_text = pending.pop()
        if relative_text in visited:
            continue
        if relative_text not in expected:
            raise CTestClosureError(
                f"generated CTest graph references an undeclared file: {relative_text}"
            )
        visited.add(relative_text)
        owner = PurePosixPath(relative_text)
        commands = _parse_cmake_control(contents[relative_text], relative_text)
        control_flow = [
            command for command in commands if command.name in {"else", "endif", "if"}
        ]
        if control_flow:
            if (
                len(commands) != 5
                or commands[0].name != "if"
                or len(commands[0].arguments) != 2
                or commands[0].arguments[0] != "EXISTS"
                or commands[1].name != "include"
                or len(commands[1].arguments) != 1
                or commands[2] != _CMakeCommand("else", (), commands[2].line)
                or commands[3].name != "add_test"
                or len(commands[3].arguments) != 2
                or commands[4] != _CMakeCommand("endif", (), commands[4].line)
            ):
                raise CTestClosureError(
                    f"conditional CTest control is not an exact NOT_BUILT wrapper: {relative_text}"
                )
            first_target = _explicit_control_include_target(
                commands[0].arguments[1], owner, producer_root
            )
            include_target = _explicit_control_include_target(
                commands[1].arguments[0], owner, producer_root
            )
            fallback_name, fallback_command = commands[3].arguments
            if (
                first_target != include_target
                or include_target not in expected
                or not fallback_name.endswith("_NOT_BUILT")
                or fallback_command != fallback_name
            ):
                raise CTestClosureError(
                    f"NOT_BUILT wrapper identity differs: {relative_text}"
                )
            pending.append(include_target)
            continue

        local_test_names: list[str] = []
        inert_test_list: str | None = None
        for command in commands:
            context = f"{relative_text}:{command.line}"
            if command.name == "include":
                if len(command.arguments) != 1:
                    raise CTestClosureError(f"{context}: include shape differs")
                pending.append(
                    _explicit_control_include_target(
                        command.arguments[0], owner, producer_root
                    )
                )
            elif command.name == "subdirs":
                if len(command.arguments) != 1:
                    raise CTestClosureError(f"{context}: subdirs shape differs")
                pending.append(
                    _control_relative_target(
                        command.arguments[0], owner, producer_root, subdirectory=True
                    )
                )
            elif command.name == "add_test":
                if len(command.arguments) < 2 or command.arguments[0] == "NAME":
                    raise CTestClosureError(f"{context}: add_test shape differs")
                add_test(
                    command.arguments[0],
                    command.arguments[1:],
                    context,
                    owner.parent,
                )
                local_test_names.append(command.arguments[0])
            elif command.name == "set_tests_properties":
                try:
                    property_index = command.arguments.index("PROPERTIES")
                except ValueError as error:
                    raise CTestClosureError(
                        f"{context}: set_tests_properties lacks PROPERTIES"
                    ) from error
                names = command.arguments[:property_index]
                properties = command.arguments[property_index + 1 :]
                if (
                    not names
                    or not properties
                    or len(properties) % 2 != 0
                ):
                    raise CTestClosureError(
                        f"{context}: set_tests_properties shape differs"
                    )
                for name in names:
                    destination = property_values.setdefault(name, {})
                    for index in range(0, len(properties), 2):
                        property_name = properties[index]
                        if property_name in destination:
                            raise CTestClosureError(
                                f"{context}: duplicate property {property_name} for {name}"
                            )
                        converted = _control_property_value(
                            property_name, properties[index + 1], context
                        )
                        if property_name not in _IGNORED_CONTROL_PROPERTIES:
                            destination[property_name] = converted
            elif command.name == "set":
                inert_test_list = _validate_inert_generated_test_list(
                    command,
                    local_test_names,
                    inert_test_list,
                    context,
                )
            else:
                raise CTestClosureError(
                    f"{context}: unknown or side-effect-capable CTest command {command.name}"
                )
        if inert_test_list is not None:
            test_list_command = next(
                command for command in commands if command.name == "set"
            )
            if tuple(test_list_command.arguments[1:]) != tuple(local_test_names):
                raise CTestClosureError(
                    f"{relative_text}:{test_list_command.line}: generated set "
                    "does not equal the final local test list"
                )

    if visited != expected:
        raise CTestClosureError(
            "archived CTest control set contains unreachable or missing members"
        )
    if not tests:
        raise CTestClosureError("archived CTest control graph has no built tests")
    if set(property_values) - set(tests):
        raise CTestClosureError(
            "CTest properties name an undeclared or NOT_BUILT test"
        )
    records: list[dict[str, Any]] = []
    for name in sorted(tests):
        properties = dict(sorted(property_values.get(name, {}).items()))
        working = properties.get("WORKING_DIRECTORY")
        if working is None:
            command_working = producer_root / test_directories[name]
            # CTest JSON-v1 materializes the effective default as an absolute
            # property even though generated CTest control omits it.
            properties["WORKING_DIRECTORY"] = command_working.as_posix()
            properties = dict(sorted(properties.items()))
        else:
            if not isinstance(working, str) or not working:
                raise CTestClosureError(
                    f"CTest working directory for {name!r} is malformed"
                )
            working_path = PurePosixPath(working)
            command_working = (
                working_path
                if working_path.is_absolute()
                else producer_root / test_directories[name] / working_path
            )
        labels = _labels(name, properties)
        disabled = properties.get("DISABLED", False)
        if not isinstance(disabled, bool):
            raise CTestClosureError(f"CTest DISABLED for {name!r} is not boolean")
        command = list(tests[name])
        command_zero = PurePosixPath(command[0])
        if (
            not command_zero.is_absolute()
            and (command[0].startswith(".") or "/" in command[0])
        ):
            if not command_working.is_absolute():
                raise CTestClosureError(
                    f"CTest working directory for {name!r} cannot resolve command zero"
                )
            command[0] = (command_working / command_zero).as_posix()
        records.append(
            {
                "command": _normalize_command(
                    command, source_root, producer_build_root, name
                ),
                "disabled": disabled,
                "labels": labels,
                "name": name,
                "properties": {
                    property_name: _normalize_property_value(
                        property_name,
                        property_value,
                        source_root,
                        producer_build_root,
                        name,
                    )
                    for property_name, property_value in properties.items()
                },
            }
        )
    return records, dict(sorted(digests.items()))


def _effective_working_directory(
    test_name: str,
    properties: Mapping[str, Any],
    source_root: Path,
    build_root: Path,
) -> Path:
    """Resolve one CTest working directory without selecting its whole tree.

    Args:
        test_name: Exact CTest identity used in diagnostics.
        properties: Strict property mapping from the raw JSON-v1 record.
        source_root: Exact candidate checkout root.
        build_root: Exact producer or restored build root.

    Returns:
        A real directory inside the build or source root. An absent property
        uses the top-level build root, while a relative property is interpreted
        from that same root. CTest normally serializes the effective default as
        an absolute property; the fallback keeps hand-written and older
        inventories deterministic.

    Raises:
        CTestClosureError: The property is malformed, missing, linked, or
            outside both maintained roots.

    Note:
        The returned directory is a path-resolution base only. Callers add only
        independently referenced files or bounded path-list directories.
    """
    raw = properties.get("WORKING_DIRECTORY")
    if raw is None:
        working = build_root
    else:
        if not isinstance(raw, str) or not raw or "\0" in raw:
            raise CTestClosureError(
                f"CTest WORKING_DIRECTORY for {test_name!r} is malformed"
            )
        working = Path(raw)
        if not working.is_absolute():
            working = build_root / working
    if working.is_symlink() or not working.is_dir():
        raise CTestClosureError(
            f"CTest WORKING_DIRECTORY for {test_name!r} is missing or linked: "
            f"{working}"
        )
    resolved = working.resolve()
    for root in (build_root, source_root):
        try:
            resolved.relative_to(root.resolve())
            return resolved
        except ValueError:
            continue
    raise CTestClosureError(
        f"CTest WORKING_DIRECTORY for {test_name!r} escapes maintained roots: "
        f"{working}"
    )


def _path_value_fragments(value: str, *, split_path_list: bool = False) -> list[str]:
    """Split one command/property value into conservative path candidates.

    Args:
        value: One decoded CTest command argument or property value.
        split_path_list: Whether the platform path-list separator is also a
            delimiter after CMake-list splitting.

    Returns:
        Stable unique nonempty fragments. A command-line ``KEY=value`` or
        ``--option=value`` contributes only its value in addition to the
        original spelling, so an option cannot hide a referenced file.

    Raises:
        CTestClosureError: The value contains NUL.

    Note:
        Existence and maintained-root confinement are checked separately. This
        parser never treats ``WORKING_DIRECTORY`` or an exact maintained root
        locator argument as a payload request.
    """
    if "\0" in value:
        raise CTestClosureError("CTest path-bearing value contains NUL")
    pending = [value]
    if "=" in value:
        pending.append(value.split("=", 1)[1])
    fragments: list[str] = []
    for candidate in pending:
        for cmake_item in candidate.split(";"):
            path_items = (
                cmake_item.split(os.pathsep) if split_path_list else [cmake_item]
            )
            for item in path_items:
                item = item.strip()
                if item and item not in fragments:
                    fragments.append(item)
    return fragments


def _record_path_candidate(
    value: str,
    working_directory: Path,
    source_root: Path,
    build_root: Path,
    runtime: set[str],
    source_files: set[str],
    *,
    require_runtime: bool,
    allow_bare_relative: bool,
    context: str,
) -> None:
    """Classify and record one actual CTest file or bounded directory path.

    Args:
        value: Absolute or working-directory-relative candidate spelling.
        working_directory: Effective base of the owning CTest.
        source_root: Exact candidate checkout root.
        build_root: Exact producer or restored build root.
        runtime: Mutable build-relative payload member set.
        source_files: Mutable source-relative hash/size input set.
        require_runtime: Whether build members must be physically collected.
        allow_bare_relative: Whether a separator-free relative value may name
            data. Command zero sets this false so ``python3`` and other bare
            PATH commands are not confused with build products; an explicit
            ``./tool`` or ``subdir/tool`` remains a build-file reference.
        context: Stable diagnostic label.

    Returns:
        None. Nonexistent candidates and external absolute tools/data are not
        dependencies of the role; existing relative paths that escape both
        maintained roots fail closed.

    Raises:
        CTestClosureError: A relative path escapes, or a selected in-root path
            is linked, special, too broad, or contains forbidden residue.
    """
    if not value or value.startswith(("$<", "${")):
        return
    candidate = Path(value)
    relative = not candidate.is_absolute()
    if relative:
        if (
            not allow_bare_relative
            and len(candidate.parts) == 1
            and not value.startswith(".")
        ):
            return
        candidate = working_directory / candidate
    if not candidate.exists() and not candidate.is_symlink():
        return
    candidate_resolved = candidate.resolve()
    if not candidate.is_symlink() and candidate_resolved in {
        source_root.resolve(),
        build_root.resolve(),
    }:
        # Several maintained tests pass ``--repo <source_root>`` or
        # ``--build-dir <build_root>`` as a lookup base. The root itself is not
        # payload; separately referenced descendants remain collected by their
        # own command/property arguments.
        return
    try:
        candidate_resolved.relative_to(build_root.resolve())
    except (OSError, ValueError):
        try:
            candidate_resolved.relative_to(source_root.resolve())
        except (OSError, ValueError) as error:
            if relative:
                raise CTestClosureError(
                    f"{context} escapes maintained roots: {candidate}"
                ) from error
            return
        if candidate.is_file():
            source_files.add(
                _relative_regular(candidate, source_root, context).as_posix()
            )
        elif candidate.is_symlink() or candidate.exists():
            raise CTestClosureError(
                f"{context} selects a non-file source input: {candidate}"
            )
        return
    if require_runtime:
        _add_tree_files(candidate, build_root, runtime, context)


def _environment_modification_values(value: Any) -> Iterable[tuple[str, bool]]:
    """Yield path-bearing CTest environment-modification values.

    Args:
        value: Raw ``ENVIRONMENT_MODIFICATION`` JSON property value.

    Yields:
        ``(value, split_path_list)`` pairs. CMake-list operations use semicolon
        splitting in the common parser; platform path-list operations also use
        ``os.pathsep``. String operations are considered only when their value
        resolves to an actual maintained path.

    Raises:
        CTestClosureError: An entry has no ``variable=operation:value`` shape or
            names an unsupported operation.
    """
    supported = {
        "cmake_list_append": False,
        "cmake_list_prepend": False,
        "path_list_append": True,
        "path_list_prepend": True,
        "set": False,
        "string_append": False,
        "string_prepend": False,
    }
    ignored = {"reset", "unset"}
    for entry in _walk_json_strings(value):
        if "=" not in entry:
            raise CTestClosureError(
                "CTest ENVIRONMENT_MODIFICATION entry has no variable assignment"
            )
        _, modification = entry.split("=", 1)
        if ":" not in modification:
            raise CTestClosureError(
                "CTest ENVIRONMENT_MODIFICATION entry has no operation value"
            )
        operation, path_value = modification.split(":", 1)
        if operation in ignored:
            if path_value:
                raise CTestClosureError(
                    f"CTest {operation} environment modification has a value"
                )
            continue
        if operation not in supported:
            raise CTestClosureError(
                f"unsupported CTest environment modification: {operation!r}"
            )
        yield path_value, supported[operation]


def _walk_json_strings(value: Any) -> Iterable[str]:
    """Yield every string recursively contained in one decoded JSON value."""
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for item in value:
            yield from _walk_json_strings(item)
    elif isinstance(value, dict):
        for item in value.values():
            yield from _walk_json_strings(item)


def _add_tree_files(
    path: Path,
    root: Path,
    destination: set[str],
    context: str,
) -> None:
    """Add one regular file or bounded directory tree to a closure set."""
    if path.is_symlink():
        raise CTestClosureError(f"{context} is a symlink: {path}")
    if path.is_file():
        relative = _relative_regular(path, root, context)
        if relative.name.endswith(_FORBIDDEN_RUNTIME_SUFFIXES):
            raise CTestClosureError(f"{context} selects forbidden residue: {relative}")
        destination.add(relative.as_posix())
        return
    if not path.is_dir() or path.resolve() == root.resolve():
        raise CTestClosureError(f"{context} is missing or too broad: {path}")
    for child in sorted(path.rglob("*")):
        if child.is_symlink():
            raise CTestClosureError(f"{context} contains a symlink: {child}")
        if child.is_dir():
            continue
        if not child.is_file():
            raise CTestClosureError(f"{context} contains a special file: {child}")
        relative = _relative_regular(child, root, context)
        if relative.name.endswith(_FORBIDDEN_RUNTIME_SUFFIXES):
            raise CTestClosureError(f"{context} contains forbidden residue: {relative}")
        destination.add(relative.as_posix())


def _runtime_and_source_inputs(
    payload: Any,
    ordinary_tests: Sequence[Mapping[str, Any]],
    source_root: Path,
    build_root: Path,
    *,
    require_runtime: bool,
) -> tuple[list[str], list[dict[str, Any]]]:
    """Collect property-aware build runtime and source-checkout inputs.

    Each ordinary test owns one effective working directory. Relative
    executables with an explicit path component, command arguments,
    ``REQUIRED_FILES``, environment values, and environment modifications are
    resolved from that base. Bare command zero remains a PATH command. Missing
    consumer files disappear from the independently recomputed closure and
    therefore differ from the producer manifest before execution.
    """
    runtime: set[str] = set()
    source_files: set[str] = set()
    raw_tests = payload["tests"]
    ordinary_names = {record["name"] for record in ordinary_tests}
    for item in raw_tests:
        test_name = item.get("name")
        if test_name not in ordinary_names:
            continue
        properties = _properties(test_name, item.get("properties"))
        working = _effective_working_directory(
            test_name, properties, source_root, build_root
        )
        command = item.get("command")
        if not isinstance(command, list) or not command:
            raise CTestClosureError(
                f"ordinary CTest {test_name!r} has no valid command"
            )
        for index, argument in enumerate(command):
            for candidate in _path_value_fragments(argument):
                _record_path_candidate(
                    candidate,
                    working,
                    source_root,
                    build_root,
                    runtime,
                    source_files,
                    require_runtime=require_runtime,
                    allow_bare_relative=index != 0,
                    context=f"CTest {test_name!r} command input",
                )

        for required in _walk_json_strings(properties.get("REQUIRED_FILES", [])):
            for candidate in _path_value_fragments(required):
                _record_path_candidate(
                    candidate,
                    working,
                    source_root,
                    build_root,
                    runtime,
                    source_files,
                    require_runtime=require_runtime,
                    allow_bare_relative=True,
                    context=f"CTest {test_name!r} required file",
                )

        for assignment in _walk_json_strings(properties.get("ENVIRONMENT", [])):
            if "=" not in assignment:
                raise CTestClosureError(
                    f"CTest ENVIRONMENT for {test_name!r} has no assignment"
                )
            _, environment_value = assignment.split("=", 1)
            for candidate in _path_value_fragments(
                environment_value, split_path_list=True
            ):
                _record_path_candidate(
                    candidate,
                    working,
                    source_root,
                    build_root,
                    runtime,
                    source_files,
                    require_runtime=require_runtime,
                    allow_bare_relative=True,
                    context=f"CTest {test_name!r} environment input",
                )

        for modification_value, split_path_list in _environment_modification_values(
            properties.get("ENVIRONMENT_MODIFICATION", [])
        ):
            for candidate in _path_value_fragments(
                modification_value, split_path_list=split_path_list
            ):
                _record_path_candidate(
                    candidate,
                    working,
                    source_root,
                    build_root,
                    runtime,
                    source_files,
                    require_runtime=require_runtime,
                    allow_bare_relative=True,
                    context=f"CTest {test_name!r} environment modification input",
                )

    if require_runtime:
        for path in sorted(build_root.rglob("*")):
            lexical = PurePosixPath(path.relative_to(build_root).as_posix())
            shared_library = (
                bool(lexical.parts)
                and lexical.parts[0] in _SHARED_LIBRARY_ROOTS
                and _is_shared_library_name(lexical.name)
            )
            runtime_root = bool(lexical.parts) and lexical.parts[0] in _RUNTIME_ROOTS
            trust_material = lexical.parts[:2] == ("generated", "plugin_trust")
            if path.is_symlink():
                if not shared_library:
                    if runtime_root or trust_material:
                        raise CTestClosureError(
                            f"runtime closure contains a non-library link: {lexical}"
                        )
                    continue
                try:
                    resolved = path.resolve(strict=True)
                    target = _relative_regular(
                        resolved, build_root, "runtime library alias target"
                    )
                except (OSError, CTestClosureError) as error:
                    raise CTestClosureError(
                        f"runtime library alias is dangling or escapes: {lexical}"
                    ) from error
                runtime.add(lexical.as_posix())
                runtime.add(target.as_posix())
                continue
            if path.is_dir():
                continue
            if not path.is_file():
                if runtime_root or trust_material or shared_library:
                    raise CTestClosureError(
                        f"runtime closure contains a special entry: {lexical}"
                    )
                continue
            relative = _relative_regular(path, build_root, "runtime candidate")
            if shared_library or runtime_root or trust_material:
                if relative.name.endswith(_FORBIDDEN_RUNTIME_SUFFIXES):
                    raise CTestClosureError(
                        f"runtime closure selects forbidden residue: {relative}"
                    )
                runtime.add(relative.as_posix())
        if not runtime:
            raise CTestClosureError("ordinary CTest runtime closure is empty")

    source_inputs = [
        {
            "path": path,
            "sha256": _sha256_file(source_root / path),
            "size": (source_root / path).stat().st_size,
        }
        for path in sorted(source_files)
    ]
    return sorted(runtime), source_inputs


def create_closure(
    source_root: Path,
    build_root: Path,
    inventory_path: Path,
    config: str,
    *,
    require_runtime: bool = True,
) -> dict[str, Any]:
    """Create one canonical ordinary CTest closure value from real state.

    Args:
        source_root: Exact checked-out candidate repository.
        build_root: Complete post-build producer or restored runtime tree.
        inventory_path: Complete CTest JSON inventory generated at this tree.
        config: Exact CTest configuration name.
        require_runtime: Whether referenced runtime files must be present.

    Returns:
        Canonical closure object ready for serialization or comparison.

    Raises:
        CTestClosureError: Inventory, control graph, runtime, source, or path
            state is malformed, incomplete, linked, special, or too broad.
    """
    if not source_root.is_dir() or source_root.is_symlink():
        raise CTestClosureError("source root must be a real directory")
    if not build_root.is_dir() or build_root.is_symlink():
        raise CTestClosureError("build root must be a real directory")
    if not config or "\0" in config:
        raise CTestClosureError("CTest configuration identity is empty")
    payload = _load_json(inventory_path)
    ordinary = _ordinary_tests(payload, source_root, build_root)
    runtime, source_inputs = _runtime_and_source_inputs(
        payload,
        ordinary,
        source_root,
        build_root,
        require_runtime=require_runtime,
    )
    return {
        "config": config,
        "control_paths": _control_paths(build_root),
        "ordinary_tests": ordinary,
        "runtime_paths": runtime,
        "schema": "photospider-ordinary-ctest-closure-v1",
        "source_inputs": source_inputs,
    }


def write_closure(
    source_root: Path,
    build_root: Path,
    inventory_path: Path,
    output: Path,
    config: str,
) -> None:
    """Atomically write the complete post-build ordinary CTest closure."""
    value = create_closure(source_root, build_root, inventory_path, config)
    if output.is_symlink():
        raise CTestClosureError(f"refusing symlink closure output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + f".tmp.{os.getpid()}")
    temporary.write_bytes(_canonical_bytes(value))
    temporary.replace(output)


def load_closure(path: Path) -> dict[str, Any]:
    """Load and validate one canonical closure manifest structure."""
    value = _load_json(path)
    expected = {
        "config",
        "control_paths",
        "ordinary_tests",
        "runtime_paths",
        "schema",
        "source_inputs",
    }
    if not isinstance(value, dict) or set(value) != expected:
        raise CTestClosureError("ordinary CTest closure fields differ")
    if value["schema"] != "photospider-ordinary-ctest-closure-v1":
        raise CTestClosureError("unknown ordinary CTest closure schema")
    if path.read_bytes() != _canonical_bytes(value):
        raise CTestClosureError("ordinary CTest closure is not canonical JSON")
    if not isinstance(value["config"], str) or not value["config"]:
        raise CTestClosureError("ordinary CTest closure config is invalid")
    for field in ("control_paths", "runtime_paths"):
        entries = value[field]
        if (
            not isinstance(entries, list)
            or not entries
            or not all(isinstance(item, str) and item for item in entries)
            or entries != sorted(entries)
            or len(entries) != len(set(entries))
        ):
            raise CTestClosureError(f"ordinary CTest {field} is invalid")
        for item in entries:
            pure = PurePosixPath(item)
            if (
                item.startswith("/")
                or "\\" in item
                or pure.as_posix() != item
                or any(part in ("", ".", "..") for part in pure.parts)
            ):
                raise CTestClosureError(f"ordinary CTest {field} path is unsafe")
    tests = value["ordinary_tests"]
    if (
        not isinstance(tests, list)
        or not tests
        or not all(
            isinstance(item, dict)
            and set(item) == {"command", "disabled", "labels", "name", "properties"}
            and isinstance(item["name"], str)
            and item["name"]
            and isinstance(item["command"], list)
            and item["command"]
            and all(isinstance(argument, str) for argument in item["command"])
            and isinstance(item["disabled"], bool)
            and isinstance(item["labels"], list)
            and all(isinstance(label, str) and label for label in item["labels"])
            and isinstance(item["properties"], dict)
            for item in tests
        )
        or [item["name"] for item in tests] != sorted(item["name"] for item in tests)
        or len({item["name"] for item in tests}) != len(tests)
    ):
        raise CTestClosureError("ordinary CTest test inventory is invalid")
    source_inputs = value["source_inputs"]
    if not isinstance(source_inputs, list) or not all(
        isinstance(item, dict)
        and set(item) == {"path", "sha256", "size"}
        and isinstance(item["path"], str)
        and item["path"]
        and isinstance(item["sha256"], str)
        and re.fullmatch(r"[0-9a-f]{64}", item["sha256"])
        and isinstance(item["size"], int)
        and not isinstance(item["size"], bool)
        and item["size"] >= 0
        for item in source_inputs
    ):
        raise CTestClosureError("ordinary CTest source input inventory is invalid")
    source_paths = [item["path"] for item in source_inputs]
    if source_paths != sorted(source_paths) or len(source_paths) != len(
        set(source_paths)
    ):
        raise CTestClosureError("ordinary CTest source inputs are not sorted")
    for source_path in source_paths:
        pure = PurePosixPath(source_path)
        if (
            source_path.startswith("/")
            or "\\" in source_path
            or pure.as_posix() != source_path
            or any(part in ("", ".", "..") for part in pure.parts)
        ):
            raise CTestClosureError("ordinary CTest source input path is unsafe")
    return value


def expected_role_paths(build_root: Path, role: str) -> list[str]:
    """Return the exact payload members allowed for one CTest artifact role."""
    if role not in {"ctest-control", "ctest-runtime"}:
        raise CTestClosureError(f"unsupported CTest artifact role: {role}")
    closure = load_closure(build_root.joinpath(*CLOSURE_RELATIVE_PATH.parts))
    selected = {
        ".photospider-ci-build-complete",
        "CMakeCache.txt",
        *closure["control_paths"],
    }
    inventory_root = build_root / "generated/ci_inventory"
    if not inventory_root.is_dir() or inventory_root.is_symlink():
        raise CTestClosureError("generated CI inventory is unavailable")
    for path in sorted(inventory_root.rglob("*")):
        if path.is_symlink() or (not path.is_dir() and not path.is_file()):
            raise CTestClosureError(f"generated CI inventory entry is unsafe: {path}")
        if path.is_file():
            selected.add(
                _relative_regular(
                    path, build_root, "generated CI inventory file"
                ).as_posix()
            )
    if role == "ctest-runtime":
        selected.update(closure["runtime_paths"])
    return sorted(selected)


def validate_staged_role(
    source_root: Path,
    build_root: Path,
    role: str,
    content_paths: Sequence[str],
) -> None:
    """Validate extracted role members and candidate source inputs exactly."""
    expected = expected_role_paths(build_root, role)
    if list(content_paths) != expected:
        raise CTestClosureError(
            f"{role} members differ from the ordinary CTest closure"
        )
    closure = load_closure(build_root.joinpath(*CLOSURE_RELATIVE_PATH.parts))
    for item in closure["source_inputs"]:
        if not isinstance(item, dict) or set(item) != {"path", "sha256", "size"}:
            raise CTestClosureError("ordinary CTest source input fields differ")
        path = source_root / item["path"]
        if path.stat().st_size != item["size"] or _sha256_file(path) != item["sha256"]:
            raise CTestClosureError(
                f"ordinary CTest source input differs: {item['path']}"
            )


def query_inventory(build_root: Path, ctest_executable: str, config: str) -> bytes:
    """Run complete CTest JSON discovery at one restored canonical build root."""
    command = [
        ctest_executable,
        "--test-dir",
        str(build_root),
        "--show-only=json-v1",
        "-C",
        config,
    ]
    completed = subprocess.run(command, check=False, capture_output=True)
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode("utf-8", errors="replace").strip()
        raise CTestClosureError(
            "restored CTest inventory failed"
            + (f": {diagnostic}" if diagnostic else "")
        )
    return completed.stdout


def verify_restored_runtime(
    source_root: Path,
    build_root: Path,
    ctest_executable: str = "ctest",
) -> None:
    """Re-discover and compare the complete restored ordinary runtime closure."""
    closure_path = build_root.joinpath(*CLOSURE_RELATIVE_PATH.parts)
    expected = load_closure(closure_path)
    inventory_path = build_root / "generated/ci_inventory/restored_ctest_inventory.json"
    inventory_path.write_bytes(
        query_inventory(build_root, ctest_executable, expected["config"])
    )
    try:
        actual = create_closure(
            source_root,
            build_root,
            inventory_path,
            expected["config"],
            require_runtime=True,
        )
    finally:
        inventory_path.unlink(missing_ok=True)
    if actual != expected:
        raise CTestClosureError(
            "restored ordinary CTest inventory or runtime closure differs"
        )


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """Build and parse the producer/restored-consumer command interface."""
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create")
    create.add_argument("--source-root", type=Path, required=True)
    create.add_argument("--build-root", type=Path, required=True)
    create.add_argument("--inventory", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--config", required=True)
    create.set_defaults(handler="create")
    verify = subparsers.add_parser("verify-restored-runtime")
    verify.add_argument("--source-root", type=Path, required=True)
    verify.add_argument("--build-root", type=Path, required=True)
    verify.add_argument("--ctest-executable", default="ctest")
    verify.set_defaults(handler="verify")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """Execute one closure operation and convert contract errors to status one."""
    arguments = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if arguments.handler == "create":
            write_closure(
                arguments.source_root.resolve(),
                arguments.build_root.resolve(),
                arguments.inventory.resolve(),
                arguments.output.resolve(),
                arguments.config,
            )
            print("ordinary CTest runtime closure created")
        else:
            verify_restored_runtime(
                arguments.source_root.resolve(),
                arguments.build_root.resolve(),
                arguments.ctest_executable,
            )
            print("restored ordinary CTest runtime closure verified")
    except (CTestClosureError, OSError, UnicodeError) as error:
        print(f"ordinary CTest closure failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
