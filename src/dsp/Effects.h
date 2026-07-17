#pragma once
#include "DSPNode.h"
#include "license/Entitlement.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Biquad Filter Helper
class Biquad {
public:
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void reset() {
        x1 = x2 = y1 = y2 = 0;
    }

    inline float process(float x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return static_cast<float>(y);
    }

    void setLowpass(double cutoff, double sampleRate, double Q = 0.7071) {
        double w0 = 2.0 * M_PI * cutoff / sampleRate;
        double alpha = sin(w0) / (2.0 * Q);
        double cosw0 = cos(w0);

        double a0 = 1.0 + alpha;
        b0 = (1.0 - cosw0) / 2.0 / a0;
        b1 = (1.0 - cosw0) / a0;
        b2 = (1.0 - cosw0) / 2.0 / a0;
        a1 = -2.0 * cosw0 / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void setHighpass(double cutoff, double sampleRate, double Q = 0.7071) {
        double w0 = 2.0 * M_PI * cutoff / sampleRate;
        double alpha = sin(w0) / (2.0 * Q);
        double cosw0 = cos(w0);

        double a0 = 1.0 + alpha;
        b0 = (1.0 + cosw0) / 2.0 / a0;
        b1 = -(1.0 + cosw0) / a0;
        b2 = (1.0 + cosw0) / 2.0 / a0;
        a1 = -2.0 * cosw0 / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void setPeakingEQ(double freq, double sampleRate, double Q, double gainDB) {
        double A = pow(10.0, gainDB / 40.0);
        double w0 = 2.0 * M_PI * freq / sampleRate;
        double alpha = sin(w0) / (2.0 * Q);
        double cosw0 = cos(w0);

        double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = -2.0 * cosw0 / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = -2.0 * cosw0 / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    void setLowShelf(double freq, double sampleRate, double Q, double gainDB) {
        double A = pow(10.0, gainDB / 40.0);
        double w0 = 2.0 * M_PI * freq / sampleRate;
        double alpha = sin(w0) / (2.0 * Q);
        double cosw0 = cos(w0);
        double twoSqrtA = 2.0 * sqrt(A) * alpha;

        double a0 = (A + 1.0) + (A - 1.0) * cosw0 + twoSqrtA;
        b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + twoSqrtA) / a0;
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
        b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtA) / a0;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
        a2 = ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtA) / a0;
    }

    void setHighShelf(double freq, double sampleRate, double Q, double gainDB) {
        double A = pow(10.0, gainDB / 40.0);
        double w0 = 2.0 * M_PI * freq / sampleRate;
        double alpha = sin(w0) / (2.0 * Q);
        double cosw0 = cos(w0);
        double twoSqrtA = 2.0 * sqrt(A) * alpha;

        double a0 = (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtA;
        b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtA) / a0;
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
        b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtA) / a0;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtA) / a0;
    }
};

// 0. Noise Suppressor (RNNoise - recurrent-neural-net denoiser). Unlike the
// gate (which just mutes below a threshold), this removes steady background
// noise (fans, hum, hiss, keyboard rumble) WHILE speech is happening.
// Runs on 480-sample frames at 48kHz internally; a small FIFO adapts the
// engine's block size to that, costing a fixed 10ms of latency when enabled.
class NoiseSuppressorNode : public DSPNode {
public:
    NoiseSuppressorNode();
    ~NoiseSuppressorNode() override;
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Noise Suppressor (AI)"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = false;
    float m_strength = 1.0f; // 0 = bypass blend, 1 = fully denoised

private:
    struct ChannelState {
        void* st = nullptr;             // DenoiseState* (opaque - C API)
        std::vector<float> inFifo;      // pending input, rnnoise scale (+-32768)
        std::vector<float> outFifo;     // denoised output, rnnoise scale
        std::vector<float> dryFifo;     // latency-matched dry copy for blending
    };
    ChannelState m_ch[2];
    int m_frameSize = 480;

    void resetChannel(ChannelState& ch);
    void destroyStates();
};

// 1. Noise Gate
class NoiseGateNode : public DSPNode {
public:
    NoiseGateNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Noise Gate"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = true;
    float m_thresholdDB = -45.0f; // Gate opens above this
    float m_releaseMs = 150.0f;    // Time to close gate in ms

private:
    double m_sampleRate = 48000.0;
    float m_envelope = 0.0f;
    float m_gain = 1.0f;
};

// 2. Compressor
class CompressorNode : public DSPNode {
public:
    CompressorNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Compressor"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = true;
    float m_thresholdDB = -18.0f;
    float m_ratio = 4.0f; // 4:1 compression
    float m_attackMs = 10.0f;
    float m_releaseMs = 100.0f;
    float m_makeupGainDB = 4.0f;

private:
    double m_sampleRate = 48000.0;
    float m_envelope = 0.0f;
};

// 3. 3-Band Parametric EQ
class ParametricEQNode : public DSPNode {
public:
    ParametricEQNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Parametric EQ"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = true;
    
    // Low Shelf
    float m_lowShelfFreq = 200.0f;
    float m_lowShelfGain = 0.0f; // dB
    
    // Mid Peak
    float m_midFreq = 1000.0f;
    float m_midGain = 0.0f; // dB
    float m_midQ = 1.0f;
    
    // High Shelf
    float m_highShelfFreq = 4000.0f;
    float m_highShelfGain = 0.0f; // dB

private:
    double m_sampleRate = 48000.0;
    Biquad m_lowFilter[2];
    Biquad m_midFilter[2];
    Biquad m_highFilter[2];
};

// 4. Pitch Shifter (Time-Domain delay line with crossfade - zero block-latency!)
class PitchShifterNode : public DSPNode {
public:
    PitchShifterNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Pitch Shifter"; }
    bool& isEnabled() override { return m_enabled; }

    bool m_enabled = true;
    float m_pitchFactor = 1.0f; // 0.5 (deep) to 2.0 (chipmunk)
    float m_dryWet = 1.0f;       // 0.0 to 1.0

private:
    double m_sampleRate = 48000.0;
    std::vector<float> m_delayBuffer[2]; // Stereo buffers
    int m_writePos = 0;
    float m_phase = 0.0f;     // Tap-1 delay in frames (sawtooth over 0..m_delayLength)
    int m_bufferSize = 8192;
    int m_delayLength = 2048; // Size of overlap window

    // Linear-interpolated read `delay` frames behind the write head
    float readTap(int channel, float delay) const;
};

// 5. Robotizer (Ring Modulator)
class RobotizerNode : public DSPNode {
public:
    RobotizerNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Robotizer (Ring Mod)"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = false;
    float m_modFreq = 80.0f; // Carrier sine frequency
    float m_dryWet = 0.7f;

private:
    double m_sampleRate = 48000.0;
    double m_phase = 0.0;
};

// 6. Reverb (Feedback Delay Line Schroeder Reverb)
class ReverbNode : public DSPNode {
public:
    ReverbNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Reverb"; }
    bool& isEnabled() override { return m_enabled; }

    bool m_enabled = false;
    float m_roomSize = 0.7f;  // Feedback coefficient
    float m_damping = 0.3f;   // Lowpass filter coefficient
    float m_dryWet = 0.3f;

private:
    double m_sampleRate = 48000.0;
    
    // Comb & All-pass Filters for Schroeder Reverb
    struct CombFilter {
        std::vector<float> buffer;
        size_t writePos = 0;
        float feedback = 0.5f;
        float filterState = 0.0f;
        float damping = 0.2f;

        void resize(size_t size) {
            buffer.assign(size, 0.0f);
            writePos = 0;
        }
        
        inline float process(float input) {
            float output = buffer[writePos];
            filterState = (output * (1.0f - damping)) + (filterState * damping);
            // Flush denormals: after silence the feedback tail decays into
            // denormal range, which is extremely slow on x86 and causes
            // CPU spikes/crackling. Snap tiny values to true zero instead.
            if (filterState > -1e-15f && filterState < 1e-15f) filterState = 0.0f;
            buffer[writePos] = input + (filterState * feedback);
            writePos = (writePos + 1) % buffer.size();
            return output;
        }
    };

    struct AllPassFilter {
        std::vector<float> buffer;
        size_t writePos = 0;
        float feedback = 0.5f;

        void resize(size_t size) {
            buffer.assign(size, 0.0f);
            writePos = 0;
        }
        
        inline float process(float input) {
            float bufOut = buffer[writePos];
            if (bufOut > -1e-15f && bufOut < 1e-15f) bufOut = 0.0f; // denormal flush
            float output = -input + bufOut;
            buffer[writePos] = input + (bufOut * feedback);
            writePos = (writePos + 1) % buffer.size();
            return output;
        }
    };

    std::vector<CombFilter> m_combFiltersL;
    std::vector<CombFilter> m_combFiltersR;
    std::vector<AllPassFilter> m_allPassFiltersL;
    std::vector<AllPassFilter> m_allPassFiltersR;
};

// 7. Distortion
class DistortionNode : public DSPNode {
public:
    DistortionNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Distortion"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = false;
    float m_drive = 5.0f; // Gain multiplier before waveshaping
    float m_dryWet = 0.4f;
};

// 8. Telephone Filter (Bandpass 300Hz - 3400Hz + subtle crunch)
class TelephoneFilterNode : public DSPNode {
public:
    TelephoneFilterNode();
    void prepare(double sampleRate) override;
    void process(float* buffer, size_t numSamples, int numChannels) override;
    const char* getName() const override { return "Telephone Filter"; }
    bool& isEnabled() override { return m_enabled; }
    uint32_t requiredFeature() const override { return ent::FEAT_ALL_EFFECTS; }

    bool m_enabled = false;
    float m_dryWet = 1.0f;

private:
    double m_sampleRate = 48000.0;
    Biquad m_lowFilter[2];
    Biquad m_highFilter[2];
};
