.DEFAULT_GOAL := all

CXX ?= g++
AR ?= ar
PYTHON ?= python3
BUILD_DIR ?= build
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=
# Match CMake Debug ASan by default. GCC 13 libstdc++ <regex> emits
# false maybe-uninitialized diagnostics with -O1 + ASan (GCC PR105616),
# also through Valijson. Keep -Werror and all sanitizer checks enabled.
# Use ASAN_OPTIMIZATION=-O1 explicitly on toolchains that support it.
ASAN_OPTIMIZATION ?= -O0

REQUIRED_CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread
INCLUDES := -Iinclude -Itests
S1_INCLUDES := -isystem third_party/valijson/include -isystem third_party/libyaml/include
S1_LIBS := -lsqlite3 -lcrypto -lcurl -l:libyaml-0.so.2

KERNEL_SOURCES := src/task_engine.cpp src/config.cpp src/scheduler.cpp src/journal_posix.cpp
KERNEL_OBJECTS := $(KERNEL_SOURCES:%.cpp=$(BUILD_DIR)/%.o)
TEST_NAMES := journal_tests config_tests scheduler_tests task_engine_tests
TEST_OBJECTS := $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .o,$(TEST_NAMES)))
TEST_BINARIES := $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))
S1_SOURCES := src/boost_json.cpp src/runtime_types.cpp src/runtime_resources.cpp src/migrations.cpp src/capacity.cpp src/store.cpp src/config_loader.cpp src/model.cpp src/provider_profile.cpp src/protocol_codec.cpp src/sse_replay.cpp src/https_transport.cpp src/live_model.cpp src/billing.cpp src/single_agent.cpp src/management_context.cpp src/owned_capacity.cpp src/context.cpp src/structured_context.cpp src/structured_history.cpp src/worker_history.cpp src/schema.cpp src/offline_runtime.cpp src/event_runtime.cpp src/workspace_posix.cpp src/sandbox_posix.cpp src/process_evidence.cpp src/worker_tools.cpp src/delivery.cpp src/cleanup_posix.cpp src/cleanup_runtime.cpp
S1_OBJECTS := $(S1_SOURCES:%.cpp=$(BUILD_DIR)/%.o)
S1_TEST_NAMES := runtime_types_tests runtime_resources_tests store_tests event_store_tests workflow_store_tests acceptance_store_tests config_loader_tests model_tests schema_tests offline_runtime_tests event_runtime_tests workflow_runtime_tests acceptance_runtime_tests workspace_tests sandbox_tests worker_tools_tests delivery_tests cleanup_tests cleanup_runtime_tests audit24_reopen_tests process_evidence_tests audit24_reconcile_tests audit24_capacity_tests audit24_capacity_repro_tests r1_history_tests r1_structured_tests r1_window_tests r2_rework_tests r2_recovery_tests r24_recovery_tests r24_cleanup_recovery_tests r2_amend_tests r2a_mkdir_tests allocation_recovery_tests doctor_diagnostics_tests protocol_codec_tests sse_replay_tests r1_management_tests owned_capacity_tests owned_capacity_runtime_tests live_model_store_tests billing_tests single_agent_tests
S1_TEST_OBJECTS := $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .o,$(S1_TEST_NAMES))) $(BUILD_DIR)/tests/https_transport_driver.o $(BUILD_DIR)/tests/live_model_store_driver.o $(BUILD_DIR)/tests/live_workflow_driver.o
S1_TEST_BINARIES := $(addprefix $(BUILD_DIR)/,$(S1_TEST_NAMES))
HTTPS_TEST_DRIVER := $(BUILD_DIR)/https_transport_driver
LIVE_TEST_DRIVER := $(BUILD_DIR)/live_model_store_driver
WORKFLOW_TEST_DRIVER := $(BUILD_DIR)/live_workflow_driver
S1_LIBRARY := $(BUILD_DIR)/libwls_s1.a
S1_APPLICATION := $(BUILD_DIR)/wls
S1_MAIN_OBJECT := $(BUILD_DIR)/src/cli.o
MAIN_OBJECT := $(BUILD_DIR)/src/main.o
LIBRARY := $(BUILD_DIR)/libwls861.a
APPLICATION := $(BUILD_DIR)/wls861
DEPENDENCIES := $(KERNEL_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(MAIN_OBJECT:.o=.d) $(S1_OBJECTS:.o=.d) $(S1_TEST_OBJECTS:.o=.d) $(S1_MAIN_OBJECT:.o=.d)

.PHONY: all test test-binaries verify-handoff verify-contracts sanitize ubsan asan

all: $(APPLICATION) $(S1_APPLICATION)

$(S1_OBJECTS) $(S1_TEST_OBJECTS) $(S1_MAIN_OBJECT): override CPPFLAGS += $(S1_INCLUDES)
$(S1_TEST_OBJECTS): override CPPFLAGS += -DWLS_SOURCE_DIR='"$(abspath .)"'

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(INCLUDES) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(LIBRARY): $(KERNEL_OBJECTS)
	$(AR) rcs "$@" $(KERNEL_OBJECTS)

$(S1_LIBRARY): $(S1_OBJECTS)
	$(AR) rcs "$@" $(S1_OBJECTS)

$(S1_APPLICATION): $(S1_MAIN_OBJECT) $(S1_LIBRARY) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(LDFLAGS) "$(S1_MAIN_OBJECT)" "$(S1_LIBRARY)" "$(LIBRARY)" $(S1_LIBS) $(LDLIBS) -o "$@"

$(S1_TEST_BINARIES) $(HTTPS_TEST_DRIVER) $(LIVE_TEST_DRIVER) $(WORKFLOW_TEST_DRIVER): $(BUILD_DIR)/%: $(BUILD_DIR)/tests/%.o $(S1_LIBRARY) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(LDFLAGS) "$<" "$(S1_LIBRARY)" "$(LIBRARY)" $(S1_LIBS) $(LDLIBS) -o "$@"

$(APPLICATION): $(MAIN_OBJECT) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(LDFLAGS) "$(MAIN_OBJECT)" "$(LIBRARY)" $(LDLIBS) -o "$@"

$(TEST_BINARIES): $(BUILD_DIR)/%: $(BUILD_DIR)/tests/%.o $(LIBRARY)
	$(CXX) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(LDFLAGS) "$<" "$(LIBRARY)" $(LDLIBS) -o "$@"

test-binaries: $(TEST_BINARIES) $(S1_TEST_BINARIES) $(HTTPS_TEST_DRIVER) $(LIVE_TEST_DRIVER) $(WORKFLOW_TEST_DRIVER)

verify-handoff:
	$(PYTHON) scripts/verify_handoff.py
	$(PYTHON) scripts/verify_review_source.py

verify-contracts:
	$(PYTHON) tests/contracts_tests.py

test: all $(TEST_BINARIES) $(S1_TEST_BINARIES) $(HTTPS_TEST_DRIVER) $(LIVE_TEST_DRIVER) $(WORKFLOW_TEST_DRIVER)
	$(PYTHON) scripts/verify_handoff.py
	$(PYTHON) scripts/verify_review_source.py
	$(PYTHON) tests/contracts_tests.py
	"$(BUILD_DIR)/journal_tests"
	"$(BUILD_DIR)/config_tests"
	"$(BUILD_DIR)/scheduler_tests"
	"$(BUILD_DIR)/task_engine_tests"
	$(PYTHON) tests/demo_smoke.py "$(abspath $(APPLICATION))"
	"$(BUILD_DIR)/runtime_types_tests"
	"$(BUILD_DIR)/runtime_resources_tests"
	"$(BUILD_DIR)/store_tests"
	"$(BUILD_DIR)/event_store_tests"
	"$(BUILD_DIR)/workflow_store_tests"
	"$(BUILD_DIR)/acceptance_store_tests"
	"$(BUILD_DIR)/config_loader_tests"
	"$(BUILD_DIR)/model_tests"
	"$(BUILD_DIR)/schema_tests"
	"$(BUILD_DIR)/offline_runtime_tests"
	"$(BUILD_DIR)/event_runtime_tests"
	"$(BUILD_DIR)/workflow_runtime_tests"
	"$(BUILD_DIR)/acceptance_runtime_tests"
	"$(BUILD_DIR)/workspace_tests"
	"$(BUILD_DIR)/sandbox_tests"
	"$(BUILD_DIR)/worker_tools_tests"
	"$(BUILD_DIR)/delivery_tests"
	"$(BUILD_DIR)/cleanup_tests"
	"$(BUILD_DIR)/cleanup_runtime_tests"
	"$(BUILD_DIR)/audit24_reopen_tests"
	"$(BUILD_DIR)/audit24_capacity_tests"
	"$(BUILD_DIR)/audit24_capacity_repro_tests"
	"$(BUILD_DIR)/r1_history_tests"
	"$(BUILD_DIR)/r1_structured_tests"
	"$(BUILD_DIR)/r1_window_tests"
	"$(BUILD_DIR)/r2_rework_tests"

	"$(BUILD_DIR)/r2_recovery_tests"
	"$(BUILD_DIR)/r24_recovery_tests"
	"$(BUILD_DIR)/r24_cleanup_recovery_tests"
	"$(BUILD_DIR)/r2_amend_tests"
	"$(BUILD_DIR)/r2a_mkdir_tests"
	"$(BUILD_DIR)/allocation_recovery_tests"
	"$(BUILD_DIR)/doctor_diagnostics_tests"
	"$(BUILD_DIR)/protocol_codec_tests"
	"$(BUILD_DIR)/sse_replay_tests"
	"$(BUILD_DIR)/r1_management_tests"
	"$(BUILD_DIR)/owned_capacity_tests"
	"$(BUILD_DIR)/owned_capacity_runtime_tests"
	"$(BUILD_DIR)/live_model_store_tests"
	"$(BUILD_DIR)/billing_tests"
	"$(BUILD_DIR)/single_agent_tests" --group history
	"$(BUILD_DIR)/single_agent_tests" --group runtime
	"$(BUILD_DIR)/process_evidence_tests"
	"$(BUILD_DIR)/audit24_reconcile_tests"
	$(PYTHON) tests/s1_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/s12_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/s13_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/s14_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/s15_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/s16_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/audit24_capacity_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/audit24_reconcile_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/audit24_reopen_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r1_history_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r1_structured_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r1_window_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r2_rework_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"

	$(PYTHON) tests/r24_recovery_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r2_recovery_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r2_integration_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r2_amend_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r2a_mkdir_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/test_s1_allocation_recovery_cli.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/test_doctor_diagnostics_cli.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/test_protocol_replay_cli.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/r3_evaluation_tests.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/test_https_transport.py "$(abspath $(HTTPS_TEST_DRIVER))" "$(abspath .)"
	$(PYTHON) tests/test_live_model_store.py "$(abspath $(LIVE_TEST_DRIVER))" "$(abspath .)"
	$(PYTHON) tests/test_live_workflow.py "$(abspath $(WORKFLOW_TEST_DRIVER))" "$(abspath .)"
	$(PYTHON) tests/test_live_workflow_cli.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/test_billing_baseline_e2e.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/runtime_resources_cli_tests.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"
	$(PYTHON) tests/review_cli_smoke.py "$(abspath $(S1_APPLICATION))" "$(abspath .)"

# The default sanitizer gate is UBSan; ASan is independently available so a
# runtime without working ASan support cannot be mistaken for a code failure.
sanitize: ubsan

ubsan:
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(MAKE) BUILD_DIR="$(BUILD_DIR)-ubsan" CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=undefined" test

asan:
	ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 $(MAKE) BUILD_DIR="$(BUILD_DIR)-asan" CXXFLAGS="$(CXXFLAGS) $(ASAN_OPTIMIZATION) -g -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address" test

-include $(DEPENDENCIES)
