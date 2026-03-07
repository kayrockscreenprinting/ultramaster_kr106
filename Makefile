BUILD_DIR = build-juce
CONFIG   ?= Debug

# ── JUCE / CMake targets ─────────────────────────────────────────────────────

.PHONY: build run reaper clean help

build:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)
	cmake --build $(BUILD_DIR) --config $(CONFIG)

run: build
	open $(BUILD_DIR)/KR106_artefacts/$(CONFIG)/Standalone/KR106.app

reaper: build
	@echo "Restarting Reaper with fresh VST cache..."
	-killall REAPER 2>/dev/null; sleep 1
	rm -f "$(HOME)/Library/Application Support/REAPER/reaper-vstplugins64.ini"
	open -a REAPER

clean:
	rm -rf $(BUILD_DIR)

# ── iPlug2 / Xcode targets (legacy) ──────────────────────────────────────────

IPLUG_PROJECT = projects/KR106-macOS.xcodeproj

.PHONY: iplug-app iplug-vst3 iplug-au iplug-all iplug-clean check-iplug2

iplug-app: check-iplug2
	xcodebuild -project "$(IPLUG_PROJECT)" -target APP -configuration $(CONFIG)

iplug-vst3: check-iplug2
	xcodebuild -project "$(IPLUG_PROJECT)" -target VST3 -configuration $(CONFIG)

iplug-au: check-iplug2
	xcodebuild -project "$(IPLUG_PROJECT)" -target AU -configuration $(CONFIG)

iplug-all: check-iplug2
	xcodebuild -project "$(IPLUG_PROJECT)" -target All -configuration $(CONFIG)

iplug-clean:
	rm -rf build-mac
	find "$(HOME)/Library/Developer/Xcode/DerivedData" -maxdepth 1 -name 'KR106-macOS-*' -exec rm -rf {} +

check-iplug2:
	@test -f ../iPlug2/common-mac.xcconfig || \
	  (echo "ERROR: iPlug2 not found at ../iPlug2"; exit 1)

# ── Help ──────────────────────────────────────────────────────────────────────

help:
	@echo ""
	@echo "Ultramaster KR-106"
	@echo ""
	@echo "  JUCE (CMake):"
	@echo "    make build        Build all formats (AU, VST3, LV2, Standalone)"
	@echo "    make run          Build and launch Standalone"
	@echo "    make reaper       Restart Reaper with fresh VST cache"
	@echo "    make clean        Remove build directory"
	@echo ""
	@echo "  iPlug2 (Xcode, legacy):"
	@echo "    make iplug-app    Standalone .app"
	@echo "    make iplug-vst3   VST3 plugin"
	@echo "    make iplug-au     Audio Unit"
	@echo "    make iplug-all    All formats"
	@echo "    make iplug-clean  Remove Xcode build artifacts"
	@echo ""
	@echo "  CONFIG=Release make build  — release build"
	@echo ""
