#pragma once

#include <string>
#include <vector>

namespace ps {
class Host;
}

namespace ps {

/**
 * @brief Result of one context-aware CLI completion request.
 *
 * The value preserves every matching candidate and, when candidates share a
 * prefix, carries the line and cursor position after applying that prefix.
 *
 * @throws std::bad_alloc if copying or mutating the stored strings or vector
 *         exhausts memory.
 * @note This is an owned value. It does not retain references to the input
 *       line, the completer, or the Host.
 */
struct CompletionResult {
  /** @brief Matching completion candidates in provider-defined order. */
  std::vector<std::string> options;

  /** @brief Input line with the common completion prefix applied. */
  std::string new_line;

  /** @brief Cursor byte offset in `new_line` after completion. */
  int new_cursor_pos;
};

/**
 * @brief Provides context-aware completion for the interactive graph CLI.
 *
 * Completion first tokenizes the text before the cursor, selects a provider
 * for the active command and argument, and then applies the longest common
 * prefix. Dynamic graph, node, policy, and execution candidates are queried
 * only
 * through the borrowed public Host boundary.
 *
 * @throws std::bad_alloc if command inventory, token, candidate, path, or
 *         result allocation exhausts memory.
 * @note The completer owns its command and graph-name strings but not `svc_`.
 *       The referenced Host must outlive the completer. The type is not
 *       thread-safe; use it from the interactive input thread.
 */
class CliAutocompleter {
 public:
  /**
   * @brief Constructs a completer bound to a public Host.
   *
   * The constructor copies and sorts the fixed command inventory. Dynamic
   * candidates are fetched lazily during `Complete`.
   *
   * @param svc Borrowed Host used for graph, node, policy, and route
   * candidates.
   * @throws std::bad_alloc if command inventory construction exhausts memory.
   * @note `svc` must outlive this object and is never owned or synchronized by
   *       the completer.
   */
  explicit CliAutocompleter(ps::Host& svc);

  /**
   * @brief Selects the graph used for dynamic node-id completion.
   *
   * @param graph_name Opaque graph session name copied into the completer.
   * @return Nothing.
   * @throws std::bad_alloc if copying the graph name exhausts memory.
   * @note The name is not validated and no Host call occurs. Call only from
   *       the same thread that invokes `Complete`.
   */
  void SetCurrentGraph(const std::string& graph_name) {
    current_graph_ = graph_name;
  }

  /**
   * @brief Completes the command token or active command argument.
   *
   * The method tokenizes `line` through `cursor_pos`, gathers matching fixed,
   * filesystem, or Host-backed candidates, and replaces the active token with
   * their longest common prefix. A unique non-directory candidate receives a
   * trailing space.
   *
   * @param line Complete editable command line.
   * @param cursor_pos Cursor byte offset in the inclusive range from zero to
   *        `line.size()`.
   * @return Owned candidates, edited line, and resulting cursor offset. When
   *         no common prefix exists, the line and cursor remain unchanged.
   * @throws std::bad_alloc if token, candidate, Host result, path, or output
   *         construction exhausts memory.
   * @note Recoverable Host and ordinary filesystem failures are best-effort:
   *       the affected dynamic candidates are omitted. The caller must honor
   *       the cursor range precondition. No Host value is retained.
   */
  CompletionResult Complete(const std::string& line, int cursor_pos);

 private:
  /**
   * @brief Splits a command prefix into whitespace-delimited tokens.
   *
   * @param line Command prefix ending at the active cursor.
   * @return Owned tokens in input order.
   * @throws std::bad_alloc if stream or token storage exhausts memory.
   * @note Quoting and escaping are intentionally not interpreted; this mirrors
   *       the existing lightweight completion grammar.
   */
  std::vector<std::string> Tokenize(const std::string& line) const;

  /**
   * @brief Finds the bytewise longest prefix shared by all candidates.
   *
   * @param options Candidate strings to compare.
   * @return Shared prefix, the sole candidate, or an empty string when the
   *         input is empty or has no common prefix.
   * @throws std::bad_alloc if copying the working prefix exhausts memory.
   * @note The method does not normalize case or Unicode and does not mutate
   *       `options`.
   */
  std::string FindLongestCommonPrefix(
      const std::vector<std::string>& options) const;

  /**
   * @brief Appends fixed command names matching a prefix.
   * @param prefix Required bytewise command prefix.
   * @param options Mutable destination receiving matching command names.
   * @return Nothing.
   * @throws std::bad_alloc if appending candidates exhausts memory.
   * @note No Host or filesystem state is accessed.
   */
  void CompleteCommand(const std::string& prefix,
                       std::vector<std::string>& options) const;

  /**
   * @brief Appends filesystem path candidates matching a prefix.
   * @param prefix Path prefix, optionally including directory components.
   * @param options Mutable destination receiving files and slash-terminated
   *        directories.
   * @return Nothing.
   * @throws std::bad_alloc if path or candidate allocation exhausts memory.
   * @note Ordinary filesystem errors are best-effort and produce no candidates
   *       for the failing directory. No Host state is accessed.
   */
  void CompletePath(const std::string& prefix,
                    std::vector<std::string>& options) const;

  /**
   * @brief Appends YAML files and traversable directories matching a prefix.
   * @param prefix Path prefix, optionally including directory components.
   * @param options Mutable destination receiving `.yaml`, `.yml`, and
   *        slash-terminated directory candidates.
   * @return Nothing.
   * @throws std::bad_alloc if path, filtering, or candidate allocation
   *         exhausts memory.
   * @note Extension matching is case-sensitive. Ordinary filesystem errors are
   *       best-effort and no Host state is accessed.
   */
  void CompleteYamlPath(const std::string& prefix,
                        std::vector<std::string>& options) const;

  /**
   * @brief Appends the `all` token and matching ids from the current graph.
   * @param prefix Required bytewise node-id prefix.
   * @param options Mutable destination receiving matching candidates.
   * @return Nothing.
   * @throws std::bad_alloc if Host result or candidate construction exhausts
   *         memory.
   * @note An unset graph or recoverable Host failure omits dynamic ids. The
   *       Host result is copied for the duration of this call and not retained.
   */
  void CompleteNodeId(const std::string& prefix,
                      std::vector<std::string>& options) const;

  /**
   * @brief Appends node-id or print-mode candidates for the active argument.
   * @param prefix Required bytewise argument prefix.
   * @param options Mutable destination receiving matching candidates.
   * @param only_mode_args If true, offer only print modes; otherwise delegate
   *        to node-id completion.
   * @return Nothing.
   * @throws std::bad_alloc if Host result or candidate construction exhausts
   *         memory.
   * @note Recoverable Host failure affects only delegated node-id candidates.
   */
  void CompletePrintArgs(const std::string& prefix,
                         std::vector<std::string>& options,
                         bool only_mode_args) const;

  /**
   * @brief Appends fixed traversal display and cache-check flags.
   * @param prefix Required bytewise argument prefix.
   * @param options Mutable destination receiving matching flags.
   * @return Nothing.
   * @throws std::bad_alloc if fixed candidate storage or appending exhausts
   *         memory.
   * @note No Host or filesystem state is accessed.
   */
  void CompleteTraversalArgs(const std::string& prefix,
                             std::vector<std::string>& options) const;

  /**
   * @brief Appends fixed compute-mode flags.
   * @param prefix Required bytewise argument prefix.
   * @param options Mutable destination receiving matching flags.
   * @return Nothing.
   * @throws std::bad_alloc if fixed candidate storage or appending exhausts
   *         memory.
   * @note Node-id completion is handled separately for the first argument; no
   *       Host state is accessed here.
   */
  void CompleteComputeArgs(const std::string& prefix,
                           std::vector<std::string>& options) const;

  /**
   * @brief Appends loaded graph names matching a prefix.
   * @param prefix Required bytewise graph-name prefix.
   * @param options Mutable destination receiving matching graph names.
   * @return Nothing.
   * @throws std::bad_alloc if the Host result or candidate appending exhausts
   *         memory.
   * @note A recoverable Host failure is best-effort and contributes no graph
   *       names. Returned Host values are copied and not retained.
   */
  void CompleteGraphName(const std::string& prefix,
                         std::vector<std::string>& options) const;

  /**
   * @brief Appends fixed operation-listing mode names and aliases.
   * @param prefix Required bytewise mode prefix.
   * @param options Mutable destination receiving matching modes.
   * @return Nothing.
   * @throws std::bad_alloc if fixed candidate storage or appending exhausts
   *         memory.
   * @note The provider does not load plugins or query Host operation state.
   */
  void CompleteOpsMode(const std::string& prefix,
                       std::vector<std::string>& options) const;

  /**
   * @brief Completes session names by scanning local session directories.
   *
   * @param prefix Required name prefix.
   * @param options Mutable destination receiving matching directory names.
   * @return Nothing.
   * @throws std::bad_alloc if path, directory-entry, or result storage cannot
   * allocate.
   * @note Ordinary filesystem failures are ignored because completion is
   * best-effort; no Host or graph state is accessed.
   */
  void CompleteSessionName(const std::string& prefix,
                           std::vector<std::string>& options) const;

  /**
   * @brief Borrowed public Host used for dynamic completion candidates.
   * @note The reference is immutable as an ownership relation but permits Host
   *       calls; its referent must outlive the completer and is not
   * synchronized.
   */
  ps::Host& svc_;

  /**
   * @brief Opaque graph name used by node-id completion.
   * @note Empty means that dynamic node ids are unavailable.
   */
  std::string current_graph_;

  /**
   * @brief Sorted fixed command names and aliases offered at token zero.
   * @note The inventory is constructed once and is not mutated afterward.
   */
  std::vector<std::string> commands_;
};

}  // namespace ps
