#include "OutputLimiter.h"
#include <cmath>
#include <algorithm>

void OutputLimiter::prepare(double sampleRate, int channels) {
    m_sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    m_channels = channels > 0 ? channels : 2;

    // ~1.5ms look-ahead: long enough to bring the gain down before a peak
    // lands, short enough to be imperceptible latency for live voice.
    m_delayFrames = static_cast<size_t>(m_sampleRate * 0.0015);
    if (m_delayFrames < 1) m_delayFrames = 1;
    m_delay.assign(m_delayFrames * m_channels, 0.0f);
    m_writePos = 0;

    // Attack reaches the needed reduction across the look-ahead window (so the
    // peak is fully tamed by the time it exits the delay line). Release is slow
    // enough (~120ms) that gentle, pumping-free recovery follows a peak.
    double attackSamples = std::max<double>(1.0, m_sampleRate * 0.0015);
    double releaseSamples = std::max<double>(1.0, m_sampleRate * 0.120);
    m_attackCoeff = static_cast<float>(std::exp(-1.0 / attackSamples));
    m_releaseCoeff = static_cast<float>(std::exp(-1.0 / releaseSamples));

    m_gain = 1.0f;
    m_lastReductionDb = 0.0f;
}

void OutputLimiter::reset() {
    std::fill(m_delay.begin(), m_delay.end(), 0.0f);
    m_writePos = 0;
    m_gain = 1.0f;
    m_lastReductionDb = 0.0f;
}

void OutputLimiter::process(float* buffer, size_t frameCount, int channels, float makeupGainDb) {
    if (channels != m_channels || m_delay.empty()) {
        // Channel count changed under us (device switch) - re-prepare lazily.
        prepare(m_sampleRate, channels);
    }
    const float makeup = std::pow(10.0f, makeupGainDb / 20.0f);
    float maxReduction = 0.0f; // track worst-case for the meter

    for (size_t f = 0; f < frameCount; ++f) {
        // 1) Apply makeup gain to the incoming frame and find its peak.
        float peak = 0.0f;
        float in[8]; // interleaved apps never exceed a couple of channels
        const int ch = channels < 8 ? channels : 8;
        for (int c = 0; c < ch; ++c) {
            float s = buffer[f * channels + c] * makeup;
            in[c] = s;
            peak = std::max(peak, std::fabs(s));
        }

        // 2) Gain the limiter would need to keep this frame under the ceiling.
        float desired = (peak > kCeiling) ? (kCeiling / peak) : 1.0f;

        // 3) Smooth toward it: attack fast when we need MORE reduction (desired
        //    below current), release slowly when we need less.
        if (desired < m_gain) {
            m_gain = m_attackCoeff * m_gain + (1.0f - m_attackCoeff) * desired;
        } else {
            m_gain = m_releaseCoeff * m_gain + (1.0f - m_releaseCoeff) * desired;
        }

        // 4) Push this makeup-applied frame into the look-ahead ring, and pull
        //    out the frame from m_delayFrames ago to actually emit. Because we
        //    started reducing gain m_delayFrames earlier, the delayed loud peak
        //    now meets an already-lowered gain.
        size_t slot = m_writePos * channels;
        float out[8];
        for (int c = 0; c < ch; ++c) {
            out[c] = m_delay[slot + c];      // delayed sample (to output)
            m_delay[slot + c] = in[c];       // store current (makeup applied)
        }
        m_writePos = (m_writePos + 1) % m_delayFrames;

        // 5) Apply the (shared) gain reduction + a hard safety clamp so the
        //    ceiling is guaranteed even if the envelope slightly overshoots.
        for (int c = 0; c < ch; ++c) {
            float s = out[c] * m_gain;
            if (s > kCeiling) s = kCeiling;
            else if (s < -kCeiling) s = -kCeiling;
            buffer[f * channels + c] = s;
        }
        // Channels beyond 8 (never happens): leave untouched rather than crash.

        maxReduction = std::max(maxReduction, 1.0f - m_gain);
    }

    // Convert the worst gain factor this block into dB of reduction for the UI.
    if (maxReduction > 0.0f) {
        float g = 1.0f - maxReduction;
        m_lastReductionDb = (g > 0.0f) ? -20.0f * std::log10(g) : 60.0f;
    } else {
        m_lastReductionDb = 0.0f;
    }
}
