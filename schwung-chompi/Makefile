# CHOMPI -> Schwung (Ableton Move) module — build & offline render
#
#   make host        build the module .so for *this* machine (x86-64 Linux);
#                    used by the offline render harness and CI-ish checks
#   make render      render demo audio through the real plugin ABI
#   make aarch64     cross-build the device .so (needs an aarch64 toolchain)
#   make module      assemble a drop-in module/ directory for Schwung
#   make clean
#
# The device build needs aarch64-linux-gnu-g++ (or zig as above). If you have
# neither, build on the Move itself or let moveforge's mise tasks do it — see
# README.md.

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

# zig one-liner for the device build, without a full cross toolchain:
#   make module ZIG=1 ZIG_BIN=/path/to/zig
# Targets glibc $(GLIBC) so the .so loads on the widest range of Move firmware.
# (zig's default target is 2.27, which needs expf/exp2f/logf/powf@2.27; the
# 2.17 target binds the older symbol versions, which every newer glibc keeps.)
ZIG      ?=
ZIG_BIN  ?= zig
GLIBC    ?= 2.17
ifeq ($(ZIG),1)
CROSS    := $(ZIG_BIN) c++ -target aarch64-linux-gnu.$(GLIBC)
endif

.PHONY: all host render aarch64 module tools tools-aarch64 check deploy probe logs logs-off samples-full clean

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

aarch64: $(AARCH64_OUT)

# Rebuilt when any source changes. Switching CROSS does not retrigger it —
# `make clean` (or touch a source) when changing toolchains.
$(AARCH64_OUT): $(PORT_SRC) $(wildcard $(DSP_DIR)/engine/*.h) $(wildcard $(DSP_DIR)/compat/*.h)
	@mkdir -p $(BUILD)/aarch64
	$(CROSS) $(CXXFLAGS) -o $@ $(PORT_SRC) $(LDFLAGS)
	@echo "built $@ — copy it to the module dir as dsp.so"

# Small dev probes that dlopen the built .so and exercise the plugin ABI
# without a Move: param serving, state round-trip, key handling. `tools`
# builds them for HOST, `tools-aarch64` for the device (then scp them over).
TOOL_SRC  := tools/serve_params.cpp tools/state_roundtrip.cpp tools/key_probe.cpp
TOOLS     := $(patsubst tools/%.cpp,$(BUILD)/%,$(TOOL_SRC))
TOOLS_A64 := $(patsubst tools/%.cpp,$(BUILD)/aarch64/%,$(TOOL_SRC))

tools: $(HOST_OUT) $(TOOLS)

tools-aarch64: $(TOOLS_A64)

$(TOOLS): $(BUILD)/%: tools/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -D_Static_assert=static_assert -I $(DSP_DIR)/third_party/schwung -o $@ $< -ldl

$(TOOLS_A64): $(BUILD)/aarch64/%: tools/%.cpp
	@mkdir -p $(BUILD)/aarch64
	$(CROSS) $(CXXFLAGS) -o $@ $< -ldl
	@echo "built $@"

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

# The full CHOMPI sample set (MIT, in this repo) — the two files in samples/
# are enough to test, this is what you actually want to play with. Copies the
# tape-2.0 card's CUBBI + JAMMI wavs (~150 MB) into samples/.
SAMPLES_SRC ?= ../firmware/card-profiles/tape-2.0
samples-full:
	@test -d $(SAMPLES_SRC) || { echo "not found: $(SAMPLES_SRC) — run from the repo checkout, or set SAMPLES_SRC="; exit 1; }
	cp $(SAMPLES_SRC)/cubbi_*.wav $(SAMPLES_SRC)/jammi_*.wav samples/
	@ls samples/ | wc -l

# --- deploy to a Move on the LAN -------------------------------------------
# MOVE=move.local MOVE_USER=ableton by default (Schwung's SSH setup puts the
# key in /data/authorized_keys). Override for another host/port:
#   make deploy MOVE=192.168.1.42
MOVE      ?= move.local
MOVE_USER ?= ableton
REMOTE_DIR = /data/UserData/schwung/modules/sound_generators/chompi
SSH       := ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new $(MOVE_USER)@$(MOVE)

deploy: module
	$(SSH) "mkdir -p $(REMOTE_DIR)/samples"
	scp build/module/dsp.so build/module/module.json $(MOVE_USER)@$(MOVE):$(REMOTE_DIR)/
	scp build/module/samples/*.wav $(MOVE_USER)@$(MOVE):$(REMOTE_DIR)/samples/
	@echo "deployed to $(MOVE_USER)@$(MOVE):$(REMOTE_DIR)"
	@echo "reload the chain (or reboot) to pick it up; watch: make logs"

# Cross-compiled ABI probes, dropped in /tmp on the Move so they can be run
# against the installed module without any UI involvement:
#   make probe MOVE=move.local
#   ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./key_probe ./dsp.so <module_dir> 2'
probe: tools-aarch64 module
	$(SSH) "mkdir -p /data/UserData/schwung/chompi-probe/samples"
	scp $(TOOLS_A64) $(MOVE_USER)@$(MOVE):/data/UserData/schwung/chompi-probe/
	scp build/module/dsp.so build/module/module.json $(MOVE_USER)@$(MOVE):/data/UserData/schwung/chompi-probe/
	scp build/module/samples/*.wav $(MOVE_USER)@$(MOVE):/data/UserData/schwung/chompi-probe/samples/
	@echo "probes staged in /data/UserData/schwung/chompi-probe on $(MOVE); run e.g."
	@echo "  ssh $(MOVE_USER)@$(MOVE) 'cd /data/UserData/schwung/chompi-probe && ./serve_params ./dsp.so chain_params ui_hierarchy && ./key_probe ./dsp.so . 2'"

# Schwung's log: opt-in, then tail it.
logs:
	$(SSH) "touch /data/UserData/schwung/debug_log_on && tail -n 80 /data/UserData/schwung/debug.log"

logs-off:
	$(SSH) "rm -f /data/UserData/schwung/debug_log_on"

# Verify the device artifacts without readelf (works on macOS too).
check: module tools-aarch64
	python3 tools/check_abi.py build/module/dsp.so --module
	@for p in $(TOOLS_A64); do python3 tools/check_abi.py $$p; done

clean:
	rm -rf $(BUILD)
