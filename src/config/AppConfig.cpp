#include "AppConfig.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace fs = std::filesystem;

std::string AppConfig::configFilePath() {
    const char* base = std::getenv("APPDATA");
    fs::path dir = base ? fs::path(base) / "Antigravity" : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "config.json").string();
}

// ---------------------------------------------------------------------------
// JSON emit
// ---------------------------------------------------------------------------

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

bool AppConfig::save() const {
    std::ostringstream j;
    j << "{\n";
    j << "  \"inputDevice\": \""   << jsonEscape(inputDevice)   << "\",\n";
    j << "  \"outputDevice\": \""  << jsonEscape(outputDevice)  << "\",\n";
    j << "  \"monitorDevice\": \"" << jsonEscape(monitorDevice) << "\",\n";
    j << "  \"bufferMs\": "        << bufferMs << ",\n";
    j << "  \"exclusiveMode\": "   << (exclusiveMode ? "true" : "false") << ",\n";
    j << "  \"monitorEnabled\": "  << (monitorEnabled ? "true" : "false") << ",\n";
    j << "  \"monitorVolume\": "   << monitorVolume << ",\n";
    j << "  \"duckingEnabled\": "  << (duckingEnabled ? "true" : "false") << ",\n";
    j << "  \"duckingAmount\": "   << duckingAmount << ",\n";
    j << "  \"stopAllHotkey\": "   << stopAllHotkey << ",\n";
    j << "  \"clips\": [\n";
    for (size_t i = 0; i < clips.size(); ++i) {
        const ClipConfig& c = clips[i];
        j << "    {\"path\": \"" << jsonEscape(c.path) << "\", "
          << "\"hotkey\": " << c.hotkey << ", "
          << "\"volume\": " << c.volume << ", "
          << "\"loop\": " << (c.loop ? "true" : "false") << "}"
          << (i + 1 < clips.size() ? "," : "") << "\n";
    }
    j << "  ]\n";
    j << "}\n";

    std::ofstream f(configFilePath(), std::ios::trunc);
    if (!f) return false;
    f << j.str();
    return true;
}

// ---------------------------------------------------------------------------
// JSON read (tolerant scanner for the subset save() writes)
// ---------------------------------------------------------------------------

// Finds "key": and returns the raw token that follows (string contents
// unescaped, or the bare number/bool token). Empty string if absent.
static std::string extractValue(const std::string& json, const std::string& key,
                                size_t from = 0, size_t to = std::string::npos) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos || (to != std::string::npos && k >= to)) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return "";

    if (json[p] == '"') {
        // String: unescape until the closing quote
        std::string out;
        ++p;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) {
                char n = json[p + 1];
                if (n == 'n') out += '\n';
                else if (n == 'r') out += '\r';
                else if (n == 't') out += '\t';
                else out += n; // covers \\ and \" and anything else
                p += 2;
            } else {
                out += json[p++];
            }
        }
        return out;
    }

    // Number / bool: read until a delimiter
    std::string out;
    while (p < json.size() && json[p] != ',' && json[p] != '}' &&
           json[p] != ']' && json[p] != '\n') {
        out += json[p++];
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

static bool  toBool(const std::string& s, bool def)   { return s.empty() ? def : (s == "true"); }
static int   toInt(const std::string& s, int def)     { return s.empty() ? def : atoi(s.c_str()); }
static float toFloat(const std::string& s, float def) { return s.empty() ? def : static_cast<float>(atof(s.c_str())); }

bool AppConfig::load() {
    std::ifstream f(configFilePath());
    if (!f) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    std::string json = buf.str();
    if (json.empty()) return false;

    // The clips array is parsed separately, so scan scalars only before it.
    size_t clipsPos = json.find("\"clips\"");
    size_t scalarEnd = (clipsPos == std::string::npos) ? json.size() : clipsPos;

    inputDevice    = extractValue(json, "inputDevice",   0, scalarEnd);
    outputDevice   = extractValue(json, "outputDevice",  0, scalarEnd);
    monitorDevice  = extractValue(json, "monitorDevice", 0, scalarEnd);
    bufferMs       = toInt(extractValue(json, "bufferMs", 0, scalarEnd), bufferMs);
    exclusiveMode  = toBool(extractValue(json, "exclusiveMode", 0, scalarEnd), exclusiveMode);
    monitorEnabled = toBool(extractValue(json, "monitorEnabled", 0, scalarEnd), monitorEnabled);
    monitorVolume  = toFloat(extractValue(json, "monitorVolume", 0, scalarEnd), monitorVolume);
    duckingEnabled = toBool(extractValue(json, "duckingEnabled", 0, scalarEnd), duckingEnabled);
    duckingAmount  = toFloat(extractValue(json, "duckingAmount", 0, scalarEnd), duckingAmount);
    stopAllHotkey  = toInt(extractValue(json, "stopAllHotkey", 0, scalarEnd), stopAllHotkey);

    clips.clear();
    if (clipsPos != std::string::npos) {
        size_t p = json.find('[', clipsPos);
        size_t end = (p == std::string::npos) ? std::string::npos : json.find(']', p);
        while (p != std::string::npos && end != std::string::npos) {
            size_t objStart = json.find('{', p);
            if (objStart == std::string::npos || objStart > end) break;
            size_t objEnd = json.find('}', objStart);
            if (objEnd == std::string::npos || objEnd > end) break;

            ClipConfig c;
            c.path   = extractValue(json, "path", objStart, objEnd + 1);
            c.hotkey = toInt(extractValue(json, "hotkey", objStart, objEnd + 1), -1);
            c.volume = toFloat(extractValue(json, "volume", objStart, objEnd + 1), 0.8f);
            c.loop   = toBool(extractValue(json, "loop", objStart, objEnd + 1), false);
            if (!c.path.empty()) clips.push_back(c);

            p = objEnd + 1;
        }
    }
    return true;
}
