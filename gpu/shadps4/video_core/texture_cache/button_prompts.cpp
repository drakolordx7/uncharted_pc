// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/texture_cache/button_prompts.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

#include <miniz.h>
#include <SDL3/SDL_keyboard.h>

// Our own copy of stb_truetype (ImGui's is internal to imgui_draw.cpp).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

// DejaVu Sans, embedded by the settings menu (gpu/shim/bbport_overlay.cpp).
extern "C" const unsigned char bb_font_ttf[];

namespace VideoCore::ButtonPrompts {

namespace {

// input.ini actions with a picture on the sheets, in runtime_pad.c's names.
// RotCcw and RotCw (the keys that turn the left stick in circles) have no picture on the sheets;
// the stick prompt of valves and wheels shows them.
enum Action { Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Options, Touchpad, RotCcw, RotCw, Actions };
constexpr const char* action_names[Actions] = {"cross", "circle", "square", "triangle",
                                               "l1",    "r1",     "l2",     "r2",
                                               "l3",    "r3",     "options", "touchpad",
                                               "rotate_ccw", "rotate_cw"};
constexpr const char* action_defaults[Actions] = {"Space", "C", "F", "E", "Q", "G", "Mouse Right",
                                                  "Mouse Left", "Left Shift", "V", "Escape", "Tab",
                                                  "R", "T"};
enum Icon { Move = Actions, Look };
// The d-pad pictures: DPad plus the lit directions.
enum : u8 { DPad = 16, LitUp = 1, LitDown = 2, LitLeft = 4, LitRight = 8 };

struct Slot {
    u16 x0, y0, x1, y1; ///< the PlayStation picture's bounds (alpha), exclusive end
    u8 icon;            ///< Action, Move, Look or DPad | lit directions
};

// What a sheet texture holds: U1 and U2 keep colour and alpha in one RGBA texture; U3 splits
// them into a colour sheet (alpha 0) and a mask sheet (the alpha in R, G and B, alpha 0).
enum class Layer : u8 { Rgba, Colour, Mask };

struct Sheet {
    u64 hash;
    u32 width, height;
    const char* name;
    std::vector<Slot> slots;
    Layer layer = Layer::Rgba;
};

// The PlayStation icon sheet, the same layout in all three games. SHARE has no key and stays.
const std::vector<Slot> ps4_sheet = {
    {5, 81, 59, 134, Circle},       {69, 80, 123, 134, Square},   {134, 81, 187, 134, Triangle},
    {197, 81, 251, 134, Cross},     {7, 144, 58, 194, R2},        {70, 144, 121, 194, L2},
    {132, 148, 198, 186, Move},     {153, 196, 208, 239, Options}, {14, 202, 61, 248, Move},
    {88, 202, 135, 248, Look},      {7, 256, 76, 300, Touchpad},  {96, 259, 150, 291, R1},
    {173, 250, 227, 282, L1},       {15, 306, 61, 351, L3},       {89, 306, 135, 351, R3},
    {263, 5, 314, 55, R2},     {326, 5, 377, 55, L2},        {406, 55, 466, 101, Options},
    {270, 63, 317, 109, Move},      {344, 63, 391, 109, Look},    {429, 111, 483, 143, L1},
    {352, 120, 406, 152, R1},       {271, 167, 317, 212, L3},     {345, 167, 391, 212, R3},
    {4, 3, 71, 72, DPad},      {79, 3, 146, 72, DPad | LitLeft | LitRight},
    {154, 3, 221, 72, DPad | LitUp | LitDown}, {247, 224, 315, 294, DPad | LitUp},
    {4, 358, 71, 427, DPad | LitDown},
};

const Sheet sheets[] = {
    {0x97779d0906f16551ull, 512, 512, "Uncharted 2", ps4_sheet},
    {0xad5ada34fd34c76bull, 512, 512, "Uncharted 1", ps4_sheet},
    {0x8d03f2ff457f032dull, 512, 512, "Uncharted 3 (colour)", ps4_sheet, Layer::Colour},
    {0xae0c06306763d424ull, 512, 512, "Uncharted 3 (mask)", ps4_sheet, Layer::Mask},
};

// The big prompts and the weapon d-pad: whole BC1 pictures, stored upside down and cut out by
// the game's own masks (those stay). Around the button is the glow colour.
enum Shape : u8 {
    RoundButton, ///< 256x256: circle at 128.5, 126.5 (stored), radius 70
    Puck,        ///< 512x512: a raised button seen from the side (tapping prompts)
    PressedPuck, ///< 512x512: the same button pressed down (hold prompts)
    HudDPad,     ///< 256x256: the weapon switch d-pad
    Bumper,      ///< 256x256: L1 / R1, flat top, round bottom
    LeftTrigger, ///< 256x256: L2, round top, the left side leaning out
    RightTrigger, ///< 256x256: R2, the same mirrored
};
struct Picture {
    u64 hash;
    u16 size;
    u8 shape;
    u8 icon; ///< the Action, or for HudDPad the lit directions
    u8 glow[3];
    const char* name;
};
const Picture pictures[] = {
    {0x5db884aa1bae54f9ull, 256, RoundButton, Triangle, {55, 216, 104}, "Uncharted 2 triangle prompt"},
    {0x7af1abf688d8c824ull, 256, RoundButton, Circle, {230, 58, 35}, "Uncharted 2 circle prompt"},
    {0x9edd4f2f0f64f6e5ull, 256, RoundButton, Square, {243, 160, 155}, "Uncharted 2 square prompt"},
    // The rest of the big prompts of art/general/hud/buttons (hud.pak; the same pictures in Uncharted 1).
    {0x2bf5d90eb864162full, 256, RoundButton, Cross, {136, 149, 238}, "Uncharted 2 cross prompt"},
    {0xb88838d137123548ull, 256, Bumper, L1, {197, 170, 131}, "Uncharted 2 L1 prompt"},
    {0x4999c8b20a45cd6dull, 256, Bumper, R1, {197, 170, 131}, "Uncharted 2 R1 prompt"},
    {0x539b560401a1ac83ull, 256, LeftTrigger, L2, {197, 170, 131}, "Uncharted 2 L2 prompt"},
    {0xe2c351d65c6a63f1ull, 256, RightTrigger, R2, {197, 170, 131}, "Uncharted 2 R2 prompt"},
    // Uncharted 3's own encodings of the same pictures (the triangle, circle and square are shared).
    {0x305c2c731ec227e6ull, 256, RoundButton, Cross, {136, 149, 238}, "Uncharted 3 cross prompt"},
    {0x6c3fc301ede4114aull, 256, Bumper, L1, {199, 172, 136}, "Uncharted 3 L1 prompt"},
    {0xbd74a617a879f120ull, 256, Bumper, R1, {199, 172, 136}, "Uncharted 3 R1 prompt"},
    {0x2e8ae3386e2a91ddull, 256, LeftTrigger, L2, {199, 172, 136}, "Uncharted 3 L2 prompt"},
    {0x784278339a5a55bfull, 256, RightTrigger, R2, {199, 172, 136}, "Uncharted 3 R2 prompt"},
    {0x07ff3d0e4090be85ull, 512, Puck, Triangle, {54, 215, 103}, "Uncharted 3 triangle tapping prompt"},
    {0x4181f3ae6bd1efffull, 512, PressedPuck, Triangle, {54, 215, 103},
     "Uncharted 3 triangle hold prompt"},
    {0x668cf3cc04ab6963ull, 512, Puck, Square, {255, 158, 158}, "Uncharted 3 square tapping prompt"},
    {0x1b7e089f40b3bb21ull, 512, PressedPuck, Square, {255, 158, 158}, "Uncharted 3 square hold prompt"},
    {0x25d954ce23a8f5e2ull, 512, Puck, Triangle, {55, 216, 104}, "Uncharted 2 triangle tapping prompt"},
    {0xc96e34fbb203279eull, 512, PressedPuck, Triangle, {55, 216, 104},
     "Uncharted 2 triangle hold prompt"},
    {0x705083db743dd2f9ull, 256, HudDPad, LitUp, {206, 185, 156}, "Uncharted 2 weapon d-pad"},
    {0x94d741f6b51815eeull, 512, Puck, Circle, {222, 32, 32}, "Uncharted 3 circle tapping prompt"},
    {0x93b46ba81469882cull, 512, PressedPuck, Circle, {222, 32, 32}, "Uncharted 3 circle hold prompt"},
};

// The "turn the left stick in circles" prompt (valves and wheels): the ring with its arrows over a
// stick cap that tips around in its socket, one 256x512 BC1 picture per frame (stored upside down),
// the cap's "L" facing the player in some of them. Only that "L" and the cap face around it are
// replaced, by the bound rotate key; the rest of the game's picture stays. Frames in which the
// cap's face is turned away have no "L" and are not listed.
// The cap face around each "L" as the game drew it, without the "L": colours every 4 pixels
// (19 x 19 points, the middle one at cx, cy), measured on the game's pictures.
constexpr int stick_grid = 19, stick_grid_step = 4;
const u8 stick_face_right[19 * 19 * 3] = {
    89, 71, 51, 89, 70, 51, 87, 69, 51, 85, 68, 50, 83, 64, 48, 80, 63, 48, 76, 60, 46, 74, 59, 46, 73, 59, 45, 72, 58, 43, 72, 56, 41, 69, 55, 40, 64, 50, 38, 56, 45, 37, 61, 48, 38, 64, 50, 39, 64, 50, 39, 64, 50, 38, 64, 50, 38,
    90, 71, 51, 90, 71, 51, 90, 69, 50, 87, 70, 53, 86, 68, 50, 82, 65, 49, 79, 62, 47, 75, 60, 46, 73, 59, 45, 72, 57, 43, 71, 56, 41, 68, 53, 40, 63, 50, 38, 57, 46, 37, 52, 41, 35, 55, 43, 36, 64, 50, 38, 64, 51, 38, 65, 51, 39,
    90, 72, 51, 91, 72, 51, 93, 72, 52, 90, 73, 56, 86, 69, 54, 85, 68, 52, 82, 66, 50, 79, 63, 48, 74, 59, 45, 71, 57, 43, 68, 54, 41, 66, 52, 39, 62, 49, 38, 57, 46, 37, 53, 42, 35, 53, 43, 35, 54, 45, 34, 65, 51, 39, 67, 52, 40,
    90, 72, 52, 90, 73, 53, 93, 74, 56, 94, 76, 57, 89, 73, 56, 89, 72, 55, 87, 70, 53, 83, 67, 51, 78, 62, 47, 73, 58, 44, 69, 55, 41, 66, 52, 40, 62, 49, 38, 58, 46, 37, 55, 44, 36, 52, 42, 34, 50, 41, 33, 57, 44, 36, 68, 54, 40,
    89, 71, 55, 92, 74, 56, 96, 78, 57, 98, 80, 59, 94, 75, 57, 93, 75, 57, 92, 74, 56, 89, 72, 55, 83, 67, 51, 77, 61, 47, 74, 58, 45, 71, 56, 43, 67, 53, 41, 61, 49, 39, 57, 46, 36, 55, 44, 35, 51, 41, 33, 48, 40, 33, 66, 53, 41,
    91, 72, 57, 97, 76, 57, 100, 81, 59, 101, 82, 64, 98, 80, 61, 97, 79, 60, 96, 78, 59, 94, 76, 58, 86, 70, 53, 81, 65, 50, 80, 64, 49, 79, 63, 48, 75, 60, 46, 69, 55, 43, 63, 50, 39, 59, 46, 36, 55, 43, 34, 54, 42, 33, 56, 46, 38,
    92, 73, 57, 98, 77, 58, 103, 83, 62, 103, 86, 65, 101, 83, 64, 99, 82, 62, 98, 81, 61, 96, 78, 60, 88, 71, 55, 83, 67, 51, 82, 65, 51, 81, 65, 50, 79, 63, 49, 75, 60, 46, 69, 55, 42, 63, 50, 38, 60, 46, 36, 60, 45, 36, 50, 42, 35,
    92, 73, 57, 99, 78, 58, 103, 85, 62, 104, 87, 65, 102, 85, 64, 101, 83, 63, 100, 82, 62, 95, 78, 60, 87, 70, 54, 84, 67, 52, 83, 66, 51, 81, 65, 50, 82, 65, 50, 79, 63, 48, 73, 58, 44, 67, 53, 41, 63, 49, 38, 63, 49, 37, 51, 42, 33,
    92, 74, 57, 98, 79, 59, 103, 84, 63, 102, 85, 63, 101, 84, 63, 100, 83, 63, 99, 82, 62, 94, 76, 58, 86, 69, 53, 84, 67, 51, 84, 67, 52, 84, 67, 53, 84, 67, 52, 78, 63, 48, 73, 59, 45, 70, 54, 42, 66, 52, 41, 63, 50, 38, 52, 42, 33,
    91, 73, 56, 97, 77, 57, 102, 82, 61, 100, 81, 62, 99, 81, 62, 99, 81, 61, 97, 80, 61, 91, 74, 56, 85, 68, 52, 84, 67, 51, 84, 67, 52, 85, 68, 52, 86, 69, 53, 82, 66, 49, 78, 62, 46, 74, 58, 41, 68, 53, 41, 62, 49, 38, 53, 42, 33,
    88, 72, 54, 93, 75, 57, 98, 78, 58, 96, 77, 60, 96, 77, 60, 96, 78, 59, 94, 76, 58, 88, 71, 54, 83, 67, 50, 83, 67, 50, 84, 68, 50, 85, 69, 52, 84, 69, 51, 82, 65, 49, 78, 61, 45, 72, 57, 41, 67, 53, 41, 61, 48, 37, 52, 42, 33,
    85, 69, 52, 91, 73, 56, 93, 74, 57, 91, 73, 57, 91, 73, 57, 92, 74, 57, 90, 72, 56, 84, 68, 51, 82, 65, 49, 82, 65, 49, 83, 66, 49, 84, 68, 49, 84, 66, 49, 80, 62, 47, 76, 58, 43, 70, 54, 41, 65, 51, 40, 60, 46, 36, 49, 40, 33,
    83, 66, 49, 86, 69, 53, 87, 69, 54, 86, 68, 53, 87, 69, 53, 87, 70, 54, 85, 68, 52, 81, 64, 48, 79, 63, 47, 80, 63, 47, 81, 64, 48, 82, 65, 49, 82, 65, 49, 78, 60, 45, 74, 57, 41, 68, 53, 41, 64, 50, 37, 58, 45, 34, 44, 39, 33,
    79, 62, 48, 82, 65, 49, 81, 64, 48, 81, 64, 49, 82, 65, 50, 82, 65, 50, 79, 63, 48, 76, 60, 45, 76, 60, 44, 76, 60, 45, 77, 61, 46, 79, 62, 48, 78, 61, 46, 74, 58, 42, 71, 54, 41, 65, 51, 38, 61, 47, 35, 54, 43, 32, 45, 41, 38,
    77, 59, 45, 78, 61, 48, 77, 61, 46, 77, 61, 46, 76, 60, 46, 75, 59, 45, 72, 56, 43, 71, 55, 41, 72, 56, 42, 73, 57, 42, 73, 58, 42, 76, 61, 44, 74, 58, 42, 71, 55, 41, 66, 53, 39, 62, 49, 37, 56, 46, 33, 46, 39, 33, 65, 57, 52,
    75, 57, 43, 74, 57, 44, 73, 57, 43, 72, 56, 43, 70, 55, 42, 67, 53, 41, 65, 51, 38, 65, 51, 38, 68, 53, 39, 71, 55, 40, 72, 55, 40, 73, 58, 41, 70, 55, 41, 66, 52, 38, 62, 50, 37, 59, 46, 34, 51, 42, 32, 48, 39, 36, 90, 75, 65,
    73, 56, 42, 71, 54, 41, 68, 53, 41, 66, 52, 40, 65, 50, 39, 63, 49, 38, 61, 48, 36, 62, 48, 36, 63, 49, 37, 66, 51, 38, 69, 52, 38, 69, 53, 40, 65, 51, 39, 62, 48, 37, 59, 45, 34, 54, 42, 32, 42, 37, 33, 71, 61, 53, 85, 72, 61,
    70, 54, 42, 68, 53, 41, 64, 51, 40, 62, 49, 39, 61, 47, 38, 60, 46, 36, 59, 46, 35, 59, 46, 34, 59, 46, 35, 61, 48, 37, 63, 50, 38, 63, 50, 39, 61, 47, 36, 57, 45, 33, 53, 41, 32, 42, 36, 32, 57, 50, 43, 76, 66, 56, 79, 68, 58,
    68, 53, 41, 66, 51, 40, 63, 49, 40, 63, 48, 39, 61, 46, 37, 60, 46, 36, 59, 47, 36, 58, 46, 33, 58, 46, 34, 60, 46, 36, 60, 47, 36, 59, 46, 35, 54, 45, 33, 48, 40, 32, 39, 35, 33, 44, 39, 35, 55, 48, 42, 67, 58, 49, 70, 61, 51,
};
const u8 stick_face_left[19 * 19 * 3] = {
    64, 51, 40, 64, 51, 40, 64, 51, 40, 64, 50, 39, 68, 53, 40, 70, 56, 43, 73, 58, 44, 75, 59, 45, 76, 60, 46, 76, 60, 46, 73, 58, 44, 67, 53, 41, 63, 51, 38, 61, 49, 37, 58, 46, 36, 59, 47, 36, 59, 48, 37, 60, 48, 38, 60, 49, 38,
    64, 51, 40, 63, 50, 40, 63, 50, 40, 65, 51, 40, 67, 53, 40, 69, 55, 42, 72, 57, 43, 74, 58, 44, 75, 59, 45, 75, 59, 45, 72, 57, 44, 68, 54, 41, 65, 52, 40, 67, 53, 40, 73, 57, 44, 64, 51, 41, 57, 47, 37, 59, 48, 38, 60, 49, 38,
    64, 51, 40, 64, 51, 40, 64, 51, 41, 64, 51, 40, 66, 52, 40, 69, 54, 41, 71, 56, 42, 72, 57, 43, 73, 58, 44, 73, 58, 44, 72, 58, 44, 71, 57, 43, 72, 57, 43, 74, 59, 45, 79, 61, 47, 84, 64, 48, 66, 53, 41, 58, 47, 37, 61, 49, 38,
    64, 51, 40, 67, 52, 41, 66, 52, 41, 66, 52, 41, 67, 53, 41, 68, 54, 41, 70, 55, 42, 73, 57, 44, 76, 60, 46, 79, 63, 48, 81, 65, 50, 81, 66, 50, 81, 65, 50, 81, 65, 49, 83, 65, 50, 85, 66, 51, 86, 68, 50, 61, 50, 38, 61, 49, 38,
    63, 50, 39, 68, 53, 41, 68, 53, 41, 68, 53, 40, 68, 54, 41, 69, 54, 41, 71, 56, 42, 76, 60, 46, 85, 69, 52, 94, 77, 58, 97, 80, 61, 96, 79, 61, 93, 76, 58, 90, 72, 55, 87, 69, 53, 86, 68, 52, 89, 70, 54, 79, 64, 48, 57, 47, 37,
    64, 51, 40, 70, 54, 41, 71, 54, 40, 71, 55, 40, 70, 55, 41, 70, 55, 41, 73, 58, 43, 83, 67, 50, 99, 82, 62, 106, 88, 67, 106, 89, 67, 104, 87, 66, 101, 84, 64, 95, 78, 61, 91, 73, 56, 89, 71, 53, 91, 73, 56, 88, 70, 55, 64, 52, 42,
    65, 52, 40, 72, 55, 41, 73, 58, 41, 72, 57, 40, 72, 57, 41, 72, 57, 41, 76, 60, 44, 91, 74, 56, 107, 89, 67, 109, 91, 69, 108, 90, 68, 108, 91, 68, 106, 89, 68, 100, 82, 65, 94, 77, 60, 96, 77, 58, 94, 75, 57, 89, 71, 56, 76, 61, 47,
    66, 52, 40, 72, 56, 41, 74, 59, 41, 73, 58, 41, 73, 57, 42, 73, 57, 42, 77, 61, 45, 95, 78, 58, 108, 90, 68, 110, 92, 70, 109, 91, 69, 111, 93, 71, 111, 94, 72, 108, 89, 70, 104, 86, 65, 100, 81, 59, 95, 77, 57, 89, 72, 56, 84, 66, 49,
    65, 51, 40, 72, 56, 41, 74, 58, 43, 74, 57, 43, 74, 57, 42, 74, 58, 42, 77, 61, 45, 94, 77, 58, 108, 89, 68, 110, 92, 70, 113, 93, 72, 116, 96, 75, 117, 98, 76, 113, 93, 72, 106, 88, 66, 101, 81, 61, 95, 76, 57, 90, 72, 56, 84, 66, 49,
    65, 51, 40, 70, 55, 41, 74, 59, 44, 75, 58, 43, 74, 57, 43, 73, 57, 42, 76, 60, 44, 92, 74, 56, 106, 87, 67, 109, 90, 69, 111, 92, 70, 112, 93, 71, 114, 96, 73, 111, 90, 70, 105, 85, 64, 98, 79, 58, 93, 74, 57, 88, 71, 54, 83, 66, 48,
    63, 50, 39, 68, 53, 41, 73, 58, 42, 73, 58, 42, 73, 57, 42, 72, 57, 42, 74, 58, 43, 87, 70, 53, 102, 84, 63, 106, 87, 66, 106, 87, 66, 106, 86, 65, 110, 89, 69, 106, 85, 65, 102, 81, 61, 96, 76, 57, 91, 72, 56, 88, 70, 51, 81, 65, 48,
    61, 48, 37, 67, 52, 41, 73, 56, 41, 73, 57, 41, 72, 56, 41, 71, 56, 41, 72, 57, 42, 82, 65, 49, 97, 79, 59, 101, 82, 62, 100, 81, 61, 99, 80, 60, 102, 83, 61, 101, 81, 60, 96, 78, 57, 92, 75, 57, 88, 71, 55, 85, 67, 50, 79, 62, 48,
    59, 46, 35, 65, 50, 40, 72, 54, 41, 72, 55, 41, 70, 54, 41, 69, 54, 41, 70, 55, 41, 76, 60, 45, 90, 73, 54, 95, 77, 57, 94, 76, 57, 93, 75, 55, 94, 77, 56, 95, 76, 57, 93, 74, 54, 89, 71, 53, 85, 68, 51, 81, 66, 48, 72, 58, 44,
    58, 45, 34, 62, 48, 37, 66, 52, 40, 70, 54, 41, 68, 53, 41, 67, 52, 40, 67, 52, 39, 71, 56, 42, 82, 66, 49, 88, 71, 53, 88, 71, 52, 86, 69, 51, 87, 71, 53, 89, 72, 53, 88, 69, 52, 83, 67, 50, 81, 64, 48, 79, 61, 46, 62, 49, 39,
    55, 43, 32, 59, 45, 35, 62, 49, 38, 66, 52, 41, 65, 51, 39, 64, 51, 38, 63, 50, 38, 65, 51, 39, 73, 59, 44, 80, 64, 48, 82, 66, 48, 81, 65, 47, 81, 64, 46, 85, 66, 49, 83, 65, 49, 80, 63, 47, 78, 60, 45, 72, 57, 44, 50, 42, 35,
    54, 42, 32, 56, 44, 32, 59, 46, 35, 62, 49, 38, 61, 49, 36, 61, 48, 36, 59, 47, 35, 59, 47, 35, 63, 50, 38, 70, 56, 42, 74, 59, 44, 76, 60, 45, 76, 60, 45, 78, 62, 48, 78, 61, 46, 77, 59, 44, 72, 57, 41, 56, 45, 37, 49, 39, 34,
    54, 42, 32, 54, 42, 32, 56, 44, 32, 59, 46, 35, 58, 46, 34, 57, 46, 33, 55, 44, 34, 54, 43, 33, 56, 45, 34, 61, 48, 37, 66, 52, 39, 69, 55, 41, 70, 56, 42, 73, 57, 43, 74, 57, 42, 71, 56, 41, 58, 48, 39, 40, 36, 34, 45, 38, 34,
    53, 42, 32, 53, 42, 32, 53, 41, 32, 56, 43, 32, 56, 44, 32, 54, 43, 32, 52, 42, 32, 51, 41, 32, 52, 42, 33, 56, 44, 34, 59, 46, 35, 63, 49, 38, 66, 52, 40, 69, 54, 41, 68, 53, 39, 56, 46, 37, 39, 36, 34, 41, 37, 34, 43, 38, 34,
    53, 42, 32, 53, 42, 32, 52, 41, 32, 50, 41, 32, 52, 41, 32, 50, 41, 32, 48, 40, 32, 48, 39, 32, 51, 41, 32, 54, 42, 32, 56, 44, 33, 59, 46, 35, 62, 49, 38, 59, 48, 39, 46, 41, 36, 39, 36, 33, 40, 37, 34, 41, 37, 34, 43, 38, 35,
};

struct StickFrame {
    u64 hash;
    bool clockwise;
    float cx, cy;     ///< the middle of the "L", stored pixels
    float m[4];       ///< x' = cx + m0 x + m1 y, y' = cy + m2 x + m3 y for a label drawn at size 100
    const u8* face;   ///< the cap face there: stick_grid x stick_grid RGB points
    const char* name;
};
constexpr u32 stick_width = 256, stick_height = 512;
const StickFrame stick_frames[] = {
    {0x349208a673329cceull, false, 181.63f, 185.76f, {0.6450f, 0.1113f, 0.1430f, -0.7140f},
     stick_face_right, "Uncharted 1 stick prompt, cap right"},
    {0x7515a2f4f6a1022dull, false, 80.62f, 185.84f, {0.6568f, -0.1355f, -0.1058f, -0.7059f},
     stick_face_left, "Uncharted 1 stick prompt, cap left"},
};

struct Bindings {
    std::string keys[Actions];
    std::string move[4]; ///< ls_up, ls_left, ls_down, ls_right
    std::string dpad[4]; ///< up, down, left, right
    bool mouse_look = true;
};

/// The key's label in the player's keyboard layout. A default letter is a position, as in
/// runtime_pad.c (W is the key labelled Z on AZERTY); a letter the player chose is that letter.
std::string Labelled(const std::string& name, const char* fallback) {
    if (name.size() != 1 || name != fallback) {
        return name;
    }
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    const SDL_Keycode key =
        code == SDL_SCANCODE_UNKNOWN ? SDLK_UNKNOWN : SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
    const char* label = key == SDLK_UNKNOWN ? nullptr : SDL_GetKeyName(key);
    return label && label[0] ? label : name;
}

Bindings LoadBindings() {
    Bindings b;
    for (int a = 0; a < Actions; ++a) {
        b.keys[a] = action_defaults[a];
    }
    const char* move_names[4] = {"ls_up", "ls_left", "ls_down", "ls_right"};
    const char* move_defaults[4] = {"W", "A", "S", "D"};
    const char* dpad_names[4] = {"up", "down", "left", "right"};
    const char* dpad_defaults[4] = {"Up", "Down", "Left", "Right"};
    for (int i = 0; i < 4; ++i) {
        b.move[i] = move_defaults[i];
        b.dpad[i] = dpad_defaults[i];
    }
    const char* path = std::getenv("BB_INPUT_CONFIG");
    FILE* f = path && *path ? std::fopen(path, "r") : nullptr;
    char line[256];
    while (f && std::fgets(line, sizeof(line), f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char* eq = std::strchr(line, '=');
        if (line[0] == '#' || !eq) {
            continue;
        }
        *eq = 0;
        std::string value = eq + 1;
        value = value.substr(0, value.find('|')); // the first of two keys
        if (!std::strcmp(line, "mouse_look")) {
            b.mouse_look = std::atoi(value.c_str()) != 0;
        } else if (!std::strncmp(line, "key.", 4)) {
            for (int a = 0; a < Actions; ++a) {
                if (!std::strcmp(line + 4, action_names[a])) {
                    b.keys[a] = value;
                }
            }
            for (int i = 0; i < 4; ++i) {
                if (!std::strcmp(line + 4, move_names[i])) {
                    b.move[i] = value;
                }
                if (!std::strcmp(line + 4, dpad_names[i])) {
                    b.dpad[i] = value;
                }
            }
        }
    }
    if (f) {
        std::fclose(f);
    }
    for (int a = 0; a < Actions; ++a) {
        b.keys[a] = Labelled(b.keys[a], action_defaults[a]);
    }
    for (int i = 0; i < 4; ++i) {
        b.move[i] = Labelled(b.move[i], move_defaults[i]);
    }
    return b;
}

/// The text on a key cap for an SDL key name.
std::string KeyLabel(const std::string& name) {
    static const std::map<std::string, std::string> labels = {
        {"Space", "SPACE"},     {"Left Shift", "SHIFT"}, {"Right Shift", "SHIFT"},
        {"Left Ctrl", "CTRL"},  {"Right Ctrl", "CTRL"},  {"Left Alt", "ALT"},
        {"Right Alt", "ALT"},   {"Escape", "ESC"},       {"Return", "ENTER"},
        {"Tab", "TAB"},         {"Backspace", "BKSP"},   {"CapsLock", "CAPS"},
        {"Delete", "DEL"},      {"Insert", "INS"},       {"PageUp", "PGUP"},
        {"PageDown", "PGDN"},   {"Up", "\xE2\x86\x91"},  {"Down", "\xE2\x86\x93"},
        {"Left", "\xE2\x86\x90"}, {"Right", "\xE2\x86\x92"}, {"Mouse 4", "M4"},
        {"Mouse 5", "M5"},      {"Wheel Up", "WHEEL\xE2\x86\x91"}, {"Wheel Down", "WHEEL\xE2\x86\x93"},
    };
    if (const auto it = labels.find(name); it != labels.end()) {
        return it->second;
    }
    if (!name.compare(0, 7, "Keypad ")) {
        return "NUM" + KeyLabel(name.substr(7));
    }
    std::string out = name;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

struct Color {
    float r, g, b;
};
constexpr Color cap_top{0.235f, 0.220f, 0.200f};
constexpr Color cap_bottom{0.110f, 0.102f, 0.094f};
constexpr Color rim{0.850f, 0.800f, 0.670f};
constexpr Color text_color{0.960f, 0.930f, 0.850f};
constexpr Color highlight{0.910f, 0.470f, 0.160f}; ///< the d-pad's orange

/// A straight-alpha RGBA float canvas the size of the sheet.
struct Canvas {
    u32 width, height;
    std::vector<float> px;
    Canvas(u32 w, u32 h) : width{w}, height{h}, px(size_t(w) * h * 4, 0.0f) {}

    /// Paints `color` with coverage `a` over what is there.
    void Blend(int x, int y, Color color, float a) {
        if (x < 0 || y < 0 || x >= int(width) || y >= int(height) || a <= 0.0f) {
            return;
        }
        float* p = &px[(size_t(y) * width + x) * 4];
        const float out_a = a + p[3] * (1.0f - a);
        const float keep = p[3] * (1.0f - a);
        p[0] = (color.r * a + p[0] * keep) / out_a;
        p[1] = (color.g * a + p[1] * keep) / out_a;
        p[2] = (color.b * a + p[2] * keep) / out_a;
        p[3] = out_a;
    }
};

/// Signed distance from a rounded rectangle (negative inside).
float RoundedRect(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::fabs(x - cx) - hw + r;
    const float qy = std::fabs(y - cy) - hh + r;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

float Coverage(float distance) {
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

/// A shape given by a distance function: soft dark halo, rim, gradient body.
template <typename Distance>
void DrawCap(Canvas& c, float x0, float y0, float x1, float y1, Distance distance,
             const Color* face = nullptr) {
    for (int y = int(y0) - 3; y < int(std::ceil(y1)) + 3; ++y) {
        for (int x = int(x0) - 3; x < int(std::ceil(x1)) + 3; ++x) {
            const float d = distance(x + 0.5f, y + 0.5f);
            // A halo like the PlayStation pictures' glow, so the key reads on bright scenes.
            c.Blend(x, y, {0.0f, 0.0f, 0.0f}, 0.45f * std::clamp(1.0f - d / 3.0f, 0.0f, 1.0f) *
                                                  (d > -1.0f ? 1.0f : 0.0f));
            const float rim_a = Coverage(d);
            if (rim_a <= 0.0f) {
                continue;
            }
            c.Blend(x, y, rim, rim_a);
            const float t = std::clamp((y + 0.5f - y0) / std::max(y1 - y0, 1.0f), 0.0f, 1.0f);
            const Color body{cap_top.r + (cap_bottom.r - cap_top.r) * t,
                             cap_top.g + (cap_bottom.g - cap_top.g) * t,
                             cap_top.b + (cap_bottom.b - cap_top.b) * t};
            c.Blend(x, y, face ? *face : body, Coverage(d + 1.6f));
        }
    }
}

class Font {
public:
    Font() {
        ok = stbtt_InitFont(&info, bb_font_ttf, stbtt_GetFontOffsetForIndex(bb_font_ttf, 0)) != 0;
    }

    /// Draws `text` (UTF-8) centred on (cx, cy), as large as fits `max_width` up to `size` px.
    void Draw(Canvas& c, const std::string& text, float cx, float cy, float max_width,
              float size, Color color = text_color) const {
        if (!ok) {
            return;
        }
        const auto codepoints = Decode(text);
        float scale = stbtt_ScaleForPixelHeight(&info, size);
        float width = Width(codepoints, scale);
        if (width > max_width) {
            scale *= max_width / width;
            width = max_width;
        }
        // Centre the capital letters' height.
        int ix0, iy0, ix1, iy1;
        stbtt_GetCodepointBitmapBox(&info, 'H', scale, scale, &ix0, &iy0, &ix1, &iy1);
        const float baseline = cy + (iy1 - iy0) * 0.5f;
        float pen = cx - width * 0.5f;
        for (size_t i = 0; i < codepoints.size(); ++i) {
            const int cp = codepoints[i];
            const float shift = pen - std::floor(pen);
            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(&info, scale, scale, shift,
                                                                     0.0f, cp, &w, &h, &xoff, &yoff);
            const int left = bitmap ? int(std::floor(pen)) + xoff : 0;
            const int top = int(std::lround(baseline)) + yoff;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    // Slightly bold: each pixel also takes its left neighbour's coverage.
                    const float a = std::max(bitmap[y * w + x], x ? bitmap[y * w + x - 1] : u8(0));
                    c.Blend(left + x, top + y, color, a / 255.0f);
                }
            }
            stbtt_FreeBitmap(bitmap, nullptr);
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
            pen += advance * scale + 1.0f;
            if (i + 1 < codepoints.size()) {
                pen += stbtt_GetCodepointKernAdvance(&info, cp, codepoints[i + 1]) * scale;
            }
        }
    }

private:
    static std::vector<int> Decode(const std::string& s) {
        std::vector<int> out;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = s[i];
            const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            int cp = n == 1 ? c : c & (0x3F >> (n - 1));
            for (int k = 1; k < n && i + k < s.size(); ++k) {
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
            out.push_back(cp);
            i += n;
        }
        return out;
    }

    float Width(const std::vector<int>& codepoints, float scale) const {
        float width = 0.0f;
        for (size_t i = 0; i < codepoints.size(); ++i) {
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&info, codepoints[i], &advance, &lsb);
            width += advance * scale + (i + 1 < codepoints.size() ? 1.0f : 0.0f);
        }
        return width;
    }

    stbtt_fontinfo info{};
    bool ok = false;
};

/// A mouse seen from above; `button`: 1 left, 2 right, 4 middle, 0 none highlighted.
void DrawMouse(Canvas& c, const Slot& s, int button) {
    const float w = s.x1 - s.x0, h = s.y1 - s.y0;
    const float bh = h * 0.86f, bw = std::min(w * 0.86f, bh * 0.68f);
    const float cx = (s.x0 + s.x1) * 0.5f, cy = (s.y0 + s.y1) * 0.5f;
    const float top = cy - bh * 0.5f, split = top + bh * 0.42f;
    DrawCap(c, cx - bw * 0.5f, top, cx + bw * 0.5f, top + bh, [&](float x, float y) {
        return RoundedRect(x, y, cx, cy, bw * 0.5f, bh * 0.5f, bw * 0.48f);
    });
    for (int y = int(top); y < int(top + bh) + 1; ++y) {
        for (int x = int(cx - bw * 0.5f); x < int(cx + bw * 0.5f) + 1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float inside = Coverage(RoundedRect(px, py, cx, cy, bw * 0.5f - 1.6f,
                                                      bh * 0.5f - 1.6f, bw * 0.48f - 1.6f));
            if (inside <= 0.0f || py > split + 0.8f) {
                continue;
            }
            if ((button == 1 && px < cx - 0.8f) || (button == 2 && px > cx + 0.8f)) {
                c.Blend(x, y, highlight, inside);
            }
            // The seams between the buttons and the body.
            const float seam = std::max(1.0f - std::fabs(px - cx) / 0.9f,
                                        1.0f - std::fabs(py - split) / 0.9f);
            c.Blend(x, y, rim, inside * std::clamp(seam, 0.0f, 1.0f));
        }
    }
    if (button == 4) {
        const float ww = bw * 0.16f, wh = bh * 0.2f, wy = top + bh * 0.24f;
        for (int y = int(wy - wh); y < int(wy + wh) + 1; ++y) {
            for (int x = int(cx - ww) - 1; x < int(cx + ww) + 2; ++x) {
                c.Blend(x, y, highlight,
                        Coverage(RoundedRect(x + 0.5f, y + 0.5f, cx, wy, ww, wh, ww)));
            }
        }
    }
}

void DrawKey(Canvas& c, const Font& font, const Slot& s, const std::string& label) {
    const float x0 = s.x0 + 2.0f, y0 = s.y0 + 2.0f, x1 = s.x1 - 2.0f, y1 = s.y1 - 2.0f;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float radius = std::min(x1 - x0, y1 - y0) * 0.22f;
    DrawCap(c, x0, y0, x1, y1, [&](float x, float y) {
        return RoundedRect(x, y, cx, cy, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f, radius);
    });
    font.Draw(c, label, cx, cy, (x1 - x0) - 8.0f, (y1 - y0) * 0.5f);
}

void DrawBinding(Canvas& c, const Font& font, const Slot& s, const std::string& key) {
    if (key == "Mouse Left" || key == "Mouse Right" || key == "Mouse Middle") {
        DrawMouse(c, s, key == "Mouse Left" ? 1 : key == "Mouse Right" ? 2 : 4);
    } else {
        DrawKey(c, font, s, KeyLabel(key));
    }
}

Canvas Downsample(const Canvas& level);

// ---- Xbox pictures: Kenney's Input Prompts (CC0), packed by tools/make_xbox_prompts.py ------

#include "video_core/texture_cache/button_prompts_xbox.inc"

// The order of xbox_glyphs in the .inc.
enum XGlyph : u8 {
    XA, XB, XX, XY, XLB, XRB, XLT, XRT, XLeftPress, XRightPress, XLeftStick, XRightStick, XMenu,
    XView, XDpad, XDpadUp, XDpadDown, XDpadHorizontal, XDpadVertical, XDpadLeft, XDpadRight,
    XGlyphs
};
static_assert(sizeof(xbox_glyphs) / sizeof(xbox_glyphs[0]) == XGlyphs, "glyph table");

struct XboxAtlas {
    std::vector<u8> rgba; ///< xbox_atlas_width x xbox_atlas_height, straight alpha
    struct Glyph {
        int x, y;                     ///< the glyph's cell
        int left, top, right, bottom; ///< the drawn part inside the cell (alpha), exclusive end
    } glyph[XGlyphs];
};

const XboxAtlas* Atlas() {
    static const std::unique_ptr<XboxAtlas> atlas = []() -> std::unique_ptr<XboxAtlas> {
        size_t size = 0;
        void* raw = tinfl_decompress_mem_to_heap(xbox_atlas_z, xbox_atlas_size, &size,
                                                 TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (!raw || size != size_t(xbox_atlas_width) * xbox_atlas_height * 4) {
            mz_free(raw);
            std::printf("Button prompts: the Xbox pictures could not be unpacked\n");
            return nullptr;
        }
        auto out = std::make_unique<XboxAtlas>();
        out->rgba.assign(static_cast<const u8*>(raw), static_cast<const u8*>(raw) + size);
        mz_free(raw);
        for (int i = 0; i < XGlyphs; ++i) {
            auto& g = out->glyph[i];
            g = {xbox_glyphs[i].x, xbox_glyphs[i].y, 1 << 30, 1 << 30, 0, 0};
            for (u32 y = 0; y < xbox_glyph_size; ++y) {
                for (u32 x = 0; x < xbox_glyph_size; ++x) {
                    if (out->rgba[(size_t(g.y + y) * xbox_atlas_width + g.x + x) * 4 + 3] > 8) {
                        g.left = std::min(g.left, int(x));
                        g.top = std::min(g.top, int(y));
                        g.right = std::max(g.right, int(x) + 1);
                        g.bottom = std::max(g.bottom, int(y) + 1);
                    }
                }
            }
        }
        return out;
    }();
    return atlas.get();
}

/// A box filter over a plane, horizontally then vertically.
void BoxBlur(std::vector<float>& plane, int w, int h, int radius) {
    std::vector<float> tmp(plane.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                sum += plane[size_t(y) * w + std::clamp(x + k, 0, w - 1)];
            }
            tmp[size_t(y) * w + x] = sum / float(2 * radius + 1);
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                sum += tmp[size_t(std::clamp(y + k, 0, h - 1)) * w + x];
            }
            plane[size_t(y) * w + x] = sum / float(2 * radius + 1);
        }
    }
}

/// An Xbox picture with its drawn part `w` x `h` px, centred on (cx, cy), over a soft dark halo
/// (the PlayStation pictures have a glow) that keeps it readable on bright scenes.
void DrawGlyph(Canvas& c, int glyph, float cx, float cy, float w, float h, bool halo = true) {
    const XboxAtlas* atlas = Atlas();
    if (!atlas || w < 1.0f || h < 1.0f) {
        return;
    }
    const auto& g = atlas->glyph[glyph];
    const float gw = float(g.right - g.left), gh = float(g.bottom - g.top);
    if (gw <= 0.0f || gh <= 0.0f) {
        return;
    }
    const float x0 = cx - w * 0.5f, y0 = cy - h * 0.5f;
    const int margin = 5;
    const int ix0 = int(std::floor(x0)) - margin, iy0 = int(std::floor(y0)) - margin;
    const int tw = int(std::ceil(x0 + w)) + margin - ix0, th = int(std::ceil(y0 + h)) + margin - iy0;
    // The glyph resampled into the box: premultiplied colour, 4x4 bilinear taps per pixel.
    std::vector<float> layer(size_t(tw) * th * 4, 0.0f);
    const auto texel = [&](int x, int y, float* out) {
        x = std::clamp(x, 0, int(xbox_glyph_size) - 1);
        y = std::clamp(y, 0, int(xbox_glyph_size) - 1);
        const u8* p = &atlas->rgba[(size_t(g.y + y) * xbox_atlas_width + g.x + x) * 4];
        const float a = p[3] / 255.0f;
        out[0] = p[0] / 255.0f * a, out[1] = p[1] / 255.0f * a, out[2] = p[2] / 255.0f * a, out[3] = a;
    };
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            float sum[4]{};
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float u = g.left + (ix0 + x + (sx + 0.5f) / 4.0f - x0) / w * gw - 0.5f;
                    const float v = g.top + (iy0 + y + (sy + 0.5f) / 4.0f - y0) / h * gh - 0.5f;
                    const int iu = int(std::floor(u)), iv = int(std::floor(v));
                    const float fu = u - iu, fv = v - iv;
                    float t[4][4];
                    texel(iu, iv, t[0]), texel(iu + 1, iv, t[1]);
                    texel(iu, iv + 1, t[2]), texel(iu + 1, iv + 1, t[3]);
                    for (int k = 0; k < 4; ++k) {
                        sum[k] += (t[0][k] * (1 - fu) + t[1][k] * fu) * (1 - fv) +
                                  (t[2][k] * (1 - fu) + t[3][k] * fu) * fv;
                    }
                }
            }
            for (int k = 0; k < 4; ++k) {
                layer[(size_t(y) * tw + x) * 4 + k] = sum[k] / 16.0f;
            }
        }
    }
    if (halo) {
        std::vector<float> shadow(size_t(tw) * th);
        for (size_t i = 0; i < shadow.size(); ++i) {
            shadow[i] = layer[i * 4 + 3];
        }
        BoxBlur(shadow, tw, th, 2);
        BoxBlur(shadow, tw, th, 2);
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                c.Blend(ix0 + x, iy0 + y, {0.0f, 0.0f, 0.0f},
                        std::min(shadow[size_t(y) * tw + x] * 1.5f, 1.0f) * 0.6f);
            }
        }
    }
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            const float* p = &layer[(size_t(y) * tw + x) * 4];
            if (p[3] > 0.0f) {
                c.Blend(ix0 + x, iy0 + y, {p[0] / p[3], p[1] / p[3], p[2] / p[3]}, p[3]);
            }
        }
    }
}

/// The picture as large as fits `width` x `height`, `squash` times as tall (a button seen at an
/// angle).
void DrawGlyphFit(Canvas& c, int glyph, float cx, float cy, float width, float height,
                  float squash = 1.0f, bool halo = true) {
    const XboxAtlas* atlas = Atlas();
    if (!atlas) {
        return;
    }
    const auto& g = atlas->glyph[glyph];
    const float gw = float(g.right - g.left), gh = float(g.bottom - g.top);
    const float scale = std::min(width / gw, height / (gh * squash));
    DrawGlyph(c, glyph, cx, cy, gw * scale, gh * scale * squash, halo);
}

/// The Xbox picture for a picture of the sheets: A B X Y, bumpers, triggers, stick presses, the
/// Menu and View buttons, the sticks, and the d-pad with the lit directions.
int XboxGlyph(u8 icon) {
    if (icon >= DPad) {
        const bool up = icon & LitUp, down = icon & LitDown, left = icon & LitLeft, right = icon & LitRight;
        if ((up || down) && (left || right)) {
            return XDpad;
        }
        if (up && down) {
            return XDpadVertical;
        }
        if (left && right) {
            return XDpadHorizontal;
        }
        return up ? XDpadUp : down ? XDpadDown : left ? XDpadLeft : right ? XDpadRight : XDpad;
    }
    switch (icon) {
    case Cross: return XA;
    case Circle: return XB;
    case Square: return XX;
    case Triangle: return XY;
    case L1: return XLB;
    case R1: return XRB;
    case L2: return XLT;
    case R2: return XRT;
    case L3: return XLeftPress;
    case R3: return XRightPress;
    case Options: return XMenu;
    case Touchpad: return XView;
    case Move: return XLeftStick;
    default: return XRightStick; // Look
    }
}

void DrawXbox(Canvas& c, const Slot& s) {
    // Inset, so the halo stays inside the area the patch covers.
    DrawGlyphFit(c, XboxGlyph(s.icon), (s.x0 + s.x1) * 0.5f, (s.y0 + s.y1) * 0.5f, s.x1 - s.x0 - 6.0f,
                 s.y1 - s.y0 - 6.0f);
}

Color Expand565(u16 v) {
    return {float((v >> 11) & 31) / 31.0f, float((v >> 5) & 63) / 63.0f, float(v & 31) / 31.0f};
}

u16 To565(const float* c) {
    const auto q = [](float v, int max) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * max)); };
    return u16((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

/// BC1 blocks of an opaque canvas: each block's colour range as end points (bounding box).
std::vector<u8> EncodeBc1(const Canvas& c) {
    const u32 bw = (c.width + 3) / 4, bh = (c.height + 3) / 4;
    std::vector<u8> out(size_t(bw) * bh * 8);
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            float px[16][3];
            float lo[3] = {1.0f, 1.0f, 1.0f}, hi[3] = {0.0f, 0.0f, 0.0f};
            for (u32 i = 0; i < 16; ++i) {
                const u32 x = std::min(bx * 4 + i % 4, c.width - 1);
                const u32 y = std::min(by * 4 + i / 4, c.height - 1);
                for (int k = 0; k < 3; ++k) {
                    px[i][k] = c.px[(size_t(y) * c.width + x) * 4 + k];
                    lo[k] = std::min(lo[k], px[i][k]);
                    hi[k] = std::max(hi[k], px[i][k]);
                }
            }
            u16 c0 = To565(hi), c1 = To565(lo);
            if (c0 < c1) {
                std::swap(c0, c1);
            }
            u32 indices = 0;
            if (c0 != c1) {
                const Color e0 = Expand565(c0), e1 = Expand565(c1);
                const float pal[4][3] = {
                    {e0.r, e0.g, e0.b},
                    {e1.r, e1.g, e1.b},
                    {(2 * e0.r + e1.r) / 3, (2 * e0.g + e1.g) / 3, (2 * e0.b + e1.b) / 3},
                    {(e0.r + 2 * e1.r) / 3, (e0.g + 2 * e1.g) / 3, (e0.b + 2 * e1.b) / 3},
                };
                for (u32 i = 0; i < 16; ++i) {
                    u32 best = 0;
                    float best_d = 1e9f;
                    for (u32 k = 0; k < 4; ++k) {
                        const float dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1],
                                    db = px[i][2] - pal[k][2];
                        const float d = dr * dr + dg * dg + db * db;
                        if (d < best_d) {
                            best_d = d;
                            best = k;
                        }
                    }
                    indices |= best << (2 * i);
                }
            }
            u8* block = &out[(size_t(by) * bw + bx) * 8];
            std::memcpy(block, &c0, 2);
            std::memcpy(block + 2, &c1, 2);
            std::memcpy(block + 4, &indices, 4);
        }
    }
    return out;
}

Color Mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

/// Coverage of a shape over a pixel, from 4x4 samples.
template <typename Inside>
float Supersample(u32 x, u32 y, Inside inside) {
    int hits = 0;
    for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) {
            hits += inside(x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f) ? 1 : 0;
        }
    }
    return hits / 16.0f;
}

/// The key's label (or a mouse) centred on (cx, cy), `squash` times as tall: on a button seen
/// at an angle.
void DrawLabel(Canvas& c, const Font& font, const std::string& key, float cx, float cy,
               float width, float size, float squash) {
    Canvas label{c.width, c.height};
    if (key == "Mouse Left" || key == "Mouse Right" || key == "Mouse Middle") {
        const float h = size * 0.6f;
        DrawMouse(label, {u16(cx - h), u16(cy - h), u16(cx + h), u16(cy + h), 0},
                  key == "Mouse Left" ? 1 : key == "Mouse Right" ? 2 : 4);
    } else {
        font.Draw(label, KeyLabel(key), cx, cy, width, size);
    }
    for (u32 y = 0; y < c.height; ++y) {
        const float sy = cy + (y + 0.5f - cy) / squash;
        if (sy < 0.0f || sy >= c.height) {
            continue;
        }
        for (u32 x = 0; x < c.width; ++x) {
            const float* p = &label.px[(size_t(sy) * c.width + x) * 4];
            c.Blend(x, y, {p[0], p[1], p[2]}, p[3]);
        }
    }
}

/// A round prompt button as a key: dark face, light rim, the bound key.
void DrawRoundButton(Canvas& up, const Font& font, const std::string& key, int xbox = -1) {
    const float cx = 128.5f, cy = 256.0f - 126.5f, radius = 70.0f;
    for (u32 y = 0; y < up.height; ++y) {
        for (u32 x = 0; x < up.width; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - radius;
            up.Blend(x, y, {0.03f, 0.03f, 0.03f}, Coverage(d));
            const float t = std::clamp((y + 0.5f - (cy - radius)) / (2 * radius), 0.0f, 1.0f);
            up.Blend(x, y, Mix(cap_top, cap_bottom, t), Coverage(d + 5.0f));
            up.Blend(x, y, rim, 0.8f * std::min(Coverage(d + 9.0f), 1.0f - Coverage(d + 12.0f)));
        }
    }
    if (xbox >= 0) {
        DrawGlyphFit(up, xbox, cx, cy, 2 * radius - 36.0f, 2 * radius - 36.0f, 1.0f, false);
        return;
    }
    DrawLabel(up, font, key, cx, cy, 2 * radius - 34.0f, 80.0f, 1.0f);
}

/// A shoulder button prompt (L1/R1 bumper, L2/R2 trigger) as a key: dark face, light rim, the
/// bound key. The outlines are measured on the game's pictures (upright); its masks cut them out.
void DrawShoulder(Canvas& up, const Font& font, const std::string& key, u8 shape, int xbox = -1) {
    const bool bumper = shape == Bumper;
    const bool mirror = shape == RightTrigger;
    // Approximate signed distance from the outline (negative inside).
    const auto distance = [&](float x, float y) {
        if (bumper) {
            const float cx = 127.5f, half = 83.0f, top = 76.0f, mid = 112.0f, ry = 62.0f;
            if (y <= mid) {
                return RoundedRect(x, y, cx, mid, half, mid - top, 10.0f);
            }
            const float ex = (x - cx) / half, ey = (y - mid) / ry;
            return (std::sqrt(ex * ex + ey * ey) - 1.0f) * ry;
        }
        if (mirror) {
            x = 257.0f - x;
        }
        // L2: the left side leans out, the right side nearly upright, round top, flat bottom.
        const float left = (66.0f - 0.275f * (y - 112.0f) - x) / 1.0371f;
        const float right = (x - 192.0f - 0.075f * (y - 112.0f)) / 1.0028f;
        float d = std::max({left, right, y - 202.0f});
        if (y < 114.25f) {
            d = std::max(d, std::hypot(x - 129.0f, y - 114.25f) - 62.25f);
        }
        return d;
    };
    const float top = bumper ? 76.0f : 52.0f, bottom = bumper ? 174.0f : 202.0f;
    for (u32 y = 0; y < up.height; ++y) {
        for (u32 x = 0; x < up.width; ++x) {
            const float d = distance(x + 0.5f, y + 0.5f);
            up.Blend(x, y, {0.03f, 0.03f, 0.03f}, Coverage(d));
            const float t = std::clamp((y + 0.5f - top) / (bottom - top), 0.0f, 1.0f);
            up.Blend(x, y, Mix(cap_top, cap_bottom, t), Coverage(d + 5.0f));
            up.Blend(x, y, rim, 0.8f * std::min(Coverage(d + 9.0f), 1.0f - Coverage(d + 12.0f)));
        }
    }
    const float cx = bumper ? 127.5f : mirror ? 128.0f : 129.0f, cy = bumper ? 118.0f : 136.0f;
    const float width = bumper ? 116.0f : 96.0f, height = bumper ? 60.0f : 90.0f;
    if (xbox >= 0) {
        DrawGlyphFit(up, xbox, cx, cy, width, height, 1.0f, false);
        return;
    }
    DrawLabel(up, font, key, cx, cy, width, std::min(height, 72.0f), 1.0f);
}

/// The tapping prompt's raised button (a puck seen from above at an angle) or the hold prompt's
/// pressed one, as a key with the label lying on its face.
void DrawPuck(Canvas& up, const Font& font, const std::string& key, bool pressed, int xbox = -1) {
    // Measured on the game's pictures (upright): face ellipse, and the side down to y 273.
    const float cx = 256.5f, rx = 98.5f, ry = 53.0f, bottom = 273.0f;
    const float face = pressed ? bottom : bottom - 24.0f;
    const auto in_ellipse = [&](float x, float y, float ecy, float grow) {
        const float dx = (x - cx) / (rx + grow), dy = (y - ecy) / (ry + grow);
        return dx * dx + dy * dy <= 1.0f;
    };
    const auto in_side = [&](float x, float y, float grow) {
        const float dx = (x - cx) / (rx + grow);
        if (dx * dx > 1.0f) {
            return false;
        }
        const float h = (ry + grow) * std::sqrt(1.0f - dx * dx);
        return y >= face - h && y <= bottom + h;
    };
    for (u32 y = u32(face - ry - 4); y < u32(bottom + ry + 4); ++y) {
        for (u32 x = u32(cx - rx - 4); x < u32(cx + rx + 4); ++x) {
            up.Blend(x, y, {0.02f, 0.02f, 0.02f},
                     Supersample(x, y, [&](float px, float py) { return in_side(px, py, 1.5f); }));
            if (!pressed) {
                const float shade = 1.0f - std::fabs(x + 0.5f - cx) / rx;
                up.Blend(x, y, Mix({0.04f, 0.04f, 0.04f}, {0.16f, 0.15f, 0.14f}, shade),
                         Supersample(x, y, [&](float px, float py) { return in_side(px, py, 0.0f); }));
            }
            const float t = std::clamp((y + 0.5f - (face - ry)) / (2 * ry), 0.0f, 1.0f);
            up.Blend(x, y, {0.02f, 0.02f, 0.02f}, Supersample(x, y, [&](float px, float py) {
                         return in_ellipse(px, py, face, 0.0f);
                     }));
            up.Blend(x, y, Mix(cap_top, cap_bottom, t), Supersample(x, y, [&](float px, float py) {
                         return in_ellipse(px, py, face, -3.0f);
                     }));
            up.Blend(x, y, rim, 0.8f * Supersample(x, y, [&](float px, float py) {
                         return in_ellipse(px, py, face, -7.0f) && !in_ellipse(px, py, face, -10.0f);
                     }));
        }
    }
    if (xbox >= 0) {
        DrawGlyphFit(up, xbox, cx, face, rx * 1.2f, ry * 1.5f, ry / rx, false);
        return;
    }
    DrawLabel(up, font, key, cx, face, rx * 1.2f, rx * 0.95f, ry / rx);
}

/// The weapon switch d-pad as the four keys bound to it, the lit ones orange.
void DrawHudDPad(Canvas& up, const Font& font, const Bindings& b, u8 lit) {
    constexpr Color hud_orange{0.969f, 0.694f, 0.396f};
    struct Key {
        float cx, cy;
        int binding;
        u8 lit;
    };
    // The centres of the arms of the game's masks (upright).
    const Key keys[4] = {{127.5f, 62.0f, 0, LitUp}, {127.5f, 194.0f, 1, LitDown},
                         {62.0f, 128.0f, 2, LitLeft}, {193.0f, 128.0f, 3, LitRight}};
    const float half = 30.0f;
    for (const Key& k : keys) {
        DrawCap(
            up, k.cx - half, k.cy - half, k.cx + half, k.cy + half,
            [&](float x, float y) { return RoundedRect(x, y, k.cx, k.cy, half, half, 9.0f); },
            (lit & k.lit) ? &hud_orange : nullptr);
        font.Draw(up, KeyLabel(b.dpad[k.binding]), k.cx, k.cy, 2 * half - 8.0f, 34.0f);
    }
}

/// The weapon switch d-pad as the Xbox d-pad, the lit direction in red.
void DrawXboxHudDPad(Canvas& up, u8 lit) {
    DrawGlyphFit(up, XboxGlyph(DPad | lit), 127.5f, 128.0f, 184.0f, 184.0f, 1.0f, false);
}

std::vector<Patch> BuildPicture(const Picture& picture, u32 levels, Style style) {
    const Bindings bindings = LoadBindings();
    const Font font;
    const u32 size = picture.size;
    Canvas up{size, size};
    const Color glow{picture.glow[0] / 255.0f, picture.glow[1] / 255.0f, picture.glow[2] / 255.0f};
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            up.Blend(x, y, glow, 1.0f);
        }
    }
    const int xbox = style == Style::Xbox && picture.shape != HudDPad ? XboxGlyph(picture.icon) : -1;
    switch (picture.shape) {
    case RoundButton:
        DrawRoundButton(up, font, bindings.keys[picture.icon], xbox);
        break;
    case Puck:
    case PressedPuck:
        DrawPuck(up, font, bindings.keys[picture.icon], picture.shape == PressedPuck, xbox);
        break;
    case Bumper:
    case LeftTrigger:
    case RightTrigger:
        DrawShoulder(up, font, bindings.keys[picture.icon], picture.shape, xbox);
        break;
    case HudDPad:
        if (style == Style::Xbox) {
            DrawXboxHudDPad(up, picture.icon);
        } else {
            DrawHudDPad(up, font, bindings, picture.icon);
        }
        break;
    }
    // Upside down like the game's pictures.
    Canvas level{size, size};
    for (u32 y = 0; y < size; ++y) {
        std::memcpy(&level.px[size_t(y) * size * 4], &up.px[size_t(size - 1 - y) * size * 4],
                    size * 4 * sizeof(float));
    }
    std::vector<Patch> patches;
    for (u32 mip = 0; mip < std::max(levels, 1u); ++mip) {
        patches.push_back({mip, 0, 0, level.width, level.height, EncodeBc1(level)});
        if (level.width == 1 && level.height == 1) {
            break;
        }
        level = Downsample(level);
    }
    return patches;
}

/// The next mip level: 2x2 averages, colour weighted by alpha.
Canvas Downsample(const Canvas& level) {
    Canvas next{std::max(level.width / 2, 1u), std::max(level.height / 2, 1u)};
    for (u32 y = 0; y < next.height; ++y) {
        for (u32 x = 0; x < next.width; ++x) {
            float sum[4]{};
            for (u32 dy = 0; dy < 2; ++dy) {
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 sx = std::min(x * 2 + dx, level.width - 1);
                    const u32 sy = std::min(y * 2 + dy, level.height - 1);
                    const float* p = &level.px[(size_t(sy) * level.width + sx) * 4];
                    for (int k = 0; k < 3; ++k) {
                        sum[k] += p[k] * p[3];
                    }
                    sum[3] += p[3];
                }
            }
            float* out = &next.px[(size_t(y) * next.width + x) * 4];
            for (int k = 0; k < 3; ++k) {
                out[k] = sum[3] > 0.0f ? sum[k] / sum[3] : 0.0f;
            }
            out[3] = sum[3] * 0.25f;
        }
    }
    return next;
}

/// The d-pad as the four keys bound to it, arrow-key style (one on top, three below); the
/// directions the picture lights are orange.
void DrawArrows(Canvas& c, const Font& font, const Slot& s, const Bindings& b) {
    const float gap = 2.0f;
    const float k = std::floor(std::min((s.x1 - s.x0 - 2 * gap) / 3.0f, (s.y1 - s.y0 - gap) / 2.0f));
    const float cx = (s.x0 + s.x1) * 0.5f, cy = (s.y0 + s.y1) * 0.5f;
    struct Key {
        float x, y;
        int binding; ///< index in Bindings::dpad
        u8 lit;
    };
    const Key keys[4] = {
        {cx - k * 0.5f, cy - k - gap * 0.5f, 0, LitUp},
        {cx - k * 1.5f - gap, cy + gap * 0.5f, 2, LitLeft},
        {cx - k * 0.5f, cy + gap * 0.5f, 1, LitDown},
        {cx + k * 0.5f + gap, cy + gap * 0.5f, 3, LitRight},
    };
    for (const Key& key : keys) {
        const float kx = key.x + k * 0.5f, ky = key.y + k * 0.5f;
        DrawCap(
            c, key.x, key.y, key.x + k, key.y + k,
            [&](float x, float y) { return RoundedRect(x, y, kx, ky, k * 0.5f, k * 0.5f, k * 0.22f); },
            (s.icon & key.lit) ? &highlight : nullptr);
        font.Draw(c, KeyLabel(b.dpad[key.binding]), kx, ky, k - 4.0f, k * 0.62f);
    }
}

/// For a colour sheet sampled apart from its mask: transparent pixels take the colour of their
/// covered neighbours, so filtering at the edges doesn't pull in black (the game's own colour
/// sheet extends past its mask the same way).
void BleedColour(Canvas& c, int passes) {
    for (int pass = 0; pass < passes; ++pass) {
        const std::vector<float> src = c.px;
        for (u32 y = 0; y < c.height; ++y) {
            for (u32 x = 0; x < c.width; ++x) {
                float* p = &c.px[(size_t(y) * c.width + x) * 4];
                if (src[(size_t(y) * c.width + x) * 4 + 3] > 0.0f) {
                    continue;
                }
                float sum[3]{}, weight = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = int(x) + dx, sy = int(y) + dy;
                        if (sx < 0 || sy < 0 || sx >= int(c.width) || sy >= int(c.height)) {
                            continue;
                        }
                        const float* q = &src[(size_t(sy) * c.width + sx) * 4];
                        const bool filled = q[3] > 0.0f || q[0] + q[1] + q[2] > 0.0f;
                        if (filled) {
                            for (int k = 0; k < 3; ++k) {
                                sum[k] += q[k];
                            }
                            weight += 1.0f;
                        }
                    }
                }
                if (weight > 0.0f) {
                    for (int k = 0; k < 3; ++k) {
                        p[k] = sum[k] / weight;
                    }
                }
            }
        }
    }
}

/// One canvas pixel as the sheet's four bytes.
void StorePixel(u8* out, const float* src, Layer layer) {
    const auto byte = [](float v) { return u8(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    switch (layer) {
    case Layer::Rgba:
        for (int k = 0; k < 4; ++k) {
            out[k] = byte(src[k]);
        }
        break;
    case Layer::Colour:
        out[0] = byte(src[0]), out[1] = byte(src[1]), out[2] = byte(src[2]), out[3] = 0;
        break;
    case Layer::Mask:
        out[0] = out[1] = out[2] = byte(src[3]), out[3] = 0;
        break;
    }
}

std::vector<Patch> Build(const Sheet& sheet, u32 levels, Style style) {
    const Bindings bindings = LoadBindings();
    const Font font;
    Canvas canvas{sheet.width, sheet.height};
    for (const Slot& s : sheet.slots) {
        if (style == Style::Xbox) {
            DrawXbox(canvas, s);
        } else if (s.icon >= DPad) {
            DrawArrows(canvas, font, s, bindings);
        } else if (s.icon < Actions) {
            DrawBinding(canvas, font, s, bindings.keys[s.icon]);
        } else if (s.icon == Look && bindings.mouse_look) {
            DrawMouse(canvas, s, 0);
        } else {
            // Moving (WASD) or looking with keys: the four keys on one cap.
            std::string label;
            for (int i = 0; i < 4; ++i) {
                label += KeyLabel(s.icon == Move ? bindings.move[i] : "");
            }
            DrawKey(canvas, font, s, s.icon == Move ? label : "LOOK");
        }
    }

    std::vector<Patch> patches;
    Canvas level = canvas;
    for (u32 mip = 0; mip < std::max(levels, 1u); ++mip) {
        if (sheet.layer == Layer::Colour) {
            BleedColour(level, 4); // past the 3 px around each picture that the patches cover
        }
        for (const Slot& s : sheet.slots) {
            // The picture's bounds and its glow (3 px; the pictures are 8+ px apart).
            const u32 x0 = (std::max(int(s.x0) - 3, 0)) >> mip;
            const u32 y0 = (std::max(int(s.y0) - 3, 0)) >> mip;
            const u32 x1 = std::min((std::min(s.x1 + 3u, sheet.width) + (1u << mip) - 1) >> mip,
                                    level.width);
            const u32 y1 = std::min((std::min(s.y1 + 3u, sheet.height) + (1u << mip) - 1) >> mip,
                                    level.height);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            Patch p{mip, x0, y0, x1 - x0, y1 - y0, {}};
            p.data.resize(size_t(p.width) * p.height * 4);
            for (u32 y = 0; y < p.height; ++y) {
                for (u32 x = 0; x < p.width; ++x) {
                    const float* src = &level.px[(size_t(y0 + y) * level.width + x0 + x) * 4];
                    StorePixel(&p.data[(size_t(y) * p.width + x) * 4], src, sheet.layer);
                }
            }
            patches.push_back(std::move(p));
        }
        if (level.width == 1 && level.height == 1) {
            break;
        }
        level = Downsample(level);
    }
    return patches;
}

/// The stick prompt's frame with the bound rotate key on the cap instead of the "L": the cap
/// face's colour, a dark halo and the label, drawn into the blocks around the "L" (at the mip
/// levels it is seen at).
std::vector<Patch> BuildStick(const StickFrame& frame, u32 levels) {
    const Bindings bindings = LoadBindings();
    const Font font;
    const std::string& key = bindings.keys[frame.clockwise ? RotCw : RotCcw];
    // The label drawn upright around (100, 100) at size 100, with a halo plane.
    constexpr int ref_size = 200;
    Canvas ref{ref_size, ref_size};
    if (key == "Mouse Left" || key == "Mouse Right" || key == "Mouse Middle") {
        DrawMouse(ref, {74, 52, 126, 148, 0}, key == "Mouse Left" ? 1 : key == "Mouse Right" ? 2 : 4);
    } else {
        font.Draw(ref, KeyLabel(key), 100.0f, 100.0f, 84.0f, 100.0f, {0.957f, 0.941f, 0.792f});
    }
    std::vector<float> halo(size_t(ref_size) * ref_size);
    for (size_t i = 0; i < halo.size(); ++i) {
        halo[i] = ref.px[i * 4 + 3];
    }
    BoxBlur(halo, ref_size, ref_size, 3);
    BoxBlur(halo, ref_size, ref_size, 3);
    const float det = frame.m[0] * frame.m[3] - frame.m[1] * frame.m[2];
    const float inv[4] = {frame.m[3] / det, -frame.m[1] / det, -frame.m[2] / det, frame.m[0] / det};
    // Bilinear lookup of the label (colour, alpha) and halo at a picture pixel position.
    const auto at = [&](int x, int y, int k) {
        return k < 4 ? ref.px[(size_t(y) * ref_size + x) * 4 + k] : halo[size_t(y) * ref_size + x];
    };
    const auto sample = [&](float px, float py, float* out) {
        const float dx = px - frame.cx, dy = py - frame.cy;
        const float u = inv[0] * dx + inv[1] * dy + 100.0f - 0.5f;
        const float v = inv[2] * dx + inv[3] * dy + 100.0f - 0.5f;
        out[0] = out[1] = out[2] = out[3] = out[4] = 0.0f;
        if (u < 0.0f || v < 0.0f || u >= ref_size - 1 || v >= ref_size - 1) {
            return;
        }
        const int iu = int(u), iv = int(v);
        const float fu = u - iu, fv = v - iv;
        for (int k = 0; k < 5; ++k) {
            out[k] = (at(iu, iv, k) * (1 - fu) + at(iu + 1, iv, k) * fu) * (1 - fv) +
                     (at(iu, iv + 1, k) * (1 - fu) + at(iu + 1, iv + 1, k) * fu) * fv;
        }
    };
    // The face colour at a picture position: bilinear between the measured points.
    const auto face = [&](float px, float py, float* out) {
        const float gx = std::clamp((px - frame.cx) / stick_grid_step + stick_grid / 2, 0.0f, stick_grid - 1.001f);
        const float gy = std::clamp((py - frame.cy) / stick_grid_step + stick_grid / 2, 0.0f, stick_grid - 1.001f);
        const int ix = int(gx), iy = int(gy);
        const float fx = gx - ix, fy = gy - iy;
        const auto point = [&](int x, int y, int k) { return float(frame.face[(y * stick_grid + x) * 3 + k]); };
        for (int k = 0; k < 3; ++k) {
            out[k] = ((point(ix, iy, k) * (1 - fx) + point(ix + 1, iy, k) * fx) * (1 - fy) +
                      (point(ix, iy + 1, k) * (1 - fx) + point(ix + 1, iy + 1, k) * fx) * fy) / 255.0f;
        }
    };
    constexpr float half_x = 27.0f, up = 33.0f, down = 38.0f; // the part of the face repainted: the "L" with its halo
    std::vector<Patch> patches;
    for (u32 mip = 0; mip < std::min(std::max(levels, 1u), 3u); ++mip) {
        const float scale = float(1u << mip);
        const int w = int(stick_width >> mip), h = int(stick_height >> mip);
        const int x0 = std::max(int(std::floor((frame.cx - half_x) / scale / 4.0f)) * 4, 0);
        const int y0 = std::max(int(std::floor((frame.cy - up) / scale / 4.0f)) * 4, 0);
        const int x1 = std::min(int(std::ceil((frame.cx + half_x) / scale / 4.0f)) * 4, w);
        const int y1 = std::min(int(std::ceil((frame.cy + down) / scale / 4.0f)) * 4, h);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        Canvas region{u32(x1 - x0), u32(y1 - y0)};
        constexpr int taps = 4;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                float sum[3]{};
                for (int sy = 0; sy < taps; ++sy) {
                    for (int sx = 0; sx < taps; ++sx) {
                        const float px = (x + (sx + 0.5f) / taps) * scale;
                        const float py = (y + (sy + 0.5f) / taps) * scale;
                        float c[3];
                        face(px, py, c);
                        float s[5];
                        sample(px, py, s);
                        const float halo_a = std::min(s[4] * 1.8f, 1.0f) * 0.75f;
                        for (int k = 0; k < 3; ++k) {
                            c[k] *= 1.0f - halo_a;                     // dark halo
                            c[k] = c[k] * (1.0f - s[3]) + s[k] * s[3]; // the label
                            sum[k] += c[k];
                        }
                    }
                }
                float* out = &region.px[(size_t(y - y0) * region.width + (x - x0)) * 4];
                out[0] = sum[0] / (taps * taps), out[1] = sum[1] / (taps * taps);
                out[2] = sum[2] / (taps * taps), out[3] = 1.0f;
            }
        }
        patches.push_back({mip, u32(x0), u32(y0), region.width, region.height, EncodeBc1(region)});
    }
    return patches;
}

} // namespace

namespace {
std::atomic<int> device_used{0}; ///< 0 keyboard and mouse, 1 PlayStation pad, 2 Xbox or other pad
std::atomic<u32> switches{0};

enum class Force : u8 { Auto, Off, Keyboard, Xbox };

/// BB_BUTTON_PROMPTS: unset or auto follows the device used last; 0 (or off, playstation) keeps the
/// game's own pictures; keyboard or xbox shows that style always.
Force Forced() {
    static const Force forced = [] {
        const char* env = std::getenv("BB_BUTTON_PROMPTS");
        if (!env || !*env || !std::strcmp(env, "auto")) {
            return Force::Auto;
        }
        if (!std::strcmp(env, "keyboard")) {
            return Force::Keyboard;
        }
        if (!std::strcmp(env, "xbox")) {
            return Force::Xbox;
        }
        return !std::strcmp(env, "0") || !std::strcmp(env, "off") || !std::strcmp(env, "playstation")
                   ? Force::Off
                   : Force::Auto;
    }();
    return forced;
}
} // namespace

void SetDevice(int device) {
    device = std::clamp(device, 0, 2);
    if (device_used.exchange(device) != device) {
        switches.fetch_add(1, std::memory_order_release);
        static constexpr const char* names[] = {"keyboard keys", "PlayStation controller used, PlayStation buttons",
                                                "Xbox controller used, Xbox buttons"};
        std::printf("Button prompts: %s\n", names[device]);
    }
}

Style Shown() {
    switch (Forced()) {
    case Force::Off:
        return Style::Original;
    case Force::Keyboard:
        return Style::Keyboard;
    case Force::Xbox:
        return Style::Xbox;
    case Force::Auto:
        break;
    }
    const int device = device_used.load(std::memory_order_relaxed);
    return device == 0 ? Style::Keyboard : device == 1 ? Style::Original : Style::Xbox;
}

u32 Switches() {
    // BB_BUTTON_PROMPTS_FILE=<path>: the first word of that file (ps, xbox or keyboard) is the device
    // used last, read twice a second; for runs without a controller (scripted runs ignore real ones).
    static const char* path = std::getenv("BB_BUTTON_PROMPTS_FILE");
    if (path && *path) {
        static auto checked = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - checked > std::chrono::milliseconds(500)) {
            checked = now;
            char word[16]{};
            if (FILE* f = std::fopen(path, "r")) {
                if (std::fscanf(f, "%15s", word) == 1) {
                    SetDevice(!std::strcmp(word, "ps") ? 1 : !std::strcmp(word, "xbox") ? 2 : 0);
                }
                std::fclose(f);
            }
        }
    }
    return switches.load(std::memory_order_acquire);
}

bool Candidate(u32 width, u32 height, Format format) {
    if (Forced() == Force::Off) {
        return false;
    }
    if (format == Format::Bc1) {
        for (const Picture& picture : pictures) {
            if (picture.size == width && picture.size == height) {
                return true;
            }
        }
        return width == stick_width && height == stick_height;
    }
    for (const Sheet& sheet : sheets) {
        if (sheet.width == width && sheet.height == height) {
            return true;
        }
    }
    return false;
}

const std::vector<Patch>* Find(u64 hash, u32 width, u32 height, u32 levels, Format format) {
    if (Forced() == Force::Off) {
        return nullptr;
    }
    static std::mutex mutex;
    static std::map<std::tuple<u64, u32, Style>, std::vector<Patch>> built;
    static const std::vector<Patch> original;
    const Style style = Shown();
    std::scoped_lock lock{mutex};
    const auto known_sheet = [&]() -> const Sheet* {
        for (const Sheet& sheet : sheets) {
            if (format == Format::Rgba8 && sheet.hash == hash && sheet.width == width &&
                sheet.height == height) {
                return &sheet;
            }
        }
        return nullptr;
    };
    const auto known_picture = [&]() -> const Picture* {
        for (const Picture& picture : pictures) {
            if (format == Format::Bc1 && picture.hash == hash && picture.size == width &&
                picture.size == height) {
                return &picture;
            }
        }
        return nullptr;
    };
    const auto known_stick = [&]() -> const StickFrame* {
        for (const StickFrame& frame : stick_frames) {
            if (format == Format::Bc1 && frame.hash == hash && width == stick_width &&
                height == stick_height) {
                return &frame;
            }
        }
        return nullptr;
    };
    const Sheet* sheet = known_sheet();
    const Picture* picture = sheet ? nullptr : known_picture();
    const StickFrame* stick = sheet || picture ? nullptr : known_stick();
    if (!sheet && !picture && !stick) {
        return nullptr;
    }
    // A known picture is shown as the game drew it. The stick prompt keeps its "L" for Xbox
    // controllers too (the left stick's own letter).
    if (style == Style::Original || (stick && style == Style::Xbox)) {
        return &original;
    }
    const auto key = std::make_tuple(hash, levels, style);
    if (const auto it = built.find(key); it != built.end()) {
        return &it->second;
    }
    const char* look = style == Style::Xbox ? "Xbox buttons" : "keyboard and mouse keys";
    if (sheet) {
        auto& patches = built[key] = Build(*sheet, levels, style);
        std::printf("Button prompts: %s button pictures drawn as %s (%zu pictures, %u mip levels)\n",
                    sheet->name, look, sheet->slots.size(), levels);
        return &patches;
    }
    if (stick) {
        auto& patches = built[key] = BuildStick(*stick, levels);
        std::printf("Button prompts: %s drawn with the rotate key (%u mip levels)\n", stick->name,
                    levels);
        return &patches;
    }
    auto& patches = built[key] = BuildPicture(*picture, levels, style);
    std::printf("Button prompts: %s drawn as %s (%u mip levels)\n", picture->name, look, levels);
    return &patches;
}

} // namespace VideoCore::ButtonPrompts
