#include "Effects.h"
#include <algorithm>
#include <iostream>

// ==========================================
// 1. Noise Gate
// ==========================================
NoiseGateNode::NoiseGateNode() {}

void NoiseGateNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    m_envelope = 0.0f;
    m_gain = 1.0f;
}

void NoiseGateNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;
    // Entitlement gate (check #2 - see DSPGraph::process for #1). Independent
    // bypass in this translation unit so unlocking premium effects takes
    // patching every copy, not one.
    if (!ent::hasFeature(ent::FEAT_ALL_EFFECTS)) return;

    float threshold = pow(10.0f, m_thresholdDB / 20.0f);
    // Release coefficient based on release time
    float releaseCoeff = exp(-1.0f / (m_releaseMs * 0.001f * m_sampleRate));
    float attackCoeff = exp(-1.0f / (0.001f * m_sampleRate)); // fast 1ms attack for gate opening

    for (size_t i = 0; i < numSamples; i += numChannels) {
        float maxVal = 0.0f;
        for (int c = 0; c < numChannels; ++c) {
            maxVal = std::max(maxVal, std::abs(buffer[i + c]));
        }

        // Smooth envelope follower
        if (maxVal > m_envelope) {
            m_envelope = attackCoeff * m_envelope + (1.0f - attackCoeff) * maxVal;
        } else {
            m_envelope = releaseCoeff * m_envelope + (1.0f - releaseCoeff) * maxVal;
        }

        // Determine target gain
        float targetGain = (m_envelope > threshold) ? 1.0f : 0.0f;

        // Smooth gain transition
        m_gain = 0.95f * m_gain + 0.05f * targetGain;

        for (int c = 0; c < numChannels; ++c) {
            buffer[i + c] *= m_gain;
        }
    }
}

// ==========================================
// 2. Compressor
// ==========================================
CompressorNode::CompressorNode() {}

void CompressorNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    m_envelope = 0.0f;
}

void CompressorNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;
    if (!ent::hasFeature(ent::FEAT_ALL_EFFECTS)) return;

    float threshold = pow(10.0f, m_thresholdDB / 20.0f);
    float attackCoeff = exp(-1.0f / (m_attackMs * 0.001f * m_sampleRate));
    float releaseCoeff = exp(-1.0f / (m_releaseMs * 0.001f * m_sampleRate));
    float makeupGain = pow(10.0f, m_makeupGainDB / 20.0f);

    for (size_t i = 0; i < numSamples; i += numChannels) {
        float maxVal = 0.0f;
        for (int c = 0; c < numChannels; ++c) {
            maxVal = std::max(maxVal, std::abs(buffer[i + c]));
        }

        // Envelope follower
        if (maxVal > m_envelope) {
            m_envelope = attackCoeff * m_envelope + (1.0f - attackCoeff) * maxVal;
        } else {
            m_envelope = releaseCoeff * m_envelope + (1.0f - releaseCoeff) * maxVal;
        }

        float gain = 1.0f;
        if (m_envelope > threshold && m_envelope > 0.0f) {
            // Convert to dB
            float envDB = 20.0f * log10(m_envelope);
            // Apply ratio to the signal exceeding threshold
            float targetDB = m_thresholdDB + (envDB - m_thresholdDB) / m_ratio;
            // Target gain in multiplier
            gain = pow(10.0f, (targetDB - envDB) / 20.0f);
        }

        // Apply compression gain + makeup gain
        for (int c = 0; c < numChannels; ++c) {
            buffer[i + c] *= gain * makeupGain;
        }
    }
}

// ==========================================
// 3. Parametric EQ
// ==========================================
ParametricEQNode::ParametricEQNode() {}

void ParametricEQNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    for (int c = 0; c < 2; ++c) {
        m_lowFilter[c].reset();
        m_midFilter[c].reset();
        m_highFilter[c].reset();
    }
}

void ParametricEQNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;
    if (!ent::hasFeature(ent::FEAT_ALL_EFFECTS)) return;

    // Recalculate coefficients
    for (int c = 0; c < std::min(numChannels, 2); ++c) {
        m_lowFilter[c].setLowShelf(m_lowShelfFreq, m_sampleRate, 0.7071, m_lowShelfGain);
        m_midFilter[c].setPeakingEQ(m_midFreq, m_sampleRate, m_midQ, m_midGain);
        m_highFilter[c].setHighShelf(m_highShelfFreq, m_sampleRate, 0.7071, m_highShelfGain);
    }

    for (size_t i = 0; i < numSamples; i += numChannels) {
        for (int c = 0; c < numChannels; ++c) {
            int filterIdx = std::min(c, 1);
            float sample = buffer[i + c];
            
            sample = m_lowFilter[filterIdx].process(sample);
            sample = m_midFilter[filterIdx].process(sample);
            sample = m_highFilter[filterIdx].process(sample);
            
            buffer[i + c] = sample;
        }
    }
}

// ==========================================
// 4. Pitch Shifter
// ==========================================
PitchShifterNode::PitchShifterNode() {
    m_delayBuffer[0].assign(m_bufferSize, 0.0f);
    m_delayBuffer[1].assign(m_bufferSize, 0.0f);
}

void PitchShifterNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    std::fill(m_delayBuffer[0].begin(), m_delayBuffer[0].end(), 0.0f);
    std::fill(m_delayBuffer[1].begin(), m_delayBuffer[1].end(), 0.0f);
    m_writePos = 0;
    m_phase = 0.0f;
}

float PitchShifterNode::readTap(int channel, float delay) const {
    float pos = static_cast<float>(m_writePos) - delay;
    if (pos < 0.0f) pos += static_cast<float>(m_bufferSize);
    int i0 = static_cast<int>(pos);
    float frac = pos - static_cast<float>(i0);
    int i1 = i0 + 1;
    if (i1 >= m_bufferSize) i1 = 0;
    const std::vector<float>& buf = m_delayBuffer[channel];
    return (1.0f - frac) * buf[i0] + frac * buf[i1];
}

void PitchShifterNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;

    if (m_pitchFactor == 1.0f) {
        // Passthrough, but keep the delay line fed so engaging the shifter
        // doesn't replay stale audio from the last time it ran.
        for (size_t i = 0; i < numSamples; i += numChannels) {
            for (int c = 0; c < numChannels; ++c) {
                m_delayBuffer[std::min(c, 1)][m_writePos] = buffer[i + c];
            }
            m_writePos = (m_writePos + 1) % m_bufferSize;
        }
        return;
    }

    const float L = static_cast<float>(m_delayLength);
    const float halfL = L * 0.5f;
    // Per-frame delay drift: factor < 1 grows the delay, > 1 shrinks it.
    const float drift = 1.0f - m_pitchFactor;

    for (size_t i = 0; i < numSamples; i += numChannels) {
        // Two taps half a window apart. Triangle weights hit zero exactly
        // where each tap's delay wraps (0 <-> L), so the wrap never clicks.
        float d1 = m_phase;
        float d2 = d1 + halfL;
        if (d2 >= L) d2 -= L;
        float w1 = 1.0f - std::abs(2.0f * d1 / L - 1.0f);
        float w2 = 1.0f - w1;

        for (int c = 0; c < numChannels; ++c) {
            int ch = std::min(c, 1);
            m_delayBuffer[ch][m_writePos] = buffer[i + c];

            float processed = readTap(ch, d1) * w1 + readTap(ch, d2) * w2;
            buffer[i + c] = (1.0f - m_dryWet) * buffer[i + c] + m_dryWet * processed;
        }

        m_writePos = (m_writePos + 1) % m_bufferSize;

        m_phase += drift;
        if (m_phase >= L) m_phase -= L;
        else if (m_phase < 0.0f) m_phase += L;
    }
}

// ==========================================
// 5. Robotizer (Ring Modulator)
// ==========================================
RobotizerNode::RobotizerNode() {}

void RobotizerNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    m_phase = 0.0;
}

void RobotizerNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;
    if (!ent::hasFeature(ent::FEAT_ALL_EFFECTS)) return;

    double phaseIncrement = 2.0 * M_PI * m_modFreq / m_sampleRate;

    for (size_t i = 0; i < numSamples; i += numChannels) {
        float modulator = static_cast<float>(sin(m_phase));
        m_phase += phaseIncrement;
        if (m_phase >= 2.0 * M_PI) m_phase -= 2.0 * M_PI;

        for (int c = 0; c < numChannels; ++c) {
            float processed = buffer[i + c] * modulator;
            buffer[i + c] = (1.0f - m_dryWet) * buffer[i + c] + m_dryWet * processed;
        }
    }
}

// ==========================================
// 6. Reverb
// ==========================================
ReverbNode::ReverbNode() {
    m_combFiltersL.resize(4);
    m_combFiltersR.resize(4);
    m_allPassFiltersL.resize(2);
    m_allPassFiltersR.resize(2);
}

void ReverbNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;

    // Comb filter delay line lengths in samples at 48kHz
    // (slightly adjusted for L/R channel decorrelation to give width)
    size_t sizesL[4] = {1116, 1188, 1277, 1356};
    size_t sizesR[4] = {1138, 1205, 1251, 1378};

    size_t allPassSizesL[2] = {225, 341};
    size_t allPassSizesR[2] = {243, 317};

    float scale = static_cast<float>(sampleRate / 48000.0);

    for (int i = 0; i < 4; ++i) {
        m_combFiltersL[i].resize(static_cast<size_t>(sizesL[i] * scale));
        m_combFiltersR[i].resize(static_cast<size_t>(sizesR[i] * scale));
        
        m_combFiltersL[i].feedback = m_roomSize;
        m_combFiltersR[i].feedback = m_roomSize;
        
        m_combFiltersL[i].damping = m_damping;
        m_combFiltersR[i].damping = m_damping;
    }

    for (int i = 0; i < 2; ++i) {
        m_allPassFiltersL[i].resize(static_cast<size_t>(allPassSizesL[i] * scale));
        m_allPassFiltersR[i].resize(static_cast<size_t>(allPassSizesR[i] * scale));
        m_allPassFiltersL[i].feedback = 0.5f;
        m_allPassFiltersR[i].feedback = 0.5f;
    }
}

void ReverbNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;

    for (int i = 0; i < 4; ++i) {
        m_combFiltersL[i].feedback = m_roomSize;
        m_combFiltersR[i].feedback = m_roomSize;
        m_combFiltersL[i].damping = m_damping;
        m_combFiltersR[i].damping = m_damping;
    }

    for (size_t i = 0; i < numSamples; i += numChannels) {
        float inL = buffer[i];
        float inR = (numChannels > 1) ? buffer[i + 1] : buffer[i];

        // 1. Parallel Comb Filters
        float outCombL = 0.0f;
        float outCombR = 0.0f;
        for (int f = 0; f < 4; ++f) {
            outCombL += m_combFiltersL[f].process(inL * 0.125f);
            outCombR += m_combFiltersR[f].process(inR * 0.125f);
        }

        // 2. All-Pass Filters in series
        float outAPL = outCombL;
        float outAPR = outCombR;
        for (int a = 0; a < 2; ++a) {
            outAPL = m_allPassFiltersL[a].process(outAPL);
            outAPR = m_allPassFiltersR[a].process(outAPR);
        }

        // Mix back
        buffer[i] = (1.0f - m_dryWet) * buffer[i] + m_dryWet * outAPL;
        if (numChannels > 1) {
            buffer[i + 1] = (1.0f - m_dryWet) * buffer[i + 1] + m_dryWet * outAPR;
        }
    }
}

// ==========================================
// 7. Distortion
// ==========================================
DistortionNode::DistortionNode() {}

void DistortionNode::prepare(double) {}

void DistortionNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;
    if (!ent::hasFeature(ent::FEAT_ALL_EFFECTS)) return;

    float d = m_drive;
    float norm = 1.0f / tanh(d);

    for (size_t i = 0; i < numSamples; ++i) {
        float in = buffer[i];
        float out = tanh(in * d) * norm;
        buffer[i] = (1.0f - m_dryWet) * in + m_dryWet * out;
    }
}

// ==========================================
// 8. Telephone Filter
// ==========================================
TelephoneFilterNode::TelephoneFilterNode() {}

void TelephoneFilterNode::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    for (int c = 0; c < 2; ++c) {
        m_lowFilter[c].reset();
        m_highFilter[c].reset();
    }
}

void TelephoneFilterNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled) return;
    if (!ent::hasFeature(ent::FEAT_ALL_EFFECTS)) return;

    for (int c = 0; c < std::min(numChannels, 2); ++c) {
        // Highpass at 300Hz, Lowpass at 3400Hz
        m_lowFilter[c].setHighpass(300.0, m_sampleRate, 0.7071);
        m_highFilter[c].setLowpass(3400.0, m_sampleRate, 0.7071);
    }

    for (size_t i = 0; i < numSamples; i += numChannels) {
        for (int c = 0; c < numChannels; ++c) {
            int filterIdx = std::min(c, 1);
            float sample = buffer[i + c];
            
            // Bandpass filter (HPF + LPF in series)
            sample = m_lowFilter[filterIdx].process(sample);
            sample = m_highFilter[filterIdx].process(sample);
            
            // Add slight crunch
            float telephoneCrunch = sample;
            if (std::abs(telephoneCrunch) > 0.05f) {
                telephoneCrunch = tanh(telephoneCrunch * 1.5f) / 1.2f;
            }

            buffer[i + c] = (1.0f - m_dryWet) * buffer[i + c] + m_dryWet * telephoneCrunch;
        }
    }
}
