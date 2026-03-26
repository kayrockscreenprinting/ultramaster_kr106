CXX ?= clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
DSP = ../../Source/DSP

all: vcf_sweep vcf_noise vcf_quick vcf_res_sweep vcf_thd vcf_peaks vcf_cutoff

vcf_sweep: vcf_sweep.cpp $(DSP)/KR106VCF.h $(DSP)/KR106VcfFreqJ6.h $(DSP)/KR106VcfFreqJ106.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_sweep.cpp

vcf_noise: vcf_noise.cpp $(DSP)/KR106VCF.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_noise.cpp

vcf_quick: vcf_quick.cpp $(DSP)/KR106VCF.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_quick.cpp

vcf_res_sweep: vcf_res_sweep.cpp $(DSP)/KR106VCF.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_res_sweep.cpp

vcf_thd: vcf_thd.cpp $(DSP)/KR106VCF.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_thd.cpp

vcf_peaks: vcf_peaks.cpp $(DSP)/KR106VCF.h $(DSP)/KR106OscillatorsWT.h $(DSP)/KR106Oscillators.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_peaks.cpp

vcf_cutoff: vcf_cutoff.cpp $(DSP)/KR106VCF.h $(DSP)/KR106Noise.h
	$(CXX) $(CXXFLAGS) -o $@ vcf_cutoff.cpp

clean:
	rm -f vcf_sweep vcf_noise vcf_quick vcf_res_sweep vcf_thd vcf_peaks vcf_cutoff

.PHONY: all clean
