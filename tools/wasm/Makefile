EMCC ?= emcc
DSP = ../../Source/DSP
PRESETS = ../../Source/KR106_Presets_JUCE.h
VERSION := $(shell grep 'project(KR106 VERSION' ../../CMakeLists.txt | sed 's/.*VERSION //' | sed 's/)//')

CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

# Emscripten flags:
#   Output kr106_dsp.js + kr106_dsp.wasm
#   MODULARIZE — exports a factory function we can call in the worklet
#   EXPORT_NAME — the factory function name
#   ENVIRONMENT=web — no Node.js support needed
EMFLAGS = \
	-s EXPORTED_FUNCTIONS='[ \
		"_kr106_create", "_kr106_destroy", "_kr106_process", \
		"_kr106_note_on", "_kr106_note_off", "_kr106_force_release", "_kr106_set_param", \
		"_kr106_load_preset", "_kr106_get_num_presets", "_kr106_get_preset_name", "_kr106_get_preset_value", \
		"_kr106_get_scope_ring", "_kr106_get_scope_ring_r", "_kr106_get_scope_sync_ring", "_kr106_get_scope_write_pos", "_kr106_get_scope_ring_size", \
		"_malloc", "_free" ]' \
	-s EXPORTED_RUNTIME_METHODS='["HEAPF32","UTF8ToString"]' \
	-s MODULARIZE=1 \
	-s EXPORT_NAME='createKR106' \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=16777216 \
	-s ENVIRONMENT=web \
	--no-entry

SCOPE_JS = kr106-scope.js
INDEX_HTML = ../../docs/website/index.html

all: kr106_dsp.js version

kr106_dsp.js: kr106_wasm.cpp $(wildcard $(DSP)/*.h) $(PRESETS)
	$(EMCC) $(CXXFLAGS) $(EMFLAGS) -o kr106_dsp.js kr106_wasm.cpp

BUILD_DATE := $(shell date +%m-%d\ %H:%M)
BUILD_HASH := $(shell date +%s)

version:
	@sed -i '' "s/const lines=\['ULTRAMASTER','KR-106','[^']*','BUILD [^']*'\]/const lines=['ULTRAMASTER','KR-106','$(VERSION)','BUILD $(BUILD_DATE)']/" $(SCOPE_JS)
	@sed -i '' "s/const KR106_CACHE = '[^']*'/const KR106_CACHE = '$(BUILD_HASH)'/" kr106-audio.js
	@sed -i '' 's/\(dist\/kr106[^"]*\.js\)?v=[^"]*/\1?v=$(BUILD_HASH)/g' $(INDEX_HTML)
	@echo "Updated scope version to $(VERSION) build $(BUILD_DATE)"

# Dev server for testing (requires python3)
serve:
	python3 -m http.server 8080

clean:
	rm -f kr106_dsp.js kr106_dsp.wasm

.PHONY: all clean serve
