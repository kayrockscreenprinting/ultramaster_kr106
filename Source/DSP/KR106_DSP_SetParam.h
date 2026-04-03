// KR106DSP::SetParam — separated for readability.
// Included at the bottom of KR106_DSP.h (template, must be in header).

#pragma once

template <typename T>
void KR106DSP<T>::SetParam(int paramIdx, double value)
{
  enum {
    kBenderDco = 0, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
    kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
    kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
    kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
    kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw, kDcoSubSw,
    kChorusOff, kChorusI, kChorusII,
    kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
    kVcfEnvInv, kVcaMode,
    kBender, kTuning, kPower,
    kPortaMode, kPortaRate,
    kTransposeOffset, kBenderLfo,
    kAdsrMode,
    kMasterVol,
    kSettingVoices, kSettingOversample, kSettingIgnoreVel,
    kSettingArpLimitKbd, kSettingArpSync, kSettingLfoSync,
    kSettingMonoRetrig, kSettingMidiSysEx,
    kArpQuantize,
    kLfoQuantize
  };

  switch (paramIdx)
  {
    case kDcoLfo: {
      mSliderDcoLfo = static_cast<float>(value);
      float depth = (mSynthModel == kr106::kJ106) ? kr106::Voice<T>::dcoLfoDepth_j106(mSliderDcoLfo)
                  : (mSynthModel == kr106::kJ60)  ? kr106::Voice<T>::dcoLfoDepth_j60(mSliderDcoLfo)
                  : kr106::Voice<T>::dcoLfoDepth6(mSliderDcoLfo);
      ForEachVoice([depth](kr106::Voice<T>& v) { v.mDcoLfo = depth; });
      break;
    }
    case kDcoPwm:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mDcoPwm = static_cast<float>(value); });
      break;
    case kDcoSub: {
      mSliderDcoSub = static_cast<float>(value);
      float level = (mSynthModel == kr106::kJ106) ? kr106::Voice<T>::dcoSubLevel_j106(mSliderDcoSub)
                  : (mSynthModel == kr106::kJ60)  ? kr106::Voice<T>::dcoSubLevel_j60(mSliderDcoSub)
                  : kr106::Voice<T>::dcoSubLevel_j6(mSliderDcoSub);
      ForEachVoice([level](kr106::Voice<T>& v) { v.mDcoSub = level; });
      break;
    }
    case kDcoNoise: {
      mSliderDcoNoise = static_cast<float>(value);
      float level = (mSynthModel == kr106::kJ106) ? kr106::Voice<T>::dcoNoiseLevel_j106(mSliderDcoNoise)
                  : (mSynthModel == kr106::kJ60)  ? kr106::Voice<T>::dcoNoiseLevel_j60(mSliderDcoNoise)
                  : kr106::Voice<T>::dcoNoiseLevel_j6(mSliderDcoNoise);
      ForEachVoice([level](kr106::Voice<T>& v) { v.mDcoNoise = level; });
      break;
    }
    case kVcfFreq: {
      mSliderVcfFreq = static_cast<float>(value);
      float s = mSliderVcfFreq;
      float hzJ6 = kr106::j6_vcf_freq_from_slider(s);
      float hzJ60 = kr106::j60_vcf_freq_from_slider(s);
      uint16_t cutoffInt = static_cast<uint16_t>(s * 0x3F80);
      float hzJ106 = kr106::dacToHz(cutoffInt);
      ForEachVoice([hzJ6, hzJ60, hzJ106, cutoffInt](kr106::Voice<T>& v) {
        v.mVcfFreq = hzJ6;
        v.mVcfFreqJ60 = hzJ60;
        v.mVcfFreqJ106 = hzJ106;
        v.mVcfCutoffInt = cutoffInt;
      });
      break;
    }
    case kVcfRes:
      // J106: linear passthrough (no scaling in firmware — pot value used directly)
      ForEachVoice([value](kr106::Voice<T>& v) { v.mVcfRes = static_cast<float>(value); });
      break;
    case kVcfEnv: {
      mSliderVcfEnv = static_cast<float>(value);
      uint8_t envModInt = static_cast<uint8_t>(mSliderVcfEnv * 254.f);
      ForEachVoice([value, envModInt](kr106::Voice<T>& v) {
        v.mVcfEnv = static_cast<float>(value);
        v.mVcfEnvModInt = envModInt;
      });
      break;
    }
    case kVcfLfo: {
      mSliderVcfLfo = static_cast<float>(value);
      float depth = (mSynthModel == kr106::kJ106) ? kr106::Voice<T>::vcfLfoDepth_j106(mSliderVcfLfo)
                  : (mSynthModel == kr106::kJ60)  ? kr106::Voice<T>::vcfLfoDepth_j60(mSliderVcfLfo)
                  : kr106::Voice<T>::vcfLfoDepth6(mSliderVcfLfo);
      uint8_t lfoDepthInt = static_cast<uint8_t>(mSliderVcfLfo * 254.f);
      ForEachVoice([depth, lfoDepthInt](kr106::Voice<T>& v) {
        v.mVcfLfo = depth;
        v.mVcfLfoDepthInt = lfoDepthInt;
      });
      break;
    }
    case kVcfKbd: {
      mSliderVcfKbd = static_cast<float>(value);
      uint8_t keyTrackInt = static_cast<uint8_t>(mSliderVcfKbd * 254.f);
      ForEachVoice([value, keyTrackInt](kr106::Voice<T>& v) {
        v.mVcfKbd = static_cast<float>(value);
        v.mVcfKeyTrackInt = keyTrackInt;
      });
      break;
    }
    case kBenderDco:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mBendDco = static_cast<float>(value); });
      break;
    case kBenderVcf: {
      mSliderBenderVcf = static_cast<float>(value);
      uint8_t bendSensInt = static_cast<uint8_t>(mSliderBenderVcf * 255.f);
      ForEachVoice([value, bendSensInt](kr106::Voice<T>& v) {
        v.mBendVcf = static_cast<float>(value);
        v.mVcfBendSensInt = bendSensInt;
      });
      break;
    }
    case kBenderLfo:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mBendLfo = static_cast<float>(value); });
      break;

    case kEnvA: {
      mSliderA = static_cast<float>(value);
      if (mSynthModel == kr106::kJ106) {
        float s = mSliderA;
        ForEachVoice([s](kr106::Voice<T>& v) { v.mADSR.Set106Attack(s); });
      } else {
        // J6/J60: analog RC circuit on IR3R01
        float tau = (mSynthModel == kr106::kJ60)
          ? kr106::ADSR::AttackTauJ60(mSliderA)
          : kr106::ADSR::AttackTauJ6(mSliderA);
        ForEachVoice([tau](kr106::Voice<T>& v) { v.mADSR.SetAttackTau(tau); });
      }
      break;
    }
    case kEnvD: {
      mSliderD = static_cast<float>(value);
      if (mSynthModel == kr106::kJ106) {
        int idx = static_cast<int>(mSliderD * 127.f + 0.5f);
        ForEachVoice([idx](kr106::Voice<T>& v) { v.mADSR.Set106Decay(idx); });
      } else {
        // J6/J60: analog RC circuit on IR3R01
        float tau = (mSynthModel == kr106::kJ60)
          ? kr106::ADSR::DecRelTauJ60(mSliderD)
          : kr106::ADSR::DecRelTauJ6(mSliderD);
        ForEachVoice([tau](kr106::Voice<T>& v) { v.mADSR.SetDecayTau(tau); });
      }
      break;
    }
    case kEnvS: {
      float s = std::max(static_cast<float>(value), 0.001f);
      ForEachVoice([s](kr106::Voice<T>& v) { v.mADSR.SetSustain(s); });
      break;
    }
    case kEnvR: {
      mSliderR = static_cast<float>(value);
      if (mSynthModel == kr106::kJ106) {
        int idx = static_cast<int>(mSliderR * 127.f + 0.5f);
        ForEachVoice([idx](kr106::Voice<T>& v) { v.mADSR.Set106Release(idx); });
      } else {
        // J6/J60: same circuit as decay (shared R/C network on IR3R01)
        float tau = (mSynthModel == kr106::kJ60)
          ? kr106::ADSR::DecRelTauJ60(mSliderR)
          : kr106::ADSR::DecRelTauJ6(mSliderR);
        ForEachVoice([tau](kr106::Voice<T>& v) { v.mADSR.SetReleaseTau(tau); });
      }
      break;
    }

    case kDcoPulse:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mPulseOn = value > 0.5; });
      break;
    case kDcoSaw:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mSawOn = value > 0.5; });
      break;
    case kDcoSubSw:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mSubOn = value > 0.5; });
      break;
    case kPwmMode:
      ForEachVoice([value](kr106::Voice<T>& v) {
        v.mPwmMode = static_cast<int>(value) - 1;
      });
      break;
    case kVcfEnvInv:
      ForEachVoice([value](kr106::Voice<T>& v) {
        v.mVcfEnvInvert = (static_cast<int>(value) != 0) ? -1 : 1;
      });
      break;
    case kVcaMode:
      ForEachVoice([value](kr106::Voice<T>& v) {
        int newMode = static_cast<int>(value);
        if (newMode != v.mVcaMode && v.GetBusy())
        {
          // Snap envelope values to prevent click when switching mid-note.
          // Gate→Env: copy gateEnv level to ADSR env so it doesn't drop to zero.
          // Env→Gate: copy ADSR env to gateEnv so it doesn't jump to full.
          if (newMode == 0) // switching to env mode
            v.mADSR.mEnv = v.mADSR.mGateEnv;
          else // switching to gate mode
            v.mADSR.mGateEnv = v.mADSR.mEnv;
        }
        v.mVcaMode = newMode;
      });
      break;
    case kAdsrMode: {
      // UI: 0 = J60 (60 mode), 1 = J106 (106 mode)
      mSynthModel = (static_cast<int>(value) == 0) ? kr106::kJ60 : kr106::kJ106;
      ForEachVoice([this](kr106::Voice<T>& v) {
        v.mModel = mSynthModel;
        v.mADSR.mModel = mSynthModel;
        v.mVCF.mJ106Res = (mSynthModel == kr106::kJ106);
        v.mOsc.mPulseInvert = (mSynthModel == kr106::kJ106);
      });
      mLFO.mModel = mSynthModel;
      mHPF.mModel = mSynthModel;
      SetParam(kEnvA, mSliderA);
      SetParam(kEnvD, mSliderD);
      SetParam(kEnvR, mSliderR);
      SetParam(kLfoRate, mSliderLfoRate);
      SetParam(kLfoDelay, mSliderLfoDelay);
      SetParam(kDcoLfo, mSliderDcoLfo);
      SetParam(kVcfLfo, mSliderVcfLfo);
      SetParam(kVcfFreq, mSliderVcfFreq);
      SetParam(kVcfEnv, mSliderVcfEnv);
      SetParam(kVcfKbd, mSliderVcfKbd);
      SetParam(kBenderVcf, mSliderBenderVcf);
      SetParam(kDcoSub, mSliderDcoSub);
      SetParam(kDcoNoise, mSliderDcoNoise);
      SetParam(kVcaLevel, mSliderVcaLevel);
      SetParam(kHpfFreq, mSliderHpf);
      break;
    }
    case kBender:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mRawBend = static_cast<float>(value); });
      break;

    case kLfoRate:
      mSliderLfoRate = static_cast<float>(value);
      mLFO.SetRate(mSliderLfoRate, mSampleRate);
      break;
    case kLfoQuantize:
      mLFO.mDivision = std::max(0, std::min(static_cast<int>(value), static_cast<int>(kr106::kNumLfoDivisions) - 1));
      break;
    case kLfoDelay:
      mSliderLfoDelay = static_cast<float>(value);
      mLFO.SetDelay(mSliderLfoDelay);
      break;
    case kLfoMode:
      mLFO.SetMode(static_cast<int>(value));
      break;

    case kHpfFreq: {
      mSliderHpf = static_cast<float>(value);
      mHPF.SetMode(static_cast<int>(mSliderHpf + 0.5f));
      break;
    }

    case kVcaLevel: {
      mSliderVcaLevel = static_cast<float>(value);
      // J6: no patch-level VCA -- signal goes HPF -> chorus at unity.
      // J60/J106: PC1252H2 compander IC used as VCA.
      //
      // A user-adjustable level slider stored with each patch normalizes
      // signal amplitude for consistent volume when switching patches
      // during performance. All signal passes through this stage regardless
      // of whether the chorus is engaged.
      //
      // The control voltage ranges from +4V (max cut) to -6V (max boost)
      // with -1V as unity. A 15K pullup to +15V biases the asymmetric CV
      // range into a symmetric gain range. The PC1252H2 control constant
      // is -5.9 mV/dB, yielding approximately +/-10 dB of gain range.
      // Confirmed by hardware recording (Lewis Francis): -9.8 dB / +9.9 dB.
      // dB-linear law: slider 0 = -10 dB, slider 0.5 = unity, slider 1 = +10 dB
      if (mSynthModel == kr106::kJ6) {
        mVcaLevel = 1.f; // J6: no patch-level VCA
      } else {
        static constexpr float kMinDB = -10.f;
        static constexpr float kMaxDB = +10.f;
        float dB = kMinDB + (kMaxDB - kMinDB) * static_cast<float>(value);
        mVcaLevel = powf(10.f, dB / 20.f);
      }
      break;
    }

    case kChorusOff: break;
    case kChorusI:
      mChorusI = value > 0.5;
      UpdateChorusMode();
      break;
    case kChorusII:
      mChorusII = value > 0.5;
      UpdateChorusMode();
      break;

    case kOctTranspose: {
      mOctaveTranspose = static_cast<int>(value) - 1;
      float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
      ForEachVoice([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
      break;
    }
    case kTuning: {
      mTuning = value;
      float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
      ForEachVoice([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
      break;
    }

    case kPortaMode: {
      int prevMode = mPortaMode;
      mPortaMode = static_cast<int>(value);
      if (mSuppressHoldRelease)
      {
        bool portaOn = (mPortaMode <= 1);
        ForEachVoice([portaOn](kr106::Voice<T>& v) { v.mPortaEnabled = portaOn; });
        break;
      }
      bool prevPoly = (prevMode >= 1);
      bool newPoly  = (mPortaMode >= 1);
      if (!(prevPoly && newPoly))
      {
        std::bitset<128> activeNotes = mKeysDown | mHeldNotes;
        if (prevMode == 0) { ReleaseUnisonVoices(); mUnisonNote = -1; mUnisonStack.clear(); }
        else
        {
          int nv2 = static_cast<int>(NVoices());
          for (int i = 0; i < nv2; i++)
            if (mVoiceNote[i] >= 0) { mVoices[i]->Release(); mVoiceNote[i] = -1; }
        }
        mHeldNotes.reset();
        for (int i = 0; i < 128; i++)
        {
          if (activeNotes.test(i))
          {
            SendToSynth(i, true, 127);
            if (!mKeysDown.test(i)) mHeldNotes.set(i);
          }
        }
      }
      bool portaOn = (mPortaMode <= 1);
      ForEachVoice([portaOn](kr106::Voice<T>& v) { v.mPortaEnabled = portaOn; });
      break;
    }
    case kPortaRate: {
      float rate = static_cast<float>(value);
      ForEachVoice([rate](kr106::Voice<T>& v) { v.mPortaRateParam = rate; v.UpdatePortaCoeff(); });
      break;
    }

    case kArpRate:
      mSliderArpRate = static_cast<float>(value);
      mArp.mRate = kr106::Arpeggiator::arpRate(mSliderArpRate);
      break;
    case kArpQuantize:
      mArp.mDivision = std::max(0, std::min(static_cast<int>(value), static_cast<int>(kr106::kNumArpDivisions) - 1));
      break;
    case kArpMode: mArp.mMode = static_cast<int>(value); mArp.mStepIndex = 0; mArp.mDirection = 1; break;
    case kArpRange: mArp.mRange = static_cast<int>(value); break;
    case kArpeggio: {
      bool wasEnabled = mArp.mEnabled;
      mArp.mEnabled = value > 0.5;
      if (mSuppressHoldRelease) break;
      if (mArp.mEnabled && !wasEnabled)
      {
        std::bitset<128> toSeed = mKeysDown | mHeldNotes;
        for (int i = 0; i < 128; i++)
          if (toSeed.test(i)) { mArp.NoteOn(i); SendToSynth(i, false, 0); }
        mHeldNotes.reset();
      }
      else if (!mArp.mEnabled && wasEnabled)
      {
        if (mArp.mLastNote >= 0) { SendToSynth(mArp.mLastNote, false, 0); mArp.mLastNote = -1; }
        if (mHold)
        {
          mHeldNotes.reset();
          for (int n : mArp.mHeldNotes) { SendToSynth(n, true, 127); mHeldNotes.set(n); }
        }
        mArp.Reset();
      }
      break;
    }

    case kHold:
      mHold = value > 0.5;
      if (!mHold && !mSuppressHoldRelease) ReleaseHeldNotes();
      break;

    case kTranspose:
      mTranspose = value > 0.5;
      break;

    default: break;
  }
}
