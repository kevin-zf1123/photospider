# ===============================================================
# Photospider Project Makefile (Final Version)
# ===============================================================

# --- Project Paths ---
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
INC_DIR      := $(PROJECT_ROOT)/include
SRC_DIR      := $(PROJECT_ROOT)/src
CLI_DIR      := $(PROJECT_ROOT)/cli
BUILD_DIR    := $(PROJECT_ROOT)/build
PLUGIN_SRC_DIR := $(PROJECT_ROOT)/custom_ops
PLUGIN_OUT_DIR := $(BUILD_DIR)/plugins

# Paths for FTXUI dependency
FTXUI_SRC_ROOT  := $(PROJECT_ROOT)/extern/ftxui/src
FTXUI_BUILD_DIR := $(BUILD_DIR)/obj/ftxui

# --- Targets ---
TARGET := $(BUILD_DIR)/graph_cli
FTXUI_LIB := $(BUILD_DIR)/libftxui.a

# --- Compiler and Flags ---
ifeq ($(shell uname), Darwin)
	CXX ?= clang++
else
	CXX ?= g++
endif

# Add FTXUI's own src dir to includes for its internal headers
CXXFLAGS := -std=c++17 -O2 -Wall \
            -I$(INC_DIR) \
            -I$(PROJECT_ROOT)/extern/ftxui/include \
            -I$(FTXUI_SRC_ROOT) \
            $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv 2>/dev/null)

YAML_PREFIX := $(shell brew --prefix yaml-cpp 2>/dev/null)
ifneq ($(YAML_PREFIX),)
	CXXFLAGS += -I$(YAML_PREFIX)/include
	LDFLAGS  += -L$(YAML_PREFIX)/lib
endif

# Link against OpenCV, yaml-cpp, and our own compiled FTXUI library
LDFLAGS += $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv 2>/dev/null) \
           -lyaml-cpp \
           -L$(BUILD_DIR) -lftxui

PLUGIN_LDFLAGS := $(LDFLAGS)
ifeq ($(shell uname), Darwin)
	PLUGIN_LDFLAGS += -undefined dynamic_lookup
endif

# --- Source & Object File Definitions ---

# 1. Application Sources and Objects
APP_SRC := $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(CLI_DIR)/*.cpp)
APP_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/obj/%.o,$(notdir $(APP_SRC)))
VPATH := $(SRC_DIR):$(CLI_DIR) # VPATH is now safe as it's only for the app

# 2. FTXUI Library Sources and Objects
ALL_FTXUI_FILES := $(wildcard $(FTXUI_SRC_ROOT)/ftxui/component/*.cpp) \
                   $(wildcard $(FTXUI_SRC_ROOT)/ftxui/dom/*.cpp) \
                   $(wildcard $(FTXUI_SRC_ROOT)/ftxui/screen/*.cpp) \
                   $(wildcard $(FTXUI_SRC_ROOT)/ftxui/util/*.cpp)
FTXUI_SRC := $(filter-out %_test.cpp %_fuzzer.cpp %_benchmark.cpp, $(ALL_FTXUI_FILES))
FTXUI_OBJS := $(patsubst $(FTXUI_SRC_ROOT)/%.cpp,$(FTXUI_BUILD_DIR)/%.o,$(FTXUI_SRC))

# 3. Plugin Sources and Targets
PLUGIN_SRC := $(wildcard $(PLUGIN_SRC_DIR)/*.cpp)
ALL_PLUGIN_TARGETS := $(patsubst $(PLUGIN_SRC_DIR)/%.cpp,$(PLUGIN_OUT_DIR)/%.so,$(PLUGIN_SRC))

# --- Build Rules ---
all: $(TARGET) $(ALL_PLUGIN_TARGETS)

# Create all necessary build directories
ALL_BUILD_DIRS := $(BUILD_DIR) $(BUILD_DIR)/obj $(FTXUI_BUILD_DIR) $(PLUGIN_OUT_DIR)
$(ALL_BUILD_DIRS):
	@mkdir -p $@

# Rule to build the FTXUI static library
$(FTXUI_LIB): $(FTXUI_OBJS)
	@echo "Archiving FTXUI static library: $@"
	ar rcs $@ $^

# Rule to build the final executable
# Depends on app object files and the FTXUI static library
$(TARGET): $(APP_OBJS) $(FTXUI_LIB)
	@echo "Linking executable: $@"
	$(CXX) $(APP_OBJS) -o $@ $(LDFLAGS)

# Rule for compiling application files (using VPATH)
$(BUILD_DIR)/obj/%.o: %.cpp | $(BUILD_DIR)/obj
	@echo "Compiling APP: $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule for compiling FTXUI library files (no VPATH needed)
$(FTXUI_BUILD_DIR)/%.o: $(FTXUI_SRC_ROOT)/%.cpp | $(FTXUI_BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "Compiling LIB: $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule for compiling plugins
$(PLUGIN_OUT_DIR)/%.so: $(PLUGIN_SRC_DIR)/%.cpp | $(PLUGIN_OUT_DIR)
	@echo "Compiling PLUGIN: $< -> $@"
	$(CXX) $(CXXFLAGS) -shared -fPIC $< -o $@ $(PLUGIN_LDFLAGS)

clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)

.PHONY: all clean