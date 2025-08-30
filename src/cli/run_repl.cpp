// FILE: src/cli/run_repl.cpp
#include <iostream>
#include <vector>
#include <string>
#include <functional>

#include "cli/run_repl.hpp"
#include "cli/terminal_input.hpp"
#include "cli/cli_history.hpp"
#include "cli/cli_autocompleter.hpp"
#include "input_match_state.hpp"
#include "cli/process_command.hpp"

void run_repl(ps::InteractionService& svc, CliConfig& config, const std::string& initial_graph) {
    bool modified = false;
    std::string current_graph = initial_graph;

    ps::CliHistory history;
    history.SetMaxSize(config.history_size);
    ps::CliAutocompleter completer(svc);
    if (!current_graph.empty()) completer.SetCurrentGraph(current_graph);

    struct CompletionState {
        std::vector<std::string> options;
        int current_index = -1;
        size_t original_cursor_pos = 0;
        std::string original_prefix;
        void Reset() { options.clear(); current_index = -1; }
        bool IsActive() const { return current_index != -1; }
    } completion_state;

    std::string line_buffer;
    int cursor_pos = 0;
    ps::InputMatchState history_match_state;

    std::function<void(const std::vector<std::string>&)> redraw_line_impl;
    auto redraw_line = [&](const std::vector<std::string>& options_to_display = {}) {
        redraw_line_impl(options_to_display);
    };

    redraw_line_impl = [&](const std::vector<std::string>&) {
        std::cout << "\r\x1B[K" << "ps> ";
        if (completion_state.IsActive()) {
            size_t start_idx = 0;
            if (completion_state.original_cursor_pos >= completion_state.original_prefix.length())
                start_idx = completion_state.original_cursor_pos - completion_state.original_prefix.length();
            size_t left_len = std::min(start_idx, line_buffer.size());
            size_t mid_len  = (cursor_pos > (int)start_idx && (size_t)cursor_pos <= line_buffer.size())
                              ? (size_t)cursor_pos - start_idx
                              : (line_buffer.size() > start_idx ? line_buffer.size() - start_idx : 0);
            std::cout << line_buffer.substr(0, left_len)
                      << "\x1B[7m"
                      << line_buffer.substr(left_len, mid_len)
                      << "\x1B[0m"
                      << line_buffer.substr(left_len + mid_len)
                      << std::flush;
        } else {
            std::cout << line_buffer << std::flush;
        }
        std::cout << "\r\x1B[" << (4 + cursor_pos) << "C" << std::flush;
    };

    std::cout << "Photospider dynamic graph shell (decoupled). Type 'help' for commands.\n";
    std::cout << "History file: " << history.Path() << "\n";
    ps::TerminalInput term_input;
    redraw_line();

    while (true) {
        int key = term_input.GetChar();
        if (key != ps::TAB) {
            completion_state.Reset();
        }
        auto reset_history_match = [&](){ history_match_state.Reset(); };
        switch (key) {
            case ps::ENTER: {
                term_input.Restore();
                std::cout << "\r\n";
                if (!line_buffer.empty()) {
                    history.Add(line_buffer);
                    history.Save();
                }
                bool continue_repl = process_command(line_buffer, svc, current_graph, modified, config);
                completer.SetCurrentGraph(current_graph);
                term_input.SetRaw();
                if (!continue_repl) {
                    return;
                }
                line_buffer.clear();
                cursor_pos = 0;
                history.ResetNavigation();
                history_match_state.Reset();
                redraw_line();
                break;
            }
            case ps::CTRL_C: {
                if (line_buffer.empty()) {
                    std::cout << "\r\n(To exit, type 'exit' or press Ctrl+C again on an empty line)\r\n";
                    redraw_line();
                    key = term_input.GetChar();
                    if(key == ps::CTRL_C) {
                        std::cout << "\r\nExiting." << std::endl;
                        return;
                    }
                }
                line_buffer.clear();
                cursor_pos = 0;
                history.ResetNavigation();
                history_match_state.Reset();
                redraw_line();
                break;
            }
            case ps::BACKSPACE: {
                if (cursor_pos > 0) {
                    line_buffer.erase(cursor_pos - 1, 1);
                    cursor_pos--;
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case ps::DEL: {
                 if (cursor_pos < (int)line_buffer.length()) {
                    line_buffer.erase(cursor_pos, 1);
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case ps::UP: {
                if (!history_match_state.active) {
                    history_match_state.Begin(line_buffer.substr(0, cursor_pos), cursor_pos);
                }
                const std::string& sticky = history_match_state.original_prefix;
                line_buffer = history.GetPrevious(sticky);
                cursor_pos = line_buffer.length();
                redraw_line();
                break;
            }
            case ps::DOWN: {
                if (!history_match_state.active) {
                    history_match_state.Begin(line_buffer.substr(0, cursor_pos), cursor_pos);
                }
                const std::string& sticky = history_match_state.original_prefix;
                line_buffer = history.GetNext(sticky);
                cursor_pos = line_buffer.length();
                redraw_line();
                break;
            }
            case ps::LEFT: {
                if (cursor_pos > 0) {
                    cursor_pos--;
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case ps::RIGHT: {
                if (cursor_pos < (int)line_buffer.length()) {
                    cursor_pos++;
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case ps::TAB: {
                completer.SetCurrentGraph(current_graph);
                if (completion_state.IsActive()) {
                    completion_state.current_index = (completion_state.current_index + 1) % completion_state.options.size();
                    size_t token_start = completion_state.original_cursor_pos - completion_state.original_prefix.length();
                    line_buffer.erase(token_start, line_buffer.size() - token_start);
                    const std::string& opt = completion_state.options[completion_state.current_index];
                    line_buffer.insert(token_start, opt);
                    cursor_pos = (int)(token_start + opt.size());
                    redraw_line(completion_state.options);
                } else {
                    auto result = completer.Complete(line_buffer, cursor_pos);
                    if (result.options.empty()) break;
                    completion_state.options = result.options;
                    completion_state.current_index = 0;
                    size_t start = line_buffer.find_last_of(" \t", cursor_pos ? cursor_pos-1 : 0);
                    start = (start==std::string::npos)?0:start+1;
                    completion_state.original_prefix = line_buffer.substr(start, cursor_pos - (int)start);
                    completion_state.original_cursor_pos = cursor_pos;
                    line_buffer = result.new_line;
                    cursor_pos = result.new_cursor_pos;
                    redraw_line(completion_state.options);
                }
                break; }
            case ps::UNKNOWN:
                break;
            default: {
                if(key >= 32 && key <= 126) {
                    line_buffer.insert(cursor_pos, 1, static_cast<char>(key));
                    cursor_pos++;
                    history.ResetNavigation();
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
        }
    }
}
