#include "protocol.hpp"

#include <cstdlib>
#include <cstring>

namespace proto {
namespace {

const char* next_token(const char*& p, char* buf, size_t cap) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) return nullptr;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (n + 1 < cap) buf[n++] = *p;
        ++p;
    }
    buf[n] = 0;
    return buf;
}

bool parse_slot(char c, disp::Slot& s) {
    switch (c) {
    case 'W': s = disp::Slot::Working; return true;
    case 'Y': s = disp::Slot::Waiting; return true;
    case 'I': s = disp::Slot::Idle; return true;
    case 'D': s = disp::Slot::Done; return true;
    case 'E': s = disp::Slot::Error; return true;
    default: return false;
    }
}

bool parse_colour(char c, disp::Colour& out) {
    using disp::Colour;
    switch (c) {
    case 'c': out = Colour::Cyan; return true;
    case 'y': out = Colour::Yellow; return true;
    case 'r': out = Colour::Red; return true;
    case 'g': out = Colour::Green; return true;
    case 'w': out = Colour::White; return true;
    case 'm': out = Colour::Magenta; return true;
    case 'o': out = Colour::Orange; return true;
    case 'b': out = Colour::Blue; return true;
    case 'x': out = Colour::Rainbow; return true;
    case 'k': out = Colour::Off; return true;
    default: return false;
    }
}

bool parse_panel(const char* tok, disp::PanelValue& pv) {
    // <value>:<unit>:<colour>
    const char* c1 = strchr(tok, ':');
    if (!c1) return false;
    const char* c2 = strchr(c1 + 1, ':');
    if (!c2 || c2 != c1 + 2 || c2[1] == 0 || c2[2] != 0) return false;

    if (tok[0] == '-' && c1 == tok + 1) {
        pv.value = disp::PanelValue::kDashes;
    } else if (tok[0] == '_' && c1 == tok + 1) {
        pv.value = disp::PanelValue::kBlank;
    } else {
        int v = 0;
        for (const char* p = tok; p < c1; ++p) {
            if (*p < '0' || *p > '9') return false;
            v = v * 10 + (*p - '0');
        }
        if (c1 == tok || v > 99) return false;
        pv.value = (int8_t)v;
    }
    switch (c1[1]) {
    case 'n': pv.unit = disp::Unit::None; break;
    case 's': pv.unit = disp::Unit::Sec; break;
    case 'm': pv.unit = disp::Unit::Min; break;
    case 'h': pv.unit = disp::Unit::Hour; break;
    default: return false;
    }
    return parse_colour(c2[1], pv.colour);
}

}  // namespace

bool parse_frame(const char* p, disp::Model& m) {
    char tok[64];
    disp::Model out{};

    if (!next_token(p, tok, sizeof tok)) return false;
    out.seq = (uint32_t)strtoul(tok, nullptr, 10);

    if (!next_token(p, tok, sizeof tok)) return false;
    if (tok[0] == 'A') out.starry = false;
    else if (tok[0] == 'S') out.starry = true;
    else return false;

    if (!next_token(p, tok, sizeof tok)) return false;
    if (strcmp(tok, "-") != 0) {
        for (const char* c = tok; *c; ++c) {
            if (*c == '+' && c[1] == 0) { out.overflow = true; break; }
            if (out.nslots >= disp::kMaxSlots) { out.overflow = true; break; }
            if (!parse_slot(*c, out.slots[out.nslots])) return false;
            ++out.nslots;
        }
    }

    for (int i = 0; i < disp::kPanels; ++i) {
        if (!next_token(p, tok, sizeof tok)) return false;
        if (!parse_panel(tok, out.panels[i])) return false;
    }
    m = out;
    return true;
}

}  // namespace proto
