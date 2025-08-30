// FILE: cli/graph_cli.cpp
#include <optional>
#include <getopt.h>
#include <limits>
#include <regex>
#include <sstream>
#include <iostream>
#include <fstream>
#include <unordered_set>
#include <chrono>
#include <algorithm>
#include <map>
#include "node_graph.hpp"
#include "../src/ops.hpp"
#include <filesystem>
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "tui_editor.hpp"
#include "ftxui/dom/node.hpp"
#include <opencv2/core/ocl.hpp>

#include "terminal_input.hpp"
#include "cli_history.hpp"
#include "cli_autocompleter.hpp"
#include "path_complete.hpp"
#include "input_match_state.hpp"
#include "path_complete.hpp"
#include "cli_config.hpp"
#include "plugin_loader.hpp"
#include "kernel/kernel.hpp"
#include "kernel/interaction.hpp"
#include "cli/node_editor.hpp"
#include "cli/node_editor_full.hpp"

using namespace ps;
using namespace ftxui;

// Front-end config editor extracted
#include "cli/config_editor.hpp"
// Kernel interaction layer
#include "kernel/kernel.hpp"
#include "kernel/interaction.hpp"

// --- removed inline ConfigEditor implementation (moved to src/cli/config_editor.cpp) ---
/* class ConfigEditor : public TuiEditor {
private:
    struct EditableLine {
        std::string label;
        std::string* value_ptr; 
        bool is_radio = false;
        std::vector<std::string>* radio_options = nullptr;
        int* radio_selected_idx = nullptr;
        std::function<void()> add_fn = nullptr;
        std::function<void()> del_fn = nullptr;
        bool is_toggle = false;
        bool* toggle_ptr = nullptr;
    };

public:
    bool changes_applied = false;

    ConfigEditor(ScreenInteractive& screen, CliConfig& config)
        : TuiEditor(screen), original_config_(config), temp_config_(config) 
    {
        InputOption opt;
        opt.on_enter = [this] { CommitEdit(); };
        editor_input_ = Input(&edit_buffer_, "...", opt);
        radio_editor_ = Radiobox(&dummy_radio_options_, &dummy_radio_selected_idx_);
        SyncModelToUiState();
        RebuildLineView();
    }

    void Run() override {
        auto main_container = Container::Vertical({});
        auto component = Renderer(main_container, [&] {
            Elements line_elements;
            for(size_t i = 0; i < editable_lines_.size(); ++i) {
                auto& line = editable_lines_[i];
                Element display_element;

                if (mode_ == Mode::Edit && selected_ == (int)i) {
                    if (line.is_radio) {
                        display_element = radio_editor_->Render();
                    } else if (line.is_toggle && line.toggle_ptr) {
                        display_element = text((*line.toggle_ptr) ? "[x]" : "[ ]");
                    } else {
                        display_element = editor_input_->Render();
                    }
                } else {
                     if (line.is_radio) {
                        display_element = text((*line.radio_options)[*line.radio_selected_idx]);
                     } else if (line.is_toggle && line.toggle_ptr) {
                        display_element = text((*line.toggle_ptr) ? "[x]" : "[ ]");
                     } else if (line.value_ptr) {
                        display_element = text(*line.value_ptr);
                     } else {
                        display_element = text("...") | dim;
                     }
                }

                auto content = hbox({
                    text(line.label) | size(WIDTH, EQUAL, 28),
                    display_element | flex,
                });

                if (selected_ == (int)i) content |= inverted;
                line_elements.push_back(content);
            }
            
            return vbox({
                text("Interactive Configuration Editor") | bold | hcenter,
                separator(),
                vbox(std::move(line_elements)) | vscroll_indicator | frame | flex,
                separator(),
                RenderStatusBar()
            });
        });

        component |= CatchEvent([&](Event event) {
            if (mode_ == Mode::Command) {
                 if (event == Event::Return) { ExecuteCommand(); return true; }
                 if (event == Event::Escape) { mode_ = Mode::Navigate; command_buffer_.clear(); return true; }
                 if (event.is_character()) { command_buffer_ += event.character(); return true; }
                 if (event == Event::Backspace && !command_buffer_.empty()) { command_buffer_.pop_back(); return true; }
                 return false;
            }
            
            if (mode_ == Mode::Edit) {
                if (event == Event::Escape) { mode_ = Mode::Navigate; return true; }
                if (event == Event::Return) { CommitEdit(); return true; }
                if (editable_lines_[selected_].is_radio) { return radio_editor_->OnEvent(event); } 
                else {
                    // Add basic path completion on Tab for path-like fields.
                    auto is_path_like = [&](){
                        if (selected_ >= (int)editable_lines_.size()) return false;
                        const auto& lbl = editable_lines_[selected_].label;
                        if (lbl == "active_config_file" || lbl == "cache_root_dir" ||
                            lbl == "default_exit_save_path" || lbl == "default_timer_log_path") return true;
                        if (lbl.rfind("plugin_dirs[", 0) == 0) return true;
                        return false;
                    };
                    if (event == Event::Tab && is_path_like()) {
                        auto options = PathCompleteOptions(edit_buffer_);
                        if (!options.empty()) {
                            auto lcp = LongestCommonPrefix(options);
                            if (!lcp.empty()) {
                                edit_buffer_ = lcp;
                                return true;
                            }
                        }
                    }
                    return editor_input_->OnEvent(event);
                }
            }
            
            if (event == Event::ArrowUp) { selected_ = std::max(0, selected_ - 1); return true; }
            if (event == Event::ArrowDown) { selected_ = std::min(int(editable_lines_.size() - 1), selected_ + 1); return true; }
            if (event == Event::Character('e')) { EnterEditMode(); return true; }
            if (event == Event::Character('a')) {
                if(selected_ < (int)editable_lines_.size() && editable_lines_[selected_].add_fn) editable_lines_[selected_].add_fn();
                return true;
            }
            if (event == Event::Character('d')) {
                if(selected_ < (int)editable_lines_.size() && editable_lines_[selected_].del_fn) editable_lines_[selected_].del_fn();
                return true;
            }
            if (event == Event::Character(':')) { mode_ = Mode::Command; command_buffer_.clear(); return true; }
            if (event == Event::Character('q')) { screen_.Exit(); return true; }
            return false;
        });
        screen_.Loop(component);
    }

private:
    void SyncModelToUiState() {
        auto find_idx = [](const auto& vec, const auto& val) {
            auto it = std::find(vec.begin(), vec.end(), val);
            return it == vec.end() ? 0 : std::distance(vec.begin(), it);
        };
        active_config_path_buffer_ = temp_config_.loaded_config_path;
        history_size_buffer_ = std::to_string(temp_config_.history_size);
        selected_cache_precision_idx_ = find_idx(cache_precision_entries_, temp_config_.cache_precision);
        // Back-compat mapping: detailed/d -> full; s -> simplified
        if (temp_config_.default_print_mode == "detailed" || temp_config_.default_print_mode == "d") temp_config_.default_print_mode = "full";
        if (temp_config_.default_print_mode == "s") temp_config_.default_print_mode = "simplified";
        selected_print_mode_idx_ = find_idx(print_mode_entries_, temp_config_.default_print_mode);
        selected_ops_list_mode_idx_ = find_idx(ops_list_mode_entries_, temp_config_.default_ops_list_mode);
        selected_path_mode_idx_ = find_idx(path_mode_entries_, temp_config_.ops_plugin_path_mode);
        selected_cache_clear_idx_ = find_idx(cache_clear_entries_, temp_config_.default_cache_clear_arg);
        selected_exit_sync_idx_ = temp_config_.exit_prompt_sync ? 0 : 1;
        selected_config_save_idx_ = find_idx(config_save_entries_, temp_config_.config_save_behavior);
        selected_editor_save_idx_ = find_idx(editor_save_entries_, temp_config_.editor_save_behavior);
        plugin_dirs_str_ = temp_config_.plugin_dirs;

        // Parse traversal defaults into UI state
        selected_traversal_tree_idx_ = 2; // no_tree by default
        selected_traversal_check_idx_ = 0; // none
        int cache_m = 0, cache_d = 0; // count m/d presence
        {
            std::istringstream iss(temp_config_.default_traversal_arg);
            std::string tok;
            while (iss >> tok) {
                if (tok == "f" || tok == "full" || tok == "d" || tok == "detailed") selected_traversal_tree_idx_ = 0; // legacy d/detailed => full
                else if (tok == "s" || tok == "simplified") selected_traversal_tree_idx_ = 1;
                else if (tok == "n" || tok == "no_tree" || tok == "none") selected_traversal_tree_idx_ = 2;
                else if (tok == "md") { cache_m = 1; cache_d = 1; }
                else if (tok == "m") cache_m = 1;
                else if (tok == "d") cache_d = 1;
                else if (tok == "c") selected_traversal_check_idx_ = 1;
                else if (tok == "cr") selected_traversal_check_idx_ = 2;
            }
        }
        selected_traversal_cache_idx_ = (cache_m && cache_d) ? 1 : (cache_m ? 0 : (cache_d ? 2 : 1));

        // Parse compute defaults into UI state
        compute_force_ = compute_force_deep_ = compute_parallel_ = compute_timer_console_ = compute_timer_log_ = compute_mute_ = false;
        {
            std::istringstream iss(temp_config_.default_compute_args);
            std::string tok;
            while (iss >> tok) {
                if (tok == "force") compute_force_ = true;
                else if (tok == "force-deep") compute_force_deep_ = true;
                else if (tok == "parallel") compute_parallel_ = true;
                else if (tok == "t" || tok == "-t" || tok == "timer") compute_timer_console_ = true;
                else if (tok == "tl" || tok == "-tl") compute_timer_log_ = true;
                else if (tok == "m" || tok == "-m" || tok == "mute") compute_mute_ = true;
            }
        }
    }

    void SyncUiStateToModel() {
        temp_config_.loaded_config_path = active_config_path_buffer_;
        try { temp_config_.history_size = std::stoi(history_size_buffer_); } catch(...) { temp_config_.history_size = 1000; }
        temp_config_.cache_precision = cache_precision_entries_[selected_cache_precision_idx_];
        temp_config_.default_print_mode = print_mode_entries_[selected_print_mode_idx_];
        temp_config_.default_ops_list_mode = ops_list_mode_entries_[selected_ops_list_mode_idx_];
        temp_config_.ops_plugin_path_mode = path_mode_entries_[selected_path_mode_idx_];
        temp_config_.default_cache_clear_arg = cache_clear_entries_[selected_cache_clear_idx_];
        temp_config_.exit_prompt_sync = (selected_exit_sync_idx_ == 0);
        temp_config_.config_save_behavior = config_save_entries_[selected_config_save_idx_];
        temp_config_.editor_save_behavior = editor_save_entries_[selected_editor_save_idx_];
        temp_config_.plugin_dirs = plugin_dirs_str_;

        // Compose traversal defaults from UI state
        {
            std::vector<std::string> parts;
            if (selected_traversal_tree_idx_ == 0) parts.push_back("full");
            else if (selected_traversal_tree_idx_ == 1) parts.push_back("simplified");
            else parts.push_back("n");
            parts.push_back(traversal_cache_entries_[selected_traversal_cache_idx_]); // m/md/d
            if (selected_traversal_check_idx_ == 1) parts.push_back("c");
            else if (selected_traversal_check_idx_ == 2) parts.push_back("cr");
            std::ostringstream oss; bool first = true;
            for (auto& p : parts) { if (!first) oss << ' '; first = false; oss << p; }
            temp_config_.default_traversal_arg = oss.str();
        }

        // Compose compute defaults from UI state
        {
            std::vector<std::string> parts;
            if (compute_force_) parts.push_back("force");
            if (compute_force_deep_) parts.push_back("force-deep");
            if (compute_parallel_) parts.push_back("parallel");
            if (compute_timer_console_) parts.push_back("t");
            if (compute_timer_log_) parts.push_back("tl");
            if (compute_mute_) parts.push_back("m");
            std::ostringstream oss; bool first = true;
            for (auto& p : parts) { if (!first) oss << ' '; first = false; oss << p; }
            temp_config_.default_compute_args = oss.str();
        }
    }

    void RebuildLineView() {
        editable_lines_.clear();
        auto add_text_line = [&](std::string label, std::string* value_ptr) {
            editable_lines_.push_back({std::move(label), value_ptr});
        };
        auto add_radio_line = [&](std::string label, std::vector<std::string>* opts, int* selected_idx) {
            editable_lines_.push_back({std::move(label), nullptr, true, opts, selected_idx});
        };
        auto add_toggle_line = [&](std::string label, bool* value_ptr) {
            editable_lines_.push_back({std::move(label), nullptr, false, nullptr, nullptr, nullptr, nullptr, true, value_ptr});
        };
        
        add_text_line("active_config_file", &active_config_path_buffer_);
        add_text_line("cache_root_dir", &temp_config_.cache_root_dir);
        add_text_line("history_size", &history_size_buffer_);
        add_radio_line("cache_precision", &cache_precision_entries_, &selected_cache_precision_idx_);
        add_text_line("default_exit_save_path", &temp_config_.default_exit_save_path);
        add_text_line("default_timer_log_path", &temp_config_.default_timer_log_path);
        // Traversal defaults (tree mode + cache status flags via single-choice)
        add_radio_line("traversal_tree_mode", &traversal_tree_entries_, &selected_traversal_tree_idx_);
        add_radio_line("traversal_cache_flags", &traversal_cache_entries_, &selected_traversal_cache_idx_); // m / md / d
        add_radio_line("traversal_check", &traversal_check_entries_, &selected_traversal_check_idx_);
        // Compute defaults (multi-select UI)
        add_toggle_line("compute_force", &compute_force_);
        add_toggle_line("compute_force_deep", &compute_force_deep_);
        add_toggle_line("compute_parallel", &compute_parallel_);
        add_toggle_line("compute_timer_console(-t)", &compute_timer_console_);
        add_toggle_line("compute_timer_log(-tl)", &compute_timer_log_);
        add_toggle_line("compute_mute(-m)", &compute_mute_);
        
        add_radio_line("default_print_mode", &print_mode_entries_, &selected_print_mode_idx_);
        add_radio_line("default_ops_list_mode", &ops_list_mode_entries_, &selected_ops_list_mode_idx_);
        add_radio_line("ops_plugin_path_mode", &path_mode_entries_, &selected_path_mode_idx_);
        add_radio_line("default_cache_clear_arg", &cache_clear_entries_, &selected_cache_clear_idx_);
        add_radio_line("exit_prompt_sync", &exit_sync_entries_, &selected_exit_sync_idx_);
        add_radio_line("config_save_behavior", &config_save_entries_, &selected_config_save_idx_);
        add_radio_line("editor_save_behavior", &editor_save_entries_, &selected_editor_save_idx_);

        for (size_t i = 0; i < plugin_dirs_str_.size(); ++i) {
            auto del_fn = [this, i] {
                plugin_dirs_str_.erase(plugin_dirs_str_.begin() + i);
                RebuildLineView();
                selected_ = std::min((int)editable_lines_.size() - 1, selected_);
            };
            editable_lines_.push_back({"plugin_dirs[" + std::to_string(i) + "]", &plugin_dirs_str_[i], false, nullptr, nullptr, nullptr, del_fn});
        }
        
        auto add_fn = [this] {
            plugin_dirs_str_.push_back("<new_path>");
            RebuildLineView();
            selected_ = editable_lines_.size() - 2;
            EnterEditMode();
        };
        editable_lines_.push_back({"(Plugin Dirs)", nullptr, false, nullptr, nullptr, add_fn, nullptr});
    }
    
    void EnterEditMode() {
        if (selected_ >= (int)editable_lines_.size()) return;
        const auto& line = editable_lines_[selected_];
        if (!line.is_radio && !line.value_ptr && !line.is_toggle) return;

        mode_ = Mode::Edit;
        if (line.is_radio) {
            RadioboxOption opt;
            opt.entries = line.radio_options;
            opt.selected = line.radio_selected_idx;
            opt.focused_entry = line.radio_selected_idx;
            radio_editor_ = Radiobox(opt);
            radio_editor_->TakeFocus();
        } else if (line.is_toggle && line.toggle_ptr) {
            // Toggle immediately; no dedicated edit widget.
            *line.toggle_ptr = !*line.toggle_ptr;
            mode_ = Mode::Navigate;
            return;
        } else {
            edit_buffer_ = *line.value_ptr;
            editor_input_->TakeFocus();
        }
    }
    
    void CommitEdit() {
        if (selected_ >= (int)editable_lines_.size()) return;
        const auto& line = editable_lines_[selected_];
        if (!line.is_radio && line.value_ptr) {
            *line.value_ptr = edit_buffer_;
        }
        mode_ = Mode::Navigate;
    }

    Element RenderStatusBar() {
        std::string help;
        if (mode_ == Mode::Navigate) {
            help = "↑/↓:Move | e:Edit | q:Quit | ::Command";
            if (selected_ < (int)editable_lines_.size()) {
                if (editable_lines_[selected_].add_fn) help += " | a:Add";
                if (editable_lines_[selected_].del_fn) help += " | d:Delete";
            }
        }
        else if (mode_ == Mode::Edit) help = "Enter:Accept | Esc:Cancel | ←/→:Change";
        else if (mode_ == Mode::Command) {
            return hbox({ text(":" + command_buffer_), text(" ") | blink, filler(), text("[a]pply | [w]rite | [q]uit | Esc:Cancel") | dim, }) | inverted;
        }
        return hbox({ text(help) | dim, filler(), text(status_message_) | bold });
    }

    void ExecuteCommand() {
        std::string cmd = command_buffer_;
        command_buffer_.clear();
        mode_ = Mode::Navigate;

        if (cmd == "a" || cmd == "apply") {
            std::string new_path = active_config_path_buffer_;
            std::string old_path = original_config_.loaded_config_path;

            if (new_path != old_path) {
                if (fs::exists(new_path)) {
                    load_or_create_config(new_path, original_config_);
                    temp_config_ = original_config_;
                    SyncModelToUiState();
                    RebuildLineView();
                    status_message_ = "Loaded config from: " + new_path;
                } else {
                    SyncUiStateToModel();
                    temp_config_.loaded_config_path = fs::absolute(new_path).string();
                    original_config_ = temp_config_;
                    if (write_config_to_file(original_config_, new_path)) { status_message_ = "Created and applied new config: " + new_path; } 
                    else { status_message_ = "Error creating file: " + new_path; original_config_.loaded_config_path = old_path; }
                    temp_config_ = original_config_;
                    SyncModelToUiState();
                    RebuildLineView();
                }
            } else {
                SyncUiStateToModel();
                original_config_ = temp_config_;
                status_message_ = "Settings applied to current session.";
                // Keep UI in sync with any composed fields.
                SyncModelToUiState();
                RebuildLineView();
            }
            changes_applied = true;
        } else if (cmd == "w" || cmd == "write") {
            SyncUiStateToModel();
            original_config_ = temp_config_;
            if(!original_config_.loaded_config_path.empty()){
                if (write_config_to_file(original_config_, original_config_.loaded_config_path)) { status_message_ = "Changes applied and saved."; changes_applied = true; } 
                else { status_message_ = "Error: failed to write file.";}
            } else {
                status_message_ = "Error: No config file loaded. Use 'apply' with a new path first.";
            }
        } else if (cmd == "q" || cmd == "quit") {
            screen_.Exit();
        } else {
            status_message_ = "Error: Unknown command '" + cmd + "'";
        }
    }

    CliConfig& original_config_;
    CliConfig temp_config_;
    std::string status_message_ = "Ready";

    enum class Mode { Navigate, Edit, Command };
    Mode mode_ = Mode::Navigate;
    int selected_ = 0;
    std::vector<EditableLine> editable_lines_;
    std::string command_buffer_;
    
    std::string edit_buffer_;
    Component editor_input_;
    Component radio_editor_;
    std::vector<std::string> dummy_radio_options_;
    int dummy_radio_selected_idx_ = 0;

    std::string active_config_path_buffer_;
    std::string history_size_buffer_;
    int selected_cache_precision_idx_ = 0;
    int selected_print_mode_idx_ = 0;
    int selected_ops_list_mode_idx_ = 0;
    int selected_path_mode_idx_ = 0;
    int selected_cache_clear_idx_ = 0;
    int selected_exit_sync_idx_ = 0;
    int selected_config_save_idx_ = 0;
    int selected_editor_save_idx_ = 0;

    std::vector<std::string> plugin_dirs_str_;
    
    std::vector<std::string> cache_precision_entries_ = {"int8", "int16"};
    std::vector<std::string> print_mode_entries_ = {"full", "simplified"};
    std::vector<std::string> ops_list_mode_entries_ = {"all", "builtin", "plugins"};
    std::vector<std::string> path_mode_entries_ = {"name_only", "relative_path", "absolute_path"};
    std::vector<std::string> cache_clear_entries_ = {"md", "d", "m"};
    std::vector<std::string> exit_sync_entries_ = {"true", "false"};
    std::vector<std::string> config_save_entries_ = {"current", "default", "ask", "none"};
    std::vector<std::string> editor_save_entries_ = {"ask", "auto_save_on_apply", "manual"};
    // Traversal multi-select UI state
    std::vector<std::string> traversal_tree_entries_ = {"full", "simplified", "no_tree"};
    int selected_traversal_tree_idx_ = 2; // default no_tree
    std::vector<std::string> traversal_cache_entries_ = {"m", "md", "d"};
    int selected_traversal_cache_idx_ = 1; // default md
    std::vector<std::string> traversal_check_entries_ = {"none", "c", "cr"};
    int selected_traversal_check_idx_ = 0;

    // Compute multi-select UI state
    bool compute_force_ = false;
    bool compute_force_deep_ = false;
    bool compute_parallel_ = false;
    bool compute_timer_console_ = false;
    bool compute_timer_log_ = false;
    bool compute_mute_ = false;
}; */

// YAML read/write helpers moved to src/cli_config.cpp and declared in include/cli_config.hpp
static void print_cli_help() {
    std::cout << "Usage: graph_cli [options]\n\n"
              << "Options:\n"
              << "  -h, --help                 Show this help message\n"
              << "  -r, --read <file>          Read YAML and create graph\n"
              << "  -o, --output <file>        Save current graph to YAML\n"
              << "  -p, --print                Print dependency tree\n"
              << "  -t, --traversal            Show evaluation order\n"
              << "      --config <file>        Use a specific configuration file\n"
              << "      --clear-cache          Delete the contents of the cache directory\n"
              << "      --repl                 Start interactive shell (REPL)\n"
              << std::endl;
}

static void print_repl_help(const CliConfig& config) {
    std::cout << "Available REPL (interactive shell) commands:\n\n"
              << "  help\n"
              << "    Show this help message.\n\n"

              << "  clear\n"
              << "    Clear the terminal screen.\n\n"

              << "  config\n"
              << "    Open the interactive configuration editor.\n\n"

              << "  graphs\n"
              << "    List loaded graphs and current selection.\n\n"

              << "  load <name> [yaml]\n"
              << "    Load a graph with a name. If [yaml] is omitted, it loads from sessions/<name>/content.yaml when available.\n"
              << "    Switch after load: " << (config.switch_after_load ? "true" : "false") << "\n\n"

              << "  switch <name>\n"
              << "    Switch current graph.\n\n"

              << "  close <name>\n"
              << "    Close a loaded graph.\n\n"

              << "  ops [mode]\n"
              << "    List all registered operations.\n"
              << "    Modes: all(a), builtin(b), plugins(p)\n"
              << "    Default: " << config.default_ops_list_mode << "\n\n"

              << "  read <file>\n"
              << "    Reload YAML into current graph.\n\n"

              << "  source <file>\n"
              << "    Execute commands from a script file.\n\n"

              << "  print [<id>|all] [mode]\n"
              << "    Show the dependency tree.\n"
              << "    Modes: full(f), simplified(s)\n"
              << "    Default mode: " << config.default_print_mode << "\n\n"

              << "  node [<id>]\n"
              << "    Open the interactive editor for a node.\n"
              << "    If <id> is omitted, a selection menu is shown.\n\n"

              << "  traversal [flags]\n"
              << "    Show evaluation order with cache status and tree flags.\n"
              << "    Tree Flags: full(f), simplified(s), no_tree(n)\n"
              << "    Cache Flags: m(memory), d(disk), c(check), cr(check&remove)\n"
              << "    Default flags: '" << config.default_traversal_arg << "'\n\n"

              << "  output <file>\n"
              << "    Save the current graph to a YAML file.\n\n"

              << "  clear-graph\n"
              << "    Clear the current in-memory graph.\n\n"

              << "  cc, clear-cache [d|m|md]\n"
              << "    Clear on-disk, in-memory, or both caches.\n"
              << "    Default: " << config.default_cache_clear_arg << "\n\n"

              << "  compute <id|all> [flags]\n"
              << "    Compute node(s) with optional flags:\n"
              << "      force:     Clear in-memory caches before compute.\n"
              << "      force-deep: Clear disk+memory caches before compute.\n"
              << "      parallel:  Use multiple threads to compute.\n"
              << "      t:         Print a simple timer summary to the console.\n"
              << "      tl [path]: Log detailed timings to a YAML file.\n"
              << "      m | -m:    Mute node result output (timers still print when enabled).\n"
              << "    Defaults: flags='" << config.default_compute_args << "', log_path='" << config.default_timer_log_path << "'\n\n"

              << "  save <id> <file>\n"
              << "    Compute a node and save its image output to a file.\n\n"

              << "  free\n"
              << "    Free memory used by non-essential intermediate nodes.\n\n"

              << "  exit\n"
              << "    Quit the shell.\n"
              << "    Sync prompt default (exit_prompt_sync): "
              << (config.exit_prompt_sync ? "true" : "false") << "\n";
}

static std::string ask(const std::string& q, const std::string& def) {
    std::cout << q; if (!def.empty()) std::cout << " [" << def << "]";
    std::cout << ": "; std::string s; std::getline(std::cin, s);
    if (s.empty()) return def; return s;
}

static bool ask_yesno(const std::string& q, bool def) {
    std::string d = def ? "Y" : "n";
    while (true) {
        std::string s = ask(q + " [Y/n]", d); if (s.empty()) return def;
        if (s == "Y" || s == "y") return true; if (s == "N" || s == "n") return false;
        std::cout << "Please answer Y or n.\n";
    }
}
void handle_interactive_save(CliConfig& config) {
    if (config.editor_save_behavior == "ask") {
        if (ask_yesno("Save configuration changes?", true)) {
            if (config.loaded_config_path.empty()) {
                std::string path = ask("Enter path to save new config file", "config.yaml");
                if (!path.empty()) write_config_to_file(config, path);
            } else {
                write_config_to_file(config, config.loaded_config_path);
            }
        }
    } else if (config.editor_save_behavior == "auto_save_on_apply") {
         if (config.loaded_config_path.empty()) {
            std::cout << "Warning: auto_save is on, but no config file was loaded. Cannot save." << std::endl;
         } else {
            write_config_to_file(config, config.loaded_config_path);
         }
    }
}
static void do_traversal(const NodeGraph& graph, bool show_mem, bool show_disk) {
    auto ends = graph.ending_nodes();
    if (ends.empty()) { std::cout << "(no ending nodes or graph is cyclic)\n"; return; }

    for (int end : ends) {
        try {
            auto order = graph.topo_postorder_from(end);
            std::cout << "\nPost-order (eval order) for end node " << end << ":\n";
            for (size_t i = 0; i < order.size(); ++i) {
                const auto& node = graph.nodes.at(order[i]);
                std::cout << (i + 1) << ". " << node.id << " (" << node.name << ")";
                
                std::vector<std::string> statuses;
                if (show_mem && node.cached_output.has_value()) { statuses.push_back("in memory"); }
                if (show_disk && !node.caches.empty()) {
                    bool on_disk = false;
                    for (const auto& cache : node.caches) {
                        fs::path cache_file = graph.node_cache_dir(node.id) / cache.location;
                        fs::path meta_file = cache_file; meta_file.replace_extension(".yml");
                        if (fs::exists(cache_file) || fs::exists(meta_file)) { on_disk = true; break; }
                    }
                    if(on_disk) statuses.push_back("on disk");
                }
                if (!statuses.empty()) {
                    std::cout << " (";
                    for(size_t j=0; j<statuses.size(); ++j) std::cout << statuses[j] << (j < statuses.size() - 1 ? ", " : "");
                    std::cout << ")";
                }
                std::cout << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Traversal error on end node " << end << ": " << e.what() << "\n";
        }
    }
}
// static void save_config_interactive(CliConfig& config) {
//     std::string def_char;
//     std::string prompt = "Save updated configuration? [";
    
//     if (config.config_save_behavior == "current" && !config.loaded_config_path.empty()) {
//         prompt += "C]urrent/[d]efault/[a]sk/[n]one"; def_char = "c";
//     } else if (config.config_save_behavior == "default") {
//         prompt += "c]urrent/[D]efault/[a]sk/[n]one"; def_char = "d";
//     } else if (config.config_save_behavior == "ask") {
//         prompt += "c]urrent/[d]efault/[A]sk/[n]one"; def_char = "a";
//     } else {
//         prompt += "c]urrent/[d]efault/[a]sk/[N]one"; def_char = "n";
//     }
    
//     if (config.loaded_config_path.empty()) {
//         prompt.replace(prompt.find("c]urrent"), 8, "(no current)");
//         if (def_char == "c") def_char = "d";
//     }
//     prompt += "]";

//     while(true) {
//         std::string choice = ask(prompt, def_char);
//         if (choice == "c" && !config.loaded_config_path.empty()) {
//             write_config_to_file(config, config.loaded_config_path);
//             break;
//         } else if (choice == "d") {
//             write_config_to_file(config, "config.yaml");
//             break;
//         } else if (choice == "a") {
//             std::string path = ask("Enter path to save new config file");
//             if (!path.empty()) write_config_to_file(config, path);
//             break;
//         } else if (choice == "n") {
//             std::cout << "Configuration changes will not be saved." << std::endl;
//             break;
//         } else {
//             std::cout << "Invalid choice." << std::endl;
//         }
//     }
// }
// run_config_editor is provided by src/cli/config_editor.cpp
static bool save_fp32_image(const cv::Mat& mat, const std::string& path, const CliConfig& config) {
    if (mat.empty()) { std::cout << "Error: Cannot save an empty image.\n"; return false; }
    cv::Mat out_mat;
    if (config.cache_precision == "int16") { mat.convertTo(out_mat, CV_16U, 65535.0); } 
    else { mat.convertTo(out_mat, CV_8U, 255.0); }
    return cv::imwrite(path, out_mat);
}

static bool process_command(const std::string& line, ps::InteractionService& svc, std::string& current_graph, bool& modified, CliConfig& config) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return true;
    // Avoid creating FTXUI screens here to prevent terminal mode conflicts with raw REPL
    try {
        if (cmd == "help") {
            print_repl_help(config);
        } else if (cmd == "clear" || cmd == "cls") {
            std::cout << "\033[2J\033[1;1H";
        } else if (cmd == "graphs") {
            auto names = svc.cmd_list_graphs();
            if (names.empty()) { std::cout << "(no graphs loaded)\n"; return true; }
            std::cout << "Loaded graphs:" << std::endl;
            for (auto& n : names) {
                std::cout << "  - " << n; if (n == current_graph) std::cout << "  [current]"; std::cout << std::endl;
            }
        } else if (cmd == "load") {
            std::string name, yaml; iss >> name >> yaml;
            if (name.empty()) { std::cout << "Usage: load <name> [yaml]\n"; return true; }
            if (yaml.empty()) {
                auto session_yaml = ps::fs::path("sessions")/name/"content.yaml";
                if (!ps::fs::exists(session_yaml)) {
                    std::cout << "Error: session YAML not found: " << session_yaml << "\n";
                    std::cout << "Hint: provide an explicit YAML path: load <name> <yaml>\n";
                    return true;
                }
                auto ok = svc.cmd_load_graph(name, "sessions", "", config.loaded_config_path);
                if (!ok) { std::cout << "Error: failed to load session graph '" << name << "' from disk.\n"; return true; }
                if (config.switch_after_load) {
                    current_graph = *ok;
                    config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/name/"config.yaml")).string();
                }
                std::cout << "Loaded graph '" << name << "' from session (content.yaml).\n";
            } else {
                auto ok = svc.cmd_load_graph(name, "sessions", yaml, config.loaded_config_path);
                if (!ok) { std::cout << "Error: failed to load graph '" << name << "' from '" << yaml << "'.\n"; return true; }
                if (config.switch_after_load) {
                    current_graph = *ok;
                    config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/name/"config.yaml")).string();
                }
                std::cout << "Loaded graph '" << name << "' (yaml: " << yaml << ").\n";
            }
        } else if (cmd == "switch") {
            std::string name; iss >> name; if (name.empty()) { std::cout << "Usage: switch <name>\n"; return true; }
            auto names = svc.cmd_list_graphs();
            if (std::find(names.begin(), names.end(), name) == names.end()) { std::cout << "Graph not found: " << name << "\n"; return true; }
            current_graph = name; std::cout << "Switched to '" << name << "'.\n";
        } else if (cmd == "close") {
            std::string name; iss >> name; if (name.empty()) { std::cout << "Usage: close <name>\n"; return true; }
            if (!svc.cmd_close_graph(name)) { std::cout << "Error: failed to close '" << name << "'.\n"; return true; }
            if (current_graph == name) current_graph.clear();
            std::cout << "Closed graph '" << name << "'.\n";
        } else if (cmd == "print") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string target_str = "all";
            std::string mode_str = config.default_print_mode;
            bool target_is_set = false;

            std::string arg;
            while(iss >> arg) {
                if (arg == "f" || arg == "full" || arg == "s" || arg == "simplified") { mode_str = arg; } 
                else { 
                    if (target_is_set) { std::cout << "Warning: Multiple targets specified for print; using last one ('" << arg << "').\n"; }
                    target_str = arg;
                    target_is_set = true;
                }
            }

            bool show_params = (mode_str == "f" || mode_str == "full");
            if (target_str == "all") {
                auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, show_params);
                if (dump) std::cout << *dump; else std::cout << "(failed to dump tree)\n";
            } else {
                try {
                    int node_id = std::stoi(target_str);
                    auto dump = svc.cmd_dump_tree(current_graph, node_id, show_params);
                    if (dump) std::cout << *dump; else std::cout << "(failed to dump tree)\n";
                } catch (const std::exception&) {
                    std::cout << "Error: Invalid target '" << target_str << "'. Must be an integer ID or 'all'." << std::endl;
                }
            }
        } else if (cmd == "node") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::optional<int> maybe_id;
            std::string word;
            if (iss >> word) { try { maybe_id = std::stoi(word); } catch (...) { maybe_id.reset(); } }
            run_node_editor_full(svc, current_graph, maybe_id);
            // After leaving the FTXUI node editor, clear screen to refresh REPL view
            std::cout << "\033[2J\033[1;1H";
        }  else if (cmd == "ops") {
            std::string mode_arg; iss >> mode_arg;
            if (mode_arg.empty()) { mode_arg = config.default_ops_list_mode; }

            std::string display_mode, display_title;

            if (mode_arg == "all" || mode_arg == "a") { display_mode = "all"; display_title = "all"; } 
            else if (mode_arg == "builtin" || mode_arg == "b") { display_mode = "builtin"; display_title = "built-in"; } 
            else if (mode_arg == "plugins" || mode_arg == "custom" || mode_arg == "p" || mode_arg == "c") { display_mode = "plugins"; display_title = "plugins"; } 
            else { std::cout << "Error: Invalid mode for 'ops'. Use: all (a), builtin (b), or plugins (p/c)." << std::endl; return true; }

            std::map<std::string, std::vector<std::pair<std::string, std::string>>> grouped_ops;
            int op_count = 0;

            auto op_sources = svc.cmd_ops_sources();
            for (const auto& pair : op_sources) {
                const std::string& key = pair.first;
                const std::string& source = pair.second;
                bool is_builtin = (source == "built-in");

                if ((display_mode == "builtin" && !is_builtin) || (display_mode == "plugins" && is_builtin)) continue;

                size_t colon_pos = key.find(':');
                if (colon_pos != std::string::npos) {
                    std::string type = key.substr(0, colon_pos);
                    std::string subtype = key.substr(colon_pos + 1);
                    grouped_ops[type].push_back({subtype, source});
                    op_count++;
                }
            }
            
            if (op_count == 0) {
                if (display_mode == "plugins") std::cout << "No plugin operations are registered." << std::endl;
                else std::cout << "No operations are registered." << std::endl;
                return true;
            }

            std::cout << "Available Operations (" << display_title << "):" << std::endl;
            for (auto& pair : grouped_ops) {
                std::sort(pair.second.begin(), pair.second.end());
                std::cout << "\n  Type: " << pair.first << std::endl;
                for (const auto& op_info : pair.second) {
                    std::cout << "    - " << op_info.first;
                    if (op_info.second != "built-in") {
                        std::string plugin_path_str = op_info.second;
                        std::string display_path;
                        if (config.ops_plugin_path_mode == "absolute_path") { display_path = plugin_path_str; } 
                        else if (config.ops_plugin_path_mode == "relative_path") { display_path = fs::relative(plugin_path_str).string(); } 
                        else { display_path = fs::path(plugin_path_str).filename().string(); }
                        std::cout << "  [plugin: " << display_path << "]";
                    }
                    std::cout << std::endl;
                }
            }

        } else if (cmd == "traversal") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string arg;
            std::string print_tree_mode = "none";
            bool show_mem = false, show_disk = false, do_check = false, do_check_remove = false;
            
            // Collect real args (ignores trailing spaces). If none, fall back to config defaults.
            std::vector<std::string> args;
            while (iss >> arg) args.push_back(arg);
            const auto parse_tokens = [&](const std::vector<std::string>& toks){
                for (const auto& a : toks) {
                    if (a == "f" || a == "full") print_tree_mode = "full";
                    else if (a == "s" || a == "simplified") print_tree_mode = "simplified";
                    else if (a == "n" || a == "no_tree") print_tree_mode = "none";
                    else if (a == "md") { show_mem = true; show_disk = true; }
                    else if (a == "m") show_mem = true;
                    else if (a == "d") show_disk = true;
                    else if (a == "cr") do_check_remove = true;
                    else if (a == "c") do_check = true;
                }
            };
            if (args.empty()) {
                std::istringstream default_iss(config.default_traversal_arg);
                std::vector<std::string> def_args; while (default_iss >> arg) def_args.push_back(arg);
                parse_tokens(def_args);
            } else {
                parse_tokens(args);
            }

            if (do_check_remove) svc.cmd_synchronize_disk_cache(current_graph, config.cache_precision);
            else if (do_check) svc.cmd_cache_all_nodes(current_graph, config.cache_precision);

            if (print_tree_mode == "full") { auto s = svc.cmd_dump_tree(current_graph, std::nullopt, true); if (s) std::cout << *s; }
            else if (print_tree_mode == "simplified") { auto s = svc.cmd_dump_tree(current_graph, std::nullopt, false); if (s) std::cout << *s; }

            auto orders = svc.cmd_traversal_orders(current_graph);
            if (!orders || orders->empty()) { std::cout << "(no ending nodes or graph is cyclic)\n"; return true; }
            for (const auto& kv : *orders) {
                std::cout << "\nPost-order (eval order) for end node " << kv.first << ":\n";
                bool first = true;
                for (int id : kv.second) { if (!first) std::cout << " -> "; std::cout << id; first = false; }
                std::cout << "\n";
            }
        } else if (cmd == "config") {
            run_config_editor(config);
            // base dir used on subsequent `load`
        } 
        else if (cmd == "read") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string path; iss >> path; if (path.empty()) std::cout << "Usage: read <filepath>\n";
            else { if (svc.cmd_reload_yaml(current_graph, path)) { modified = false; std::cout << "Loaded graph from " << path << "\n"; } else { std::cout << "Failed to load." << std::endl; } }
        } else if (cmd == "source") {
            std::string filename; iss >> filename;
            if (filename.empty()) { std::cout << "Usage: source <filename>\n"; return true; }
            std::ifstream script_file(filename);
            if (!script_file) { std::cout << "Error: Cannot open script file: " << filename << "\n"; return true; }
            std::string script_line;
            while (std::getline(script_file, script_line)) {
                if (script_line.empty() || script_line.find_first_not_of(" \t") == std::string::npos || script_line[0] == '#') continue;
                std::cout << "ps> " << script_line << std::endl;
                if (!process_command(script_line, svc, current_graph, modified, config)) return false;
            }
        } 
        else if (cmd == "output") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string path; iss >> path; if (path.empty()) std::cout << "Usage: output <filepath>\n";
            else { if (svc.cmd_save_yaml(current_graph, path)) { modified = false; std::cout << "Saved to " << path << "\n"; } else { std::cout << "Failed to save." << std::endl; } }
        } else if (cmd == "clear-graph") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            svc.cmd_clear_graph(current_graph); modified = true; std::cout << "Graph cleared.\n";
        } else if (cmd == "clear-cache" || cmd == "cc") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string arg; iss >> arg;
            if (arg.empty()) { arg = config.default_cache_clear_arg; }
            if (arg == "both" || arg == "md" || arg == "dm") svc.cmd_clear_cache(current_graph);
            else if (arg == "drive" || arg == "d") svc.cmd_clear_drive_cache(current_graph);
            else if (arg == "memory" || arg == "m") svc.cmd_clear_memory_cache(current_graph);
        } 
        else if (cmd == "compute") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string target_id_str; iss >> target_id_str;
            if (target_id_str.empty()) { target_id_str = "all"; }

            bool force = false, force_deep = false, timer_console = false, timer_log_file = false, parallel = false, mute = false;
            std::string timer_log_path = "";
            // Collect tokens either from user input or from defaults when none provided.
            std::vector<std::string> tokens;
            if (iss.rdbuf()->in_avail() == 0) {
                std::istringstream def_iss(config.default_compute_args);
                std::string tok; while (def_iss >> tok) tokens.push_back(tok);
            } else {
                std::string tok; while (iss >> tok) tokens.push_back(tok);
            }
            auto is_flag = [](const std::string& s){
                return s == "force" || s == "force-deep" || s == "t" || s == "-t" || s == "timer" || s == "parallel" || s == "tl" || s == "-tl" || s == "m" || s == "-m" || s == "mute";
            };
            for (size_t i = 0; i < tokens.size(); ++i) {
                const std::string& a = tokens[i];
                if (a == "force") force = true;
                else if (a == "force-deep") force_deep = true;
                else if (a == "t" || a == "-t" || a == "timer") timer_console = true;
                else if (a == "parallel") parallel = true;
                else if (a == "m" || a == "-m" || a == "mute") mute = true;
                else if (a == "tl" || a == "-tl") {
                    timer_log_file = true;
                    if (i + 1 < tokens.size() && !is_flag(tokens[i+1])) { timer_log_path = tokens[++i]; }
                }
            }

            bool enable_timing = timer_console || timer_log_file;
            if (force_deep) svc.cmd_clear_cache(current_graph); else if (force) svc.cmd_clear_memory_cache(current_graph);
            auto total_start = std::chrono::high_resolution_clock::now();
            if (target_id_str == "all") {
                auto orders = svc.cmd_traversal_orders(current_graph);
                if (!orders || orders->empty()) { std::cout << "(no ending nodes or graph is cyclic)\n"; return true; }
                for (const auto& kv : *orders) {
                    svc.cmd_compute(current_graph, kv.first, config.cache_precision, false, enable_timing, parallel, /*quiet=*/mute);
                    if (!mute) std::cout << "-> End node " << kv.first << " computed.\n";
                }
            } else {
                int id = std::stoi(target_id_str);
                svc.cmd_compute(current_graph, id, config.cache_precision, false, enable_timing, parallel, /*quiet=*/mute);
                if (!mute) std::cout << "-> Node " << id << " computed.\n";
            }
            auto timing = svc.cmd_timing(current_graph);
            if (timer_console) {
                std::cout << "--- Computation Timers (Console) ---\n";
                if (timing) { for (const auto& t : timing->node_timings) printf("  - Node %-3d (%-20s): %10.4f ms [%s]\n", t.id, t.name.c_str(), t.elapsed_ms, t.source.c_str()); }
                else std::cout << "(no timing data)\n";
            }
            if (timer_log_file) {
                auto total_end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> total_elapsed = total_end - total_start;
                if (timer_log_path.empty()) timer_log_path = config.default_timer_log_path;
                fs::path out_path(timer_log_path); if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path());
                YAML::Node root; YAML::Node steps(YAML::NodeType::Sequence);
                if (timing) { for (const auto& t : timing->node_timings) { YAML::Node n; n["id"]=t.id; n["name"]=t.name; n["time_ms"]=t.elapsed_ms; n["source"]=t.source; steps.push_back(n);} }
                root["steps"]=steps; root["total_time_ms"]= timing ? timing->total_ms : total_elapsed.count(); std::ofstream fout(timer_log_path); fout << root; std::cout << "Timer log successfully written to '" << timer_log_path << "'." << std::endl;
            }

        } else if (cmd == "save") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            std::string id_str, path; iss >> id_str >> path; if (id_str.empty() || path.empty()) { std::cout << "Usage: save <node_id> <filepath>\n"; }
            else { int id = std::stoi(id_str); auto mat = svc.cmd_compute_and_get_image(current_graph, id, config.cache_precision, false, false, false); if (!mat || mat->empty()) { std::cout << "Compute returned empty image.\n"; } else if (save_fp32_image(*mat, path, config)) { std::cout << "Successfully saved node " << id << " image to " << path << "\n"; } else { std::cout << "Error: Failed to save image to " << path << "\n"; } }
        } else if (cmd == "free") {
            if (current_graph.empty()) { std::cout << "No current graph. Use load/switch.\n"; return true; }
            svc.cmd_free_transient_memory(current_graph);
        } else if (cmd == "exit") {
            std::cout << std::endl;
            if (modified && ask_yesno("You have unsaved changes. Save graph to file?", true)) {
                if (current_graph.empty()) { std::cout << "No current graph to save.\n"; }
                else {
                    std::string path = ask("output file", config.default_exit_save_path);
                    if (svc.cmd_save_yaml(current_graph, path)) std::cout << "Saved to " << path << "\n";
                }
            }
            // 说明：此处实现了 config 中的 `exit_prompt_sync` 行为。
            // 含义：退出 REPL 前是否将内存中的缓存状态同步到磁盘。
            // - 提示默认值由 `config.exit_prompt_sync` 决定（true=默认 Yes，false=默认 No）。
            // - 若用户确认，则调用 `synchronize_disk_cache` 将内存缓存刷写到磁盘缓存。
            if (!current_graph.empty() && ask_yesno("Synchronize disk cache with memory state before exiting?", config.exit_prompt_sync)) {
                svc.cmd_synchronize_disk_cache(current_graph, config.cache_precision);
            }
            return false;
        } else {
            std::cout << "Unknown command: " << cmd << ". Type 'help' for a list of commands.\n";
        }
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return true;
}

// --- Decoupled REPL using InteractionService ---
static void run_repl(ps::InteractionService& svc, CliConfig& config, const std::string& initial_graph) {
    bool modified = false;
    std::string current_graph = initial_graph;
    
    CliHistory history;
    history.SetMaxSize(config.history_size);
    CliAutocompleter completer(svc);
    if (!current_graph.empty()) completer.SetCurrentGraph(current_graph);

    // --- State for the new cyclical autocompletion feature ---
    struct CompletionState {
        std::vector<std::string> options;
        int current_index = -1;
        size_t original_cursor_pos = 0;
        std::string original_prefix;

        void Reset() {
            options.clear();
            current_index = -1;
        }
        bool IsActive() const { return current_index != -1; }
    } completion_state;

    std::string line_buffer;
    int cursor_pos = 0;
    InputMatchState history_match_state;
    
    // This lambda is now more complex to handle highlighting for cyclical completion.
    std::function<void(const std::vector<std::string>&)> redraw_line_impl;
    auto redraw_line = [&](const std::vector<std::string>& options_to_display = {}) {
        redraw_line_impl(options_to_display);
    };

    redraw_line_impl = [&](const std::vector<std::string>& options_to_display) {
        // Erase the current line and move cursor to the beginning.
        std::cout << "\r\x1B[K" << "ps> ";

        if (completion_state.IsActive()) {
            // Highlight the active completion (invert only the completed token)
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

        // Move cursor back to its logical position.
        std::cout << "\r\x1B[" << (4 + cursor_pos) << "C" << std::flush;

        // No multi-line candidates display; keep everything on a single line per requirement.
    };

    // FIX: Print the welcome message *before* putting the terminal in raw mode.
    std::cout << "Photospider dynamic graph shell (decoupled). Type 'help' for commands.\n";
    std::cout << "History file: " << history.Path() << "\n";
    TerminalInput term_input;
    redraw_line();

    while (true) {
        int key = term_input.GetChar();

        // Any keypress other than Tab breaks the completion cycle.
        if (key != TAB) {
            completion_state.Reset();
        }

        // Reset sticky history matching on edits or cursor moves
        auto reset_history_match = [&](){ history_match_state.Reset(); };

        switch (key) {
            case ENTER: {
                // Restore cooked mode first so newlines behave normally,
                // and ensure we start at column 0 on the next line.
                term_input.Restore();
                std::cout << "\r\n";
                if (!line_buffer.empty()) {
                    history.Add(line_buffer);
                    history.Save();
                }
                // Now, execute the command.
                bool continue_repl = process_command(line_buffer, svc, current_graph, modified, config);
                // Keep autocompleter in sync with current graph changes
                completer.SetCurrentGraph(current_graph);

                // Re-enter raw mode to capture individual keystrokes for the next prompt.
                term_input.SetRaw();

                if (!continue_repl) {
                    return; // The 'exit' command was issued.
                }

                // Reset for the next line of input.
                line_buffer.clear();
                cursor_pos = 0;
                history.ResetNavigation();
                history_match_state.Reset();
                redraw_line();
                break;
            }
            case CTRL_C: {
                if (line_buffer.empty()) {
                    // Print starting at column 0 even in raw mode.
                    std::cout << "\r\n(To exit, type 'exit' or press Ctrl+C again on an empty line)\r\n";
                    redraw_line();
                    key = term_input.GetChar();
                    if(key == CTRL_C) {
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
            case BACKSPACE: {
                if (cursor_pos > 0) {
                    line_buffer.erase(cursor_pos - 1, 1);
                    cursor_pos--;
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case DEL: {
                 if (cursor_pos < (int)line_buffer.length()) {
                    line_buffer.erase(cursor_pos, 1);
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case UP: {
                if (!history_match_state.active) {
                    history_match_state.Begin(line_buffer.substr(0, cursor_pos), cursor_pos);
                }
                const std::string& sticky = history_match_state.original_prefix;
                line_buffer = history.GetPrevious(sticky);
                cursor_pos = line_buffer.length();
                redraw_line();
                break;
            }
            case DOWN: {
                if (!history_match_state.active) {
                    history_match_state.Begin(line_buffer.substr(0, cursor_pos), cursor_pos);
                }
                const std::string& sticky = history_match_state.original_prefix;
                line_buffer = history.GetNext(sticky);
                cursor_pos = line_buffer.length();
                redraw_line();
                break;
            }
            case LEFT: {
                if (cursor_pos > 0) {
                    cursor_pos--;
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case RIGHT: {
                if (cursor_pos < (int)line_buffer.length()) {
                    cursor_pos++;
                    history_match_state.Reset();
                    redraw_line();
                }
                break;
            }
            case TAB: {
                // Unified completer using InteractionService
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
                    // Always enter cycling mode, even for a single option, to avoid duplicate appends
                    completion_state.options = result.options;
                    completion_state.current_index = 0;
                    // compute prefix and cursor pos based on the original buffer/cursor
                    size_t start = line_buffer.find_last_of(" \t", cursor_pos ? cursor_pos-1 : 0);
                    start = (start==std::string::npos)?0:start+1;
                    completion_state.original_prefix = line_buffer.substr(start, cursor_pos - (int)start);
                    completion_state.original_cursor_pos = cursor_pos;
                    // apply the completion result (may include trailing space from completer for exact match)
                    line_buffer = result.new_line;
                    cursor_pos = result.new_cursor_pos;
                    redraw_line(completion_state.options);
                }
                break; }
            case UNKNOWN:
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

// load_plugins moved to src/plugin_loader.cpp

int main(int argc, char** argv) {
    cv::ocl::setUseOpenCL(false);
    ops::register_builtin();
    ps::Kernel kernel;
    ps::InteractionService svc(kernel);
    svc.cmd_seed_builtin_ops();

    CliConfig config;
    std::string custom_config_path;

    const char* const short_opts = "hr:o:pt:R";
    const option long_opts[] = {
        {"help", no_argument, nullptr, 'h'}, {"read", required_argument, nullptr, 'r'},
        {"output", required_argument, nullptr, 'o'}, {"print", no_argument, nullptr, 'p'}, 
        {"traversal", no_argument, nullptr, 't'}, {"clear-cache", no_argument, nullptr, 1001},
        {"repl", no_argument, nullptr, 'R'}, {"config", required_argument, nullptr, 2001},
        {nullptr, 0, nullptr, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        if (opt == 2001) { custom_config_path = optarg; }
    }
    optind = 1;

    std::string config_to_load = custom_config_path.empty() ? "config.yaml" : custom_config_path;
    load_or_create_config(config_to_load, config);
    svc.cmd_plugins_load(config.plugin_dirs);
    std::string current_graph;
    
    bool did_any_action = false;
    bool start_repl_after_actions = false;

    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        try {
            switch (opt) {
            case 'h': print_cli_help(); return 0;
            case 'r': {
                auto ok = svc.cmd_load_graph("default", "sessions", optarg, config.loaded_config_path);
                if (ok) {
                    if (config.switch_after_load) current_graph = *ok;
                    config.loaded_config_path = (ps::fs::absolute(ps::fs::path("sessions")/"default"/"config.yaml")).string();
                    std::cout << "Loaded graph from " << optarg << "\n"; did_any_action = true;
                }
                else { std::cerr << "Failed to load graph from '" << optarg << "'.\n"; }
                break; }
            case 'o': {
                if (current_graph.empty()) { std::cerr << "No graph loaded; use -r first.\n"; break; }
                if (svc.cmd_save_yaml(current_graph, optarg)) { std::cout << "Saved graph to " << optarg << "\n"; did_any_action = true; }
                else { std::cerr << "Failed to save graph.\n"; }
                break; }
            case 'p': {
                if (current_graph.empty()) { std::cerr << "No graph loaded; use -r first.\n"; break; }
                auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, /*show_params*/true);
                if (dump) { std::cout << *dump; did_any_action = true; }
                else std::cerr << "Failed to print tree.\n";
                break; }
            case 't': {
                if (current_graph.empty()) { std::cerr << "No graph loaded; use -r first.\n"; break; }
                auto dump = svc.cmd_dump_tree(current_graph, std::nullopt, /*show_params*/true);
                if (dump) std::cout << *dump;
                auto orders = svc.cmd_traversal_orders(current_graph);
                if (orders) {
                    for (const auto& kv : *orders) {
                        std::cout << "\nPost-order (eval order) for end node " << kv.first << ":\n";
                        bool first = true; for (int id : kv.second) { if (!first) std::cout << " -> "; std::cout << id; first = false; }
                        std::cout << "\n";
                    }
                }
                did_any_action = true; break; }
            
            case 1001: if (!current_graph.empty()) { svc.cmd_clear_cache(current_graph); did_any_action = true; } break;
            case 'R': start_repl_after_actions = true; break;
            case 2001: break;
            default: print_cli_help(); return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n"; return 2;
        }
    }

    if (start_repl_after_actions || !did_any_action) {
        if (did_any_action) {
            std::cout << "\n--- Command-line actions complete. Entering interactive shell. ---\n";
        }
        run_repl(svc, config, current_graph);
    } else {
         std::cout << "\n--- Command-line actions complete. Exiting. ---\n";
    }
    
    return 0;
}
