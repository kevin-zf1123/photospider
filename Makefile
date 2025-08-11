# ===============================================================
# Photospider Project Makefile
#
# Builds the main executable and any external plugins.
#
# Commands:
#   make         - Build the main application and all plugins.
#   make plugins - Build only the plugins.
#   make clean   - Remove all build artifacts.
# ===============================================================

# --- Project Paths ---
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
INC_DIR      := $(PROJECT_ROOT)/include
SRC_DIR      := $(PROJECT_ROOT)/src
CLI_DIR      := $(PROJECT_ROOT)/cli
BUILD_DIR    := $(PROJECT_ROOT)/build
PLUGIN_SRC_DIR := $(PROJECT_ROOT)/custom_ops
PLUGIN_OUT_DIR := $(BUILD_DIR)/plugins

# --- Targets ---
TARGET := $(BUILD_DIR)/graph_cli

# --- Compiler and Flags ---
# Use clang++ by default on macOS, g++ elsewhere.
ifeq ($(shell uname), Darwin)
	CXX ?= clang++
else
	CXX ?= g++
endif

# Compiler flags are shared between the main app and plugins
CXXFLAGS := -std=c++17 -O2 -Wall \
            -I$(INC_DIR) \
            $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv 2>/dev/null)

# Add Homebrew's yaml-cpp path if it exists
YAML_PREFIX := $(shell brew --prefix yaml-cpp 2>/dev/null)
ifneq ($(YAML_PREFIX),)
	CXXFLAGS += -I$(YAML_PREFIX)/include
	LDFLAGS  += -L$(YAML_PREFIX)/lib
endif

LDFLAGS += $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv 2>/dev/null) -lyaml-cpp


# --- Plugin Specific Flags ---
# On macOS (Darwin), we need to tell the linker that symbols from the main executable
# (like OpRegistry) will be available at runtime. This resolves "Undefined symbols" errors.
PLUGIN_LDFLAGS := $(LDFLAGS)
ifeq ($(shell uname), Darwin)
	PLUGIN_LDFLAGS += -undefined dynamic_lookup
endif


# --- Source and Object Files ---

# 1. Main Application Sources
SRC := $(wildcard $(SRC_DIR)/*.cpp)
OBJ := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/obj/%.o,$(SRC))
CLI_OBJ := $(patsubst $(CLI_DIR)/%.cpp,$(BUILD_DIR)/obj/%.o,$(wildcard $(CLI_DIR)/*.cpp))
ALL_OBJS := $(OBJ) $(CLI_OBJ)

# 2. Plugin Sources
PLUGIN_SRC := $(wildcard $(PLUGIN_SRC_DIR)/*.cpp)
PLUGIN_TARGETS := $(patsubst $(PLUGIN_SRC_DIR)/%.cpp,$(PLUGIN_OUT_DIR)/%.so,$(PLUGIN_SRC))


# --- Build Rules ---

# Default target: build the main executable and all plugins
all: $(TARGET) $(PLUGIN_TARGETS)

# Target to build only the plugins
plugins: $(PLUGIN_TARGETS)

# Create output directories as needed
$(BUILD_DIR) $(BUILD_DIR)/obj $(PLUGIN_OUT_DIR):
	@mkdir -p $@

# Linking rule for the main executable
$(TARGET): $(ALL_OBJS) | $(BUILD_DIR)
	@echo "Linking executable $@"
	$(CXX) $(CXXFLAGS) $(ALL_OBJS) -o $@ $(LDFLAGS)

# Compilation rule for main application source files
$(BUILD_DIR)/obj/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)/obj
	@echo "Compiling $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compilation rule for CLI source files
$(BUILD_DIR)/obj/%.o: $(CLI_DIR)/%.cpp | $(BUILD_DIR)/obj
	@echo "Compiling $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# --- Plugin Compilation Rules ---

# Generic rule to compile any plugin source into a shared library (.so)
$(PLUGIN_OUT_DIR)/%.so: $(PLUGIN_SRC_DIR)/%.cpp | $(PLUGIN_OUT_DIR)
	@echo "Compiling plugin $< -> $@"
	$(CXX) $(CXXFLAGS) -shared -fPIC $< -o $@ $(PLUGIN_LDFLAGS) # <-- MODIFIED to use plugin-specific flags


# --- Cleanup ---
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR) $(PROJECT_ROOT)/graph_cli

# --- Phony Targets ---
.PHONY: all clean plugins