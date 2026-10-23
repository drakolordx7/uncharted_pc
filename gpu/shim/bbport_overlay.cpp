// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_keyicons.h"
#include "bbport_settings.h"
#include "../bbgpu.h"
#include "common/path_util.h"
#include <miniz.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// window.cpp: a display mode change is applied on the thread that owns the SDL window.
extern "C" void bbgpu_display_mode_request(int mode);

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)
extern "C" int runtime_savedata_wait_idle(unsigned timeout_ms); // bb-probe (runtime_savedata.c)
#ifdef _WIN32
// winmm (linked): MCI plays the custom trophy sound, MP3 or WAV. Declared here to keep <windows.h> out.
extern "C" __declspec(dllimport) unsigned long __stdcall mciSendStringW(const wchar_t* command,
                                                                       wchar_t* reply, unsigned size,
                                                                       void* window);
#endif

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
// The keys that open and close the menu (input.ini key.overlay through bbgpu_overlay_key_configure),
// guarded by imgui_mutex. Insert until the runtime says otherwise.
struct ToggleKey {
    int kind = 0; // BBGPU_OVERLAY_KEY_*
    int code = 0;
};
ToggleKey toggle_keys[2] = {{BBGPU_OVERLAY_KEY_SCANCODE, SDL_SCANCODE_INSERT}, {}};

// Whether the key types a character (letters, digits, punctuation, Space, number pad). Bound as the
// menu key, it is the menu key only while nothing is being typed.
bool TypesCharacter(const SDL_KeyboardEvent& key) {
    const SDL_Keycode code = key.key;
    if (code >= SDLK_SPACE && code <= 0x10FFFF && code != SDLK_DELETE) {
        return true;
    }
    return (key.scancode >= SDL_SCANCODE_KP_DIVIDE && key.scancode <= SDL_SCANCODE_KP_PLUS) ||
           (key.scancode >= SDL_SCANCODE_KP_1 && key.scancode <= SDL_SCANCODE_KP_PERIOD) ||
           key.scancode == SDL_SCANCODE_KP_EQUALS;
}

// `typing`: an ImGui text field wants the keyboard (io.WantTextInput) or the game's text dialog is
// open (text_entry_active). A character key bound as the menu key then types instead of toggling;
// the other keys (Insert, F-keys, ...) still do.
bool IsToggleKey(const SDL_KeyboardEvent& key, bool typing) {
    if (typing && TypesCharacter(key)) {
        return false;
    }
    for (const ToggleKey& t : toggle_keys) {
        if ((t.kind == BBGPU_OVERLAY_KEY_SCANCODE && static_cast<int>(key.scancode) == t.code) ||
            (t.kind == BBGPU_OVERLAY_KEY_KEYCODE && static_cast<int>(key.key) == t.code)) {
            return true;
        }
    }
    return false;
}

bool IsToggleButton(u8 button) {
    for (const ToggleKey& t : toggle_keys) {
        if (t.kind == BBGPU_OVERLAY_KEY_MOUSE && t.code == button) {
            return true;
        }
    }
    return false;
}

// "Insert / L3+R3": the title of the menu.
std::string ToggleHint() {
    std::string text;
    for (const ToggleKey& t : toggle_keys) {
        std::string name;
        if (t.kind == BBGPU_OVERLAY_KEY_SCANCODE) {
            name = SDL_GetScancodeName(static_cast<SDL_Scancode>(t.code));
        } else if (t.kind == BBGPU_OVERLAY_KEY_KEYCODE) {
            name = SDL_GetKeyName(static_cast<SDL_Keycode>(t.code));
        } else if (t.kind == BBGPU_OVERLAY_KEY_MOUSE) {
            name = t.code == SDL_BUTTON_MIDDLE ? "Mouse Middle" : t.code == SDL_BUTTON_X1 ? "Mouse 4" : "Mouse 5";
        }
        if (!name.empty()) {
            text += name + " / ";
        }
    }
    return text + "L3+R3";
}
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f, last_present_ms = 0.0f;

// ---- the keyboard and mouse bindings window ("Touches clavier / souris", opened from the game's Commandes menu) ----
// It edits input.ini (BB_INPUT_CONFIG) line by line, keeps every other line, and tells the pad code to read it again.
std::atomic<bool> controls_open{false};
std::atomic<bool> controls_changed{false};
std::vector<std::string> ini_lines;
int capture_index = -1, capture_slot = 0; // the binding being captured: index into ControlActions, slot 0 or 1

// The window speaks the game's language (the four of the dump): 0 English, 1 French, 2 Spanish (Latin America), 3 Brazilian
// Portuguese. src/runtime_pad.c finds it from the text of the "Keyboard/Mouse" row and tells it through bbgpu_set_language.
std::atomic<int> ui_language{1};
// The button next to U2's Commandes menu (SetControlsHint): shown, cursor over it, rectangle in the 1280x720 menu space.
std::atomic<bool> hint_show{false};
std::atomic<bool> hint_hot{false};
std::atomic<float> hint_rect[4];
const char* T(const char* en, const char* fr, const char* es, const char* pt) {
    switch (ui_language.load()) {
    case 0: return en;
    case 2: return es;
    case 3: return pt;
    default: return fr;
    }
}

struct ControlAction {
    const char* name;     // input.ini: key.<name>
    const char* label[4]; // what the key does in the game (as on the controller diagram), in the four languages: en, fr, es, pt
    const char* fallback; // default keys when the file has none
};
constexpr ControlAction ControlActions[] = {
    {"ls_up", {"Move forward", "Avancer", "Avanzar", "Avançar"}, "W"},
    {"ls_down", {"Move back", "Reculer", "Retroceder", "Recuar"}, "S"},
    {"ls_left", {"Move left", "Aller à gauche", "Ir a la izquierda", "Ir para a esquerda"}, "A"},
    {"ls_right", {"Move right", "Aller à droite", "Ir a la derecha", "Ir para a direita"}, "D"},
    {"rs_up", {"Camera up (without the mouse)", "Caméra vers le haut (sans souris)", "Cámara hacia arriba (sin mouse)",
               "Câmera para cima (sem mouse)"}, "I"},
    {"rs_down", {"Camera down (without the mouse)", "Caméra vers le bas (sans souris)", "Cámara hacia abajo (sin mouse)",
                 "Câmera para baixo (sem mouse)"}, "K"},
    {"rs_left", {"Camera left (without the mouse)", "Caméra vers la gauche (sans souris)", "Cámara hacia la izquierda (sin mouse)",
                 "Câmera para a esquerda (sem mouse)"}, "J"},
    {"rs_right", {"Camera right (without the mouse)", "Caméra vers la droite (sans souris)", "Cámara hacia la derecha (sin mouse)",
                  "Câmera para a direita (sem mouse)"}, "L"},
    {"cross", {"Jump / Climb", "Sauter / Escalader", "Saltar / Escalar", "Pular / Escalar"}, "Space"},
    {"circle", {"Take cover / Roll / Drop down", "À couvert / Rouler / Descendre", "Cubrirse / Rodar / Descender",
                "Abrigar-se / Rolar / Descer"}, "C"},
    {"square", {"Melee", "Combat rapproché", "Combate cuerpo a cuerpo", "Combate corpo a corpo"}, "F"},
    {"triangle", {"Interact / Pick up", "Interagir / Ramasser", "Interactuar / Recoger", "Interagir / Pegar"}, "E"},
    {"l1", {"Grenade / Hold to aim", "Grenade / Maintenir pour viser", "Granada / Mantener para apuntar",
            "Granada / Segurar para mirar"}, "G"},
    {"r1", {"Reload", "Recharger", "Recargar", "Recarregar"}, "R"},
    {"l2", {"Aim weapon", "Viser avec l'arme", "Apuntar con el arma", "Mirar com a arma"}, "Mouse Right"},
    {"r2", {"Fire weapon", "Tirer avec l'arme", "Disparar el arma", "Atirar com a arma"}, "Mouse Left"},
    {"l3", {"Switch shoulder while aiming", "Changer d'épaule en visant", "Cambiar de hombro al apuntar",
            "Trocar de ombro ao mirar"}, "Left Shift"},
    {"r3", {"Reset camera", "Réinitialiser la caméra", "Reiniciar la cámara", "Redefinir a câmera"}, "V"},
    {"walk", {"Walk (hold with the movement keys)", "Marcher (à tenir avec les touches de déplacement)",
              "Caminar (mantener con las teclas de movimiento)", "Andar (segurar com as teclas de movimento)"}, ""},
    {"options", {"Pause", "Pause", "Pausa", "Pausa"}, "Escape|Return"},
    {"touchpad", {"Journal", "Journal", "Diario", "Diário"}, "Tab"},
    {"up", {"Look", "Regarder", "Mirar", "Olhar"}, "Up"},
    {"down", {"D-pad down (photo mode removed)", "Croix du bas (mode photo retiré)", "Cruceta abajo (modo foto retirado)",
              "Direcional para baixo (modo foto removido)"}, "Down"},
    {"left", {"Weapon 1 (sidearm)", "Arme 1 (arme de poing)", "Arma 1 (arma corta)", "Arma 1 (arma secundária)"}, "1|Wheel Up"},
    {"right", {"Weapon 2 (main weapon)", "Arme 2 (arme principale)", "Arma 2 (arma principal)", "Arma 2 (arma principal)"}, "2|Wheel Down"},
};
const char* ActionLabel(const ControlAction& action) {
    return action.label[ui_language.load()];
}
constexpr int ControlActionCount = int(sizeof(ControlActions) / sizeof(ControlActions[0]));

std::string IniPath() {
    const char* env = std::getenv("BB_INPUT_CONFIG");
    return env && env[0] ? env : "";
}

void IniLoad() {
    ini_lines.clear();
    const std::string path = IniPath();
    if (path.empty()) {
        return;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        ini_lines.push_back(line);
    }
}

std::string IniGet(const std::string& key, const std::string& fallback) {
    const std::string prefix = key + "=";
    for (const std::string& line : ini_lines) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            return line.substr(prefix.size());
        }
    }
    return fallback;
}

void IniSet(const std::string& key, const std::string& value) {
    const std::string prefix = key + "=";
    for (std::string& line : ini_lines) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            line = prefix + value;
            return;
        }
    }
    ini_lines.push_back(prefix + value);
}

/// Writes the file (a temporary one, then renamed over it) and tells the others.
void IniSave() {
    const std::string path = IniPath();
    if (path.empty()) {
        std::printf("Overlay: BB_INPUT_CONFIG is not set, the bindings cannot be saved\n");
        return;
    }
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const std::string& line : ini_lines) {
            out << line << "\n";
        }
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::printf("Overlay: could not write %s\n", path.c_str());
        return;
    }
    controls_changed = true;
    BbKeyIcons::ForceRepaint();
}

std::string BindingSlot(const std::string& value, int slot) {
    const auto bar = value.find('|');
    if (slot == 0) {
        return value.substr(0, bar);
    }
    return bar == std::string::npos ? std::string{} : value.substr(bar + 1);
}

void SetBinding(int index, int slot, const std::string& name) {
    const ControlAction& action = ControlActions[index];
    const std::string current = IniGet(std::string("key.") + action.name, action.fallback);
    std::string first = BindingSlot(current, 0), second = BindingSlot(current, 1);
    (slot == 0 ? first : second) = name;
    if (first.empty()) {
        first = second;
        second.clear();
    }
    IniSet(std::string("key.") + action.name, second.empty() ? first : first + "|" + second);
    IniSave();
}

/// The binding name for an SDL mouse button, "" for the ones the game cannot use.
std::string MouseButtonName(int button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return "Mouse Left";
    case SDL_BUTTON_RIGHT: return "Mouse Right";
    case SDL_BUTTON_MIDDLE: return "Mouse Middle";
    case SDL_BUTTON_X1: return "Mouse 4";
    case SDL_BUTTON_X2: return "Mouse 5";
    default: return {};
    }
}

void SetOpen(bool value);

void CloseControls() {
    controls_open = false;
    capture_index = -1;
    SetOpen(false);
}

void ControlsWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(780.0f * base_scale, std::min(720.0f * base_scale, viewport->WorkSize.y * 0.92f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    const std::string window_title = std::string(T("Keyboard / Mouse", "Touches clavier / souris", "Teclado / Mouse", "Teclado / Mouse")) + "###keysmouse";
    if (!ImGui::Begin(window_title.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!keep_open) {
            CloseControls();
        }
        return;
    }
    ImGui::TextWrapped("%s", T("Click a key, then press the new key, a mouse button or the wheel. Esc cancels. The × button clears a key. Changes are saved at once.",
                               "Cliquez sur une touche, puis appuyez sur la nouvelle touche, un bouton de la souris ou la molette. Échap annule. Le bouton × efface une touche. Les changements sont enregistrés tout de suite.",
                               "Haz clic en una tecla y luego pulsa la nueva tecla, un botón del mouse o la rueda. Esc cancela. El botón × borra una tecla. Los cambios se guardan al instante.",
                               "Clique em uma tecla e depois pressione a nova tecla, um botão do mouse ou a roda. Esc cancela. O botão × apaga uma tecla. As alterações são salvas na hora."));
    if (IniPath().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", T("The keys file was not found: nothing will be saved.", "Le fichier des touches est introuvable : rien ne sera enregistré.", "No se encuentra el archivo de teclas: no se guardará nada.", "O arquivo das teclas não foi encontrado: nada será salvo."));
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y - 150.0f * base_scale))) {
        ImGui::TableSetupColumn(T("Action", "Action", "Acción", "Ação"), ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn(T("Key 1", "Touche 1", "Tecla 1", "Tecla 1"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(T("Key 2", "Touche 2", "Tecla 2", "Tecla 2"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < ControlActionCount; ++i) {
            const ControlAction& action = ControlActions[i];
            const std::string value = IniGet(std::string("key.") + action.name, action.fallback);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(ActionLabel(action));
            for (int slot = 0; slot < 2; ++slot) {
                ImGui::TableSetColumnIndex(1 + slot);
                ImGui::PushID(i * 2 + slot);
                const bool capturing = capture_index == i && capture_slot == slot;
                const std::string name = BindingSlot(value, slot);
                const std::string text = capturing ? std::string(T("Press a key…", "Appuyez sur une touche…", "Pulsa una tecla…", "Pressione uma tecla…"))
                                                   : name.empty() ? std::string(T("(none)", "(aucune)", "(ninguna)", "(nenhuma)")) : name;
                const float clear_width = ImGui::GetFrameHeight();
                if (ImGui::Button((text + "###bind").c_str(), ImVec2(ImGui::GetContentRegionAvail().x - clear_width - 4.0f, 0.0f))) {
                    capture_index = i;
                    capture_slot = slot;
                }
                ImGui::SameLine();
                if (ImGui::Button("×", ImVec2(clear_width, 0.0f)) && !name.empty()) {
                    SetBinding(i, slot, "");
                    capture_index = -1;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(T("Mouse", "Souris", "Mouse", "Mouse"));
    float sensitivity = float(std::atof(IniGet("mouse_sensitivity", "1.0").c_str()));
    float aim_sensitivity = float(std::atof(IniGet("mouse_aim_sensitivity", "1.0").c_str()));
    bool invert_y = std::atoi(IniGet("mouse_invert_y", "0").c_str()) != 0;
    bool mouse_look = std::atoi(IniGet("mouse_look", "1").c_str()) != 0;
    bool free_smoothing = std::atoi(IniGet("free_view_smoothing", "0").c_str()) != 0;
    bool aim_raw = std::atoi(IniGet("aim_raw", "0").c_str()) != 0;
    bool changed = false;
    if (ImGui::SliderFloat(T("Sensitivity", "Sensibilité", "Sensibilidad", "Sensibilidade"), &sensitivity, 0.1f, 5.0f, "%.2f")) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", sensitivity);
        IniSet("mouse_sensitivity", text);
        changed = true;
    }
    if (ImGui::SliderFloat(T("Sensitivity while aiming", "Sensibilité en visant", "Sensibilidad al apuntar", "Sensibilidade ao mirar"), &aim_sensitivity, 0.1f, 5.0f, "%.2f")) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", aim_sensitivity);
        IniSet("mouse_aim_sensitivity", text);
        changed = true;
    }
    if (ImGui::Checkbox(T("Invert the mouse vertical axis", "Inverser l'axe vertical de la souris", "Invertir el eje vertical del mouse", "Inverter o eixo vertical do mouse"), &invert_y)) {
        IniSet("mouse_invert_y", invert_y ? "1" : "0");
        changed = true;
    }
    if (ImGui::Checkbox(T("Look with the mouse", "Regarder avec la souris", "Mirar con el mouse", "Olhar com o mouse"), &mouse_look)) {
        IniSet("mouse_look", mouse_look ? "1" : "0");
        changed = true;
    }
    if (ImGui::Checkbox(T("Smooth the free camera (game's own smoothing)", "Lisser la vue libre (lissage du jeu)",
                          "Suavizar la cámara libre (suavizado del juego)", "Suavizar a câmera livre (suavização do jogo)"), &free_smoothing)) {
        IniSet("free_view_smoothing", free_smoothing ? "1" : "0");
        changed = true;
    }
    if (ImGui::Checkbox(T("Raw aiming (no smoothing)", "Visée brute (sans lissage)", "Apuntado sin procesar (sin suavizado)",
                          "Mira bruta (sem suavização)"), &aim_raw)) {
        IniSet("aim_raw", aim_raw ? "1" : "0");
        changed = true;
    }
    ImGui::TextDisabled("%s", T("Raw aiming removes the dynamic smoothing: slow mouse movements will show stepping.",
                                "La visée brute supprime le lissage dynamique : les mouvements lents de la souris feront du stepping (paliers).",
                                "El apuntado sin procesar quita el suavizado dinámico: los movimientos lentos del mouse irán a saltos.",
                                "A mira bruta remove a suavização dinâmica: movimentos lentos do mouse ficarão em degraus."));
    if (changed) {
        IniSave();
    }
    ImGui::Separator();
    if (ImGui::Button(T("Defaults", "Valeurs par défaut", "Valores predeterminados", "Padrões"))) {
        for (const ControlAction& action : ControlActions) {
            IniSet(std::string("key.") + action.name, action.fallback);
        }
        IniSave();
        capture_index = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Close", "Fermer", "Cerrar", "Fechar")) || !keep_open) {
        ImGui::End();
        CloseControls();
        return;
    }
    ImGui::End();
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (!value) {
        controls_open = false;
        capture_index = -1;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    // The widget runs first: as one argument list, v could be read before it changed.
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

// Quit game: the confirmation's labels.
struct QuitText {
    const char *button, *title, *body, *quit, *cancel, *quitting;
};
const QuitText& Quitting() {
    static const QuitText en{"Quit game", "Quit the game?",
                             "Progress since the last save is lost. A save in progress is "
                             "finished first.",
                             "Quit", "Cancel", "Quitting..."};
    return en;
}
std::atomic<bool> quitting{false};

// The same exit as closing the window (SDL_EVENT_QUIT ends the window thread's loop), once a save
// in progress has finished (at most 5 seconds).
void Quit() {
    if (quitting.exchange(true)) {
        return;
    }
    std::thread([] {
        const int waited = runtime_savedata_wait_idle(5000);
        std::printf("Quit: from the settings menu, %s\n",
                    waited == 0 ? "no save in progress"
                    : waited > 0 ? "after waiting for the save in progress to finish"
                                 : "a save still in progress after 5 s (quitting anyway)");
        std::fflush(stdout);
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }).detach();
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Menu() {
    if (controls_open) {
        ControlsWindow();
        return;
    }
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // The menu fits whatever window the game has: 620 px wide when there is room, never wider or
    // taller than the game window (the long option list scrolls inside instead of running off the
    // screen) and it can still be resized by hand within those limits.
    const float edge = 20.0f * base_scale;
    const float max_w = std::max(viewport->WorkSize.x - edge * 2.0f, 320.0f * base_scale);
    const float max_h = std::max(viewport->WorkSize.y - edge * 2.0f, 200.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(std::min(620.0f * base_scale, max_w), 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(300.0f * base_scale, max_w),
                                               std::min(160.0f * base_scale, max_h)),
                                        ImVec2(max_w, max_h));
    bool keep_open = true;
    // "###": the window keeps its identity whatever the keys are called.
    const std::string title = "Uncharted — settings  (" + ToggleHint() + ")###bbport_menu";
    if (!ImGui::Begin(title.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);

    ImGui::SeparatorText("Window");
    static const char* display_modes[] = {"Windowed", "Borderless (fullscreen)",
                                         "Exclusive fullscreen"};
    const int display_mode = s.display_mode;
    if (ImGui::BeginCombo("Display mode", display_modes[display_mode])) {
        for (int i = 0; i < BbSettings::DisplayModeCount; ++i) {
            if (ImGui::Selectable(display_modes[i], i == display_mode)) {
                Store(s.display_mode, i, true);
                bbgpu_display_mode_request(i); // the window thread applies it
            }
        }
        ImGui::EndCombo();
    }
    Hint("Windowed keeps the game in its own window. Borderless fills the screen with the desktop's "
         "own mode. Exclusive fullscreen asks the display for a mode of its own at the panel's "
         "native size and minimizes when another window takes the focus; some Wayland "
         "compositors refuse and stay borderless. The launcher's Fullscreen switch picks the "
         "mode at start (on: this choice if it is a fullscreen one, else borderless; off: windowed).");

    ImGui::SeparatorText("Temporal upscaler");
    static const char* upscalers[] = {"Off", "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
                                     "TAA (native anti-aliasing)", "DLSS (NVIDIA RTX)", "XeSS (Intel)"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo("Upscaler", upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                : i == BbSettings::UpscalerXess   ? s.xess_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                // FSR 4 on a capable GPU without its assets (fsr4_shaders, BB_FSR4_DIR).
                const char* fsr4_problem = s.fsr4_problem.load();
                // (4.1.1 offered means the GPU runs FSR 4: only the files can be missing, also
                // while a 4.1.1 problem is the message shown.)
                const bool no_assets = i == BbSettings::UpscalerFsr4 &&
                                       ((fsr4_problem && std::strncmp(fsr4_problem, "assets", 6) == 0) ||
                                        s.fsr411_supported.load());
                ImGui::TextDisabled("%s", no_assets ? "— files not installed"
                                                    : "— not supported by this GPU");
            }
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
    }
    if (const char* problem = s.xess_problem.load(); problem && upscaler == BbSettings::UpscalerXess) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "XeSS: %s", problem);
    }
    // Missing FSR 4 assets are shown next to the disabled entry above, not as a warning here.
    if (const char* problem = s.fsr4_problem.load(); problem && std::strncmp(problem, "assets", 6) != 0) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "FSR 4 unavailable: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("The mode selected above is active. FSR 4 can be selected again.");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint("FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                 "(output matches the DLL). One model for Native..Performance and a separate one "
                 "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (needs the DLLs and Proton).");
        } else {
            Hint("FSR 4 in INT8 mode (the v07 model from the AMD FidelityFX SDK sources). Higher quality "
                 "than FSR 3.1, but a heavier pass. Changing the preset rebuilds the model (a short "
                 "pause). Assets: tools/fetch_fsr4_assets.sh.");
        }
        Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
        Checkbox("FSR 4: invert jitter sign", s.fsr4_invert_jitter);
        Hint("Checks for ghosting: the FSR 4 network normalizes color by exposure and uses it to decide "
             "when to discard previous frames. Changes apply immediately, without a restart.");
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    // Uncharted renders the scene at the game's own size (Display page, BB_GAME_RES): the
    // renderer runs the upscaler as Native AA there and ignores the preset.
    const bool game_size = s.game_size_render.load();
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa || game_size);
    int preset = taa || game_size ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo("Preset", preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, render %dx%d)",
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("TAA anti-aliases the scene at the output resolution, without an FSR model or upscaling. "
                           "The saved FSR preset comes back when FSR is selected.");
    }
    ImGui::Text("Active scene render: %d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("Preset at startup: %s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("With an output other than 1080p the whole game renders at the preset's size (a startup "
                 "patch): fastest on the Steam Deck and weaker GPUs. Preset or output resolution "
                 "changes need a restart. The \"Live resolution changes\" option below allows "
                 "changes without a restart (post-processing then stays at 1080p, slower).");
        } else {
            Hint("BB_RENDER_RES fixes the scene size at startup. Remove that variable to change "
                 "resolution and presets without restarting the game.");
        }
    } else if (game_size) {
        Hint("This game renders the scene at its own resolution (the launcher's Display page): the "
             "upscaler works as anti-aliasing at that size (Native AA), the preset does not lower it.");
    } else {
        Hint("Native AA: FSR works as anti-aliasing. The other presets lower the scene's render "
             "resolution relative to the output. The UI is drawn at the output resolution. "
             "The preset applies from the next frame, without restarting the game.");
    }
    Checkbox("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpening strength", s.sharpness, 0.0f, 2.0f);
    Hint("Up to 1: the upscaler's own sharpening (RCAS). Above 1 an extra RCAS pass is added. "
         "Ctrl+click the slider to type an exact value.");
    ImGui::EndDisabled();
    Checkbox("Sub-pixel jitter", s.jitter);
    Hint("Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
         "detail from several frames. Without it you only get history-based smoothing.");

    ImGui::SeparatorText("Reactive mask");
    ImGui::BeginDisabled(taa);
    Checkbox("Enable mask", s.reactive);
    Hint("Marks transparent effects (particles, haze) so the upscaler relies less on previous "
         "frames. Fewer trails behind effects, but shimmer returns underneath them.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox("Show mask (debug)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox("Character motion vectors", s.object_motion);
    Hint("Exact vectors for animated objects: clothing and weapons break up less in motion. "
         "A static scene gets no extra pass. "
         "The change applies after restarting the game.");
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox("Show motion vectors (debug)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint("Red/green: horizontal/vertical motion (8 pixels = full brightness). "
         "Blue: the pixel got an exact object vector, not just camera motion. "
         "A moving object with neither blue nor red/green is treated by the upscaler "
         "as still, which causes trails.");
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText("Frame generation");
    static const char* frame_gens[] = {"Off", "FSR 3 (any GPU)",
                                       "DLSS (NVIDIA RTX 40/50)"};
    const int frame_gen = s.frame_gen;
    if (ImGui::BeginCombo("Frame generation", frame_gens[frame_gen])) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            const bool supported = i != BbSettings::FrameGenDlss || s.dlssg_max_multiplier > 1;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                Store(s.frame_gen, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— not supported by this GPU");
            }
        }
        ImGui::EndCombo();
    }
    Hint("Frames between two game frames: 60 FPS on screen becomes 120. Needs Mailbox or "
         "Immediate presentation and a monitor with that refresh rate. Adds a little input "
         "latency. Menus and videos are not generated.");
    const int max_multiplier = s.frame_gen == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier.load() : 2;
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff || max_multiplier <= 2);
    int multiplier = std::min(s.frame_gen_multiplier.load(), max_multiplier);
    if (ImGui::SliderInt("Multiplier", &multiplier, 2, std::max(max_multiplier, 2), "%dx")) {
        Store(s.frame_gen_multiplier, multiplier, true);
    }
    ImGui::EndDisabled();
    if (const char* problem = s.frame_gen_problem.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
    }

    ImGui::SeparatorText("Output resolution");
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    ImGui::BeginDisabled(game_size);
    if (ImGui::BeginCombo("Output resolution", outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (game_size) {
        Hint("The frame and the UI have the game's own resolution: set it on the launcher's Display "
             "page (applies after restarting the game).");
    } else if (BbSettings::FixedRenderSession()) {
        Hint("Size of the finished frame and the UI. The preset sets the scene size relative to "
             "the output: 4K Performance = 1920x1080. Applies after restarting the game.");
    } else {
        Hint("The size of the finished frame and UI changes at the next frame boundary. "
             "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
             "Changing it resets FSR's history and may cause a short pause.");
    }
    static const char* live_modes[] = {"Auto (by GPU)", "Off (faster)", "On"};
    int live = s.live_resolution + 1;
    ImGui::BeginDisabled(game_size);
    if (ImGui::BeginCombo("Live resolution changes", live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Hint("On: output resolution and preset change without a restart, but the game's post-processing "
         "stays at 1080p, noticeably slower on the Steam Deck and older GPUs. "
         "Off: everything renders at the preset's size, changes need a restart. Auto turns "
         "it on for strong discrete GPUs. Applies after restarting the game.");
    // No game-effect, cheat or model-detail toggles here: those were Bloodborne patches
    // (scripts/patches.py, patches/Bloodborne.xml), which the Uncharted launch never applies.
    bool restart = s.object_motion != s.startup_object_motion ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Changes apply after restarting the game");
        if (ImGui::Button("Apply and restart the game")) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText("Other");
    Checkbox("FPS counter in the corner", s.show_fps);
    Checkbox("Raw mouse aiming and cover camera", s.gyro_aim);
    ImGui::SameLine();
    ImGui::TextDisabled("(Uncharted 1; also for a gyro set up in Steam as a mouse)");

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::SameLine();
    const QuitText& q = Quitting();
    ImGui::BeginDisabled(quitting);
    if (ImGui::Button(quitting ? q.quitting : q.button)) {
        ImGui::OpenPopup("##quit");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Settings are saved to bbport.ini");
    // The confirmation: mouse, keyboard or controller (D-pad, Cross); Cancel has the focus.
    if (ImGui::BeginPopupModal("##quit", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(q.title);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextDisabled("%s", q.body);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(q.quit)) {
            if (dirty) {
                dirty = false;
                BbSettings::Save();
            }
            Quit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(q.cancel)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text("%.0f FPS  %.1f ms  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                : s.upscaler == BbSettings::UpscalerXess ? "XeSS"
                                                         : "");
    ImGui::End();
}


// ---- Trophy notices (NotifyTrophy): the console's "trophy earned" box, one at a time ----------
struct Toast {
    int grade = 4;
    std::string game, name;
    std::vector<unsigned char> png;
};
std::mutex toast_mutex;
std::deque<Toast> toast_queue;       // guarded by toast_mutex
std::atomic<bool> toast_busy{false}; // something is queued or on screen
bool toast_on = false;
Toast toast_now;
std::chrono::steady_clock::time_point toast_start;
ImTextureData* toast_icon = nullptr; // one 256x256 texture, rewritten for every notice
ImVec2 toast_icon_uv{1.0f, 1.0f};
constexpr float ToastSeconds = 6.0f;
constexpr int ToastIconSize = 256;

// 8-bit RGB/RGBA PNG without interlacing (what the trophy packs hold) to RGBA.
bool DecodePng(const std::vector<unsigned char>& png, std::vector<unsigned char>& rgba, int& width,
               int& height) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 33 || std::memcmp(png.data(), signature, 8) != 0) {
        return false;
    }
    const auto be32 = [](const unsigned char* p) {
        return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
    };
    std::vector<unsigned char> idat;
    int color_type = -1;
    for (size_t at = 8; at + 12 <= png.size();) {
        const u32 length = be32(&png[at]);
        const unsigned char* type = &png[at + 4];
        if (length > png.size() || at + 12 + length > png.size()) {
            return false;
        }
        const unsigned char* body = &png[at + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            width = int(be32(body));
            height = int(be32(body + 4));
            color_type = body[9];
            if (body[8] != 8 || (color_type != 2 && color_type != 6) || body[12] != 0) {
                return false;
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + length;
    }
    if (color_type < 0 || width <= 0 || height <= 0 || width > ToastIconSize ||
        height > ToastIconSize || idat.empty()) {
        return false;
    }
    const int bpp = color_type == 6 ? 4 : 3;
    const size_t stride = size_t(width) * bpp;
    std::vector<unsigned char> raw((stride + 1) * height);
    mz_ulong raw_size = mz_ulong(raw.size());
    if (mz_uncompress(raw.data(), &raw_size, idat.data(), mz_ulong(idat.size())) != MZ_OK ||
        raw_size != raw.size()) {
        return false;
    }
    std::vector<unsigned char> pixels(stride * height);
    for (int y = 0; y < height; ++y) {
        const unsigned char* in = &raw[y * (stride + 1)];
        const int filter = *in++;
        unsigned char* row = &pixels[y * stride];
        const unsigned char* up = y ? row - stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(bpp) ? row[x - bpp] : 0;
            const int b = up ? up[x] : 0;
            const int c = (up && x >= size_t(bpp)) ? up[x - bpp] : 0;
            int predictor = 0;
            switch (filter) {
            case 0: break;
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b),
                          pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return false;
            }
            row[x] = static_cast<unsigned char>(in[x] + predictor);
        }
    }
    rgba.assign(size_t(width) * height * 4, 255);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        std::memcpy(&rgba[i * 4], &pixels[i * bpp], bpp);
    }
    return true;
}

// Puts the notice's icon into the shared texture (the first notice creates it).
void SetToastIcon(const std::vector<unsigned char>& png) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    const bool ok = !png.empty() && DecodePng(png, rgba, w, h);
    if (!ok && !toast_icon) {
        return;
    }
    const bool created = toast_icon == nullptr;
    if (created) {
        toast_icon = IM_NEW(ImTextureData)();
        toast_icon->Create(ImTextureFormat_RGBA32, ToastIconSize, ToastIconSize);
        toast_icon->UseColors = true;
    }
    std::memset(toast_icon->Pixels, 0, size_t(ToastIconSize) * ToastIconSize * 4);
    if (ok) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(toast_icon->GetPixelsAt(0, y), &rgba[size_t(y) * w * 4], size_t(w) * 4);
        }
        toast_icon_uv = ImVec2(float(w) / ToastIconSize, float(h) / ToastIconSize);
    } else {
        toast_icon_uv = ImVec2(0.0f, 0.0f); // nothing to show: an empty square
    }
    if (created) {
        ImGui::RegisterUserTexture(toast_icon);
    } else {
        toast_icon->UpdateRect = {0, 0, ToastIconSize, ToastIconSize};
        toast_icon->Updates.push_back(toast_icon->UpdateRect);
        toast_icon->Status = ImTextureStatus_WantUpdates;
    }
}

// The sound of a notice: user/custom_trophy/trophy.mp3, else trophy.wav (none without either). It plays on
// its own thread, each one under its own MCI alias so a long sound does not block the next notice's.
void PlayTrophySound() {
#ifdef _WIN32
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::CustomTrophy);
    std::filesystem::path file;
    for (const char* name : {"trophy.mp3", "trophy.wav"}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / name, ec)) {
            file = dir / name;
            break;
        }
    }
    if (file.empty()) {
        return;
    }
    static std::atomic<unsigned> count{0};
    const std::wstring alias = L"bb_trophy" + std::to_wstring(count++);
    std::thread([file, alias] {
        const std::wstring open = L"open \"" + file.wstring() + L"\" type mpegvideo alias " + alias;
        if (const unsigned long error = mciSendStringW(open.c_str(), nullptr, 0, nullptr)) {
            std::printf("Trophies: cannot play %s (MCI error %lu)\n",
                        Common::FS::PathToUTF8String(file).c_str(), error);
            return;
        }
        mciSendStringW((L"play " + alias + L" wait").c_str(), nullptr, 0, nullptr);
        mciSendStringW((L"close " + alias).c_str(), nullptr, 0, nullptr);
    }).detach();
#endif
}

void TrophyToast() {
    const auto now = std::chrono::steady_clock::now();
    if (!toast_on) {
        std::scoped_lock lock{toast_mutex};
        if (toast_queue.empty()) {
            toast_busy = false;
            return;
        }
        toast_now = std::move(toast_queue.front());
        toast_queue.pop_front();
        toast_on = true;
        toast_start = now;
        PlayTrophySound();
        SetToastIcon(toast_now.png);
        toast_now.png = {};
    }
    const float t = std::chrono::duration<float>(now - toast_start).count();
    if (t >= ToastSeconds) {
        toast_on = false;
        return;
    }
    const float s = base_scale;
    const float slide = std::min(t / 0.35f, 1.0f);
    const float eased = 1.0f - (1.0f - slide) * (1.0f - slide);
    const float alpha = std::min(1.0f, (ToastSeconds - t) / 0.5f);
    static const ImVec4 colors[] = {ImVec4(0.62f, 0.82f, 0.96f, 1.0f), ImVec4(0.96f, 0.78f, 0.28f, 1.0f),
                                    ImVec4(0.80f, 0.83f, 0.88f, 1.0f), ImVec4(0.82f, 0.52f, 0.24f, 1.0f)};
    static const char* const names[] = {"Platinum trophy earned", "Gold trophy earned",
                                        "Silver trophy earned", "Bronze trophy earned"};
    const int grade = std::clamp(toast_now.grade, 1, 4) - 1;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + (18.0f - (1.0f - eased) * 110.0f) * s),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f * s, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s, 12.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, colors[grade]);
    ImGui::Begin("##trophy_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (toast_icon) {
        ImGui::Image(toast_icon->GetTexRef(), ImVec2(64.0f * s, 64.0f * s), ImVec2(0.0f, 0.0f),
                     toast_icon_uv);
        ImGui::SameLine(0.0f, 12.0f * s);
    }
    ImGui::BeginGroup();
    ImGui::TextColored(colors[grade], "%s", names[grade]);
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(toast_now.name.c_str());
    ImGui::SetWindowFontScale(1.0f);
    if (!toast_now.game.empty()) {
        ImGui::TextDisabled("%s", toast_now.game.c_str());
    }
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (%s)\n", ToggleHint().c_str());
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

void ConfigureToggleKey(int slot, int kind, int code) {
    if (slot < 0 || slot > 1) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    toggle_keys[slot] = {kind, code};
    if (slot == 1) { // the runtime sets both slots in order: log the result once
        std::printf("Overlay: menu key %s\n", ToggleHint().c_str());
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        // A mouse button bound as the menu key never reaches the game, before the menu exists too.
        return (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
               IsToggleButton(event.button.button);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    // The bindings window waiting for a key: the next key, mouse button or wheel step is the new binding.
    if (controls_open && capture_index >= 0) {
        std::string name;
        bool cancel = false;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.key == SDLK_ESCAPE) {
                cancel = true;
            } else if (event.key.key != SDLK_INSERT && event.key.key != SDLK_LALT) { // Insert: the port's menu; left Alt: frees the cursor
                name = SDL_GetScancodeName(event.key.scancode);
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            name = MouseButtonName(event.button.button);
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.y != 0.0f) {
            name = event.wheel.y > 0.0f ? "Wheel Up" : "Wheel Down";
        }
        if (cancel) {
            capture_index = -1;
            return true;
        }
        if (!name.empty()) { // (the click that started the capture was taken before: this one is the choice)
            SetBinding(capture_index, capture_slot, name);
            capture_index = -1;
            return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            return true; // Insert, left Alt and the releases: nothing
        }
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat) {
            if (IsToggleKey(event.key, io.WantTextInput || text_entry_active)) {
                SetOpen(!is_open);
                return true;
            }
            if (is_open && event.key.key == SDLK_ESCAPE) {
                SetOpen(false);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (IsToggleButton(event.button.button)) {
            // Press and release both stay out of the game, like the menu key on the keyboard.
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                SetOpen(!is_open);
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

/// The game's Commandes menu asks for the keyboard and mouse bindings window.
void SetLanguage(int language) {
    ui_language = language >= 0 && language <= 3 ? language : 1;
}

void SetControlsHint(bool show, bool hot, float x, float y, float w, float h) {
    hint_show = show;
    hint_hot = hot;
    hint_rect[0] = x;
    hint_rect[1] = y;
    hint_rect[2] = w;
    hint_rect[3] = h;
}

void OpenControls() {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return;
    }
    IniLoad();
    capture_index = -1;
    controls_open = true;
    SetOpen(true);
}

/// True once after the bindings file was written: the pad code reads it again.
bool ControlsChanged() {
    return controls_changed.exchange(false);
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || hint_show || BbSettings::Get().show_fps || toast_busy);
}

void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char> png) {
    std::scoped_lock lock{toast_mutex};
    if (toast_queue.size() < 32) {
        toast_queue.push_back({grade, std::move(game), std::move(name), std::move(png)});
        toast_busy = true;
    }
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

// Called for every presented frame: the interval for the FPS readout and BB_FPS_LOG.
void NotePresent() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_present_ms = ms;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // BB_FPS_LOG=1: presents per second and the slowest frame, every 5 seconds (test runs).
    static const bool fps_log = [] {
        const char* env = std::getenv("BB_FPS_LOG");
        return env && env[0] == '1';
    }();
    if (fps_log) {
        static auto window_start = now;
        static int frames = 0;
        static float worst = 0.0f;
        ++frames;
        worst = std::max(worst, ms < 1000.0f ? ms : 0.0f);
        const float elapsed = std::chrono::duration<float>(now - window_start).count();
        if (elapsed >= 5.0f) {
            std::printf("Frame rate: %.1f FPS, slowest frame %.1f ms\n", frames / elapsed, worst);
            window_start = now;
            frames = 0;
            worst = 0.0f;
        }
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const float ms = last_present_ms;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (hint_show && !menu_open && !controls_open) {
        // the game's menu space is 1280x720, centred in the window and scaled to fit
        const float ui_scale = std::min(float(extent.width) / 1280.0f, float(extent.height) / 720.0f);
        const float ox = (float(extent.width) - 1280.0f * ui_scale) * 0.5f;
        const float oy = (float(extent.height) - 720.0f * ui_scale) * 0.5f;
        const ImVec2 a(ox + hint_rect[0] * ui_scale, oy + hint_rect[1] * ui_scale);
        const ImVec2 b(a.x + hint_rect[2] * ui_scale, a.y + hint_rect[3] * ui_scale);
        const bool hot = hint_hot;
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        draw->AddRectFilled(a, b, hot ? IM_COL32(110, 72, 36, 235) : IM_COL32(60, 38, 20, 215), 5.0f * ui_scale);
        draw->AddRect(a, b, hot ? IM_COL32(255, 220, 140, 255) : IM_COL32(190, 150, 90, 230), 5.0f * ui_scale, 0, 2.0f * ui_scale);
        const char* label = T("Keyboard / Mouse", "Touches clavier / souris", "Teclado / Mouse", "Teclado / Mouse");
        const float font_size = (b.y - a.y) * 0.46f;
        const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
        draw->AddText(ImGui::GetFont(), font_size,
                      ImVec2(a.x + ((b.x - a.x) - text.x) * 0.5f, a.y + ((b.y - a.y) - text.y) * 0.5f),
                      hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 210, 160, 255), label);
    }
    TrophyToast();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
