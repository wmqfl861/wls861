.DEFAULT_GOAL := all

CXX ?= g++
AR ?= ar
PYTHON ?= python3
BUILD_DIR ?= build
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=

REQUIRED_CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -pthread
INCLUDES := -Iinclude -Itests

KERNEL_SOURCES := src/task_engine.cpp src/config.cpp src/scheduler.cpp src/journal_posix.cpp
KERNEL_OBJECTS := $(KERNEL_SOURCES:%.cpp=$(BUILD_DIR)/%.o)
TEST_NAMES := journal_tests config_tests scheduler_tests task_engine_tests
TEST_OBJECTS := $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .o,$(TEST_NAMES)))
TEST_BINARIES := $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))
MAIN_OBJECT := $(BUILD_DIR)/src/main.o
LIBRARY := $(BUILD_DIR)/libwls861.a
APPLICATION := $(BUILD_DIR)/wls861
DEPENDENCIES := $(KERNEL_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(MAIN_OBJECT:.o=.d)

.PHONY: all test test-binaries verify-handoff sanitize ubsan asan

all: $(APPLICATION)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p "$(dir $@)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(INCLUDES) -MMD -MP -MF "$(@:.o=.d)" -c "$<" -o "$@"

$(LIBRARY): $(KERNEL_OBJECTS)
	$(AR) rcs "$@" $(KERNEL_OBJECTS)

$(APPLICATION): $(MAIN_OBJECT) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(LDFLAGS) "$(MAIN_OBJECT)" "$(LIBRARY)" $(LDLIBS) -o "$@"

$(TEST_BINARIES): $(BUILD_DIR)/%: $(BUILD_DIR)/tests/%.o $(LIBRARY)
	$(CXX) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) $(LDFLAGS) "$<" "$(LIBRARY)" $(LDLIBS) -o "$@"

test-binaries: $(TEST_BINARIES)

verify-handoff:
	$(PYTHON) scripts/verify_handoff.py

test: all $(TEST_BINARIES)
	$(PYTHON) scripts/verify_handoff.py
	"$(BUILD_DIR)/journal_tests"
	"$(BUILD_DIR)/config_tests"
	"$(BUILD_DIR)/scheduler_tests"
	"$(BUILD_DIR)/task_engine_tests"
	$(PYTHON) tests/demo_smoke.py "$(abspath $(APPLICATION))"

# The default sanitizer gate is UBSan; ASan is independently available so a
# runtime without working ASan support cannot be mistaken for a code failure.
sanitize: ubsan

ubsan:
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(MAKE) BUILD_DIR="$(BUILD_DIR)-ubsan" CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=undefined" test

asan:
	ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 $(MAKE) BUILD_DIR="$(BUILD_DIR)-asan" CXXFLAGS="$(CXXFLAGS) -O1 -g -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address" test

-include $(DEPENDENCIES)
