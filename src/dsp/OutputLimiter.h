#pragma once
#include <vector>
#include <cstddef>

// Final loudness stage on the Primary Output (the feed the virtual cable sends
// to Discord/games). Two jobs, in order:
//
//   1. Makeup gain - lifts the whole mixed signal (voice + soundboard) to a
//      consistent, intelligible level so Discord's voice-activity gate stays
//      open on quiet speech instead of cutting in and out.
//   2. Look-ahead brick-wall limiter - catches peaks (loud soundboard hits,
//      shouting) SMOOTHLY just under full scale, instead of the old hard clip
//      that turned every peak into harsh digital distortion.
//
// The limiter shares one gain-reduction value across all channels so the
// stereo image is preserved, and delays the signal by a short look-ahead
// window (~1.5ms) so it can pull the gain down BEFORE a peak arrives - no
// overshoot, no audible clip. A final safety clamp guarantees the output can
// never exceed the ceiling even under pathological input.
//
// Real-time safe: no allocation in process(); the look-ahead ring is sized in
// prepare().
class OutputLimiter {
public:
    void prepare(double sampleRate, int channels);

    // In-place on an interleaved [frames*channels] buffer. makeupGainDb is the
    // user "Output Level" control; when the stage is disabled the caller simply
    // doesn't invoke this.
    void process(float* buffer, size_t frameCount, int channels, float makeupGainDb);

    // Current gain reduction in dB (>= 0), for an optional UI meter. 0 = the
    // limiter isn't touching the signal right now.
    float gainReductionDb() const { return m_lastReductionDb; }

    void reset();

private:
    double m_sampleRate = 48000.0;
    int m_channels = 2;

    // Look-ahead delay line (ring), holding makeup-applied samples.
    std::vector<float> m_delay;
    size_t m_delayFrames = 0;   // look-ahead length in frames
    size_t m_writePos = 0;      // frame write cursor into the ring

    float m_gain = 1.0f;        // smoothed gain-reduction envelope (<= 1)
    float m_attackCoeff = 0.0f; // per-sample smoothing toward a lower gain
    float m_releaseCoeff = 0.0f;// per-sample smoothing back toward unity
    float m_lastReductionDb = 0.0f;

    static constexpr float kCeiling = 0.98f; // brick-wall just below full scale
};
