#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Platform-independent input arbitration and INI editing, also exercised by tests.
namespace mevr {
struct MenuInput {
    bool focused = true, y = false, accept = false, back = false, other = false;
    float x = 0, z = 0;
    bool neutral() const { return !y && !accept && !back && !other &&
        std::fabs(x) < .2f && std::fabs(z) < .2f; }
};
struct MenuEvents { bool open = false, close = false, shortY = false, accept = false, back = false; int row = 0, value = 0; };
struct MenuInputState {
    bool previousY = false, spentY = false, previousAccept = false, previousBack = false;
    bool waitNeutral = false;
    uint64_t yAt = 0, repeatAt = 0;
    int previousDirection = 0;
    void quarantine() { waitNeutral = true; previousDirection = 0; }
    MenuEvents sample(uint64_t now, const MenuInput& in, bool open) {
        MenuEvents out;
        if (!in.focused) {
            previousY = previousAccept = previousBack = false;
            spentY = false; yAt = 0; quarantine(); return out;
        }
        if (waitNeutral) {
            previousY = in.y; previousAccept = in.accept; previousBack = in.back;
            if (in.neutral()) { waitNeutral = false; previousY = false; spentY = false; }
            return out;
        }
        if (in.y && !previousY) { yAt = now; spentY = false; }
        if (in.y && !spentY && now - yAt >= 1000) {
            out.open = !open; out.close = open; spentY = true; quarantine();
        }
        if (!open) {
            if (!in.y && previousY && !spentY) out.shortY = true;
        } else if (!out.close) {
            out.accept = in.accept && !previousAccept;
            out.back = in.back && !previousBack;
            int direction = 0;
            if (std::fabs(in.z) > .65f) direction = in.z > 0 ? -1 : 1;
            else if (std::fabs(in.x) > .65f) direction = in.x > 0 ? 2 : -2;
            if (direction && (direction != previousDirection || now >= repeatAt)) {
                if (std::abs(direction) == 1) out.row = direction;
                else out.value = direction / 2;
                repeatAt = now + (direction != previousDirection ? 400 : 180);
            }
            previousDirection = direction;
        }
        previousY = in.y; previousAccept = in.accept; previousBack = in.back;
        return out;
    }
};
struct IniEdit { std::string key, value; };
inline std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}
inline bool equalKey(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i];
        const char cb = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
        if (ca != cb) return false;
    }
    return true;
}
inline std::string updateIni(const std::string& source, const std::vector<IniEdit>& edits) {
    std::string result;
    std::vector<bool> found(edits.size(), false);
    size_t at = 0;
    if (source.compare(0, 3, "\xEF\xBB\xBF") == 0) { result = source.substr(0, 3); at = 3; }
    const std::string newline = source.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    while (at < source.size()) {
        auto end = source.find('\n', at);
        if (end == std::string::npos) end = source.size();
        std::string line = source.substr(at, end - at);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        const auto eq = line.find('=');
        const auto stripped = trim(line);
        if (eq != std::string::npos && !stripped.empty() && stripped[0] != ';' && stripped[0] != '#') {
            const auto key = trim(line.substr(0, eq));
            for (size_t i = 0; i < edits.size(); ++i) if (equalKey(key, edits[i].key)) {
                const auto comment = line.find_first_of(";#", eq + 1);
                const std::string suffix = comment == std::string::npos ? "" : " " + line.substr(comment);
                line = line.substr(0, eq + 1) + " " + edits[i].value + suffix;
                found[i] = true; break; // update EVERY duplicate: the parser uses the last value
            }
        }
        result += line;
        if (cr) result += '\r';
        if (end < source.size()) result += '\n';
        at = end + 1;
    }
    for (size_t i = 0; i < edits.size(); ++i) if (!found[i]) {
        if (!result.empty() && result.back() != '\n') result += newline;
        result += edits[i].key + " = " + edits[i].value + newline;
    }
    return result;
}
}
