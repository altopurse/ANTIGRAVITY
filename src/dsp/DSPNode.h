#pragma once
#include <string>
#include <cstdint>

class DSPNode {
public:
    virtual ~DSPNode() = default;

    // Prepare filter for processing given a sample rate
    virtual void prepare(double sampleRate) = 0;

    // Process in-place buffer
    virtual void process(float* buffer, size_t numSamples, int numChannels) = 0;

    // Get display name of filter
    virtual const char* getName() const = 0;

    // Reference to enabled state for GUI checkbox toggling
    virtual bool& isEnabled() = 0;

    // Entitlement bit required to run this effect. 0 means free (always
    // available). Premium effects override to ent::FEAT_ALL_EFFECTS. The graph
    // consults this each block; premium nodes also re-check inside process()
    // (a second translation unit) so a single patch can't unlock them.
    virtual uint32_t requiredFeature() const { return 0u; }
};
