#pragma once
#include "DSPGraph.h"
#include <string>
#include <vector>

// Whole-chain DSP presets: serialize every effect's enabled state + params
// (and the chain order) into a small text blob, apply it back, and manage a
// library of named presets (factory built-ins + user file).
//
// Blob format, one line per item (easy to store inside config.json too):
//   order=Noise Gate|Compressor|...
//   node=Pitch Shifter;on=1;m_pitchFactor=0.72;m_dryWet=1
namespace DspPresets {

// Serialize the graph's current state (order, enabled flags, all params).
std::string serialize(DSPGraph& graph);

// Apply a blob produced by serialize(): reorders the chain and sets every
// listed node's enabled flag + params. Unknown nodes/params are ignored.
void apply(DSPGraph& graph, const std::string& blob);

struct PresetInfo {
    std::string name;
    bool builtIn = false;
};

// Factory presets first, then user presets from the presets file.
std::vector<PresetInfo> list();

// Blob for a named preset ("" if not found). Checks factory then user file.
std::string get(const std::string& name);

// Save/overwrite a USER preset (factory names cannot be overwritten).
bool save(const std::string& name, const std::string& blob);

// Delete a user preset (no-op for factory presets).
void remove(const std::string& name);

} // namespace DspPresets
