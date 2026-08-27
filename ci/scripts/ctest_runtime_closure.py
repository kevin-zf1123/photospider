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

The consumer re-runs CTest discovery only after the runtime role has been
restored at its canonical build path.  A ``*_NOT_BUILT`` placeholder, changed
ordinary inventory, missing include, executable, data, shared library, plugin,
or trust input fails before ordinary CTest can execute.

The pre-attestation protected verifier also parses retained raw and restored
JSON through this module and compares their exact ordinary-test names with both
archived closures. It excludes only the exact ``build-smoke`` label and never
executes a test command while establishing that cross-job coverage boundary.
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
from typing import Any, Iterable, Mapping, Sequence


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


class CTestClosureError(ValueError):
    """Report malformed inventory, unsafe paths, or an incomplete closure."""


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


def _normalize_string(value: str, source_root: Path, build_root: Path) -> str:
    """Replace maintained absolute roots with stable closure tokens."""
    normalized = value.replace(str(build_root.resolve()), "${BUILD_ROOT}")
    return normalized.replace(str(source_root.resolve()), "${SOURCE_ROOT}")


def _normalize_value(value: Any, source_root: Path, build_root: Path) -> Any:
    """Recursively normalize strings while preserving JSON scalar structure."""
    if isinstance(value, str):
        return _normalize_string(value, source_root, build_root)
    if isinstance(value, list):
        return [_normalize_value(item, source_root, build_root) for item in value]
    if isinstance(value, dict):
        return {
            key: _normalize_value(item, source_root, build_root)
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


def _ordinary_tests(
    payload: Any, source_root: Path, build_root: Path
) -> list[dict[str, Any]]:
    """Normalize every non-build-smoke test from one complete CTest payload."""
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
        if BUILD_SMOKE_LABEL in labels:
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
                "command": _normalize_value(command, source_root, build_root),
                "disabled": disabled,
                "labels": labels,
                "name": name,
                "properties": _normalize_value(properties, source_root, build_root),
            }
        )
    if not records:
        raise CTestClosureError("ordinary CTest inventory is empty")
    records.sort(key=lambda record: record["name"])
    return records


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
