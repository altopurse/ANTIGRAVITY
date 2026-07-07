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
    m_readPos1 = 0.0f;
    m_readPos2 = static_cast<float>(m_delayLength / 2);
}

void PitchShifterNode::process(float* buffer, size_t numSamples, int numChannels) {
    if (!m_enabled || m_pitchFactor == 1.0f) return;

    float rate = 1.0f - m_pitchFactor;

    for (size_t i = 0; i < numSamples; i += numChannels) {
        for (int c = 0; c < numChannels; ++c) {
            int ch = std::min(c, 1);
            
            // Write input to delay buffer
            m_delayBuffer[ch][m_writePos] = buffer[i + c];
            
            // Read from delay line 1
            int rPos1_int = static_cast<int>(m_readPos1);
            float rPos1_frac = m_readPos1 - rPos1_int;
            int rPos1_next = (rPos1_int + 1) % m_bufferSize;
            float out1 = (1.0f - rPos1_frac) * m_delayBuffer[ch][rPos1_int] + rPos1_frac * m_delayBuffer[ch][rPos1_next];

            // Read from delay line 2
            int rPos2_int = static_cast<int>(m_readPos2);
            float rPos2_frac = m_readPos2 - rPos2_int;
            int rPos2_next = (rPos2_int + 1) % m_bufferSize;
            float out2 = (1.0f - rPos2_frac) * m_delayBuffer[ch][rPos2_int] + rPos2_frac * m_delayBuffer[ch][rPos2_next];

            // Calculate crossfade weight based on delay 1 from write pointer
            float delay1 = static_cast<float>(m_writePos) - m_readPos1;
            if (delay1 < 0) delay1 += m_bufferSize;
            
            // Normalize delay1 to 0..delayLength
            float w = delay1 / m_delayLength;
            if (w < 0.0f) w = 0.0f;
            if (w > 1.0f) w = 1.0f;
            
            // Linear crossfade window
            float weight1 = w;
            float weight2 = 1.0f - w;

            // Combine
            float processed = out1 * weight1 + out2 * weight2;
            
            // Dry/wet mix
            buffer[i + c] = (1.0f - m_dryWet) * buffer[i + c] + m_dryWet * processed;
        }

        // Advance indices
        m_writePos = (m_writePos + 1) % m_bufferSize;
        
        m_readPos1 += m_pitchFactor;
        if (m_readPos1 >= m_bufferSize) m_readPos1 -= m_bufferSize;

        m_readPos2 += m_pitchFactor;
        if (m_readPos2 >= m_bufferSize) m_readPos2 -= m_bufferSize;

        // Keep read positions bounded relative to write position to avoid drift
        float currentDelay1 = static_cast<float>(m_writePos) - m_readPos1;
        if (currentDelay1 < 0) currentDelay1 += m_bufferSize;
        
        if (currentDelay1 >= m_delayLength) {
            // Delay has grown too large, pull read position closer
            m_readPos1 = static_cast<float>((m_writePos - 1 + m_bufferSize) % m_bufferSize);
        } else if (currentDelay1 <= 2.0f) {
            // Delay has shrunk too small, push read position further
            m_readPos1 = static_cast<float>((m_writePos - m_delayLength + m_bufferSize) % m_bufferSize);
        }

        float currentDelay2 = static_cast<float>(m_writePos) - m_readPos2;
        if (currentDelay2 < 0) currentDelay2 += m_bufferSize;

        if (currentDelay2 >= m_delayLength) {
            m_readPos2 = static_cast<float>((m_writePos - 1 + m_bufferSize) % m_bufferSize);
        } else if (currentDelay2 <= 2.0f) {
            m_readPos2 = static_cast<float>((m_writePos - m_delayLength + m_bufferSize) % m_bufferSize);
        }
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
