#pragma once
#include <string>

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
};
