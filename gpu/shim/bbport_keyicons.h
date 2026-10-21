// SPDX-License-Identifier: GPL-2.0-or-later
// Keyboard labels on the controller button icons the games draw in their prompts.
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbKeyIcons {

/// BB_KEY_ICONS=1 turns the key icons on (off by default).
bool Enabled();

/// U1: XXH3-64 of the guest bytes of the 512x512 RGBA8 atlas that holds the button icons.
constexpr uint64_t U1AtlasHash = 0xad5ada34fd34c76bull;

/// The device the player used last (the window calls this): 0 keyboard and mouse (the key and mouse pastilles, the reworded
/// hints, the zoomed prompts), 1 PlayStation controller (the game's own icons, texts and prompts), 2 Xbox controller (the atlas
/// is painted with the Xbox buttons). Every change makes the atlas and the HUD buttons be uploaded again.
void SetInputMode(int mode);
int InputMode();
uint32_t ModeGeneration();

/// A game menu is open (bbgpu_menu_state): the atlas shows the menu keys (Enter, Esc, Alt, Backspace) on the
/// cross, circle, options and R1 icons instead of the keys of the play. MenuGeneration changes at each switch;
/// the texture cache uploads the atlas again then.
void SetMenuMode(bool open);
/// Whether the menu screen shows the Alt prompt (resume, options): only then the triangle icon is painted as Alt.
void SetMenuAlt(bool shown);
bool MenuMode();
/// A draw sampled the icon atlas (the rasterizer calls this); bbgpu_atlas_recent(ms) tells whether one did lately.
void NoteAtlasDraw();
uint32_t MenuGeneration();
/// The key bindings changed (input.ini): the atlas is painted again with the new keys.
void ForceRepaint();
/// While a menu is open the pictures of the pastilles alternate (keys, then mouse): called by the draws, it flips the phase every ~1.4 s.
void TickMenuPhase();

/// Redraws the face buttons, shoulder buttons, triggers, stick clicks, Options and the touchpad
/// button of the U1 atlas (512x512 RGBA8, tightly packed), and labels the lit arms of its d-pad
/// frames, with the keys from input.ini (BB_INPUT_CONFIG, else the port's default keys) as key
/// caps and mouse pictures. Returns false when no icon was changed.
bool PatchU1Atlas(uint8_t* rgba, uint32_t width, uint32_t height);

/// U1 (English): a few tutorial hints are reworded in memory so that the text and the key icon agree
/// (the click of the stick uses the L3 icon, the stick hints use |LA| / |RA|...: see bbport_hints.cpp).
/// Called when the atlas has been repainted; the search runs on its own thread a few seconds later.
void RequestHintPatch();
/// The device the player uses (0 keyboard and mouse, 1 PlayStation pad, 2 Xbox pad), for the texts of the game: the hints are reworded
/// for the keyboard and mouse, the shoulder names of the Controls menu are LB / RB and LT / RT for an Xbox pad. What was changed in
/// memory is put back or made again by the same background search.
void SetHintsMode(int mode);

/// Elena is on the jetski (U1): the cross and square icons show the keys of forward and back (the throttle and the
/// brake are on those keys there); the atlas is painted again at each switch.
void SetJetskiMode(bool on);

/// The HUD buttons that are BC1 or BC4 (alpha mask) textures of their own (the green "interact"
/// button and its press frames, the "rotate the stick" frames and their masks): the sizes worth
/// hashing, whether a hash is one of them, and the repaint. data holds the mip levels back to
/// back from mip 0 (size bytes, levels of them); false when nothing changed.
bool IsTextureCandidate(uint32_t width, uint32_t height);
bool IsTextureTarget(uint64_t hash);

/// Discovery: a BC1 texture of the candidate sizes that no repaint handles but that looks like a HUD
/// button (mostly one flat bright color) is printed once, with its hash, the moment the game draws it,
/// so a new prompt shows up in the run log without a texture dump. data is the guest bytes, mip 0 first.
void NoteUnknownTexture(uint64_t hash, uint32_t width, uint32_t height, const uint8_t* data, size_t size);
bool PatchU1Texture(uint64_t hash, uint8_t* data, size_t size, uint32_t levels);

/// Prompts the game draws at another size or place than the others: 1 the "rotate the stick" frames
/// (about 3 times too small), 2 the d-pad "V" (higher than the "E"), 3 the journal (its bottom higher
/// than the "V"), 4 the press frames of the "E" (their bottom as low as the flat "E"); 0 for any other texture. The draws
/// that sample one of their textures are zoomed by `zoom` about the point (anchor_x, anchor_y) and
/// moved down by `down`, fractions of the render target, so the result is the same at any
/// resolution. BB_STICK_ZOOM=<zoom>,<x>,<y> and BB_DPAD_DOWN=<down> override the values.
int PromptKind(uint64_t hash);

/// A mask texture whose halo blinks with a phase painted into it (the pressed "E"): the texture cache
/// has to upload it again every few tenths of a second while it is drawn.
bool IsPulseMask(uint64_t hash);
void PromptTransform(int kind, float* zoom, float* anchor_x, float* anchor_y, float* down, float* right);

} // namespace BbKeyIcons
