# CHOMPI -> Schwung (Ableton Move) module — build & offline render
#
#   make host        build the module .so for *this* machine (x86-64 Linux);
#                    used by the offline render harness and CI-ish checks
#   make render      render demo audio through the real plugin ABI
#   make aarch64     cross-build the device .so (needs an aarch64 toolchain)
#   make module      assemble a drop-in module/ directory for Schwung
#   make clean
#
# The device build needs aarch64-linux-gnu-g++ (or zig cc with
# -target aarch64-linux-gnu). If you have neither, build on the Move itself
# or let moveforge's mise tasks do it — see README.md.

MODULE_DIR   := module
DSP_DIR      := $(MODULE_DIR)/dsp
BUILD        := build
HOST_OUT     := $(BUILD)/chompi_move.so
AARCH64_OUT  := $(BUILD)/aarch64/chompi_move.so

# --- sources ---------------------------------------------------------------
# New code (the port) + CHOMPI's engine, verbatim, + the two files we need
# from DaisySP, + the POSIX platform layer.
PORT_SRC := \
	$(DSP_DIR)/chompi_move.cpp \
	$(DSP_DIR)/port/chompi_port.cpp \
	$(DSP_DIR)/compat/chompi_platform.cpp \
	$(DSP_DIR)/engine/FileStreamingManager.cpp \
	$(DSP_DIR)/third_party/daisysp/Control/adsr.cpp \
	$(DSP_DIR)/third_party/daisysp/Utility/dcblock.cpp \
	$(DSP_DIR)/third_party/daisysp/Filters/svf.cpp

INCLUDES := \
	-I $(DSP_DIR)/compat \
	-I $(DSP_DIR)/engine \
	-I $(DSP_DIR)/third_party \
	-I $(DSP_DIR)/third_party/daisysp \
	-I $(DSP_DIR)/third_party/schwung

# _Static_assert is C11; the Schwung ABI header is C that we compile as C++.
# CHOMPI_RAM_BUFFER_SAMPLES shrinks the two SDRAM arenas the firmware assumes
# (each ~31.7 MB by default). 4M samples per buffer = ~8 MB each, ~4 min total
# of stereo audio at 44.1 kHz; set to taste for the device.
CXXFLAGS := -std=c++17 -O2 -fPIC -fno-strict-aliasing -Wall -Wno-unused-variable \
	-D_Static_assert=static_assert \
	-DCHOMPI_RAM_BUFFER_SAMPLES=4194304 \
	$(INCLUDES)

LDFLAGS := -shared -lpthread

CXX      ?= g++
CROSS    ?= aarch64-linux-gnu-g++

.PHONY: all host render aarch64 module tools clean

all: host

host: $(HOST_OUT)

$(HOST_OUT): $(PORT_SRC) $(wildcard $(DSP_DIR)/engine/*.h) $(wildcard $(DSP_DIR)/compat/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(PORT_SRC) $(LDFLAGS)

render: $(HOST_OUT)
	@mkdir -p $(BUILD)/render
	./$(BUILD)/render_test $(HOST_OUT) $(BUILD)/render/chompi_ram.wav ram samples 6
	./$(BUILD)/render_test $(HOST_OUT) $(BUILD)/render/chompi_cubbi.wav cubbi samples 6
	./$(BUILD)/render_test $(HOST_OUT) $(BUILD)/render/chompi_record.wav record samples 6
	@ls -la $(BUILD)/render

render_test: $(BUILD)/render_test
$(BUILD)/render_test: host/render_test.cpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O2 -D_Static_assert=static_assert -I $(DSP_DIR)/third_party/schwung -o $@ $< -ldl

aarch64:
	@mkdir -p $(BUILD)/aarch64
	$(CROSS) $(CXXFLAGS) -o $(AARCH64_OUT) $(PORT_SRC) $(LDFLAGS)
	@echo "built $(AARCH64_OUT) — copy it to the module dir as dsp.so"

# Small dev probes that dlopen the built .so and exercise the plugin ABI
# without a Move: param serving, state round-trip, key handling. They run on
# the HOST .so, so build it first.
TOOLS := $(BUILD)/serve_params $(BUILD)/state_roundtrip $(BUILD)/key_probe
tools: $(HOST_OUT) $(TOOLS)

$(BUILD)/serve_params: tools/serve_params.cpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -D_Static_assert=static_assert -I $(DSP_DIR)/third_party/schwung -o $@ $< -ldl

$(BUILD)/state_roundtrip: tools/state_roundtrip.cpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -D_Static_assert=static_assert -I $(DSP_DIR)/third_party/schwung -o $@ $< -ldl

$(BUILD)/key_probe: tools/key_probe.cpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -D_Static_assert=static_assert -I $(DSP_DIR)/third_party/schwung -o $@ $< -ldl

# Staged drop-in directory: build/module/ — copy this to the device as
#   /data/UserData/schwung/modules/sound_generators/chompi/
# (device route: `make module CROSS=...`, see README.md)
MODULE_STAGE := $(BUILD)/module
module: $(AARCH64_OUT) module/module.json
	@rm -rf $(MODULE_STAGE)
	@mkdir -p $(MODULE_STAGE)/samples
	cp $(AARCH64_OUT) $(MODULE_STAGE)/dsp.so
	cp module/module.json $(MODULE_STAGE)/module.json
	cp samples/*.wav $(MODULE_STAGE)/samples/ 2>/dev/null || true
	@echo "staged $(MODULE_STAGE):"
	@ls -la $(MODULE_STAGE) $(MODULE_STAGE)/samples

clean:
	rm -rf $(BUILD)
