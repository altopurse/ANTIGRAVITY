#include "DSPGraph.h"
#include <algorithm>

DSPGraph::DSPGraph() {}
DSPGraph::~DSPGraph() {}

void DSPGraph::prepare(double sampleRate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sampleRate = sampleRate;
    for (auto& node : m_nodes) {
        node->prepare(sampleRate);
    }
}

void DSPGraph::process(float* buffer, size_t numSamples, int numChannels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& node : m_nodes) {
        if (node->isEnabled()) {
            node->process(buffer, numSamples, numChannels);
        }
    }
}

void DSPGraph::addNode(std::unique_ptr<DSPNode> node) {
    std::lock_guard<std::mutex> lock(m_mutex);
    node->prepare(m_sampleRate);
    m_nodes.push_back(std::move(node));
}

void DSPGraph::moveNodeUp(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index > 0 && index < m_nodes.size()) {
        std::swap(m_nodes[index], m_nodes[index - 1]);
    }
}

void DSPGraph::reorder(const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::unique_ptr<DSPNode>> result;
    result.reserve(m_nodes.size());
    for (const auto& name : names) {
        for (auto& node : m_nodes) {
            if (node && name == node->getName()) {
                result.push_back(std::move(node));
                break;
            }
        }
    }
    // Keep any nodes the preset didn't mention (e.g. effects added in a
    // newer app version) in their existing relative order at the end.
    for (auto& node : m_nodes) {
        if (node) result.push_back(std::move(node));
    }
    m_nodes = std::move(result);
}

void DSPGraph::moveNodeDown(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Guard against underflow: if m_nodes is empty, m_nodes.size() - 1 would
    // wrap around (size_t is unsigned) and the old check would pass with an
    // out-of-bounds index, causing undefined behavior.
    if (!m_nodes.empty() && index + 1 < m_nodes.size()) {
        std::swap(m_nodes[index], m_nodes[index + 1]);
    }
}
