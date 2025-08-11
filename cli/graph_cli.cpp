// FILE: cli/graph_cli.cpp
//
// --- MODIFICATIONS ---
//
// 1.  The `NodeEditor` class has been significantly refactored to fix the editing bug.
// 2.  It no longer uses `static` variables inside the `FlattenNode` method.
// 3.  It now uses a single, shared input component (`editor_input_`) for all fields,
//     mirroring the robust pattern used in working UI editors.
// 4.  The concept of "flattening" the node data into string member variables and
//     then "un-flattening" them upon applying changes has been properly implemented.
//
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
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace ps;
using namespace ftxui;


struct CliConfig {
    std::string loaded_config_path;
    std::string cache_root_dir = "cache";
    std::vector<std::string> plugin_dirs = {"build/plugins"};
    std::string default_print_mode = "detailed";
    std::string default_traversal_arg = "n";
    std::string default_cache_clear_arg = "md";
    std::string default_exit_save_path = "graph_out.yaml";
    bool exit_prompt_sync = true;
    std::string config_save_behavior = "current";
    std::string editor_save_behavior = "ask";
    std::string default_timer_log_path = "out/timer.yaml";
    std::string default_ops_list_mode = "all";
    std::string ops_plugin_path_mode = "name_only";
};


// Forward declarations
static bool write_config_to_file(const CliConfig& config, const std::string& path);
static void save_config_interactive(CliConfig& config);
static std::string ask(const std::string& q, const std::string& def = "");
// --- Interactive Config Editor ---
class ConfigEditor : public TuiEditor {
private:
    // Helper struct to define each editable line in the UI
    struct EditableLine {
        std::string label;
        std::string* value_ptr; // Points to the string being edited
        bool is_radio = false;
        std::vector<std::string>* radio_options = nullptr;
        int* radio_selected_idx = nullptr;
        std::function<void()> add_fn = nullptr;
        std::function<void()> del_fn = nullptr;
    };

public:
    ConfigEditor(ScreenInteractive& screen, CliConfig& config)
        : TuiEditor(screen), original_config_(config), temp_config_(config) 
    {
        // Set up the single, shared input component for text editing
        InputOption opt;
        opt.on_enter = [this] { CommitEdit(); };
        editor_input_ = Input(&edit_buffer_, "...", opt);
        
        // Initial sync from the config object to the editor's internal state
        SyncModelToStrings();
        RebuildLineView();
    }

    void Run() override {
        // The main container is now just a component that holds our renderer
        auto main_container = Container::Vertical({});

        auto component = Renderer(main_container, [&] {
            Elements line_elements;
            for(size_t i = 0; i < editable_lines_.size(); ++i) {
                auto& line = editable_lines_[i];
                Element display_element;

                if (mode_ == Mode::Edit && selected_ == i) {
                    if (line.is_radio) {
                        // In edit mode, render the Radiobox for radio button lines
                        auto radio = Radiobox(line.radio_options, line.radio_selected_idx);
                        display_element = radio->Render();
                    } else {
                        // Render the shared text input for normal lines
                        display_element = editor_input_->Render();
                    }
                } else {
                     if (line.is_radio) {
                        // In navigate mode, just show the selected text for radio buttons
                        display_element = text((*line.radio_options)[*line.radio_selected_idx]);
                     } else if (line.value_ptr) {
                        // Show the current string value for normal lines
                        display_element = text(*line.value_ptr);
                     } else {
                        // Placeholder for lines that are just for actions (like 'add')
                        display_element = text("...") | dim;
                     }
                }

                auto content = hbox({
                    text(line.label) | size(WIDTH, EQUAL, 25),
                    display_element | flex,
                });

                if (selected_ == i) content |= inverted;
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
            // --- Command Mode Logic ---
            if (mode_ == Mode::Command) {
                 if (event == Event::Return) { ExecuteCommand(); return true; }
                 if (event == Event::Escape) { mode_ = Mode::Navigate; command_buffer_.clear(); return true; }
                 if (event.is_character()) { command_buffer_ += event.character(); return true; }
                 if (event == Event::Backspace && !command_buffer_.empty()) { command_buffer_.pop_back(); return true; }
                 return false;
            }
            
            // --- Edit Mode Logic ---
            if (mode_ == Mode::Edit) {
                if (event == Event::Escape) { mode_ = Mode::Navigate; return true; }
                // For radio buttons, handle navigation and selection
                if (editable_lines_[selected_].is_radio) {
                    if (event == Event::ArrowLeft) { *editable_lines_[selected_].radio_selected_idx = std::max(0, *editable_lines_[selected_].radio_selected_idx - 1); return true; }
                    if (event == Event::ArrowRight) { *editable_lines_[selected_].radio_selected_idx = std::min((int)editable_lines_[selected_].radio_options->size() - 1, *editable_lines_[selected_].radio_selected_idx + 1); return true; }
                    if (event == Event::Return) { CommitEdit(); return true; }
                }
                // For text fields, delegate to the shared input component
                return editor_input_->OnEvent(event);
            }
            
            // --- Navigate Mode Logic ---
            if (event == Event::ArrowUp) { selected_ = std::max(0, selected_ - 1); return true; }
            if (event == Event::ArrowDown) { selected_ = std::min(int(editable_lines_.size() - 1), selected_ + 1); return true; }
            if (event == Event::Character('e')) { EnterEditMode(); return true; }
            if (event == Event::Character('a')) {
                if(selected_ < editable_lines_.size() && editable_lines_[selected_].add_fn) editable_lines_[selected_].add_fn();
                return true;
            }
            if (event == Event::Character('d')) {
                if(selected_ < editable_lines_.size() && editable_lines_[selected_].del_fn) editable_lines_[selected_].del_fn();
                return true;
            }
            if (event == Event::Character(':')) {
                mode_ = Mode::Command;
                command_buffer_.clear();
                return true;
            }
            // Pressing 'q' in navigate mode is a shortcut for the :q command
            if (event == Event::Character('q')) {
                screen_.Exit();
                return true;
            }
            return false;
        });

        screen_.Loop(component);
    }

private:
    void SyncModelToStrings() {
        // Find the index for each radio button setting
        auto find_idx = [](const auto& vec, const auto& val) {
            auto it = std::find(vec.begin(), vec.end(), val);
            return it == vec.end() ? 0 : std::distance(vec.begin(), it);
        };
        selected_print_mode_idx_ = find_idx(print_mode_entries_, temp_config_.default_print_mode);
        selected_ops_list_mode_idx_ = find_idx(ops_list_mode_entries_, temp_config_.default_ops_list_mode);
        selected_path_mode_idx_ = find_idx(path_mode_entries_, temp_config_.ops_plugin_path_mode);
        
        // Copy vector data
        plugin_dirs_str_ = temp_config_.plugin_dirs;
    }

    void SyncStringsToModel() {
        // Apply the selected radio button text back to the config object
        temp_config_.default_print_mode = print_mode_entries_[selected_print_mode_idx_];
        temp_config_.default_ops_list_mode = ops_list_mode_entries_[selected_ops_list_mode_idx_];
        temp_config_.ops_plugin_path_mode = path_mode_entries_[selected_path_mode_idx_];
        
        // Apply vector data back
        temp_config_.plugin_dirs = plugin_dirs_str_;
    }

    void RebuildLineView() {
        editable_lines_.clear();
        
        // Helper to add a standard text line
        auto add_text_line = [&](std::string label, std::string* value_ptr) {
            editable_lines_.push_back({std::move(label), value_ptr});
        };
        
        // Helper to add a radio button line
        auto add_radio_line = [&](std::string label, std::vector<std::string>* opts, int* selected_idx) {
            editable_lines_.push_back({std::move(label), nullptr, true, opts, selected_idx});
        };
        
        add_text_line("cache_root_dir", &temp_config_.cache_root_dir);
        add_text_line("default_exit_save_path", &temp_config_.default_exit_save_path);
        add_text_line("default_timer_log_path", &temp_config_.default_timer_log_path);
        
        add_radio_line("default_print_mode", &print_mode_entries_, &selected_print_mode_idx_);
        add_radio_line("default_ops_list_mode", &ops_list_mode_entries_, &selected_ops_list_mode_idx_);
        add_radio_line("ops_plugin_path_mode", &path_mode_entries_, &selected_path_mode_idx_);

        // Add a line for each plugin directory path
        for (size_t i = 0; i < plugin_dirs_str_.size(); ++i) {
            auto del_fn = [this, i] {
                plugin_dirs_str_.erase(plugin_dirs_str_.begin() + i);
                RebuildLineView();
                // Ensure selection is not out of bounds
                selected_ = std::min((int)editable_lines_.size() - 1, selected_);
            };
            editable_lines_.push_back({"plugin_dirs[" + std::to_string(i) + "]", &plugin_dirs_str_[i], false, nullptr, nullptr, nullptr, del_fn});
        }
        
        // Add a final line with an "add" action
        auto add_fn = [this] {
            plugin_dirs_str_.push_back("<new_path>");
            RebuildLineView();
            selected_ = editable_lines_.size() - 2; // Select the newly added line
            EnterEditMode();
        };
        editable_lines_.push_back({"(Plugin Dirs)", nullptr, false, nullptr, nullptr, add_fn, nullptr});
    }
    
    void EnterEditMode() {
        if (selected_ >= editable_lines_.size()) return;
        const auto& line = editable_lines_[selected_];

        // Do nothing if the line is not editable (e.g., the 'add' line)
        if (!line.is_radio && !line.value_ptr) return;

        mode_ = Mode::Edit;
        // For text fields, copy the current value to the shared buffer and focus the input
        if (!line.is_radio) {
            edit_buffer_ = *line.value_ptr;
            editor_input_->TakeFocus();
        }
    }
    
    void CommitEdit() {
        if (selected_ >= editable_lines_.size()) return;
        const auto& line = editable_lines_[selected_];
        
        if (!line.is_radio && line.value_ptr) {
            *line.value_ptr = edit_buffer_;
        }
        // For radio buttons, the change is already applied via the index pointer,
        // so we just need to switch modes.
        mode_ = Mode::Navigate;
    }

    Element RenderStatusBar() {
        std::string help;
        if (mode_ == Mode::Navigate) {
            help = "↑/↓:Move | e:Edit | q:Quit | ::Command";
            if (selected_ < editable_lines_.size()) {
                if (editable_lines_[selected_].add_fn) help += " | a:Add";
                if (editable_lines_[selected_].del_fn) help += " | d:Delete";
            }
        }
        else if (mode_ == Mode::Edit) help = "Enter:Accept | Esc:Cancel | ←/→:Change";
        else if (mode_ == Mode::Command) {
            // --- MODIFIED PART ---
            // Display command tips when in command mode.
            return hbox({
                       text(":" + command_buffer_),
                       text(" ") | blink,
                       filler(),
                       text("[a]pply | [w]rite | [q]uit | Esc:Cancel") | dim,
                   }) |
                   inverted;
        }
        
        return hbox({ text(help) | dim, filler(), text(status_message_) | bold });
    }

    void ExecuteCommand() {
        std::string cmd = command_buffer_;
        mode_ = Mode::Navigate;
        command_buffer_.clear();

        if (cmd == "a" || cmd == "apply") {
            SyncStringsToModel();
            status_message_ = "Changes applied to current session.";
        } else if (cmd == "w" || cmd == "write") {
            SyncStringsToModel();
            original_config_ = temp_config_;
            save_config_interactive(original_config_);
            status_message_ = "Changes applied and saved.";
        } else if (cmd == "q" || cmd == "quit") {
            screen_.Exit();
        } else {
            status_message_ = "Error: Unknown command '" + cmd + "'";
        }
    }

    CliConfig& original_config_; // The real config object
    CliConfig temp_config_;      // The copy we are editing
    std::string status_message_ = "Ready";

    // --- Editor State ---
    enum class Mode { Navigate, Edit, Command };
    Mode mode_ = Mode::Navigate;
    int selected_ = 0;
    std::vector<EditableLine> editable_lines_;
    std::string command_buffer_;
    
    // --- Buffer for text editing ---
    std::string edit_buffer_;
    Component editor_input_;

    // --- Buffers for radio buttons and vectors ---
    int selected_print_mode_idx_ = 0;
    int selected_ops_list_mode_idx_ = 0;
    int selected_path_mode_idx_ = 0;
    std::vector<std::string> plugin_dirs_str_;
    
    // --- Constant options for radio buttons ---
    std::vector<std::string> print_mode_entries_ = {"detailed", "simplified"};
    std::vector<std::string> ops_list_mode_entries_ = {"all", "builtin", "plugins"};
    std::vector<std::string> path_mode_entries_ = {"name_only", "relative_path", "absolute_path"};
};


// --- Interactive Node Editor (REFACTORED) ---
class NodeEditor : public TuiEditor {
private:
    // This helper struct defines the structure of each line in the editor UI.
    // It no longer contains a UI component.
    struct EditableLine {
        std::string label;
        std::string* value_ptr; // Pointer to the string representation in the editor's state
        std::function<void()> add_fn = nullptr;
        std::function<void()> del_fn = nullptr;
    };

public:
    NodeEditor(ScreenInteractive& screen, int node_id, NodeGraph& graph, CliConfig& config)
        : TuiEditor(screen), graph_(graph), node_id_(node_id), temp_node_(graph.nodes.at(node_id)), config_(config) 
    {
        // Configure the single, shared input component.
        InputOption opt;
        opt.on_enter = [this] {
            // On pressing enter, commit the buffer's content back to the string it was editing.
            if (selected_ < editable_lines_.size()) {
                *editable_lines_[selected_].value_ptr = edit_buffer_;
            }
            mode_ = Mode::Navigate;
        };
        editor_input_ = Input(&edit_buffer_, "...", opt);

        // Initial data synchronization
        SyncModelToStrings();
        RebuildLineView();
        RefreshTree();
    }
    
    void Run() override {
        // The container is now just a placeholder for the renderer to fill.
        auto main_container = Container::Vertical({});

        auto component = Renderer(main_container, [&] {
            Elements left_elements;
            for(size_t i = 0; i < editable_lines_.size(); ++i) {
                auto& line = editable_lines_[i];
                auto content = hbox({
                    text(line.label) | size(WIDTH, EQUAL, 25),
                    // Display the shared input component only on the selected line in edit mode
                    (selected_ == i && mode_ == Mode::Edit)
                        ? editor_input_->Render() | flex
                        : text(*line.value_ptr) | flex,
                });
                if (selected_ == i) content |= inverted;
                left_elements.push_back(content);
            }
            auto node_view = vbox(std::move(left_elements)) | vscroll_indicator | frame | flex;
            auto tree_view = vbox(tree_lines_) | vscroll_indicator | frame | flex;
            
            return vbox({
                hbox(node_view, separator(), tree_view),
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
                // If Esc is pressed, cancel the edit without saving from the buffer.
                if (event == Event::Escape) { mode_ = Mode::Navigate; return true; }
                // All other events go to the single shared input component.
                return editor_input_->OnEvent(event);
            }
            
            // --- Navigation Mode Logic ---
            if (event == Event::ArrowUp) { selected_ = std::max(0, selected_ - 1); return true; }
            if (event == Event::ArrowDown) { selected_ = std::min(int(editable_lines_.size() - 1), selected_ + 1); return true; }
            if (event == Event::Character('e')) { EnterEditMode(); return true; }
            if (event == Event::Character('a')) {
                if(editable_lines_[selected_].add_fn) editable_lines_[selected_].add_fn();
                return true;
            }
            if (event == Event::Character('d')) {
                if(editable_lines_[selected_].del_fn) editable_lines_[selected_].del_fn();
                return true;
            }
            if (event == Event::Character(':')) {
                mode_ = Mode::Command;
                command_buffer_.clear();
                return true;
            }
            if (event == Event::Character('q')) {
                // TODO: Add check for unsaved changes
                screen_.Exit();
                return true;
            }
            return false;
        });
        
        screen_.Loop(component);
    }

private:
    // Syncs the data from the `temp_node_` object into the editor's string member variables.
    void SyncModelToStrings() {
        name_str_ = temp_node_.name;
        type_str_ = temp_node_.type;
        subtype_str_ = temp_node_.subtype;

        YAML::Emitter emitter;
        emitter << temp_node_.parameters;
        params_yaml_str_ = emitter.c_str();

        image_inputs_str_.clear();
        for (const auto& input : temp_node_.image_inputs) {
            image_inputs_str_.emplace_back(std::to_string(input.from_node_id), input.from_output_name);
        }
    }

    // Syncs the data from the editor's string member variables back to the `temp_node_`.
    // This is where validation and parsing happens.
    void SyncStringsToModel() {
        temp_node_.name = name_str_;
        temp_node_.type = type_str_;
        temp_node_.subtype = subtype_str_;

        // It's critical to wrap parsing in a try-catch block
        try {
            temp_node_.parameters = YAML::Load(params_yaml_str_);

            temp_node_.image_inputs.clear();
            for (const auto& str_pair : image_inputs_str_) {
                ImageInput i;
                i.from_node_id = std::stoi(str_pair.first);
                i.from_output_name = str_pair.second;
                temp_node_.image_inputs.push_back(i);
            }
        } catch (const std::exception& e) {
            status_message_ = std::string("Parse Error: ") + e.what();
            // Revert the string that caused the error to its previous valid state.
            // A more robust implementation would be needed for a real application.
            throw; // Re-throw to abort the apply operation
        }
    }

    // Rebuilds the `editable_lines_` vector to reflect the current state of the string members.
    void RebuildLineView() {
        editable_lines_.clear();
        
        auto add_line = [&](std::string label, std::string* value_ptr, std::function<void()> add_fn, std::function<void()> del_fn) {
            editable_lines_.push_back({std::move(label), value_ptr, std::move(add_fn), std::move(del_fn)});
        };
        
        add_line("Name", &name_str_, nullptr, nullptr);
        add_line("Type", &type_str_, nullptr, nullptr);
        add_line("Subtype", &subtype_str_, nullptr, nullptr);
        add_line("Static Params (YAML)", &params_yaml_str_, nullptr, nullptr);
        
        for (size_t i = 0; i < image_inputs_str_.size(); ++i) {
            auto add_fn = [this, i] {
                image_inputs_str_.insert(image_inputs_str_.begin() + i + 1, {"-1", "image"});
                RebuildLineView();
            };
            auto del_fn = [this, i] {
                image_inputs_str_.erase(image_inputs_str_.begin() + i);
                RebuildLineView();
            };
            add_line("Img Input " + std::to_string(i) + " Node ID", &image_inputs_str_[i].first, add_fn, del_fn);
            add_line("Img Input " + std::to_string(i) + " Output", &image_inputs_str_[i].second, nullptr, nullptr);
        }
        // If there are no image inputs, provide a way to add the first one.
        if (image_inputs_str_.empty()) {
             add_line("(Image Inputs)", nullptr, [this]{
                image_inputs_str_.emplace_back("-1", "image");
                RebuildLineView();
             }, nullptr);
        }
    }
    
    void EnterEditMode() {
        if (selected_ >= editable_lines_.size() || !editable_lines_[selected_].value_ptr) return;
        mode_ = Mode::Edit;
        // Copy the current value into the shared edit buffer before editing starts.
        edit_buffer_ = *editable_lines_[selected_].value_ptr;
        editor_input_->TakeFocus();
    }
    
    void RefreshTree() {
        tree_lines_.clear();
        std::stringstream ss;
        graph_.print_dependency_tree(ss, node_id_, true);
        std::string line;
        while(std::getline(ss, line)) {
            tree_lines_.push_back(text(line));
        }
    }

    Element RenderStatusBar() {
        std::string help;
        if (mode_ == Mode::Navigate) {
            help = "↑/↓:Move | e:Edit | q:Quit | ::Command";
            if (selected_ < editable_lines_.size()) {
                if (editable_lines_[selected_].add_fn) help += " | a:Add";
                if (editable_lines_[selected_].del_fn) help += " | d:Delete";
            }
        }
        else if (mode_ == Mode::Edit) help = "Enter:Accept | Esc:Cancel";
        else if (mode_ == Mode::Command) {
            // --- MODIFIED PART ---
            // Display command tips when in command mode.
            return hbox({
                       text(":" + command_buffer_),
                       text(" ") | blink,
                       filler(),
                       text("[a]pply | [w]rite | [q]uit | Esc:Cancel") | dim,
                   }) |
                   inverted;
        }
        
        return hbox({ text(help) | dim, filler(), text(status_message_) | bold });
    }

    void ExecuteCommand() {
        std::string cmd = command_buffer_;
        mode_ = Mode::Navigate;
        command_buffer_.clear();

        if (cmd == "a" || cmd == "apply") HandleApply();
        else if (cmd == "w" || cmd == "write") {
            if (HandleApply()) {
                std::string path = ask("Output file", config_.default_exit_save_path);
                if (!path.empty()) { graph_.save_yaml(path); status_message_ = "Graph saved to " + path; }
            }
        } else if (cmd == "q" || cmd == "quit") {
            screen_.Exit();
        } else {
            status_message_ = "Error: Unknown command '" + cmd + "'";
        }
    }

    bool HandleApply() {
        try {
            // "Un-flatten" the strings back into the temporary node object.
            SyncStringsToModel();
        } catch (const std::exception&) {
            // status_message_ is already set by SyncStringsToModel on error
            return false;
        }
        
        // Create a temporary graph to perform a cycle check before committing.
        NodeGraph temp_graph = graph_;
        temp_graph.nodes.at(node_id_) = temp_node_;
        bool cycle_found = false;
        try {
            temp_graph.topo_postorder_from(node_id_);
        } catch (const GraphError&) {
            cycle_found = true;
            status_message_ = "Error: Cycle detected! Changes aborted.";
        }

        if (!cycle_found) {
            // If validation passes, commit the changes to the real graph.
            graph_.nodes.at(node_id_) = temp_node_;
            status_message_ = "Changes applied successfully.";
            RefreshTree();
            return true;
        }
        return false;
    }

    NodeGraph& graph_;
    int node_id_;
    ps::Node temp_node_;
    CliConfig& config_;
    
    // --- State for the editor UI ---
    std::vector<EditableLine> editable_lines_;
    Elements tree_lines_;
    std::string status_message_ = "Ready";
    int selected_ = 0;

    // --- State for string representations of node properties ---
    std::string name_str_;
    std::string type_str_;
    std::string subtype_str_;
    std::string params_yaml_str_;
    std::vector<std::pair<std::string, std::string>> image_inputs_str_;

    // --- Single shared component for editing ---
    std::string edit_buffer_;
    Component editor_input_;

    enum class Mode { Navigate, Edit, Command };
    Mode mode_ = Mode::Navigate;
    std::string command_buffer_;
};


static bool write_config_to_file(const CliConfig& config, const std::string& path) {
    YAML::Node root;
    root["_comment1"] = "Photospider CLI configuration.";
    root["cache_root_dir"] = config.cache_root_dir;
    root["plugin_dirs"] = config.plugin_dirs;
    root["default_print_mode"] = config.default_print_mode;
    root["default_traversal_arg"] = config.default_traversal_arg;
    root["default_cache_clear_arg"] = config.default_cache_clear_arg;
    root["default_exit_save_path"] = config.default_exit_save_path;
    root["exit_prompt_sync"] = config.exit_prompt_sync;
    root["config_save_behavior"] = config.config_save_behavior;
    root["editor_save_behavior"] = config.editor_save_behavior;
    root["default_timer_log_path"] = config.default_timer_log_path;
    root["default_ops_list_mode"] = config.default_ops_list_mode;
    root["ops_plugin_path_mode"] = config.ops_plugin_path_mode;

    try {
        std::ofstream fout(path);
        fout << root;
        // std::cout << "Successfully saved configuration to '" << path << "'." << std::endl;
        return true;
    } catch (const std::exception& e) {
        // std::cerr << "Error: Could not write to config file '" << path << "'. Error: " << e.what() << std::endl;
        return false;
    }
}

static void load_or_create_config(const std::string& config_path, CliConfig& config) {
    if (fs::exists(config_path)) {
        config.loaded_config_path = fs::absolute(config_path).string();
        try {
            YAML::Node root = YAML::LoadFile(config_path);
            if (root["cache_root_dir"]) config.cache_root_dir = root["cache_root_dir"].as<std::string>();
            
            if (root["plugin_dirs"] && root["plugin_dirs"].IsSequence()) {
                config.plugin_dirs = root["plugin_dirs"].as<std::vector<std::string>>();
            } else if (root["plugin_dir"] && root["plugin_dir"].IsScalar()) {
                config.plugin_dirs.clear();
                config.plugin_dirs.push_back(root["plugin_dir"].as<std::string>());
            }

            if (root["default_print_mode"]) config.default_print_mode = root["default_print_mode"].as<std::string>();
            if (root["default_traversal_arg"]) config.default_traversal_arg = root["default_traversal_arg"].as<std::string>();
            if (root["default_cache_clear_arg"]) config.default_cache_clear_arg = root["default_cache_clear_arg"].as<std::string>();
            if (root["default_exit_save_path"]) config.default_exit_save_path = root["default_exit_save_path"].as<std::string>();
            if (root["exit_prompt_sync"]) config.exit_prompt_sync = root["exit_prompt_sync"].as<bool>();
            if (root["config_save_behavior"]) config.config_save_behavior = root["config_save_behavior"].as<std::string>();
            if (root["editor_save_behavior"]) config.editor_save_behavior = root["editor_save_behavior"].as<std::string>();
            if (root["default_timer_log_path"]) config.default_timer_log_path = root["default_timer_log_path"].as<std::string>();
            if (root["default_ops_list_mode"]) config.default_ops_list_mode = root["default_ops_list_mode"].as<std::string>();
            if (root["ops_plugin_path_mode"]) config.ops_plugin_path_mode = root["ops_plugin_path_mode"].as<std::string>();
            std::cout << "Loaded configuration from '" << config_path << "'." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not parse config file '" << config_path << "'. Using default settings. Error: " << e.what() << std::endl;
        }
    } else if (config_path == "config.yaml") {
        std::cout << "Configuration file 'config.yaml' not found. Creating a default one." << std::endl;
        config.plugin_dirs = {"build/plugins"};
        config.editor_save_behavior = "ask";
        config.default_print_mode = "detailed";
        config.default_traversal_arg = "n";
        config.config_save_behavior = "current";
        config.default_timer_log_path = "out/timer.yaml";
        config.default_ops_list_mode = "all";
        config.ops_plugin_path_mode = "name_only";
        if (write_config_to_file(config, "config.yaml")) {
            config.loaded_config_path = fs::absolute("config.yaml").string();
        }
    }
}
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


static void print_repl_help() {
    std::cout << "Available REPL (interactive shell) commands:\n"
              << "  help                       Show this help message.\n"
              << "  clear                      Clear the terminal screen.\n"
              << "  config                     Open the interactive configuration editor.\n"
              << "  ops [mode]                 List all registered operations.\n"
              << "                             Modes: all(a), builtin(b), plugins(p)\n"
              << "  read <file>                Load a YAML graph from a file.\n"
              << "  source <file>              Execute commands from a script file.\n"
              << "  print [<id>|all] [mode]    Show the dependency tree. (Default: all)\n"
              << "                             Modes: detailed(d), simplified(s)\n"
              << "  node <id>                  Open the interactive editor for a single node.\n"
              << "  traversal [flags]          Show evaluation order with cache status and tree flags.\n"
              << "                             Tree Flags: detailed(d), simplified(s), no_tree(n)\n"
              << "                             Cache Flags: m(memory), d(disk), c(check), cr(check&remove)\n"
              << "  output <file>              Save the current graph to a YAML file.\n"
              << "  clear-graph                Clear the current in-memory graph.\n"
              << "  cc, clear-cache [d|m|md]   Clear on-disk, in-memory, or both caches.\n"
              << "  compute <id|all> [flags]   Compute node(s) with optional flags:\n"
              << "                             force: Re-compute even if cached.\n"
              << "                             t:     Print a simple timer summary to the console.\n"
              << "                             tl [path]: Log detailed timings to a YAML file.\n"
              << "  save <id> <file>           Compute a node and save its image output to a file.\n"
              << "  free                       Free memory used by non-essential intermediate nodes.\n"
              << "  exit                       Quit the shell.\n";
}

// ... (ask, ask_yesno, do_traversal, print_config, save_config_interactive are unchanged) ...
static std::string ask(const std::string& q, const std::string& def) {
    std::cout << q; if (!def.empty()) std::cout << " [" << def << "]";
    std::cout << ": "; std::string s; std::getline(std::cin, s);
    if (s.empty()) return def; return s;
}

static bool ask_yesno(const std::string& q, bool def = true) {
    std::string d = def ? "Y" : "n";
    while (true) {
        std::string s = ask(q + " [Y/n]", d); if (s.empty()) return def;
        if (s == "Y" || s == "y") return true; if (s == "N" || s == "n") return false;
        std::cout << "Please answer Y or n.\n";
    }
}
// --- Helper function for interactive saving logic ---
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
    // "manual" does nothing, user must use the old 'config' command to save.
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
                if (show_mem && node.cached_output.has_value()) {
                    statuses.push_back("in memory");
                }
                if (show_disk && !node.caches.empty()) {
                    bool on_disk = false;
                    for (const auto& cache : node.caches) {
                        fs::path cache_file = graph.node_cache_dir(node.id) / cache.location;
                        fs::path meta_file = cache_file; meta_file.replace_extension(".yml");
                        if (fs::exists(cache_file) || fs::exists(meta_file)) {
                            on_disk = true;
                            break;
                        }
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
// static void print_config(const CliConfig& config) {
//     std::cout << "Current CLI Configuration:\n"
//               << "  - loaded_config_path:      " << (config.loaded_config_path.empty() ? "(none)" : config.loaded_config_path) << "\n"
//               << "  - cache_root_dir:          " << config.cache_root_dir << "\n"
//               << "  - plugin_dirs:             \n";
//     if (config.plugin_dirs.empty()) {
//         std::cout << "    (none)\n";
//     } else {
//         for (const auto& dir : config.plugin_dirs) {
//             std::cout << "    - " << dir << "\n";
//         }
//     }
//     std::cout << "  - ops_plugin_path_mode:    " << config.ops_plugin_path_mode << "\n"
//               << "  - default_print_mode:      " << config.default_print_mode << "\n"
//               << "  - default_ops_list_mode:   " << config.default_ops_list_mode << "\n"
//               << "  - default_traversal_arg:   " << config.default_traversal_arg << "\n"
//               << "  - default_cache_clear_arg: " << config.default_cache_clear_arg << "\n"
//               << "  - default_exit_save_path:  " << config.default_exit_save_path << "\n"
//               << "  - exit_prompt_sync:        " << (config.exit_prompt_sync ? "true" : "false") << "\n"
//               << "  - config_save_behavior:    " << config.config_save_behavior << " (default action for the 'config' command)\n";
// }

static void save_config_interactive(CliConfig& config) {
    std::string def_char;
    std::string prompt = "Save updated configuration? [";
    
    if (config.config_save_behavior == "current" && !config.loaded_config_path.empty()) {
        prompt += "C]urrent/[d]efault/[a]sk/[n]one"; def_char = "c";
    } else if (config.config_save_behavior == "default") {
        prompt += "c]urrent/[D]efault/[a]sk/[n]one"; def_char = "d";
    } else if (config.config_save_behavior == "ask") {
        prompt += "c]urrent/[d]efault/[A]sk/[n]one"; def_char = "a";
    } else {
        prompt += "c]urrent/[d]efault/[a]sk/[N]one"; def_char = "n";
    }
    
    if (config.loaded_config_path.empty()) {
        prompt.replace(prompt.find("c]urrent"), 8, "(no current)");
        if (def_char == "c") def_char = "d";
    }
    prompt += "]";

    while(true) {
        std::string choice = ask(prompt, def_char);
        if (choice == "c" && !config.loaded_config_path.empty()) {
            write_config_to_file(config, config.loaded_config_path);
            break;
        } else if (choice == "d") {
            write_config_to_file(config, "config.yaml");
            break;
        } else if (choice == "a") {
            std::string path = ask("Enter path to save new config file");
            if (!path.empty()) write_config_to_file(config, path);
            break;
        } else if (choice == "n") {
            std::cout << "Configuration changes will not be saved." << std::endl;
            break;
        } else {
            std::cout << "Invalid choice." << std::endl;
        }
    }
}
void run_config_editor(CliConfig& config) {
    auto screen = ScreenInteractive::Fullscreen();
    ConfigEditor editor(screen, config);
    editor.Run();
}
void run_node_editor(int node_id, NodeGraph& graph, CliConfig& config) {
     auto screen = ScreenInteractive::Fullscreen();
     NodeEditor editor(screen, node_id, graph, config);
     editor.Run();
}

static bool process_command(const std::string& line, NodeGraph& graph, bool& modified, CliConfig& config, const std::map<std::string, std::string>& op_sources) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) return true;
    auto screen = ScreenInteractive::FitComponent();
    try {
        if (cmd == "help") {
            print_repl_help();
        } else if (cmd == "clear" || cmd == "cls") {
            std::cout << "\033[2J\033[1;1H";
        } else if (cmd == "print") {
            // --- NEW: Robust argument parsing for 'print' ---
            std::string target_str = "all";
            std::string mode_str = config.default_print_mode;
            bool target_is_set = false;

            std::string arg;
            while(iss >> arg) {
                // Check if the argument is a mode flag
                if (arg == "d" || arg == "detailed" || arg == "s" || arg == "simplified") {
                    mode_str = arg;
                } else { // Otherwise, assume it's a target
                    if (target_is_set) {
                         std::cout << "Warning: Multiple targets specified for print; using last one ('" << arg << "').\n";
                    }
                    target_str = arg;
                    target_is_set = true;
                }
            }

            bool show_params = (mode_str == "d" || mode_str == "detailed");

            if (target_str == "all") {
                graph.print_dependency_tree(std::cout, show_params);
            } else {
                try {
                    int node_id = std::stoi(target_str);
                    graph.print_dependency_tree(std::cout, node_id, show_params);
                } catch (const std::exception&) {
                    std::cout << "Error: Invalid target '" << target_str << "'. Must be an integer ID or 'all'." << std::endl;
                }
            }
        } else if (cmd == "node") {
            std::string id_str;
            iss >> id_str;
            if (id_str.empty()) { std::cout << "Usage: node <id>" << std::endl; return true; }
            try {
                int node_id = std::stoi(id_str);
                if (graph.has_node(node_id)) {
                    run_node_editor(node_id, graph, config);
                    modified = true; // Assume the user modified it
                } else {
                    std::cout << "Error: Node with ID " << node_id << " not found." << std::endl;
                }
            } catch (const std::exception&) {
                std::cout << "Error: Invalid node ID. Please provide an integer." << std::endl;
            }
        }  else if (cmd == "ops") {
            std::string mode_arg;
            iss >> mode_arg;
            if (mode_arg.empty()) {
                mode_arg = config.default_ops_list_mode;
            }

            std::string display_mode;
            std::string display_title;

            if (mode_arg == "all" || mode_arg == "a") {
                display_mode = "all";
                display_title = "all";
            } else if (mode_arg == "builtin" || mode_arg == "b") {
                display_mode = "builtin";
                display_title = "built-in";
            } else if (mode_arg == "plugins" || mode_arg == "custom" || mode_arg == "p" || mode_arg == "c") {
                display_mode = "plugins";
                display_title = "plugins";
            } else {
                std::cout << "Error: Invalid mode for 'ops'. Use: all (a), builtin (b), or plugins (p/c)." << std::endl;
                return true;
            }

            std::map<std::string, std::vector<std::pair<std::string, std::string>>> grouped_ops;
            int op_count = 0;

            for (const auto& pair : op_sources) {
                const std::string& key = pair.first;
                const std::string& source = pair.second;
                bool is_builtin = (source == "built-in");

                if ((display_mode == "builtin" && !is_builtin) || (display_mode == "plugins" && is_builtin)) {
                    continue;
                }

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
                        if (config.ops_plugin_path_mode == "absolute_path") {
                            display_path = plugin_path_str;
                        } else if (config.ops_plugin_path_mode == "relative_path") {
                            display_path = fs::relative(plugin_path_str).string();
                        } else { 
                            display_path = fs::path(plugin_path_str).filename().string();
                        }
                        std::cout << "  [plugin: " << display_path << "]";
                    }
                    std::cout << std::endl;
                }
            }

        } else if (cmd == "traversal") {
            std::string arg;
            std::string print_tree_mode = "none";
            bool show_mem = false, show_disk = false, do_check = false, do_check_remove = false;
            
            if (iss.rdbuf()->in_avail() == 0) {
                 std::istringstream default_iss(config.default_traversal_arg);
                 while(default_iss >> arg) {
                    if (arg == "d" || arg == "detailed") print_tree_mode = "detailed";
                    else if (arg == "s" || arg == "simplified") print_tree_mode = "simplified";
                    else if (arg == "n" || arg == "no_tree") print_tree_mode = "none";
                    else if (arg.find('m') != std::string::npos) show_mem = true;
                    else if (arg.find('d') != std::string::npos && arg != "detailed") show_disk = true;
                    else if (arg == "cr") do_check_remove = true;
                    else if (arg == "c") do_check = true;
                 }
            } else {
                while (iss >> arg) {
                    if (arg == "d" || arg == "detailed") print_tree_mode = "detailed";
                    else if (arg == "s" || arg == "simplified") print_tree_mode = "simplified";
                    else if (arg == "n" || arg == "no_tree") print_tree_mode = "none";
                    else if (arg.find('m') != std::string::npos) show_mem = true;
                    else if (arg.find('d') != std::string::npos && arg != "detailed") show_disk = true;
                    else if (arg == "cr") do_check_remove = true;
                    else if (arg == "c") do_check = true;
                }
            }

            if (do_check_remove) graph.synchronize_disk_cache();
            else if (do_check) graph.cache_all_nodes();

            if (print_tree_mode == "detailed") {
                graph.print_dependency_tree(std::cout, true);
            } else if (print_tree_mode == "simplified") {
                graph.print_dependency_tree(std::cout, false);
            }
            
            do_traversal(graph, show_mem, show_disk);
        } else if (cmd == "config") {
            run_config_editor(config);
        } 
        else if (cmd == "read") {
            std::string path; iss >> path; if (path.empty()) std::cout << "Usage: read <filepath>\n";
            else { graph.load_yaml(path); modified = false; std::cout << "Loaded graph from " << path << "\n"; }
        } else if (cmd == "source") {
            std::string filename; iss >> filename;
            if (filename.empty()) { std::cout << "Usage: source <filename>\n"; return true; }
            std::ifstream script_file(filename);
            if (!script_file) { std::cout << "Error: Cannot open script file: " << filename << "\n"; return true; }
            std::string script_line;
            while (std::getline(script_file, script_line)) {
                if (script_line.empty() || script_line.find_first_not_of(" \t") == std::string::npos || script_line[0] == '#') continue;
                std::cout << "ps> " << script_line << std::endl;
                if (!process_command(script_line, graph, modified, config, op_sources)) return false;
            }
        } 
        else if (cmd == "output") {
            std::string path; iss >> path; if (path.empty()) std::cout << "Usage: output <filepath>\n";
            else { graph.save_yaml(path); modified = false; std::cout << "Saved to " << path << "\n"; }
        } else if (cmd == "clear-graph") {
            graph.clear(); modified = true; std::cout << "Graph cleared.\n";
        } else if (cmd == "clear-cache" || cmd == "cc") {
            std::string arg;
            iss >> arg;
            if (arg.empty()) {
                arg = config.default_cache_clear_arg;
            }
            if (arg == "both" || arg == "md" || arg == "dm") graph.clear_cache();
            else if (arg == "drive" || arg == "d") graph.clear_drive_cache();
            else if (arg == "memory" || arg == "m") graph.clear_memory_cache();
        } 
        
        else if (cmd == "compute") {
            std::string target_id_str;
            iss >> target_id_str;
            if (target_id_str.empty()) {
                std::cout << "Usage: compute <id|all> [flags]\n";
                return true;
            }

            // Parse all flags and arguments
            bool force = false;
            bool timer_console = false; // 't' flag
            bool timer_log_file = false;  // 'tl' flag
            std::string timer_log_path = "";

            std::string arg;
            while (iss >> arg) {
                if (arg == "force") {
                    force = true;
                } else if (arg == "t" || arg == "timer") {
                    timer_console = true;
                } else if (arg == "tl") {
                    timer_log_file = true;
                    // Peek at the next argument. If it's not a flag, assume it's the path.
                    if (iss.peek() != EOF && iss.peek() != ' ') {
                        std::string next_arg;
                        iss >> next_arg;
                        if (next_arg != "force" && next_arg != "t" && next_arg != "timer") {
                            timer_log_path = next_arg;
                        } else {
                            // It was a flag, so put it back in the stream
                            iss.seekg(-(next_arg.length()), std::ios_base::cur);
                        }
                    }
                }
            }

            bool enable_timing = timer_console || timer_log_file;
            if (enable_timing) {
                graph.clear_timing_results();
            }
            
            // Start total timer if logging to file
            std::chrono::time_point<std::chrono::high_resolution_clock> total_start;
            if (timer_log_file) {
                total_start = std::chrono::high_resolution_clock::now();
            }

            auto print_output = [](int id, const NodeOutput& out) {
                std::cout << "-> Node " << id << " computed.\n";
                if (!out.image_matrix.empty()) { std::cout << "   Image Output: " << out.image_matrix.cols << "x" << out.image_matrix.rows << " (" << out.image_matrix.channels() << " ch)\n"; }
                if (!out.data.empty()) { 
                    std::cout << "   Data Outputs:\n";
                    YAML::Emitter yml;
                    yml << YAML::Flow << YAML::BeginMap;
                    for(const auto& pair : out.data) {
                        yml << YAML::Key << pair.first << YAML::Value << pair.second;
                    }
                    yml << YAML::EndMap;
                    std::cout << "     " << yml.c_str() << "\n"; 
                    }
            };

            // Execute computation
            if (target_id_str == "all") {
                for (int id : graph.ending_nodes()) {
                    print_output(id, graph.compute(id, force, enable_timing));
                }
            } else {
                int id = std::stoi(target_id_str);
                print_output(id, graph.compute(id, force, enable_timing));
            }

            // --- Post-computation actions based on flags ---

            // 1. Print simple summary to console if 't' was used
            if (timer_console) {
                std::cout << "--- Computation Timers (Console) ---\n";
                for (const auto& timing : graph.timing_results.node_timings) {
                    printf("  - Node %-3d (%-20s): %10.4f ms [%s]\n", 
                           timing.id, timing.name.c_str(), timing.elapsed_ms, timing.source.c_str());
                }
            }
            
            // 2. Write detailed log to file if 'tl' was used
            if (timer_log_file) {
                auto total_end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> total_elapsed = total_end - total_start;
                graph.timing_results.total_ms = total_elapsed.count();

                if (timer_log_path.empty()) {
                    timer_log_path = config.default_timer_log_path;
                }

                fs::path out_path(timer_log_path);
                if (out_path.has_parent_path()) {
                    fs::create_directories(out_path.parent_path());
                }

                YAML::Node root;
                YAML::Node steps_node(YAML::NodeType::Sequence);
                for (const auto& timing : graph.timing_results.node_timings) {
                    YAML::Node step;
                    step["id"] = timing.id;
                    step["name"] = timing.name;
                    step["time_ms"] = timing.elapsed_ms;
                    step["source"] = timing.source; 
                    steps_node.push_back(step);
                }

                root["steps"] = steps_node;
                root["total_time_ms"] = graph.timing_results.total_ms;
                
                std::ofstream fout(timer_log_path);
                fout << root;
                std::cout << "Timer log successfully written to '" << timer_log_path << "'." << std::endl;
            }

        } else if (cmd == "save") {
            std::string id_str, path; iss >> id_str >> path;
            if (id_str.empty() || path.empty()) { std::cout << "Usage: save <node_id> <filepath>\n"; }
            else {
                int id = std::stoi(id_str);
                const auto& result = graph.compute(id);
                if (result.image_matrix.empty()) { std::cout << "Error: Computed node " << id << " has no image output to save.\n"; }
                else if (cv::imwrite(path, result.image_matrix)) { std::cout << "Successfully saved node " << id << " image to " << path << "\n"; }
                else { std::cout << "Error: Failed to save image to " << path << "\n"; }
            }
        } else if (cmd == "free") {
            graph.free_transient_memory();
        } else if (cmd == "exit") {
            if (modified && ask_yesno("You have unsaved changes. Save graph to file?", true)) {
                std::string path = ask("output file", config.default_exit_save_path);
                graph.save_yaml(path); std::cout << "Saved to " << path << "\n";
            }
            if (ask_yesno("Synchronize disk cache with memory state before exiting?", config.exit_prompt_sync)) {
                graph.synchronize_disk_cache();
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

// ... (run_repl, load_plugins, main are unchanged)
static void run_repl(NodeGraph& graph, CliConfig& config, const std::map<std::string, std::string>& op_sources) {
    bool modified = false;
    std::string line;
    std::cout << "Photospider dynamic graph shell. Type 'help' for commands.\n";
    while (true) {
        std::cout << "ps> ";
        if (!std::getline(std::cin, line)) break;
        if (!process_command(line, graph, modified, config, op_sources)) break;
    }
}

static void load_plugins(const std::vector<std::string>& plugin_dir_paths, std::map<std::string, std::string>& op_sources) {
    auto& registry = ps::OpRegistry::instance();

    for (const auto& plugin_dir_path : plugin_dir_paths) {
        if (!fs::exists(plugin_dir_path) || !fs::is_directory(plugin_dir_path)) {
            continue;
        }

        std::cout << "Scanning for plugins in '" << plugin_dir_path << "'..." << std::endl;

        for (const auto& entry : fs::directory_iterator(plugin_dir_path)) {
            const auto& path = entry.path();
            
    #ifdef _WIN32
            const std::string extension = ".dll";
    #else
            const std::string extension = ".so";
    #endif
            if (path.extension() != extension) continue;

            std::cout << "  - Attempting to load plugin: " << path.filename().string() << std::endl;
            
            auto keys_before = registry.get_keys();
            
    #ifdef _WIN32
            HMODULE handle = LoadLibrary(path.string().c_str());
            if (!handle) {
                std::cerr << "    Error: Failed to load plugin. Code: " << GetLastError() << std::endl;
                continue;
            }
            using RegisterFunc = void(*)();
            RegisterFunc register_func = (RegisterFunc)GetProcAddress(handle, "register_photospider_ops");
            if (!register_func) {
                std::cerr << "    Error: Cannot find 'register_photospider_ops' export in plugin." << std::endl;
                FreeLibrary(handle);
                continue;
            }
    #else 
            void* handle = dlopen(path.c_str(), RTLD_LAZY);
            if (!handle) {
                std::cerr << "    Error: Failed to load plugin: " << dlerror() << std::endl;
                continue;
            }
            void (*register_func)();
            *(void**)(&register_func) = dlsym(handle, "register_photospider_ops");
            const char* dlsym_error = dlerror();
            if (dlsym_error) {
                std::cerr << "    Error: Cannot find 'register_photospider_ops' export in plugin: " << dlsym_error << std::endl;
                dlclose(handle);
                continue;
            }
    #endif
            
            try {
                register_func();
                auto keys_after = registry.get_keys();
                std::vector<std::string> new_keys;

                std::set_difference(keys_after.begin(), keys_after.end(),
                                    keys_before.begin(), keys_before.end(),
                                    std::back_inserter(new_keys));

                for (const auto& key : new_keys) {
                    op_sources[key] = fs::absolute(path).string();
                }
                
                std::cout << "    Success: Plugin loaded and " << new_keys.size() << " operation(s) registered." << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "    Error: An exception occurred during plugin registration: " << e.what() << std::endl;
    #ifdef _WIN32
                FreeLibrary(handle);
    #else
                dlclose(handle);
    #endif
            }
        }
    }
}

int main(int argc, char** argv) {
    std::map<std::string, std::string> op_sources;
    auto& registry = ps::OpRegistry::instance();

    auto keys_before_builtin = registry.get_keys();
    ops::register_builtin();
    auto keys_after_builtin = registry.get_keys();
    std::vector<std::string> builtin_keys;
    std::set_difference(keys_after_builtin.begin(), keys_after_builtin.end(),
                        keys_before_builtin.begin(), keys_before_builtin.end(),
                        std::back_inserter(builtin_keys));
    for (const auto& key : builtin_keys) {
        op_sources[key] = "built-in";
    }

    CliConfig config;
    std::string custom_config_path;

    const char* const short_opts = "hr:o:pt:R";
    const option long_opts[] = {
        {"help", no_argument, nullptr, 'h'}, {"read", required_argument, nullptr, 'r'},
        {"output", required_argument, nullptr, 'o'},
        {"print", no_argument, nullptr, 'p'}, {"traversal", no_argument, nullptr, 't'},
        {"clear-cache", no_argument, nullptr, 1001},
        {"repl", no_argument, nullptr, 'R'}, 
        {"config", required_argument, nullptr, 2001},
        {nullptr, 0, nullptr, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        if (opt == 2001) {
            custom_config_path = optarg;
        }
    }
    optind = 1;

    std::string config_to_load = custom_config_path.empty() ? "config.yaml" : custom_config_path;
    load_or_create_config(config_to_load, config);

    load_plugins(config.plugin_dirs, op_sources);

    NodeGraph graph{config.cache_root_dir};
    
    bool did_any_action = false;
    bool start_repl_after_actions = false;

    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) != -1) {
        try {
            switch (opt) {
            case 'h': print_cli_help(); return 0;
            case 'r': 
                graph.load_yaml(optarg); 
                std::cout << "Loaded graph from " << optarg << "\n"; 
                did_any_action = true; 
                break;
            case 'o': 
                graph.save_yaml(optarg); 
                std::cout << "Saved graph to " << optarg << "\n"; 
                did_any_action = true; 
                break;
            case 'p': 
                graph.print_dependency_tree(std::cout); 
                did_any_action = true; 
                break;
            case 't': 
                {
                    std::string print_tree_mode = "none";
                    bool show_mem = false, show_disk = false;
                    std::istringstream iss(config.default_traversal_arg);
                    std::string arg;
                     while(iss >> arg) {
                        if (arg == "d" || arg == "detailed") print_tree_mode = "detailed";
                        else if (arg == "s" || arg == "simplified") print_tree_mode = "simplified";
                        else if (arg == "n" || arg == "no_tree") print_tree_mode = "none";
                        else if (arg.find('m') != std::string::npos) show_mem = true;
                        else if (arg.find('d') != std::string::npos && arg != "detailed") show_disk = true;
                     }
                    if (print_tree_mode == "detailed") graph.print_dependency_tree(std::cout, true);
                    else if (print_tree_mode == "simplified") graph.print_dependency_tree(std::cout, false);
                    do_traversal(graph, show_mem, show_disk);
                }
                did_any_action = true; 
                break;
            case 1001: 
                graph.clear_cache(); 
                did_any_action = true; 
                break;
            case 'R': 
                start_repl_after_actions = true; 
                break;
            case 2001: /* Already handled */ break;
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
        run_repl(graph, config, op_sources);
    } else {
         std::cout << "\n--- Command-line actions complete. Exiting. ---\n";
    }
    
    return 0;
}