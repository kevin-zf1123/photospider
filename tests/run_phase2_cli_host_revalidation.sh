#!/usr/bin/env bash
set -euo pipefail

OUT="${1:-tests/results/codebase-refactor/phase-2-cli-host}"
mkdir -p "$OUT"

BOUNDARY_PATTERN='InteractionService|Kernel::|svc\.kernel\(|GraphModel|GraphRuntime|kernel/interaction|kernel/kernel|graph_model.hpp|cmd_'
BOUNDARY_PATHS=(
  cli/graph_cli.cpp
  include/cli
  src/cli
  include/benchmark
  src/benchmark
)

set +e
rg -n "$BOUNDARY_PATTERN" "${BOUNDARY_PATHS[@]}" >"$OUT/cli_boundary_rg.log"
RG_RC=$?
set -e
if [[ "$RG_RC" -eq 0 ]]; then
  BOUNDARY_OK=0
elif [[ "$RG_RC" -eq 1 ]]; then
  BOUNDARY_OK=1
else
  echo "rg failed with code $RG_RC" >>"$OUT/cli_boundary_rg.log"
  BOUNDARY_OK=0
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  >"$OUT/cmake_configure.log" 2>&1
cmake --build build --target graph_cli -j \
  >"$OUT/build_graph_cli.log" 2>&1

ctest --output-on-failure --test-dir build \
  -R 'Phase2(CliHost|HostAdapter)Scan|PublicHeaderSelfContainment' \
  >"$OUT/ctest_boundary.log" 2>&1

cmake --build build --target test_host_adapter -j \
  >"$OUT/build_test_host_adapter.log" 2>&1
./build/tests/test_host_adapter >"$OUT/test_host_adapter.log" 2>&1

cmake --build build --target test_cli_dirty_snapshot_formatter -j \
  >"$OUT/build_test_cli_dirty_snapshot_formatter.log" 2>&1
./build/tests/test_cli_dirty_snapshot_formatter \
  >"$OUT/test_cli_dirty_snapshot_formatter.log" 2>&1

cat >"$OUT/repl_commands.txt" <<'CMDS'
load phase2_cli_host util/testcases/propagation_linear_test.yaml
compute all parallel nosave m
inspect dirty
exit
n
CMDS

mkdir -p "$OUT/home"
HOME="$PWD/$OUT/home" ./build/bin/graph_cli --repl \
  <"$OUT/repl_commands.txt" \
  >"$OUT/graph_cli_repl_stdout.log" \
  2>"$OUT/graph_cli_repl_stderr.log"
REPL_RC=$?

python3 - "$OUT" "$BOUNDARY_OK" "$REPL_RC" <<'PY'
import json
import re
import sys
from pathlib import Path

out = Path(sys.argv[1])
boundary_ok = sys.argv[2] == "1"
repl_rc = int(sys.argv[3])

stdout = (out / "graph_cli_repl_stdout.log").read_text(encoding="utf-8", errors="replace")
stderr = (out / "graph_cli_repl_stderr.log").read_text(encoding="utf-8", errors="replace")
ctest = (out / "ctest_boundary.log").read_text(encoding="utf-8", errors="replace")
host_test = (out / "test_host_adapter.log").read_text(encoding="utf-8", errors="replace")
formatter_test = (out / "test_cli_dirty_snapshot_formatter.log").read_text(
    encoding="utf-8", errors="replace"
)

ansi_re = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
clean_stdout = ansi_re.sub("", stdout).replace("\r", "")
(out / "graph_cli_repl_stdout.clean.log").write_text(clean_stdout, encoding="utf-8")

actual = {
    "cli_boundary_rg_passed": boundary_ok,
    "ctest_boundary_passed": "100% tests passed" in ctest,
    "host_adapter_tests_passed": "[  PASSED  ] 11 tests." in host_test,
    "dirty_snapshot_formatter_tests_passed": "[  PASSED  ] 1 test." in formatter_test,
    "graph_cli_repl_returncode": repl_rc,
    "graph_cli_loaded_session": "Loaded session 'phase2_cli_host'" in clean_stdout,
    "graph_cli_compute_finished": "Computation finished." in clean_stdout,
    "graph_cli_inspect_dirty_ran": "inspect dirty" in clean_stdout,
    "graph_cli_inspect_dirty_no_snapshot": "(No dirty snapshot recorded.)" in clean_stdout,
    "graph_cli_stderr_warnings": [line for line in stderr.splitlines() if line.strip()],
}
expected = {
    "cli_boundary_rg_passed": True,
    "ctest_boundary_passed": True,
    "host_adapter_tests_passed": True,
    "dirty_snapshot_formatter_tests_passed": True,
    "graph_cli_repl_returncode": 0,
    "graph_cli_loaded_session": True,
    "graph_cli_compute_finished": True,
    "graph_cli_inspect_dirty_ran": True,
    "graph_cli_inspect_dirty_no_snapshot": True,
}
checks = {
    key: actual.get(key) == value for key, value in expected.items()
}
overall = all(checks.values())
(out / "actual.json").write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n", encoding="utf-8")
(out / "expected.json").write_text(json.dumps(expected, indent=2, sort_keys=True) + "\n", encoding="utf-8")
(out / "compare.log").write_text(
    "\n".join(
        ["phase2_cli_host_revalidation"]
        + [f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()]
        + [f"overall={'PASS' if overall else 'FAIL'}", ""]
    ),
    encoding="utf-8",
)
(out / "summary.md").write_text(
    "\n".join(
        [
            "# codebase-refactor phase-2 CLI Host revalidation",
            "",
            "## Test objective",
            "",
            "Verify GitHub issue #30: graph_cli defaults to embedded Host mode,",
            "CLI/common boundaries avoid direct kernel implementation includes,",
            "and the scriptable load/compute/inspect sanity path keeps existing",
            "behavior.",
            "",
            "## Commands",
            "",
            "- `rg -n \"$BOUNDARY_PATTERN\" cli/graph_cli.cpp include/cli src/cli include/benchmark src/benchmark`",
            "- `cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo`",
            "- `cmake --build build --target graph_cli -j`",
            "- `ctest --output-on-failure --test-dir build -R 'Phase2(CliHost|HostAdapter)Scan|PublicHeaderSelfContainment'`",
            "- `cmake --build build --target test_host_adapter -j`",
            "- `./build/tests/test_host_adapter`",
            "- `cmake --build build --target test_cli_dirty_snapshot_formatter -j`",
            "- `./build/tests/test_cli_dirty_snapshot_formatter`",
            "- `HOME=$OUT/home ./build/bin/graph_cli --repl < $OUT/repl_commands.txt`",
            "",
            "## Evidence files",
            "",
            "- `cli_boundary_rg.log`",
            "- `build_graph_cli.log`",
            "- `ctest_boundary.log`",
            "- `test_host_adapter.log`",
            "- `test_cli_dirty_snapshot_formatter.log`",
            "- `graph_cli_repl_stdout.clean.log`",
            "- `graph_cli_repl_stderr.log`",
            "- `expected.json`, `actual.json`, `compare.log`",
            "",
            "## Result",
            "",
            f"- CLI boundary scan passed: {actual['cli_boundary_rg_passed']}",
            f"- Boundary CTest passed: {actual['ctest_boundary_passed']}",
            f"- Host adapter tests passed: {actual['host_adapter_tests_passed']}",
            "- Dirty snapshot formatter tests passed: "
            f"{actual['dirty_snapshot_formatter_tests_passed']}",
            f"- REPL returned 0: {actual['graph_cli_repl_returncode'] == 0}",
            f"- Loaded session observed: {actual['graph_cli_loaded_session']}",
            f"- Compute finished observed: {actual['graph_cli_compute_finished']}",
            f"- Inspect dirty command observed: {actual['graph_cli_inspect_dirty_ran']}",
            "- Inspect dirty no-snapshot output observed: "
            f"{actual['graph_cli_inspect_dirty_no_snapshot']}",
            f"- Overall: {'PASS' if overall else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "The boundary scan proves the CLI entrypoint and common/frontend",
            "helpers no longer name InteractionService, Kernel request types,",
            "GraphModel, GraphRuntime, direct kernel includes, graph_model.hpp,",
            "or old cmd_* calls. The REPL transcript proves `load ...`,",
            "`compute all parallel nosave m`, and `inspect dirty` still complete",
            "through the default embedded Host path. The Host adapter and formatter",
            "tests prove `inspect dirty` can still display monolithic dirty-region",
            "and edge-mapping diagnostics after the CLI Host migration.",
        ]
    ) + "\n",
    encoding="utf-8",
)
if not overall:
    sys.exit(1)
PY
