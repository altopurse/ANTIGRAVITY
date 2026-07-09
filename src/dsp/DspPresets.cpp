#include "DspPresets.h"
#include "Effects.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {

struct ParamRef {
    const char* name;
    float* value;
};

// Central registry of every tweakable float on every node type. Serialization
// and preset-apply both go through this, so adding a param here is the only
// step needed to make it persist.
std::vector<ParamRef> collectParams(DSPNode* node) {
    std::vector<ParamRef> out;
    if (auto* g = dynamic_cast<NoiseGateNode*>(node)) {
        out = {{"thresholdDB", &g->m_thresholdDB}, {"releaseMs", &g->m_releaseMs}};
    } else if (auto* c = dynamic_cast<CompressorNode*>(node)) {
        out = {{"thresholdDB", &c->m_thresholdDB}, {"ratio", &c->m_ratio},
               {"attackMs", &c->m_attackMs}, {"releaseMs", &c->m_releaseMs},
               {"makeupGainDB", &c->m_makeupGainDB}};
    } else if (auto* e = dynamic_cast<ParametricEQNode*>(node)) {
        out = {{"lowShelfFreq", &e->m_lowShelfFreq}, {"lowShelfGain", &e->m_lowShelfGain},
               {"midFreq", &e->m_midFreq}, {"midGain", &e->m_midGain}, {"midQ", &e->m_midQ},
               {"highShelfFreq", &e->m_highShelfFreq}, {"highShelfGain", &e->m_highShelfGain}};
    } else if (auto* p = dynamic_cast<PitchShifterNode*>(node)) {
        out = {{"pitchFactor", &p->m_pitchFactor}, {"dryWet", &p->m_dryWet}};
    } else if (auto* r = dynamic_cast<RobotizerNode*>(node)) {
        out = {{"modFreq", &r->m_modFreq}, {"dryWet", &r->m_dryWet}};
    } else if (auto* v = dynamic_cast<ReverbNode*>(node)) {
        out = {{"roomSize", &v->m_roomSize}, {"damping", &v->m_damping}, {"dryWet", &v->m_dryWet}};
    } else if (auto* d = dynamic_cast<DistortionNode*>(node)) {
        out = {{"drive", &d->m_drive}, {"dryWet", &d->m_dryWet}};
    } else if (auto* t = dynamic_cast<TelephoneFilterNode*>(node)) {
        out = {{"dryWet", &t->m_dryWet}};
    }
    return out;
}

std::string presetsFilePath() {
    const char* base = std::getenv("APPDATA");
    fs::path dir = base ? fs::path(base) / "Antigravity" : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "presets.txt").string();
}

// ---------------------------------------------------------------------------
// Factory presets: built once by tweaking a default-constructed chain, so
// every preset always covers every node (loading one gives a fully
// deterministic sound, not a partial overlay on the previous settings).
// ---------------------------------------------------------------------------

std::unique_ptr<DSPGraph> makeDefaultGraph() {
    auto g = std::make_unique<DSPGraph>();
    g->addNode(std::make_unique<NoiseGateNode>());
    g->addNode(std::make_unique<CompressorNode>());
    g->addNode(std::make_unique<ParametricEQNode>());
    g->addNode(std::make_unique<PitchShifterNode>());
    g->addNode(std::make_unique<RobotizerNode>());
    g->addNode(std::make_unique<DistortionNode>());
    g->addNode(std::make_unique<TelephoneFilterNode>());
    g->addNode(std::make_unique<ReverbNode>());
    return g;
}

template <typename T>
T* findNode(DSPGraph& g) {
    for (auto& n : g.getNodes()) {
        if (auto* t = dynamic_cast<T*>(n.get())) return t;
    }
    return nullptr;
}

const std::vector<std::pair<std::string, std::string>>& factoryPresets() {
    static std::vector<std::pair<std::string, std::string>> presets = [] {
        std::vector<std::pair<std::string, std::string>> out;
        auto add = [&out](const char* name, void (*tweak)(DSPGraph&)) {
            auto g = makeDefaultGraph();
            tweak(*g);
            out.emplace_back(name, DspPresets::serialize(*g));
        };

        add("Clean (Default)", [](DSPGraph&) {});
        add("Deep Voice", [](DSPGraph& g) {
            findNode<PitchShifterNode>(g)->m_pitchFactor = 0.72f;
        });
        add("Chipmunk", [](DSPGraph& g) {
            findNode<PitchShifterNode>(g)->m_pitchFactor = 1.45f;
        });
        add("Robot", [](DSPGraph& g) {
            auto* r = findNode<RobotizerNode>(g);
            r->m_enabled = true; r->m_modFreq = 80.0f; r->m_dryWet = 0.9f;
            auto* d = findNode<DistortionNode>(g);
            d->m_enabled = true; d->m_drive = 4.0f; d->m_dryWet = 0.25f;
        });
        add("Telephone", [](DSPGraph& g) {
            auto* t = findNode<TelephoneFilterNode>(g);
            t->m_enabled = true; t->m_dryWet = 1.0f;
        });
        add("Cave Echo", [](DSPGraph& g) {
            auto* v = findNode<ReverbNode>(g);
            v->m_enabled = true; v->m_roomSize = 0.92f; v->m_damping = 0.25f; v->m_dryWet = 0.45f;
        });
        add("Monster", [](DSPGraph& g) {
            findNode<PitchShifterNode>(g)->m_pitchFactor = 0.55f;
            auto* d = findNode<DistortionNode>(g);
            d->m_enabled = true; d->m_drive = 6.0f; d->m_dryWet = 0.35f;
            auto* v = findNode<ReverbNode>(g);
            v->m_enabled = true; v->m_roomSize = 0.6f; v->m_dryWet = 0.2f;
        });
        return out;
    }();
    return presets;
}

// User presets file: "[preset:Name]" section headers, blob lines beneath.
std::map<std::string, std::string> loadUserPresets() {
    std::map<std::string, std::string> out;
    std::ifstream f(presetsFilePath());
    if (!f) return out;

    std::string line, current, body;
    auto flush = [&]() {
        if (!current.empty() && !body.empty()) out[current] = body;
        body.clear();
    };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("[preset:", 0) == 0 && line.back() == ']') {
            flush();
            current = line.substr(8, line.size() - 9);
        } else if (!current.empty() && !line.empty()) {
            body += line + "\n";
        }
    }
    flush();
    return out;
}

bool saveUserPresets(const std::map<std::string, std::string>& presets) {
    std::ofstream f(presetsFilePath(), std::ios::trunc);
    if (!f) return false;
    for (const auto& [name, body] : presets) {
        f << "[preset:" << name << "]\n" << body;
        if (!body.empty() && body.back() != '\n') f << "\n";
        f << "\n";
    }
    return true;
}

bool isFactory(const std::string& name) {
    for (const auto& [n, blob] : factoryPresets()) {
        if (n == name) return true;
    }
    return false;
}

} // namespace

namespace DspPresets {

std::string serialize(DSPGraph& graph) {
    std::ostringstream out;

    out << "order=";
    const auto& nodes = graph.getNodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        out << nodes[i]->getName() << (i + 1 < nodes.size() ? "|" : "");
    }
    out << "\n";

    for (const auto& node : nodes) {
        out << "node=" << node->getName() << ";on=" << (node->isEnabled() ? 1 : 0);
        for (const auto& p : collectParams(node.get())) {
            out << ";" << p.name << "=" << *p.value;
        }
        out << "\n";
    }
    return out.str();
}

void apply(DSPGraph& graph, const std::string& blob) {
    std::istringstream in(blob);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("order=", 0) == 0) {
            std::vector<std::string> names;
            std::string rest = line.substr(6), part;
            std::istringstream split(rest);
            while (std::getline(split, part, '|')) {
                if (!part.empty()) names.push_back(part);
            }
            graph.reorder(names);
            continue;
        }

        if (line.rfind("node=", 0) != 0) continue;

        // node=<Name>;on=<0|1>;<param>=<value>;...
        std::vector<std::pair<std::string, std::string>> kvs;
        std::string part;
        std::istringstream split(line);
        while (std::getline(split, part, ';')) {
            size_t eq = part.find('=');
            if (eq != std::string::npos) {
                kvs.emplace_back(part.substr(0, eq), part.substr(eq + 1));
            }
        }
        if (kvs.empty() || kvs[0].first != "node") continue;

        DSPNode* target = nullptr;
        for (const auto& node : graph.getNodes()) {
            if (kvs[0].second == node->getName()) { target = node.get(); break; }
        }
        if (!target) continue;

        auto params = collectParams(target);
        for (size_t i = 1; i < kvs.size(); ++i) {
            if (kvs[i].first == "on") {
                target->isEnabled() = (kvs[i].second == "1");
                continue;
            }
            for (const auto& p : params) {
                if (kvs[i].first == p.name) {
                    *p.value = static_cast<float>(atof(kvs[i].second.c_str()));
                    break;
                }
            }
        }
    }
}

std::vector<PresetInfo> list() {
    std::vector<PresetInfo> out;
    for (const auto& [name, blob] : factoryPresets()) {
        out.push_back({name, true});
    }
    for (const auto& [name, body] : loadUserPresets()) {
        if (!isFactory(name)) out.push_back({name, false});
    }
    return out;
}

std::string get(const std::string& name) {
    for (const auto& [n, blob] : factoryPresets()) {
        if (n == name) return blob;
    }
    auto user = loadUserPresets();
    auto it = user.find(name);
    return it != user.end() ? it->second : "";
}

bool save(const std::string& name, const std::string& blob) {
    if (name.empty() || isFactory(name)) return false;
    auto user = loadUserPresets();
    user[name] = blob;
    return saveUserPresets(user);
}

void remove(const std::string& name) {
    if (isFactory(name)) return;
    auto user = loadUserPresets();
    if (user.erase(name)) saveUserPresets(user);
}

} // namespace DspPresets
