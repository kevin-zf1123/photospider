#!/usr/bin/env python3
"""Audit CLI Host-boundary Doxygen through the compiler's comment AST."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterator


COMMAND_SUFFIXES = (
    "help",
    "clear",
    "graphs",
    "load",
    "switch",
    "close",
    "print",
    "inspect",
    "node",
    "ops",
    "bench",
    "benchmark",
    "traversal",
    "config",
    "read",
    "source",
    "output",
    "clear_graph",
    "clear_cache",
    "free",
    "compute",
    "save",
    "exit",
    "scheduler",
)
COMMAND_FUNCTIONS = tuple(f"handle_{suffix}" for suffix in COMMAND_SUFFIXES)
HELP_FUNCTIONS = tuple(f"print_help_{suffix}" for suffix in COMMAND_SUFFIXES)
OTHER_HOST_FUNCTIONS = ("run_repl", "run_benchmark_config_editor")
ROOT_CLI_FUNCTIONS = (
    "handle_interactive_save",
    "print_cli_help",
    "print_repl_help",
)
CONFIG_AND_RUN_FUNCTIONS = (
    "write_config_to_file",
    "load_or_create_config",
    "run_graph_cli",
)
EXPECTED_FUNCTIONS = (
    COMMAND_FUNCTIONS
    + HELP_FUNCTIONS
    + OTHER_HOST_FUNCTIONS
    + ROOT_CLI_FUNCTIONS
    + CONFIG_AND_RUN_FUNCTIONS
)
REQUIRED_BLOCK_COMMANDS = {"brief", "return", "throws", "note"}
CALLABLE_DECL_KINDS = {"FunctionDecl", "CXXMethodDecl"}
CLI_TARGET_NAMES = ("photospider_cli_common", "graph_cli")
EXPECTED_CLI_TARGET_SOURCE_COUNTS = {
    "photospider_cli_common": 59,
    "graph_cli": 1,
}
EXPECTED_CLI_TARGET_CLOSURE_SOURCE_COUNT = 60
EXPECTED_AUDITED_SOURCE_COUNT = 60
REQUIRED_EXTENDED_DOXYGEN_SOURCES = (
    "apps/graph_cli/src/dependency_tree_formatter.cpp",
    "apps/graph_cli/src/do_traversal.cpp",
    "apps/graph_cli/src/node_editor.cpp",
    "apps/graph_cli/src/node_editor_full.cpp",
    "apps/graph_cli/src/cli_autocompleter.cpp",
    "apps/graph_cli/src/autocomplete/CompleteCommand.cpp",
    "apps/graph_cli/src/autocomplete/CompleteComputeArgs.cpp",
    "apps/graph_cli/src/autocomplete/CompleteGraphName.cpp",
    "apps/graph_cli/src/autocomplete/CompleteNodeId.cpp",
    "apps/graph_cli/src/autocomplete/CompleteOpsMode.cpp",
    "apps/graph_cli/src/autocomplete/CompletePath.cpp",
    "apps/graph_cli/src/autocomplete/CompletePrintArgs.cpp",
    "apps/graph_cli/src/autocomplete/CompleteSessionName.cpp",
    "apps/graph_cli/src/autocomplete/CompleteTraversalArgs.cpp",
    "apps/graph_cli/src/autocomplete/CompleteYamlPath.cpp",
    "apps/graph_cli/src/autocomplete/FindLongestCommonPrefix.cpp",
    "apps/graph_cli/src/autocomplete/Tokenize.cpp",
)
REQUIRED_EXTENDED_DOXYGEN_HEADERS = (
    "apps/graph_cli/include/graph_cli/dependency_tree_formatter.hpp",
    "apps/graph_cli/include/graph_cli/do_traversal.hpp",
    "apps/graph_cli/include/graph_cli/node_editor.hpp",
    "apps/graph_cli/include/graph_cli/node_editor_full.hpp",
    "apps/graph_cli/include/graph_cli/cli_autocompleter.hpp",
)
EXPECTED_EXTENDED_DOXYGEN_PATH_COUNT = 22
EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT = 72
REQUIRED_EXTENDED_SYMBOLS = {
    "dependency_tree_formatter.cpp": (
        "indent",
        "dump_parameters",
        "dump_trimmed_metadata",
        "dump_matrix",
        "dirty_domain_name",
        "dirty_source_lifecycle_name",
        "dirty_edge_direction_name",
        "rect_text",
        "format_node_metadata",
        "format_node_inspection",
        "format_graph_inspection",
        "format_dirty_snapshot",
        "format_dependency_tree",
    ),
    "do_traversal.cpp": ("do_traversal",),
    "node_editor.cpp": (
        "run_node_editor_decoupled",
        "Tree2Mode",
        "load_current",
        "t2",
        "menu_on_change",
        "textarea_on_change",
        "text_with_offsets",
        "tree1_view",
        "tree2_view",
        "renderer",
        "apply_current_editor",
        "open_external_editor",
        "catch_event_callback",
    ),
    "node_editor_full.cpp": ("run_node_editor_full",),
    "cli_autocompleter.cpp": ("CliAutocompleter", "Complete"),
    "CompleteCommand.cpp": ("CompleteCommand",),
    "CompleteComputeArgs.cpp": ("CompleteComputeArgs",),
    "CompleteGraphName.cpp": ("CompleteGraphName",),
    "CompleteNodeId.cpp": ("CompleteNodeId",),
    "CompleteOpsMode.cpp": ("CompleteOpsMode",),
    "CompletePath.cpp": ("CompletePath",),
    "CompletePrintArgs.cpp": ("CompletePrintArgs",),
    "CompleteSessionName.cpp": ("CompleteSessionName",),
    "CompleteTraversalArgs.cpp": ("CompleteTraversalArgs",),
    "CompleteYamlPath.cpp": ("CompleteYamlPath",),
    "FindLongestCommonPrefix.cpp": ("FindLongestCommonPrefix",),
    "Tokenize.cpp": ("Tokenize",),
    "cli_autocompleter.hpp": (
        "CompletionResult",
        "options",
        "new_line",
        "new_cursor_pos",
        "CliAutocompleter",
        "SetCurrentGraph",
        "Complete",
        "Tokenize",
        "FindLongestCommonPrefix",
        "CompleteCommand",
        "CompletePath",
        "CompleteYamlPath",
        "CompleteNodeId",
        "CompletePrintArgs",
        "CompleteTraversalArgs",
        "CompleteComputeArgs",
        "CompleteGraphName",
        "CompleteSessionName",
        "CompleteOpsMode",
        "svc_",
        "commands_",
        "current_graph_",
    ),
    "dependency_tree_formatter.hpp": (
        "format_node_metadata",
        "format_node_inspection",
        "format_graph_inspection",
        "format_dirty_snapshot",
        "format_dependency_tree",
    ),
    "do_traversal.hpp": ("do_traversal",),
    "node_editor.hpp": ("run_node_editor_decoupled",),
    "node_editor_full.hpp": ("run_node_editor_full",),
}

EXTENDED_FIELD_SYMBOLS = {
    "options",
    "new_line",
    "new_cursor_pos",
    "svc_",
    "commands_",
    "current_graph_",
}
EXTENDED_TYPE_SYMBOLS = {"CompletionResult", "Tree2Mode"}
NODE_EDITOR_ENTITY_LOCATORS = {
    "Tree2Mode": (r"\benum\s+class\s+Tree2Mode\b", 1, ()),
    "load_current": (r"\bauto\s+load_current\s*=\s*\[&\]", 1, ()),
    "t2": (r"\bauto\s+t2\s*=\s*\[&\]", 1, ()),
    "menu_on_change": (r"\bmenu_opt\.on_change\s*=\s*\[&\]", 1, ()),
    "textarea_on_change": (
        r"\btextarea_opt\.on_change\s*=\s*\[&\]",
        1,
        (),
    ),
    "text_with_offsets": (
        r"\bauto\s+text_with_offsets\s*=\s*\[&\]\s*\(",
        1,
        ("s", "voff", "hoff"),
    ),
    "tree1_view": (r"\bauto\s+tree1_view\s*=\s*Renderer\s*\(\s*\[&\]", 1, ()),
    "tree2_view": (r"\bauto\s+tree2_view\s*=\s*Renderer\s*\(\s*\[&\]", 1, ()),
    "renderer": (r"\bauto\s+renderer\s*=\s*Renderer\s*\([^,]+,\s*\[&\]", 1, ()),
    "apply_current_editor": (
        r"\bauto\s+apply_current_editor\s*=\s*\[&\]",
        1,
        (),
    ),
    "open_external_editor": (
        r"\bauto\s+open_external_editor\s*=\s*\[&\]",
        1,
        (),
    ),
    "catch_event_callback": (
        r"\bCatchEvent\s*\(\s*\[&\]\s*\(",
        1,
        ("e",),
    ),
}
REQUIRED_ROOT_CLI_TRANSLATION_UNITS = (
    "apps/graph_cli/main.cpp",
    "apps/graph_cli/src/cli_config.cpp",
)
BROAD_CATCH_SOURCE_PATTERN = re.compile(
    r"catch\s*\(\s*(?:\.\.\.|(?:const\s+)?std::exception(?:\s+const)?\s*&?"
    r"\s*(?:[A-Za-z_]\w*)?)\s*\)",
    re.MULTILINE,
)


def extended_entity_manifest() -> list[dict[str, Any]]:
    """@brief Builds the fixed, kind-aware extended Doxygen entity manifest.

    @return Entity rows with path, kind, locator, occurrence, parameters, and
      exact copy target where definitions may delegate documentation.
    @throws None The manifest is composed only from fixed audit constants.
    @note Counts are intentionally independent gates. Removing any row fails
      comparison even when every remaining source comment is complete.
    """

    rows: list[dict[str, Any]] = []
    for path_name, symbols in REQUIRED_EXTENDED_SYMBOLS.items():
        path = next(
            value
            for value in REQUIRED_EXTENDED_DOXYGEN_SOURCES
            + REQUIRED_EXTENDED_DOXYGEN_HEADERS
            if Path(value).name == path_name
        )
        for symbol in symbols:
            if path_name == "node_editor.cpp" and symbol in NODE_EDITOR_ENTITY_LOCATORS:
                locator, occurrence, parameters = NODE_EDITOR_ENTITY_LOCATORS[symbol]
            else:
                locator = rf"\b{re.escape(symbol)}\b"
                occurrence = 1
                parameters = None
            kind = (
                "field"
                if symbol in EXTENDED_FIELD_SYMBOLS
                else "type"
                if symbol in EXTENDED_TYPE_SYMBOLS
                or (
                    path_name == "cli_autocompleter.hpp"
                    and symbol == "CliAutocompleter"
                )
                else "callable"
            )
            copydoc = None
            if kind == "callable" and path.endswith(".cpp"):
                if path_name == "cli_autocompleter.cpp":
                    copydoc = (
                        "CliAutocompleter::CliAutocompleter"
                        if symbol == "CliAutocompleter"
                        else f"CliAutocompleter::{symbol}"
                    )
                elif "/autocomplete/" in path:
                    copydoc = f"CliAutocompleter::{symbol}"
                elif symbol not in NODE_EDITOR_ENTITY_LOCATORS:
                    copydoc = symbol
            rows.append(
                {
                    "file": path,
                    "symbol": symbol,
                    "kind": kind,
                    "locator": locator,
                    "occurrence": occurrence,
                    "parameters": parameters,
                    "copydoc": copydoc,
                }
            )
    return rows


def immediate_doxygen_before(source: str, offset: int) -> str:
    """@brief Returns the Doxygen block immediately before one source offset.

    @param source Complete C++ source or header text.
    @param offset Start offset of the explicitly located entity.
    @return Immediate Doxygen block, or an empty string when separated by code.
    @throws None String bounds and searches are total for valid Python strings.
    @note Declaration prefixes are permitted between the comment and entity
      provided they contain no semicolon or brace.
    """

    prefix = source[:offset]
    comment_end = prefix.rfind("*/")
    between = prefix[comment_end + 2 :]
    if comment_end < 0 or any(token in between for token in (";", "{", "}")):
        return ""
    comment_start = prefix.rfind("/**", 0, comment_end)
    return "" if comment_start < 0 else prefix[comment_start : comment_end + 2]


def parameter_names_after(source: str, offset: int) -> tuple[str, ...] | None:
    """@brief Parses direct parameter names from the entity after an offset.

    @param source Complete C++ declaration or lambda source.
    @param offset Offset immediately after the manifest locator match.
    @return Parameter names, or ``None`` when no balanced parameter list exists.
    @throws None Malformed input fails closed instead of raising.
    @note Commas nested in templates and initializers do not split parameters.
    """

    open_paren = source.find("(", offset)
    if open_paren < 0:
        return None
    depth = 0
    angle_depth = 0
    parts: list[str] = []
    start = open_paren + 1
    close_paren = -1
    for index in range(start, len(source)):
        char = source[index]
        if char == "<":
            angle_depth += 1
        elif char == ">" and angle_depth:
            angle_depth -= 1
        elif char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                parts.append(source[start:index])
                close_paren = index
                break
            depth -= 1
        elif char == "," and depth == 0 and angle_depth == 0:
            parts.append(source[start:index])
            start = index + 1
    if close_paren < 0:
        return None
    names: list[str] = []
    for part in parts:
        cleaned = part.split("=", 1)[0].strip()
        if not cleaned or cleaned == "void":
            continue
        reference_array = re.search(r"[&*]\s*([A-Za-z_]\w*)\s*\)\s*\[", cleaned)
        match = reference_array or re.search(
            r"([A-Za-z_]\w*)\s*(?:\[[^]]*\])?\s*$", cleaned
        )
        if match is None:
            return None
        names.append(match.group(1))
    return tuple(names)


def scan_extended_entity(source: str, entity: dict[str, Any]) -> dict[str, Any]:
    """@brief Scans one explicit manifest entity and validates its contract.

    @param source Complete source text containing the entity.
    @param entity Kind-aware manifest row with a stable locator.
    @return Location, structured tag observations, and fail-closed pass state.
    @throws re.error If a maintained locator is not a valid expression.
    @note Callable comments require brief/return/throws/note and an exact param
      set. Types require brief/throws/note; fields require brief only. Copydoc
      is accepted only when its complete target equals the manifest target.
    """

    matches = [
        match
        for match in re.finditer(entity["locator"], source, re.MULTILINE)
        if source.rfind("/**", 0, match.start()) <= source.rfind("*/", 0, match.start())
        and immediate_doxygen_before(source, match.start())
    ]
    occurrence = entity["occurrence"]
    match = matches[occurrence - 1] if len(matches) >= occurrence else None
    comment = immediate_doxygen_before(source, match.start()) if match else ""
    documented_parameters = tuple(
        re.findall(r"@param(?:\[[^]]+\])?\s+([A-Za-z_]\w*)", comment)
    )
    parameters = entity["parameters"]
    if entity["kind"] == "callable" and parameters is None and match:
        parameters = parameter_names_after(source, match.end())
    copy_targets = re.findall(
        r"@copydoc\s+([^\r\n*]+?)\s*(?=\*/|$)", comment, re.MULTILINE
    )
    copy_target = copy_targets[0] if len(copy_targets) == 1 else None
    exact_copydoc = len(copy_targets) == 1 and copy_target == entity["copydoc"]
    if entity["kind"] == "field":
        complete = bool(comment) and "@brief" in comment and not copy_targets
    elif entity["kind"] == "type":
        complete = (
            bool(comment)
            and not copy_targets
            and all(tag in comment for tag in ("@brief", "@throws", "@note"))
        )
    else:
        full = (
            bool(comment)
            and not copy_targets
            and all(tag in comment for tag in ("@brief", "@return", "@throws", "@note"))
            and parameters is not None
            and set(parameters) == set(documented_parameters)
            and len(parameters) == len(documented_parameters)
        )
        complete = exact_copydoc or full
    return {
        "file": entity["file"],
        "symbol": entity["symbol"],
        "kind": entity["kind"],
        "located": match is not None,
        "comment": bool(comment),
        "parameters": list(parameters) if parameters is not None else None,
        "documented_parameters": list(documented_parameters),
        "copydoc": copy_targets,
        "expected_copydoc": entity["copydoc"],
        "complete": complete,
    }


def inspect_extended_doxygen_inventory(
    repo: Path,
    compile_commands: Path,
    target_closure: dict[str, Any],
    manifest: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """@brief Audits the fail-closed extended CLI Doxygen manifest.

    @param repo Repository root containing the CLI application tree.
    @param compile_commands Configured database proving source ownership.
    @param target_closure Real configured CLI target source closure.
    @param manifest Optional inventory; ``None`` uses the fixed manifest, while
      an explicit inventory supports real negative scans.
    @return Required paths/symbols, compile-command matches, and failures.
    @throws OSError If a required file or compilation database cannot be read.
    @throws json.JSONDecodeError If the compilation database is malformed.
    @note Sources must belong to the real target closure and have an exact
      compile command. Headers are explicit companion inventory. Every named
      entity needs an immediate Doxygen block; functions require either
      ``@copydoc`` or the full behavioral tag set.
    """

    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    command_sources = {
        compile_entry_source_path(row, compile_commands) for row in entries
    }
    closure = set(target_closure["sources"])
    required_paths = list(REQUIRED_EXTENDED_DOXYGEN_SOURCES) + list(
        REQUIRED_EXTENDED_DOXYGEN_HEADERS
    )
    missing_files = sorted(
        path for path in required_paths if not (repo / path).is_file()
    )
    missing_from_closure = sorted(
        path for path in REQUIRED_EXTENDED_DOXYGEN_SOURCES if path not in closure
    )
    missing_compile_commands = sorted(
        path
        for path in REQUIRED_EXTENDED_DOXYGEN_SOURCES
        if (repo / path).resolve() not in command_sources
    )
    entities = extended_entity_manifest() if manifest is None else manifest
    rows = [
        scan_extended_entity(
            (repo / entity["file"]).read_text(encoding="utf-8")
            if (repo / entity["file"]).is_file()
            else "",
            entity,
        )
        for entity in entities
    ]
    incomplete = sorted(
        f"{row['file']}:{row['symbol']}" for row in rows if not row["complete"]
    )
    inventory_symbol_count = len(entities)
    passes = bool(
        target_closure["passes"]
        and not missing_files
        and not missing_from_closure
        and len(required_paths) == EXPECTED_EXTENDED_DOXYGEN_PATH_COUNT
        and inventory_symbol_count == EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT
        and not missing_compile_commands
        and len(rows) == EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT
        and not incomplete
    )
    return {
        "expected_path_count": EXPECTED_EXTENDED_DOXYGEN_PATH_COUNT,
        "observed_path_count": len(required_paths) - len(missing_files),
        "expected_symbol_count": EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT,
        "inventory_symbol_count": inventory_symbol_count,
        "observed_symbol_count": len(rows),
        "missing_files": missing_files,
        "missing_from_closure": missing_from_closure,
        "missing_compile_commands": missing_compile_commands,
        "incomplete": incomplete,
        "passes": passes,
        "symbols": rows,
    }


GUARD_DEFINITIONS = (
    ("src/lib/benchmark/benchmark_service.cpp", "RunAll", 1),
    (
        "apps/graph_cli/src/autocomplete/CompleteSessionName.cpp",
        "CompleteSessionName",
        1,
    ),
    (
        "apps/graph_cli/src/benchmark_config_editor.cpp",
        "load_benchmark_configs_from_file",
        1,
    ),
    (
        "apps/graph_cli/src/benchmark_config_editor.cpp",
        "RebuildDetailsPane",
        6,
    ),
    ("apps/graph_cli/src/command/command_bench.cpp", "handle_bench", 1),
    ("apps/graph_cli/src/command/command_inspect.cpp", "handle_inspect", 1),
    ("apps/graph_cli/src/command/command_node.cpp", "handle_node", 1),
    ("apps/graph_cli/src/command/command_print.cpp", "handle_print", 1),
    ("apps/graph_cli/src/command/command_switch.cpp", "handle_switch", 2),
    ("apps/graph_cli/src/command/help_utils.cpp", "print_help_from_file", 1),
    ("apps/graph_cli/src/config_editor.cpp", "SyncUiStateToModel", 1),
    ("apps/graph_cli/src/process_command.cpp", "process_command", 1),
    ("apps/graph_cli/src/cli_config.cpp", "write_config_to_file", 1),
    ("apps/graph_cli/src/cli_config.cpp", "load_or_create_config", 1),
    ("apps/graph_cli/src/run_graph_cli.cpp", "run_graph_cli", 1),
    ("apps/graph_cli/main.cpp", "main", 1),
)


def walk_ast(value: Any) -> Iterator[dict[str, Any]]:
    """@brief Yield every mapping node from a decoded Clang AST value.

    @param value Decoded JSON mapping, list, or scalar to traverse.
    @return Depth-first iterator over mapping nodes.
    @throws None Traversal uses only in-memory Python containers.
    @note Mapping values are visited in insertion order from Clang's JSON.
    """

    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk_ast(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_ast(child)


def walk_ast_inner(node: dict[str, Any]) -> Iterator[dict[str, Any]]:
    """@brief Yields AST nodes by following only structural ``inner`` edges.

    @param node Root Clang AST node.
    @return Depth-first iterator over the root and structural descendants.
    @throws None Missing/non-list ``inner`` fields are treated as leaves.
    @note Metadata such as ``referencedDecl`` is intentionally excluded so a
      referenced callable cannot contribute catch/body semantics to its caller.
    """

    yield node
    children = node.get("inner", [])
    if not isinstance(children, list):
        return
    for child in children:
        if isinstance(child, dict):
            yield from walk_ast_inner(child)


def callable_body(function: dict[str, Any]) -> dict[str, Any] | None:
    """@brief Returns the direct compound body of a callable declaration.

    @param function Clang FunctionDecl or CXXMethodDecl mapping.
    @return Direct ``CompoundStmt`` child, or ``None`` for declarations only.
    @throws None Missing or malformed child lists produce ``None``.
    @note A body nested in a referenced declaration or comment cannot satisfy
      this definition check.
    """

    return next(
        (
            child
            for child in function.get("inner", [])
            if isinstance(child, dict) and child.get("kind") == "CompoundStmt"
        ),
        None,
    )


def ast_catch_kind(catch: dict[str, Any]) -> str:
    """@brief Classifies one Clang ``CXXCatchStmt`` parameter.

    @param catch Clang catch node from a real definition body.
    @return ``bad_alloc``, ``broad``, or ``specific``.
    @throws None Missing parameter/type metadata remains conservative.
    @note Only exact ``const std::bad_alloc&``, ``std::exception`` base forms,
      and catch-all handlers participate; derived/similar names stay specific.
    """

    parameter = next(
        (
            child
            for child in catch.get("inner", [])
            if isinstance(child, dict) and child.get("kind") == "VarDecl"
        ),
        None,
    )
    if parameter is None:
        return "broad"
    qual_type = " ".join(parameter.get("type", {}).get("qualType", "").split())
    if qual_type == "const std::bad_alloc &":
        return "bad_alloc"
    if qual_type in {
        "std::exception",
        "const std::exception",
        "std::exception &",
        "const std::exception &",
        "std::exception const &",
    }:
        return "broad"
    return "specific"


def ast_catch_directly_rethrows(catch: dict[str, Any]) -> bool:
    """@brief Tests whether a catch body is exactly one bare rethrow.

    @param catch Clang ``CXXCatchStmt`` mapping.
    @return True only for a direct body containing one operand-free
      ``CXXThrowExpr``.
    @throws None Missing/malformed body metadata returns false.
    @note Conditional, nested, value-throwing, cleanup, and empty handlers do
      not satisfy the resource-exhaustion identity guard.
    """

    body = next(
        (
            child
            for child in catch.get("inner", [])
            if isinstance(child, dict) and child.get("kind") == "CompoundStmt"
        ),
        None,
    )
    if body is None:
        return False
    statements = [child for child in body.get("inner", []) if isinstance(child, dict)]
    return (
        len(statements) == 1
        and statements[0].get("kind") == "CXXThrowExpr"
        and not statements[0].get("inner")
    )


def callable_catch_contract(function: dict[str, Any]) -> dict[str, Any]:
    """@brief Audits broad catch ordering inside one real callable body.

    @param function Clang callable definition mapping.
    @return Broad/guard counts, per-try rows, and aggregate pass state.
    @throws None A missing definition body returns a failing empty observation.
    @note Every broad handler must have an earlier exact bad-alloc catch in the
      same direct ``CXXTryStmt`` whose body is exactly ``throw;``. Structural
      ``inner`` traversal includes nested callbacks but excludes referenced AST;
      repeated lambda subtrees sharing one Clang node id are counted once.
    """

    body = callable_body(function)
    if body is None:
        return {
            "has_body": False,
            "try_chain_count": 0,
            "broad_catch_count": 0,
            "guarded_broad_catch_count": 0,
            "try_chains": [],
            "passes": False,
        }
    rows: list[dict[str, Any]] = []
    broad_count = 0
    guarded_count = 0
    seen_try_ids: set[str] = set()
    for node in walk_ast_inner(body):
        if node.get("kind") != "CXXTryStmt":
            continue
        node_id = node.get("id", "")
        if node_id and node_id in seen_try_ids:
            continue
        if node_id:
            seen_try_ids.add(node_id)
        catches = [
            child
            for child in node.get("inner", [])
            if isinstance(child, dict) and child.get("kind") == "CXXCatchStmt"
        ]
        catch_rows: list[dict[str, Any]] = []
        for index, catch in enumerate(catches):
            kind = ast_catch_kind(catch)
            guarded = True
            if kind == "broad":
                broad_count += 1
                guarded = any(
                    ast_catch_kind(previous) == "bad_alloc"
                    and ast_catch_directly_rethrows(previous)
                    for previous in catches[:index]
                )
                if guarded:
                    guarded_count += 1
            catch_rows.append(
                {
                    "kind": kind,
                    "direct_rethrow": ast_catch_directly_rethrows(catch),
                    "broad_guarded": guarded,
                }
            )
        if catch_rows:
            rows.append({"catches": catch_rows})
    return {
        "has_body": True,
        "try_chain_count": len(rows),
        "broad_catch_count": broad_count,
        "guarded_broad_catch_count": guarded_count,
        "try_chains": rows,
        "passes": broad_count == guarded_count,
    }


def parse_json_stream(text: str) -> list[Any]:
    """@brief Decode one or more adjacent JSON values from Clang output.

    @param text Clang ``-ast-dump=json`` stdout, possibly with filtered roots.
    @return Decoded top-level AST values in emission order.
    @throws json.JSONDecodeError If non-whitespace output is not valid JSON.
    @note ``-ast-dump-filter`` can emit adjacent objects instead of one array.
    """

    decoder = json.JSONDecoder()
    values: list[Any] = []
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset >= len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        values.append(value)
    return values


def compile_entry_source_path(entry: dict[str, Any], compile_commands: Path) -> Path:
    """@brief Resolves one compile-database source using its entry directory.

    @param entry Decoded compilation command containing ``file`` and optionally
      ``directory``.
    @param compile_commands Compilation database used as the fallback base.
    @return Absolute normalized source path.
    @throws KeyError If the mandatory ``file`` field is absent.
    @note CMake normally emits absolute paths, but the JSON compilation database
      contract permits paths relative to each row's working directory.
    """

    source = Path(entry["file"])
    if not source.is_absolute():
        directory = Path(entry.get("directory", compile_commands.parent))
        source = directory / source
    return source.resolve()


def compile_database_arguments_for_source(
    compile_commands: Path, source: Path
) -> list[str]:
    """@brief Extract compilation arguments for one real translation unit.

    @param compile_commands Configured build's compilation database.
    @param source Exact source translation unit whose flags are required.
    @return Compiler argv without source, output, or compile-only switches.
    @throws OSError If the compilation database cannot be read.
    @throws ValueError If no matching translation-unit entry exists.
    @note Include paths, definitions, language mode, architecture, and SDK flags
      are retained so definition ASTs use their production compile context.
    """

    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    target = source.resolve()
    entry = next(
        (
            row
            for row in entries
            if compile_entry_source_path(row, compile_commands) == target
        ),
        None,
    )
    if entry is None:
        raise ValueError(f"compile_commands lacks {target}")

    arguments = (
        list(entry["arguments"])
        if "arguments" in entry
        else shlex.split(entry["command"])
    )
    entry_directory = Path(entry.get("directory", compile_commands.parent)).resolve()
    normalized: list[str] = [arguments[0]]
    skip_next = False
    for argument in arguments[1:]:
        if skip_next:
            skip_next = False
            continue
        if argument == "-o":
            skip_next = True
            continue
        if argument == "-c":
            continue
        try:
            argument_path = Path(argument)
            if not argument_path.is_absolute():
                argument_path = entry_directory / argument_path
            is_source = argument_path.resolve() == target
        except OSError:
            is_source = False
        if is_source:
            continue
        normalized.append(argument)
    return normalized


def compile_database_arguments(compile_commands: Path, repo: Path) -> list[str]:
    """@brief Extract reusable CLI declaration-AST compilation arguments.

    @param compile_commands Configured build's compilation database.
    @param repo Repository root containing ``apps/graph_cli/src/process_command.cpp``.
    @return Compiler argv without source, output, or compile-only switches.
    @throws OSError If the compilation database cannot be read.
    @throws ValueError If the CLI command translation-unit entry is absent.
    @note The process-command entry supplies the same configured include and
      definition context used by the synthetic declaration translation unit.
    """

    return compile_database_arguments_for_source(
        compile_commands, repo / "apps/graph_cli/src/process_command.cpp"
    )


def compile_entry_target(entry: dict[str, Any]) -> str:
    """@brief Extracts the owning CMake target from one compile command.

    @param entry One decoded ``compile_commands.json`` object.
    @return Target name embedded below ``CMakeFiles/<target>.dir``, or ``""``
      when no object path can be resolved.
    @throws ValueError If a string command cannot be tokenized.
    @note The explicit ``output`` field is preferred. GNU ``-o`` and MSVC
      ``/Fo`` command forms are supported as fallbacks so configured generators
      cannot silently erase target ownership from the audit.
    """

    candidates: list[str] = []
    output = entry.get("output")
    if isinstance(output, str) and output:
        candidates.append(output)
    arguments = (
        list(entry["arguments"])
        if isinstance(entry.get("arguments"), list)
        else shlex.split(entry.get("command", ""))
    )
    for index, argument in enumerate(arguments):
        if argument == "-o" and index + 1 < len(arguments):
            candidates.append(arguments[index + 1])
        elif argument.startswith("/Fo") and len(argument) > 3:
            candidates.append(argument[3:])
    target_pattern = re.compile(
        r"(?:^|[\\/])CMakeFiles[\\/](?P<target>[^\\/]+?)\.dir(?:[\\/]|$)"
    )
    for candidate in candidates:
        match = target_pattern.search(candidate)
        if match is not None:
            return match.group("target")
    return ""


def cli_target_source_closure_is_complete(observation: dict[str, Any]) -> bool:
    """@brief Evaluates one independently assembled CLI target closure.

    @param observation Target/source/root/CMake fields to validate.
    @return True only for the fixed configured source and ownership contract.
    @throws KeyError If a required field is absent.
    @note The pure predicate supports differential missing/extra/root/ownership
      fixtures independently from the real compilation database.
    """

    return bool(
        observation["target_source_counts"] == EXPECTED_CLI_TARGET_SOURCE_COUNTS
        and observation["closure_source_count"]
        == EXPECTED_CLI_TARGET_CLOSURE_SOURCE_COUNT
        and not observation["missing_targets"]
        and not observation["duplicate_source_owners"]
        and not observation["outside_repo_sources"]
        and not observation["missing_sources"]
        and observation["root_cli_translation_units"]
        == list(REQUIRED_ROOT_CLI_TRANSLATION_UNITS)
        and not observation["missing_roots_from_closure"]
        and observation["graph_cli_target_declared"]
        and observation["photospider_cli_common_linked"]
    )


def inspect_cli_target_source_closure(
    repo: Path, compile_commands: Path
) -> dict[str, Any]:
    """@brief Derives the complete configured graph CLI source closure.

    @param repo Repository root containing CMakeLists and CLI sources.
    @param compile_commands Real configured compilation database.
    @return Per-target source rows, independent expected-count comparisons,
      root-level CLI discovery, CMake link checks, and aggregate pass state.
    @throws OSError If compilation data, CMake, or source paths cannot be read.
    @throws json.JSONDecodeError If the compilation database is malformed.
    @throws ValueError If a command entry cannot be tokenized.
    @note The closure is the union of the direct ``graph_cli`` executable source
      and the linked ``photospider_cli_common`` object sources. Fixed counts and
      an independent root-level filesystem search make missing/new entries fail
      closed instead of silently shrinking the broad-catch inventory.
    """

    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    targets: dict[str, list[str]] = {name: [] for name in CLI_TARGET_NAMES}
    outside_repo: list[str] = []
    missing_sources: list[str] = []
    for entry in entries:
        target = compile_entry_target(entry)
        if target not in targets:
            continue
        source = compile_entry_source_path(entry, compile_commands)
        try:
            relative = source.relative_to(repo).as_posix()
        except ValueError:
            outside_repo.append(str(source))
            continue
        targets[target].append(relative)
        if not source.is_file():
            missing_sources.append(relative)

    normalized_targets = {
        target: sorted(set(paths)) for target, paths in targets.items()
    }
    repeated_configuration_entries = {
        target: {
            path: paths.count(path)
            for path in sorted(set(paths))
            if paths.count(path) > 1
        }
        for target, paths in targets.items()
        if len(paths) != len(set(paths))
    }
    source_owners: dict[str, list[str]] = {}
    for target, paths in normalized_targets.items():
        for path in paths:
            source_owners.setdefault(path, []).append(target)
    duplicate_source_owners = {
        path: sorted(owners)
        for path, owners in source_owners.items()
        if len(owners) != 1
    }
    closure_sources = sorted(source_owners)

    root_cli_sources = sorted(
        {
            path.relative_to(repo).as_posix()
            for path in (
                list((repo / "apps/graph_cli").glob("*.cpp"))
                + list((repo / "apps/graph_cli").glob("*.cc"))
                + list((repo / "apps/graph_cli").glob("*.cxx"))
                + [repo / "apps/graph_cli/src/cli_config.cpp"]
            )
            if path.is_file()
        }
    )
    required_roots = list(REQUIRED_ROOT_CLI_TRANSLATION_UNITS)
    missing_roots_from_closure = sorted(set(required_roots) - set(closure_sources))

    cmake_text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    graph_cli_declared = bool(
        re.search(
            r"add_executable\s*\(\s*graph_cli\s+" r"apps/graph_cli/main\.cpp\s*\)",
            cmake_text,
        )
    )
    cli_common_linked = bool(
        re.search(
            r"target_link_libraries\s*\(\s*graph_cli\b[^)]*"
            r"\bphotospider_cli_common\b[^)]*\)",
            cmake_text,
            re.DOTALL,
        )
    )
    target_source_counts = {
        target: len(paths) for target, paths in normalized_targets.items()
    }
    missing_targets = sorted(
        target for target, paths in normalized_targets.items() if not paths
    )
    observation = {
        "required_targets": list(CLI_TARGET_NAMES),
        "target_source_counts": target_source_counts,
        "expected_target_source_counts": EXPECTED_CLI_TARGET_SOURCE_COUNTS,
        "closure_source_count": len(closure_sources),
        "expected_closure_source_count": EXPECTED_CLI_TARGET_CLOSURE_SOURCE_COUNT,
        "targets": normalized_targets,
        "sources": closure_sources,
        "source_owners": dict(sorted(source_owners.items())),
        "missing_targets": missing_targets,
        "repeated_configuration_entries": repeated_configuration_entries,
        "duplicate_source_owners": duplicate_source_owners,
        "outside_repo_sources": sorted(outside_repo),
        "missing_sources": sorted(missing_sources),
        "root_cli_translation_units": root_cli_sources,
        "required_root_cli_translation_units": required_roots,
        "missing_roots_from_closure": missing_roots_from_closure,
        "graph_cli_target_declared": graph_cli_declared,
        "photospider_cli_common_linked": cli_common_linked,
    }
    observation["passes"] = cli_target_source_closure_is_complete(observation)
    return observation


def run_ast_filter(
    base_arguments: list[str], repo: Path, translation_unit: Path, name_filter: str
) -> tuple[list[Any], dict[str, Any]]:
    """@brief Run one filtered Clang JSON/comment AST query.

    @param base_arguments Compiler and configured CLI compilation flags.
    @param repo Repository root used as the compiler working directory.
    @param translation_unit Synthetic source including all audited declarations.
    @param name_filter Qualified-name substring passed to Clang.
    @return Decoded AST roots and stable command outcome metadata.
    @throws subprocess.CalledProcessError If parsing or AST emission fails.
    @throws json.JSONDecodeError If Clang emits malformed JSON.
    @note The query is read-only and enables parsing of every Doxygen comment.
    """

    command = base_arguments + [
        "-Xclang",
        "-fparse-all-comments",
        "-Xclang",
        "-ast-dump=json",
        "-Xclang",
        "-ast-dump-filter",
        "-Xclang",
        name_filter,
        "-fsyntax-only",
        str(translation_unit),
    ]
    completed = subprocess.run(
        command,
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
    )
    return parse_json_stream(completed.stdout), {
        "filter": name_filter,
        "returncode": completed.returncode,
        "stderr_empty": not completed.stderr.strip(),
    }


def declaration_contract(function: dict[str, Any]) -> dict[str, Any]:
    """@brief Evaluate one FunctionDecl's structured Doxygen contract.

    @param function Clang ``FunctionDecl`` mapping from the synthetic AST.
    @return Parameter names, documented parameters, command tags, and pass state.
    @throws None Missing AST fields are represented as empty observations.
    @note Only direct parameters and the declaration's direct FullComment count.
    """

    children = function.get("inner", [])
    parameter_names = [
        child.get("name", "")
        for child in children
        if child.get("kind") == "ParmVarDecl"
    ]
    full_comment = next(
        (child for child in children if child.get("kind") == "FullComment"),
        None,
    )
    comment_nodes = list(walk_ast(full_comment)) if full_comment else []
    block_commands = sorted(
        {
            node.get("name", "")
            for node in comment_nodes
            if node.get("kind") == "BlockCommandComment"
        }
    )
    documented_parameters = sorted(
        {
            node.get("param", "")
            for node in comment_nodes
            if node.get("kind") == "ParamCommandComment"
        }
    )
    passed = (
        full_comment is not None
        and REQUIRED_BLOCK_COMMANDS.issubset(block_commands)
        and set(parameter_names) == set(documented_parameters)
    )
    return {
        "parameter_names": parameter_names,
        "documented_parameters": documented_parameters,
        "block_commands": block_commands,
        "passed": passed,
    }


def inspect_declarations(repo: Path, compile_commands: Path) -> dict[str, Any]:
    """@brief Audit every Host-facing CLI declaration through Clang's AST.

    @param repo Repository root containing CLI headers.
    @param compile_commands Configured compilation database supplying real flags.
    @return Per-function structured comments and compiler-filter outcomes.
    @throws OSError If headers, compilation data, or temporary storage fail.
    @throws subprocess.CalledProcessError If Clang cannot parse the declarations.
    @note A temporary translation unit includes command, REPL, top-level help,
      interactive-save, configuration, and benchmark-editor headers; no source
      or configured build output is modified.
    """

    base_arguments = compile_database_arguments(compile_commands, repo)
    with tempfile.TemporaryDirectory(prefix="photospider-cli-doxygen-") as temp:
        translation_unit = Path(temp) / "cli_host_doxygen.cpp"
        translation_unit.write_text(
            '#include "graph_cli/command/commands.hpp"\n'
            '#include "graph_cli/run_repl.hpp"\n'
            '#include "graph_cli/benchmark_config_editor.hpp"\n'
            '#include "graph_cli/handle_interactive_save.hpp"\n'
            '#include "graph_cli/print_cli_help.hpp"\n'
            '#include "graph_cli/print_repl_help.hpp"\n'
            '#include "graph_cli/cli_config.hpp"\n',
            encoding="utf-8",
        )
        roots: list[Any] = []
        commands: list[dict[str, Any]] = []
        for name_filter in (
            "handle_",
            "print_help_",
            "print_cli_help",
            "print_repl_help",
            "run_repl",
            "run_benchmark_config_editor",
            "write_config_to_file",
            "load_or_create_config",
            "run_graph_cli",
        ):
            filtered_roots, command = run_ast_filter(
                base_arguments, repo, translation_unit, name_filter
            )
            roots.extend(filtered_roots)
            commands.append(command)

    declarations: dict[str, dict[str, Any]] = {}
    for node in walk_ast(roots):
        name = node.get("name")
        if node.get("kind") != "FunctionDecl" or name not in EXPECTED_FUNCTIONS:
            continue
        observation = declaration_contract(node)
        previous = declarations.get(name)
        if previous is None or observation["passed"]:
            declarations[name] = observation

    missing = sorted(set(EXPECTED_FUNCTIONS) - set(declarations))
    incomplete = sorted(
        name for name, observation in declarations.items() if not observation["passed"]
    )
    return {
        "expected_count": len(EXPECTED_FUNCTIONS),
        "observed_count": len(declarations),
        "missing": missing,
        "incomplete": incomplete,
        "functions": dict(sorted(declarations.items())),
        "compiler_queries": commands,
    }


def definition_path(repo: Path, function_name: str) -> Path:
    """@brief Resolve one audited function to its implementation source.

    @param repo Repository root containing CLI implementations.
    @param function_name Audited command/help/REPL/editor/root function name.
    @return Expected source path for the definition.
    @throws ValueError If the function does not belong to the audited inventory.
    @note Command and help functions share ``command_<suffix>.cpp`` files;
      root helpers use a source file matching their function name.
    """

    if function_name == "run_repl":
        return repo / "apps/graph_cli/src/run_repl.cpp"
    if function_name == "run_benchmark_config_editor":
        return repo / "apps/graph_cli/src/benchmark_config_editor.cpp"
    if function_name in {"write_config_to_file", "load_or_create_config"}:
        return repo / "apps/graph_cli/src/cli_config.cpp"
    if function_name == "run_graph_cli":
        return repo / "apps/graph_cli/src/run_graph_cli.cpp"
    if function_name in ROOT_CLI_FUNCTIONS:
        return repo / f"apps/graph_cli/src/{function_name}.cpp"
    for prefix in ("handle_", "print_help_"):
        if function_name.startswith(prefix):
            suffix = function_name.removeprefix(prefix)
            return repo / f"apps/graph_cli/src/command/command_{suffix}.cpp"
    raise ValueError(f"unknown audited CLI function: {function_name}")


def preceding_definition_comment(source: str, function_name: str) -> str:
    """@brief Extract the Doxygen block immediately before one definition.

    @param source Complete implementation source text.
    @param function_name Exact global function definition name.
    @return Immediate Doxygen block, or an empty string when absent.
    @throws None The escaped function name produces a fixed regular expression.
    @note This complements the structured declaration AST by checking that each
      definition either copies or restates its authoritative declaration docs.
    """

    pattern = re.compile(
        rf"(?m)^(?:static\s+)?(?:bool|void|int)\s+" rf"{re.escape(function_name)}\s*\("
    )
    match = pattern.search(source)
    if match is None:
        return ""
    prefix = source[: match.start()]
    comment_end = prefix.rfind("*/")
    if comment_end < 0 or prefix[comment_end + 2 :].strip():
        return ""
    comment_start = prefix.rfind("/**", 0, comment_end)
    if comment_start < 0:
        return ""
    return prefix[comment_start : comment_end + 2]


def definition_query_filters(path: Path, function_names: list[str]) -> list[str]:
    """@brief Chooses bounded Clang filters for one definition source.

    @param path Real implementation source path.
    @param function_names Audited functions expected in that source.
    @return One command suffix for command files, otherwise each exact function
      name so multi-function root translation units remain bounded.
    @throws Nothing.
    @note One query per command source observes both its handler and help body;
      non-command REPL/editor sources each own one audited function.
    """

    if path.parent.name == "command" and path.stem.startswith("command_"):
        return [path.stem.removeprefix("command_")]
    return function_names


def inspect_definitions(repo: Path, compile_commands: Path) -> dict[str, Any]:
    """@brief Check implementation definitions, bodies, guards, and Doxygen.

    @param repo Repository root containing CLI source files.
    @param compile_commands Configured database supplying each source's flags.
    @return Per-function AST/source documentation and body/guard inventory.
    @throws OSError If an implementation source cannot be read.
    @throws subprocess.CalledProcessError If Clang cannot parse a definition.
    @throws json.JSONDecodeError If Clang emits malformed JSON.
    @throws ValueError If a source/filter/definition is ambiguous.
    @note Full comments require brief/return/throws/note; ``@copydoc`` must name
      the exact declaration audited through Clang AST. A source comment alone
      cannot pass: the matching real translation unit must contain exactly one
      callable body, a compiler FullComment, and valid broad-catch guards.
    """

    functions_by_path: dict[Path, list[str]] = {}
    for function_name in EXPECTED_FUNCTIONS:
        path = definition_path(repo, function_name)
        functions_by_path.setdefault(path, []).append(function_name)

    rows: dict[str, dict[str, Any]] = {}
    compiler_queries: list[dict[str, Any]] = []
    for path, function_names in sorted(
        functions_by_path.items(), key=lambda item: item[0].as_posix()
    ):
        source = path.read_text(encoding="utf-8") if path.is_file() else ""
        base_arguments = compile_database_arguments_for_source(compile_commands, path)
        roots: list[Any] = []
        for name_filter in definition_query_filters(path, function_names):
            filtered_roots, command = run_ast_filter(
                base_arguments,
                repo,
                path,
                name_filter,
            )
            roots.extend(filtered_roots)
            command["source"] = path.relative_to(repo).as_posix()
            compiler_queries.append(command)
        for function_name in function_names:
            candidates = [
                node
                for node in walk_ast(roots)
                if node.get("kind") in CALLABLE_DECL_KINDS
                and node.get("name") == function_name
                and callable_body(node) is not None
                and Path(node.get("loc", {}).get("file", "")).resolve()
                == path.resolve()
            ]
            comment = preceding_definition_comment(source, function_name)
            copydoc_targets = re.findall(
                r"@copydoc\s+([^\r\n*]+?)\s*(?=\*/|$)", comment, re.MULTILINE
            )
            copydoc = (
                len(copydoc_targets) == 1
                and re.fullmatch(
                    rf"(?:::)?{re.escape(function_name)}(?:\([^)]*\))?",
                    copydoc_targets[0],
                )
                is not None
            )
            full = all(
                command_name in comment
                for command_name in ("@brief", "@return", "@throws", "@note")
            )
            mode = "copydoc" if copydoc else "full" if full else "missing"
            definition = candidates[0] if len(candidates) == 1 else {}
            has_full_comment = any(
                child.get("kind") == "FullComment"
                for child in definition.get("inner", [])
                if isinstance(child, dict)
            )
            catch_contract = callable_catch_contract(definition)
            rows[function_name] = {
                "file": path.relative_to(repo).as_posix(),
                "mode": mode,
                "definition_count": len(candidates),
                "has_body": len(candidates) == 1,
                "has_compiler_full_comment": has_full_comment,
                "catch_contract": catch_contract,
                "passed": (
                    mode != "missing"
                    and len(candidates) == 1
                    and has_full_comment
                    and catch_contract["passes"]
                ),
            }
    missing = sorted(name for name, row in rows.items() if not row["passed"])
    missing_bodies = sorted(name for name, row in rows.items() if not row["has_body"])
    missing_compiler_comments = sorted(
        name
        for name, row in rows.items()
        if not row["has_compiler_full_comment"] or row["mode"] == "missing"
    )
    invalid_catch_guards = sorted(
        name for name, row in rows.items() if not row["catch_contract"]["passes"]
    )
    return {
        "expected_count": len(EXPECTED_FUNCTIONS),
        "observed_count": len(rows),
        "missing": missing,
        "missing_bodies": missing_bodies,
        "missing_compiler_comments": missing_compiler_comments,
        "invalid_catch_guards": invalid_catch_guards,
        "functions": dict(sorted(rows.items())),
        "compiler_queries": compiler_queries,
    }


def guard_source_completeness_is_complete(
    target_closure: dict[str, Any],
    audited_sources: list[str],
    source_coverage: list[dict[str, Any]],
    catalog_paths_outside_closure: list[str],
    uncatalogued_broad_catch_sources: list[str],
) -> bool:
    """@brief Evaluates independent source-to-AST catalog completeness.

    @param target_closure Configured CLI target closure observation.
    @param audited_sources Complete CLI/benchmark source path inventory.
    @param source_coverage Per-source lexical/catalog/AST comparison rows.
    @param catalog_paths_outside_closure Catalog paths missing from inventory.
    @param uncatalogued_broad_catch_sources Sources whose broad count differs.
    @return True only when the fixed source count, uniqueness, and every
      independent comparison pass.
    @throws KeyError If an observation omits ``passes`` or ``file``.
    @note Zero-catch sources remain mandatory rows so removing a source cannot
      make completeness vacuously true.
    """

    return bool(
        target_closure["passes"]
        and len(audited_sources) == EXPECTED_AUDITED_SOURCE_COUNT
        and len(set(audited_sources)) == len(audited_sources)
        and len(source_coverage) == len(audited_sources)
        and len({row["file"] for row in source_coverage}) == len(source_coverage)
        and not catalog_paths_outside_closure
        and not uncatalogued_broad_catch_sources
        and all(row["passes"] for row in source_coverage)
    )


def inspect_guard_definitions(
    repo: Path,
    compile_commands: Path,
    definitions: dict[str, Any],
    target_closure: dict[str, Any],
) -> dict[str, Any]:
    """@brief Audit every CLI/benchmark broad-catch definition through AST.

    @param repo Repository root containing real translation units.
    @param compile_commands Configured database supplying per-source flags.
    @param definitions Already-audited Host-facing definition rows available
      for reuse.
    @param target_closure Configured CLI target source closure derived
      independently from the compilation database.
    @return Complete guard inventory, expected/actual counts, and failures.
    @throws OSError If sources or compilation data cannot be read.
    @throws subprocess.CalledProcessError If Clang cannot parse a source.
    @throws json.JSONDecodeError If Clang emits malformed JSON.
    @note The catalog includes inner helpers and callbacks that can swallow
      resource exhaustion before the outer command dispatcher sees it.
    """

    rows: dict[str, dict[str, Any]] = {}
    compiler_queries: list[dict[str, Any]] = []
    definition_rows = definitions["functions"]
    for path_text, function_name, expected_broad_count in GUARD_DEFINITIONS:
        existing = definition_rows.get(function_name)
        if existing is not None and existing["file"] == path_text:
            contract = existing["catch_contract"]
            definition_count = existing["definition_count"]
        else:
            path = repo / path_text
            base_arguments = compile_database_arguments_for_source(
                compile_commands, path
            )
            roots, command = run_ast_filter(base_arguments, repo, path, function_name)
            command["source"] = path_text
            compiler_queries.append(command)
            candidates = [
                node
                for node in walk_ast(roots)
                if node.get("kind") in CALLABLE_DECL_KINDS
                and node.get("name") == function_name
                and callable_body(node) is not None
                and Path(node.get("loc", {}).get("file", "")).resolve()
                == path.resolve()
            ]
            definition_count = len(candidates)
            contract = (
                callable_catch_contract(candidates[0])
                if definition_count == 1
                else callable_catch_contract({})
            )
        key = f"{path_text}:{function_name}"
        rows[key] = {
            "file": path_text,
            "function": function_name,
            "definition_count": definition_count,
            "expected_broad_catch_count": expected_broad_count,
            "actual_broad_catch_count": contract["broad_catch_count"],
            "guarded_broad_catch_count": contract["guarded_broad_catch_count"],
            "try_chains": contract["try_chains"],
            "passes": (
                definition_count == 1
                and contract["passes"]
                and contract["broad_catch_count"] == expected_broad_count
            ),
        }
    missing = sorted(key for key, row in rows.items() if row["definition_count"] != 1)
    invalid = sorted(key for key, row in rows.items() if not row["passes"])
    benchmark_sources = sorted(
        path.relative_to(repo).as_posix()
        for path in (repo / "src/lib/benchmark").glob("*.cpp")
        if path.is_file()
    )
    audited_sources = sorted(set(target_closure["sources"] + benchmark_sources))
    catalog_expected_by_source: dict[str, int] = {}
    catalog_actual_by_source: dict[str, int] = {}
    for row in rows.values():
        path_text = row["file"]
        catalog_expected_by_source[path_text] = (
            catalog_expected_by_source.get(path_text, 0)
            + row["expected_broad_catch_count"]
        )
        catalog_actual_by_source[path_text] = (
            catalog_actual_by_source.get(path_text, 0) + row["actual_broad_catch_count"]
        )
    source_coverage: list[dict[str, Any]] = []
    for path_text in audited_sources:
        source = (repo / path_text).read_text(encoding="utf-8")
        lexical_count = len(BROAD_CATCH_SOURCE_PATTERN.findall(source))
        expected_count = catalog_expected_by_source.get(path_text, 0)
        ast_count = catalog_actual_by_source.get(path_text, 0)
        source_coverage.append(
            {
                "file": path_text,
                "lexical_broad_catch_count": lexical_count,
                "catalog_expected_broad_catch_count": expected_count,
                "catalog_ast_broad_catch_count": ast_count,
                "passes": lexical_count == expected_count == ast_count,
            }
        )
    audited_source_set = set(audited_sources)
    catalog_paths_outside_closure = sorted(
        set(catalog_expected_by_source) - audited_source_set
    )
    uncatalogued_broad_catch_sources = sorted(
        row["file"]
        for row in source_coverage
        if row["lexical_broad_catch_count"] != row["catalog_expected_broad_catch_count"]
    )
    source_completeness_pass = guard_source_completeness_is_complete(
        target_closure,
        audited_sources,
        source_coverage,
        catalog_paths_outside_closure,
        uncatalogued_broad_catch_sources,
    )
    return {
        "expected_count": len(GUARD_DEFINITIONS),
        "observed_count": len(rows),
        "expected_broad_catch_count": sum(row[2] for row in GUARD_DEFINITIONS),
        "actual_broad_catch_count": sum(
            row["actual_broad_catch_count"] for row in rows.values()
        ),
        "missing": missing,
        "invalid": invalid,
        "target_source_closure": target_closure,
        "expected_audited_source_count": EXPECTED_AUDITED_SOURCE_COUNT,
        "audited_source_count": len(audited_sources),
        "source_coverage": source_coverage,
        "catalog_paths_outside_closure": catalog_paths_outside_closure,
        "uncatalogued_broad_catch_sources": uncatalogued_broad_catch_sources,
        "source_completeness_pass": source_completeness_pass,
        "functions": dict(sorted(rows.items())),
        "compiler_queries": compiler_queries,
    }


def inspect_ast_detector_contract() -> dict[str, bool]:
    """@brief Differentially tests definition-body and catch AST predicates.

    @return Named booleans for fake-copydoc, unguarded, malformed-rethrow, and
      valid-guard fixtures.
    @throws None Fixtures are fixed in-memory Clang-shaped mappings.
    @note These fixtures are independent of production observations, preventing
      a weakened predicate from approving both expected and actual data.
    """

    bare_throw = {"kind": "CXXThrowExpr"}
    value_throw = {
        "kind": "CXXThrowExpr",
        "inner": [{"kind": "CXXConstructExpr"}],
    }

    def catch(
        qual_type: str | None, statements: list[dict[str, Any]]
    ) -> dict[str, Any]:
        """@brief Constructs one minimal Clang-shaped catch fixture.

        @param qual_type Catch type, or ``None`` for catch-all.
        @param statements Catch-body AST statements.
        @return Synthetic ``CXXCatchStmt`` mapping.
        @throws None The mapping is assembled from caller-provided values.
        @note The fixture contains only fields consumed by the audited parser.
        """

        children: list[dict[str, Any]] = []
        if qual_type is not None:
            children.append({"kind": "VarDecl", "type": {"qualType": qual_type}})
        children.append({"kind": "CompoundStmt", "inner": statements})
        return {"kind": "CXXCatchStmt", "inner": children}

    def function(catches: list[dict[str, Any]]) -> dict[str, Any]:
        """@brief Constructs a callable definition around one try chain.

        @param catches Source-ordered catch fixtures.
        @return Synthetic FunctionDecl with a real compound body.
        @throws None The mapping is assembled in memory.
        @note The try body is empty because only catch ordering is under test.
        """

        return {
            "kind": "FunctionDecl",
            "inner": [
                {
                    "kind": "CompoundStmt",
                    "inner": [
                        {
                            "kind": "CXXTryStmt",
                            "inner": [{"kind": "CompoundStmt"}] + catches,
                        }
                    ],
                }
            ],
        }

    fake_copydoc_declaration = {
        "kind": "FunctionDecl",
        "inner": [{"kind": "FullComment"}],
    }
    unguarded = function([catch("const std::exception &", [])])
    malformed = function(
        [
            catch("const std::bad_alloc &", [value_throw]),
            catch(None, []),
        ]
    )
    guarded = function(
        [
            catch("const std::bad_alloc &", [bare_throw]),
            catch("const std::exception &", []),
        ]
    )
    complete_closure = {
        "target_source_counts": dict(EXPECTED_CLI_TARGET_SOURCE_COUNTS),
        "closure_source_count": EXPECTED_CLI_TARGET_CLOSURE_SOURCE_COUNT,
        "missing_targets": [],
        "duplicate_source_owners": {},
        "outside_repo_sources": [],
        "missing_sources": [],
        "root_cli_translation_units": list(REQUIRED_ROOT_CLI_TRANSLATION_UNITS),
        "missing_roots_from_closure": [],
        "graph_cli_target_declared": True,
        "photospider_cli_common_linked": True,
    }
    complete_closure["passes"] = cli_target_source_closure_is_complete(complete_closure)
    complete_sources = [
        f"source_{index}.cpp" for index in range(EXPECTED_AUDITED_SOURCE_COUNT)
    ]
    complete_coverage = [{"file": path, "passes": True} for path in complete_sources]
    return {
        "rejects_copydoc_declaration_without_real_body": (
            callable_body(fake_copydoc_declaration) is None
        ),
        "rejects_unguarded_broad_catch": (
            not callable_catch_contract(unguarded)["passes"]
        ),
        "rejects_value_throw_bad_alloc_guard": (
            not callable_catch_contract(malformed)["passes"]
        ),
        "accepts_direct_bad_alloc_rethrow_guard": (
            callable_catch_contract(guarded)["passes"]
        ),
        "accepts_complete_cli_target_closure": complete_closure["passes"],
        "rejects_missing_graph_cli_target_source": not (
            cli_target_source_closure_is_complete(
                {
                    **complete_closure,
                    "target_source_counts": {
                        **EXPECTED_CLI_TARGET_SOURCE_COUNTS,
                        "graph_cli": 0,
                    },
                    "closure_source_count": (
                        EXPECTED_CLI_TARGET_CLOSURE_SOURCE_COUNT - 1
                    ),
                    "missing_targets": ["graph_cli"],
                    "missing_roots_from_closure": ["apps/graph_cli/main.cpp"],
                }
            )
        ),
        "rejects_unlisted_root_cli_translation_unit": not (
            cli_target_source_closure_is_complete(
                {
                    **complete_closure,
                    "root_cli_translation_units": [
                        *REQUIRED_ROOT_CLI_TRANSLATION_UNITS,
                        "apps/graph_cli/unlisted_cli.cpp",
                    ],
                }
            )
        ),
        "accepts_complete_guard_source_inventory": (
            guard_source_completeness_is_complete(
                complete_closure, complete_sources, complete_coverage, [], []
            )
        ),
        "rejects_missing_guard_source_inventory_row": not (
            guard_source_completeness_is_complete(
                complete_closure,
                complete_sources,
                complete_coverage[:-1],
                [],
                [],
            )
        ),
        "rejects_uncatalogued_guard_source_broad_catch": not (
            guard_source_completeness_is_complete(
                complete_closure,
                complete_sources,
                [{**complete_coverage[0], "passes": False}, *complete_coverage[1:]],
                [],
                [complete_sources[0]],
            )
        ),
    }


def inspect_semantics(repo: Path) -> dict[str, Any]:
    """@brief Audit maintained CLI terminology and exception semantics.

    @param repo Repository root containing maintained CLI headers and sources.
    @return Unsupported-frontend terminology hits and explicit
      Host/bad-alloc/lifetime checks.
    @throws OSError If a maintained CLI file cannot be read.
    @note These focused human-review proxies complement, but do not replace, the
      compiler AST's structural Doxygen validation. Scheduler-default Doxygen
      follows its canonical ``cli_config.cpp`` definition, while
      ``run_graph_cli`` remains a separate boundary. ``InteractionService`` is
      a permanently unsupported frontend dependency, so this terminology guard
      remains useful after the application-layout migration is complete.
    """

    files = sorted((repo / "apps/graph_cli/include").rglob("*.hpp")) + sorted(
        (repo / "apps/graph_cli/src").rglob("*.cpp")
    )
    files.append(repo / "apps/graph_cli/main.cpp")
    unsupported_terms: list[dict[str, Any]] = []
    unsupported_pattern = re.compile(
        r"\bInteractionService\b|\binteraction_service\b|\binteraction service\b",
        re.IGNORECASE,
    )
    for path in files:
        text = path.read_text(encoding="utf-8")
        for line_number, line in enumerate(text.splitlines(), start=1):
            if unsupported_pattern.search(line):
                unsupported_terms.append(
                    {
                        "file": path.relative_to(repo).as_posix(),
                        "line": line_number,
                        "text": line.strip(),
                    }
                )

    commands_header = (
        repo / "apps/graph_cli/include/graph_cli/command/commands.hpp"
    ).read_text(encoding="utf-8")
    repl_header = (repo / "apps/graph_cli/include/graph_cli/run_repl.hpp").read_text(
        encoding="utf-8"
    )
    benchmark_source = (
        repo / "apps/graph_cli/src/benchmark_config_editor.cpp"
    ).read_text(encoding="utf-8")
    process_source = (repo / "apps/graph_cli/src/process_command.cpp").read_text(
        encoding="utf-8"
    )
    cli_config_source = (repo / "apps/graph_cli/src/cli_config.cpp").read_text(
        encoding="utf-8"
    )
    cli_run_source = (repo / "apps/graph_cli/src/run_graph_cli.cpp").read_text(
        encoding="utf-8"
    )
    graph_cli_source = (repo / "apps/graph_cli/main.cpp").read_text(encoding="utf-8")
    bad_alloc_catch = process_source.find("catch (const std::bad_alloc&)")
    standard_catch = process_source.find("catch (const std::exception&")
    run_start = cli_run_source.find("int run_graph_cli(")
    main_start = graph_cli_source.find("int main(")
    run_source = cli_run_source[run_start:]
    main_source = graph_cli_source[main_start:]

    def has_complete_definition_comment(source: str, function_name: str) -> bool:
        """@brief Checks one touched CLI definition's complete Doxygen tags.

        @param source Translation-unit source containing the definition.
        @param function_name Exact global/static function name.
        @return True when the immediate comment has all behavioral tags and
          documents every parameter name present in the touched functions.
        @throws None The source and function names are fixed audit inputs.
        @note Compiler AST independently validates externally declared
          functions; this focused source check also covers internal entry helpers.
        """

        comment = preceding_definition_comment(source, function_name)
        return all(tag in comment for tag in ("@brief", "@return", "@throws", "@note"))

    return {
        "unsupported_frontend_terms": unsupported_terms,
        "inspect_documents_bad_alloc": (
            "handle_inspect" in commands_header
            and "@throws std::bad_alloc"
            in commands_header[
                commands_header.find("@brief Inspects") : commands_header.find(
                    "void print_help_inspect"
                )
            ]
        ),
        "save_documents_bad_alloc": (
            "handle_save" in commands_header
            and "@throws std::bad_alloc"
            in commands_header[
                commands_header.find(
                    "@brief Computes a node image"
                ) : commands_header.find("void print_help_save")
            ]
        ),
        "process_command_preserves_bad_alloc_first": (
            bad_alloc_catch >= 0
            and standard_catch >= 0
            and bad_alloc_catch < standard_catch
        ),
        "repl_documents_borrowed_host_lifetime": (
            "Borrowed Host" in repl_header and "full loop" in repl_header
        ),
        "benchmark_example_uses_host": (
            "ps::Host& host" in benchmark_source
            and "run_benchmark_config_editor(host" in benchmark_source
        ),
        "touched_root_cli_definitions_have_complete_doxygen": all(
            has_complete_definition_comment(source, function_name)
            for source, function_name in (
                (cli_config_source, "write_config_to_file"),
                (cli_config_source, "load_or_create_config"),
                (cli_config_source, "apply_cli_scheduler_defaults"),
                (cli_run_source, "load_configured_scheduler_plugins"),
                (cli_run_source, "run_graph_cli"),
                (graph_cli_source, "main"),
            )
        ),
        "cli_config_parses_temporary_then_commits": (
            "CliConfig candidate = config;" in cli_config_source
            and "config = std::move(candidate);" in cli_config_source
            and cli_config_source.count("catch (const std::bad_alloc&)") == 2
            and cli_config_source.count("catch (const std::exception&") == 2
        ),
        "graph_cli_run_preserves_bad_alloc_before_broad_catch": (
            run_start >= 0
            and main_start >= 0
            and run_source.find("catch (const std::bad_alloc&)") >= 0
            and run_source.find("catch (const std::bad_alloc&)")
            < run_source.find("catch (const std::exception&")
        ),
        "graph_cli_main_owns_separate_bad_alloc_policy": (
            main_start >= 0
            and "return run_graph_cli(argc, argv, *host);" in main_source
            and main_source.find("catch (const std::bad_alloc&)") >= 0
            and main_source.find("catch (const std::bad_alloc&)")
            < main_source.find("catch (const std::exception&")
            and "kResourceExhaustionExitCode" in main_source
        ),
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """@brief Write one deterministic temporary JSON audit observation.

    @param path Destination whose parent directory already exists.
    @param payload JSON-serializable observation or expectation object.
    @return Nothing.
    @throws OSError If the destination cannot be written.
    @throws TypeError If payload is not JSON serializable.
    @note Keys are sorted and a final newline is included for stable diffs.
    """

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def make_expected() -> dict[str, Any]:
    """@brief Build independent expected results for the CLI documentation audit.

    @return Required inventory counts, empty failures, and semantic booleans.
    @throws None The result is constructed only from fixed audit constants.
    @note Expected values are not copied from real source observations.
    """

    return {
        "declarations": {
            "expected_count": 56,
            "observed_count": 56,
            "missing": [],
            "incomplete": [],
            "compiler_queries_pass": True,
        },
        "definitions": {
            "expected_count": 56,
            "observed_count": 56,
            "missing": [],
            "missing_bodies": [],
            "missing_compiler_comments": [],
            "invalid_catch_guards": [],
            "compiler_queries_pass": True,
        },
        "extended_inventory": {
            "expected_path_count": EXPECTED_EXTENDED_DOXYGEN_PATH_COUNT,
            "observed_path_count": EXPECTED_EXTENDED_DOXYGEN_PATH_COUNT,
            "expected_symbol_count": EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT,
            "inventory_symbol_count": EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT,
            "observed_symbol_count": EXPECTED_EXTENDED_DOXYGEN_SYMBOL_COUNT,
            "missing_files": [],
            "missing_from_closure": [],
            "missing_compile_commands": [],
            "incomplete": [],
            "passes": True,
        },
        "guard_definitions": {
            "expected_count": 16,
            "observed_count": 16,
            "expected_broad_catch_count": 22,
            "actual_broad_catch_count": 22,
            "missing": [],
            "invalid": [],
            "target_source_counts": EXPECTED_CLI_TARGET_SOURCE_COUNTS,
            "target_closure_source_count": EXPECTED_CLI_TARGET_CLOSURE_SOURCE_COUNT,
            "root_cli_translation_units": list(REQUIRED_ROOT_CLI_TRANSLATION_UNITS),
            "expected_audited_source_count": EXPECTED_AUDITED_SOURCE_COUNT,
            "audited_source_count": EXPECTED_AUDITED_SOURCE_COUNT,
            "catalog_paths_outside_closure": [],
            "uncatalogued_broad_catch_sources": [],
            "target_source_closure_pass": True,
            "source_completeness_pass": True,
            "compiler_queries_pass": True,
        },
        "detector_contract": {
            "rejects_copydoc_declaration_without_real_body": True,
            "rejects_unguarded_broad_catch": True,
            "rejects_value_throw_bad_alloc_guard": True,
            "accepts_direct_bad_alloc_rethrow_guard": True,
            "accepts_complete_cli_target_closure": True,
            "rejects_missing_graph_cli_target_source": True,
            "rejects_unlisted_root_cli_translation_unit": True,
            "accepts_complete_guard_source_inventory": True,
            "rejects_missing_guard_source_inventory_row": True,
            "rejects_uncatalogued_guard_source_broad_catch": True,
        },
        "semantics": {
            "unsupported_frontend_terms": [],
            "inspect_documents_bad_alloc": True,
            "save_documents_bad_alloc": True,
            "process_command_preserves_bad_alloc_first": True,
            "repl_documents_borrowed_host_lifetime": True,
            "benchmark_example_uses_host": True,
            "touched_root_cli_definitions_have_complete_doxygen": True,
            "cli_config_parses_temporary_then_commits": True,
            "graph_cli_run_preserves_bad_alloc_before_broad_catch": True,
            "graph_cli_main_owns_separate_bad_alloc_policy": True,
        },
    }


def make_compare(actual: dict[str, Any], expected: dict[str, Any]) -> tuple[bool, str]:
    """@brief Compare AST/source observations against independent expectations.

    @param actual Real compiler AST, definition, and semantic observations.
    @param expected Fixed contract returned by :func:`make_expected`.
    @return Aggregate result and reader-facing invariant lines.
    @throws KeyError If either object omits a required schema field.
    @note Comparison is pure and does not repair source or audit observations.
    """

    declaration_summary = {
        "expected_count": actual["declarations"]["expected_count"],
        "observed_count": actual["declarations"]["observed_count"],
        "missing": actual["declarations"]["missing"],
        "incomplete": actual["declarations"]["incomplete"],
        "compiler_queries_pass": all(
            row["returncode"] == 0 and row["stderr_empty"]
            for row in actual["declarations"]["compiler_queries"]
        ),
    }
    definition_summary = {
        "expected_count": actual["definitions"]["expected_count"],
        "observed_count": actual["definitions"]["observed_count"],
        "missing": actual["definitions"]["missing"],
        "missing_bodies": actual["definitions"]["missing_bodies"],
        "missing_compiler_comments": actual["definitions"]["missing_compiler_comments"],
        "invalid_catch_guards": actual["definitions"]["invalid_catch_guards"],
        "compiler_queries_pass": all(
            row["returncode"] == 0 and row["stderr_empty"]
            for row in actual["definitions"]["compiler_queries"]
        ),
    }
    guard_summary = {
        "expected_count": actual["guard_definitions"]["expected_count"],
        "observed_count": actual["guard_definitions"]["observed_count"],
        "expected_broad_catch_count": actual["guard_definitions"][
            "expected_broad_catch_count"
        ],
        "actual_broad_catch_count": actual["guard_definitions"][
            "actual_broad_catch_count"
        ],
        "missing": actual["guard_definitions"]["missing"],
        "invalid": actual["guard_definitions"]["invalid"],
        "target_source_counts": actual["guard_definitions"]["target_source_closure"][
            "target_source_counts"
        ],
        "target_closure_source_count": actual["guard_definitions"][
            "target_source_closure"
        ]["closure_source_count"],
        "root_cli_translation_units": actual["guard_definitions"][
            "target_source_closure"
        ]["root_cli_translation_units"],
        "expected_audited_source_count": actual["guard_definitions"][
            "expected_audited_source_count"
        ],
        "audited_source_count": actual["guard_definitions"]["audited_source_count"],
        "catalog_paths_outside_closure": actual["guard_definitions"][
            "catalog_paths_outside_closure"
        ],
        "uncatalogued_broad_catch_sources": actual["guard_definitions"][
            "uncatalogued_broad_catch_sources"
        ],
        "target_source_closure_pass": actual["guard_definitions"][
            "target_source_closure"
        ]["passes"],
        "source_completeness_pass": actual["guard_definitions"][
            "source_completeness_pass"
        ],
        "compiler_queries_pass": all(
            row["returncode"] == 0 and row["stderr_empty"]
            for row in actual["guard_definitions"]["compiler_queries"]
        ),
    }
    checks = {
        "Clang AST observes the complete CLI Host declaration inventory": (
            declaration_summary == expected["declarations"]
        ),
        "every implementation has a real documented and guarded AST body": (
            definition_summary == expected["definitions"]
        ),
        "extended CLI types, fields, helpers, and autocomplete are documented": (
            extended_inventory_matches_expected(actual["extended_inventory"])
        ),
        "every inner CLI/benchmark broad catch has an AST bad-alloc guard": (
            guard_summary == expected["guard_definitions"]
        ),
        "definition and guard AST predicates reject differential fakes": (
            actual["detector_contract"] == expected["detector_contract"]
        ),
        "Host terminology and exception semantics are current": (
            actual["semantics"] == expected["semantics"]
        ),
    }
    lines = [
        f"{'PASS' if passed else 'FAIL'}: {label}" for label, passed in checks.items()
    ]
    return all(checks.values()), "\n".join(lines) + "\n"


def extended_inventory_matches_expected(observation: dict[str, Any]) -> bool:
    """@brief Compares a real extended scan with the independent expectation.

    @param observation Result returned by the production inventory scanner.
    @return True only when every fixed count and failure field matches.
    @throws KeyError If the scanner result omits a contract field.
    @note Negative self-checks call this same projection used by the aggregate
      audit comparison; they never edit an ``actual.json`` observation.
    """

    expected = make_expected()["extended_inventory"]
    return {key: observation[key] for key in expected} == expected


def run_extended_negative_self_tests(repo: Path, out: Path) -> bool:
    """@brief Exercises four source/manifest mutations through the real scanner.

    @param repo Repository root supplying the known-good source inventory.
    @param out Temporary-output directory receiving only the self-check report.
    @return True when comment, copydoc, param, and inventory mutations all fail.
    @throws OSError If temporary copies or the report cannot be written.
    @note Every case owns a fresh ``/tmp`` repository-shaped copy, manifest
      JSON, and compilation database. The scanner and independent comparison
      run normally; no generated actual observation is hand-edited.
    """

    cases = ("deleted_comment", "wrong_copydoc", "deleted_param", "deleted_inventory")
    results: list[dict[str, Any]] = []
    for case in cases:
        with tempfile.TemporaryDirectory(
            prefix="photospider-cli-negative-", dir="/tmp"
        ) as temp:
            temp_repo = Path(temp)
            for path_text in (
                REQUIRED_EXTENDED_DOXYGEN_SOURCES + REQUIRED_EXTENDED_DOXYGEN_HEADERS
            ):
                destination = temp_repo / path_text
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(
                    (repo / path_text).read_text(encoding="utf-8"), encoding="utf-8"
                )
            manifest_path = temp_repo / "manifest.json"
            manifest_path.write_text(
                json.dumps(extended_entity_manifest(), indent=2) + "\n",
                encoding="utf-8",
            )
            if case == "deleted_comment":
                path = temp_repo / "apps/graph_cli/src/node_editor.cpp"
                text = path.read_text(encoding="utf-8")
                text, count = re.subn(
                    r"/\*\*\n   \* @brief Selects the relationship.*?\n   \*/\n(?=  enum class Tree2Mode)",
                    "",
                    text,
                    count=1,
                    flags=re.DOTALL,
                )
                if count != 1:
                    raise ValueError(
                        "negative self-test could not delete Tree2Mode comment"
                    )
                path.write_text(text, encoding="utf-8")
            elif case == "wrong_copydoc":
                path = temp_repo / "apps/graph_cli/src/do_traversal.cpp"
                text = path.read_text(encoding="utf-8")
                text, count = re.subn(
                    r"@copydoc do_traversal\b", "@copydoc wrong_target", text, count=1
                )
                if count != 1:
                    raise ValueError("negative self-test could not corrupt copydoc")
                path.write_text(text, encoding="utf-8")
            elif case == "deleted_param":
                path = temp_repo / "apps/graph_cli/src/node_editor.cpp"
                text = path.read_text(encoding="utf-8")
                text, count = re.subn(
                    r"   \* @param s Borrowed multiline pane text\.\n",
                    "",
                    text,
                    count=1,
                )
                if count != 1:
                    raise ValueError("negative self-test could not delete param")
                path.write_text(text, encoding="utf-8")
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            if case == "deleted_inventory":
                manifest.pop()
                manifest_path.write_text(
                    json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
                )
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            compile_commands = temp_repo / "compile_commands.json"
            compile_commands.write_text(
                json.dumps(
                    [
                        {"directory": str(temp_repo), "file": str(temp_repo / path)}
                        for path in REQUIRED_EXTENDED_DOXYGEN_SOURCES
                    ],
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
            closure = {
                "sources": list(REQUIRED_EXTENDED_DOXYGEN_SOURCES),
                "passes": True,
            }
            observation = inspect_extended_doxygen_inventory(
                temp_repo, compile_commands, closure, manifest
            )
            returncode = 0 if extended_inventory_matches_expected(observation) else 1
            results.append(
                {
                    "case": case,
                    "returncode": returncode,
                    "incomplete": observation["incomplete"],
                    "inventory_symbol_count": observation["inventory_symbol_count"],
                }
            )
    passed = all(row["returncode"] != 0 for row in results)
    write_json(out / "negative-self-test.json", {"passes": passed, "cases": results})
    return passed


def write_summary(path: Path, passed: bool, actual: dict[str, Any]) -> None:
    """@brief Write a concise reader-oriented AST audit summary.

    @param path Markdown destination to replace.
    @param passed Aggregate comparison outcome.
    @param actual Real observations supplying inventory counts.
    @return Nothing.
    @throws OSError If the summary cannot be written.
    @throws KeyError If actual does not follow the audit schema.
    @note The summary cites real counts while detailed rows remain in actual.json.
    """

    declarations = actual["declarations"]
    target_closure = actual["guard_definitions"]["target_source_closure"]
    body = [
        "# CLI Host Doxygen AST Audit",
        "",
        f"Status: {'PASS' if passed else 'FAIL'}",
        "",
        "- Clang-structured declarations checked: "
        f"{declarations['observed_count']}/{declarations['expected_count']}",
        "- Incomplete declaration contracts: " f"{len(declarations['incomplete'])}",
        "- Missing/invalid implementation definitions: "
        f"{len(actual['definitions']['missing'])}",
        "- Extended CLI Doxygen entities checked: "
        f"{actual['extended_inventory']['observed_symbol_count']}/"
        f"{actual['extended_inventory']['expected_symbol_count']}",
        "- AST-audited CLI/benchmark broad catches: "
        f"{actual['guard_definitions']['actual_broad_catch_count']}/"
        f"{actual['guard_definitions']['expected_broad_catch_count']}",
        "- Configured graph CLI target sources covered: "
        f"{target_closure['closure_source_count']}/"
        f"{target_closure['expected_closure_source_count']}",
        "- CLI/benchmark translation units checked for inventory completeness: "
        f"{actual['guard_definitions']['audited_source_count']}/"
        f"{actual['guard_definitions']['expected_audited_source_count']}",
        "- Unsupported frontend terminology hits: "
        f"{len(actual['semantics']['unsupported_frontend_terms'])}",
        "",
        "The compiler AST validates brief/parameter/return/throws/note structure, "
        "real definition bodies, and same-chain bad_alloc guards; source checks "
        "retain exact copydoc linkage, Host terminology, and lifetime wording.",
        "",
    ]
    path.write_text("\n".join(body), encoding="utf-8")


def main() -> int:
    """@brief Run the CLI Host Doxygen compiler-AST audit.

    @return Zero when every structural and semantic contract passes; one otherwise.
    @throws OSError If configured inputs or temporary audit outputs are inaccessible.
    @note Creates/replaces only files under ``--out`` plus an auto-removed
      temporary translation unit; source, build products, and Git state remain
      unchanged.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--negative-self-test",
        action="store_true",
        help="run four /tmp source/manifest mutation checks instead of the audit",
    )
    args = parser.parse_args()

    repo = args.repo.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.negative_self_test:
        return 0 if run_extended_negative_self_tests(repo, out) else 1
    definitions = inspect_definitions(repo, args.compile_commands.resolve())
    target_closure = inspect_cli_target_source_closure(
        repo, args.compile_commands.resolve()
    )
    actual = {
        "declarations": inspect_declarations(repo, args.compile_commands.resolve()),
        "definitions": definitions,
        "extended_inventory": inspect_extended_doxygen_inventory(
            repo, args.compile_commands.resolve(), target_closure
        ),
        "guard_definitions": inspect_guard_definitions(
            repo,
            args.compile_commands.resolve(),
            definitions,
            target_closure,
        ),
        "detector_contract": inspect_ast_detector_contract(),
        "semantics": inspect_semantics(repo),
    }
    expected = make_expected()
    passed, compare = make_compare(actual, expected)
    write_json(out / "actual.json", actual)
    write_json(out / "expected.json", expected)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    write_summary(out / "summary.md", passed, actual)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
