// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <string>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "../bbgpu.h"
#include "bbport_keyicons.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "bbport_settings.h"

namespace {
// bbport: mouse input for runtime_pad.c (keyboard and mouse controls). The window thread adds
// the motion, buttons and wheel of its events; the pad sampling takes them (bbgpu_mouse_sample).
std::mutex mouse_mutex;
float mouse_dx{}, mouse_dy{};
int mouse_wheel{};
u32 mouse_buttons{};
std::atomic<bool> mouse_look{false}, mouse_captured{false};
std::atomic<bool> menu_drag{false}; // the mouse turns the 3D model of a menu (left button held on it): the free cursor's motion counts
std::atomic<bool> game_menu{false}; // a game menu is open: the cursor is free
std::atomic<bool> menu_cursor_hidden{false}; // menu keys on but no menu drawn (loading, cutscene): no cursor
bool window_focused{true};

// bbport: the screenshot key (key.share): two bindings, kinds as bbgpu_overlay_key_configure. A press sets
// share_requested; the presenter takes it (bbgpu_share_take) and saves the next frame.
struct ShareKey {
    int kind = 0; // BBGPU_OVERLAY_KEY_*
    int code = 0;
};
std::mutex share_mutex;
ShareKey share_keys[2];
std::atomic<bool> share_requested{false};

bool IsShareButton(u8 button) {
    std::scoped_lock lock{share_mutex};
    for (const ShareKey& k : share_keys) {
        if (k.kind == BBGPU_OVERLAY_KEY_MOUSE && k.code == button) {
            return true;
        }
    }
    return false;
}

bool IsShareKey(const SDL_KeyboardEvent& key) {
    std::scoped_lock lock{share_mutex};
    for (const ShareKey& k : share_keys) {
        if ((k.kind == BBGPU_OVERLAY_KEY_SCANCODE && static_cast<int>(key.scancode) == k.code) ||
            (k.kind == BBGPU_OVERLAY_KEY_KEYCODE && static_cast<int>(key.key) == k.code)) {
            return true;
        }
    }
    return false;
}

// The device the player used last: 0 keyboard and mouse, 1 PlayStation controller, 2 Xbox (or other) controller. The events of
// the window decide (a key, a click, a clear mouse move, a button or a pushed stick of a controller); runtime_pad.c and the icon
// atlas follow it (bbgpu_input_mode, BbKeyIcons::SetInputMode).
std::atomic<int> input_mode{0};
std::atomic<SDL_JoystickID> active_gamepad{0}; // the controller used last (0: none yet): the pad code reads this one
bool cursor_hidden_for_pad{false};
Uint64 ignore_mouse_until{0}; // SDL_GetTicks() before which a mouse motion does not count as the player's

const char* InputModeName(int mode) {
    return mode == 1 ? "PlayStation controller" : mode == 2 ? "Xbox controller" : "keyboard and mouse";
}

void SetInputMode(int mode) {
    if (input_mode.exchange(mode) != mode) {
        ignore_mouse_until = SDL_GetTicks() + 300;
        std::printf("Input: now %s\n", InputModeName(mode));
        BbKeyIcons::SetInputMode(mode);
    }
}

// The controllers made by Sony keep the PlayStation icons; every other one (Xbox, generic, Switch...) gets the Xbox ones.
int ModeOfGamepad(SDL_JoystickID id) {
    SDL_GamepadType type = SDL_GetRealGamepadTypeForID(id);
    if (type == SDL_GAMEPAD_TYPE_UNKNOWN || type == SDL_GAMEPAD_TYPE_STANDARD) {
        type = SDL_GetGamepadTypeForID(id);
    }
    return type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5 ? 1 : 2;
}

u32 MouseBit(u8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return 1u;
    case SDL_BUTTON_RIGHT: return 2u;
    case SDL_BUTTON_MIDDLE: return 4u;
    case SDL_BUTTON_X1: return 8u;
    case SDL_BUTTON_X2: return 16u;
    default: return 0u;
    }
}
#include "bbport_cursor_data.inc"

// The golden arrow of the menus (the cursor of the game's own screens): one master picture per state, scaled to the window
// (about 26 px high at 1080p) and made an SDL colour cursor. runtime_pad.c says the state (0 normal, 1 over something that
// answers a click, 2 pressed); the window thread (PollEvents) puts the cursor on while a game menu is open.
std::atomic<int> cursor_state{0};
SDL_Cursor* menu_cursors[3]{};
int menu_cursor_size{};
int applied_cursor{-1}; // -1: the system's, 0..2: menu_cursors[n]

void FreeMenuCursors() {
    for (SDL_Cursor*& c : menu_cursors) {
        if (c) {
            SDL_DestroyCursor(c);
            c = nullptr;
        }
    }
}

void BuildMenuCursors(int size) {
    if (applied_cursor != -1) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        applied_cursor = -1;
    }
    FreeMenuCursors();
    menu_cursor_size = size;
    const int master = CursorMasterSize;
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4);
    for (int s = 0; s < 3; ++s) {
        // box filter on premultiplied colours (the picture is larger than the cursor)
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int x0 = x * master / size, x1 = std::max(x0 + 1, (x + 1) * master / size);
                const int y0 = y * master / size, y1 = std::max(y0 + 1, (y + 1) * master / size);
                float r = 0, g = 0, b = 0, a = 0;
                int n = 0;
                for (int yy = y0; yy < y1 && yy < master; ++yy) {
                    for (int xx = x0; xx < x1 && xx < master; ++xx) {
                        const unsigned char* p = &CursorPixels[s][(yy * master + xx) * 4];
                        const float pa = p[3] / 255.0f;
                        r += p[0] * pa;
                        g += p[1] * pa;
                        b += p[2] * pa;
                        a += pa;
                        ++n;
                    }
                }
                unsigned char* q = &pixels[(static_cast<size_t>(y) * size + x) * 4];
                if (a > 0.0f && n > 0) {
                    q[0] = static_cast<unsigned char>(std::min(255.0f, r / a));
                    q[1] = static_cast<unsigned char>(std::min(255.0f, g / a));
                    q[2] = static_cast<unsigned char>(std::min(255.0f, b / a));
                    q[3] = static_cast<unsigned char>(std::min(255.0f, a / n * 255.0f));
                } else {
                    q[0] = q[1] = q[2] = q[3] = 0;
                }
            }
        }
        SDL_Surface* surface = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32, pixels.data(), size * 4);
        if (surface) {
            menu_cursors[s] = SDL_CreateColorCursor(surface, CursorHotX * size / master, CursorHotY * size / master);
            SDL_DestroySurface(surface);
        }
    }
}

void UpdateMenuCursor(SDL_Window* window, bool menu_open) {
    if (!menu_open) {
        if (applied_cursor != -1) {
            SDL_SetCursor(SDL_GetDefaultCursor());
            applied_cursor = -1;
        }
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    const int size = std::clamp(static_cast<int>(h * 26.0f / 1080.0f + 0.5f), 16, 128);
    if (size != menu_cursor_size || !menu_cursors[0]) {
        BuildMenuCursors(size);
    }
    const int state = std::clamp(cursor_state.load(), 0, 2);
    if (menu_cursors[state] && applied_cursor != state) {
        SDL_SetCursor(menu_cursors[state]);
        applied_cursor = state;
    }
}
#if !defined(_WIN32) && SDL_VERSION_ATLEAST(3, 4, 0)
// The window icon from launcher/uncharted.ico (on Windows the executable's icon resource gives
// it). The .ico holds PNG images: the largest one is used, the desktop scales it.
void SetWindowIcon(SDL_Window* window) {
    const char* base = SDL_GetBasePath(); // out/ in a source tree
    const std::string paths[] = {std::string(base ? base : "") + "../launcher/uncharted.ico",
                                 "launcher/uncharted.ico"};
    for (const std::string& path : paths) {
        size_t size = 0;
        auto* data = static_cast<const u8*>(SDL_LoadFile(path.c_str(), &size));
        if (!data) {
            continue;
        }
        const auto u32_at = [&](size_t at) {
            return u32(data[at]) | u32(data[at + 1]) << 8 | u32(data[at + 2]) << 16 | u32(data[at + 3]) << 24;
        };
        const size_t count = size >= 6 ? (data[4] | data[5] << 8) : 0;
        u32 best_size = 0, best_offset = 0;
        for (size_t i = 0; i < count && 6 + 16 * (i + 1) <= size; ++i) {
            const u32 image_size = u32_at(6 + 16 * i + 8), offset = u32_at(6 + 16 * i + 12);
            if (image_size > best_size && offset <= size && image_size <= size - offset) {
                best_size = image_size;
                best_offset = offset;
            }
        }
        SDL_Surface* icon =
            best_size ? SDL_LoadPNG_IO(SDL_IOFromConstMem(data + best_offset, best_size), true) : nullptr;
        if (icon) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
        SDL_free(const_cast<u8*>(data));
        return;
    }
}
#else
void SetWindowIcon(SDL_Window*) {}
#endif
/// Switches the window between the three modes of bbport_settings.h. SDL wants window calls on
/// the thread that owns the window, so the in-game menu only posts a request
/// (bbgpu_display_mode_request) and the event pump, which runs on that thread, applies it here.
void ApplyDisplayMode(SDL_Window* window, int mode) {
    switch (mode) {
    case BbSettings::DisplayBorderless:
        // The desktop mode as it is, the window covering the screen (no mode switch).
        SDL_SetWindowFullscreenMode(window, nullptr);
        SDL_SetWindowFullscreen(window, true);
        break;
    case BbSettings::DisplayExclusive: {
        // A mode of the display's own: the panel's native one, so the desktop does not scale the
        // picture twice (the upscaler does the scaling). Asking for the mode closest to the
        // window's current size picks small modes (1600x900 on a 1080p screen), so use the
        // desktop's. Wayland compositors may refuse the switch and leave it borderless. On Windows
        // SDL then owns the display mode (ChangeDisplaySettingsEx when it differs from the
        // desktop's) and minimizes the window on focus loss, which gives the mode back; the
        // presenter skips presenting while the window is minimized (0x0).
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        static SDL_DisplayMode chosen; // SDL copies it; kept static for older SDL versions
        if (const SDL_DisplayMode* native = SDL_GetDesktopDisplayMode(display)) {
            chosen = *native;
            if (!SDL_SetWindowFullscreenMode(window, &chosen)) {
                std::printf("Window: exclusive mode %dx%d@%.2f refused (%s), staying borderless\n",
                            chosen.w, chosen.h, chosen.refresh_rate, SDL_GetError());
                SDL_SetWindowFullscreenMode(window, nullptr);
            }
        }
        SDL_SetWindowFullscreen(window, true);
        break;
    }
    default:
        SDL_SetWindowFullscreen(window, false);
        break;
    }
    SDL_SyncWindow(window); // so the new size is known before the next frame
}

std::atomic<int> requested_mode{-1};
} // namespace

extern "C" void bbgpu_display_mode_request(int mode) {
    requested_mode = mode;
}

extern "C" void bbgpu_set_cursor(int state) {
    cursor_state = state;
}

extern "C" void bbgpu_mouse_configure(int look) {
    mouse_look = look != 0;
}

extern "C" void bbgpu_share_key_configure(int slot, int kind, int code) {
    if (slot < 0 || slot > 1) {
        return;
    }
    std::scoped_lock lock{share_mutex};
    share_keys[slot] = {kind, code};
}

extern "C" int bbgpu_share_take(void) {
    return share_requested.exchange(false) ? 1 : 0;
}

extern "C" void bbgpu_open_controls(void) {
    BbOverlay::OpenControls();
}

extern "C" void bbgpu_set_language(int language) {
    BbOverlay::SetLanguage(language);
}

extern "C" int bbgpu_controls_changed(void) {
    return BbOverlay::ControlsChanged() ? 1 : 0;
}

extern "C" uint32_t bbgpu_active_gamepad(void) {
    return active_gamepad.load();
}

extern "C" int bbgpu_input_mode(void) {
    return input_mode.load();
}

extern "C" void bbgpu_menu_state(int open) {
    game_menu = open != 0;
    BbKeyIcons::SetMenuMode(open != 0);
}

extern "C" void bbgpu_controls_hint(int show, int hot, float x, float y, float w, float h) {
    BbOverlay::SetControlsHint(show != 0, hot != 0, x, y, w, h);
}

extern "C" void bbgpu_game_menu_cursor(int open) {
    game_menu = open != 0;
}

extern "C" void bbgpu_menu_cursor_hidden(int hidden) {
    menu_cursor_hidden = hidden != 0;
}

extern "C" void bbgpu_jetski_state(int on) {
    BbKeyIcons::SetJetskiMode(on != 0);
}

extern "C" void bbgpu_menu_alt(int shown) {
    BbKeyIcons::SetMenuAlt(shown != 0);
}

extern "C" int bbgpu_mouse_sample(float* dx, float* dy, uint32_t* buttons, int* wheel) {
    std::scoped_lock lock{mouse_mutex};
    *dx = mouse_dx;
    *dy = mouse_dy;
    *buttons = mouse_buttons;
    *wheel = mouse_wheel;
    mouse_dx = mouse_dy = 0.0f;
    mouse_wheel = 0;
    return (mouse_captured || menu_drag) ? 1 : 0;
}

extern "C" void bbgpu_menu_drag(int on) {
    menu_drag = on != 0;
}

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    // The controller keeps playing while another window has the focus (the game on one monitor,
    // other apps on the keyboard and mouse); the keyboard and mouse look already stop then.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    // bbport: test windows (scripted runs next to the player's desktop), all off by default:
    //   BB_WINDOW_POS=x,y     where the window opens (e.g. -3000,0: off every screen)
    //   BB_NO_FOCUS=1         never takes the focus or the taskbar button (cannot be activated)
    //   BB_WINDOW_HIDDEN=1    created hidden; the game keeps rendering, the frame dump still works
    //   BB_WINDOW_MINIMIZED=1 created minimized (the presenter then skips presenting)
    int pos_x = SDL_WINDOWPOS_CENTERED, pos_y = SDL_WINDOWPOS_CENTERED;
    if (const char* pos = std::getenv("BB_WINDOW_POS")) {
        int x = 0, y = 0;
        if (std::sscanf(pos, "%d,%d", &x, &y) == 2) {
            pos_x = x;
            pos_y = y;
        }
    }
    const auto flag = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (flag("BB_NO_FOCUS")) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN, true);
        window_focused = false;
    }
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, flag("BB_WINDOW_HIDDEN"));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN, flag("BB_WINDOW_MINIMIZED"));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, pos_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, pos_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    // The mode comes from the settings: the in-game menu (bbport.ini) or BB_FULLSCREEN=1.
    const int display_mode = BbSettings::Get().display_mode;
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           display_mode != BbSettings::DisplayWindowed);
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    SetWindowIcon(window);
    if (display_mode == BbSettings::DisplayExclusive) {
        ApplyDisplayMode(window, display_mode);
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {} ({})", w, h, driver, BbSettings::DisplayModeName(display_mode));
    std::printf("Window: %dx%d on %s (%s)\n", w, h, driver, BbSettings::DisplayModeName(display_mode));
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    if (const int mode = requested_mode.exchange(-1); mode >= 0) { // the in-game menu asked
        ApplyDisplayMode(window, mode);
        LOG_INFO(Frontend, "Display mode: {}", BbSettings::DisplayModeName(mode));
        std::printf("Window: display mode -> %s\n", BbSettings::DisplayModeName(mode));
    }
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    // Mouse look holds the cursor in the window (relative mode) while the game has the focus;
    // the settings menu, text entry, a game menu and (in play) a held Left Alt give it back.
    {
        static bool drag_was = false;
        static float drag_x = 0.0f, drag_y = 0.0f;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const bool want = mouse_look && window_focused && !text_active && (!game_menu || menu_drag) && input_mode == 0 &&
                          !BbOverlay::CapturesInput() && !(keys && keys[SDL_SCANCODE_LALT]);
        if (want != mouse_captured) {
            ignore_mouse_until = SDL_GetTicks() + 300;
            if (want && menu_drag) SDL_GetMouseState(&drag_x, &drag_y);   // (the cursor comes back where it was taken)
            SDL_SetWindowRelativeMouseMode(window, want);
            if (!want && game_menu && drag_was) SDL_WarpMouseInWindow(window, drag_x, drag_y);
            mouse_captured = want;
            std::scoped_lock lock{mouse_mutex};
            mouse_dx = mouse_dy = 0.0f;
            if (!(want ? menu_drag.load() : (game_menu.load() && drag_was))) mouse_buttons = 0;   // (turning a menu's model: the button is still held, and stays so)
        }
        drag_was = menu_drag;
    }
    UpdateMenuCursor(window, game_menu && !menu_cursor_hidden && input_mode == 0 && !BbOverlay::CapturesInput());
    {   // with a controller in hand, the pointer is out of the way (it comes back with the mouse)
        const bool hide = input_mode != 0 && !text_active && !BbOverlay::CapturesInput();
        if (hide != cursor_hidden_for_pad) {
            cursor_hidden_for_pad = hide;
            if (hide) {
                SDL_HideCursor();
            } else {
                SDL_ShowCursor();
            }
        }
    }
    {   // every controller already plugged in is opened (the ones plugged in later are, at their SDL_EVENT_GAMEPAD_ADDED)
        static bool opened_all = false;
        if (!opened_all) {
            opened_all = true;
            int count = 0;
            if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
                for (int i = 0; i < count; ++i) {
                    SDL_OpenGamepad(ids[i]);
                }
                SDL_free(ids);
            }
        }
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // BB_TEXTURE_DUMP=1: F10 saves the textures of the next frame (user/texture_dump).
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat) {
            Vulkan::FrameCapture::ArmTextureDump();
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        // The screenshot key (a mouse button bound to it stays out of the game, like the menu key).
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && IsShareKey(event.key)) {
            share_requested = true;
        } else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                   IsShareButton(event.button.button)) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                share_requested = true;
            }
            continue;
        }
        // A button released while the overlay window is open (the click that opened it is released over it): the overlay takes the
        // event, but the game must not keep the button as held (Drake fired when the window closed).
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            std::scoped_lock lock{mouse_mutex};
            mouse_buttons &= ~MouseBit(event.button.button);
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                SetInputMode(0);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            SDL_OpenGamepad(event.gdevice.which); // a controller sends events only once opened
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            active_gamepad = event.gbutton.which;
            SetInputMode(ModeOfGamepad(event.gbutton.which));
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: // a stick pushed well past its drift, or a trigger pulled half way
            if (std::abs(static_cast<int>(event.gaxis.value)) > 16000) {
                active_gamepad = event.gaxis.which;
                SetInputMode(ModeOfGamepad(event.gaxis.which));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (active_gamepad == event.gdevice.which) {
                active_gamepad = 0;
            }
            if (SDL_Gamepad* gone = SDL_GetGamepadFromID(event.gdevice.which)) {
                SDL_CloseGamepad(gone);
            }
            if (input_mode != 0) {
                int count = 0;
                SDL_free(SDL_GetGamepads(&count));
                if (count == 0) {
                    SetInputMode(0);
                }
            }
            break;
        default:
            break;
        }
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            // (not just after the pointer was captured or given back: SDL may report the jump of the cursor as a motion)
            if (SDL_GetTicks() >= ignore_mouse_until && std::fabs(event.motion.xrel) + std::fabs(event.motion.yrel) >= 2.0f) {
                SetInputMode(0);
            }
            if (mouse_captured || menu_drag) {
                std::scoped_lock lock{mouse_mutex};
                mouse_dx += event.motion.xrel;
                mouse_dy += event.motion.yrel;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                SetInputMode(0);
            }
            std::scoped_lock lock{mouse_mutex};
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !BbOverlay::CapturesInput()) {
                mouse_buttons |= MouseBit(event.button.button);
            } else {
                mouse_buttons &= ~MouseBit(event.button.button);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            SetInputMode(0);
            std::scoped_lock lock{mouse_mutex};
            const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            mouse_wheel += y > 0 ? 1 : y < 0 ? -1 : 0;
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            window_focused = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            window_focused = false;
            std::scoped_lock lock{mouse_mutex};
            mouse_buttons = 0;
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend
