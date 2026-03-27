// VCA gain law test: render one patch at every VCA level, measure peak.
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"

enum EParams {
  kBenderDco=0, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
  kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
  kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
  kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
  kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw, kDcoSubSw,
  kChorusOff, kChorusI, kChorusII,
  kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
  kVcfEnvInv, kVcaMode, kBender, kTuning, kPower,
  kPortaMode, kPortaRate, kTransposeOffset, kBenderLfo, kAdsrMode,
  kMasterVol, kNumParams
};

static void setP(KR106DSP<float>& d, int p, float v) { d.SetParam(p, (double)v); }

int main() {
  float sr = 44100.f;
  int blockSize = 512;
  int noteSamples = (int)(2.f * sr);  // 2s note
  int tailSamples = (int)(1.f * sr);  // 1s tail
  int total = noteSamples + tailSamples;

  printf("vca_raw,vca_norm,peak,peak_dBFS,expected_dB,error_dB\n");

  for (int vcaRaw = 0; vcaRaw <= 127; vcaRaw += 1) {
    KR106DSP<float> dsp(6);
    dsp.Reset(sr, blockSize);

    setP(dsp, kPower, 1.f);
    setP(dsp, kAdsrMode, 1.f);
    dsp.mMasterVol = 1.f;  // unity master vol to isolate VCA
    setP(dsp, kPortaMode, 2.f);

    // Simple saw patch: open filter, no resonance, ADSR env
    setP(dsp, kDcoSaw, 1.f);
    setP(dsp, kDcoPulse, 0.f);
    setP(dsp, kDcoSubSw, 0.f);
    setP(dsp, kVcfFreq, 1.f);    // fully open
    setP(dsp, kVcfRes, 0.f);
    setP(dsp, kVcfEnv, 0.f);
    setP(dsp, kVcfKbd, 0.f);
    setP(dsp, kEnvA, 0.f);
    setP(dsp, kEnvD, 0.f);
    setP(dsp, kEnvS, 1.f);       // full sustain
    setP(dsp, kEnvR, 0.2f);
    setP(dsp, kVcaMode, 0.f);    // ADSR mode
    setP(dsp, kChorusOff, 1.f);  // no chorus
    setP(dsp, kVcaLevel, vcaRaw / 127.f);

    std::vector<float> outL(blockSize, 0.f), outR(blockSize, 0.f);
    float peak = 0.f;
    int rendered = 0;
    bool noteOff = false;

    while (rendered < total) {
      int bs = std::min(blockSize, total - rendered);
      if (rendered == 0) dsp.NoteOn(60, 127);
      if (!noteOff && rendered >= noteSamples) { dsp.NoteOff(60); noteOff = true; }

      std::fill(outL.begin(), outL.begin()+bs, 0.f);
      std::fill(outR.begin(), outR.begin()+bs, 0.f);
      float* outs[2] = {outL.data(), outR.data()};
      dsp.ProcessBlock(nullptr, outs, 2, bs);

      for (int i = 0; i < bs; i++) {
        peak = std::max(peak, fabsf(outL[i]));
        peak = std::max(peak, fabsf(outR[i]));
      }
      rendered += bs;
    }

    float peakDB = (peak > 1e-10f) ? 20.f*log10f(peak) : -200.f;
    float vnorm = vcaRaw / 127.f;
    float expectedDB = -10.6f + 21.2f * vnorm;
    float error = peakDB - expectedDB;

    printf("%d,%.4f,%.6f,%.2f,%.2f,%.2f\n", vcaRaw, vnorm, peak, peakDB, expectedDB, error);
  }
  return 0;
}
