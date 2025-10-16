/**
 * @file benchmark_config_editor.cpp
 * @brief 提供交互式终端界面，用于加载、查看、编辑并保存基准测试配置。
 *
 * 本模块功能：
 *   - 操作名称标准化（SanitizeOpName）
 *   - 会话名称自动生成（GenerateSessionName）
 *   - 从 YAML 文件加载配置（load_benchmark_configs_from_file）
 *   - 将配置序列化并保存到 YAML（save_benchmark_configs_to_file）
 *   - 交互式配置编辑器（BenchmarkConfigEditor）：
 *       · 构造函数：初始化界面、加载现有配置、获取可用算子类型
 *       · Run()：启动主事件循环，处理保存、添加、删除、退出等操作
 *       · RebuildSessionList()：根据 configs_
 * 重建左侧会话列表，支持启用/禁用切换 ·
 * RebuildDetailsPane()：根据当前选中会话渲染右侧详情面板，支持自动生成与手动
 * YAML 模式 · SyncStatisticsModel()：同步统计复选框状态到当前会话配置 ·
 * AddNewSession()：插入一条默认参数的新会话 ·
 * DeleteSelectedSession()：删除当前选中会话 ·
 * SaveConfig()：自动更新会话名称并将所有配置写回磁盘 ·
 * RenderStatusBar()：渲染底部状态栏，显示提示信息及快捷键说明
 *   - 运行入口：run_benchmark_config_editor()
 *
 * 使用方式：
 *   run_benchmark_config_editor(interaction_service, "/path/to/benchmark_dir");
 *
 * 支持配置项：
 *   - Generator
 * Config：input_op_type、main_op_type、output_op_type、width、height、chain_length、num_outputs
 *   - Execution Config：runs、threads、parallel
 *   -
 * Statistics：total_time、typical_time、thread_count、io_time、scheduler_overhead
 */

#include "cli/benchmark_config_editor.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "kernel/interaction.hpp"

namespace fs = std::filesystem;
using namespace ftxui;

namespace ps {

// ... (顶部的 SanitizeOpName, GenerateSessionName, form 辅助函数保持不变) ...
/**
 * @brief 对操作名称进行标准化并返回简化后的名称
 *
 * 该函数首先检查输入字符串是否为空，若为空返回 "none"。否则提取冒号 (':')
 * 之后的子串作为简单名称， 并对特定的操作名称进行映射：
 *   - "gaussian_blur"    -> "gblur"
 *   - "constant"         -> "const"
 *   - "perlin_noise"     -> "perlin"
 *   - "curve_transform"  -> "curve"
 *   - "get_dimensions"   -> "dims"
 * 对于未列出的名称，直接返回提取后的子串。
 *
 * @param op_name 原始操作名称（可能包含前缀和冒号）
 * @return 标准化后的简化操作名称
 */
static std::string SanitizeOpName(const std::string& op_name) {
  if (op_name.empty())
    return "none";
  std::string simple_name = op_name.substr(op_name.find(':') + 1);
  if (simple_name == "gaussian_blur")
    return "gblur";
  if (simple_name == "constant")
    return "const";
  if (simple_name == "perlin_noise")
    return "perlin";
  if (simple_name == "curve_transform")
    return "curve";
  if (simple_name == "get_dimensions")
    return "dims";
  return simple_name;
}

/**
 * @brief 根据会话配置和基准目录名称生成唯一的会话名称
 *
 * 如果 cfg.auto_generate 为 false，则返回 cfg.name；
 * 否则从 cfg.generator_config 中提取输入算子类型、主算子类型、输出算子类型、
 * 分辨率(width x height)和链长度(chain_length)，对算子名称进行过滤，
 * 并按下列格式拼接生成会话名称：
 *   benchmark_dir_name-inputOp-mainOp-outputOp-resolution-chainLength
 *
 * @param cfg 会话配置，包含名称、自动生成开关及生成器相关配置
 * @param benchmark_dir_name 基准测试目录名称，用作生成的名称前缀
 * @return 生成的会话名称字符串
 */
static std::string GenerateSessionName(const BenchmarkSessionConfig& cfg,
                                       const std::string& benchmark_dir_name) {
  if (!cfg.auto_generate) {
    return cfg.name;
  }
  const auto& gen_cfg = cfg.generator_config;
  std::string input_op_sanitized = SanitizeOpName(gen_cfg.input_op_type);
  std::string main_op_sanitized = SanitizeOpName(gen_cfg.main_op_type);
  std::string output_op_sanitized = SanitizeOpName(gen_cfg.output_op_type);
  std::string resolution =
      std::to_string(gen_cfg.width) + "x" + std::to_string(gen_cfg.height);
  std::string chain = "L" + std::to_string(gen_cfg.chain_length);
  return benchmark_dir_name + "-" + input_op_sanitized + "-" +
         main_op_sanitized + "-" + output_op_sanitized + "-" + resolution +
         "-" + chain;
}

/**
 * @brief 根据给定的元素对列表生成双列表单布局
 *
 * 此函数接收一组 Element 二元组，其中每个二元组的第一个元素
 * 会被右对齐后放置于左列，第二个元素放置于右列。处理完成后，
 * 将左右两列组合为一个 gridbox 元素并返回。
 *
 * @param pairs 要生成表单的元素对列表，每对包含左列和右列的 Element
 * @return Element 生成的双列 gridbox 元素
 */
static Element form(std::vector<std::pair<Element, Element>> pairs) {
  Elements left;
  Elements right;
  for (auto& p : pairs) {
    left.push_back(p.first | align_right);
    right.push_back(p.second);
  }
  return gridbox({left, right});
}

// 加载函数已支持 parallel, runs, threads, 无需修改
/**
 * @brief 从 YAML 文件加载基准会话配置
 *
 * 该函数尝试读取指定路径的 YAML 文件，解析其中的 "sessions" 列表，
 * 并根据每个会话节点构建 BenchmarkSessionConfig 对象：
 *   - name：会话名称
 *   - enabled：是否启用（默认 true）
 *   - auto_generate：是否自动生成配置（默认 true）
 *     - 若 auto_generate 为 true，则从 "config" 节点读取生成器配置：
 *       input_op_type、main_op_type、output_op_type、
 *       width、height、chain_length、num_outputs
 *     - 否则读取 yaml_path 字段
 *   - execution：若存在 "execution" 节点，则读取 runs、threads、parallel
 *   - statistics：若存在且为序列，则读取为字符串列表
 *
 * 如果文件不存在或根节点中不含 "sessions" 列表，则返回空列表；
 * 若解析过程中发生异常，会将错误信息输出到 std::cerr 并返回空列表。
 *
 * @param path 要加载的配置文件路径
 * @return std::vector<BenchmarkSessionConfig> 已加载的基准会话配置列表
 */
static std::vector<BenchmarkSessionConfig> load_benchmark_configs_from_file(
    const fs::path& path) {
  std::vector<BenchmarkSessionConfig> configs;
  if (!fs::exists(path))
    return configs;
  try {
    YAML::Node root = YAML::LoadFile(path.string());
    if (!root["sessions"])
      return configs;
    for (const auto& session_node : root["sessions"]) {
      BenchmarkSessionConfig cfg;
      cfg.name = session_node["name"].as<std::string>();
      cfg.enabled = session_node["enabled"].as<bool>(true);
      cfg.auto_generate = session_node["auto_generate"].as<bool>(true);
      if (cfg.auto_generate) {
        const auto& gen_cfg = session_node["config"];
        cfg.generator_config.input_op_type =
            gen_cfg["input_op_type"].as<std::string>("");
        cfg.generator_config.main_op_type =
            gen_cfg["main_op_type"].as<std::string>("");
        cfg.generator_config.output_op_type =
            gen_cfg["output_op_type"].as<std::string>(
                "analyzer:get_dimensions");
        cfg.generator_config.width = gen_cfg["width"].as<int>(0);
        cfg.generator_config.height = gen_cfg["height"].as<int>(0);
        cfg.generator_config.chain_length = gen_cfg["chain_length"].as<int>(1);
        cfg.generator_config.num_outputs = gen_cfg["num_outputs"].as<int>(1);
      } else {
        cfg.yaml_path = session_node["yaml_path"].as<std::string>("");
      }
      if (session_node["execution"]) {
        cfg.execution.runs = session_node["execution"]["runs"].as<int>(10);
        cfg.execution.threads = session_node["execution"]["threads"].as<int>(0);
        cfg.execution.parallel =
            session_node["execution"]["parallel"].as<bool>(true);
      }
      if (session_node["statistics"] &&
          session_node["statistics"].IsSequence()) {
        cfg.statistics =
            session_node["statistics"].as<std::vector<std::string>>();
      }
      configs.push_back(cfg);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error loading benchmark config: " << e.what() << std::endl;
  }
  return configs;
}

// --- 关键修改点 1: 完善保存函数 ---
// 确保 execution 块的所有字段都被写回
/**
 * @brief 将给定的基准会话配置列表序列化并保存为 YAML 文件
 *
 * 本函数会构建一个包含多个会话节点 ("sessions") 的 YAML 文档，每个节点包括：
 *  - name：会话名称
 *  - enabled：是否启用该会话
 *  - auto_generate：是否自动生成测试数据
 *    - 若为 true，则将 generator_config 中的
 * input_op_type、main_op_type、output_op_type、
 *      width、height、chain_length、num_outputs 等字段写入子节点 "config"
 *    - 若为 false，则将 yaml_path 字段写入节点
 *  - execution：执行配置，包括
 * runs（运行次数）、threads（线程数）、parallel（并行标志）
 *  - statistics（可选）：统计数据列表，若非空则一并写入
 *
 * @param path    输出文件的路径（fs::path）
 * @param configs
 * 要保存的基准会话配置向量（std::vector<BenchmarkSessionConfig>）
 */
static void save_benchmark_configs_to_file(
    const fs::path& path, const std::vector<BenchmarkSessionConfig>& configs) {
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

    // [修改] 创建一个完整的 execution 节点
    YAML::Node exec_cfg;
    exec_cfg["runs"] = cfg.execution.runs;
    exec_cfg["threads"] = cfg.execution.threads;
    exec_cfg["parallel"] = cfg.execution.parallel;
    session_node["execution"] = exec_cfg;

    if (!cfg.statistics.empty()) {
      session_node["statistics"] = cfg.statistics;
    }
    sessions_node.push_back(session_node);
  }
  root["sessions"] = sessions_node;
  std::ofstream fout(path.string());
  fout << root;
}

/**
 * @class BenchmarkConfigEditor
 * @brief 提供交互式终端界面，用于编辑和管理基准测试配置。
 *
 * 该类基于 ftxui 库，实现会话列表与详细配置面板的联动，
 * 支持会话的添加、删除、启用/禁用切换，以及执行与统计选项的配置。
 *
 * 功能包括：
 *  - 加载并显示存储在 YAML 文件中的基准测试会话配置
 *  - 支持上下键、回车、字母键等事件操作
 *  - 实时更新界面和内部配置数据模型
 *  - 保存修改到磁盘
 *
 * 成员函数：
 *  - BenchmarkConfigEditor(ScreenInteractive&, ps::InteractionService&, const
 * std::string&) ·
 * 构造函数，初始化界面与服务引用，加载配置文件，构建初始会话列表 · @param
 * screen 终端交互屏幕实例 · @param svc 基准测试交互服务，用于获取可用操作类型
 *    · @param benchmark_dir 基准测试配置所在目录路径
 *
 *  - void Run()
 *    · 启动主事件循环，渲染会话列表、详情面板与状态栏
 *    · 监听全局键盘事件：Ctrl+C/q 退出，Ctrl+S 保存，a 新增会话，d 删除会话
 *
 * 私有成员函数：
 *  - void RebuildSessionList()
 *    · 根据当前 configs_ 重建会话列表组件
 *    · 支持回车键切换启用状态并刷新详情区域
 *
 *  - void RebuildDetailsPane()
 *    · 根据选中会话重建右侧详情面板组件
 *    · 动态切换“自动生成”与“手动 YAML”模式
 *    · 提供 Generator 配置、Execution 配置及统计选项的输入控件
 *
 *  - void SyncStatisticsModel()
 *    · 将统计选项复选框的变化同步到当前会话配置的 statistics 列表
 *
 *  - void AddNewSession()
 *    · 创建并插入一个默认配置的新会话，设置为选中状态
 *
 *  - void DeleteSelectedSession()
 *    · 删除当前选中会话，并更新选中索引与界面
 *
 *  - void SaveConfig()
 *    · 根据 auto_generate 标记自动生成会话名称
 *    · 将 configs_ 序列化并写入 YAML 文件
 *    · 刷新界面并更新状态提示
 *
 *  - Element RenderStatusBar()
 *    · 渲染底部状态栏，显示当前提示信息及快捷键说明
 *
 * 私有成员变量：
 *  - ScreenInteractive& screen_  交互式屏幕对象引用
 *  - ps::InteractionService& svc_ 交互服务引用，用于获取可用操作类型
 *  - std::string benchmark_dir_   基准测试配置目录
 *  - fs::path    config_path_     YAML 配置文件路径
 *  - std::vector<BenchmarkSessionConfig> configs_  会话配置列表
 *  - int selected_session_        当前选中会话索引
 *  - std::vector<std::string> session_entries_    会话名称及启用状态列表
 *  - std::string status_message_  状态栏提示信息
 *  - std::vector<std::string> available_ops_      可用操作类型列表
 *  - 输入框、下拉框、单选框、复选框等 UI 状态变量（如 input_op_selected_,
 * width_input_str_ 等）
 *  - const std::vector<std::string> statistics_options_  支持的统计项名称
 *  - std::map<std::string,bool> statistics_checked_map_  统计项状态映射
 *  - Component session_list_、details_pane_        UI 渲染组件
 */
class BenchmarkConfigEditor {
public:
  BenchmarkConfigEditor(ScreenInteractive& screen, ps::InteractionService& svc,
                        const std::string& benchmark_dir)
      : screen_(screen), svc_(svc), benchmark_dir_(benchmark_dir) {
    config_path_ = fs::path(benchmark_dir) / "benchmark_config.yaml";
    configs_ = load_benchmark_configs_from_file(config_path_);

    auto op_sources = svc_.cmd_ops_sources();
    for (const auto& pair : op_sources) {
      available_ops_.push_back(pair.first);
    }
    std::sort(available_ops_.begin(), available_ops_.end());

    RebuildSessionList();
  }

  void Run() {
    // ... (Run 函数无需修改) ...
    auto layout = Container::Horizontal({
        session_list_ | flex,
        Renderer([] { return separator(); }),
        details_pane_ | flex,
    });

    auto main_container = Renderer(layout, [this, &layout] {
      return vbox({text("Benchmark Configuration Editor: " + benchmark_dir_) |
                       bold | hcenter,
                   separator(), layout->Render(), separator(),
                   RenderStatusBar()}) |
             border;
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
  // ... (RebuildSessionList, SyncStatisticsModel, AddNewSession,
  // DeleteSelectedSession, SaveConfig, RenderStatusBar 等函数无需修改) ...
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
    menu |= CatchEvent([this](Event event) {
      if (event == Event::Return) {
        if (selected_session_ >= 0 &&
            selected_session_ < (int)configs_.size()) {
          configs_[selected_session_].enabled =
              !configs_[selected_session_].enabled;
          RebuildSessionList();
        }
        return true;
      }
      return false;
    });
    session_list_ = Renderer(menu, [this, menu] {
      return vbox({text("Sessions (Enter to toggle, 'a' add, 'd' del)") | bold,
                   separator(), menu->Render()}) |
             vscroll_indicator | frame;
    });
    RebuildDetailsPane();
  }
  void SyncStatisticsModel() {
    if (selected_session_ < 0 || selected_session_ >= (int)configs_.size())
      return;
    auto& stats_vec = configs_[selected_session_].statistics;
    stats_vec.clear();
    for (const auto& pair : statistics_checked_map_) {
      if (pair.second) {
        stats_vec.push_back(pair.first);
      }
    }
  }
  void AddNewSession() {
    BenchmarkSessionConfig new_cfg;
    new_cfg.generator_config.width = 256;
    new_cfg.generator_config.height = 256;
    if (!available_ops_.empty()) {
      new_cfg.generator_config.input_op_type = available_ops_[0];
      new_cfg.generator_config.main_op_type = available_ops_[0];
      new_cfg.generator_config.output_op_type = available_ops_[0];
    }
    new_cfg.name = GenerateSessionName(
        new_cfg, fs::path(benchmark_dir_).filename().string());
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
    for (auto& cfg : configs_) {
      if (cfg.auto_generate) {
        cfg.name = GenerateSessionName(
            cfg, fs::path(benchmark_dir_).filename().string());
      }
    }
    save_benchmark_configs_to_file(config_path_, configs_);
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

  // --- 关键修改点 2: 完善 RebuildDetailsPane ---
  void RebuildDetailsPane() {
    if (selected_session_ < 0 || selected_session_ >= (int)configs_.size()) {
      details_pane_ = Renderer([] {
        return text("No session selected or list is empty.") | center;
      });
      return;
    }

    auto& current_cfg = configs_[selected_session_];

    // ... (顶部的 UI 组件创建逻辑保持不变) ...
    static std::vector<std::string> auto_gen_options = {"Auto-generated",
                                                        "Manual YAML"};
    auto_gen_selected_ = current_cfg.auto_generate ? 0 : 1;
    RadioboxOption radio_opt;
    radio_opt.entries = &auto_gen_options;
    radio_opt.selected = &auto_gen_selected_;
    radio_opt.on_change = [this] {
      configs_[selected_session_].auto_generate = (auto_gen_selected_ == 0);
      RebuildDetailsPane();
    };
    auto auto_gen_radio = Radiobox(radio_opt);
    auto it_input = std::find(available_ops_.begin(), available_ops_.end(),
                              current_cfg.generator_config.input_op_type);
    input_op_selected_ = (it_input == available_ops_.end())
                             ? 0
                             : std::distance(available_ops_.begin(), it_input);
    auto it_main = std::find(available_ops_.begin(), available_ops_.end(),
                             current_cfg.generator_config.main_op_type);
    main_op_selected_ = (it_main == available_ops_.end())
                            ? 0
                            : std::distance(available_ops_.begin(), it_main);
    auto it_output = std::find(available_ops_.begin(), available_ops_.end(),
                               current_cfg.generator_config.output_op_type);
    output_op_selected_ =
        (it_output == available_ops_.end())
            ? 0
            : std::distance(available_ops_.begin(), it_output);
    DropdownOption input_dd_opt;
    input_dd_opt.radiobox.entries = &available_ops_;
    input_dd_opt.radiobox.selected = &input_op_selected_;
    input_dd_opt.radiobox.on_change = [this] {
      if (!available_ops_.empty())
        configs_[selected_session_].generator_config.input_op_type =
            available_ops_[input_op_selected_];
    };
    auto input_op_dropdown = Dropdown(input_dd_opt);
    DropdownOption main_dd_opt;
    main_dd_opt.radiobox.entries = &available_ops_;
    main_dd_opt.radiobox.selected = &main_op_selected_;
    main_dd_opt.radiobox.on_change = [this] {
      if (!available_ops_.empty())
        configs_[selected_session_].generator_config.main_op_type =
            available_ops_[main_op_selected_];
    };
    auto main_op_dropdown = Dropdown(main_dd_opt);
    DropdownOption output_dd_opt;
    output_dd_opt.radiobox.entries = &available_ops_;
    output_dd_opt.radiobox.selected = &output_op_selected_;
    output_dd_opt.radiobox.on_change = [this] {
      if (!available_ops_.empty())
        configs_[selected_session_].generator_config.output_op_type =
            available_ops_[output_op_selected_];
    };
    auto output_op_dropdown = Dropdown(output_dd_opt);
    width_input_str_ = std::to_string(current_cfg.generator_config.width);
    InputOption width_opt;
    width_opt.content = &width_input_str_;
    width_opt.on_change = [this] {
      try {
        configs_[selected_session_].generator_config.width =
            std::stoi(width_input_str_);
      } catch (...) {
      }
    };
    auto width_input = Input(width_opt);
    height_input_str_ = std::to_string(current_cfg.generator_config.height);
    InputOption height_opt;
    height_opt.content = &height_input_str_;
    height_opt.on_change = [this] {
      try {
        configs_[selected_session_].generator_config.height =
            std::stoi(height_input_str_);
      } catch (...) {
      }
    };
    auto height_input = Input(height_opt);
    chain_input_str_ =
        std::to_string(current_cfg.generator_config.chain_length);
    InputOption chain_opt;
    chain_opt.content = &chain_input_str_;
    chain_opt.on_change = [this] {
      try {
        configs_[selected_session_].generator_config.chain_length =
            std::stoi(chain_input_str_);
      } catch (...) {
      }
    };
    auto chain_input = Input(chain_opt);
    outputs_input_str_ =
        std::to_string(current_cfg.generator_config.num_outputs);
    InputOption outputs_opt;
    outputs_opt.content = &outputs_input_str_;
    outputs_opt.on_change = [this] {
      try {
        configs_[selected_session_].generator_config.num_outputs =
            std::stoi(outputs_input_str_);
      } catch (...) {
      }
    };
    auto outputs_input = Input(outputs_opt);
    auto manual_yaml_input =
        Input(&current_cfg.yaml_path, "relative/path/to/graph.yaml");

    // [新增] 为 execution.* 创建 UI 组件
    runs_input_str_ = std::to_string(current_cfg.execution.runs);
    InputOption runs_opt;
    runs_opt.content = &runs_input_str_;
    runs_opt.on_change = [this] {
      try {
        configs_[selected_session_].execution.runs = std::stoi(runs_input_str_);
      } catch (...) {
      }
    };
    auto runs_input = Input(runs_opt);

    threads_input_str_ = std::to_string(current_cfg.execution.threads);
    InputOption threads_opt;
    threads_opt.content = &threads_input_str_;
    threads_opt.on_change = [this] {
      try {
        configs_[selected_session_].execution.threads =
            std::stoi(threads_input_str_);
      } catch (...) {
      }
    };
    auto threads_input = Input(threads_opt);

    CheckboxOption parallel_opt;
    static std::string parallel_label = "Parallel Execution";
    parallel_opt.label = &parallel_label;
    parallel_opt.checked = &current_cfg.execution.parallel;
    auto parallel_checkbox = Checkbox(parallel_opt);
    // --- 新增结束 ---

    statistics_checked_map_.clear();
    for (const auto& opt : statistics_options_) {
      auto it = std::find(current_cfg.statistics.begin(),
                          current_cfg.statistics.end(), opt);
      statistics_checked_map_[opt] = (it != current_cfg.statistics.end());
    }
    Components statistics_checkboxes;
    for (const auto& opt_name : statistics_options_) {
      CheckboxOption cb_opt;
      cb_opt.label = &opt_name;
      cb_opt.checked = &statistics_checked_map_.at(opt_name);
      cb_opt.on_change = [this] { SyncStatisticsModel(); };
      statistics_checkboxes.push_back(Checkbox(cb_opt));
    }
    auto statistics_container = Container::Vertical(statistics_checkboxes);

    // [修改] 将新组件添加到容器中进行事件管理
    auto details_container = Container::Vertical({
        auto_gen_radio,
        input_op_dropdown,
        main_op_dropdown,
        output_op_dropdown,
        width_input,
        height_input,
        chain_input,
        outputs_input,
        manual_yaml_input,
        runs_input,
        threads_input,
        parallel_checkbox,  // <-- 新增
        statistics_container,
    });

    // [修改] 更新渲染逻辑以包含新的 execution 配置区域
    details_pane_ = Renderer(details_container, [=, &current_cfg] {
      auto details_form = form({
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
        pane = form({{text("  YAML Path: "), manual_yaml_input->Render()}});
      }

      // [新增] 为 execution 参数创建一个新的 pane
      auto execution_pane = form({
          {text("  Runs:     "), runs_input->Render()},
          {text("  Threads:  "), threads_input->Render()},
          {text("  Mode:     "), parallel_checkbox->Render()},
      });

      return vbox({
                 text("Session Details") | bold,
                 separator(),
                 details_form,
                 separator(),
                 pane,
                 separator(),
                 text("Execution Config") | bold,  // <-- 新增标题
                 execution_pane,                   // <-- 新增 Pane
                 separator(),
                 text("Statistics to collect:") | bold,
                 statistics_container->Render() | frame | vscroll_indicator |
                     size(HEIGHT, LESS_THAN, 5),
             }) |
             frame;
    });
  }

  // --- 关键修改点 3: 在类中添加新的成员变量 ---
  // UI State
  ScreenInteractive& screen_;
  ps::InteractionService& svc_;
  std::string benchmark_dir_;
  fs::path config_path_;
  std::vector<BenchmarkSessionConfig> configs_;

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
  std::string runs_input_str_;     // <-- 新增
  std::string threads_input_str_;  // <-- 新增

  int auto_gen_selected_ = 0;

  const std::vector<std::string> statistics_options_ = {
      "total_time", "typical_time", "thread_count", "io_time",
      "scheduler_overhead"};
  std::map<std::string, bool> statistics_checked_map_;

  Component session_list_ = Renderer([] { return text(""); });
  Component details_pane_ = Renderer([] { return text(""); });
};

// ... (文件底部的 run_benchmark_config_editor 函数保持不变) ...
/**
 * @brief 启动基准配置编辑器
 *
 * 该函数会检查指定的基准目录是否存在：
 *   - 如果目录不存在，则输出错误提示并创建该目录，
 *     随后生成一个默认的 benchmark_config.yaml 文件。
 *   - 如果目录已存在，则直接进入编辑流程。
 *
 * 编辑器以全屏模式运行，通过 BenchmarkConfigEditor 提供交互式界面
 * 允许用户查看、修改并保存基准测试配置。
 *
 * @param svc 引用交互服务对象，用于处理用户输入/输出交互
 * @param benchmark_dir 基准测试配置保存的目录路径
 */
void run_benchmark_config_editor(ps::InteractionService& svc,
                                 const std::string& benchmark_dir) {
  if (!fs::is_directory(benchmark_dir)) {
    std::cout << "Error: Benchmark directory not found: " << benchmark_dir
              << std::endl;
    std::cout << "Creating it with a default config..." << std::endl;
    fs::create_directories(benchmark_dir);
    save_benchmark_configs_to_file(
        fs::path(benchmark_dir) / "benchmark_config.yaml", {});
  }

  auto screen = ScreenInteractive::Fullscreen();
  BenchmarkConfigEditor editor(screen, svc, benchmark_dir);
  editor.Run();
}

}  // namespace ps