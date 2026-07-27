ROOT_DIR := ../..
HTSIM_SIM_DIR := $(ROOT_DIR)/third_party/csg-htsim/sim
PATCHSET_MANIFEST := $(ROOT_DIR)/third_party/csg-htsim/LOCAL_PATCHSET.json
BUILD_DIR := build

CXX ?= c++
PYTHON ?= python
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude -I$(HTSIM_SIM_DIR)

PARENT_BUILD_COMMIT := $(shell git -C $(ROOT_DIR) rev-parse HEAD 2>/dev/null || printf unknown)
PARENT_BUILD_STATE := $(shell test -z "$$(git -C $(ROOT_DIR) status --porcelain 2>/dev/null)" && printf clean || printf dirty)
LOCAL_PATCHSET_SHA256 := $(shell sha256sum $(PATCHSET_MANIFEST) | awk '{print $$1}')

VERSION_CPPFLAGS := \
	-DHTSIM_OCS_PARENT_BUILD_COMMIT=\"$(PARENT_BUILD_COMMIT)\" \
	-DHTSIM_OCS_PARENT_BUILD_STATE=\"$(PARENT_BUILD_STATE)\" \
	-DHTSIM_OCS_LOCAL_PATCHSET_SHA256=\"$(LOCAL_PATCHSET_SHA256)\"

SHIM_OBJECT := $(BUILD_DIR)/htsim_logged_shim.o
CORE_OBJECTS := \
	$(BUILD_DIR)/core/eventlist.o \
	$(BUILD_DIR)/core/network.o \
	$(BUILD_DIR)/core/route.o \
	$(BUILD_DIR)/core/pipe.o \
	$(BUILD_DIR)/core/trigger.o
COMMON_OBJECTS := $(SHIM_OBJECT) $(CORE_OBJECTS)
CLI_OBJECT := $(BUILD_DIR)/main_ocs.o
PROBE_OBJECT := $(BUILD_DIR)/core_link_probe.o

TARGET := $(BUILD_DIR)/htsim_ocs
PROBE_TARGET := $(BUILD_DIR)/core_link_probe
TEST_RUNNER_ARGS ?=

.PHONY: all clean test check-vendor core-link-probe

all: $(TARGET)

$(TARGET): $(COMMON_OBJECTS) $(CLI_OBJECT)
	$(CXX) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/htsim_ocs.map -o $@ $^ $(LDLIBS)

$(PROBE_TARGET): $(COMMON_OBJECTS) $(PROBE_OBJECT)
	$(CXX) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/core_link_probe.map -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/main_ocs.o: src/main_ocs.cpp include/version.h $(PATCHSET_MANIFEST) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(VERSION_CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/htsim_logged_shim.o: src/htsim_logged_shim.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/core_link_probe.o: tests/core_link_probe.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/core/%.o: $(HTSIM_SIM_DIR)/%.cpp | $(BUILD_DIR)/core
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR) $(BUILD_DIR)/core:
	mkdir -p $@

core-link-probe: $(PROBE_TARGET)
	$(PROBE_TARGET)

test: all core-link-probe
	$(PYTHON) tests/run_tests.py \
		--binary $(TARGET) \
		--probe-binary $(PROBE_TARGET) \
		--link-map $(BUILD_DIR)/htsim_ocs.map \
		--probe-link-map $(BUILD_DIR)/core_link_probe.map \
		$(TEST_RUNNER_ARGS)

check-vendor: all
	$(PYTHON) tests/run_tests.py --check-vendor --binary $(TARGET)

clean:
	$(RM) -r -- $(BUILD_DIR)

-include $(CLI_OBJECT:.o=.d) $(PROBE_OBJECT:.o=.d) \
	$(SHIM_OBJECT:.o=.d) $(CORE_OBJECTS:.o=.d)
