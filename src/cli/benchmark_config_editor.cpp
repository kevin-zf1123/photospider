// FILE: src/cli/benchmark_config_editor.cpp (修改后)

#include "cli/benchmark_config_editor.hpp"
#include "benchmark/benchmark_types.hpp"
#include "kernel/interaction.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <algorithm>
#include <map>
#include <regex>

namespace fs = std::filesystem;
using namespace ftxui;

namespace ps {

// 辅助函数，用于简化操作名称以用于文件名/会话名
static std::string SanitizeOpName(const std::string& op_name) {
    if (op_name.empty()) return "none";
    std::string simple_name = op_name.substr(op_name.find(':') + 1);
    // 使用更简单的缩写
    if (simple_name == "gaussian_blur") return "gblur";
    if (simple_name == "constant") return "const";
    if (simple_name == "perlin_noise") return "perlin";
    if (simple_name == "curve_transform") return "curve";
    if (simple_name == "get_dimensions") return "dims";
    return simple_name;
}

// <--- 新增：统一的名称生成函数
static std::string GenerateSessionName(const BenchmarkSessionConfig& cfg, const std::string& benchmark_dir_name) {
    if (!cfg.auto_generate) {
        return cfg.name; // 对于手动配置，保持原名
    }
    const auto& gen_cfg = cfg.generator_config;
    std::string input_op_sanitized = SanitizeOpName(gen_cfg.input_op_type);
    std::string main_op_sanitized = SanitizeOpName(gen_cfg.main_op_type);
    std::string output_op_sanitized = SanitizeOpName(gen_cfg.output_op_type);
    std::string resolution = std::to_string(gen_cfg.width) + "x" + std::to_string(gen_cfg.height);
    std::string chain = "L" + std::to_string(gen_cfg.chain_length);

    return benchmark_dir_name + "-" + input_op_sanitized + "-" + main_op_sanitized +
           "-" + output_op_sanitized + "-" + resolution + "-" + chain;
}


// 布局辅助函数
static Element form(std::vector<std::pair<Element, Element>> pairs) {
    Elements left;
    Elements right;
    for (auto& p : pairs) {
        left.push_back(p.first | align_right);
        right.push_back(p.second);
    }
    return gridbox({
        left,
        right,
    });
}

// (load/save 函数与之前版本相同，此处为完整性保留)
static std::vector<BenchmarkSessionConfig> load_benchmark_configs_from_file(const fs::path& path) {
    std::vector<BenchmarkSessionConfig> configs;
    if (!fs::exists(path)) return configs;

    try {
        YAML::Node root = YAML::LoadFile(path.string());
        if (!root["sessions"]) return configs;
        for (const auto& session_node : root["sessions"]) {
            BenchmarkSessionConfig cfg;
            cfg.name = session_node["name"].as<std::string>();
            cfg.enabled = session_node["enabled"].as<bool>(true);
            cfg.auto_generate = session_node["auto_generate"].as<bool>(true);
            if (cfg.auto_generate) {
                const auto& gen_cfg = session_node["config"];
                cfg.generator_config.input_op_type = gen_cfg["input_op_type"].as<std::string>("");
                cfg.generator_config.main_op_type = gen_cfg["main_op_type"].as<std::string>("");
                cfg.generator_config.output_op_type = gen_cfg["output_op_type"].as<std::string>("analyzer:get_dimensions");
                cfg.generator_config.width = gen_cfg["width"].as<int>(0);
                cfg.generator_config.height = gen_cfg["height"].as<int>(0);
                cfg.generator_config.chain_length = gen_cfg["chain_length"].as<int>(1);
                cfg.generator_config.num_outputs = gen_cfg["num_outputs"].as<int>(1);
            } else {
                cfg.yaml_path = session_node["yaml_path"].as<std::string>("");
            }
            if (session_node["statistics"] && session_node["statistics"].IsSequence()) {
                cfg.statistics = session_node["statistics"].as<std::vector<std::string>>();
            }
            configs.push_back(cfg);
        }
    } catch(const std::exception& e) {
        std::cerr << "Error loading benchmark config: " << e.what() << std::endl;
    }
    return configs;
}

static void save_benchmark_configs_to_file(const fs::path& path, const std::vector<BenchmarkSessionConfig>& configs) {
    YAML::Node root;
    YAML::Node sessions_node(YAML::NodeType::Sequence);
    for (const auto& cfg : configs) {
        YAML::Node session_node;
        session_node["name"] = cfg.name;
        session_node["enabled"] = cfg.enabled;
        session_node["auto_generate"] = cfg.auto_generate;
        if (cfg.auto_generate) {
            YAML::Node gen_cfg;
            gen_cfg["input_op_type"] = cfg.generator_config.input_op_type;
            gen_cfg["main_op_type"] = cfg.generator_config.main_op_type;
            gen_cfg["output_op_type"] = cfg.generator_config.output_op_type;
            gen_cfg["width"] = cfg.generator_config.width;
            gen_cfg["height"] = cfg.generator_config.height;
            gen_cfg["chain_length"] = cfg.generator_config.chain_length;
            gen_cfg["num_outputs"] = cfg.generator_config.num_outputs;
            session_node["config"] = gen_cfg;
        } else {
            session_node["yaml_path"] = cfg.yaml_path;
        }
        if (!cfg.statistics.empty()) {
            session_node["statistics"] = cfg.statistics;
        }
        sessions_node.push_back(session_node);
    }
    root["sessions"] = sessions_node;
    std::ofstream fout(path.string());
    fout << root;
}


class BenchmarkConfigEditor {
public:
    BenchmarkConfigEditor(ScreenInteractive& screen, ps::InteractionService& svc, const std::string& benchmark_dir)
        : screen_(screen), svc_(svc), benchmark_dir_(benchmark_dir) {
        config_path_ = fs::path(benchmark_dir) / "benchmark_config.yaml";
        configs_ = load_benchmark_configs_from_file(config_path_);
        
        auto op_sources = svc_.cmd_ops_sources();
        for(const auto& pair : op_sources) {
            available_ops_.push_back(pair.first);
        }
        std::sort(available_ops_.begin(), available_ops_.end());
        
        RebuildSessionList();
    }

    void Run() {
        auto layout = Container::Horizontal({
            session_list_ | flex,
            Renderer([]{ return separator(); }),
            details_pane_ | flex,
        });

        auto main_container = Renderer(layout, [this, &layout] {
            return vbox({
                text("Benchmark Configuration Editor: " + benchmark_dir_) | bold | hcenter,
                separator(),
                layout->Render(),
                separator(),
                RenderStatusBar()
            }) | border;
        });

        main_container |= CatchEvent([this](Event event) {
            if (event == Event::Character('q') || event == Event::Special({"C-c"})) {
                screen_.Exit();
                return true;
            }
            if (event == Event::Character('s') || event == Event::Special({"C-s"})) {
                SaveConfig();
                return true;
            }
            if (event == Event::Character('a')) {
                AddNewSession();
                return true;
            }
            if (event == Event::Character('d')) {
                DeleteSelectedSession();
                return true;
            }
            return false;
        });

        screen_.Loop(main_container);
    }

private:
    void RebuildSessionList() {
        session_entries_.clear();
        for (const auto& cfg : configs_) {
            session_entries_.push_back((cfg.enabled ? "[x] " : "[ ] ") + cfg.name);
        }

        if (selected_session_ >= (int)configs_.size()) {
            selected_session_ = (int)configs_.size() - 1;
        }
        if (selected_session_ < 0) {
            selected_session_ = 0;
        }

        MenuOption option;
        option.entries = &session_entries_;
        option.selected = &selected_session_;
        option.on_change = [this] { RebuildDetailsPane(); };

        auto menu = Menu(option);
        menu |= CatchEvent([this](Event event){
            if (event == Event::Return) {
                if (selected_session_ >= 0 && selected_session_ < (int)configs_.size()) {
                    configs_[selected_session_].enabled = !configs_[selected_session_].enabled;
                    RebuildSessionList();
                }
                return true;
            }
            return false;
        });

        session_list_ = Renderer(menu, [this, menu]{
            return vbox({
                text("Sessions (Enter to toggle, 'a' add, 'd' del)") | bold,
                separator(),
                menu->Render()
            }) | vscroll_indicator | frame;
        });

        RebuildDetailsPane();
    }
    
    void SyncStatisticsModel() {
        if (selected_session_ < 0 || selected_session_ >= (int)configs_.size()) return;
        auto& stats_vec = configs_[selected_session_].statistics;
        stats_vec.clear();
        for(const auto& pair : statistics_checked_map_) {
            if (pair.second) {
                stats_vec.push_back(pair.first);
            }
        }
    }

    void RebuildDetailsPane() {
        if (selected_session_ < 0 || selected_session_ >= (int)configs_.size()) {
            details_pane_ = Renderer([] { return text("No session selected or list is empty.") | center; });
            return;
        }

        auto& current_cfg = configs_[selected_session_];
        
        // <--- 修改：不再需要 name_input 组件
        // auto name_input = Input(&current_cfg.name, "Name");
        
        static std::vector<std::string> auto_gen_options = {"Auto-generated", "Manual YAML"};
        auto_gen_selected_ = current_cfg.auto_generate ? 0 : 1;
        
        RadioboxOption radio_opt;
        radio_opt.entries = &auto_gen_options;
        radio_opt.selected = &auto_gen_selected_;
        radio_opt.on_change = [this] { 
            configs_[selected_session_].auto_generate = (auto_gen_selected_ == 0);
            RebuildDetailsPane();
        };
        auto auto_gen_radio = Radiobox(radio_opt);
        
        auto it_input = std::find(available_ops_.begin(), available_ops_.end(), current_cfg.generator_config.input_op_type);
        input_op_selected_ = (it_input == available_ops_.end()) ? 0 : std::distance(available_ops_.begin(), it_input);
        
        auto it_main = std::find(available_ops_.begin(), available_ops_.end(), current_cfg.generator_config.main_op_type);
        main_op_selected_ = (it_main == available_ops_.end()) ? 0 : std::distance(available_ops_.begin(), it_main);
        
        auto it_output = std::find(available_ops_.begin(), available_ops_.end(), current_cfg.generator_config.output_op_type);
        output_op_selected_ = (it_output == available_ops_.end()) ? 0 : std::distance(available_ops_.begin(), it_output);

        DropdownOption input_dd_opt;
        input_dd_opt.radiobox.entries = &available_ops_;
        input_dd_opt.radiobox.selected = &input_op_selected_;
        input_dd_opt.radiobox.on_change = [this]{ if (!available_ops_.empty()) configs_[selected_session_].generator_config.input_op_type = available_ops_[input_op_selected_]; };
        auto input_op_dropdown = Dropdown(input_dd_opt);

        DropdownOption main_dd_opt;
        main_dd_opt.radiobox.entries = &available_ops_;
        main_dd_opt.radiobox.selected = &main_op_selected_;
        main_dd_opt.radiobox.on_change = [this]{ if (!available_ops_.empty()) configs_[selected_session_].generator_config.main_op_type = available_ops_[main_op_selected_]; };
        auto main_op_dropdown = Dropdown(main_dd_opt);

        DropdownOption output_dd_opt;
        output_dd_opt.radiobox.entries = &available_ops_;
        output_dd_opt.radiobox.selected = &output_op_selected_;
        output_dd_opt.radiobox.on_change = [this]{ if (!available_ops_.empty()) configs_[selected_session_].generator_config.output_op_type = available_ops_[output_op_selected_]; };
        auto output_op_dropdown = Dropdown(output_dd_opt);


        width_input_str_ = std::to_string(current_cfg.generator_config.width);
        InputOption width_opt;
        width_opt.content = &width_input_str_;
        width_opt.on_change = [this]{ try { configs_[selected_session_].generator_config.width = std::stoi(width_input_str_); } catch(...) {} };
        auto width_input = Input(width_opt);

        height_input_str_ = std::to_string(current_cfg.generator_config.height);
        InputOption height_opt;
        height_opt.content = &height_input_str_;
        height_opt.on_change = [this]{ try { configs_[selected_session_].generator_config.height = std::stoi(height_input_str_); } catch(...) {} };
        auto height_input = Input(height_opt);
        
        chain_input_str_ = std::to_string(current_cfg.generator_config.chain_length);
        InputOption chain_opt;
        chain_opt.content = &chain_input_str_;
        chain_opt.on_change = [this]{ try { configs_[selected_session_].generator_config.chain_length = std::stoi(chain_input_str_); } catch(...) {} };
        auto chain_input = Input(chain_opt);

        outputs_input_str_ = std::to_string(current_cfg.generator_config.num_outputs);
        InputOption outputs_opt;
        outputs_opt.content = &outputs_input_str_;
        outputs_opt.on_change = [this]{ try { configs_[selected_session_].generator_config.num_outputs = std::stoi(outputs_input_str_); } catch(...) {} };
        auto outputs_input = Input(outputs_opt);
        
        auto manual_yaml_input = Input(&current_cfg.yaml_path, "relative/path/to/graph.yaml");

        statistics_checked_map_.clear();
        for(const auto& opt : statistics_options_) {
            auto it = std::find(current_cfg.statistics.begin(), current_cfg.statistics.end(), opt);
            statistics_checked_map_[opt] = (it != current_cfg.statistics.end());
        }

        Components statistics_checkboxes;
        for(const auto& opt_name : statistics_options_) {
            CheckboxOption cb_opt;
            cb_opt.label = &opt_name;
            cb_opt.checked = &statistics_checked_map_.at(opt_name);
            cb_opt.on_change = [this]{ SyncStatisticsModel(); };
            statistics_checkboxes.push_back(Checkbox(cb_opt));
        }
        auto statistics_container = Container::Vertical(statistics_checkboxes);

        auto details_container = Container::Vertical({
            // <--- 修改：移除 name_input
            auto_gen_radio,
            input_op_dropdown,
            main_op_dropdown,
            output_op_dropdown,
            width_input,
            height_input,
            chain_input,
            outputs_input,
            manual_yaml_input,
            statistics_container,
        });

        details_pane_ = Renderer(details_container, [=, &current_cfg] { // <--- 捕获 current_cfg 的引用
            auto details_form = form({
                // <--- 修改：将 Input 替换为 text
                {text("Name: "), text(current_cfg.name)},
                {text("Mode: "), auto_gen_radio->Render()},
            });

            Element pane;
            if (configs_[selected_session_].auto_generate) {
                pane = form({
                    {text("  Input Op: "), input_op_dropdown->Render()},
                    {text("  Main Op:  "), main_op_dropdown->Render()},
                    {text("  Output Op:"), output_op_dropdown->Render()},
                    {text("  Width:    "), width_input->Render()},
                    {text("  Height:   "), height_input->Render()},
                    {text("  Chain Len:"), chain_input->Render()},
                    {text("  Outputs:  "), outputs_input->Render()},
                });
            } else {
                pane = form({ {text("  YAML Path: "), manual_yaml_input->Render()} });
            }

            return vbox({
                text("Session Details") | bold,
                separator(),
                details_form,
                separator(),
                pane,
                separator(),
                text("Statistics to collect:") | bold,
                statistics_container->Render() | frame | vscroll_indicator | size(HEIGHT, LESS_THAN, 5),
            }) | frame;
        });
    }

    void AddNewSession() {
        BenchmarkSessionConfig new_cfg;
        // 设置合理的默认值
        new_cfg.generator_config.width = 256;
        new_cfg.generator_config.height = 256;

        if (!available_ops_.empty()) {
            new_cfg.generator_config.input_op_type = available_ops_[0];
            new_cfg.generator_config.main_op_type = available_ops_[0];
            new_cfg.generator_config.output_op_type = available_ops_[0];
        }
        
        // <--- 修改：使用新的辅助函数生成名称
        new_cfg.name = GenerateSessionName(new_cfg, fs::path(benchmark_dir_).filename().string());

        configs_.push_back(new_cfg);
        selected_session_ = (int)configs_.size() - 1;
        RebuildSessionList();
        status_message_ = "New session added. Press Ctrl+S to save.";
    }

    void DeleteSelectedSession() {
        if (selected_session_ >= 0 && selected_session_ < (int)configs_.size()) {
            configs_.erase(configs_.begin() + selected_session_);
            RebuildSessionList();
            status_message_ = "Session deleted. Press Ctrl+S to save.";
        }
    }

    void SaveConfig() {
        // <--- 修改：在保存前更新所有自动生成会话的名称
        for (auto& cfg : configs_) {
            if (cfg.auto_generate) {
                cfg.name = GenerateSessionName(cfg, fs::path(benchmark_dir_).filename().string());
            }
        }
        
        save_benchmark_configs_to_file(config_path_, configs_);

        // <--- 修改：刷新UI以显示更新后的名称
        RebuildSessionList();
        RebuildDetailsPane();
        status_message_ = "Configuration saved to " + config_path_.string();
    }

    Element RenderStatusBar() {
        return hbox({
            text(" " + status_message_),
            filler(),
            text("Ctrl+S: Save | Ctrl+C/q: Quit ") | inverted,
        });
    }

    ScreenInteractive& screen_;
    ps::InteractionService& svc_;
    std::string benchmark_dir_;
    fs::path config_path_;
    std::vector<BenchmarkSessionConfig> configs_;
    
    // UI State
    int selected_session_ = 0;
    std::vector<std::string> session_entries_;
    std::string status_message_ = "Ready";

    std::vector<std::string> available_ops_;
    int input_op_selected_ = 0;
    int main_op_selected_ = 0;
    int output_op_selected_ = 0;
    std::string width_input_str_;
    std::string height_input_str_;
    std::string chain_input_str_;
    std::string outputs_input_str_;

    int auto_gen_selected_ = 0;

    const std::vector<std::string> statistics_options_ = {
        "total_time", "typical_time", "thread_count", "io_time", "scheduler_overhead"
    };
    std::map<std::string, bool> statistics_checked_map_;

    // Components
    Component session_list_ = Renderer([]{ return text(""); });
    Component details_pane_ = Renderer([]{ return text(""); });
};


void run_benchmark_config_editor(ps::InteractionService& svc, const std::string& benchmark_dir) {
    if (!fs::is_directory(benchmark_dir)) {
        std::cout << "Error: Benchmark directory not found: " << benchmark_dir << std::endl;
        std::cout << "Creating it with a default config..." << std::endl;
        fs::create_directories(benchmark_dir);
        save_benchmark_configs_to_file(fs::path(benchmark_dir) / "benchmark_config.yaml", {});
    }

    auto screen = ScreenInteractive::Fullscreen();
    BenchmarkConfigEditor editor(screen, svc, benchmark_dir);
    editor.Run();
}

} // namespace ps