/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Prints the last draws and dispatches the CPU recorded (BB_GPU_CHECKPOINTS=1) and how far the
 * GPU got: for a fault where no Vulkan call reported a lost device (the driver crashed). */
void bbgpu_dump_recent_checkpoints(void);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Button prompts (button_prompts.cpp), the device used last: 0 keyboard or mouse (pictures of the
 * bound keys), 1 a PlayStation controller (the game's own buttons), 2 an Xbox or other controller
 * (Xbox buttons). */
void bbgpu_prompts_controller(int device);
/* Mouse controls: look=1 captures the cursor while the game window has the focus (relative
 * mode). sample returns the motion (pixels) and wheel steps since the last call and the held
 * buttons (1 left, 2 right, 4 middle, 8 X1, 16 X2); 1 while the cursor is captured. */
void bbgpu_mouse_configure(int look);
/* 1 while a game menu is open (pause, options...): the cursor is given back to the player. */
void bbgpu_menu_state(int open);
void bbgpu_menu_cursor_hidden(int hidden);
void bbgpu_controls_hint(int show, int hot, float x, float y, float w, float h); /* U2: the "Keyboard / Mouse" button next to the Commandes menu (menu space 1280x720) */
void bbgpu_game_menu_cursor(int open); /* U2: a menu is on screen, the cursor is free (the pictograms stay as they are) */
void bbgpu_menu_alt(int shown);
/* 1 while Elena rides the jetski (U1): the cross and square icons show the keys of forward and back. */
void bbgpu_jetski_state(int on);
/* 1 when a draw sampled the icon atlas (a button icon in a hint, a bar...) in the last ms milliseconds. */
int bbgpu_atlas_recent(int ms);
/* The device the player used last: 0 keyboard and mouse, 1 PlayStation controller, 2 Xbox (or other) controller. The window tracks
 * it from the SDL events; the pad code gives the game a plain pad, and the icon atlas paints the buttons, accordingly. */
int bbgpu_input_mode(void);
/* The SDL id of the controller used last (0 if none): with several plugged in, the pad code reads this one. */
uint32_t bbgpu_active_gamepad(void);
void bbgpu_set_language(int language);
void bbgpu_set_cursor(int state); /* 0 normal, 1 over a clickable thing, 2 pressed */
/* The game's Commandes menu asks for the keyboard and mouse bindings window (the port's overlay); controls_changed is 1 once after
 * the bindings file (BB_INPUT_CONFIG) was written: the pad code reads it again. */
void bbgpu_open_controls(void);
int bbgpu_controls_changed(void);
/* Heading (degrees, about the vertical axis) of the camera the game last rendered with and how old
 * it is; 0 before the first frame with a known camera. */
int bbgpu_camera_yaw(float *yaw_degrees, double *age_seconds);
/* 1 while the menu's Gyro aiming setting is on (bbport.ini gyro_aim). */
int bbgpu_gyro_aim(void);
/* The keys that open and close the in-game menu (input.ini key.overlay, two bindings: slot 0 and 1).
 * kind: NONE unbound, SCANCODE a physical key (SDL_Scancode), KEYCODE a key by the character it
 * types in the current layout (SDL_Keycode), MOUSE an SDL mouse button number (not left or right).
 * Until this is called the menu opens with Insert in slot 0. */
enum { BBGPU_OVERLAY_KEY_NONE, BBGPU_OVERLAY_KEY_SCANCODE, BBGPU_OVERLAY_KEY_KEYCODE, BBGPU_OVERLAY_KEY_MOUSE };
void bbgpu_overlay_key_configure(int slot, int kind, int code);
int bbgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel);
/* The screenshot key (input.ini key.share, the PS4 Share button; two bindings, same kinds as the menu key):
 * pressing it saves the next frame the game shows, at the game's own resolution, as a picture in the user
 * folder (screenshots/). */
void bbgpu_share_key_configure(int slot, int kind, int code);
/* GPU thread: 1 once per press of the screenshot key. */
int bbgpu_share_take(void);
/* In-game trophy notice (the overlay draws it for a few seconds, one at a time): grade 1 platinum,
 * 2 gold, 3 silver, 4 bronze; png is the trophy's icon (a PNG file) or empty. */
void bbgpu_trophy_notify(int grade, const char *game, const char *name, const void *png, uint64_t png_size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
