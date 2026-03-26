CXX ?= clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

all: gen_preset_midi

DSP = ../../Source/DSP

gen_preset_midi: gen_preset_midi.cpp ../../Source/KR106_Presets_JUCE.h $(DSP)/KR106ADSR.h
	$(CXX) $(CXXFLAGS) -o $@ gen_preset_midi.cpp

clean:
	rm -f gen_preset_midi

.PHONY: all clean
