// Front-end only: TUI Config Editor implementation extracted from CLI
#include "cli/config_editor.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "cli_config.hpp"
#include "cli/path_complete.hpp"
#include "cli/tui_editor.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"

using namespace ftxui;
using namespace ps;
namespace fs = std::filesystem;

// Local editor UI (moved from cli/graph_cli.cpp), trimmed for encapsulation.
class ConfigEditor : public TuiEditor {
    struct EditableLine {
        std::string label;
        std::string* value_ptr = nullptr;
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
        : TuiEditor(screen), original_config_(config), temp_config_(config) {
        InputOption opt; opt.on_enter = [this]{ CommitEdit(); };
        editor_input_ = Input(&edit_buffer_, "...", opt);
        radio_editor_ = Radiobox(&dummy_radio_options_, &dummy_radio_selected_idx_);
        SyncModelToUiState();
        RebuildLineView();
    }
    void Run() override {
        auto main_container = Container::Vertical({});
        auto component = Renderer(main_container, [&] {
            Elements line_elements;
            for (size_t i = 0; i < editable_lines_.size(); ++i) {
                auto& line = editable_lines_[i];
                Element display_element;
                if (mode_ == Mode::Edit && selected_ == (int)i) {
                    if (line.is_radio) display_element = radio_editor_->Render();
                    else if (line.is_toggle && line.toggle_ptr) display_element = text((*line.toggle_ptr) ? "[x]" : "[ ]");
                    else display_element = editor_input_->Render();
                } else {
                    if (line.is_radio) display_element = text((*line.radio_options)[*line.radio_selected_idx]);
                    else if (line.is_toggle && line.toggle_ptr) display_element = text((*line.toggle_ptr) ? "[x]" : "[ ]");
                    else if (line.value_ptr) display_element = text(*line.value_ptr);
                    else display_element = text("...") | dim;
                }
                auto content = hbox({ text(line.label) | size(WIDTH, EQUAL, 28), display_element | flex, });
                if (selected_ == (int)i) content |= inverted;
                line_elements.push_back(content);
            }
            return vbox({ text("Interactive Configuration Editor") | bold | hcenter,
                         separator(), vbox(std::move(line_elements)) | vscroll_indicator | frame | flex,
                         separator(), RenderStatusBar() });
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
                if (editable_lines_[selected_].is_radio) return radio_editor_->OnEvent(event);
                else {
                    // Basic path completion on Tab
                    auto is_path_like = [&](){
                        if (selected_ >= (int)editable_lines_.size()) return false;
                        const auto& lbl = editable_lines_[selected_].label;
                        return lbl.find("path") != std::string::npos || lbl.find("file") != std::string::npos || lbl.find("dir") != std::string::npos;
                    };
                    if (event == Event::Tab && is_path_like()) {
                        auto options = ps::PathCompleteOptions(edit_buffer_);
                        if (!options.empty()) {
                            auto common = ps::LongestCommonPrefix(options);
                            if (!common.empty()) edit_buffer_ = common;
                        }
                        return true;
                    }
                    return editor_input_->OnEvent(event);
                }
            }
            if (event == Event::Character('e')) { EnterEditMode(); return true; }
            if (event == Event::Character('q')) { screen_.Exit(); return true; }
            if (event == Event::Character(':')) { mode_ = Mode::Command; return true; }
            if (event == Event::ArrowDown) { selected_ = std::min(selected_ + 1, (int)editable_lines_.size() - 1); return true; }
            if (event == Event::ArrowUp) { selected_ = std::max(selected_ - 1, 0); return true; }
            if (event == Event::Character('a')) { if (selected_ < (int)editable_lines_.size() && editable_lines_[selected_].add_fn) { editable_lines_[selected_].add_fn(); return true; } }
            if (event == Event::Character('d')) { if (selected_ < (int)editable_lines_.size() && editable_lines_[selected_].del_fn) { editable_lines_[selected_].del_fn(); return true; } }
            return false;
        });
        screen_.Loop(component);
    }

private:
    void SyncModelToUiState() {
        plugin_dirs_str_ = original_config_.plugin_dirs;
        active_config_path_buffer_ = original_config_.loaded_config_path;
        history_size_buffer_ = std::to_string(original_config_.history_size);
        auto find_index = [](const std::vector<std::string>& v, const std::string& val){ for(size_t i=0;i<v.size();++i) if (v[i]==val) return (int)i; return 0; };
        selected_cache_precision_idx_ = find_index(cache_precision_entries_, original_config_.cache_precision);
        selected_print_mode_idx_ = find_index(print_mode_entries_, original_config_.default_print_mode);
        selected_ops_list_mode_idx_ = find_index(ops_list_mode_entries_, original_config_.default_ops_list_mode);
        selected_path_mode_idx_ = find_index(path_mode_entries_, original_config_.ops_plugin_path_mode);
        selected_cache_clear_idx_ = find_index(cache_clear_entries_, original_config_.default_cache_clear_arg);
        selected_exit_sync_idx_ = find_index(exit_sync_entries_, original_config_.exit_prompt_sync ? std::string("true") : std::string("false"));
        selected_config_save_idx_ = find_index(config_save_entries_, original_config_.config_save_behavior);
        selected_editor_save_idx_ = find_index(editor_save_entries_, original_config_.editor_save_behavior);

        // Parse traversal defaults into UI state: tree radio + cache checkboxes + check action
        selected_traversal_tree_idx_ = 2; // no_tree by default
        traversal_cache_m_ = false; traversal_cache_d_ = false;
        selected_traversal_check_idx_ = 0; // none
        {
            std::istringstream iss(original_config_.default_traversal_arg);
            std::string tok;
            while (iss >> tok) {
                if (tok == "f" || tok == "full" || tok == "detailed" || tok == "d") selected_traversal_tree_idx_ = 0;
                else if (tok == "s" || tok == "simplified") selected_traversal_tree_idx_ = 1;
                else if (tok == "n" || tok == "no_tree" || tok == "none") selected_traversal_tree_idx_ = 2;
                else if (tok == "m") traversal_cache_m_ = true;
                else if (tok == "d") traversal_cache_d_ = true;
                else if (tok == "md") { traversal_cache_m_ = true; traversal_cache_d_ = true; }
                else if (tok == "c") selected_traversal_check_idx_ = 1;
                else if (tok == "cr") selected_traversal_check_idx_ = 2;
            }
        }

        // Parse compute defaults into UI state
        compute_force_ = compute_force_deep_ = compute_parallel_ = compute_timer_console_ = compute_timer_log_ = compute_mute_ = false;
        {
            std::istringstream iss(original_config_.default_compute_args);
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
        original_config_.plugin_dirs = plugin_dirs_str_;
        original_config_.loaded_config_path = active_config_path_buffer_;
        try { original_config_.history_size = std::stoi(history_size_buffer_); } catch (...) {}
        original_config_.cache_precision = cache_precision_entries_[selected_cache_precision_idx_];
        original_config_.default_print_mode = print_mode_entries_[selected_print_mode_idx_];
        original_config_.default_ops_list_mode = ops_list_mode_entries_[selected_ops_list_mode_idx_];
        original_config_.ops_plugin_path_mode = path_mode_entries_[selected_path_mode_idx_];
        original_config_.default_cache_clear_arg = cache_clear_entries_[selected_cache_clear_idx_];
        original_config_.exit_prompt_sync = (exit_sync_entries_[selected_exit_sync_idx_] == "true");
        original_config_.config_save_behavior = config_save_entries_[selected_config_save_idx_];
        original_config_.editor_save_behavior = editor_save_entries_[selected_editor_save_idx_];

        // Compose traversal defaults from UI state
        {
            std::ostringstream oss;
            // tree mode first
            if (selected_traversal_tree_idx_ == 0) oss << "f";
            else if (selected_traversal_tree_idx_ == 1) oss << "s";
            else oss << "n";
            if (traversal_cache_m_) oss << " m";
            if (traversal_cache_d_) oss << " d";
            if (selected_traversal_check_idx_ == 1) oss << " c";
            else if (selected_traversal_check_idx_ == 2) oss << " cr";
            original_config_.default_traversal_arg = oss.str();
        }

        // Compose compute defaults from checkboxes
        {
            std::ostringstream oss;
            bool first = true;
            auto add = [&](const std::string& s){ if (!first) oss << ' '; oss << s; first = false; };
            if (compute_force_) add("force");
            if (compute_force_deep_) add("force-deep");
            if (compute_parallel_) add("parallel");
            if (compute_timer_console_) add("t");
            if (compute_timer_log_) add("tl");
            if (compute_mute_) add("m");
            original_config_.default_compute_args = oss.str();
        }
    }
    void RebuildLineView() {
        editable_lines_.clear();
        auto add_line = [&](const std::string& label, std::string* ptr){ editable_lines_.push_back({label, ptr}); };
        auto add_radio_line = [&](const std::string& label, std::vector<std::string>* opts, int* idx){ editable_lines_.push_back({label, nullptr, true, opts, idx}); };
        auto add_toggle_line = [&](const std::string& label, bool* ptr){ editable_lines_.push_back({label, nullptr, false, nullptr, nullptr, nullptr, nullptr, true, ptr}); };

        add_line("active_config", &active_config_path_buffer_);
        add_line("cache_root_dir", &original_config_.cache_root_dir);
        add_line("history_size", &history_size_buffer_);
        add_radio_line("cache_precision", &cache_precision_entries_, &selected_cache_precision_idx_);
        // Traversal defaults: tree mode + cache flags + check action
        add_radio_line("traversal_tree_mode", &traversal_tree_entries_, &selected_traversal_tree_idx_);
        add_toggle_line("traversal_cache_memory(m)", &traversal_cache_m_);
        add_toggle_line("traversal_cache_disk(d)", &traversal_cache_d_);
        add_radio_line("traversal_check", &traversal_check_entries_, &selected_traversal_check_idx_);
        add_line("default_exit_save_path", &original_config_.default_exit_save_path);
        add_line("default_timer_log_path", &original_config_.default_timer_log_path);
        add_toggle_line("exit_prompt_sync", &original_config_.exit_prompt_sync);
        add_toggle_line("switch_after_load", &original_config_.switch_after_load);
        add_toggle_line("session_warning", &original_config_.session_warning);
        // Compute defaults: checkboxes
        add_toggle_line("compute_force", &compute_force_);
        add_toggle_line("compute_force_deep", &compute_force_deep_);
        add_toggle_line("compute_parallel", &compute_parallel_);
        add_toggle_line("compute_timer_console(t)", &compute_timer_console_);
        add_toggle_line("compute_timer_log(tl)", &compute_timer_log_);
        add_toggle_line("compute_mute(m)", &compute_mute_);
        add_radio_line("config_save_behavior", &config_save_entries_, &selected_config_save_idx_);
        add_radio_line("editor_save_behavior", &editor_save_entries_, &selected_editor_save_idx_);
        add_radio_line("default_print_mode", &print_mode_entries_, &selected_print_mode_idx_);
        add_radio_line("default_ops_list_mode", &ops_list_mode_entries_, &selected_ops_list_mode_idx_);
        add_radio_line("ops_plugin_path_mode", &path_mode_entries_, &selected_path_mode_idx_);
        add_radio_line("default_cache_clear_arg", &cache_clear_entries_, &selected_cache_clear_idx_);

        for (size_t i = 0; i < plugin_dirs_str_.size(); ++i) {
            auto del_fn = [this, i]{ plugin_dirs_str_.erase(plugin_dirs_str_.begin() + i); RebuildLineView(); selected_ = std::min((int)editable_lines_.size() - 1, selected_); };
            editable_lines_.push_back({"plugin_dirs[" + std::to_string(i) + "]", &plugin_dirs_str_[i], false, nullptr, nullptr, nullptr, del_fn});
        }
        auto add_fn = [this]{ plugin_dirs_str_.push_back("<new_path>"); RebuildLineView(); selected_ = editable_lines_.size() - 2; EnterEditMode(); };
        editable_lines_.push_back({"(Plugin Dirs)", nullptr, false, nullptr, nullptr, add_fn, nullptr});
    }
    void EnterEditMode() {
        if (selected_ >= (int)editable_lines_.size()) return;
        const auto& line = editable_lines_[selected_];
        if (!line.is_radio && !line.value_ptr && !line.is_toggle) return;
        mode_ = Mode::Edit;
        if (line.is_radio) {
            RadioboxOption opt; opt.entries = line.radio_options; opt.selected = line.radio_selected_idx; opt.focused_entry = line.radio_selected_idx; radio_editor_ = Radiobox(opt); radio_editor_->TakeFocus();
        } else if (line.is_toggle && line.toggle_ptr) { *line.toggle_ptr = !*line.toggle_ptr; mode_ = Mode::Navigate; return; }
        else { edit_buffer_ = *line.value_ptr; editor_input_->TakeFocus(); }
    }
    void CommitEdit() {
        if (selected_ >= (int)editable_lines_.size()) return;
        const auto& line = editable_lines_[selected_];
        if (!line.is_radio && line.value_ptr) *line.value_ptr = edit_buffer_;
        mode_ = Mode::Navigate;
    }
    Element RenderStatusBar() {
        std::string help;
        if (mode_ == Mode::Navigate) {
            help = "↑/↓:Move | e:Edit | q:Quit | ::Command";
            if (selected_ < (int)editable_lines_.size()) { if (editable_lines_[selected_].add_fn) help += " | a:Add"; if (editable_lines_[selected_].del_fn) help += " | d:Delete"; }
        } else if (mode_ == Mode::Edit) help = "Enter:Accept | Esc:Cancel | ←/→:Change";
        else if (mode_ == Mode::Command) { return hbox({ text(":" + command_buffer_), text(" ") | blink, filler(), text("[a]pply | [w]rite | [q]uit | Esc:Cancel") | dim, }) | inverted; }
        return hbox({ text(help) | dim, filler(), text(status_message_) | bold });
    }
    void ExecuteCommand() {
        std::string cmd = command_buffer_;
        // Trim and lowercase to accept variants like ': a' or ':A'
        auto do_trim = [](std::string& s){
            size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; s.erase(0, i);
            size_t j = s.size(); while (j > 0 && std::isspace(static_cast<unsigned char>(s[j-1]))) --j; s.erase(j);
        };
        do_trim(cmd);
        for (auto& c : cmd) c = (char)std::tolower((unsigned char)c);
        command_buffer_.clear();
        mode_ = Mode::Navigate;
        if (cmd == "a" || cmd == "apply") {
            std::string new_path = active_config_path_buffer_;
            std::string old_path = original_config_.loaded_config_path;
            if (new_path != old_path) {
                if (fs::exists(new_path)) {
                    load_or_create_config(new_path, original_config_);
                    temp_config_ = original_config_;
                    SyncModelToUiState(); RebuildLineView(); status_message_ = "Loaded config from: " + new_path;
                } else {
                    SyncUiStateToModel(); temp_config_.loaded_config_path = fs::absolute(new_path).string(); original_config_ = temp_config_;
                    if (write_config_to_file(original_config_, new_path)) { status_message_ = "Created and applied new config: " + new_path; }
                    else { status_message_ = "Error creating file: " + new_path; original_config_.loaded_config_path = old_path; }
                    temp_config_ = original_config_; SyncModelToUiState(); RebuildLineView();
                }
            } else {
                SyncUiStateToModel(); original_config_ = temp_config_; status_message_ = "Settings applied to current session."; SyncModelToUiState(); RebuildLineView();
            }
            changes_applied = true;
        } else if (cmd == "w" || cmd == "write") {
            SyncUiStateToModel(); original_config_ = temp_config_;
            if(!original_config_.loaded_config_path.empty()){
                if (write_config_to_file(original_config_, original_config_.loaded_config_path)) { status_message_ = "Changes applied and saved."; changes_applied = true; }
                else { status_message_ = "Error: failed to write file."; }
            } else { status_message_ = "Error: No config file loaded. Use 'apply' with a new path first."; }
        } else if (cmd == "q" || cmd == "quit") { screen_.Exit(); }
        else { status_message_ = "Error: Unknown command '" + cmd + "'"; }
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

    // Traversal UI state
    int selected_traversal_tree_idx_ = 2; // 0=full,1=simplified,2=no_tree
    bool traversal_cache_m_ = false;
    bool traversal_cache_d_ = false;
    std::vector<std::string> traversal_tree_entries_ = {"full", "simplified", "no_tree"};
    std::vector<std::string> traversal_check_entries_ = {"none", "c", "cr"};
    int selected_traversal_check_idx_ = 0;

    // Compute defaults UI state
    bool compute_force_ = false;
    bool compute_force_deep_ = false;
    bool compute_parallel_ = false;
    bool compute_timer_console_ = false;
    bool compute_timer_log_ = false;
    bool compute_mute_ = false;
};

// Local helpers (replicated minimal UI ask functions for encapsulation)
static std::string ask(const std::string& q, const std::string& def) {
    std::cout << q; if (!def.empty()) std::cout << " [" << def << "]"; std::cout << ": ";
    std::string s; std::getline(std::cin, s); if (s.empty()) return def; return s;
}
static bool ask_yesno(const std::string& q, bool def) {
    std::string d = def ? "Y" : "n";
    while (true) { std::string s = ask(q + " [Y/n]", d); if (s.empty()) return def; if (s == "Y" || s == "y") return true; if (s == "N" || s == "n") return false; std::cout << "Please answer Y or n.\n"; }
}

void run_config_editor(CliConfig& config) {
    auto screen = ScreenInteractive::Fullscreen();
    ConfigEditor editor(screen, config);
    editor.Run();
    if (!editor.changes_applied) return;
    if (config.config_save_behavior == "ask") {
        if (ask_yesno("Save configuration changes to a file?", true)) {
            std::string default_path = config.loaded_config_path.empty() ? "config.yaml" : config.loaded_config_path;
            std::string path = ask("Enter path to save config file", default_path);
            if (!path.empty()) {
                if(write_config_to_file(config, path)) { config.loaded_config_path = fs::absolute(path).string(); std::cout << "Configuration saved to " << path << std::endl; }
                else { std::cout << "Error: Failed to save configuration to " << path << std::endl; }
            }
        }
    } else if (config.config_save_behavior == "current") {
        if (config.loaded_config_path.empty()) {
            std::cout << "Config save behavior is 'current', but no config file was loaded. Saving to default 'config.yaml'." << std::endl;
            if (write_config_to_file(config, "config.yaml")) { config.loaded_config_path = fs::absolute("config.yaml").string(); std::cout << "Configuration saved to " << config.loaded_config_path << std::endl; }
        } else {
            if (write_config_to_file(config, config.loaded_config_path)) { std::cout << "Configuration saved to " << config.loaded_config_path << std::endl; }
        }
    } else if (config.config_save_behavior == "default") {
        if (write_config_to_file(config, "config.yaml")) { config.loaded_config_path = fs::absolute("config.yaml").string(); std::cout << "Configuration saved to default 'config.yaml'." << std::endl; }
    }
}
