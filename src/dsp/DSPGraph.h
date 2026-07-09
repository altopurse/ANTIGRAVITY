#pragma once
#include "DSPNode.h"
#include <vector>
#include <memory>
#include <mutex>
#include <string>

class DSPGraph {
public:
    DSPGraph();
    ~DSPGraph();

    void prepare(double sampleRate);
    void process(float* buffer, size_t numSamples, int numChannels);
    
    // Add effect node to the pipeline (transfers ownership)
    void addNode(std::unique_ptr<DSPNode> node);
    
    // Reorder nodes in the processing graph
    void moveNodeUp(size_t index);
    void moveNodeDown(size_t index);

    // Rearrange the chain to match the given node-name order (names not
    // listed keep their relative order at the end). Used by presets.
    void reorder(const std::vector<std::string>& names);
    
    const std::vector<std::unique_ptr<DSPNode>>& getNodes() const { return m_nodes; }

private:
    std::vector<std::unique_ptr<DSPNode>> m_nodes;
    double m_sampleRate = 48000.0;
    std::mutex m_mutex; // Protects graph structures during real-time processing and UI swaps
};
