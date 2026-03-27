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
    kMasterVol
  };

  switch (paramIdx)
  {
    case kDcoLfo: {
      mSliderDcoLfo = static_cast<float>(value);
      float depth = (mAdsrMode == 0)
        ? kr106::Voice<T>::dcoLfoDepth6(mSliderDcoLfo)
        : kr106::Voice<T>::dcoLfoDepth106(mSliderDcoLfo);
      ForEachVoice([depth](kr106::Voice<T>& v) { v.mDcoLfo = depth; });
      break;
    }
    case kDcoPwm:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mDcoPwm = static_cast<float>(value); });
      break;
    case kDcoSub:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mDcoSub = static_cast<float>(value); });
      break;
    case kDcoNoise:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mDcoNoise = static_cast<float>(value); });
      break;
    case kVcfFreq: {
      mSliderVcfFreq = static_cast<float>(value);
      float s = mSliderVcfFreq;
      float hzJ6 = kr106::j6_vcf_freq_from_slider(s);
      uint16_t cutoffInt = static_cast<uint16_t>(s * 0x3F80);
      float hzJ106 = kr106::dacToHz(cutoffInt);
      ForEachVoice([hzJ6, hzJ106, cutoffInt](kr106::Voice<T>& v) {
        v.mVcfFreq = hzJ6;
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
      float depth = (mAdsrMode == 0)
        ? kr106::Voice<T>::vcfLfoDepth6(mSliderVcfLfo)
        : kr106::Voice<T>::vcfLfoDepth106(mSliderVcfLfo);
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
      if (mAdsrMode == 0) {
        // Measured from 1982 Juno-6: 6 voices at slider positions 1–9,
        // averaged completion times (seconds):
        //   A=1: .003  A=2: .016  A=3: .053  A=4: .115  A=5: .217
        //   A=6: .427  A=7: .889  A=8: 1.089 A=9: 2.495
        // Endpoints from spec: A=0: 1ms, A=10: 3s.
        // Note: A/D/R pots are reverse-log (C-taper), so slider position maps
        // nonlinearly to resistance — the dead spot at A=7-8 is a pot characteristic.
        // Log-linear interpolation between tau values (tau = completion / ln(6),
        // since kAttackTarget=1.2 and the RC reaches 1.0 at ln(6) time constants).
        static constexpr float kAttackTau[11] = {
          0.000558f, 0.001674f, 0.008762f, 0.029468f, 0.064015f, 0.120998f,
          0.238481f, 0.495993f, 0.607950f, 1.392486f, 1.674332f
        };
        float s = mSliderA * 10.f;
        int idx = static_cast<int>(s);
        if (idx >= 10) idx = 9;
        float frac = s - idx;
        float tau = std::exp(std::log(kAttackTau[idx]) + frac * (std::log(kAttackTau[idx + 1]) - std::log(kAttackTau[idx])));
        ForEachVoice([tau](kr106::Voice<T>& v) { v.mADSR.SetAttackTau(tau); });
      } else {
        float s = mSliderA;
        ForEachVoice([s](kr106::Voice<T>& v) { v.mADSR.Set106Attack(s); });
      }
      break;
    }
    case kEnvD: {
      mSliderD = static_cast<float>(value);
      if (mAdsrMode == 0) {
        // Measured from 1982 Juno-6, VCF sweep durations (seconds):
        //   D=2: .086  D=3: .262  D=4: .821  D=5: 2.610  D=6: 2.325
        //   D=7: 5.609  D=8: 8.774  D=9: 20.051  D=10: 22.093
        // Re-measured D=4b: .876  D=5b: 1.587  D=6b: 2.564
        // Quadratic-exponential fit excluding dead spots at D=4, D=8.
        // FIXME(kr106): re-measure with pot voltage readings for better accuracy.
        float s = mSliderD;
        float tau = 0.003577f * std::exp(12.9460f * s + -5.0638f * s * s);
        ForEachVoice([tau](kr106::Voice<T>& v) { v.mADSR.SetDecayTau(tau); });
      } else {
        int idx = static_cast<int>(mSliderD * 127.f + 0.5f);
        ForEachVoice([idx](kr106::Voice<T>& v) { v.mADSR.Set106Decay(idx); });
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
      if (mAdsrMode == 0) {
        // Same circuit as decay (shared R/C network on IR3R01).
        // FIXME(kr106): re-measure with pot voltage readings for better accuracy.
        float s = mSliderR;
        float tau = 0.003577f * std::exp(12.9460f * s + -5.0638f * s * s);
        ForEachVoice([tau](kr106::Voice<T>& v) { v.mADSR.SetReleaseTau(tau); });
      } else {
        int idx = static_cast<int>(mSliderR * 127.f + 0.5f);
        ForEachVoice([idx](kr106::Voice<T>& v) { v.mADSR.Set106Release(idx); });
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
      mAdsrMode = static_cast<int>(value);
      bool j6 = (mAdsrMode == 0);
      ForEachVoice([j6](kr106::Voice<T>& v) {
        v.mADSR.mJ6Mode = j6;
        v.mVCF.mJ106Res = !j6;
        v.mOsc.mPulseInvert = !j6;
      });
      mLFO.mJ6Mode = j6;
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
      SetParam(kHpfFreq, mSliderHpf);
      break;
    }
    case kBender:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mRawBend = static_cast<float>(value); });
      break;

    case kLfoRate:
      mSliderLfoRate = static_cast<float>(value);
      mLFO.SetRate(mSliderLfoRate, mSampleRate);
      mLFO.mDivision = kr106::lfoDivisionFromSlider(mSliderLfoRate);
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
      if (mAdsrMode == 0)
      {
        // Map J106 preset values to J6 continuous positions
        // 0,1 (bass boost/flat) → 0; 2 (240Hz) → 0.685; 3 (720Hz) → 2.202
        static constexpr float kJ106toJ6[] = { 0.f, 0.f, 0.685f, 2.202f };
        float v = mSliderHpf;
        int iv = static_cast<int>(v + 0.5f);
        if (iv >= 0 && iv <= 3 && std::abs(v - iv) < 0.01f)
          v = kJ106toJ6[iv];
        mHPF.SetFreqHz(getJuno6HPFFreqPCHIP(v / 3.f));
      }
      else
        mHPF.SetMode(static_cast<int>(mSliderHpf + 0.5f));
      break;
    }

    case kVcaLevel: {
      // Patch Level VCA (PC1252H2 compander IC used as VCA)
      //
      // A user-adjustable level slider stored with each patch normalizes
      // signal amplitude for consistent volume when switching patches
      // during performance. All signal passes through this stage regardless
      // of whether the chorus is engaged.
      //
      // The control voltage ranges from +4V (max cut) to -6V (max boost)
      // with -1V as unity. A 15K pullup to +15V biases the asymmetric CV
      // range into a symmetric gain range. The PC1252H2 control constant
      // is -5.9 mV/dB, yielding approximately +/-10.6 dB of gain range.
      // dB-linear law: slider 0 = -10.6 dB, slider 0.5 = unity, slider 1 = +10.6 dB
      static constexpr float kMinDB = -10.6f;
      static constexpr float kMaxDB = +10.6f;
      float dB = kMinDB + (kMaxDB - kMinDB) * static_cast<float>(value);
      mVcaLevel = powf(10.f, dB / 20.f);
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
      mArp.mDivision = kr106::divisionFromSlider(mSliderArpRate);
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
