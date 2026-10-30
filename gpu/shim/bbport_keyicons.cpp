// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_keyicons.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <miniz.h>

// stb_truetype from Dear ImGui, private to this file (static functions).
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

// DejaVu Sans, embedded by bbport_overlay.cpp.
extern "C" const unsigned char bb_font_ttf[];

// The templates of the keys and mice (premultiplied RGBA8, gpu/shim/assets/bbport_key_assets.bin), embedded.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_key_assets\n"
    "bb_key_assets:\n"
    ".incbin \"" BB_KEYASSETS_PATH "\"\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_key_assets\n"
    ".global bb_key_assets\n"
    "bb_key_assets:\n"
    ".incbin \"" BB_KEYASSETS_PATH "\"\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_key_assets[];

// The Xbox pictures drawn by hand: the big "interact" button's three frames and the
// journal with its View button, with the journal's mask.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_xbox_assets\n"
    "bb_xbox_assets:\n"
    ".incbin \"" BB_XBOXASSETS_PATH "\"\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_xbox_assets\n"
    ".global bb_xbox_assets\n"
    "bb_xbox_assets:\n"
    ".incbin \"" BB_XBOXASSETS_PATH "\"\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_xbox_assets[];

namespace BbKeyIcons {

namespace {

struct Action {
    const char* name;
    const char* default_keys; // as in src/runtime_pad.c
};

// Same defaults as the input code reads when input.ini has no entry.
constexpr Action Actions[] = {
    {"cross", "Space"},       {"circle", "C"},          {"square", "F"},       {"triangle", "E"},
    {"l1", "G"},              {"r1", "R"},             {"l2", "Mouse Right"}, {"r2", "Mouse Left"},
    {"l3", "Left Shift"},     {"r3", "V"},              {"touchpad", "Tab"},   {"ls_up", "W"},
    {"ls_down", "S"},         {"ls_left", "A"},         {"ls_right", "D"},     {"rs_up", "I"},
    {"rs_down", "K"},         {"rs_left", "J"},         {"rs_right", "L"},     {"options", "Escape|Return"},
    {"up", "Up"},             {"down", "Down"},         {"left", "1|Wheel Up"},
    {"right", "2|Wheel Down"},
};
constexpr int ActionCount = sizeof(Actions) / sizeof(Actions[0]);

struct Bindings {
    std::string keys[ActionCount];
    bool mouse_look = true;
};

int ActionIndex(const std::string& name) {
    for (int i = 0; i < ActionCount; ++i) {
        if (name == Actions[i].name) {
            return i;
        }
    }
    return -1;
}

std::atomic<bool> g_menu_mode{false};
std::atomic<uint32_t> g_menu_generation{0};
std::atomic<bool> g_menu_alt{false}; // the screen shows an Alt prompt (resume / options): only then the triangle is painted as Alt, else it is the play key
std::atomic<int> g_input_mode{0}; // the device used last: 0 keyboard and mouse, 1 PlayStation controller, 2 Xbox controller (SetInputMode)
std::atomic<uint32_t> g_mode_generation{1}; // changes with the mode: the HUD buttons are uploaded again
std::atomic<int> g_menu_phase{0}; // 0: the keys (Enter, Esc, WASD), 1: the mouse (left button, side button, wheel); they alternate

/// The keys of the menus (src/runtime_pad.c, menu keys): they replace those of four buttons while a menu is open.
void ApplyMenuKeys(Bindings& b) {
    static const struct {
        const char* action;
        const char* keys;
    } menu_keys[] = {{"cross", "Return"},       {"circle", "Escape"}, {"triangle", "Left Alt"},
                  {"options", "Left Alt"}, {"r1", "Backspace"}};
    for (const auto& k : menu_keys) {
        if (!g_menu_alt && !std::strcmp(k.action, "triangle")) {
            continue; // a medal's text shows the triangle of its combo: the key of the play (E), not Alt
        }
        const int a = ActionIndex(k.action);
        if (a >= 0) {
            b.keys[a] = k.keys;
        }
    }
}

std::atomic<bool> g_jetski_mode{false}; // Elena is on the jetski (src/runtime_pad.c, u1_on_jetski)

/// On the jetski the keys of forward and back (the left stick's up and down keys) are the throttle and the brake, which
/// the game's hints show with the cross and square icons: they get those keys.
void ApplyJetskiKeys(Bindings& b) {
    const int cross = ActionIndex("cross"), square = ActionIndex("square");
    const int forward = ActionIndex("ls_up"), back = ActionIndex("ls_down");
    if (cross >= 0 && forward >= 0) {
        b.keys[cross] = b.keys[forward];
    }
    if (square >= 0 && back >= 0) {
        b.keys[square] = b.keys[back];
    }
}

Bindings LoadBindings() {
    Bindings b;
    for (int i = 0; i < ActionCount; ++i) {
        b.keys[i] = Actions[i].default_keys;
    }
    const char* path = std::getenv("BB_INPUT_CONFIG");
    FILE* file = path && path[0] ? std::fopen(path, "r") : nullptr;
    if (!file) {
        return b;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text{line};
        text.erase(text.find_last_not_of("\r\n") + 1);
        const auto eq = text.find('=');
        if (text.empty() || text[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string key = text.substr(0, eq);
        const std::string value = text.substr(eq + 1);
        if (key == "mouse_look") {
            b.mouse_look = std::atoi(value.c_str()) != 0;
        } else if (key.compare(0, 4, "key.") == 0) {
            const int a = ActionIndex(key.substr(4));
            if (a >= 0) {
                b.keys[a] = value;
            }
        }
    }
    std::fclose(file);
    return b;
}

/// Short text for a binding name as SDL and input.ini spell it; empty when unbound.
std::string ShortName(const std::string& binding) {
    std::string name = binding.substr(0, binding.find('|'));
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    static const struct {
        const char* from;
        const char* to;
    } names[] = {
        {"Mouse Left", "LMB"},   {"Mouse Right", "RMB"}, {"Mouse Middle", "MMB"},
        {"Mouse 4", "M4"},       {"Mouse 5", "M5"},      {"Wheel Up", "Wh.Up"},
        {"Wheel Down", "Wh.Dn"}, {"Left Ctrl", "Ctrl"},  {"Right Ctrl", "Ctrl"},
        {"Left Shift", "Shift"}, {"Right Shift", "Shift"}, {"Left Alt", "Alt"},
        {"Right Alt", "Alt"},    {"Escape", "Esc"},      {"Return", "Enter"},
        {"Backspace", "Bksp"},   {"CapsLock", "Caps"},   {"Delete", "Del"},
        {"Insert", "Ins"},       {"PageUp", "PgUp"},     {"PageDown", "PgDn"},
    };
    for (const auto& n : names) {
        if (name == n.from) {
            return n.to;
        }
    }
    // The numeric keypad: "Num X" (SDL spells these keys "Keypad X").
    if (name.compare(0, 7, "Keypad ") == 0 && name.size() > 7) {
        return "Num " + name.substr(7);
    }
    if (name.size() == 1) {
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    }
    return name;
}

struct Icon {
    const char* label; // action name, or "ls" / "rs" for the stick icons
    int x, y, w, h;    // bounding box in the 512x512 atlas
};

// Each icon exists twice in the atlas (two columns). Boxes measured on the U1 atlas.
constexpr Icon Icons[] = {
    {"circle", 10, 85, 44, 44},  {"square", 74, 85, 44, 44},   {"triangle", 138, 85, 44, 44},
    {"cross", 202, 85, 44, 44},  {"r2", 264, 6, 49, 48},       {"l2", 327, 6, 49, 48},
    {"r2", 8, 145, 49, 48},      {"l2", 71, 145, 49, 48},      {"r1", 352, 121, 53, 30},
    {"l1", 429, 112, 53, 30},    {"r1", 96, 260, 53, 30},      {"l1", 173, 251, 53, 30},
    {"l3", 273, 168, 42, 43},    {"r3", 347, 168, 42, 43},     {"l3", 17, 307, 42, 43},
    {"r3", 91, 307, 42, 43},     {"touchpad", 8, 257, 67, 42}, {"ls", 272, 65, 43, 42},
    {"rs", 346, 65, 43, 42},     {"ls", 16, 204, 43, 42},      {"rs", 90, 204, 43, 42},
    {"options", 406, 55, 60, 45}, {"options", 154, 197, 53, 41},
};

struct Font {
    stbtt_fontinfo info;
    bool ok = false;
};

Font& GetFont() {
    static Font font = [] {
        Font f;
        f.ok = stbtt_InitFont(&f.info, bb_font_ttf, stbtt_GetFontOffsetForIndex(bb_font_ttf, 0)) != 0;
        return f;
    }();
    return font;
}

/// The Unicode code points of a UTF-8 string.
std::vector<int> Codepoints(const std::string& text) {
    std::vector<int> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        int cp = extra == 0 ? c : c & (0x3F >> extra);
        for (int k = 1; k <= extra && i + size_t(k) < text.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + size_t(k)]) & 0x3F);
        }
        out.push_back(cp);
        i += size_t(extra) + 1;
    }
    return out;
}

float TextWidth(const Font& font, const std::string& text, float scale) {
    float width = 0.0f;
    for (const int cp : Codepoints(text)) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font.info, cp, &advance, &bearing);
        width += float(advance) * scale;
    }
    return width;
}

void Blend(uint8_t* px, int stride, int x, int y, const uint8_t color[3], float cover) {
    uint8_t* p = px + (size_t(y) * size_t(stride) + size_t(x)) * 4;
    for (int c = 0; c < 3; ++c) {
        p[c] = uint8_t(float(p[c]) + (float(color[c]) - float(p[c])) * cover + 0.5f);
    }
}

struct Label {
    float scale = 0.0f;
    float pen_x = 0.0f;
    int baseline = 0;
};

/// The biggest text (up to max_height pixels, down to min_height) that fits box_w, centered on
/// (center_x, center_y) by its capital letters.
Label LayoutLabel(const Font& font, const std::string& text, float center_x, float center_y,
                  float box_w, float box_h, float max_height, float min_height) {
    float pixel_height = std::min(box_h * 0.75f, max_height);
    float scale = stbtt_ScaleForPixelHeight(&font.info, pixel_height);
    while (pixel_height > min_height && TextWidth(font, text, scale) > box_w) {
        pixel_height -= 1.0f;
        scale = stbtt_ScaleForPixelHeight(&font.info, pixel_height);
    }
    int cap_x0, cap_y0, cap_x1, cap_y1;
    stbtt_GetCodepointBox(&font.info, 'H', &cap_x0, &cap_y0, &cap_x1, &cap_y1);
    Label label;
    label.scale = scale;
    label.pen_x = center_x - TextWidth(font, text, scale) * 0.5f;
    label.baseline = int(center_y + float(cap_y1) * scale * 0.5f + 0.5f);
    return label;
}

/// Calls put(x, y, cover) for every pixel of the text that has some coverage.
template <class Put>
void ForEachLabelPixel(const Font& font, const std::string& text, const Label& label, Put put) {
    float pen_x = label.pen_x;
    for (const int cp : Codepoints(text)) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font.info, cp, &advance, &bearing);
        int gx0, gy0, gx1, gy1;
        stbtt_GetCodepointBitmapBox(&font.info, cp, label.scale, label.scale, &gx0, &gy0, &gx1, &gy1);
        const int gw = gx1 - gx0, gh = gy1 - gy0;
        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> glyph(size_t(gw) * size_t(gh));
            stbtt_MakeCodepointBitmap(&font.info, glyph.data(), gw, gh, gw, label.scale, label.scale, cp);
            for (int gy = 0; gy < gh; ++gy) {
                for (int gx = 0; gx < gw; ++gx) {
                    const uint8_t v = glyph[size_t(gy) * size_t(gw) + size_t(gx)];
                    if (v) {
                        put(int(pen_x) + gx0 + gx, label.baseline + gy0 + gy, float(v) / 255.0f);
                    }
                }
            }
        }
        pen_x += float(advance) * label.scale;
    }
}

// ---- Key caps and mouse drawn into the atlas: keys and a mouse as they are IRL, in the worn black
// ---- and tan look of the original buttons, with their soft halo --------------------------------

struct Color {
    float r, g, b;
};

Color Lerp(const Color& a, const Color& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

/// Source-over of color c with alpha a on the straight-alpha RGBA8 pixel (x, y).
void Over(uint8_t* px, int size, int x, int y, const Color& c, float a) {
    if (a <= 0.0f || x < 0 || y < 0 || x >= size || y >= size) {
        return;
    }
    a = std::min(a, 1.0f);
    uint8_t* p = px + (size_t(y) * size_t(size) + size_t(x)) * 4;
    const float da = float(p[3]) / 255.0f;
    const float oa = a + da * (1.0f - a);
    const float k = da * (1.0f - a);
    const float in[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) {
        p[i] = uint8_t(std::clamp((in[i] * a + float(p[i]) * k) / oa, 0.0f, 255.0f) + 0.5f);
    }
    p[3] = uint8_t(oa * 255.0f + 0.5f);
}

/// Signed distance to a rounded rectangle centered on the origin (negative inside).
float RoundRect(float x, float y, float half_w, float half_h, float radius) {
    const float qx = std::fabs(x) - (half_w - radius), qy = std::fabs(y) - (half_h - radius);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
}

/// A fixed pseudo-random value in [0, 1) for a pixel, for the wear of the keys.
float Wear(int x, int y, int salt) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(salt) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65536.0f;
}

constexpr Color Cream{200.0f, 178.0f, 140.0f};    // the halo of most buttons, and their edge
constexpr Color TanEdge{168.0f, 144.0f, 108.0f};
constexpr Color Ink{255.0f, 255.0f, 252.0f};      // legends: white
constexpr Color Orange{252.0f, 170.0f, 90.0f};    // a lit key, as the lit arms of the d-pad

/// The halo of every key: orange, the color of the lit keys (the colors of the controller buttons
/// are dropped). The action is kept so the colors can come back per button.
Color Halo(const char* /*action*/) {
    return Orange;
}

// ---- Keys and mice: hand-drawn templates (gpu/shim/assets) with their legends in the game's own font ----------

// The templates: 3 keys (flat, pressed, released), canvas 512x512, then 7 mice, canvas 256x256 (premultiplied RGBA8).
enum Tpl {
    TplKeyFlat, TplKeyPressed, TplKeyReleased, TplMouseNeutral, TplMouseLeft, TplMouseRight, TplMouseWheel,
    TplMouseWheelClick, TplMouseThumbLow, TplMouseThumbHigh
};

// Measures in pixels of the .ai models (canvas 4167 x 4167): the rules of the legends are written in them.
constexpr float AiCanvas = 4167.0f;
constexpr float AiBody = 3916.0f;                       // side of the flat key, its black outline included
constexpr float AiStroke = 41.0f;                       // thickness of the black outline: the margins start inside it
constexpr float AiInner = AiBody - 2.0f * AiStroke;
constexpr float AiMarginSide = 602.0f;                  // least room left and right of a legend: the width of the scroll arrow
constexpr float AiMarginTop = 750.0f;                   // room above and under a single letter
constexpr float AiCenter = 2083.5f;                     // center of the canvas
constexpr float AiFaceCenterPressed = 1958.0f;          // center of the top of the pressed key (the face)
constexpr float AiFaceCenterReleased = 1833.0f;         // ... of the released one
constexpr float AiFaceHeight = 1943.0f;                 // height of that top, outline included (the same for both)
constexpr float FaceSquash = (AiFaceHeight - 2.0f * AiStroke) / AiInner; // the perspective: how much the legend is squashed
constexpr float MouseWidthRatio = 2178.0f / AiBody;     // width of the body of a mouse, in its height

const uint8_t* TemplateData(Tpl t, int* size) {
    constexpr size_t key = size_t(512) * 512 * 4, mouse = size_t(256) * 256 * 4;
    if (t < TplMouseNeutral) {
        *size = 512;
        return bb_key_assets + size_t(t) * key;
    }
    *size = 256;
    return bb_key_assets + 3 * key + size_t(t - TplMouseNeutral) * mouse;
}

/// Draws template t, its canvas `canvas` pixels wide, centered on (cx, cy). lit repaints the cap orange (the key an
/// animation is on).
void DrawTemplate(uint8_t* px, int size, Tpl t, float cx, float cy, float canvas, bool lit) {
    int ts = 0;
    const uint8_t* src = TemplateData(t, &ts);
    const float f = float(ts) / canvas; // source pixels per destination pixel
    const int n = std::clamp(int(std::ceil(f)), 1, 4);
    const float left = cx - canvas * 0.5f, top = cy - canvas * 0.5f;
    const int x0 = std::max(int(std::floor(left)), 0), x1 = std::min(int(std::ceil(left + canvas)), size);
    const int y0 = std::max(int(std::floor(top)), 0), y1 = std::min(int(std::ceil(top + canvas)), size);
    const auto texel = [&](int x, int y, int c) {
        x = std::clamp(x, 0, ts - 1);
        y = std::clamp(y, 0, ts - 1);
        return float(src[(size_t(y) * size_t(ts) + size_t(x)) * 4 + size_t(c)]);
    };
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < n; ++sy) {
                for (int sx = 0; sx < n; ++sx) {
                    const float u = (float(x) + (float(sx) + 0.5f) / float(n) - left) * f - 0.5f;
                    const float v = (float(y) + (float(sy) + 0.5f) / float(n) - top) * f - 0.5f;
                    const int iu = int(std::floor(u)), iv = int(std::floor(v));
                    const float tu = u - float(iu), tv = v - float(iv);
                    for (int c = 0; c < 4; ++c) {
                        acc[c] += texel(iu, iv, c) * (1 - tu) * (1 - tv) + texel(iu + 1, iv, c) * tu * (1 - tv) +
                                  texel(iu, iv + 1, c) * (1 - tu) * tv + texel(iu + 1, iv + 1, c) * tu * tv;
                    }
                }
            }
            const float inv = 1.0f / float(n * n);
            const float a = acc[3] * inv / 255.0f; // premultiplied: the colors are already times alpha
            if (a <= 0.002f) {
                continue;
            }
            Color c{acc[0] * inv / 255.0f / a * 255.0f, acc[1] * inv / 255.0f / a * 255.0f, acc[2] * inv / 255.0f / a * 255.0f};
            if (lit && a > 0.98f && c.r + c.g + c.b > 45.0f) {
                const float ty = std::clamp((float(y) + 0.5f - top) / canvas * AiCanvas / AiBody - 125.0f / AiBody, 0.0f, 1.0f);
                const float grain = std::clamp(0.9f + ((c.r + c.g + c.b) / 3.0f - 45.0f) / 200.0f, 0.8f, 1.1f);
                const Color o = Lerp(Orange, {226.0f, 120.0f, 46.0f}, ty);
                c = {std::min(o.r * grain, 255.0f), std::min(o.g * grain, 255.0f), std::min(o.b * grain, 255.0f)};
            }
            Over(px, size, x, y, c, a);
        }
    }
}

// ---- The game's font ("Adelon-Serial DB", art/general/textures/font/ps4/game3.fnt in u1data/data.psarc), read from the
// ---- user's dump when the first legend is drawn; DejaVu when the dump cannot be read --------------------------------

struct Mask {
    int w = 0, h = 0;
    std::vector<float> a; // coverage 0..1, cropped to the ink
};

struct GameFont {
    struct Glyph {
        int x, y, w, h, xo, yo, xa, chnl;
    };
    bool ok = false;
    int line = 0, aw = 0, ah = 0;
    std::map<int, Glyph> glyph;
    std::map<std::pair<int, int>, int> kern;
    std::vector<uint8_t> atlas; // BGRA
};

uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t Be40(const uint8_t* p) {
    return (uint64_t(p[0]) << 32) | uint64_t(Be32(p + 1));
}

/// One file of a PSARC archive (zlib), nullopt-like empty when absent. Throws nothing.
bool ReadPsarcFiles(const std::string& path, const std::vector<std::string>& wanted, std::vector<std::vector<uint8_t>>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    uint8_t head[32];
    in.read(reinterpret_cast<char*>(head), 32);
    if (in.gcount() != 32 || std::memcmp(head, "PSAR", 4) != 0) {
        return false;
    }
    const uint32_t toc_len = Be32(head + 12), entry_size = Be32(head + 16), entries = Be32(head + 20), block = Be32(head + 24);
    if (toc_len < 32 || entry_size < 30 || entries == 0 || block == 0 || toc_len > (64u << 20)) {
        return false;
    }
    std::vector<uint8_t> toc(toc_len - 32);
    in.read(reinterpret_cast<char*>(toc.data()), std::streamsize(toc.size()));
    if (size_t(in.gcount()) != toc.size() || toc.size() < size_t(entries) * entry_size) {
        return false;
    }
    const size_t width = block <= 0x10000 ? 2 : block <= 0x1000000 ? 3 : 4;
    const size_t table_at = size_t(entries) * entry_size;
    const auto block_size = [&](size_t i) {
        uint32_t v = 0;
        if (table_at + (i + 1) * width > toc.size()) {
            return v;
        }
        for (size_t k = 0; k < width; ++k) {
            v = (v << 8) | toc[table_at + i * width + k];
        }
        return v;
    };
    const auto read_entry = [&](uint32_t index, std::vector<uint8_t>& data) {
        const uint8_t* e = &toc[size_t(index) * entry_size];
        size_t b = Be32(e + 16);
        const uint64_t total = Be40(e + 20), offset = Be40(e + 25);
        if (total > (256u << 20)) {
            return false;
        }
        in.clear();
        in.seekg(std::streamoff(offset));
        data.clear();
        std::vector<uint8_t> raw(block), plain(block);
        while (data.size() < total) {
            const uint32_t cs = block_size(b++);
            const size_t want = cs == 0 ? block : cs;
            if (want > block) {
                return false;
            }
            in.read(reinterpret_cast<char*>(raw.data()), std::streamsize(want));
            if (size_t(in.gcount()) != want) {
                return false;
            }
            mz_ulong plain_len = mz_ulong(block);
            if (cs != 0 && cs < block && mz_uncompress(plain.data(), &plain_len, raw.data(), mz_ulong(cs)) == MZ_OK) {
                data.insert(data.end(), plain.begin(), plain.begin() + std::ptrdiff_t(plain_len));
            } else {
                data.insert(data.end(), raw.begin(), raw.begin() + std::ptrdiff_t(want));
            }
        }
        data.resize(size_t(total));
        return true;
    };
    std::vector<uint8_t> manifest;
    if (!read_entry(0, manifest)) {
        return false;
    }
    const std::string names(manifest.begin(), manifest.end());
    out.assign(wanted.size(), {});
    size_t start = 0;
    uint32_t index = 1;
    bool all = true;
    while (start <= names.size() && index < entries) {
        size_t end = names.find('\n', start);
        if (end == std::string::npos) {
            end = names.size();
        }
        const std::string name = names.substr(start, end - start);
        for (size_t w = 0; w < wanted.size(); ++w) {
            if (name.size() >= wanted[w].size() && name.compare(name.size() - wanted[w].size(), wanted[w].size(), wanted[w]) == 0) {
                if (!read_entry(index, out[w])) {
                    return false;
                }
            }
        }
        ++index;
        start = end + 1;
    }
    for (const auto& d : out) {
        all = all && !d.empty();
    }
    return all;
}

const GameFont& GetGameFont() {
    static const GameFont font = [] {
        GameFont f;
        std::vector<std::string> bases;
        for (const char* name : {"BB_GAME_DIR_U1", "BB_GAME_DIR"}) {
            if (const char* v = std::getenv(name); v && v[0]) {
                bases.push_back(v);
            }
        }
        std::vector<std::vector<uint8_t>> files;
        std::string found;
        for (const std::string& base : bases) {
            for (const char* sub : {"/u1data/data.psarc", "/data.psarc"}) {
                if (ReadPsarcFiles(base + sub, {"/game3.fnt", "/game3_00.tga"}, files)) {
                    found = base + sub;
                    break;
                }
            }
            if (!found.empty()) {
                break;
            }
        }
        if (found.empty()) {
            std::printf("KeyIcons: the game's font (game3.fnt) was not found in the dump, DejaVu is used for the legends\n");
            return f;
        }
        const std::vector<uint8_t>& tga = files[1];
        if (tga.size() < 18 || tga[2] != 2 || tga[16] != 32) {
            std::printf("KeyIcons: unexpected format of game3_00.tga, DejaVu is used for the legends\n");
            return f;
        }
        f.aw = tga[12] | (tga[13] << 8);
        f.ah = tga[14] | (tga[15] << 8);
        const size_t pixels = size_t(f.aw) * size_t(f.ah);
        if (f.aw <= 0 || f.ah <= 0 || tga.size() < 18 + size_t(tga[0]) + pixels * 4) {
            return f;
        }
        f.atlas.resize(pixels * 4);
        const uint8_t* data = &tga[18 + size_t(tga[0])];
        const bool top_left = (tga[17] & 0x20) != 0;
        for (int y = 0; y < f.ah; ++y) {
            std::memcpy(&f.atlas[size_t(y) * size_t(f.aw) * 4], data + size_t(top_left ? y : f.ah - 1 - y) * size_t(f.aw) * 4,
                        size_t(f.aw) * 4);
        }
        const std::string text(files[0].begin(), files[0].end());
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            int id, x, y, w, h, xo, yo, xa, page, chnl, first, second, amount, lh;
            if (std::sscanf(line.c_str(), "char id=%d x=%d y=%d width=%d height=%d xoffset=%d yoffset=%d xadvance=%d page=%d chnl=%d",
                            &id, &x, &y, &w, &h, &xo, &yo, &xa, &page, &chnl) == 10) {
                f.glyph[id] = {x, y, w, h, xo, yo, xa, chnl};
            } else if (std::sscanf(line.c_str(), "kerning first=%d second=%d amount=%d", &first, &second, &amount) == 3) {
                f.kern[{first, second}] = amount;
            } else if (std::sscanf(line.c_str(), "common lineHeight=%d", &lh) == 1) {
                f.line = lh;
            }
        }
        f.ok = f.line > 0 && f.glyph.count('E') && f.glyph.count('?');
        if (f.ok) {
            std::printf("KeyIcons: legends in the game's font (%s, %zu glyphs)\n", found.c_str(), f.glyph.size());
        }
        return f;
    }();
    return font;
}

/// Whether the font the legends use has this code point (the arrows of the arrow keys are not in the game's font).
bool LegendHasGlyph(int cp) {
    const GameFont& g = GetGameFont();
    if (g.ok) {
        return g.glyph.count(cp) != 0;
    }
    return stbtt_FindGlyphIndex(&GetFont().info, cp) != 0;
}

/// The ink of a text as a mask at the font's native size, cropped; empty when nothing is drawn.
Mask BuildMask(const std::string& text) {
    Mask mask;
    const GameFont& g = GetGameFont();
    const std::vector<int> cps = Codepoints(text);
    std::vector<float> canvas;
    int cw = 0, ch = 0;
    if (g.ok) {
        int pen = 12, prev = -1;
        struct Item {
            int x, y;
            const GameFont::Glyph* glyph;
        };
        std::vector<Item> items;
        for (int cp : cps) {
            auto it = g.glyph.find(cp);
            if (it == g.glyph.end()) {
                it = g.glyph.find('?');
            }
            if (prev >= 0) {
                const auto k = g.kern.find({prev, it->first});
                pen += k == g.kern.end() ? 0 : k->second;
            }
            items.push_back({pen + it->second.xo, it->second.yo + 8, &it->second});
            pen += it->second.xa;
            prev = it->first;
        }
        cw = pen + 24;
        ch = g.line + 24;
        canvas.assign(size_t(cw) * size_t(ch), 0.0f);
        for (const Item& item : items) {
            const GameFont::Glyph& gl = *item.glyph;
            const int channel = gl.chnl == 1 ? 0 : gl.chnl == 2 ? 1 : gl.chnl == 4 ? 2 : 3; // BGRA
            for (int j = 0; j < gl.h; ++j) {
                for (int i = 0; i < gl.w; ++i) {
                    const int dx = item.x + i, dy = item.y + j, sx = gl.x + i, sy = gl.y + j;
                    if (dx < 0 || dy < 0 || dx >= cw || dy >= ch || sx >= g.aw || sy >= g.ah) {
                        continue;
                    }
                    float& d = canvas[size_t(dy) * size_t(cw) + size_t(dx)];
                    d = std::max(d, float(g.atlas[(size_t(sy) * size_t(g.aw) + size_t(sx)) * 4 + size_t(channel)]) / 255.0f);
                }
            }
        }
    } else {
        const Font& font = GetFont();
        if (!font.ok) {
            return mask;
        }
        const float scale = stbtt_ScaleForPixelHeight(&font.info, 48.0f);
        cw = int(TextWidth(font, text, scale)) + 48;
        ch = 96;
        canvas.assign(size_t(cw) * size_t(ch), 0.0f);
        Label label;
        label.scale = scale;
        label.pen_x = 12.0f;
        label.baseline = 64;
        ForEachLabelPixel(font, text, label, [&](int x, int y, float cover) {
            if (x >= 0 && y >= 0 && x < cw && y < ch) {
                canvas[size_t(y) * size_t(cw) + size_t(x)] = std::max(canvas[size_t(y) * size_t(cw) + size_t(x)], cover);
            }
        });
    }
    int x0 = cw, y0 = ch, x1 = -1, y1 = -1;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            if (canvas[size_t(y) * size_t(cw) + size_t(x)] > 0.08f) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < x0 || y1 < y0) {
        return mask;
    }
    mask.w = x1 - x0 + 1;
    mask.h = y1 - y0 + 1;
    mask.a.resize(size_t(mask.w) * size_t(mask.h));
    for (int y = 0; y < mask.h; ++y) {
        std::memcpy(&mask.a[size_t(y) * size_t(mask.w)], &canvas[size_t(y0 + y) * size_t(cw) + size_t(x0)], size_t(mask.w) * sizeof(float));
    }
    return mask;
}

Mask GetMask(const std::string& text) {
    static std::mutex mutex;
    static std::map<std::string, Mask> cache;
    std::lock_guard<std::mutex> lock{mutex};
    auto it = cache.find(text);
    if (it == cache.end()) {
        it = cache.emplace(text, BuildMask(text)).first;
    }
    return it->second;
}

constexpr Color LegendInk{245.0f, 240.0f, 224.0f};
constexpr Color LegendInkLit{40.0f, 24.0f, 10.0f};

/// The legend of a key whose body (outline included) is `body` pixels wide, on a face centered on (cx, cy); squash < 1
/// is the perspective of the pressed and released keys. The rules, in the measures of the .ai model scaled to the key:
/// several characters fill the width keeping 602 px from the inside of the outline on each side, without getting taller
/// than a single character; a single character leaves 750 px above and under it.
void DrawLegend(uint8_t* px, int size, float cx, float cy, float body, const std::string& label, float squash, bool lit,
                float body_w = 0.0f) {
    const Mask mask = GetMask(label);
    if (mask.w <= 0 || mask.h <= 0) {
        return;
    }
    // body is the height of the key; body_w its width when it is wider than tall (the journal's key), else a square.
    const float u = body / AiBody;
    const float plate = AiInner * u, side = AiMarginSide * u, top = AiMarginTop * u;
    const float plate_w = body_w > 0.0f ? body_w - 2.0f * AiStroke * u : plate;
    float sc;
    if (Codepoints(label).size() == 1) {
        sc = (plate - 2.0f * top) / float(mask.h);
        if (float(mask.w) * sc > plate_w - 2.0f * side) {
            sc = (plate_w - 2.0f * side) / float(mask.w);
        }
    } else {
        sc = std::min((plate_w - 2.0f * side) / float(mask.w), (plate - 2.0f * top) / float(mask.h));
    }
    const float scy = sc * squash;
    const float dw = float(mask.w) * sc, dh = float(mask.h) * scy;
    const float left = cx - dw * 0.5f, upper = cy - dh * 0.5f;
    const float e = sc * std::min(1.0f, squash); // below 1 the mask is reduced: averaged; above, enlarged: edges sharpened
    const float band = std::clamp(0.5f / std::max(e, 1.0f), 0.06f, 0.5f);
    const int nx = std::clamp(int(std::ceil(1.0f / sc)), 1, 6), ny = std::clamp(int(std::ceil(1.0f / scy)), 1, 6);
    const auto at = [&](float mu, float mv) {
        const float fu = mu - 0.5f, fv = mv - 0.5f;
        const int x0 = int(std::floor(fu)), y0 = int(std::floor(fv));
        const float tx = fu - float(x0), ty = fv - float(y0);
        float s = 0.0f;
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int x = x0 + i, y = y0 + j;
                if (x >= 0 && y >= 0 && x < mask.w && y < mask.h) {
                    s += mask.a[size_t(y) * size_t(mask.w) + size_t(x)] * (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
                }
            }
        }
        return s;
    };
    const Color ink = lit ? LegendInkLit : LegendInk;
    for (int y = int(std::floor(upper)) - 1; y < int(std::ceil(upper + dh)) + 1; ++y) {
        for (int x = int(std::floor(left)) - 1; x < int(std::ceil(left + dw)) + 1; ++x) {
            float cover = 0.0f;
            for (int sy = 0; sy < ny; ++sy) {
                for (int sx = 0; sx < nx; ++sx) {
                    const float mu = (float(x) + (float(sx) + 0.5f) / float(nx) - left) / sc;
                    const float mv = (float(y) + (float(sy) + 0.5f) / float(ny) - upper) / scy;
                    float v = at(mu, mv);
                    if (e > 1.0f) {
                        const float t = std::clamp((v - (0.5f - band)) / (2.0f * band), 0.0f, 1.0f);
                        v = t * t * (3.0f - 2.0f * t);
                    }
                    cover += v;
                }
            }
            Over(px, size, x, y, ink, cover / float(nx * ny));
        }
    }
}

constexpr float KeyHalfHeight = 19.0f; // half height of every key at the scale of the 44 pixel atlas icons
constexpr float LegendHeight = 16.0f;  // (kept for the settings that scale legends: the rules above now size them)
float g_legend_boost = 1.0f;           // the menu pastilles are drawn small by the game (set while the atlas is painted)

enum class KeyState { Flat, Pressed, Released };

/// A key (flat, pressed or released) with its legend: body = its width, outline included; (cx, cy) the center of the body.
void DrawKeyState(uint8_t* px, int size, float cx, float cy, float body, const std::string& label, KeyState state, bool lit) {
    const float u = body / AiBody;
    const Tpl tpl = state == KeyState::Flat ? TplKeyFlat : state == KeyState::Pressed ? TplKeyPressed : TplKeyReleased;
    DrawTemplate(px, size, tpl, cx, cy, AiCanvas * u, lit);
    const float face = state == KeyState::Flat ? 0.0f
                       : (state == KeyState::Pressed ? AiFaceCenterPressed : AiFaceCenterReleased) - AiCenter;
    DrawLegend(px, size, cx, cy + face * u, body, label, state == KeyState::Flat ? 1.0f : FaceSquash, lit);
}

/// A key wider than tall (the "Tab" of the journal, which fills the box of the old button): the flat key's template with
/// its middle stretched sideways, the two ends (corners, outline, glow) kept as they are. body_w x body_h is the body,
/// outline included, centered on (cx, cy).
void DrawWideKey(uint8_t* px, int size, float cx, float cy, float body_w, float body_h, const std::string& label) {
    int ts = 0;
    const uint8_t* src = TemplateData(TplKeyFlat, &ts);
    const float u = body_h / AiBody;
    const float cw = body_w + (AiCanvas - AiBody) * u, ch = AiCanvas * u;
    const float left = cx - cw * 0.5f, top = cy - ch * 0.5f;
    constexpr float SplitL = 1000.0f, SplitR = AiCanvas - 1000.0f; // the ends: 1000 px of the .ai model on each side
    const float wl = SplitL * u, wr = (AiCanvas - SplitR) * u;
    const auto source_x = [&](float d) { // .ai x for the offset d from the left of the canvas
        if (d < wl) {
            return d / u;
        }
        if (d > cw - wr) {
            return AiCanvas - (cw - d) / u;
        }
        return SplitL + (d - wl) / (cw - wl - wr) * (SplitR - SplitL);
    };
    const auto texel = [&](int x, int y, int c) {
        x = std::clamp(x, 0, ts - 1);
        y = std::clamp(y, 0, ts - 1);
        return float(src[(size_t(y) * size_t(ts) + size_t(x)) * 4 + size_t(c)]);
    };
    constexpr int n = 3;
    const float to_src = float(ts) / AiCanvas;
    for (int y = std::max(int(std::floor(top)), 0); y < std::min(int(std::ceil(top + ch)), size); ++y) {
        for (int x = std::max(int(std::floor(left)), 0); x < std::min(int(std::ceil(left + cw)), size); ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < n; ++sy) {
                for (int sx = 0; sx < n; ++sx) {
                    const float v = (float(y) + (float(sy) + 0.5f) / float(n) - top) / u * to_src - 0.5f;
                    const float uu = source_x(float(x) + (float(sx) + 0.5f) / float(n) - left) * to_src - 0.5f;
                    const int iu = int(std::floor(uu)), iv = int(std::floor(v));
                    const float tu = uu - float(iu), tv = v - float(iv);
                    for (int c = 0; c < 4; ++c) {
                        acc[c] += texel(iu, iv, c) * (1 - tu) * (1 - tv) + texel(iu + 1, iv, c) * tu * (1 - tv) +
                                  texel(iu, iv + 1, c) * (1 - tu) * tv + texel(iu + 1, iv + 1, c) * tu * tv;
                    }
                }
            }
            const float a = acc[3] / float(n * n) / 255.0f;
            if (a > 0.002f) {
                const float k = 1.0f / float(n * n) / a;
                Over(px, size, x, y, {acc[0] * k, acc[1] * k, acc[2] * k}, a);
            }
        }
    }
    DrawLegend(px, size, cx, cy, body_h, label, 1.0f, false, body_w);
}

/// A square key of half side min(hw, hh) centered on (cx, cy). lit paints the cap orange.
void DrawKeyCap(uint8_t* px, int size, float cx, float cy, float hw, float hh, const std::string& label,
                const Font& /*font*/, const Color& /*halo*/, bool /*small*/, float /*k*/ = 1.0f, float /*depth*/ = 0.0f,
                bool lit = false) {
    DrawKeyState(px, size, cx, cy, 2.0f * std::min(hw, hh), label, KeyState::Flat, lit);
}

enum class Mouse { None, Left, Right, Wheel, WheelClick, Side4, Side5 };

/// A mouse seen from above, body_h tall; the button the label names is lit in orange (None: no button lit).
void DrawMouse(uint8_t* px, int size, float cx, float cy, float body_h, Mouse lit) {
    static const Tpl tpl[] = {TplMouseNeutral, TplMouseLeft, TplMouseRight, TplMouseWheel,
                              TplMouseWheelClick, TplMouseThumbLow, TplMouseThumbHigh};
    DrawTemplate(px, size, tpl[int(lit)], cx, cy, AiCanvas * body_h / AiBody, false);
}

/// The mouse part a binding label names; false for a keyboard key.
bool MouseLabel(const std::string& label, Mouse& lit) {
    static const struct {
        const char* label;
        Mouse lit;
    } names[] = {{"LMB", Mouse::Left},   {"RMB", Mouse::Right}, {"MMB", Mouse::WheelClick}, {"Wh.Up", Mouse::Wheel},
                 {"Wh.Dn", Mouse::Wheel}, {"M4", Mouse::Side4},  {"M5", Mouse::Side5},       {"Mouse", Mouse::None}};
    for (const auto& n : names) {
        if (label == n.label) {
            lit = n.lit;
            return true;
        }
    }
    return false;
}

/// The key (or the mouse) a label names, drawn to fill a w x h box centered on (cx, cy); the
/// limits grow with boxes bigger than the 44 pixels of the atlas icons. Keys are square.
void DrawSymbol(uint8_t* px, int size, float cx, float cy, float w, float h, const std::string& label,
                const Font& font, const Color& halo, float depth = 0.0f, bool fill = false) {
    const float k = std::max(std::min(w, h) / 44.0f, 1.0f);
    Mouse lit = Mouse::None;
    if (MouseLabel(label, lit)) {
        DrawMouse(px, size, cx, cy, 2.0f * std::min(h * 0.5f - 3.0f, KeyHalfHeight * k), lit);
        return;
    }
    if (fill) { // the journal: the key fills the box of the old button, wide as it is
        DrawWideKey(px, size, cx, cy, w - 6.0f, h - 6.0f, label);
        return;
    }
    const float hh = std::min(std::min(w, h) * 0.5f - 1.5f, KeyHalfHeight * k);
    DrawKeyCap(px, size, cx, cy, hh, hh, label, font, halo, false, k, depth);
}

/// Four keys in their layout (up above down, left and right beside it); labels are up, left, down,
/// right. key is the size of one key, lit the index of the one that is lit, -1 for none.
void DrawCluster(uint8_t* px, int size, float cx, float cy, const std::string labels[4], const Font& font,
                 float key, int lit, float k, float gap_ratio = 0.15f) {
    const float gap = key * gap_ratio, half = key * 0.5f;
    const float row1 = cy - (key + gap) * 0.5f, row2 = cy + (key + gap) * 0.5f;
    const float col[3] = {cx - key - gap, cx, cx + key + gap};
    const struct {
        float x, y;
    } at[4] = {{col[1], row1}, {col[0], row2}, {col[1], row2}, {col[2], row2}};
    for (int i = 0; i < 4; ++i) {
        DrawKeyCap(px, size, at[i].x, at[i].y, half, half, labels[i], font, Orange, key < 20.0f, k, 0.0f, i == lit);
    }
}

constexpr float AtlasScale = 1.14f;      // atlas keys vs the 44 pixel box, to match the other prompts on screen
constexpr float ShoulderDrop = -1.5f;    // atlas pixels (up). The quad of the L1/R1 icons stops about 9 pixels above their box: a key 21.7 pixels tall that rises by more than ~1.8 is cut at the top (-5.5 was)
constexpr float AtlasClusterKey = 18.0f; // one key of the WASD in the text of the tutorials (was 13)

/// Redraws one icon box of the atlas as the key (or mouse, or key group) the label names.
bool g_shoulders_cleared = false; // the L1/R1 boxes were cleared before the icons are drawn (PatchU1Atlas)

/// Empties the L1/R1 icons and their wide margin (the original glow), only where the icon is: done before anything is
/// drawn, so that the margin cannot cut the keys of the icons around them.
void ClearShoulders(uint8_t* px, int size) {
    for (const Icon& icon : Icons) {
        if (std::strcmp(icon.label, "l1") && std::strcmp(icon.label, "r1")) {
            continue;
        }
        bool present = false;
        for (int y = icon.y; y < icon.y + icon.h && !present; ++y) {
            for (int x = icon.x; x < icon.x + icon.w && !present; ++x) {
                present = px[(size_t(y) * size_t(size) + size_t(x)) * 4 + 3] > 140;
            }
        }
        if (!present) {
            continue;
        }
        const int x0 = std::max(icon.x - 14, 0), x1 = std::min(icon.x + icon.w + 14, size);
        for (int y = std::max(icon.y - 14, 0); y < std::min(icon.y + icon.h + 14, size); ++y) {
            std::memset(&px[(size_t(y) * size_t(size) + size_t(x0)) * 4], 0, size_t(x1 - x0) * 4);
        }
    }
}

bool DrawIcon(uint8_t* px, int size, const Icon& icon, const std::string& label, const Font& font) {
    // The shoulder icons (L1, R1) are 30 pixels tall and the game sits them a little higher than the
    // 44 pixel ones: their key is drawn higher, by ShoulderDrop, and more is cleared around it.
    const bool shoulder = !std::strcmp(icon.label, "l1") || !std::strcmp(icon.label, "r1");
    const int Clear = shoulder ? 14 : 8; // the original glow reaches well past the box
    const int x0 = std::max(icon.x - Clear, 0), y0 = std::max(icon.y - Clear, 0);
    const int x1 = std::min(icon.x + icon.w + Clear, size), y1 = std::min(icon.y + icon.h + Clear, size);
    // The icon has to be there: the boxes are measured on one atlas. (The shoulder icons were cleared beforehand, see
    // ClearShoulders: the others drawn first must not be cut by their wide margin.)
    if (!(shoulder && g_shoulders_cleared)) {
        bool present = false;
        for (int y = icon.y; y < icon.y + icon.h && !present; ++y) {
            for (int x = icon.x; x < icon.x + icon.w && !present; ++x) {
                present = px[(size_t(y) * size_t(size) + size_t(x)) * 4 + 3] > 140;
            }
        }
        if (!present) {
            return false;
        }
        for (int y = y0; y < y1; ++y) {
            std::memset(&px[(size_t(y) * size_t(size) + size_t(x0)) * 4], 0, size_t(x1 - x0) * 4);
        }
    }

    const float cx = float(icon.x) + float(icon.w) * 0.5f, cy = float(icon.y) + float(icon.h) * 0.5f;
    if (label.size() == 4 && (!std::strcmp(icon.label, "ls") || !std::strcmp(icon.label, "rs"))) {
        const std::string keys[4] = {label.substr(0, 1), label.substr(1, 1), label.substr(2, 1), label.substr(3, 1)};
        DrawCluster(px, size, cx, cy, keys, font, AtlasClusterKey, -1, 1.0f);
        return true;
    }

    // The keys may pass the box of the icon a little (the original glow reaches well past it, and is
    // cleared above): the short icons (L1, R1: 30 pixels) get keys as tall as the others.
    DrawSymbol(px, size, cx, cy + (shoulder ? ShoulderDrop : 0.0f), float(icon.w) * AtlasScale,
               std::max(float(icon.h), 44.0f) * AtlasScale, label, font, Halo(icon.label));
    return true;
}

/// The menu pastilles: the key and the mouse button side by side in the box of one icon (and the margin the
/// original glow leaves around it), the key on the left.
bool DrawIconPair(uint8_t* px, int size, const Icon& icon, const std::string& key, const std::string& mouse, const Font& font) {
    const int Clear = 10; // the cell of an icon in the atlas is 64 wide (pitch 64, box 44): this is all of it
    const int x0 = std::max(icon.x - Clear, 0), y0 = std::max(icon.y - Clear, 0);
    const int x1 = std::min(icon.x + icon.w + Clear, size), y1 = std::min(icon.y + icon.h + Clear, size);
    bool present = false;
    for (int y = icon.y; y < icon.y + icon.h && !present; ++y) {
        for (int x = icon.x; x < icon.x + icon.w && !present; ++x) {
            present = px[(size_t(y) * size_t(size) + size_t(x)) * 4 + 3] > 140;
        }
    }
    if (!present) {
        return false;
    }
    for (int y = y0; y < y1; ++y) {
        std::memset(&px[(size_t(y) * size_t(size) + size_t(x0)) * 4], 0, size_t(x1 - x0) * 4);
    }
    const float cx = float(icon.x) + float(icon.w) * 0.5f, cy = float(icon.y) + float(icon.h) * 0.5f;
    // Measured in game (calibration grid): the 60x60 region (box plus margin) is shown whole and square, 0.75 on screen,
    // so the keys can use all of it. A key of the normal size (38 tall) leaves no room for a second picture: a smaller
    // cap with a small legend, and a mouse beside it.
    // Measured on a capture: the Alt key (KeyHalfHeight, 38 atlas pixels tall) is 32 pixels tall on a 2000 wide screen
    // and the same height keeps the pair as tall: a 36 wide key and a 23 wide mouse, 2.5 apart, fill the 64 pixel cell.
    // The legend is the part that has to be read: the key takes 44 of the 64 pixels, the mouse 18 (a slimmer mouse).
    // With the templates the keys are square: a 36 pixel key and a 38 pixel tall mouse (21 wide), 1 pixel apart.
    constexpr float PairKey = 36.0f, PairMouse = 38.0f;
    const float mouse_w = PairMouse * MouseWidthRatio, total = PairKey + mouse_w + 1.0f;
    DrawKeyCap(px, size, cx - total * 0.5f + PairKey * 0.5f, cy, PairKey * 0.5f, PairKey * 0.5f, key, font, Halo(icon.label), false);
    Mouse lit = Mouse::None;
    MouseLabel(mouse, lit);
    DrawMouse(px, size, cx + total * 0.5f - mouse_w * 0.5f, cy, PairMouse, lit);
    return true;
}

/// TEMPORARY calibration of the menu pastilles: how the game stretches the atlas region of an icon. A grid on the
/// 44x44 box and its 8 pixel margin: white edge of the cleared region, red edge of the box, a line every 10 atlas
/// pixels (green columns, cyan rows, the 0 and 40 lines in white), a yellow disc of radius 15 at the centre and a
/// magenta square in the top left corner (which way is up).
bool DrawCalibration(uint8_t* px, int size, const Icon& icon) {
    const int Clear = 8;
    const int x0 = std::max(icon.x - Clear, 0), y0 = std::max(icon.y - Clear, 0);
    const int x1 = std::min(icon.x + icon.w + Clear, size), y1 = std::min(icon.y + icon.h + Clear, size);
    for (int y = y0; y < y1; ++y) {
        std::memset(&px[(size_t(y) * size_t(size) + size_t(x0)) * 4], 0, size_t(x1 - x0) * 4);
    }
    const auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < x0 || x >= x1 || y < y0 || y >= y1) {
            return;
        }
        uint8_t* p = &px[(size_t(y) * size_t(size) + size_t(x)) * 4];
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    const float cx = float(icon.x) + float(icon.w) * 0.5f, cy = float(icon.y) + float(icon.h) * 0.5f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const int lx = x - icon.x, ly = y - icon.y; // 0..43 inside the box
            const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
            if (dx * dx + dy * dy < 15.0f * 15.0f) {
                set(x, y, 255, 220, 40);
            }
            if (lx % 10 == 0) {
                set(x, y, lx == 0 || lx == 40 ? 255 : 60, 255, lx == 0 || lx == 40 ? 255 : 60);
            }
            if (ly % 10 == 0) {
                set(x, y, ly == 0 || ly == 40 ? 255 : 60, 255, 255);
            }
            if (x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1) {
                set(x, y, 255, 255, 255);
            }
            if (lx == 0 || lx == icon.w - 1 || ly == 0 || ly == icon.h - 1) {
                set(x, y, 255, 30, 30);
            }
            if (lx >= -6 && lx < 0 && ly >= -6 && ly < 0) {
                set(x, y, 255, 0, 255);
            }
        }
    }
    return true;
}

/// Arrow keys as arrows (↑ ↓ ← →) when the font has them.
std::string ArrowLabel(const Font& /*font*/, const std::string& label) {
    static const struct {
        const char* name;
        const char* arrow;
        int cp;
    } arrows[] = {{"Up", "\xE2\x86\x91", 0x2191}, {"Down", "\xE2\x86\x93", 0x2193},
                  {"Left", "\xE2\x86\x90", 0x2190}, {"Right", "\xE2\x86\x92", 0x2192}};
    for (const auto& a : arrows) {
        if (label == a.name && LegendHasGlyph(a.cp)) {
            return a.arrow;
        }
    }
    return label;
}

/// One d-pad frame of the atlas (the neutral pad, or with arms lit in orange): each lit arm gets
/// the key of its direction.
bool LabelDpadFrame(uint8_t* px, int size, const Icon& frame, const Bindings& b, const Font& font) {
    static const uint8_t DarkInk[3] = {44, 26, 12};
    const auto lit = [&](int x, int y) {
        const uint8_t* p = &px[(size_t(y) * size_t(size) + size_t(x)) * 4];
        return p[3] > 128 && p[0] > 200 && p[1] > 90 && p[1] < 190 && p[2] < 120;
    };
    const int x0 = frame.x, y0 = frame.y, x1 = frame.x + frame.w, y1 = frame.y + frame.h;
    std::vector<uint8_t> seen(size_t(frame.w) * size_t(frame.h), 0);
    struct Arm {
        float cx, cy;
        int n, min_x, max_x, min_y, max_y;
    };
    std::vector<Arm> arms;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!lit(x, y) || seen[size_t(y - y0) * size_t(frame.w) + size_t(x - x0)]) {
                continue;
            }
            Arm arm{0, 0, 0, x, x, y, y};
            std::vector<std::pair<int, int>> stack{{x, y}};
            seen[size_t(y - y0) * size_t(frame.w) + size_t(x - x0)] = 1;
            while (!stack.empty()) {
                const auto [sx, sy] = stack.back();
                stack.pop_back();
                arm.cx += float(sx);
                arm.cy += float(sy);
                ++arm.n;
                arm.min_x = std::min(arm.min_x, sx);
                arm.max_x = std::max(arm.max_x, sx);
                arm.min_y = std::min(arm.min_y, sy);
                arm.max_y = std::max(arm.max_y, sy);
                for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    const int nx = sx + dx, ny = sy + dy;
                    if (nx >= x0 && nx < x1 && ny >= y0 && ny < y1 && lit(nx, ny) &&
                        !seen[size_t(ny - y0) * size_t(frame.w) + size_t(nx - x0)]) {
                        seen[size_t(ny - y0) * size_t(frame.w) + size_t(nx - x0)] = 1;
                        stack.emplace_back(nx, ny);
                    }
                }
            }
            if (arm.n >= 60) {
                arm.cx /= float(arm.n);
                arm.cy /= float(arm.n);
                arms.push_back(arm);
            }
        }
    }
    const float mx = float(x0) + float(frame.w) * 0.5f, my = float(y0) + float(frame.h) * 0.5f;
    bool any = false;
    for (const Arm& arm : arms) {
        const float dx = arm.cx - mx, dy = arm.cy - my;
        const char* action = std::fabs(dx) > std::fabs(dy) ? (dx < 0 ? "left" : "right") : (dy < 0 ? "up" : "down");
        const int a = ActionIndex(action);
        const std::string label = a >= 0 ? ArrowLabel(font, ShortName(b.keys[a])) : std::string{};
        if (label.empty()) {
            continue;
        }
        const Label layout = LayoutLabel(font, label, arm.cx, arm.cy, float(arm.max_x - arm.min_x) - 2.0f,
                                         float(arm.max_y - arm.min_y) - 2.0f, 14.0f, 7.0f);
        ForEachLabelPixel(font, label, layout, [&](int x, int y, float cover) {
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                Blend(px, size, x, y, DarkInk, cover);
            }
        });
        any = true;
    }
    return any;
}

constexpr float DpadClusterKey = 19.0f; // one key of the WASD that takes the neutral d-pad frame (63 pixels wide)

/// Empties a frame of the atlas and a margin around it: the original glow reaches past the frame.
void ClearFrame(uint8_t* px, int size, const Icon& frame, int margin) {
    const int x0 = std::max(frame.x - margin, 0), x1 = std::min(frame.x + frame.w + margin, size);
    const int y0 = std::max(frame.y - margin, 0), y1 = std::min(frame.y + frame.h + margin, size);
    for (int y = y0; y < y1; ++y) {
        std::memset(&px[(size_t(y) * size_t(size) + size_t(x0)) * 4], 0, size_t(x1 - x0) * 4);
    }
}

/// The d-pad frame with the left and right arms lit, redrawn as a mouse whose wheel is lit, with an up
/// and a down arrow beside it (the wheel turns up or down).
void DrawWheelFrame(uint8_t* px, int size, const Icon& frame, const Font& font) {
    ClearFrame(px, size, frame, 6);
    const float cx = float(frame.x) + float(frame.w) * 0.5f, cy = float(frame.y) + float(frame.h) * 0.5f;
    (void)font;
    DrawMouse(px, size, cx, cy, 56.0f, Mouse::Wheel); // the template carries the two arrows of the wheel
}

// ---- BC1 (the large "interact" button of U1: color texture, separate alpha) -------------------

struct Rgb {
    int r, g, b;
};

Rgb Expand565(uint16_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

uint16_t Pack565(const Rgb& c) {
    return uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

void Bc1Palette(const uint8_t* block, Rgb palette[4]) {
    const uint16_t c0 = uint16_t(block[0] | (block[1] << 8)), c1 = uint16_t(block[2] | (block[3] << 8));
    palette[0] = Expand565(c0);
    palette[1] = Expand565(c1);
    if (c0 > c1) {
        palette[2] = {(2 * palette[0].r + palette[1].r) / 3, (2 * palette[0].g + palette[1].g) / 3,
                      (2 * palette[0].b + palette[1].b) / 3};
        palette[3] = {(palette[0].r + 2 * palette[1].r) / 3, (palette[0].g + 2 * palette[1].g) / 3,
                      (palette[0].b + 2 * palette[1].b) / 3};
    } else {
        palette[2] = {(palette[0].r + palette[1].r) / 2, (palette[0].g + palette[1].g) / 2,
                      (palette[0].b + palette[1].b) / 2};
        palette[3] = {0, 0, 0};
    }
}

/// Decodes a width x height BC1 level into RGBA8 (alpha 255).
void DecodeBc1(const uint8_t* data, int width, int height, uint8_t* rgba) {
    const int blocks_x = std::max(width / 4, 1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* block = data + (size_t(y / 4) * size_t(blocks_x) + size_t(x / 4)) * 8;
            Rgb palette[4];
            Bc1Palette(block, palette);
            const uint32_t bits = uint32_t(block[4]) | (uint32_t(block[5]) << 8) |
                                  (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
            const Rgb& c = palette[(bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3];
            uint8_t* p = rgba + (size_t(y) * size_t(width) + size_t(x)) * 4;
            p[0] = uint8_t(c.r);
            p[1] = uint8_t(c.g);
            p[2] = uint8_t(c.b);
            p[3] = 255;
        }
    }
}

/// Encodes the 4x4 pixels at (bx*4, by*4) of an RGBA8 level (edges clamped) as one BC1 block:
/// the two most distant colors are the end points.
void EncodeBc1Block(const uint8_t* rgba, int width, int height, int bx, int by, uint8_t out[8]) {
    Rgb px[16];
    for (int i = 0; i < 16; ++i) {
        const int x = std::min(bx * 4 + (i & 3), width - 1), y = std::min(by * 4 + (i >> 2), height - 1);
        const uint8_t* p = rgba + (size_t(y) * size_t(width) + size_t(x)) * 4;
        px[i] = {p[0], p[1], p[2]};
    }
    int best = -1, a = 0, b = 0;
    for (int i = 0; i < 16; ++i) {
        for (int j = i + 1; j < 16; ++j) {
            const int dr = px[i].r - px[j].r, dg = px[i].g - px[j].g, db = px[i].b - px[j].b;
            const int d = dr * dr + dg * dg + db * db;
            if (d > best) {
                best = d;
                a = i;
                b = j;
            }
        }
    }
    uint16_t c0 = Pack565(px[a]), c1 = Pack565(px[b]);
    if (c0 < c1) {
        std::swap(c0, c1);
    }
    out[0] = uint8_t(c0 & 255);
    out[1] = uint8_t(c0 >> 8);
    out[2] = uint8_t(c1 & 255);
    out[3] = uint8_t(c1 >> 8);
    uint32_t bits = 0;
    if (c0 != c1) {
        Rgb palette[4];
        Bc1Palette(out, palette);
        for (int i = 0; i < 16; ++i) {
            int pick = 0, nearest = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                const int dr = px[i].r - palette[k].r, dg = px[i].g - palette[k].g,
                          db = px[i].b - palette[k].b;
                const int d = dr * dr + dg * dg + db * db;
                if (d < nearest) {
                    nearest = d;
                    pick = k;
                }
            }
            bits |= uint32_t(pick) << (2 * i);
        }
    }
    out[4] = uint8_t(bits);
    out[5] = uint8_t(bits >> 8);
    out[6] = uint8_t(bits >> 16);
    out[7] = uint8_t(bits >> 24);
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_KEY_ICONS");
        return env && std::atoi(env) != 0;
    }();
    return on;
}

/// Paints the 512x512 atlas (the original in rgba). In a menu, phase 0 shows the keys on the pastilles that alternate
/// (select, back, the neutral d-pad), phase 1 the mouse: PatchU1Atlas fades from one painting to the other.
static bool PaintAtlas(uint8_t* rgba, int phase) {
    const Font& font = GetFont();
    if (!font.ok) {
        return false;
    }
    // In a menu the legends are drawn bigger (the game shows these pastilles small); back to normal on leaving.
    struct LegendBoost {
        LegendBoost() { g_legend_boost = g_menu_mode ? 1.4f : 1.0f; }
        ~LegendBoost() { g_legend_boost = 1.0f; }
    } legend_boost;
    Bindings b = LoadBindings();
    if (g_menu_mode) {
        ApplyMenuKeys(b);
    } else if (g_jetski_mode) {
        ApplyJetskiKeys(b);
    }
    const auto binding = [&](const char* action) {
        const int a = ActionIndex(action);
        return a >= 0 ? ShortName(b.keys[a]) : std::string{};
    };
    // Four stick keys as one word ("WASD", "IJKL") when each is a single character.
    const auto stick = [&](const char* up, const char* left, const char* down, const char* right) {
        std::string word;
        for (const char* dir : {up, left, down, right}) {
            const std::string key = binding(dir);
            if (key.size() != 1) {
                return std::string{};
            }
            word += key;
        }
        return word;
    };
    const std::string left_stick = stick("ls_up", "ls_left", "ls_down", "ls_right");
    const std::string right_stick = b.mouse_look ? std::string{"Mouse"} : stick("rs_up", "rs_left", "rs_down", "rs_right");

    int patched = 0;
    std::string summary;
    // The shoulder icons (L1, R1) come last: drawing an icon clears a margin around its box, which would cut the top of
    // the taller keys of L1/R1 if their neighbours (R stick, Options) were drawn after them.
    ClearShoulders(rgba, 512);
    g_shoulders_cleared = true;
    struct ShoulderFlag {
        ~ShoulderFlag() { g_shoulders_cleared = false; }
    } shoulder_flag;
    for (int pass = 0; pass < 2; ++pass)
    for (const Icon& icon : Icons) {
        if ((!std::strcmp(icon.label, "l1") || !std::strcmp(icon.label, "r1")) != (pass == 1)) {
            continue;
        }
        std::string label;
        if (!std::strcmp(icon.label, "ls")) {
            label = left_stick;
        } else if (!std::strcmp(icon.label, "rs")) {
            label = right_stick;
        } else {
            label = binding(icon.label);
        }
        if (label.empty()) {
            continue; // unbound: the controller icon stays
        }
        // In a menu the select and back pastilles show the key, then the mouse button (the fade goes from one to the other).
        const bool menu_pair = g_menu_mode && (!std::strcmp(icon.label, "cross") || !std::strcmp(icon.label, "circle"));
        if (menu_pair && phase == 1) {
            label = !std::strcmp(icon.label, "cross") ? "LMB" : "M4";
        }
        const bool drawn = DrawIcon(rgba, 512, icon, label, font);
        if (drawn) {
            ++patched;
            if (summary.find(std::string{icon.label} + "=") == std::string::npos) {
                summary += std::string{icon.label} + "=" + label + " ";
            }
        }
    }
    // The d-pad frames: neutral, left+right lit, up+down lit, up lit, down lit.
    static constexpr Icon Dpads[] = {{"dpad", 6, 5, 63, 65},    {"dpad", 81, 5, 63, 65},
                                     {"dpad", 156, 5, 63, 65},  {"dpad", 249, 226, 64, 66},
                                     {"dpad", 6, 360, 63, 65}};
    // The neutral pad (the menus: select) becomes the WASD; the left+right pad (change weapon) becomes the
    // mouse wheel when the left and right bindings are wheel turns.
    const auto wheel_binding = [&](const char* action) {
        const int a = ActionIndex(action);
        return a >= 0 && b.keys[a].find("Wheel") != std::string::npos;
    };
    const bool wheel = wheel_binding("left") || wheel_binding("right");
    // Weapon 1 (sidearm, "left") and weapon 2 (main weapon, "right"): the first keyboard or mouse-button binding of each
    // (a wheel turn is not a key to show); empty when only the wheel is bound.
    const auto weapon_key = [&](const char* action) {
        const int a = ActionIndex(action);
        if (a < 0) {
            return std::string{};
        }
        const std::string& value = b.keys[a];
        for (size_t start = 0; start <= value.size();) {
            size_t bar = value.find('|', start);
            if (bar == std::string::npos) {
                bar = value.size();
            }
            const std::string slot = value.substr(start, bar - start);
            if (!slot.empty() && slot.find("Wheel") == std::string::npos) {
                return ArrowLabel(font, ShortName(slot));
            }
            start = bar + 1;
        }
        return std::string{};
    };
    const std::string weapon_1 = weapon_key("left"), weapon_2 = weapon_key("right");
    const std::string down_key = ArrowLabel(font, binding("down"));
    const std::string up_key = ArrowLabel(font, binding("up"));
    for (size_t i = 0; i < sizeof(Dpads) / sizeof(Dpads[0]); ++i) {
        const Icon& dpad = Dpads[i];
        if (i == 0 && g_menu_mode && phase == 1) {
            // the menus: the d-pad icon is drawn small by the game, so the WASD (below, the usual cluster) and the mouse
            // wheel take turns at full size instead of sharing it
            DrawWheelFrame(rgba, 512, dpad, font);
            ++patched;
        } else if (i == 0 && left_stick.size() == 4) {
            ClearFrame(rgba, 512, dpad, 6);
            const std::string keys[4] = {left_stick.substr(0, 1), left_stick.substr(1, 1), left_stick.substr(2, 1),
                                         left_stick.substr(3, 1)};
            DrawCluster(rgba, 512, float(dpad.x) + float(dpad.w) * 0.5f, float(dpad.y) + float(dpad.h) * 0.5f, keys, font,
                        DpadClusterKey, -1, 1.0f);
            ++patched;
        } else if (i == 1 && g_menu_mode && left_stick.size() == 4) {
            // the menus: the left+right pad (a choice to change, Oui / Non of the pause of a cutscene) is the four keys of the stick, always:
            // the wheel does not change a choice there
            ClearFrame(rgba, 512, dpad, 6);
            const std::string keys[4] = {left_stick.substr(0, 1), left_stick.substr(1, 1), left_stick.substr(2, 1),
                                         left_stick.substr(3, 1)};
            DrawCluster(rgba, 512, float(dpad.x) + float(dpad.w) * 0.5f, float(dpad.y) + float(dpad.h) * 0.5f, keys, font,
                        DpadClusterKey, -1, 1.0f);
            ++patched;
        } else if (i == 1 && !weapon_1.empty() && !weapon_2.empty()) {
            // the change-weapon prompt: the two weapon keys side by side (the wheel only when no key is bound)
            ClearFrame(rgba, 512, dpad, 6);
            const float cx = float(dpad.x) + float(dpad.w) * 0.5f, cy = float(dpad.y) + float(dpad.h) * 0.5f;
            DrawSymbol(rgba, 512, cx - 16.0f, cy, 30.0f, 30.0f, weapon_1, font, Orange);
            DrawSymbol(rgba, 512, cx + 16.0f, cy, 30.0f, 30.0f, weapon_2, font, Orange);
            ++patched;
        } else if (i == 1 && wheel) {
            DrawWheelFrame(rgba, 512, dpad, font);
            ++patched;
        } else if (i == 2 && g_menu_mode && left_stick.size() == 4) {
            // the menus: the up+down pad (Select in a list) is moved with the stick keys and the wheel, not the d-pad
            // bindings: same prompt as the first d-pad, the WASD (phase 0) and the wheel (phase 1) taking turns
            if (phase == 1) {
                DrawWheelFrame(rgba, 512, dpad, font);
            } else {
                ClearFrame(rgba, 512, dpad, 6);
                const std::string keys[4] = {left_stick.substr(0, 1), left_stick.substr(1, 1), left_stick.substr(2, 1),
                                             left_stick.substr(3, 1)};
                DrawCluster(rgba, 512, float(dpad.x) + float(dpad.w) * 0.5f, float(dpad.y) + float(dpad.h) * 0.5f, keys, font,
                            DpadClusterKey, -1, 1.0f);
            }
            ++patched;
        } else if (i == 2 && !up_key.empty() && !down_key.empty()) {
            // up and down lit: the keys of both, one above the other
            ClearFrame(rgba, 512, dpad, 6);
            const float cx = float(dpad.x) + float(dpad.w) * 0.5f, cy = float(dpad.y) + float(dpad.h) * 0.5f;
            DrawSymbol(rgba, 512, cx, cy - 15.0f, 30.0f, 30.0f, up_key, font, Orange);
            DrawSymbol(rgba, 512, cx, cy + 15.0f, 30.0f, 30.0f, down_key, font, Orange);
            ++patched;
        } else if (i == 3 && !up_key.empty()) {
            // the d-pad up alone: a key, like the down one below
            ClearFrame(rgba, 512, dpad, 6);
            DrawSymbol(rgba, 512, float(dpad.x) + float(dpad.w) * 0.5f, float(dpad.y) + float(dpad.h) * 0.5f,
                       float(dpad.w) * 0.9f, float(dpad.h) * 0.9f, up_key, font, Orange);
            ++patched;
        } else if (i == 4 && !down_key.empty()) {
            // The d-pad down alone (the photo mode switch of the game menu): a key, like the other prompts.
            ClearFrame(rgba, 512, dpad, 6);
            DrawSymbol(rgba, 512, float(dpad.x) + float(dpad.w) * 0.5f, float(dpad.y) + float(dpad.h) * 0.5f,
                       float(dpad.w) * 0.9f, float(dpad.h) * 0.9f, down_key, font, Orange);
            ++patched;
        } else if (LabelDpadFrame(rgba, 512, dpad, b, font)) {
            ++patched;
        }
    }
    if (phase == 0) { // (the log of the second painting adds nothing)
        std::printf("KeyIcons: %d icons redrawn (%s)\n", patched, summary.c_str());
    }
    if (patched > 0 && phase == 0) {
        RequestHintPatch();
    }
    return patched > 0;
}

/// Coverage of a pixel whose center is at signed distance d from a shape (negative inside): a one pixel soft edge.
static float EdgeCover(float d) {
    return std::clamp(0.5f - d, 0.0f, 1.0f);
}

// ---- The Xbox pastilles: the game's own pictures, with the glyph changed -----------------------------------------------------
// Nothing is redrawn but the glyph: the discs, the bumpers, the halos and the press animation are the PlayStation ones. A glyph is
// found by its color (a symbol is saturated on a gray disc, a text is cream on a dark body), wiped by filling the place in from
// what surrounds it, and the Xbox glyph is drawn where it was, in the color the old one had.

struct XPic {
    uint8_t* px; // straight RGBA8, rows top to bottom
    int w, h;
};

struct XMask {
    int w, h, x0, y0, x1, y1, count;
    std::vector<uint8_t> m;
    XMask(int w_, int h_) : w(w_), h(h_), x0(w_), y0(h_), x1(-1), y1(-1), count(0), m(size_t(w_) * size_t(h_), 0) {}
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && x < w && y < h && m[size_t(y) * size_t(w) + size_t(x)] != 0;
    }
    void set(int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h || m[size_t(y) * size_t(w) + size_t(x)]) {
            return;
        }
        m[size_t(y) * size_t(w) + size_t(x)] = 1;
        ++count;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    float cx() const { return float(x0 + x1 + 1) * 0.5f; }
    float cy() const { return float(y0 + y1 + 1) * 0.5f; }
    /// The center of the box that holds the pixels but a few stray ones at its edges (2 % each side).
    void robust_center(float* rx, float* ry) const {
        std::vector<int> xs, ys;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (at(x, y)) {
                    xs.push_back(x);
                    ys.push_back(y);
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        const size_t lo = xs.size() / 50, hi = xs.size() - 1 - xs.size() / 50;
        *rx = float(xs[lo] + xs[hi] + 1) * 0.5f;
        *ry = float(ys[lo] + ys[hi] + 1) * 0.5f;
    }
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
};

/// One picture of the embedded Xbox assets (zlib-compressed, in the textures' own orientation).
struct XAsset {
    int w = 0, h = 0, ch = 0;
    std::vector<uint8_t> data;
};

static bool XLoadAsset(uint32_t id, XAsset& out) {
    const uint8_t* p = bb_xbox_assets;
    if (std::memcmp(p, "BBXA", 4) != 0) {
        return false;
    }
    uint32_t count = 0;
    std::memcpy(&count, p + 4, 4);
    size_t off = 8;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t head[5];
        std::memcpy(head, p + off, sizeof(head));
        off += sizeof(head);
        if (head[0] == id) {
            size_t n = 0;
            void* mem = tinfl_decompress_mem_to_heap(p + off, head[4], &n, TINFL_FLAG_PARSE_ZLIB_HEADER);
            const size_t want = size_t(head[1]) * size_t(head[2]) * size_t(head[3]);
            if (!mem || n != want) {
                mz_free(mem);
                return false;
            }
            out.w = int(head[1]);
            out.h = int(head[2]);
            out.ch = int(head[3]);
            out.data.assign(static_cast<uint8_t*>(mem), static_cast<uint8_t*>(mem) + n);
            mz_free(mem);
            return true;
        }
        off += head[4];
    }
    return false;
}

static uint8_t* XAt(const XPic& pic, int x, int y) {
    return pic.px + (size_t(y) * size_t(pic.w) + size_t(x)) * 4;
}

static float XLuma(const uint8_t* p) {
    return 0.299f * float(p[0]) + 0.587f * float(p[1]) + 0.114f * float(p[2]);
}

static int XSat(const uint8_t* p) {
    return std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
}

/// The pixels of a rectangle (x0..x1 and y0..y1 excluded) that pred(x, y, pixel) accepts.
template <class Pred>
static XMask XCollect(const XPic& pic, int rx0, int ry0, int rx1, int ry1, Pred pred) {
    XMask mask(pic.w, pic.h);
    for (int y = std::max(ry0, 0); y < std::min(ry1, pic.h); ++y) {
        for (int x = std::max(rx0, 0); x < std::min(rx1, pic.w); ++x) {
            if (pred(x, y, XAt(pic, x, y))) {
                mask.set(x, y);
            }
        }
    }
    return mask;
}

static XMask XDilate(const XMask& in, int r) {
    XMask out(in.w, in.h);
    for (int y = in.y0; y <= in.y1; ++y) {
        for (int x = in.x0; x <= in.x1; ++x) {
            if (in.at(x, y)) {
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        out.set(x + dx, y + dy);
                    }
                }
            }
        }
    }
    return out;
}

/// The mask with its inside: every row filled from its first pixel to its last (a figure drawn as an outline, like the triangle, has a
/// darker inside that would stay as a ghost).
static XMask XFilled(const XMask& in) {
    XMask out = in;
    for (int y = in.y0; y <= in.y1; ++y) {
        int first = -1, last = -1;
        for (int x = in.x0; x <= in.x1; ++x) {
            if (in.at(x, y)) {
                first = first < 0 ? x : first;
                last = x;
            }
        }
        for (int x = first; first >= 0 && x <= last; ++x) {
            out.set(x, y);
        }
    }
    return out;
}

/// The mean color of the third of the pixels of a mask that score the highest.
template <class Score>
static Color XTypicalColor(const XPic& pic, const XMask& mask, Score score) {
    std::vector<std::pair<float, const uint8_t*>> all;
    for (int y = mask.y0; y <= mask.y1; ++y) {
        for (int x = mask.x0; x <= mask.x1; ++x) {
            if (mask.at(x, y)) {
                all.emplace_back(score(XAt(pic, x, y)), XAt(pic, x, y));
            }
        }
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t n = std::max<size_t>(1, all.size() / 3);
    float r = 0, g = 0, b = 0;
    for (size_t i = 0; i < n; ++i) {
        r += float(all[i].second[0]);
        g += float(all[i].second[1]);
        b += float(all[i].second[2]);
    }
    return {r / float(n), g / float(n), b / float(n)};
}

/// Fills the pixels of a hole from what is around it: layer after layer from the edge (each pixel the mean of its known
/// neighbours), then softened a little.
static void XInpaint(const XPic& pic, const XMask& hole) {
    if (hole.count == 0) {
        return;
    }
    const int x0 = std::max(hole.x0 - 1, 0), y0 = std::max(hole.y0 - 1, 0);
    const int x1 = std::min(hole.x1 + 1, pic.w - 1), y1 = std::min(hole.y1 + 1, pic.h - 1);
    std::vector<uint8_t> unknown = hole.m;
    const auto is_unknown = [&](int x, int y) { return unknown[size_t(y) * size_t(pic.w) + size_t(x)] != 0; };
    struct Fill {
        int x, y;
        uint8_t v[4];
    };
    std::vector<Fill> layer;
    int remaining = hole.count;
    for (int guard = 0; remaining > 0 && guard < 400; ++guard) {
        layer.clear();
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (!is_unknown(x, y)) {
                    continue;
                }
                int sum[4] = {0, 0, 0, 0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < pic.w && ny < pic.h && !is_unknown(nx, ny)) {
                            const uint8_t* p = XAt(pic, nx, ny);
                            for (int c = 0; c < 4; ++c) {
                                sum[c] += p[c];
                            }
                            ++n;
                        }
                    }
                }
                if (n > 0) {
                    Fill f{x, y, {}};
                    for (int c = 0; c < 4; ++c) {
                        f.v[c] = uint8_t(sum[c] / n);
                    }
                    layer.push_back(f);
                }
            }
        }
        if (layer.empty()) {
            break;
        }
        for (const Fill& f : layer) {
            std::memcpy(XAt(pic, f.x, f.y), f.v, 4);
            unknown[size_t(f.y) * size_t(pic.w) + size_t(f.x)] = 0;
        }
        remaining -= int(layer.size());
    }
    // relaxation: a smooth surface from the edge of the hole to the other (no trace of the layers)
    std::vector<std::pair<int, int>> inside;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (hole.at(x, y)) {
                inside.emplace_back(x, y);
            }
        }
    }
    for (int sweep = 0; sweep < 160; ++sweep) {
        for (const auto& [x, y] : inside) {
            int sum[4] = {0, 0, 0, 0}, n = 0;
            const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& o : offsets) {
                const int nx = x + o[0], ny = y + o[1];
                if (nx >= 0 && ny >= 0 && nx < pic.w && ny < pic.h) {
                    const uint8_t* p = XAt(pic, nx, ny);
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += p[c];
                    }
                    ++n;
                }
            }
            uint8_t* p = XAt(pic, x, y);
            for (int c = 0; c < 4; ++c) {
                p[c] = uint8_t(sum[c] / n);
            }
        }
    }
}

/// Gives the saturated pixels of a rectangle (the glow, the rim and the symbol of a button) another hue, keeping their light.
static void XRecolor(const XPic& pic, int rx0, int ry0, int rx1, int ry1, float hue_degrees, int sat_min) {
    for (int y = std::max(ry0, 0); y < std::min(ry1, pic.h); ++y) {
        for (int x = std::max(rx0, 0); x < std::min(rx1, pic.w); ++x) {
            uint8_t* p = XAt(pic, x, y);
            if (XSat(p) < sat_min || p[3] == 0) {
                continue;
            }
            const float r = float(p[0]) / 255.0f, g = float(p[1]) / 255.0f, b = float(p[2]) / 255.0f;
            const float v = std::max({r, g, b}), c = v - std::min({r, g, b});
            const float s = v > 0.0f ? c / v : 0.0f;
            // HSV to RGB with the new hue
            const float h = hue_degrees / 60.0f;
            const float k = c > 0.0f ? s * v : 0.0f;
            const float m = v - k;
            const float xx = k * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
            float rgb[3] = {0, 0, 0};
            switch (int(h) % 6) {
            case 0: rgb[0] = k; rgb[1] = xx; break;
            case 1: rgb[0] = xx; rgb[1] = k; break;
            case 2: rgb[1] = k; rgb[2] = xx; break;
            case 3: rgb[1] = xx; rgb[2] = k; break;
            case 4: rgb[0] = xx; rgb[2] = k; break;
            default: rgb[0] = k; rgb[2] = xx; break;
            }
            for (int i = 0; i < 3; ++i) {
                p[i] = uint8_t(std::clamp((rgb[i] + m) * 255.0f, 0.0f, 255.0f) + 0.5f);
            }
        }
    }
}

/// Fills a hole in a button face row by row: each row of the hole is a blend of the pixels at its two ends, so the vertical shading
/// of the button goes on through the hole; a few sweeps then even out the noise.
static void XInpaintRows(const XPic& pic, const XMask& hole) {
    for (int y = hole.y0; y <= hole.y1; ++y) {
        for (int x = hole.x0; x <= hole.x1;) {
            if (!hole.at(x, y)) {
                ++x;
                continue;
            }
            int xb = x;
            while (hole.at(xb + 1, y)) {
                ++xb;
            }
            float left[4] = {0, 0, 0, 0}, right[4] = {0, 0, 0, 0};
            int nl = 0, nr = 0;
            for (int k = 1; k <= 4; ++k) {
                if (x - k >= 0) {
                    for (int c = 0; c < 4; ++c) left[c] += float(XAt(pic, x - k, y)[c]);
                    ++nl;
                }
                if (xb + k < pic.w) {
                    for (int c = 0; c < 4; ++c) right[c] += float(XAt(pic, xb + k, y)[c]);
                    ++nr;
                }
            }
            for (int xx = x; xx <= xb; ++xx) {
                const float t = float(xx - x + 1) / float(xb - x + 2);
                for (int c = 0; c < 4; ++c) {
                    const float l = nl ? left[c] / float(nl) : right[c] / float(std::max(nr, 1));
                    const float r = nr ? right[c] / float(nr) : l;
                    XAt(pic, xx, y)[c] = uint8_t(std::clamp(l * (1.0f - t) + r * t, 0.0f, 255.0f) + 0.5f);
                }
            }
            x = xb + 1;
        }
    }
    for (int sweep = 0; sweep < 4; ++sweep) {
        for (int y = hole.y0; y <= hole.y1; ++y) {
            for (int x = hole.x0; x <= hole.x1; ++x) {
                if (!hole.at(x, y) || y == 0 || y + 1 >= pic.h) {
                    continue;
                }
                for (int c = 0; c < 4; ++c) {
                    const int v = int(XAt(pic, x, y - 1)[c]) + 2 * int(XAt(pic, x, y)[c]) + int(XAt(pic, x, y + 1)[c]);
                    XAt(pic, x, y)[c] = uint8_t(v / 4);
                }
            }
        }
    }
}

/// The coverage of a text, centered on (0, 0) of its own coordinates: x0, y0 is where the buffer starts.
struct XCover {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<float> a;
};

/// A text whose capitals are cap_h pixels tall, stretched by sx and sy; bold: struck again a pixel to the right.
static XCover XTextCover(const Font& font, const std::string& text, float cap_h, float sx, float sy, int bold) {
    int bx0, by0, bx1, by1;
    stbtt_GetCodepointBox(&font.info, 'H', &bx0, &by0, &bx1, &by1);
    const float s = cap_h / float(by1);
    const float fx = s * sx, fy = s * sy;
    const float width = TextWidth(font, text, fx);
    XCover c;
    c.w = int(width) + 10;
    c.h = int(cap_h * sy * 1.8f) + 10;
    c.x0 = -c.w / 2;
    c.y0 = -c.h / 2;
    c.a.assign(size_t(c.w) * size_t(c.h), 0.0f);
    const int baseline = int(std::lround(cap_h * sy * 0.5f));
    float pen = -width * 0.5f;
    for (const int cp : Codepoints(text)) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font.info, cp, &advance, &bearing);
        int gx0, gy0, gx1, gy1;
        stbtt_GetCodepointBitmapBox(&font.info, cp, fx, fy, &gx0, &gy0, &gx1, &gy1);
        const int gw = gx1 - gx0, gh = gy1 - gy0;
        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> glyph(size_t(gw) * size_t(gh));
            stbtt_MakeCodepointBitmap(&font.info, glyph.data(), gw, gh, gw, fx, fy, cp);
            for (int j = 0; j < gh; ++j) {
                for (int i = 0; i < gw; ++i) {
                    const float v = float(glyph[size_t(j) * size_t(gw) + size_t(i)]) / 255.0f;
                    for (int pass = 0; pass <= bold; ++pass) {
                        const int X = int(std::floor(pen)) + gx0 + i + pass - c.x0, Y = baseline + gy0 + j - c.y0;
                        if (X >= 0 && Y >= 0 && X < c.w && Y < c.h) {
                            float& d = c.a[size_t(Y) * size_t(c.w) + size_t(X)];
                            d = std::max(d, v);
                        }
                    }
                }
            }
        }
        pen += float(advance) * fx;
    }
    return c;
}

/// A soft copy of a coverage (box blur of radius r, twice).
static XCover XBlur(const XCover& in, int r) {
    XCover out;
    out.x0 = in.x0 - r * 2;
    out.y0 = in.y0 - r * 2;
    out.w = in.w + r * 4;
    out.h = in.h + r * 4;
    out.a.assign(size_t(out.w) * size_t(out.h), 0.0f);
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            out.a[size_t(y + r * 2) * size_t(out.w) + size_t(x + r * 2)] = in.a[size_t(y) * size_t(in.w) + size_t(x)];
        }
    }
    std::vector<float> tmp(out.a.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < out.h; ++y) {
            for (int x = 0; x < out.w; ++x) {
                float sum = 0;
                for (int k = -r; k <= r; ++k) {
                    const int xx = x + k;
                    sum += xx >= 0 && xx < out.w ? out.a[size_t(y) * size_t(out.w) + size_t(xx)] : 0.0f;
                }
                tmp[size_t(y) * size_t(out.w) + size_t(x)] = sum / float(2 * r + 1);
            }
        }
        for (int y = 0; y < out.h; ++y) {
            for (int x = 0; x < out.w; ++x) {
                float sum = 0;
                for (int k = -r; k <= r; ++k) {
                    const int yy = y + k;
                    sum += yy >= 0 && yy < out.h ? tmp[size_t(yy) * size_t(out.w) + size_t(x)] : 0.0f;
                }
                out.a[size_t(y) * size_t(out.w) + size_t(x)] = sum / float(2 * r + 1);
            }
        }
    }
    return out;
}

static void XOver(const XPic& pic, int x, int y, const Color& c, float a) {
    if (a <= 0.0f || x < 0 || y < 0 || x >= pic.w || y >= pic.h) {
        return;
    }
    a = std::min(a, 1.0f);
    uint8_t* p = XAt(pic, x, y);
    const float da = float(p[3]) / 255.0f;
    const float oa = a + da * (1.0f - a);
    const float k = da * (1.0f - a);
    const float in[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) {
        p[i] = uint8_t(std::clamp((in[i] * a + float(p[i]) * k) / oa, 0.0f, 255.0f) + 0.5f);
    }
    p[3] = uint8_t(oa * 255.0f + 0.5f);
}

/// Paints a coverage centered on (cx, cy); flip: the picture is stored upside down (the HUD textures), the glyph is turned over.
static void XPaintCover(const XPic& pic, const XCover& cov, float cx, float cy, const Color& color, bool flip, float strength = 1.0f) {
    const int ox = int(std::lround(cx)), oy = int(std::lround(cy));
    for (int j = 0; j < cov.h; ++j) {
        for (int i = 0; i < cov.w; ++i) {
            const float a = cov.a[size_t(j) * size_t(cov.w) + size_t(i)];
            if (a > 0.003f) {
                XOver(pic, ox + cov.x0 + i, flip ? oy - 1 - (cov.y0 + j) : oy + cov.y0 + j, color, a * strength);
            }
        }
    }
}

/// A text with the dark outline of the game's own lettering.
static void XPaintOutlined(const XPic& pic, const XCover& cov, float cx, float cy, const Color& fill, bool flip) {
    constexpr Color Outline{22.0f, 18.0f, 10.0f};
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx || dy) {
                XPaintCover(pic, cov, cx + float(dx), cy + float(dy), Outline, flip);
            }
        }
    }
    XPaintCover(pic, cov, cx, cy, fill, flip);
}

/// The ring of a rounded rectangle, w pixels wide.
static void XOutlineRect(const XPic& pic, float cx, float cy, float half_w, float half_h, float radius, float w, const Color& color) {
    for (int y = int(cy - half_h) - 2; y <= int(cy + half_h) + 2; ++y) {
        for (int x = int(cx - half_w) - 2; x <= int(cx + half_w) + 2; ++x) {
            const float d = RoundRect(float(x) + 0.5f - cx, float(y) + 0.5f - cy, half_w, half_h, radius);
            XOver(pic, x, y, color, EdgeCover(std::fabs(d + w * 0.5f) - w * 0.5f));
        }
    }
}

/// The pixels joined to a corner of the picture by pixels of its color: the background of a HUD texture (the glow, same green as the
/// symbol's), which a symbol search must not take for the symbol.
static std::vector<uint8_t> XBackground(const XPic& pic) {
    std::vector<uint8_t> outside(size_t(pic.w) * size_t(pic.h), 0);
    const uint8_t* ref = XAt(pic, 0, 0);
    const auto close = [&](const uint8_t* p) {
        return std::abs(int(p[0]) - int(ref[0])) + std::abs(int(p[1]) - int(ref[1])) + std::abs(int(p[2]) - int(ref[2])) < 60;
    };
    std::vector<std::pair<int, int>> todo{{0, 0}, {pic.w - 1, 0}, {0, pic.h - 1}, {pic.w - 1, pic.h - 1}};
    while (!todo.empty()) {
        const auto [x, y] = todo.back();
        todo.pop_back();
        if (x < 0 || y < 0 || x >= pic.w || y >= pic.h || outside[size_t(y) * size_t(pic.w) + size_t(x)] || !close(XAt(pic, x, y))) {
            continue;
        }
        outside[size_t(y) * size_t(pic.w) + size_t(x)] = 1;
        todo.push_back({x + 1, y});
        todo.push_back({x - 1, y});
        todo.push_back({x, y + 1});
        todo.push_back({x, y - 1});
    }
    return outside;
}

/// The symbol of a button (a saturated figure on a gray disc, inside the ellipse of radii rx, ry around (cx, cy)) becomes a letter of
/// the same color. cap_h: the letter's height, squash: how much the picture is squashed vertically (the press frames), glow: the
/// radius of a soft light around it (0: none).
static bool XReplaceSymbol(const XPic& pic, const Font& font, float cx, float cy, float rx, float ry, int sat_min, int dilate,
                           const char* letter, float cap_h, float squash, bool flip, int glow, bool hud = false, const Color* letter_color = nullptr, int edge_guard = 6) {
    // (a HUD texture has the glow around the disc as its background: left out)
    const std::vector<uint8_t> outside = hud ? XBackground(pic) : std::vector<uint8_t>{};
    const XMask sym = XCollect(pic, int(cx - rx) - 1, int(cy - ry) - 1, int(cx + rx) + 2, int(cy + ry) + 2,
                               [&](int x, int y, const uint8_t* p) {
                                   const float u = (float(x) + 0.5f - cx) / rx, v = (float(y) + 0.5f - cy) / ry;
                                   if (!(u * u + v * v < 1.0f && XSat(p) >= sat_min && p[3] > 128)) {
                                       return false;
                                   }
                                   if (hud) { // not near the background either: the rim of the disc is blended with it
                                       for (int dy = -edge_guard; dy <= edge_guard; ++dy) {
                                           for (int dx = -edge_guard; dx <= edge_guard; ++dx) {
                                               const int nx = x + dx, ny = y + dy;
                                               if (nx >= 0 && ny >= 0 && nx < pic.w && ny < pic.h && outside[size_t(ny) * size_t(pic.w) + size_t(nx)]) {
                                                   return false;
                                               }
                                           }
                                       }
                                   }
                                   return true;
                               });
    if (sym.count < 12) {
        return false;
    }
    const Color color = letter_color ? *letter_color : XTypicalColor(pic, sym, [](const uint8_t* p) { return float(XSat(p)); });
    float gx = sym.cx(), gy = sym.cy();
    if (hud) {
        sym.robust_center(&gx, &gy);
    }
    XInpaint(pic, XDilate(XFilled(sym), dilate));
    const XCover cov = XTextCover(font, letter, cap_h, 1.0f, squash, false);
    if (glow > 0) {
        XPaintCover(pic, XBlur(cov, glow), gx, gy, color, flip, 0.6f);
    }
    XPaintCover(pic, cov, gx, gy, color, flip);
    return true;
}

/// A lettering of the game (cream with a dark outline, found as the bright pixels of a rectangle) becomes another text in the same
/// place, as tall and in the same color.
static bool XReplaceText(const XPic& pic, const Font& font, int rx0, int ry0, int rx1, int ry1, float luma_min, const char* text,
                         bool flip = false) {
    const XMask old = XCollect(pic, rx0, ry0, rx1, ry1, [&](int, int, const uint8_t* p) { return XLuma(p) >= luma_min && p[3] > 128; });
    if (old.count < 8) {
        return false;
    }
    const Color fill = XTypicalColor(pic, old, [](const uint8_t* p) { return XLuma(p); });
    const float cap_h = float(old.height());
    const float cx = old.cx(), cy = old.cy();
    XInpaint(pic, XDilate(old, 2));
    int bx0, by0, bx1, by1;
    stbtt_GetCodepointBox(&font.info, 'H', &bx0, &by0, &bx1, &by1);
    const float natural = TextWidth(font, text, cap_h / float(by1));
    const float sx = std::min(1.0f, (float(std::max(old.width(), 12)) + 4.0f) / std::max(natural, 1.0f));
    XPaintOutlined(pic, XTextCover(font, text, cap_h, sx, 1.0f, true), cx, cy, fill, flip);
    return true;
}

/// The hand of the touchpad button (the journal's too) becomes the View glyph: two overlapping sheets, in the hand's color.
static bool XReplaceHand(const XPic& pic, int rx0, int ry0, int rx1, int ry1, bool flip, int wipe) {
    const XMask hand = XCollect(pic, rx0, ry0, rx1, ry1, [&](int, int, const uint8_t* p) { return XLuma(p) >= 110.0f && XSat(p) >= 25 && p[3] > 128; });
    if (hand.count < 30) {
        return false;
    }
    const Color color = XTypicalColor(pic, hand, [](const uint8_t* p) { return XLuma(p); });
    const float cx = hand.cx(), cy = hand.cy();
    const float s = std::min(float(hand.width()), float(hand.height())) * 0.5f; // the glyph is about as big as the hand
    XInpaintRows(pic, XDilate(hand, wipe)); // (the hand has a dark shadow around it)
    const float d = flip ? -1.0f : 1.0f;
    XOutlineRect(pic, cx - s * 0.22f, cy - d * s * 0.2f, s * 0.62f, s * 0.46f, 1.5f, std::max(1.6f, s * 0.14f), color);
    XOutlineRect(pic, cx + s * 0.22f, cy + d * s * 0.2f, s * 0.62f, s * 0.46f, 1.5f, std::max(1.6f, s * 0.14f), color);
    return true;
}


// ---- Buttons and d-pad drawn here, in the manner of the game's pastilles (dark metal, bevel, soft halo) ----------------------
// Drawn four times larger and shrunk, so that the edges are clean.

/// Shrinks a supersampled picture (S x S pixels to one) into dst at (ox, oy). replace: what was there is overwritten (cleared where
/// the new picture is empty), else the new one is laid over it. flip: the destination is stored upside down.
static void XDownsample(const XPic& hi, int S, const XPic& dst, int ox, int oy, bool flip, bool replace) {
    const int w = hi.w / S, h = hi.h / S;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < S; ++sy) {
                for (int sx = 0; sx < S; ++sx) {
                    const uint8_t* p = XAt(hi, x * S + sx, y * S + sy);
                    const float pa = float(p[3]) / 255.0f;
                    r += float(p[0]) * pa;
                    g += float(p[1]) * pa;
                    b += float(p[2]) * pa;
                    a += pa;
                }
            }
            const int dx = ox + x, dy = flip ? oy + (h - 1 - y) : oy + y;
            if (dx < 0 || dy < 0 || dx >= dst.w || dy >= dst.h) {
                continue;
            }
            if (a <= 0.0f) {
                if (replace) {
                    std::memset(XAt(dst, dx, dy), 0, 4);
                }
                continue;
            }
            const Color c{r / a, g / a, b / a};
            const float alpha = a / float(S * S);
            if (replace) {
                uint8_t* p = XAt(dst, dx, dy);
                p[0] = uint8_t(std::clamp(c.r, 0.0f, 255.0f) + 0.5f);
                p[1] = uint8_t(std::clamp(c.g, 0.0f, 255.0f) + 0.5f);
                p[2] = uint8_t(std::clamp(c.b, 0.0f, 255.0f) + 0.5f);
                p[3] = uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                XOver(dst, dx, dy, c, alpha);
            }
        }
    }
}

static float XSmooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// A filled rounded rectangle.
static void XFillRound(const XPic& pic, float cx, float cy, float half_w, float half_h, float radius, const Color& color) {
    for (int y = int(cy - half_h) - 2; y <= int(cy + half_h) + 2; ++y) {
        for (int x = int(cx - half_w) - 2; x <= int(cx + half_w) + 2; ++x) {
            XOver(pic, x, y, color, EdgeCover(RoundRect(float(x) + 0.5f - cx, float(y) + 0.5f - cy, half_w, half_h, radius)));
        }
    }
}

enum class XGlyph { Letter, Menu, View };

/// A round button of radius R centered on (cx, cy) of dst: dark metal lit from the top left, a bevel, and a halo (the color of the
/// button when `colored`, else the tan of the game's own). The glyph: a bold letter in the accent color, the three bars of Menu or the
/// two sheets of View. flip: dst is stored upside down. The square of 3 R around the center is rewritten (replace) or laid over.
static void XRoundButton(const XPic& dst, const Font& font, float cx, float cy, float R, XGlyph glyph, const char* letter,
                         const Color& accent, bool colored, bool flip, bool replace) {
    constexpr int S = 4;
    constexpr Color Cream{240.0f, 236.0f, 212.0f};
    const int side = int(std::ceil(R * 3.0f));
    std::vector<uint8_t> buf(size_t(side * S) * size_t(side * S) * 4, 0);
    const XPic hi{buf.data(), side * S, side * S};
    const float half = float(side) * 0.5f, cs = float(S);
    for (int j = 0; j < hi.h; ++j) {
        for (int i = 0; i < hi.w; ++i) {
            const float x = (float(i) + 0.5f) / cs - half, y = (float(j) + 0.5f) / cs - half;
            const float d = std::hypot(x, y);
            const float light = std::clamp(0.5f - 0.5f * (x + y) / (1.4142f * R), 0.0f, 1.0f); // 1 at the top left
            if (d > R * 0.9f) { // the halo, and a thin ring of the button's color just outside the bevel
                const float k = (d - R) / (R * (colored ? 0.26f : 0.16f));
                XOver(hi, i, j, accent, (colored ? 0.75f : 0.32f) * std::exp(-k * k));
                if (colored) {
                    XOver(hi, i, j, accent, 0.95f * EdgeCover((std::fabs(d - (R + 1.1f)) - 0.9f) * cs));
                }
            }
            const float cover = EdgeCover((d - R) * cs);
            if (cover > 0.0f) {
                const Color base = Lerp({24.0f, 24.0f, 28.0f}, {72.0f, 70.0f, 70.0f}, std::pow(light, 1.5f));
                const Color rim = Lerp({12.0f, 12.0f, 14.0f}, {158.0f, 152.0f, 142.0f}, light);
                Color c = Lerp(base, rim, XSmooth(0.80f, 0.97f, d / R) * 0.9f);
                const float vignette = 1.0f - 0.2f * std::pow(d / R, 3.0f);
                c = {c.r * vignette, c.g * vignette, c.b * vignette};
                XOver(hi, i, j, c, cover);
            }
        }
    }
    const float mx = half * cs, my = half * cs;
    if (glyph == XGlyph::Letter) {
        const XCover cov = XTextCover(font, letter, R * 1.28f * cs, 1.0f, 1.0f, 7);
        XPaintCover(hi, XBlur(cov, S * 2), mx, my, accent, false, 0.6f);
        XPaintCover(hi, cov, mx, my, accent, false);
    } else if (glyph == XGlyph::Menu) {
        for (int k = -1; k <= 1; ++k) {
            XFillRound(hi, mx, my + float(k) * 0.29f * R * cs, 0.42f * R * cs, 0.075f * R * cs, 0.075f * R * cs, Cream);
        }
    } else {
        const float hw = 0.36f * R * cs, hh = 0.27f * R * cs, rad = 0.06f * R * cs, w = 0.085f * R * cs;
        const float ox = 0.15f * R * cs, oy = 0.13f * R * cs;
        XOutlineRect(hi, mx - ox, my - oy, hw, hh, rad, w, Cream);
        XFillRound(hi, mx + ox, my + oy, hw, hh, rad, {30.0f, 30.0f, 34.0f}); // the front sheet hides the back one
        XOutlineRect(hi, mx + ox, my + oy, hw, hh, rad, w, Cream);
    }
    XDownsample(hi, S, dst, int(std::lround(cx - half)), int(std::lround(cy - half)), flip, replace);
}

static float XSmin(float a, float b, float k) {
    const float h = std::max(k - std::fabs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}

/// A d-pad frame as the Xbox pad's: one cross-shaped button. lit: the arms in orange (bit 0 up, 1 down, 2 left, 3 right). The frame's
/// box and a margin of 6 pixels around it are rewritten.
static void XDpadFrame(const XPic& dst, const Icon& frame, int lit) {
    constexpr int S = 4, M = 6;
    constexpr Color Tan{200.0f, 178.0f, 140.0f};
    const int w = frame.w + 2 * M, h = frame.h + 2 * M;
    std::vector<uint8_t> buf(size_t(w * S) * size_t(h * S) * 4, 0);
    const XPic hi{buf.data(), w * S, h * S};
    const float L = 29.5f, T = 9.6f, corner = 3.6f, cs = float(S); // half length, half thickness, corner of the cross
    for (int j = 0; j < hi.h; ++j) {
        for (int i = 0; i < hi.w; ++i) {
            const float x = (float(i) + 0.5f) / cs - float(w) * 0.5f, y = (float(j) + 0.5f) / cs - float(h) * 0.5f;
            const float sd = XSmin(RoundRect(x, y, L, T, corner), RoundRect(x, y, T, L, corner), 4.0f);
            const float light = std::clamp(0.5f - 0.5f * (x + y) / (1.4142f * L), 0.0f, 1.0f);
            if (sd > -1.0f) {
                const float k = sd / 3.6f;
                XOver(hi, i, j, Tan, 0.42f * std::exp(-k * k));
            }
            const float cover = EdgeCover(sd * cs);
            if (cover > 0.0f) {
                const Color base = Lerp({30.0f, 30.0f, 33.0f}, {66.0f, 64.0f, 63.0f}, std::pow(light, 1.3f));
                const Color rim = Lerp({10.0f, 10.0f, 12.0f}, {182.0f, 174.0f, 160.0f}, std::pow(light, 1.2f));
                XOver(hi, i, j, Lerp(base, rim, XSmooth(-3.0f, -0.5f, sd) * 0.95f), cover);
            }
            const bool arm = ((lit & 1) && y < -4.0f && std::fabs(x) < T - 2.4f) || ((lit & 2) && y > 4.0f && std::fabs(x) < T - 2.4f) ||
                             ((lit & 4) && x < -4.0f && std::fabs(y) < T - 2.4f) || ((lit & 8) && x > 4.0f && std::fabs(y) < T - 2.4f);
            const float inner = EdgeCover((sd + 2.2f) * cs);
            if (arm && inner > 0.0f) {
                const float v = std::clamp((y + 14.0f) / 28.0f, 0.0f, 1.0f);
                const Color orange = Lerp({252.0f, 174.0f, 96.0f}, {226.0f, 122.0f, 48.0f}, v);
                XOver(hi, i, j, Lerp({150.0f, 76.0f, 24.0f}, orange, XSmooth(0.0f, 2.0f, -(sd + 2.2f))), inner);
            }
        }
    }
    XDownsample(hi, S, dst, frame.x - M, frame.y - M, false, true);
}

/// A 64x64 button picture of the Xbox assets (straight RGBA) written over the atlas, centered on (cx, cy): the user's own buttons,
/// taken as they are. False when the asset is missing (the caller draws its own).
static bool XBlitTile(const XPic& dst, uint32_t id, float cx, float cy) {
    XAsset tile;
    if (!XLoadAsset(id, tile) || tile.w != 64 || tile.h != 64 || tile.ch != 4) {
        return false;
    }
    const int ox = int(std::lround(cx)) - tile.w / 2, oy = int(std::lround(cy)) - tile.h / 2;
    for (int y = 0; y < tile.h; ++y) {
        for (int x = 0; x < tile.w; ++x) {
            if (ox + x >= 0 && oy + y >= 0 && ox + x < dst.w && oy + y < dst.h) {
                std::memcpy(XAt(dst, ox + x, oy + y), &tile.data[(size_t(y) * size_t(tile.w) + size_t(x)) * 4], 4);
            }
        }
    }
    return true;
}

/// The atlas (the game's original in rgba) with the Xbox glyphs: A B X Y on the face buttons, LB RB, LT RT, LS RS, MENU for Options,
/// View for the touchpad. The sticks' own icons ("L", "R"), the d-pad frames and everything around the glyphs are the game's.
static bool PaintAtlasXbox(uint8_t* rgba) {
    const Font& font = GetFont();
    if (!font.ok) {
        return false;
    }
    const XPic pic{rgba, 512, 512};
    static const struct {
        const char* action;
        const char* letter;
        Color color; // of the letter, the ring and the halo: the Xbox button's color (when the button is drawn here)
        uint32_t tile; // the user's own picture of the button (Xbox assets)
    } faces[] = {{"cross", "A", {96.0f, 200.0f, 72.0f}, 13},
                 {"circle", "B", {238.0f, 70.0f, 56.0f}, 10},
                 {"square", "X", {76.0f, 148.0f, 240.0f}, 11},
                 {"triangle", "Y", {248.0f, 204.0f, 44.0f}, 12}};
    static const struct {
        const char* action;
        const char* text;
    } texts[] = {{"l1", "LB"}, {"r1", "RB"}, {"l2", "LT"}, {"r2", "RT"}, {"l3", "LS"}, {"r3", "RS"}};
    int patched = 0;
    for (const Icon& icon : Icons) {
        const float cx = float(icon.x) + float(icon.w) * 0.5f, cy = float(icon.y) + float(icon.h) * 0.5f;
        bool done = false;
        for (const auto& f : faces) {
            if (!std::strcmp(icon.label, f.action)) {
                if (!XBlitTile(pic, f.tile, cx, cy)) {
                    XRoundButton(pic, font, cx, cy, 18.5f, XGlyph::Letter, f.letter, f.color, true, false, true);
                }
                done = true;
            }
        }
        for (const auto& t : texts) {
            if (!std::strcmp(icon.label, t.action)) {
                const bool bumper = t.action[1] == '1', trigger = t.action[1] == '2';
                const int mx = bumper ? 4 : trigger ? 8 : 8, my_top = bumper ? 3 : trigger ? 8 : 8, my_bottom = bumper ? 3 : trigger ? 14 : 8;
                done = XReplaceText(pic, font, icon.x + mx, icon.y + my_top, icon.x + icon.w - mx, icon.y + icon.h - my_bottom, 200.0f, t.text);
            }
        }
        if (!std::strcmp(icon.label, "options") || !std::strcmp(icon.label, "touchpad")) { // the old pill and the old pad: all of it goes
            for (int y = std::max(icon.y - 10, 0); y < std::min(icon.y + icon.h + 10, 512); ++y) {
                std::memset(XAt(pic, std::max(icon.x - 10, 0), y), 0, size_t(std::min(icon.x + icon.w + 10, 512) - std::max(icon.x - 10, 0)) * 4);
            }
        }
        if (!std::strcmp(icon.label, "options")) { // the Menu button: a round button with three bars (the word and the pill go)
            if (!XBlitTile(pic, 14, cx, cy)) {
                XRoundButton(pic, font, cx, cy, 19.0f, XGlyph::Menu, nullptr, {200.0f, 178.0f, 140.0f}, false, false, true);
            }
            done = true;
        }
        if (!std::strcmp(icon.label, "touchpad")) { // the View button: a round button, not the pad's wide one
            if (!XBlitTile(pic, 15, cx, cy)) {
                XRoundButton(pic, font, cx, cy, 19.0f, XGlyph::View, nullptr, {200.0f, 178.0f, 140.0f}, false, false, true);
            }
            done = true;
        }
        patched += done ? 1 : 0;
    }
    // (the d-pad frames stay the game's)
    std::printf("KeyIcons: %d icons with Xbox glyphs\n", patched);
    if (patched > 0) {
        RequestHintPatch(); // (the shoulder names of the menus are texts: a new text bank is searched too)
    }
    return patched > 0;
}

// The menu pastilles that alternate between the keys and the mouse fade from one to the other: both pictures are painted
// once per change of the bindings or of the menu (g_base_generation), then each step of the fade only blends them.
constexpr int FadeSteps = 8;
constexpr double HoldSeconds = 1.4, FadeSeconds = 0.6;
std::atomic<uint32_t> g_base_generation{1};
std::atomic<uint32_t> g_phase_epoch{0};
std::atomic<int> g_fade_step{0}; // 0 the keys ... FadeSteps the mouse

bool PatchU1Atlas(uint8_t* rgba, uint32_t width, uint32_t height, int slot) {
    if (width != 512 || height != 512 || !GetFont().ok) {
        return false;
    }
    if (g_input_mode == 1) {
        return false; // a PlayStation controller: the game's own icons
    }
    if (g_input_mode == 2) {
        return PaintAtlasXbox(rgba);
    }
    if (!g_menu_mode) {
        return PaintAtlas(rgba, 0);
    }
    static std::mutex mutex;
    static uint32_t cached[2] = {0, 0};
    static std::vector<uint8_t> keys[2], mouse[2];
    std::lock_guard<std::mutex> lock{mutex};
    constexpr size_t Bytes = size_t(512) * 512 * 4;
    if (cached[slot] != g_base_generation || keys[slot].size() != Bytes) {
        keys[slot].assign(rgba, rgba + Bytes);
        mouse[slot].assign(rgba, rgba + Bytes);
        const bool a = PaintAtlas(keys[slot].data(), 0), b = PaintAtlas(mouse[slot].data(), 1);
        if (!a && !b) {
            return false;
        }
        cached[slot] = g_base_generation;
    }
    const std::vector<uint8_t>& keys_pic = keys[slot];
    const std::vector<uint8_t>& mouse_pic = mouse[slot];
    const int step = std::clamp(g_fade_step.load(), 0, FadeSteps);
    const float w = float(step) / float(FadeSteps);
    for (size_t i = 0; i < Bytes; i += 4) {
        const float a0 = float(keys_pic[i + 3]) / 255.0f, a1 = float(mouse_pic[i + 3]) / 255.0f;
        const float a = a0 * (1.0f - w) + a1 * w;
        rgba[i + 3] = uint8_t(a * 255.0f + 0.5f);
        for (int c = 0; c < 3; ++c) { // premultiplied blend, straight result
            const float p = float(keys_pic[i + size_t(c)]) * a0 * (1.0f - w) + float(mouse_pic[i + size_t(c)]) * a1 * w;
            rgba[i + size_t(c)] = a > 0.0f ? uint8_t(std::clamp(p / a, 0.0f, 255.0f) + 0.5f) : keys_pic[i + size_t(c)];
        }
    }
    return true;
}

// U3 keeps the shapes of the icons in a second 512x512 picture (their coverage in its red channel) and leaves the alpha of
// the atlas at 0: the icons are found, drawn and covered there. The keyboard and mouse pictures are painted on a copy with a
// coverage alpha (the atlas: any lit pixel; the shapes: their red), then the colors go back to the atlas and the new
// coverage to the shapes. Same for the Xbox glyphs (the round Menu button must cut its own shape, not the old pill's);
// a PlayStation controller keeps the game's pictures.
bool PatchU3Atlas(uint64_t hash, uint8_t* rgba, uint32_t width, uint32_t height) {
    const bool is_shapes = hash == U3AtlasShapesHash;
    if (g_input_mode == 1 || width != 512 || height != 512) { // a PlayStation controller: the game's own icons
        return false;
    }
    constexpr size_t Bytes = size_t(512) * 512 * 4;
    std::vector<uint8_t> work(rgba, rgba + Bytes);
    for (size_t i = 0; i < Bytes; i += 4) {
        const uint8_t lit = is_shapes ? work[i] : std::max({work[i], work[i + 1], work[i + 2]});
        work[i + 3] = (is_shapes ? lit : (lit > 24 ? 255 : 0));
    }
    if (!PatchU1Atlas(work.data(), width, height, is_shapes ? 1 : 0)) {
        return false;
    }
    for (size_t i = 0; i < Bytes; i += 4) {
        if (is_shapes) {
            rgba[i] = rgba[i + 1] = rgba[i + 2] = work[i + 3];
            continue;
        }
        const bool changed = rgba[i] != work[i] || rgba[i + 1] != work[i + 1] || rgba[i + 2] != work[i + 2];
        std::memcpy(&rgba[i], &work[i], 3);
        if (changed) {
            rgba[i + 3] = work[i + 3];
        }
    }
    return true;
}

void SetMenuMode(bool open) {
    if (g_menu_mode.exchange(open) != open) {
        ++g_phase_epoch; // the cycle starts again with the keys
        g_fade_step = 0;
        ++g_base_generation;
        ++g_menu_generation;
    }
}

void SetInputMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (g_input_mode.exchange(mode) != mode) {
        ++g_mode_generation;
        ++g_base_generation;
        ++g_menu_generation; // the atlas is painted again: keys and mouse, the game's own, or the Xbox buttons
        SetHintsMode(mode);
    }
}

int InputMode() {
    return g_input_mode;
}

uint32_t ModeGeneration() {
    return g_mode_generation;
}

void SetJetskiMode(bool on) {
    if (g_jetski_mode.exchange(on) != on) {
        ++g_base_generation;
        ++g_menu_generation; // the atlas is painted again with the jetski's keys (or without)
    }
}

void SetMenuAlt(bool shown) {
    if (g_menu_alt.exchange(shown) != shown) {
        ++g_base_generation;
        ++g_menu_generation;
    }
}

void TickMenuPhase() {
    static std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    static uint32_t epoch = 0;
    if (!g_menu_mode || g_input_mode != 0) { // (the mouse pastilles alternate with the keys only for the keyboard and mouse)
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (epoch != g_phase_epoch) {
        epoch = g_phase_epoch;
        start = now;
    }
    // keys (held), fade to the mouse, mouse (held), fade back to the keys...
    const double cycle = 2.0 * (HoldSeconds + FadeSeconds);
    const double t = std::fmod(std::chrono::duration<double>(now - start).count(), cycle);
    double mix;
    if (t < HoldSeconds) {
        mix = 0.0;
    } else if (t < HoldSeconds + FadeSeconds) {
        mix = (t - HoldSeconds) / FadeSeconds;
    } else if (t < 2.0 * HoldSeconds + FadeSeconds) {
        mix = 1.0;
    } else {
        mix = 1.0 - (t - 2.0 * HoldSeconds - FadeSeconds) / FadeSeconds;
    }
    mix = mix * mix * (3.0 - 2.0 * mix); // eases in and out
    const int step = int(std::lround(mix * FadeSteps));
    if (step != g_fade_step) {
        g_fade_step = step;
        ++g_menu_generation; // the atlas is blended again: the pastillas show a bit more of the other picture
    }
}

namespace {
std::atomic<int64_t> g_atlas_draw_ms{-1000000};
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void NoteAtlasDraw() {
    g_atlas_draw_ms.store(NowMs(), std::memory_order_relaxed);
}

} // namespace BbKeyIcons

/// For the pad code (src/runtime_pad.c): whether a button icon was drawn in the last `ms` milliseconds.
extern "C" int bbgpu_atlas_recent(int ms) {
    return BbKeyIcons::NowMs() - BbKeyIcons::g_atlas_draw_ms.load(std::memory_order_relaxed) <= ms ? 1 : 0;
}

namespace BbKeyIcons {

bool MenuMode() {
    return g_menu_mode;
}

uint32_t MenuGeneration() {
    return g_menu_generation;
}

void ForceRepaint() {
    ++g_base_generation;
    ++g_menu_generation;
}

namespace {

constexpr int MaxLevels = 12;

struct Levels {
    size_t offsets[MaxLevels];
    size_t total = 0;
    uint32_t count = 0;
};

/// Where the levels of a width x height texture lie, back to back from mip 0 (BC1 and BC4 both use
/// 8 bytes per 4x4 block).
Levels LayoutLevels(int width, int height, uint32_t levels) {
    Levels out;
    out.count = std::clamp(levels, 1u, uint32_t(MaxLevels));
    for (uint32_t m = 0; m < out.count; ++m) {
        const size_t bx = size_t(std::max((width >> m) / 4, 1)), by = size_t(std::max((height >> m) / 4, 1));
        out.offsets[m] = out.total;
        out.total += bx * by * 8;
    }
    return out;
}

enum class Codec { Bc1, Bc4 };

/// Decodes a width x height BC4 level (one channel) into RGBA8: the value in red, green and blue.
void DecodeBc4(const uint8_t* data, int width, int height, uint8_t* rgba) {
    const int blocks_x = std::max(width / 4, 1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* block = data + (size_t(y / 4) * size_t(blocks_x) + size_t(x / 4)) * 8;
            const int r0 = block[0], r1 = block[1];
            int palette[8] = {r0, r1};
            if (r0 > r1) {
                for (int i = 1; i <= 6; ++i) {
                    palette[1 + i] = ((7 - i) * r0 + i * r1) / 7;
                }
            } else {
                for (int i = 1; i <= 4; ++i) {
                    palette[1 + i] = ((5 - i) * r0 + i * r1) / 5;
                }
                palette[6] = 0;
                palette[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; ++i) {
                bits |= uint64_t(block[2 + i]) << (8 * i);
            }
            const int v = palette[(bits >> (3 * ((y & 3) * 4 + (x & 3)))) & 7];
            uint8_t* p = rgba + (size_t(y) * size_t(width) + size_t(x)) * 4;
            p[0] = p[1] = p[2] = uint8_t(v);
            p[3] = 255;
        }
    }
}

/// Encodes the red channel of the 4x4 pixels at (bx*4, by*4) (edges clamped) as one BC4 block.
void EncodeBc4Block(const uint8_t* rgba, int width, int height, int bx, int by, uint8_t out[8]) {
    int v[16], lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) {
        const int x = std::min(bx * 4 + (i & 3), width - 1), y = std::min(by * 4 + (i >> 2), height - 1);
        v[i] = rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
    }
    int palette[8];
    out[0] = uint8_t(hi);
    out[1] = uint8_t(lo);
    palette[0] = hi;
    palette[1] = lo;
    for (int i = 1; i <= 6; ++i) {
        palette[1 + i] = ((7 - i) * hi + i * lo) / 7;
    }
    uint64_t bits = 0;
    if (hi != lo) { // hi > lo: the 8 value mode
        for (int i = 0; i < 16; ++i) {
            int pick = 0, nearest = 1 << 30;
            for (int k = 0; k < 8; ++k) {
                const int d = std::abs(v[i] - palette[k]);
                if (d < nearest) {
                    nearest = d;
                    pick = k;
                }
            }
            bits |= uint64_t(pick) << (3 * i);
        }
    }
    for (int i = 0; i < 6; ++i) {
        out[2 + i] = uint8_t(bits >> (8 * i));
    }
}

/// Writes the mip chain of a patched mip 0 (width x height RGBA8) into data: the blocks that touch the
/// box [x0, x1] x [y0, y1] (mip 0 pixels) are re-encoded, the others stay as they are.
void ReencodeChain(std::vector<uint8_t> level, int width, int height, Codec codec, const Levels& layout,
                   uint8_t* data, float x0, float y0, float x1, float y1) {
    for (uint32_t m = 0; m < layout.count; ++m) {
        if (m > 0) {
            const int nw = std::max(width / 2, 1), nh = std::max(height / 2, 1);
            std::vector<uint8_t> next(size_t(nw) * size_t(nh) * 4);
            for (int y = 0; y < nh; ++y) {
                for (int x = 0; x < nw; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        int sum = 0;
                        for (int dy = 0; dy < 2; ++dy) {
                            for (int dx = 0; dx < 2; ++dx) {
                                const int sx = std::min(x * 2 + dx, width - 1), sy = std::min(y * 2 + dy, height - 1);
                                sum += level[(size_t(sy) * size_t(width) + size_t(sx)) * 4 + size_t(c)];
                            }
                        }
                        next[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(c)] = uint8_t((sum + 2) / 4);
                    }
                }
            }
            level.swap(next);
            width = nw;
            height = nh;
        }
        const float k = float(1 << m);
        const int blocks = std::max(width / 4, 1);
        for (int by = 0; by < std::max(height / 4, 1); ++by) {
            for (int bx = 0; bx < blocks; ++bx) {
                if (float(bx * 4 + 4) > x0 / k && float(bx * 4) < x1 / k && float(by * 4 + 4) > y0 / k &&
                    float(by * 4) < y1 / k) {
                    uint8_t* block = data + layout.offsets[m] + (size_t(by) * size_t(blocks) + size_t(bx)) * 8;
                    if (codec == Codec::Bc1) {
                        EncodeBc1Block(level.data(), width, height, bx, by, block);
                    } else {
                        EncodeBc4Block(level.data(), width, height, bx, by, block);
                    }
                }
            }
        }
    }
}

/// Bilinear sample of a straight-alpha RGBA8 image (size x size); outside it is transparent.
void Sample(const std::vector<uint8_t>& img, int size, float u, float v, float out[4]) {
    const float fu = u - 0.5f, fv = v - 0.5f;
    const int x0 = int(std::floor(fu)), y0 = int(std::floor(fv));
    const float tx = fu - float(x0), ty = fv - float(y0);
    float sum[3] = {0, 0, 0}, alpha = 0.0f;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int x = x0 + i, y = y0 + j;
            const float w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
            if (x < 0 || y < 0 || x >= size || y >= size || w <= 0.0f) {
                continue;
            }
            const uint8_t* p = &img[(size_t(y) * size_t(size) + size_t(x)) * 4];
            const float a = float(p[3]) / 255.0f;
            sum[0] += float(p[0]) * a * w;
            sum[1] += float(p[1]) * a * w;
            sum[2] += float(p[2]) * a * w;
            alpha += a * w;
        }
    }
    out[3] = alpha;
    for (int c = 0; c < 3; ++c) {
        out[c] = alpha > 0.0f ? sum[c] / alpha : 0.0f;
    }
}

std::string BindingLabel(const Bindings& b, const char* action) {
    const int a = ActionIndex(action);
    return a >= 0 ? ShortName(b.keys[a]) : std::string{};
}

// ---- The big "interact" button: the key alone, the halo the shape of the key -------------------

/// A HUD button that is a texture of its own, stored upside down, drawn with a soft mask (the
/// halo: the background of the texture seen through the mask). The disc of the button and the
/// round halo become the key and an orange halo of its shape, squashed like the button is in the
/// frames of its press animation. Frames can share a mask.
struct KeyTarget {
    uint64_t hash;      // XXH3-64 of the guest bytes, as the texture dump names the files
    uint64_t mask_hash; // the soft mask it is drawn with (BC4, same size): the halo
    uint64_t hard_hash; // the hard mask of the disc, drawn with the same colors; 0 when the frame has none
    int width, height;
    float cx, cy;       // center of the button in the stored texture
    float rx, ry;       // radii of the button (the squash)
    const char* action; // the key to draw
    float depth;        // extra thickness of the key, for the frame that shows the side of the button
    float box_w, box_h; // room the key is drawn in, on the sheet (before the squash)
    bool overlay;       // the texture has more than the button (the journal): only the button box is redrawn,
                        // by a key that fills it, and its mask is left alone
    int bx0, by0, bx1, by1; // that box, in the stored texture
    uint64_t halo2_hash, disc2_hash; // a second halo mask and a second disc mask, for textures drawn with more
    float key_scale = 1.0f; // size of the key in its box: the game draws these textures at different sizes on
                        // screen, this brings all the prompts to the same size (measured in game)
    bool soft_disc = false; // the hard (disc) masks carry the halo too: in that state the game draws no soft
                            // halo mask (the "V", the pressed "E"), so the key would have no glow
    bool pulse = false;     // and its halo blinks, as the one of the flat "E" does (the game draws it still):
                            // the mask is repainted with the phase of the blink, see PulseFactor
    bool bc1_masks = false; // U3: the masks are BC1 pictures (grey), not BC4
    int mask_w = 0, mask_h = 0; // U3: the masks are smaller than the color (0: the same size)
    int halo2_w = 0;            // U3: the size of the second halo mask (square; 0: that of the first)
};

int MaskWidth(const KeyTarget& t) { return t.mask_w ? t.mask_w : t.width; }
int MaskHeight(const KeyTarget& t) { return t.mask_h ? t.mask_h : t.height; }

// On-screen size of the prompts, measured in game: the small "E" of the atlas (hold) is 77 pixels
// wide, the big flat/animated "E" 100, the "V" of the d-pad texture 110 (1920 wide screen). All are
// brought to about 88 (the atlas icons through AtlasScale).
constexpr float BigKeyScale = 0.88f;
constexpr float DpadKeyScale = 0.80f;
// The two press frames of the big button (also the "hold" prompt, with its text just above): the key is
// as wide as the flat one but squashed in height as the game does (rx 98.5, ry 59), seen from the side:
// a thick side when released, almost none when pressed. The game's own squash was 60 pixels wide.
constexpr float FrameKeyScale = 1.25f;

constexpr KeyTarget KeyTargets[] = {
    // The big green "interact" button, flat.
    {0xac68a6ce1c1540adull, 0x1d9e47b4448b1f34ull, 0xa893d1356e196b09ull, 256, 256, 128.5f, 126.5f, 70.5f, 70.5f, "triangle", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, BigKeyScale},
    // The same button squashed, two frames of its press animation (the second shows its side). The
    // two share their mask, so the key sits at the same place in both: its halo is then the same.
    {0x02c213a5ba6c6d49ull, 0xe08524b4989bd16full, 0x840d0ecfbfb0ac7bull, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "triangle", -16.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, FrameKeyScale, true},
    {0x4ae942741bdae0d1ull, 0xe08524b4989bd16full, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "triangle", 36.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, FrameKeyScale},
    // The d-pad of the HUD with the up arm lit: the arms go, the key of "up" stays.
    {0x31c161abb7d34b52ull, 0xd8440642acdcf099ull, 0x371acfb9c43daff0ull, 128, 128, 64.0f, 63.0f, 40.0f, 40.0f, "up", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0x0bc8711fd40b8604ull, 0x29d7777ccc66f17full, DpadKeyScale, true},
    // The journal with the touchpad button on it: the touchpad button becomes the key, the journal stays.
    // The key fills the box of the button, so the halo of the original mask, left as it is, fits it.
    {0x8b9946c0528a3d88ull, 0, 0, 512, 256, 216.0f, 142.0f, 70.5f, 70.5f, "touchpad", 0.0f, 169.0f, 105.0f, true, 135, 93, 298, 192},
    // Uncharted 2: the same pictures with other hashes (found by the texture dump of U2). The big button: the flat one
    // (soft halo, hard disc), then its two press frames, each with a mask of its own (the glow is in the mask) ...
    {0xc6e03b00d234a7cdull, 0x28097479d1e0fdd3ull, 0xa5f8293a037fa06eull, 256, 256, 128.5f, 126.5f, 70.5f, 70.5f, "triangle", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, BigKeyScale},
    {0xf2829d9b5230cb2bull, 0, 0xe5ffdae4c88ca07dull, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "triangle", -16.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, FrameKeyScale, true},
    {0x07fabd1a89bf9ddbull, 0x96eef6ee07602555ull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "triangle", 36.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, FrameKeyScale},
    // ... and the d-pad, 256x256 here (128x128 in U1): twice the radius.
    {0x2e528a3cffaf0059ull, 0x54c5ae40a5cb6d4eull, 0xbe699cb5e4c57461ull, 256, 256, 128.0f, 126.0f, 80.0f, 80.0f, "up", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, DpadKeyScale, true},
    // ... and the journal (512x256, button in the same place as in U1; its soft halo is a5557ee8c3c38870): the book is grey here.
    {0x10ee44c6ce616ee4ull, 0, 0, 512, 256, 216.0f, 142.0f, 70.5f, 70.5f, "touchpad", 0.0f, 169.0f, 105.0f, true, 135, 93, 298, 192},
    // Uncharted 3: the same pictures again (found by the texture dump of U3). The colors are sRGB and the masks BC1 here.
    // The big button, flat (the interact "triangle"): same geometry as U1/U2, a soft halo mask and a hard disc mask.
    {0x7e16e7522ff5748full, 0x6119b944b20126d7ull, 0xbe8319885328ef21ull, 256, 256, 128.5f, 126.5f, 70.5f, 70.5f, "triangle", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, BigKeyScale, false, false, true},
    // The same buttons flat, for the other face buttons (the masks are the triangle's: same shape).
    {0x7d3273d9fcd75b58ull, 0x6119b944b20126d7ull, 0xbe8319885328ef21ull, 256, 256, 128.5f, 126.5f, 70.5f, 70.5f, "cross", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, BigKeyScale, false, false, true},
    {0x0f2aa3306cf47da1ull, 0x6119b944b20126d7ull, 0xbe8319885328ef21ull, 256, 256, 128.5f, 126.5f, 70.5f, 70.5f, "circle", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, BigKeyScale, false, false, true},
    // The quick-time events: the press frames (the first shows the side of the button, the second is pressed), 512x512, the
    // same for the four face buttons but for the color and the symbol. Their masks (the shape and its glow) are shared by
    // the buttons, by shape: the thick one and the flat one, each at 512x512 and at half the size (the game draws either).
    {0xbda57268c785fa75ull, 0xe56cc1b653d4967bull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "circle", 36.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0xae75f527bc070bf0ull, 0, FrameKeyScale, false, false, true, 0, 0, 256},
    {0xd281f68d0ca6b604ull, 0x6e70a93320e446a4ull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "circle", -16.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0x35f9e8ad273591c5ull, 0, FrameKeyScale, false, false, true, 0, 0, 256},
    {0xde6288d2b457da2aull, 0xe56cc1b653d4967bull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "triangle", 36.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0xae75f527bc070bf0ull, 0, FrameKeyScale, false, false, true, 0, 0, 256},
    {0xca555bf12dd7402bull, 0x6e70a93320e446a4ull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "triangle", -16.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0x35f9e8ad273591c5ull, 0, FrameKeyScale, false, false, true, 0, 0, 256},
    {0x2e8f1a37800214a7ull, 0xe56cc1b653d4967bull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "square", 36.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0xae75f527bc070bf0ull, 0, FrameKeyScale, false, false, true, 0, 0, 256},
    {0xcdde88dd024eeabeull, 0x6e70a93320e446a4ull, 0, 512, 512, 256.5f, 250.0f, 98.5f, 59.0f, "square", -16.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0x35f9e8ad273591c5ull, 0, FrameKeyScale, false, false, true, 0, 0, 256},
    // "Push the left stick forward": three frames of the stick (256x128, tan background, stored upside down), each with a
    // mask of its own. The stick goes, the key of "stick up" takes its place (centered on the mask of the frame). The
    // controller keeps the stick: it is the same picture for PlayStation and Xbox.
    {0xd5517ab261f4e90cull, 0x25d2534cfad65b5dull, 0, 256, 128, 129.5f, 71.5f, 38.0f, 38.0f, "ls_up", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, 1.0f, false, false, true},
    {0xf3dfe70ef1fde14bull, 0x412777c1c2c78246ull, 0, 256, 128, 129.5f, 58.0f, 38.0f, 38.0f, "ls_up", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, 1.0f, false, false, true},
    {0x2f20dd1ea3c43f82ull, 0x08a9ae4743d4fb58ull, 0, 256, 128, 129.5f, 76.0f, 38.0f, 38.0f, "ls_up", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, 1.0f, false, false, true},
    // The d-pad of the HUD, up arm lit (stored upside down): soft mask with the glow, hard mask of the pad.
    {0x75acc5a4dbcdc36full, 0x90a2254d1a6d063cull, 0xe7ee4ecd41676109ull, 256, 256, 128.0f, 126.0f, 80.0f, 80.0f, "up", 0.0f, 170.0f, 150.0f, false, 0, 0, 0, 0, 0, 0, DpadKeyScale, true, false, true},
    // The journal (512x256, the button where it is in U1 and U2; its mask is left as it is).
    {0x47dc0f9c27f1919bull, 0, 0, 512, 256, 216.0f, 142.0f, 70.5f, 70.5f, "touchpad", 0.0f, 169.0f, 105.0f, true, 135, 93, 298, 192, 0, 0, 1.0f, false, false, true},
};

enum class Role { Color, Halo, Disc };

// The halo mask of the d-pad "V" is also a particle sprite of the game (water spray, mist...: about a thousand draws a
// second near a waterfall). It is left alone: repainted it gave the keys' shape to those particles, and its draws would
// be moved down like the prompt's.
constexpr uint64_t SharedParticleMask = 0xd8440642acdcf099ull;

const KeyTarget* FindKeyTarget(uint64_t hash, Role* role) {
    if (hash == SharedParticleMask) {
        return nullptr;
    }
    for (const KeyTarget& t : KeyTargets) {
        if (t.hash == hash) {
            *role = Role::Color;
            return &t;
        }
        if (t.mask_hash == hash || (t.halo2_hash && t.halo2_hash == hash)) {
            *role = Role::Halo;
            return &t;
        }
        if ((t.hard_hash && t.hard_hash == hash) || (t.disc2_hash && t.disc2_hash == hash)) {
            *role = Role::Disc;
            return &t;
        }
    }
    return nullptr;
}

/// Whether a mask (by its hash) is one of those the target is drawn with.
bool UsesMask(const KeyTarget& t, uint64_t hash) {
    return t.mask_hash == hash || t.halo2_hash == hash || t.hard_hash == hash || t.disc2_hash == hash;
}

/// The key and its halo of one target, upright and flat on a 256x256 sheet.
/// The two press frames of the big button (their depth is not 0) are drawn with the pressed and released key templates,
/// at the size of the texture, not squashed from a flat key: a 512x512 sheet with the key centered on (256, 256).
bool IsPressFrame(const KeyTarget& t) {
    return t.depth != 0.0f;
}

std::vector<uint8_t> KeySheet(const KeyTarget& t, const std::string& label, const Font& font) {
    if (IsPressFrame(t)) {
        std::vector<uint8_t> frame(512 * 512 * 4, 0);
        DrawKeyState(frame.data(), 512, 256.0f, 256.0f, t.box_w * t.key_scale * (t.rx / 70.5f), label,
                     t.depth > 0.0f ? KeyState::Released : KeyState::Pressed, false);
        return frame;
    }
    std::vector<uint8_t> sheet(256 * 256 * 4, 0);
    const float s = t.overlay ? 1.0f : t.key_scale;
    DrawSymbol(sheet.data(), 256, 128.0f, 128.0f, t.box_w * s, t.box_h * s, label, font, Halo(t.action), t.depth * s,
               t.overlay);
    return sheet;
}

/// How bright the blinking halos are now, 0.4 to 1, one beat a second (the halo of the flat "E" blinks).
float PulseFactor() {
    using Clock = std::chrono::steady_clock;
    const double t = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    return 0.4f + 0.6f * float(0.5 + 0.5 * std::sin(t * 6.2831853));
}

/// The sheet seen at the texture pixel (x, y) of the target (squashed, upside down).
void KeyAt(const KeyTarget& t, const std::vector<uint8_t>& sheet, int x, int y, float out[4]) {
    if (IsPressFrame(t)) {
        Sample(sheet, 512, 256.0f + (float(x) + 0.5f - t.cx), 256.0f - (float(y) + 0.5f - t.cy), out);
        return;
    }
    const float sx = t.rx / 70.5f, sy = t.ry / 70.5f; // texture pixels per sheet pixel
    Sample(sheet, 256, 128.0f + (float(x) + 0.5f - t.cx) / sx, 128.0f - (float(y) + 0.5f - t.cy) / sy, out);
}

bool PatchKeyTarget(const KeyTarget& t, Role role, uint64_t hash, uint8_t* data, size_t size, uint32_t levels) {
    const bool is_mask = role != Role::Color;
    const bool second_mask = is_mask && t.halo2_hash && t.halo2_w && hash == t.halo2_hash;
    const int W = second_mask ? t.halo2_w : is_mask ? MaskWidth(t) : t.width; // of the texture patched
    const int H = second_mask ? t.halo2_w : is_mask ? MaskHeight(t) : t.height;
    const int scale = std::max(t.width / W, 1); // texture pixels per pixel of the mask
    const Levels layout = LayoutLevels(W, H, levels);
    const Font& font = GetFont();
    if (size < layout.total || !font.ok) {
        return false;
    }
    const Bindings bindings = LoadBindings();
    const auto label_of = [&](const KeyTarget& k) { return ArrowLabel(font, BindingLabel(bindings, k.action)); };
    const std::string label = label_of(t);
    if (label.empty()) {
        return false;
    }
    // The pixels of a frame that the key changes: all of them, or the box of the old button.
    const auto in_box = [&](int x, int y, int margin) {
        return !t.overlay || (x >= t.bx0 - margin && x < t.bx1 + margin && y >= t.by0 - margin && y < t.by1 + margin);
    };
    std::vector<uint8_t> level(size_t(W) * size_t(H) * 4);
    if (is_mask) {
        if (t.bc1_masks) {
            DecodeBc1(data, W, H, level.data());
        } else {
            DecodeBc4(data, W, H, level.data());
        }
        // The key and its halo; frames that share the mask all show: their shapes are added up.
        std::vector<std::pair<const KeyTarget*, std::vector<uint8_t>>> sheets;
        for (const KeyTarget& other : KeyTargets) {
            if (UsesMask(other, hash) && other.width == t.width && other.height == t.height) {
                const std::string other_label = label_of(other);
                if (!other_label.empty()) {
                    sheets.emplace_back(&other, KeySheet(other, other_label, font));
                }
            }
        }
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (!in_box(x * scale, y * scale, 14)) {
                    continue;
                }
                float alpha = 0.0f;
                for (const auto& [other, sheet] : sheets) {
                    float s[4] = {0, 0, 0, 0};
                    for (int j = 0; j < scale; ++j) { // a smaller mask: the mean of the texture pixels it covers
                        for (int i = 0; i < scale; ++i) {
                            float one[4];
                            KeyAt(*other, sheet, x * scale + i, y * scale + j, one);
                            for (int c = 0; c < 4; ++c) {
                                s[c] += one[c] / float(scale * scale);
                            }
                        }
                    }
                    // The halo mask: the key and its halo. The disc mask: the key alone.
                    float a = role == Role::Halo || other->soft_disc
                                        ? s[3]
                                        : std::clamp((s[3] - 0.55f) / 0.4f, 0.0f, 1.0f);
                    if (other->pulse && a < 0.95f) { // the halo only, the key stays
                        a *= PulseFactor();
                    }
                    alpha = std::max(alpha, a);
                }
                uint8_t* p = &level[(size_t(y) * size_t(W) + size_t(x)) * 4];
                p[0] = p[1] = p[2] = uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    } else {
        DecodeBc1(data, t.width, t.height, level.data());
        // The background, the color of the halo, becomes orange. A texture that is only the button
        // loses its old disc as well; one with more in it (the journal) keeps it, but for the box
        // of the button.
        const uint8_t bg[3] = {level[0], level[1], level[2]};
        for (int y = 0; y < t.height; ++y) {
            for (int x = 0; x < t.width; ++x) {
                uint8_t* p = &level[(size_t(y) * size_t(t.width) + size_t(x)) * 4];
                const bool is_bg = std::abs(p[0] - bg[0]) + std::abs(p[1] - bg[1]) + std::abs(p[2] - bg[2]) < 70;
                if (t.overlay && in_box(x, y, 0)) { // the old button: the rim of the key shows at its corners
                    p[0] = uint8_t(TanEdge.r);
                    p[1] = uint8_t(TanEdge.g);
                    p[2] = uint8_t(TanEdge.b);
                } else if (!t.overlay || is_bg) {
                    p[0] = uint8_t(Orange.r);
                    p[1] = uint8_t(Orange.g);
                    p[2] = uint8_t(Orange.b);
                }
            }
        }
        const std::vector<uint8_t> sheet = KeySheet(t, label, font);
        for (int y = 0; y < t.height; ++y) {
            for (int x = 0; x < t.width; ++x) {
                if (!in_box(x, y, 14)) {
                    continue;
                }
                float s[4];
                KeyAt(t, sheet, x, y, s);
                if (s[3] > 0.0f) {
                    uint8_t* p = &level[(size_t(y) * size_t(t.width) + size_t(x)) * 4];
                    const float a = std::min(s[3], 1.0f);
                    for (int c = 0; c < 3; ++c) {
                        p[c] = uint8_t(std::clamp(s[c] * a + float(p[c]) * (1.0f - a), 0.0f, 255.0f) + 0.5f);
                    }
                }
            }
        }
    }
    ReencodeChain(std::move(level), W, H, is_mask && !t.bc1_masks ? Codec::Bc4 : Codec::Bc1, layout, data, 0.0f, 0.0f,
                  float(W), float(H));
    std::printf("KeyIcons: key %s redrawn (%s=%s, %dx%d, %u mip levels)\n",
                role == Role::Color ? "color" : role == Role::Halo ? "halo mask" : "disc mask", t.action,
                label.c_str(), W, H, layout.count);
    return true;
}

// ---- The journal prompt: the book with the View button, not the touchpad's wide one ------------------------------------------
// The game's picture is a book half hidden under a wide rounded button; two textures make it: the colors (BC1) and a soft mask (BC4,
// the book and the button together). Xbox: the hidden part of the book is filled in, the button goes, a round View button takes
// its place, and the mask follows the new shape. All the coordinates are the textures' own (stored upside down).
constexpr uint64_t JournalMaskHash = 0x098d0fef76921395ull;
constexpr uint64_t JournalMaskHashU2 = 0xa5557ee8c3c38870ull; // the same soft mask in U2 (the drawn book has U1's outline: same mask picture)
constexpr uint64_t JournalMaskHashU3 = 0xddc72d271d3f1b61ull; // U3: the same mask as a BC1 picture (the Xbox one drawn by hand goes in)
constexpr int JournalHullCount = 17;
constexpr float JournalHull[JournalHullCount][2] = {{20, 100}, {30, 89},  {57, 77},   {86, 66},   {116, 55}, {142, 46}, {156, 44}, {167, 54}, {177, 74},
                                                    {232, 197}, {229, 213}, {203, 220}, {139, 232}, {116, 234}, {103, 217}, {40, 131}, {24, 109}};
constexpr int JournalRect[4] = {131, 89, 302, 196};                                        // the old button, with a margin
constexpr float JournalButtonX = 232.0f, JournalButtonY = 143.0f, JournalButtonR = 40.0f; // the round one

/// Whether a point is on the book (the outline is convex).
bool XInJournalBook(float x, float y) {
    int sign = 0;
    for (int i = 0; i < JournalHullCount; ++i) {
        const float ax = JournalHull[i][0], ay = JournalHull[i][1];
        const float bx = JournalHull[(i + 1) % JournalHullCount][0], by = JournalHull[(i + 1) % JournalHullCount][1];
        const float c = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        if (c > 0.0f) {
            if (sign < 0) {
                return false;
            }
            sign = 1;
        } else if (c < 0.0f) {
            if (sign > 0) {
                return false;
            }
            sign = -1;
        }
    }
    return true;
}

bool XJournalColor(const XPic& pic, const Font& font) {
    const int x0 = JournalRect[0], y0 = JournalRect[1], x1 = JournalRect[2], y1 = JournalRect[3];
    const uint8_t bg[3] = {pic.px[0], pic.px[1], pic.px[2]};
    std::vector<uint8_t> hidden(size_t(pic.w) * size_t(pic.h), 0); // the part of the book under the old button
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (XInJournalBook(float(x) + 0.5f, float(y) + 0.5f)) {
                hidden[size_t(y) * size_t(pic.w) + size_t(x)] = 1;
            } else {
                uint8_t* p = XAt(pic, x, y);
                p[0] = bg[0];
                p[1] = bg[1];
                p[2] = bg[2];
                p[3] = 255;
            }
        }
    }
    // each row of the hidden part takes the color of the book just left of it (smoothed over the rows), with a little grain
    std::vector<float> left(size_t(y1 - y0) * 3, 0.0f);
    std::vector<int> run_start(size_t(y1 - y0), -1);
    for (int y = y0; y < y1; ++y) {
        int xa = -1;
        for (int x = x0; x < x1 && xa < 0; ++x) {
            xa = hidden[size_t(y) * size_t(pic.w) + size_t(x)] ? x : -1;
        }
        run_start[size_t(y - y0)] = xa;
        if (xa >= 0) {
            for (int k = 1; k <= 6; ++k) {
                const uint8_t* p = XAt(pic, std::max(xa - k, 0), y);
                for (int c = 0; c < 3; ++c) {
                    left[size_t(y - y0) * 3 + size_t(c)] += float(p[c]) / 6.0f;
                }
            }
        }
    }
    for (int y = y0; y < y1; ++y) {
        const int xa = run_start[size_t(y - y0)];
        if (xa < 0) {
            continue;
        }
        float mean[3] = {0, 0, 0};
        int n = 0;
        for (int k = -4; k <= 4; ++k) {
            const int yy = y + k;
            if (yy >= y0 && yy < y1 && run_start[size_t(yy - y0)] >= 0) {
                for (int c = 0; c < 3; ++c) {
                    mean[c] += left[size_t(yy - y0) * 3 + size_t(c)];
                }
                ++n;
            }
        }
        for (int x = xa; x < x1; ++x) {
            if (!hidden[size_t(y) * size_t(pic.w) + size_t(x)]) {
                continue;
            }
            uint8_t* p = XAt(pic, x, y);
            const int mirror = x0 - 1 - (x - x0); // the cover goes on as its reflection in the old button's left edge
            if (mirror >= 0 && XInJournalBook(float(mirror) + 0.5f, float(y) + 0.5f)) {
                const uint8_t* q = XAt(pic, mirror, y);
                const bool is_bg = std::abs(int(q[0]) - int(bg[0])) + std::abs(int(q[1]) - int(bg[1])) + std::abs(int(q[2]) - int(bg[2])) < 60;
                if (is_bg) { // the outline is a little wider than the book at its corners: nothing there
                    std::memcpy(p, bg, 3);
                } else {
                    std::memcpy(p, q, 4);
                }
                continue;
            }
            uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            const float grain = float(int((h >> 16) & 15u) - 8) * 0.6f;
            for (int c = 0; c < 3; ++c) {
                p[c] = uint8_t(std::clamp(mean[c] / float(n) + grain, 0.0f, 255.0f) + 0.5f);
            }
        }
    }
    XRoundButton(pic, font, JournalButtonX, JournalButtonY, JournalButtonR, XGlyph::View, nullptr, {200.0f, 178.0f, 140.0f}, false, true, false);
    return true;
}

/// The soft mask of the journal: the book and the round button, instead of the book and the wide one.
bool XJournalMask(uint8_t* data, size_t size, uint32_t levels, bool bc1 = false) {
    constexpr int W = 512, H = 256;
    const Levels layout = LayoutLevels(W, H, levels);
    if (size < layout.total) {
        return false;
    }
    std::vector<uint8_t> level(size_t(W) * size_t(H) * 4);
    XAsset asset;
    if (XLoadAsset(5, asset) && asset.w == W && asset.h == H && asset.ch == 1) { // the mask drawn by hand
        for (size_t i = 0, n = size_t(W) * size_t(H); i < n; ++i) {
            level[i * 4] = level[i * 4 + 1] = level[i * 4 + 2] = asset.data[i];
            level[i * 4 + 3] = 255;
        }
        ReencodeChain(std::move(level), W, H, bc1 ? Codec::Bc1 : Codec::Bc4, layout, data, 0.0f, 0.0f, float(W), float(H));
        std::printf("KeyIcons: journal mask: the Xbox picture (%u mip levels)\n", layout.count);
        return true;
    }
    if (bc1) {
        return false; // U3: only the picture drawn by hand
    }
    DecodeBc4(data, W, H, level.data());
    constexpr int zx0 = 90, zy0 = 50, zx1 = 346, zy1 = 236, zw = zx1 - zx0, zh = zy1 - zy0;
    std::vector<float> shape(size_t(zw) * size_t(zh), 0.0f), tmp(shape.size());
    for (int y = 0; y < zh; ++y) {
        for (int x = 0; x < zw; ++x) {
            const float px = float(zx0 + x) + 0.5f, py = float(zy0 + y) + 0.5f;
            const bool on = XInJournalBook(px, py) || std::hypot(px - JournalButtonX, py - JournalButtonY) <= JournalButtonR + 4.0f;
            shape[size_t(y) * size_t(zw) + size_t(x)] = on ? 1.0f : 0.0f;
        }
    }
    constexpr int R = 6; // soft edge, as the game's mask has
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < zh; ++y) {
            for (int x = 0; x < zw; ++x) {
                float sum = 0.0f;
                for (int k = -R; k <= R; ++k) {
                    const int xx = std::clamp(x + k, 0, zw - 1);
                    sum += shape[size_t(y) * size_t(zw) + size_t(xx)];
                }
                tmp[size_t(y) * size_t(zw) + size_t(x)] = sum / float(2 * R + 1);
            }
        }
        for (int y = 0; y < zh; ++y) {
            for (int x = 0; x < zw; ++x) {
                float sum = 0.0f;
                for (int k = -R; k <= R; ++k) {
                    const int yy = std::clamp(y + k, 0, zh - 1);
                    sum += tmp[size_t(yy) * size_t(zw) + size_t(x)];
                }
                shape[size_t(y) * size_t(zw) + size_t(x)] = sum / float(2 * R + 1);
            }
        }
    }
    // only around the old button; at the edge of that zone the game's own mask takes over again
    for (int y = zy0; y < zy1; ++y) {
        for (int x = zx0; x < zx1; ++x) {
            const int dx = std::max({JournalRect[0] - 14 - x, x - (JournalRect[2] + 14), 0});
            const int dy = std::max({JournalRect[1] - 14 - y, y - (JournalRect[3] + 14), 0});
            const float w = 1.0f - std::clamp(float(std::max(dx, dy)) / 6.0f, 0.0f, 1.0f);
            if (w <= 0.0f) {
                continue;
            }
            uint8_t* p = &level[(size_t(y) * W + size_t(x)) * 4];
            const float mixed = float(p[0]) * (1.0f - w) + shape[size_t(y - zy0) * size_t(zw) + size_t(x - zx0)] * 255.0f * w;
            p[0] = p[1] = p[2] = uint8_t(std::clamp(mixed, 0.0f, 255.0f) + 0.5f);
        }
    }
    ReencodeChain(std::move(level), W, H, Codec::Bc4, layout, data, 0.0f, 0.0f, float(W), float(H));
    std::printf("KeyIcons: journal mask redrawn for the round button (%u mip levels)\n", layout.count);
    return true;
}

// ---- The controller diagram of the Controls screen (the pad with its callout lines) --------------------------------------------
// A 2048x1024 RGBA texture (the picture) and a 1024x512 BC1 texture (the plate and the silhouette that the screen draws as a glow):
// the Xbox pad drawn by hand and the mask made from it.
constexpr uint64_t SchemaHash = 0xcf2985e2610ea662ull;
constexpr uint64_t SchemaMaskHash = 0x16bc917ddadab17cull;
constexpr uint64_t SchemaMaskHashU2 = 0xb1f24fd1dd0f8d61ull; // U2 draws the same picture (same hash) with a mask of its own (PlayStation pad + touchpad
                                                             // callout): it gets U1's Xbox mask, the validated one
constexpr uint64_t SchemaMaskHashU3 = 0xff11e0c8da8cf736ull; // U3: same picture, mask of its own (PlayStation pad + touchpad), same treatment

bool XSchemaColor(uint8_t* data, size_t size) {
    constexpr int W = 2048, H = 1024;
    XAsset asset;
    if (!XLoadAsset(6, asset) || asset.w != W || asset.h != H || asset.ch != 3 || size < size_t(W) * size_t(H) * 4) {
        return false;
    }
    for (size_t i = 0, n = size_t(W) * size_t(H); i < n; ++i) {
        std::memcpy(&data[i * 4], &asset.data[i * 3], 3);
        data[i * 4 + 3] = 255;
    }
    std::printf("KeyIcons: controller diagram: the Xbox picture\n");
    return true;
}

bool XSchemaMask(uint8_t* data, size_t size, uint32_t levels) {
    constexpr int W = 1024, H = 512;
    XAsset asset;
    const Levels layout = LayoutLevels(W, H, levels);
    if (!XLoadAsset(7, asset) || asset.w != W || asset.h != H || asset.ch != 1 || size < layout.total) {
        return false;
    }
    std::vector<uint8_t> level(size_t(W) * size_t(H) * 4);
    for (size_t i = 0, n = size_t(W) * size_t(H); i < n; ++i) {
        level[i * 4] = level[i * 4 + 1] = level[i * 4 + 2] = asset.data[i];
        level[i * 4 + 3] = 255;
    }
    ReencodeChain(std::move(level), W, H, Codec::Bc1, layout, data, 0.0f, 0.0f, float(W), float(H));
    std::printf("KeyIcons: controller diagram mask: the Xbox picture (%u mip levels)\n", layout.count);
    return true;
}

/// Xbox: the "interact" button (its three frames) and the journal's touchpad button keep the game's pictures; only the glyph changes
/// (the triangle becomes a Y in the color it had, the hand becomes View). The masks (the shape and the halo) are not touched.
bool PatchXboxHud(const KeyTarget& t, Role role, uint8_t* data, size_t size, uint32_t levels) {
    static const Color YellowY{248.0f, 204.0f, 44.0f};
    const bool triangle = !std::strcmp(t.action, "triangle"), journal = !std::strcmp(t.action, "touchpad");
    // U3's other face buttons (A, B, X): only the pictures drawn by hand
    const bool circle = !std::strcmp(t.action, "circle") || !std::strcmp(t.action, "cross") || !std::strcmp(t.action, "square");
    if (role != Role::Color || (!triangle && !journal && !circle)) {
        return false;
    }
    const Levels layout = LayoutLevels(t.width, t.height, levels);
    const Font& font = GetFont();
    if (size < layout.total || !font.ok) {
        return false;
    }
    std::vector<uint8_t> level(size_t(t.width) * size_t(t.height) * 4);
    DecodeBc1(data, t.width, t.height, level.data());
    const XPic pic{level.data(), t.width, t.height};
    // the pictures drawn by hand, when they are there and of the right size
    uint32_t asset_id = 0;
    if (journal) {
        asset_id = t.hash == 0x8b9946c0528a3d88ull ? 4 : 8; // U2's has its own (a grey book, drawn by hand), U3 takes it too; both go with the same mask (5)
    } else if (t.hash == 0xde6288d2b457da2aull) {
        asset_id = 2; // U3's quick-time Y: the thick frame ...
    } else if (t.hash == 0xca555bf12dd7402bull) {
        asset_id = 3; // ... and the pressed one, the pictures of the other games
    } else if (circle) {
        switch (t.hash) { // B (thick, pressed, flat), A (flat), X (thick, pressed)
        case 0xbda57268c785fa75ull: asset_id = 20; break;
        case 0xd281f68d0ca6b604ull: asset_id = 21; break;
        case 0x0f2aa3306cf47da1ull: asset_id = 33; break;
        case 0x7d3273d9fcd75b58ull: asset_id = 30; break;
        case 0x2e8f1a37800214a7ull: asset_id = 36; break;
        case 0xcdde88dd024eeabeull: asset_id = 37; break;
        default: break;
        }
    } else if (t.hash == 0xac68a6ce1c1540adull || t.hash == 0xc6e03b00d234a7cdull || t.hash == 0x7e16e7522ff5748full) {
        asset_id = 1;
    } else if (t.hash == 0x4ae942741bdae0d1ull || t.hash == 0x07fabd1a89bf9ddbull) {
        asset_id = 2;
    } else if (t.hash == 0x02c213a5ba6c6d49ull || t.hash == 0xf2829d9b5230cb2bull) {
        asset_id = 3;
    }
    XAsset asset;
    if (asset_id && XLoadAsset(asset_id, asset) && asset.w == t.width && asset.h == t.height && asset.ch == 3) {
        for (size_t i = 0, n = size_t(t.width) * size_t(t.height); i < n; ++i) {
            std::memcpy(&level[i * 4], &asset.data[i * 3], 3);
            level[i * 4 + 3] = 255;
        }
        ReencodeChain(std::move(level), t.width, t.height, Codec::Bc1, layout, data, 0.0f, 0.0f, float(t.width), float(t.height));
        std::printf("KeyIcons: HUD button %s: the Xbox picture (%dx%d)\n", t.action, t.width, t.height);
        return true;
    }
    if (circle) {
        return false; // no picture of the right size: the game's button stays
    }
    // (the textures are stored upside down: the glyph is turned over)
    const bool done = triangle ? XReplaceSymbol(pic, font, t.cx, t.cy, t.rx * 1.05f, t.ry * 1.5f, 16, t.rx > 80.0f ? 3 : 7, "Y", t.rx * 1.0f, t.ry / t.rx, true, 4, true, &YellowY, t.rx > 80.0f ? 2 : 6)
                               : XJournalColor(pic, font);
    if (!done) {
        return false;
    }
    ReencodeChain(std::move(level), t.width, t.height, Codec::Bc1, layout, data, 0.0f, 0.0f, float(t.width), float(t.height));
    std::printf("KeyIcons: HUD button %s redrawn with its Xbox glyph (%dx%d)\n", t.action, t.width, t.height);
    return true;
}

// ---- "Rotate the stick": the four keys of the stick, one lit per frame -------------------------

/// One of the four frames of the animation, a color texture and its mask (a BC4 texture of the same
/// size, the shape of the picture). Both are stored upside down. The ring with its arrow and the
/// stick are replaced by the four keys of the left stick, the one of the frame lit: the animation
/// goes round the stick, counter-clockwise seen from above (up, left, down, right), the way the
/// arrow of the ring pointed.
struct StickFrame {
    uint64_t color_hash, mask_hash;
    int lit; // 0 up, 1 left, 2 down, 3 right
};

constexpr StickFrame StickFrames[] = {
    {0x430b51bcdea8c965ull, 0xb4b1cbe8238a286bull, 0}, // the stick tilted right, behind
    {0x6701a8a3c205d247ull, 0x2eacb0f28b9cc750ull, 1}, // tilted left
    {0x54d8148ace999c94ull, 0x54f1fc42fcf8c7f9ull, 2}, // the cap seen from the front, on the left
    {0xf319078dc634e6d0ull, 0x620fd1ff5352f5c7ull, 3}, // and on the right
};
constexpr int StickWidth = 256, StickHeight = 512;
constexpr float StickCenterY = 277.0f; // middle of what the ring and the stick covered, from the top of the picture
constexpr float StickKey = 78.0f;      // size of one key (the three in a row fill the picture)
constexpr float StickGap = 0.1f;       // gap between keys, in keys

const StickFrame* FindStickFrame(uint64_t hash, bool* is_mask) {
    for (const StickFrame& f : StickFrames) {
        if (f.color_hash == hash || f.mask_hash == hash) {
            *is_mask = f.mask_hash == hash;
            return &f;
        }
    }
    return nullptr;
}

bool PatchStickFrame(const StickFrame& f, bool is_mask, uint8_t* data, size_t size, uint32_t levels) {
    const Levels layout = LayoutLevels(StickWidth, StickHeight, levels);
    const Font& font = GetFont();
    if (size < layout.total || !font.ok) {
        return false;
    }
    const Bindings b = LoadBindings();
    std::string labels[4];
    int i = 0;
    for (const char* dir : {"ls_up", "ls_left", "ls_down", "ls_right"}) {
        labels[i] = ArrowLabel(font, BindingLabel(b, dir));
        if (labels[i++].empty()) {
            return false;
        }
    }
    std::vector<uint8_t> level(size_t(StickWidth) * size_t(StickHeight) * 4);
    if (is_mask) {
        DecodeBc4(data, StickWidth, StickHeight, level.data());
    } else {
        DecodeBc1(data, StickWidth, StickHeight, level.data());
    }
    // The picture has to be there: something opaque (mask) or not the background (color).
    const uint8_t bg[3] = {level[0], level[1], level[2]};
    int present = 0;
    for (size_t k = 0; k < level.size(); k += 4) {
        present += is_mask ? level[k] > 128
                           : std::abs(level[k] - bg[0]) + std::abs(level[k + 1] - bg[1]) + std::abs(level[k + 2] - bg[2]) > 40;
    }
    if (present < 6000) {
        return false;
    }

    // The four keys, upright, on a 512x512 sheet (the draw functions want a square).
    std::vector<uint8_t> sheet(512 * 512 * 4, 0);
    DrawCluster(sheet.data(), 512, 128.0f, StickCenterY, labels, font, StickKey, f.lit, StickKey / 44.0f, StickGap);
    for (int y = 0; y < StickHeight; ++y) {
        const int y_up = StickHeight - 1 - y;
        for (int x = 0; x < StickWidth; ++x) {
            const uint8_t* s = &sheet[(size_t(y_up) * 512 + size_t(x)) * 4];
            uint8_t* p = &level[(size_t(y) * StickWidth + size_t(x)) * 4];
            if (is_mask) { // the keys and their halo
                p[0] = p[1] = p[2] = s[3];
            } else { // their colors; the halo's orange elsewhere
                p[0] = s[3] ? s[0] : uint8_t(Orange.r);
                p[1] = s[3] ? s[1] : uint8_t(Orange.g);
                p[2] = s[3] ? s[2] : uint8_t(Orange.b);
            }
        }
    }
    ReencodeChain(std::move(level), StickWidth, StickHeight, is_mask ? Codec::Bc4 : Codec::Bc1, layout, data, 0.0f,
                  0.0f, float(StickWidth), float(StickHeight));
    std::printf("KeyIcons: stick animation frame redrawn (%s, %s lit, %u mip levels)\n", is_mask ? "mask" : "color",
                labels[f.lit].c_str(), layout.count);
    return true;
}

} // namespace

bool IsTextureCandidate(uint32_t width, uint32_t height) {
    return (width == 128 && height == 128) || (width == 256 && height == 256) ||
           (width == 512 && height == 512) || (width == 256 && height == 512) || (width == 512 && height == 256) || (width == 1024 && height == 512) ||
           (width == 2048 && height == 1024);
}

void NoteUnknownTexture(uint64_t hash, uint32_t width, uint32_t height, const uint8_t* data, size_t size) {
    // A HUD button texture (BC1) is mostly one flat color, the color of its halo: green, tan, yellow...
    // Sample blocks of the first mip (tiled, so any block will do) and look for one that most of them
    // are; normal maps (blue) and grays are not buttons.
    const size_t blocks = size_t(width / 4) * size_t(height / 4);
    if (blocks < 64 || size < blocks * 8) {
        return;
    }
    constexpr int Samples = 128;
    std::map<uint64_t, int> count;
    for (int i = 0; i < Samples; ++i) {
        uint64_t block;
        std::memcpy(&block, data + (size_t(i) * blocks / Samples) * 8, 8);
        ++count[block];
    }
    uint64_t flat = 0;
    int most = 0;
    for (const auto& [block, n] : count) {
        if (n > most) {
            most = n;
            flat = block;
        }
    }
    // A flat block: the same index everywhere. Its color is the end point, or a mix of the two.
    const uint16_t c0 = uint16_t(flat & 0xFFFF), c1 = uint16_t((flat >> 16) & 0xFFFF);
    const uint32_t indices = uint32_t(flat >> 32);
    const bool uniform = indices == 0u || indices == 0x55555555u || indices == 0xAAAAAAAAu || indices == 0xFFFFFFFFu;
    if (most < Samples * 4 / 10 || !uniform) {
        return;
    }
    const auto expand = [](uint16_t c, int shift) {
        return shift == 0 ? ((c >> 11) & 31) << 3 : shift == 1 ? ((c >> 5) & 63) << 2 : (c & 31) << 3;
    };
    const int select = int(indices & 3u);
    int rgb[3];
    for (int k = 0; k < 3; ++k) {
        const int a = expand(c0, k), b = expand(c1, k);
        rgb[k] = select == 0 ? a : select == 1 ? b : c0 > c1 ? (select == 2 ? (2 * a + b) / 3 : (a + 2 * b) / 3)
                                                              : (select == 2 ? (a + b) / 2 : 0);
    }
    const int r = rgb[0], g = rgb[1], b = rgb[2];
    const bool blue = b > 140 && r < 60 && g < 90;
    const bool gray = std::abs(r - g) < 14 && std::abs(g - b) < 14;
    if (blue || gray || r + g + b < 80) {
        return;
    }
    static std::mutex mutex;
    static std::set<uint64_t> noted;
    std::lock_guard<std::mutex> lock{mutex};
    if (noted.size() < 200 && noted.insert(hash).second) {
        std::printf("KeyIcons: HUD button texture not handled yet? hash %016llx, %ux%u, flat color (%d, %d, %d)\n",
                    (unsigned long long)hash, width, height, r, g, b);
    }
}

bool IsTextureTarget(uint64_t hash) {
    bool is_mask;
    Role role;
    return FindKeyTarget(hash, &role) != nullptr || FindStickFrame(hash, &is_mask) != nullptr || hash == JournalMaskHash || hash == JournalMaskHashU2 || hash == JournalMaskHashU3 || hash == SchemaHash || hash == SchemaMaskHash || hash == SchemaMaskHashU2 || hash == SchemaMaskHashU3;
}

bool IsPulseMask(uint64_t hash) {
    for (const KeyTarget& t : KeyTargets) {
        if (t.pulse && t.hard_hash == hash) {
            return true;
        }
    }
    return false;
}

int PromptKind(uint64_t hash) {
    bool is_mask;
    Role role;
    if (FindStickFrame(hash, &is_mask)) {
        return 1;
    }
    const KeyTarget* t = FindKeyTarget(hash, &role);
    if (!t) {
        return 0;
    }
    // (6: the d-pad of U3, also moved with a controller, so that its place and size are those of the "V")
    return !std::strcmp(t->action, "up") ? (t->hash == 0x75acc5a4dbcdc36full ? 6 : t->width == 256 ? 5 : 2) : !std::strcmp(t->action, "touchpad") ? 3 : (t->width == 512 && !std::strcmp(t->action, "triangle") && t->hash != 0xde6288d2b457da2aull && t->hash != 0xca555bf12dd7402bull) ? 4 : 0; // (U3's quick-time frames are not moved)
}

void PromptTransform(int kind, float* zoom, float* anchor_x, float* anchor_y, float* down, float* right) {
    *right = 0.0f;
    if (kind == 6 && g_input_mode != 0) {
        // The game's own d-pad of U3 (measured on 2000x1125 captures): about 99x107 pixels centered (311.5, 891); the "V" is
        // 85 pixels wide centered (307.5, 977.5). Zoom 0.85 about c = (T - zoom * A) / (1 - zoom) does both.
        struct Values {
            float zoom, x, y;
        };
        static const Values values = [] {
            Values v{0.85f, 0.1424f, 1.3046f};
            if (const char* env = std::getenv("BB_U3_DPAD_PAD")) {
                std::sscanf(env, "%f,%f,%f", &v.zoom, &v.x, &v.y);
            }
            return v;
        }();
        *zoom = values.zoom;
        *anchor_x = values.x;
        *anchor_y = values.y;
        *down = 0.0f;
        return;
    }
    if (kind == 5 || kind == 6) {
        // The "V" of U2 (measured in game on 2000x1125 captures): drawn 61 pixels wide, centered 312 from the left edge and 940
        // from the top; it must be as big as the flat "E" (91 pixels, centered 306 from the right edge and 976 from the top).
        // A zoom about the fixed point c of x' = c + zoom * (x - c) does both: c = (T - zoom * A) / (1 - zoom).
        struct Values {
            float zoom, x, y;
        };
        static const Values values = [] {
            Values v{1.49f, 0.1621f, 0.630f};
            if (const char* env = std::getenv("BB_U2_DPAD_ZOOM")) {
                std::sscanf(env, "%f,%f,%f", &v.zoom, &v.x, &v.y);
            }
            return v;
        }();
        *zoom = values.zoom;
        *anchor_x = values.x;
        *anchor_y = values.y;
        *down = 0.0f;
        return;
    }
    if (kind == 2 || kind == 3 || kind == 4) {
        // The "V" was at y = 925 of a 2000x1125 capture, the "E" prompts at 977; the bottom of the
        // journal at 993, the bottom of the "V" (now moved) at 1025. The right side of the "Tab" key
        // was at x = 1830, the right side of the "V" is at 1755.
        static const float journal_right = [] {
            const char* env = std::getenv("BB_JOURNAL_RIGHT");
            return env ? float(std::atof(env)) : -0.0375f;
        }();
        if (kind == 3) {
            *right = journal_right;
        }
        static const float v_down = [] {
            const char* env = std::getenv("BB_DPAD_DOWN");
            return env ? float(std::atof(env)) : 0.0462f;
        }();
        static const float journal_down = [] {
            const char* env = std::getenv("BB_JOURNAL_DOWN");
            return env ? float(std::atof(env)) : 0.0284f;
        }();
        *zoom = 1.0f;
        *anchor_x = *anchor_y = 0.5f;
        // The press frames of the "E" (512x512): their bottom was at y = 1003, the flat "E" has it at 1022.
        static const float frame_down = [] {
            const char* env = std::getenv("BB_FRAME_DOWN");
            return env ? float(std::atof(env)) : 0.0169f;
        }();
        *down = kind == 2 ? v_down : kind == 3 ? journal_down : frame_down;
        return;
    }
    *down = 0.0f;
    // Measured in game at 1920x1080: the keys are about 30 pixels, the other prompts about 88. The
    // "A" key sat at (166, 1000) of a 2000x1125 capture and must land where the "E" prompts are,
    // (200, 977): the fixed point c of x' = c + zoom * (x - c) is (T - zoom * A) / (1 - zoom).
    struct Values {
        float zoom, x, y;
    };
    static const Values values = [] {
        Values v{2.9f, 0.0741f, 0.8996f};
        if (const char* env = std::getenv("BB_STICK_ZOOM")) {
            std::sscanf(env, "%f,%f,%f", &v.zoom, &v.x, &v.y);
        }
        return v;
    }();
    *zoom = values.zoom;
    *anchor_x = values.x;
    *anchor_y = values.y;
}

bool PatchU1Texture(uint64_t hash, uint8_t* data, size_t size, uint32_t levels) {
    if (g_input_mode == 1) {
        return false; // a PlayStation controller: the HUD buttons stay the game's own
    }
    if (g_input_mode == 2) { // Xbox: the "interact" button and the journal's get the Xbox button; the rest (d-pad, stick) stays
        if (hash == JournalMaskHash || hash == JournalMaskHashU2) {
            return XJournalMask(data, size, levels);
        }
        if (hash == JournalMaskHashU3) {
            return XJournalMask(data, size, levels, true);
        }
        if (hash == SchemaHash) {
            return XSchemaColor(data, size);
        }
        if (hash == SchemaMaskHash || hash == SchemaMaskHashU2 || hash == SchemaMaskHashU3) {
            return XSchemaMask(data, size, levels);
        }
        Role role = Role::Color;
        const KeyTarget* t = FindKeyTarget(hash, &role);
        return t && PatchXboxHud(*t, role, data, size, levels);
    }
    // BB_KEY_ICONS_MASKS=0: leave the alpha masks alone (to tell whether a shared mask is at fault
    // when another effect looks wrong).
    static const bool masks = [] {
        const char* env = std::getenv("BB_KEY_ICONS_MASKS");
        return !(env && env[0] == '0');
    }();
    Role role = Role::Color;
    if (const KeyTarget* t = FindKeyTarget(hash, &role)) {
        return (masks || role == Role::Color) && PatchKeyTarget(*t, role, hash, data, size, levels);
    }
    bool is_mask = false;
    const StickFrame* f = FindStickFrame(hash, &is_mask);
    return f && (masks || !is_mask) && PatchStickFrame(*f, is_mask, data, size, levels);
}

} // namespace BbKeyIcons

