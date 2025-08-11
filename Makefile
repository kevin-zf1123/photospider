# 简易构建：需要 g++、pkg-config、OpenCV、yaml-cpp
# macOS (Homebrew): brew install opencv yaml-cpp pkg-config
# Ubuntu: sudo apt install libopencv-dev libyaml-cpp-dev pkg-config

# --- Project Paths ---
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
INC_DIR := $(PROJECT_ROOT)/include
BUILD_DIR := $(PROJECT_ROOT)/build
TARGET := $(BUILD_DIR)/graph_cli # MODIFIED: Defined the final executable path

# --- Compiler and Flags ---
# 可选：Homebrew 前缀（yaml-cpp）
YAML_PREFIX := $(shell brew --prefix yaml-cpp 2>/dev/null)

CXX ?= clang++
CXXFLAGS := -std=c++17 -O2 \
    -I$(INC_DIR) \
    $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv 2>/dev/null)

# 如果装了 yaml-cpp，用它的头/库路径
ifneq ($(YAML_PREFIX),)
CXXFLAGS += -I$(YAML_PREFIX)/include
LDFLAGS  += -L$(YAML_PREFIX)/lib
endif

LDFLAGS += $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv 2>/dev/null) -lyaml-cpp

# --- Source and Object Files ---
SRC := \
    src/ps_types.cpp \
    src/node.cpp \
    src/node_graph.cpp \
    src/ops.cpp

OBJ := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRC)) $(BUILD_DIR)/graph_cli.o

# --- Build Rules ---

# MODIFIED: The default 'all' rule now depends on the target in the build directory.
all: $(TARGET)

# 创建 build 目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 编译源文件到 build 目录
$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 编译 cli 源文件
$(BUILD_DIR)/graph_cli.o: cli/graph_cli.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# MODIFIED: The linking rule now creates the executable inside the build directory.
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

# MODIFIED: The clean rule now removes the root-level executable (if it exists)
# and the entire build directory.
clean:
	rm -rf $(BUILD_DIR) $(PROJECT_ROOT)/graph_cli

.PHONY: all clean