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
    kAdsrMode
  };

  switch (paramIdx)
  {
    case kDcoLfo:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mDcoLfo = static_cast<float>(value); });
      break;
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
      float hz = 20.f * std::pow(900.f, static_cast<float>(value));
      ForEachVoice([hz](kr106::Voice<T>& v) { v.mVcfFreq = hz; });
      break;
    }
    case kVcfRes:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mVcfRes = static_cast<float>(value); });
      break;
    case kVcfEnv:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mVcfEnv = static_cast<float>(value); });
      break;
    case kVcfLfo:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mVcfLfo = static_cast<float>(value); });
      break;
    case kVcfKbd:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mVcfKbd = static_cast<float>(value); });
      break;
    case kBenderDco:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mBendDco = static_cast<float>(value); });
      break;
    case kBenderVcf:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mBendVcf = static_cast<float>(value); });
      break;
    case kBenderLfo:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mBendLfo = static_cast<float>(value); });
      break;

    case kEnvA: {
      mSliderA = static_cast<float>(value);
      bool j6 = (mAdsrMode == 0);
      float ms = LookupLUT(j6 ? kAttackLUT_J6 : kAttackLUT, mSliderA);
      if (j6)
        ForEachVoice([ms](kr106::Voice<T>& v) { v.mADSR.SetAttackExp(ms); });
      else
        ForEachVoice([ms](kr106::Voice<T>& v) { v.mADSR.SetAttack(ms); });
      break;
    }
    case kEnvD: {
      mSliderD = static_cast<float>(value);
      float ms = LookupLUT(mAdsrMode == 0 ? kDecayLUT_J6 : kDecayLUT, mSliderD);
      ForEachVoice([ms](kr106::Voice<T>& v) { v.mADSR.SetDecay(ms); });
      break;
    }
    case kEnvS: {
      float s = std::max(static_cast<float>(value), 0.001f);
      ForEachVoice([s](kr106::Voice<T>& v) { v.mADSR.SetSustain(s); });
      break;
    }
    case kEnvR: {
      mSliderR = static_cast<float>(value);
      float ms = LookupLUT(mAdsrMode == 0 ? kReleaseLUT_J6 : kReleaseLUT, mSliderR);
      ForEachVoice([ms](kr106::Voice<T>& v) { v.mADSR.SetRelease(ms); });
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
      ForEachVoice([value](kr106::Voice<T>& v) { v.mVcaMode = static_cast<int>(value); });
      break;
    case kAdsrMode: {
      mAdsrMode = static_cast<int>(value);
      bool expAtk = (mAdsrMode == 0);
      ForEachVoice([expAtk](kr106::Voice<T>& v) { v.mADSR.mExponentialAttack = expAtk; });
      SetParam(kEnvA, mSliderA);
      SetParam(kEnvD, mSliderD);
      SetParam(kEnvR, mSliderR);
      break;
    }
    case kBender:
      ForEachVoice([value](kr106::Voice<T>& v) { v.mRawBend = static_cast<float>(value); });
      break;

    case kLfoRate: {
      float bpm = 18.f + static_cast<float>(value) * 1182.f;
      mLFO.SetRate(bpm, mSampleRate);
      break;
    }
    case kLfoDelay:
      mLFO.SetDelay(static_cast<float>(value));
      break;
    case kLfoMode:
      mLFO.SetMode(static_cast<int>(value));
      break;

    case kHpfFreq:
      mHPF.SetMode(static_cast<int>(value));
      break;

    case kVcaLevel: {
      float bias = static_cast<float>(value) * 2.f - 1.f;
      mVcaLevel = std::pow(10.f, bias * 6.f / 20.f);
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
      mOctaveTranspose = 1 - static_cast<int>(value);
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

    case kArpRate: mArp.mRate = static_cast<float>(value); break;
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
