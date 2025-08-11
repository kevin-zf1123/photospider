# ===============================================================
# Photospider Project Makefile
#
# Builds the main executable and any external plugins found in custom_ops/.
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
ifeq ($(shell uname), Darwin)
	CXX ?= clang++
else
	CXX ?= g++
endif

CXXFLAGS := -std=c++17 -O2 -Wall \
            -I$(INC_DIR) \
            $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv 2>/dev/null)

YAML_PREFIX := $(shell brew --prefix yaml-cpp 2>/dev/null)
ifneq ($(YAML_PREFIX),)
	CXXFLAGS += -I$(YAML_PREFIX)/include
	LDFLAGS  += -L$(YAML_PREFIX)/lib
endif

LDFLAGS += $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv 2>/dev/null) -lyaml-cpp

PLUGIN_LDFLAGS := $(LDFLAGS)
ifeq ($(shell uname), Darwin)
	PLUGIN_LDFLAGS += -undefined dynamic_lookup
endif

# --- Main Application Sources ---
SRC := $(wildcard $(SRC_DIR)/*.cpp)
CLI_SRC := $(wildcard $(CLI_DIR)/*.cpp)
ALL_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/obj/%.o,$(SRC)) \
            $(patsubst $(CLI_DIR)/%.cpp,$(BUILD_DIR)/obj/%.o,$(CLI_SRC))

# --- NEW: Automatically discover all plugins ---
PLUGIN_SRC := $(wildcard $(PLUGIN_SRC_DIR)/*.cpp)
ALL_PLUGIN_TARGETS := $(patsubst $(PLUGIN_SRC_DIR)/%.cpp,$(PLUGIN_OUT_DIR)/%.so,$(PLUGIN_SRC))


# --- Build Rules ---
all: $(TARGET) $(ALL_PLUGIN_TARGETS)

# Create output directories
ALL_BUILD_DIRS := $(BUILD_DIR) $(BUILD_DIR)/obj $(PLUGIN_OUT_DIR)
$(ALL_BUILD_DIRS):
	@mkdir -p $@

# Linking rule for the main executable
$(TARGET): $(ALL_OBJS) | $(BUILD_DIR)
	@echo "Linking executable $@"
	$(CXX) $(CXXFLAGS) $(ALL_OBJS) -o $@ $(LDFLAGS)

# Compilation rules for main application source files
$(BUILD_DIR)/obj/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)/obj
	@echo "Compiling $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/%.o: $(CLI_DIR)/%.cpp | $(BUILD_DIR)/obj
	@echo "Compiling $< -> $@"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# --- NEW: A single, generic rule for ALL plugins ---
# This rule tells 'make' how to build any .so file in the plugin output directory
# from a corresponding .cpp file in the plugin source directory.
$(PLUGIN_OUT_DIR)/%.so: $(PLUGIN_SRC_DIR)/%.cpp | $(PLUGIN_OUT_DIR)
	@echo "Compiling plugin $< -> $@"
	$(CXX) $(CXXFLAGS) -shared -fPIC $< -o $@ $(PLUGIN_LDFLAGS)


# --- Cleanup ---
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)

# --- Phony Targets ---
.PHONY: all clean