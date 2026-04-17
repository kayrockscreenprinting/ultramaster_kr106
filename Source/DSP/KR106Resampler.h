#pragma once

namespace kr106 {

    // ============================================================
    // Inline half-band polyphase IIR resampler
    // ============================================================
    // 12-coefficient allpass polyphase network for 2x up/downsampling.
    // Same algorithm and coefficients as Laurent de Soras' HIIR library (WTFPL).
    //
    // Retained here so the DSP-level mix-bus decimator can reuse the same
    // Downsampler2x struct. Upsampler2x is no longer used by the VCF but
    // kept available for any caller that still wants it.
    static constexpr int kNumResamplerCoefs = 12;
    
    struct Upsampler2x
    {
      float coef[kNumResamplerCoefs] = {};
      float x[kNumResamplerCoefs] = {};
      float y[kNumResamplerCoefs] = {};
    
      void set_coefs(const double c[kNumResamplerCoefs])
      {
        for (int i = 0; i < kNumResamplerCoefs; i++)
          coef[i] = static_cast<float>(c[i]);
      }
    
      void clear_buffers()
      {
        memset(x, 0, sizeof(x));
        memset(y, 0, sizeof(y));
      }
    
      void process_sample(float& out_0, float& out_1, float input)
      {
        float even = input;
        float odd = input;
        for (int i = 0; i < kNumResamplerCoefs; i += 2)
        {
          float t0 = (even - y[i]) * coef[i] + x[i];
          float t1 = (odd - y[i + 1]) * coef[i + 1] + x[i + 1];
          x[i] = even;   x[i + 1] = odd;
          y[i] = t0;     y[i + 1] = t1;
          even = t0;     odd = t1;
        }
        out_0 = even;
        out_1 = odd;
      }
    };
    
    struct Downsampler2x
    {
      float coef[kNumResamplerCoefs] = {};
      float x[kNumResamplerCoefs] = {};
      float y[kNumResamplerCoefs] = {};
    
      void set_coefs(const double c[kNumResamplerCoefs])
      {
        for (int i = 0; i < kNumResamplerCoefs; i++)
          coef[i] = static_cast<float>(c[i]);
      }
    
      void clear_buffers()
      {
        memset(x, 0, sizeof(x));
        memset(y, 0, sizeof(y));
      }
    
      float process_sample(const float in[2])
      {
        float spl_0 = in[1];
        float spl_1 = in[0];
        for (int i = 0; i < kNumResamplerCoefs; i += 2)
        {
          float t0 = (spl_0 - y[i]) * coef[i] + x[i];
          float t1 = (spl_1 - y[i + 1]) * coef[i + 1] + x[i + 1];
          x[i] = spl_0;   x[i + 1] = spl_1;
          y[i] = t0;       y[i + 1] = t1;
          spl_0 = t0;     spl_1 = t1;
        }
        return 0.5f * (spl_0 + spl_1);
      }
    };
    
    // Shared coefficient table (same as HIIR 12-tap half-band).
    // Exposed so the DSP-level decimator pair can initialize identically.
    static constexpr double kResamplerCoefs2x[kNumResamplerCoefs] = {
      0.036681502163648017, 0.13654762463195794, 0.27463175937945444,
      0.42313861743656711, 0.56109869787919531, 0.67754004997416184,
      0.76974183386322703, 0.83988962484963892, 0.89226081800387902,
      0.9315419599631839,  0.96209454837808417, 0.98781637073289585
    };
}