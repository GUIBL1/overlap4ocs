ROOT_DIR := ../..
HTSIM_SIM_DIR := $(ROOT_DIR)/third_party/csg-htsim/sim
SCHEMA_DIR := $(ROOT_DIR)/simulator/schemas
CONTRACT_DIR := $(ROOT_DIR)/simulator/contracts
PATCHSET_MANIFEST := $(ROOT_DIR)/third_party/csg-htsim/LOCAL_PATCHSET.json
BUILD_DIR := build
GENERATED_DIR := $(BUILD_DIR)/generated
GENERATED_CONTRACT := $(GENERATED_DIR)/generated_contract.h

CXX ?= c++
PYTHON ?= python
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude -Ithird_party -I$(GENERATED_DIR) -I$(HTSIM_SIM_DIR)

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
PARSER_OBJECTS := \
	$(BUILD_DIR)/sha256.o \
	$(BUILD_DIR)/ocs_execution_plan.o \
	$(BUILD_DIR)/ocs_plan_parser.o
DATAPLANE_OBJECTS := \
	$(BUILD_DIR)/ocs_packet_flow_bridge.o \
	$(BUILD_DIR)/ocs_packet.o \
	$(BUILD_DIR)/ocs_flow.o \
	$(BUILD_DIR)/ocs_execution_mode.o \
	$(BUILD_DIR)/ocs_switch.o \
	$(BUILD_DIR)/ocs_sink.o \
	$(BUILD_DIR)/ocs_route_table.o \
	$(BUILD_DIR)/ocs_port_serializer.o \
	$(BUILD_DIR)/ocs_plane_dataplane.o \
	$(BUILD_DIR)/ocs_topology.o
RUNTIME_OBJECTS := \
	$(BUILD_DIR)/ocs_trace_collector.o \
	$(BUILD_DIR)/ocs_flow_group.o \
	$(BUILD_DIR)/ocs_dependency_tracker.o \
	$(BUILD_DIR)/ocs_program_epoch.o \
	$(BUILD_DIR)/ocs_reconfiguration.o \
	$(BUILD_DIR)/ocs_plane_runtime.o \
	$(BUILD_DIR)/ocs_guards.o \
	$(BUILD_DIR)/ocs_watchdog.o \
	$(BUILD_DIR)/ocs_coordinator.o
RESULT_OBJECTS := \
	$(BUILD_DIR)/ocs_result_writer.o
CLI_OBJECT := $(BUILD_DIR)/main_ocs.o

TARGET := $(BUILD_DIR)/htsim_ocs
LINK_CLOSURE_TEST := $(BUILD_DIR)/test_link_closure
EVENTLIST_TEST := $(BUILD_DIR)/test_eventlist_order
PARSER_TEST := $(BUILD_DIR)/test_plan_parser
SHA256_TEST := $(BUILD_DIR)/test_sha256
PACKET_ADAPTER_TEST := $(BUILD_DIR)/test_packet_adapter
ROUTE_HANDOFF_TEST := $(BUILD_DIR)/test_route_handoff
STATIC_DATAPLANE_TEST := $(BUILD_DIR)/test_static_dataplane
RUNTIME_DRIVER_TEST := $(BUILD_DIR)/test_runtime_driver
RESULT_WRITER_TEST := $(BUILD_DIR)/test_result_writer
TEST_RUNNER_ARGS ?=

.PHONY: all clean test test-link-closure test-eventlist test-parser \
	test-packet-adapter test-dataplane test-coalesced-equivalence \
	test-epoch test-dependencies test-path-policies test-watchdog \
	test-phase06 phase06-performance-evidence \
	check-link-closure check-vendor core-link-probe

all: $(TARGET)

$(TARGET): $(SHIM_OBJECT) $(CORE_OBJECTS) $(PARSER_OBJECTS) \
	$(DATAPLANE_OBJECTS) $(RUNTIME_OBJECTS) $(RESULT_OBJECTS) $(CLI_OBJECT)
	$(CXX) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/htsim_ocs.map -o $@ $^ $(LDLIBS)

$(LINK_CLOSURE_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) $(BUILD_DIR)/test_link_closure.o
	$(CXX) $(LDFLAGS) -Wl,-Map,$(BUILD_DIR)/test_link_closure.map -o $@ $^ $(LDLIBS)

$(EVENTLIST_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) $(BUILD_DIR)/test_eventlist_order.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PARSER_TEST): $(PARSER_OBJECTS) $(BUILD_DIR)/test_plan_parser.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(SHA256_TEST): $(BUILD_DIR)/sha256.o $(BUILD_DIR)/test_sha256.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PACKET_ADAPTER_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) \
	$(BUILD_DIR)/ocs_packet_flow_bridge.o $(BUILD_DIR)/ocs_packet.o \
	$(BUILD_DIR)/ocs_flow.o \
	$(BUILD_DIR)/test_packet_adapter.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ROUTE_HANDOFF_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) \
	$(BUILD_DIR)/ocs_packet_flow_bridge.o $(BUILD_DIR)/ocs_packet.o \
	$(BUILD_DIR)/ocs_switch.o $(BUILD_DIR)/test_route_handoff.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(STATIC_DATAPLANE_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) $(PARSER_OBJECTS) \
	$(DATAPLANE_OBJECTS) $(BUILD_DIR)/test_static_dataplane.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(RUNTIME_DRIVER_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) $(PARSER_OBJECTS) \
	$(DATAPLANE_OBJECTS) $(RUNTIME_OBJECTS) \
	$(BUILD_DIR)/test_runtime_driver.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(RESULT_WRITER_TEST): $(SHIM_OBJECT) $(CORE_OBJECTS) $(PARSER_OBJECTS) \
	$(DATAPLANE_OBJECTS) $(RUNTIME_OBJECTS) $(RESULT_OBJECTS) \
	$(BUILD_DIR)/test_result_writer.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(GENERATED_CONTRACT): tools/generate_contract_header.py \
	$(SCHEMA_DIR)/swot-execution-plan-v2.schema.json \
	$(SCHEMA_DIR)/swot-simulation-result-v2.schema.json \
	$(SCHEMA_DIR)/htsim-ocs-operation-event-v1.schema.json \
	$(CONTRACT_DIR)/abi-limits-v2.json | $(GENERATED_DIR)
	$(PYTHON) tools/generate_contract_header.py \
		--plan-schema $(SCHEMA_DIR)/swot-execution-plan-v2.schema.json \
		--result-schema $(SCHEMA_DIR)/swot-simulation-result-v2.schema.json \
		--operation-schema $(SCHEMA_DIR)/htsim-ocs-operation-event-v1.schema.json \
		--abi-limits $(CONTRACT_DIR)/abi-limits-v2.json \
		--build-flags '$(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(LDLIBS)' \
		--output $@

$(BUILD_DIR)/main_ocs.o: src/main_ocs.cpp include/version.h \
	include/ocs_plan_parser.h $(GENERATED_CONTRACT) $(PATCHSET_MANIFEST) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(VERSION_CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/ocs_result_writer.o: src/ocs_result_writer.cpp \
	include/ocs_result_writer.h include/version.h $(GENERATED_CONTRACT) \
	$(PATCHSET_MANIFEST) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(VERSION_CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/htsim_logged_shim.o: src/htsim_logged_shim.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: src/%.cpp $(GENERATED_CONTRACT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_link_closure.o: tests/core_link_probe.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_eventlist_order.o: tests/test_eventlist_order.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_plan_parser.o: tests/test_plan_parser.cpp $(GENERATED_CONTRACT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_sha256.o: tests/test_sha256.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_packet_adapter.o: tests/test_packet_adapter.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_route_handoff.o: tests/test_route_handoff.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_static_dataplane.o: tests/test_static_dataplane.cpp \
	$(GENERATED_CONTRACT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_runtime_driver.o: tests/test_runtime_driver.cpp \
	$(GENERATED_CONTRACT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_result_writer.o: tests/test_result_writer.cpp \
	$(GENERATED_CONTRACT) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/core/%.o: $(HTSIM_SIM_DIR)/%.cpp | $(BUILD_DIR)/core
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR) $(BUILD_DIR)/core $(GENERATED_DIR):
	mkdir -p $@

test-link-closure: $(LINK_CLOSURE_TEST)
	$(LINK_CLOSURE_TEST)

core-link-probe: test-link-closure

test-eventlist: $(EVENTLIST_TEST)
	@reference=''; \
	for run_id in $$(seq 1 100); do \
		output="$$(for case_name in timed-fifo callback-insertion trigger-fifo cancel-reschedule endtime-boundary; do \
			$(EVENTLIST_TEST) $$case_name || exit 1; \
		done)" || exit 1; \
		if test $$run_id -eq 1; then reference="$$output"; \
		elif test "$$output" != "$$reference"; then \
			echo 'EventList output changed across fresh processes' >&2; exit 1; \
		fi; \
	done; \
	echo 'eventlist determinism: PASS (5 cases x 100 fresh processes)'

test-parser: $(PARSER_TEST) $(SHA256_TEST)
	$(SHA256_TEST)
	$(PARSER_TEST) $(ROOT_DIR)

test-packet-adapter: $(PACKET_ADAPTER_TEST) $(ROUTE_HANDOFF_TEST)
	$(PACKET_ADAPTER_TEST)
	@for case_name in accepted wrong-dst wrong-config wrong-epoch \
		wrong-generation wrong-plane time-overflow; do \
		$(ROUTE_HANDOFF_TEST) $$case_name || exit 1; \
	done

test-dataplane: $(STATIC_DATAPLANE_TEST)
	$(PYTHON) tests/run_dataplane_tests.py \
		--binary $(STATIC_DATAPLANE_TEST) --suite dataplane

test-coalesced-equivalence: $(STATIC_DATAPLANE_TEST)
	$(PYTHON) tests/run_dataplane_tests.py \
		--binary $(STATIC_DATAPLANE_TEST) --suite equivalence

test-epoch: $(RUNTIME_DRIVER_TEST)
	$(PYTHON) tests/run_runtime_tests.py \
		--binary $(RUNTIME_DRIVER_TEST) --suite epoch

test-dependencies: $(RUNTIME_DRIVER_TEST)
	$(PYTHON) tests/run_runtime_tests.py \
		--binary $(RUNTIME_DRIVER_TEST) --suite dependencies

test-path-policies: $(RUNTIME_DRIVER_TEST)
	$(PYTHON) tests/run_runtime_tests.py \
		--binary $(RUNTIME_DRIVER_TEST) --suite path-policies

test-watchdog: $(RUNTIME_DRIVER_TEST)
	$(PYTHON) tests/run_runtime_tests.py \
		--binary $(RUNTIME_DRIVER_TEST) --suite watchdog

test-phase06: all $(RESULT_WRITER_TEST)
	PYTHONPATH=$(ROOT_DIR) $(PYTHON) tests/test_standalone_cases.py \
		--binary $(TARGET)
	PYTHONPATH=$(ROOT_DIR) $(PYTHON) tests/test_failure_contract.py \
		--binary $(TARGET) --result-writer-driver $(RESULT_WRITER_TEST)
	PYTHONPATH=$(ROOT_DIR) $(PYTHON) tests/test_determinism.py \
		--binary $(TARGET)
	PYTHONPATH=$(ROOT_DIR) $(PYTHON) tests/test_performance_gate.py \
		--binary $(TARGET)

phase06-performance-evidence: all
	PYTHONPATH=$(ROOT_DIR) $(PYTHON) tests/test_performance_gate.py \
		--binary $(TARGET) \
		--write-evidence tests/artifacts/phase06/performance-evidence.json

check-link-closure: all $(LINK_CLOSURE_TEST)
	$(PYTHON) tests/run_tests.py \
		--binary $(TARGET) \
		--probe-binary $(LINK_CLOSURE_TEST) \
		--link-map $(BUILD_DIR)/htsim_ocs.map \
		--probe-link-map $(BUILD_DIR)/test_link_closure.map \
		$(TEST_RUNNER_ARGS)

test: all test-link-closure test-eventlist test-parser test-packet-adapter \
	test-dataplane test-coalesced-equivalence test-epoch test-dependencies \
	test-path-policies test-watchdog test-phase06 check-link-closure

check-vendor: all
	$(PYTHON) tests/run_tests.py --check-vendor --binary $(TARGET)

clean:
	$(RM) -r -- $(BUILD_DIR)

-include $(CLI_OBJECT:.o=.d) $(SHIM_OBJECT:.o=.d) \
	$(CORE_OBJECTS:.o=.d) $(PARSER_OBJECTS:.o=.d) \
	$(DATAPLANE_OBJECTS:.o=.d) $(RUNTIME_OBJECTS:.o=.d) \
	$(RESULT_OBJECTS:.o=.d) \
	$(BUILD_DIR)/test_link_closure.d $(BUILD_DIR)/test_eventlist_order.d \
	$(BUILD_DIR)/test_plan_parser.d $(BUILD_DIR)/test_sha256.d \
	$(BUILD_DIR)/test_packet_adapter.d $(BUILD_DIR)/test_route_handoff.d \
	$(BUILD_DIR)/test_static_dataplane.d $(BUILD_DIR)/test_runtime_driver.d \
	$(BUILD_DIR)/test_result_writer.d
