CXX ?= clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
DSP = ../../Source/DSP

DEPS = $(DSP)/KR106_DSP.h $(DSP)/KR106_DSP_SetParam.h $(DSP)/KR106Voice.h $(DSP)/KR106VCA.h $(DSP)/KR106VCF.h $(DSP)/KR106ADSR.h $(DSP)/KR106Noise.h $(DSP)/KR106Chorus.h $(DSP)/KR106LFO.h $(DSP)/KR106Arpeggiator.h $(DSP)/KR106_HPF.h $(DSP)/KR106OscillatorsWT.h $(DSP)/KR106Oscillators.h $(DSP)/KR106VcfFreqJ6.h $(DSP)/KR106VcfFreqJ106.h

all: render_midi peak_presets

render_midi: render_midi.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ render_midi.cpp

peak_presets: peak_presets.cpp $(DEPS) ../../Source/KR106_Presets_JUCE.h
	$(CXX) $(CXXFLAGS) -o $@ peak_presets.cpp

clean:
	rm -f render_midi peak_presets

.PHONY: all clean
