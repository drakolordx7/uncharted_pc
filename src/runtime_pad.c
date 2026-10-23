/* libScePad on SDL3 gamepads, keyboard and mouse. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled. The controller, keyboard and mouse work
 * together; their bindings come from input.ini (BB_INPUT_CONFIG, written by the launcher):
 *
 *   key.<action>=<key or mouse name>[|<second>]   SDL key names ("Space", "Left Shift", "W"),
 *       "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Wheel Up",
 *       "Wheel Down"; actions: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad
 *       up down left right, ls_up ls_down ls_left ls_right, rs_up rs_down rs_left rs_right,
 *       rotate_ccw rotate_cw (held: the left stick circles counter-clockwise / clockwise, for
 *       the valves and wheels that ask for the stick turned in circles), overlay (opens and
 *       closes the in-game menu, Insert by default; a key or Mouse Middle / 4 / 5), walk (held:
 *       the movement keys push the left stick only part of the way, so the character walks), share
 *       (F12 by default: saves a screenshot of the game's picture, the PS4 Share button).
 *   pad.<action>=<controller button>   SDL names (a b x y leftshoulder rightshoulder
 *       lefttrigger righttrigger leftstick rightstick back start dpup dpdown dpleft dpright
 *       touchpad misc1 paddle1..paddle4) or "none", for the button actions above.
 *   mouse_look=1  mouse_sensitivity=1.0  mouse_invert_y=0  pad_invert_y=0
 *   free_view_smoothing=0 (U1: 1 keeps the game's smoothing of the walking camera)  aim_raw=0 (U1: 1 = no smoothing while aiming)
 *
 * Defaults (no file): WASD move, mouse look, left button R2 (fire), right button L2 (aim),
 * Space Cross, C Circle, F Square, E Triangle, G L1 (grenade), R R1, Left Shift L3, V R3, Escape/Enter
 * Options, Tab touchpad, arrow keys and the wheel the d-pad, IJKL the right stick. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
void stickwatch_arm(uint64_t a0, uint64_t a1, uint64_t image);   /* runtime_stickwatch.c, TEMPORARY */
void stickwatch_enable(int on);
void stickwatch_summary(void);
extern volatile uint64_t guest_callback_rsp;   /* probe.c: the guest's rsp when a callback was entered */

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    {   /* several controllers: the one the player touched last (the window follows the events of all of them) */
        const SDL_JoystickID active=bbgpu_active_gamepad();
        if (active && (!gamepad || SDL_GetGamepadID(gamepad)!=active)) {
            SDL_Gamepad *next=SDL_OpenGamepad(active);
            if (next) {
                if (gamepad) { SDL_RumbleGamepad(gamepad,0,0,0); SDL_CloseGamepad(gamepad); } /* the pad put down stops shaking */
                gamepad=next;
                printf("Runtime: gamepad in use: %s\n",SDL_GetGamepadName(gamepad));
            }
        }
    }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Degrees of camera turn per mouse pixel at sensitivity 1 (0.08 felt "very slow", issue #18; the
 * launcher's slider and box go from 0.1 to 10 times this). */
#define MOUSE_DEGREES_PER_PIXEL 0.15f

/* ---- bindings (input.ini) ---------------------------------------------------------------- */
/* BIND_KEY: a physical key (SDL scancode, US position). BIND_LAYOUT: a character key the player
 * chose, found in the current keyboard layout (SDL keycode), so "Z" is the key labelled Z on an
 * AZERTY keyboard too. The default letters stay positions: WASD is ZQSD on AZERTY. */
enum { BIND_KEY=1, BIND_MOUSE, BIND_WHEEL, BIND_LAYOUT };
typedef struct { uint8_t kind; int32_t code; } Bind;
typedef struct { const char *name; uint32_t ps; const char *keys; const char *pad; } Action;
enum { A_LS_UP=16, A_LS_DOWN, A_LS_LEFT, A_LS_RIGHT, A_RS_UP, A_RS_DOWN, A_RS_LEFT, A_RS_RIGHT, A_WALK, ACTIONS };
static const Action actions[ACTIONS]={
    {"cross",BTN_CROSS,"Space","a"}, {"circle",BTN_CIRCLE,"Left Ctrl","b"}, {"square",BTN_SQUARE,"F","x"},
    {"triangle",BTN_TRIANGLE,"E","y"}, {"l1",BTN_L1,"G","leftshoulder"}, {"r1",BTN_R1,"R","rightshoulder"},
    {"l2",BTN_L2,"Mouse Right","lefttrigger"}, {"r2",BTN_R2,"Mouse Left","righttrigger"},
    {"l3",BTN_L3,"Left Shift","leftstick"}, {"r3",BTN_R3,"Mouse Middle","rightstick"},
    {"options",BTN_OPTIONS,"Escape|Return","start"}, {"touchpad",BTN_TOUCHPAD,"Tab","back"},
    {"up",BTN_UP,"V","dpup"}, {"down",BTN_DOWN,"P","dpdown"},
    {"left",BTN_LEFT,"1|Wheel Up","dpleft"}, {"right",BTN_RIGHT,"2|Wheel Down","dpright"},
    {"ls_up",0,"W",NULL}, {"ls_down",0,"S",NULL}, {"ls_left",0,"A",NULL}, {"ls_right",0,"D",NULL},
    {"rs_up",0,"I",NULL}, {"rs_down",0,"K",NULL}, {"rs_left",0,"J",NULL}, {"rs_right",0,"L",NULL},
    {"walk",0,"C",NULL},
};
#define PAD_TRIGGER_LEFT (-2)
#define PAD_TRIGGER_RIGHT (-3)
static struct {
    Bind keys[ACTIONS][2];
    int pad[16];               /* SDL_GamepadButton, PAD_TRIGGER_*, or -1 */
    int mouse_look, mouse_invert_y, pad_invert_y;
    int free_view_smoothing;   /* 1: keep the game's own smoothing of the walking camera (U1) */
    int aim_raw;               /* 1: no smoothing of the mouse while aiming / looking around (U1) */
    float mouse_sensitivity, mouse_aim_sensitivity;
} input;

/* Whether `name` is one of the keys in `defaults` ("Escape|Return"). */
static int default_key(const char *name, const char *defaults) {
    const size_t length=strlen(name);
    for (const char *d=defaults; d && *d; ) {
        const char *end=strchr(d,'|');
        const size_t n=end ? (size_t)(end-d) : strlen(d);
        if (n==length && !SDL_strncasecmp(d,name,n)) return 1;
        d=end ? end+1 : NULL;
    }
    return 0;
}
static Bind parse_bind(const char *name, const char *defaults) {
    static const struct { const char *name; uint8_t kind; int16_t code; } mouse[]={
        {"Mouse Left",BIND_MOUSE,1}, {"Mouse Right",BIND_MOUSE,2}, {"Mouse Middle",BIND_MOUSE,4},
        {"Mouse 4",BIND_MOUSE,8}, {"Mouse 5",BIND_MOUSE,16}, {"Wheel Up",BIND_WHEEL,1}, {"Wheel Down",BIND_WHEEL,-1},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i)
        if (!SDL_strcasecmp(name,mouse[i].name)) return (Bind){mouse[i].kind,mouse[i].code};
    if (name[0]>' ' && name[0]<127 && !name[1] && !default_key(name,defaults)) {
        const SDL_Keycode key=SDL_GetKeyFromName(name);
        if (key!=SDLK_UNKNOWN) return (Bind){BIND_LAYOUT,(int32_t)key};
    }
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? (Bind){0,0} : (Bind){BIND_KEY,(int32_t)code};
}
static void set_keys(int action, const char *value) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    input.keys[action][0]=input.keys[action][1]=(Bind){0,0};
    char *second=strchr(text,'|');
    if (second) *second++=0;
    if (*text) input.keys[action][0]=parse_bind(text,actions[action].keys);
    if (second && *second) input.keys[action][1]=parse_bind(second,actions[action].keys);
}
static int parse_pad(const char *value) {
    if (!SDL_strcasecmp(value,"lefttrigger")) return PAD_TRIGGER_LEFT;
    if (!SDL_strcasecmp(value,"righttrigger")) return PAD_TRIGGER_RIGHT;
    const SDL_GamepadButton button=SDL_GetGamepadButtonFromString(value);
    return button==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)button;
}
/* The in-game menu lives in the window thread (gpu/shim/bbport_overlay.cpp), which sees the key
 * events: it gets the two bindings of key.overlay. A wheel cannot open it, nor can the left and
 * right mouse buttons (the menu is used with them). */
static void configure_overlay_keys(void) {
    for (int i=0;i<2;++i) {
        const Bind *b=&input.keys[A_OVERLAY][i];
        int kind=BBGPU_OVERLAY_KEY_NONE, code=0;
        if (b->kind==BIND_KEY) { kind=BBGPU_OVERLAY_KEY_SCANCODE; code=b->code; }
        else if (b->kind==BIND_LAYOUT) { kind=BBGPU_OVERLAY_KEY_KEYCODE; code=b->code; }
        else if (b->kind==BIND_MOUSE && (b->code==4 || b->code==8 || b->code==16)) {
            kind=BBGPU_OVERLAY_KEY_MOUSE;
            code=b->code==4 ? SDL_BUTTON_MIDDLE : b->code==8 ? SDL_BUTTON_X1 : SDL_BUTTON_X2;
        } else if (b->kind) printf("Runtime: key.overlay cannot be a wheel or the left or right mouse button, ignored\n");
        bbgpu_overlay_key_configure(i,kind,code);
    }
}
/* The screenshot key (the PS4 Share button, which the games never see: the system takes the picture) is
 * handled by the window thread like the menu key: key.share, two bindings, a key or Mouse Middle / 4 / 5. */
static void configure_share_keys(void) {
    for (int i=0;i<2;++i) {
        const Bind *b=&input.keys[A_SHARE][i];
        int kind=BBGPU_OVERLAY_KEY_NONE, code=0;
        if (b->kind==BIND_KEY) { kind=BBGPU_OVERLAY_KEY_SCANCODE; code=b->code; }
        else if (b->kind==BIND_LAYOUT) { kind=BBGPU_OVERLAY_KEY_KEYCODE; code=b->code; }
        else if (b->kind==BIND_MOUSE && (b->code==4 || b->code==8 || b->code==16)) {
            kind=BBGPU_OVERLAY_KEY_MOUSE;
            code=b->code==4 ? SDL_BUTTON_MIDDLE : b->code==8 ? SDL_BUTTON_X1 : SDL_BUTTON_X2;
        } else if (b->kind) printf("Runtime: key.share cannot be a wheel or the left or right mouse button, ignored\n");
        bbgpu_share_key_configure(i,kind,code);
    }
}
static void load_input(void) {
    for (int a=0;a<ACTIONS;++a) {
        set_keys(a,actions[a].keys);
        if (a<16) input.pad[a]=parse_pad(actions[a].pad);
    }
    input.mouse_look=1; input.mouse_sensitivity=1.0f; input.mouse_aim_sensitivity=1.0f;
    input.free_view_smoothing=0; input.aim_raw=0;
    const char *path=getenv("BB_INPUT_CONFIG");
    FILE *f=path && *path ? fopen(path,"r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            char *eq=strchr(line,'=');
            if (line[0]=='#' || !eq) continue;
            *eq=0;
            const char *key=line, *value=eq+1;
            if (!strcmp(key,"mouse_look")) input.mouse_look=atoi(value)!=0;
            else if (!strcmp(key,"mouse_sensitivity")) input.mouse_sensitivity=(float)atof(value);
            else if (!strcmp(key,"mouse_aim_sensitivity")) input.mouse_aim_sensitivity=(float)atof(value);
            else if (!strcmp(key,"mouse_invert_y")) input.mouse_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"pad_invert_y")) input.pad_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"free_view_smoothing")) input.free_view_smoothing=atoi(value)!=0;
            else if (!strcmp(key,"aim_raw")) input.aim_raw=atoi(value)!=0;
            else for (int a=0;a<ACTIONS;++a) {
                if (!strncmp(key,"key.",4) && !strcmp(key+4,actions[a].name)) set_keys(a,value);
                else if (a<16 && !strncmp(key,"pad.",4) && !strcmp(key+4,actions[a].name)) input.pad[a]=parse_pad(value);
            }
        }
        fclose(f);
        printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f)\n",path,
               input.mouse_look ? "on" : "off",input.mouse_sensitivity);
    }
    configure_overlay_keys();
    configure_share_keys();
    if (input.mouse_sensitivity<=0.0f) input.mouse_sensitivity=1.0f;
    if (input.mouse_aim_sensitivity<=0.0f) input.mouse_aim_sensitivity=1.0f;
    bbgpu_mouse_configure(input.mouse_look);
}

/* Mouse motion as a right stick: the filtered speed (pixels per second) scaled to a deflection,
 * past the game's dead zone as soon as the mouse moves. */
static float mouse_vx, mouse_vy;
static uint64_t mouse_last_us;
static uint64_t wheel_until_us[2]; /* wheel up, wheel down: held briefly for the game to see */
/* A turn rate (degrees/s) as a stick byte, through the inverse of the U1 follow camera's response
 * measured below its speed ramp: about 240 x^2.5 degrees/s (x = 0.25: 7, 0.5: 37). */
static uint8_t mouse_axis(float rate) {
    const float r=rate<0 ? -rate : rate;
    if (r<0.5f) return 128;
    float out=SDL_powf(r/240.0f,0.4f);
    if (out>1.0f) out=1.0f;
    int x=(int)(128.0f+(rate<0 ? -out : out)*127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static int bind_down(const Bind *b, const bool *k, uint32_t mouse, uint64_t now) {
    switch (b->kind) {
    case BIND_KEY: return k && k[b->code];
    case BIND_LAYOUT: {
        const SDL_Scancode code=k ? SDL_GetScancodeFromKey((SDL_Keycode)b->code,NULL) : SDL_SCANCODE_UNKNOWN;
        return code!=SDL_SCANCODE_UNKNOWN && k[code];
    }
    case BIND_MOUSE: return (mouse & (uint32_t)b->code)!=0;
    case BIND_WHEEL: return now<wheel_until_us[b->code>0 ? 0 : 1];
    default: return 0;
    }
}
/* Once the keyboard is up: where the layout put each character key the player chose. */
static void log_layout_keys(void) {
    for (int a=0;a<ACTIONS;++a) for (int i=0;i<2;++i) {
        const Bind *b=&input.keys[a][i];
        if (b->kind!=BIND_LAYOUT) continue;
        const SDL_Scancode code=SDL_GetScancodeFromKey((SDL_Keycode)b->code,NULL);
        printf("Runtime: key.%s %s: the key labelled so in this keyboard layout (US position %s)\n",
               actions[a].name,SDL_GetKeyName((SDL_Keycode)b->code),
               code==SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(code));
    }
}
static int action_down(int a, const bool *k, uint32_t mouse, uint64_t now) {
    return bind_down(&input.keys[a][0],k,mouse,now) || bind_down(&input.keys[a][1],k,mouse,now);
}

/* ---- mouse camera ------------------------------------------------------------------------
 * The games turn the camera from the right stick through their own curve, a speed ramp and a
 * top speed (U1: about 240 degrees/s, 45 while aiming), which no stick position can make feel like
 * a mouse. Where the camera's state is known, the mouse turns it directly instead: a callback at
 * the start of the camera's per-frame update (installed by the loader when the code matches)
 * rotates its direction by the mouse motion since the last frame. Elsewhere the mouse still
 * moves the right stick. */
static pthread_mutex_t camera_lock=PTHREAD_MUTEX_INITIALIZER;
/* Mouse motion (pixels) not yet given to the follow camera [0] and to the joypad state [1]: each
 * gets all of it (the follow camera does not read the joypad's right stick values, the other
 * cameras do not use the follow camera's direction). */
#define CAMERA_QUEUES 4                /* 0 follow camera, 1 joypad sticks, 2 aiming / look camera, 3 cover camera */
static volatile uint64_t cover_native_us; /* last frame the cover camera was turned by the mouse */
static volatile uint64_t zoom_camera_us;  /* last frame the zoom (aiming) camera was updated */
/* The mouse's yaw since the aim began (degrees, positive to the right): the game's message that aligns the camera with the
 * player's pose gets a pose turned by it (u1b_aim_target), or the camera is pulled back to the player's facing every
 * frame while the aim blends in (about 0.5 s), which cancelled the mouse. */
static volatile float aim_target_total;
static volatile uint64_t aim_target_us;
static float camera_dx[CAMERA_QUEUES], camera_dy[CAMERA_QUEUES];
/* U1: the camera time step (seconds, image+0xa4cb10 in CUSA02320 / CUSA02344 1.02) the cameras multiply
 * their speeds with; it is 0 while the game is paused, and the mouse must not turn anything then. */
#define U1B_CAMERA_DT 0xa4cb10
/* The last device the player used is a controller (PlayStation or Xbox, see bbgpu_input_mode): the port then gives the game a plain
 * pad. No key, no mouse, no menu mouse, no keyboard row; the callbacks of the PC controls hold back. Set by sample_host. */
static volatile int pad_only_mode;
static uint64_t camera_hook_us;       /* last follow camera callback: it turns with the mouse */
static uint64_t sticks_hook_us;       /* last joypad callback: the mouse is a right stick value there */
static float camera_test_rate=-1.0f;  /* BB_MOUSE_CAMERA_TEST=<degrees per frame>: constant turn */
static int mouse_debug(void) {
    static int on=-1;
    if (on<0) on=getenv("BB_MOUSE_DEBUG")!=NULL;
    return on;
}
static int camera_stopped(uint64_t image, uint32_t offset) {
    if (!image || !offset) return 0;
    return !(*(const volatile float *)(uintptr_t)(image+offset)>0.0f);
}
static int camera_take(int queue, float *dx, float *dy) {
    pthread_mutex_lock(&camera_lock);
    *dx=camera_dx[queue]; *dy=camera_dy[queue];
    camera_dx[queue]=camera_dy[queue]=0;
    pthread_mutex_unlock(&camera_lock);
    if (camera_test_rate<0.0f) {
        const char *test=getenv("BB_MOUSE_CAMERA_TEST");
        camera_test_rate=test ? (float)atof(test) : 0.0f;
    }
    return 1;
}
/* The follow camera's collision/position update runs once a frame with the camera object (U1
 * 1.00 and 1.02; U2 and U3 have the same function, in code compiled differently in each build, so the
 * hooks are found by instruction patterns, see guest_callbacks): its horizontal direction (x, y, z) at +0x1310 (U2 +0x9ec0, U3 +0x78c0) and the
 * vertical arc position at +0x12bc (U2 +0x9e60, U3 +0x7874: the right stick up lowers it). */
static float wrap_degrees(float a) {
    while (a>180.0f) a-=360.0f;
    while (a<-180.0f) a+=360.0f;
    return a;
}
static float heading_of(const float *dir) { return SDL_atan2f(-dir[0],-dir[2])*57.29578f; }
/* The follow camera keeps the turns of its direction while the right stick is held: with the stick
 * at rest the game sets the direction itself every few frames and most turns were lost (an uneven,
 * short turn). So while the mouse turns the direction, the joypad callback holds the stick a little
 * (0.3, about 13 degrees/s) the same way, and the mouse moves a wanted heading: each frame the
 * direction turns by the mouse's motion plus what the direction is still short of it (read from the
 * direction itself, which also takes out the stick's own turn; the shown camera follows the
 * direction with the game's smoothing, 1 to 8 frames late, too late to steer by). While the mouse
 * rests, the wanted heading follows the direction (the game's own camera moves stay free). Should
 * the camera keep no turns at all, the mouse goes to the joypad's right stick (the follow camera's
 * curve), with a small turn each frame to notice when turns are kept again. */
static volatile int follow_direct=1;
static float follow_kept=1.0f;            /* average fraction of a turn still there the next frame */
static float follow_before, follow_after; /* heading before and after the last turn */
static int follow_pending;
static volatile int aim_held;             /* L2 is held: the aiming camera (also while it blends in) */
static volatile int look_held;            /* R3 is held: first-person look around, which turns like the aim */
static float follow_target;               /* wanted heading (degrees) */
static uint64_t follow_moved_us;          /* last frame the mouse moved */
static volatile float follow_correction;  /* degrees the direction is short of the wanted heading */
static volatile float follow_push;        /* the right stick while the direction turns (-0.3, 0, 0.3) */
static volatile int follow_has_arc;       /* the follow camera's vertical arc is known */
/* arc_degrees: the camera pitch one unit of arc covers (U1 61; measured in U2 about 135, in U3 about 105, steeper upwards) */
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, float arc_degrees, int stopped) {
    const uint64_t now=now_us();
    follow_has_arc=arc_offset!=0;
    if (camera_hook_us && now-camera_hook_us>100000) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        follow_moved_us=0;
    }
    camera_hook_us=now;
    float *dir=(float *)(uintptr_t)(camera+dir_offset);
    const float heading=heading_of(dir);
    float dx, dy;
    camera_take(0,&dx,&dy);
    if (aim_held || look_held || (cover_native_us && now-cover_native_us<100000)) { follow_moved_us=0; follow_pending=0; follow_push=0.0f; return; } /* the aiming / look / cover camera takes the mouse */
    if (stopped) { follow_moved_us=0; follow_pending=0; follow_push=0.0f; follow_correction=0.0f; return; } /* paused */
    if (follow_pending) {
        float kept=wrap_degrees(heading-follow_before)/wrap_degrees(follow_after-follow_before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        follow_kept+=0.2f*(kept-follow_kept);
        if (follow_kept<0.12f) follow_direct=0;
        else if (follow_kept>0.3f) follow_direct=1;
        follow_pending=0;
    }
    float shown=heading; /* the shown camera's yaw (debug output) */
    double age=0.0;
    if (mouse_debug() && (!bbgpu_camera_yaw(&shown,&age) || age>0.25)) shown=heading;
    const float turn=camera_test_rate>0.0f ? camera_test_rate : dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    /* the wanted heading and how far the direction is short of it (degrees still to turn). The
     * camera rests 0.15 s after the mouse stopped, once the direction has reached the wanted heading
     * (fast turns go beyond what the game keeps in a frame) or at the latest after 0.5 s. */
    float correction=wrap_degrees(heading-follow_target);
    const uint64_t still=now-follow_moved_us;
    const int resting=!follow_moved_us || (still>150000 && (SDL_fabsf(correction)<0.5f || still>500000));
    if (turn!=0.0f) follow_moved_us=now;
    if (resting) {
        follow_target=heading;
        correction=0.0f;
    }
    if (correction>90.0f || correction<-90.0f) { /* at most a quarter turn behind */
        correction=correction>0.0f ? 90.0f : -90.0f;
        follow_target=wrap_degrees(heading-correction);
    }
    follow_target=wrap_degrees(follow_target-turn); /* a positive turn lowers the heading */
    follow_view=follow_direct ? follow_target : heading;
    if (!follow_direct) {
        /* the joypad turns the camera (with the shortfall as extra speed); a small probe turn shows
         * whether direct turns are kept again */
        follow_correction=correction;
        follow_push=0.0f;
        if (turn!=0.0f) {
            const float probe=turn>0.0f ? 0.3f : -0.3f;
            const float a=probe*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
            const float x=dir[0], z=dir[2];
            dir[0]=x*c-z*s;
            dir[2]=x*s+z*c;
            follow_before=heading;
            follow_after=heading_of(dir);
            follow_pending=1;
        }
        if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f kept=%.2f joypad\n",
                                  now*1e-6,dx,heading,shown,follow_target,follow_kept);
        return;
    }
    follow_correction=0.0f;
    /* the shortfall is made up a few degrees a frame (no jump when the game keeps turns again) */
    if (correction<-4.0f) correction=-4.0f;
    if (correction>4.0f) correction=4.0f;
    const float total=turn+correction;
    follow_push=total>0.05f ? 0.3f : total<-0.05f ? -0.3f : 0.0f;
    if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f dy=%.1f heading=%.2f shown=%.2f target=%.2f turn=%.2f total=%.2f arc=%.4f\n",
                              now*1e-6,dx,dy,heading,shown,follow_target,turn,total,arc_offset ? *(float *)(uintptr_t)(camera+arc_offset) : -1.0f);
    if (total!=0.0f) {
        const float a=total*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        if (SDL_fabsf(total)>=0.15f) {
            follow_before=heading;
            follow_after=heading_of(dir);
            follow_pending=1;
        }
    }
    if (dy!=0.0f && arc_offset) {
        float *arc=(float *)(uintptr_t)(camera+arc_offset);
        const float pitch=arc_to_pitch(arc_table,*arc)-(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        const float v=pitch_to_arc(arc_table,pitch);
        *arc=v<0.0f ? 0.0f : v>1.0f ? 1.0f : v;
    }
}
static uint32_t u1_camera_dt_offset;  /* set by the callbacks of the versions whose time step is known */
static uint64_t u1_image;             /* U1 image base, seen by the pad update callback */
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    follow_camera(camera,0x1310,0x12bc,61.0f,camera_stopped(image,u1_camera_dt_offset));
}
static int safe_read(void *dst, uint64_t src, size_t size);
static int safe_write(uint64_t dst, const void *src, size_t size);
/* U1 1.02: the walking camera (update image+0xaff80) aims at a point that follows the player's through a damped spring (the game's
 * smoothing of the free view). The spring is quick (speed 35 to 50 per second, constants image+0x7900a8 / +0x7900ac) while the point
 * is within 2.5 m of where it was, and slows down to the speed in the camera's shape (float at *(camera+0x1e0)+0x74) as the gap
 * grows to 3.75 m. A quick mouse turn opens a big gap, so the picture trailed the mouse by about 0.3 s and started slowly
 * (measured in user\last_run.log: the camera's direction against the shown yaw). So, unless free_view_smoothing is set, that speed
 * is raised to the quick one while the mouse is in use, and put back afterwards. The shape is also used by the spring of the camera's
 * height (image+0xaff80 near the start), which gets quicker too. BB_FREE_SPRING=<speed> tunes the value. */
/* First try (raising only the shape's speed, 20 and 25 in the dump, to 35) left a slight spring: the quick regime's own speeds
 * (the two constants, 50 and 35) rule every turn that opens a gap under 2.5 m. They are raised too, to the same value. */
#define U1B_FREE_SPRING_SPEED 200.0f
static void u1b_free_view_constants(uint64_t image, int want, float speed) {
    static const uint32_t where[2]={0x7900a8,0x7900ac};
    static float original[2]; static int raised;
    if (want && !raised) {
        float now[2];
        for (int i=0;i<2;++i) if (!safe_read(&now[i],image+where[i],4) || !(now[i]>0.0f && now[i]<1000.0f)) return;
        for (int i=0;i<2;++i) { original[i]=now[i]; if (now[i]<speed) safe_write(image+where[i],&speed,4); }
        raised=1;
        if (mouse_debug()) printf("Mouse: free view constants %.2f %.2f -> %.2f\n",original[0],original[1],speed);
    } else if (!want && raised) {
        for (int i=0;i<2;++i) safe_write(image+where[i],&original[i],4);
        raised=0;
        if (mouse_debug()) printf("Mouse: free view constants back to %.2f %.2f\n",original[0],original[1]);
    }
}
static void u1b_free_view_spring(uint64_t camera, uint64_t image) {
    static uint64_t shape; static float original; static int raised;
    static float speed=-1.0f;
    if (speed<0.0f) { const char *e=getenv("BB_FREE_SPRING"); speed=e && atof(e)>0.0 ? (float)atof(e) : U1B_FREE_SPRING_SPEED; }
    u1b_free_view_constants(image,!input.free_view_smoothing && input.mouse_look && !pad_only_mode,speed);
    uint64_t now_shape=0;
    if (!safe_read(&now_shape,camera+0x1e0,8) || now_shape<0x10000) return;
    if (now_shape!=shape) {   /* another camera object or shape: the old one gets its value back */
        if (raised && shape) safe_write(shape+0x74,&original,4);
        shape=now_shape; raised=0;
    }
    float current=0.0f;
    if (!safe_read(&current,shape+0x74,4)) return;
    const int want=!input.free_view_smoothing && input.mouse_look && !pad_only_mode;
    if (want && !raised && current>0.0f && current<speed) {
        original=current;
        if (safe_write(shape+0x74,&speed,4)) raised=1;
        if (mouse_debug()) printf("Mouse: free view spring %.2f -> %.2f (shape %#llx)\n",original,speed,(unsigned long long)shape);
    } else if (!want && raised) {
        safe_write(shape+0x74,&original,4);
        raised=0;
        if (mouse_debug()) printf("Mouse: free view spring back to %.2f\n",original);
    }
}
static void __attribute__((ms_abi)) u1b_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    u1b_free_view_spring(camera,image);
    u1_camera_dt_offset=U1B_CAMERA_DT;
    follow_camera(camera,0x1310,0x12bc,61.0f,camera_stopped(image,U1B_CAMERA_DT));
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,135.0f,0);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,105.0f,0);
}
/* ---- U2 1.02 native mouse cameras (antikmewzik's U2 mouse cameras, uncharted_pc_controls_qol u2-mkb-qol) ----
 * The camera manager of U2 1.02 calls each camera of its stack as update(rdi output block, rsi camera, rdx input
 * block), the update from the camera's vtable. A hook just before that call (the thunk saved r8 = the update, r10 =
 * the camera, r13 = the camera's stack entry; the manager's frame: input block at rsp + 0xe0, whose previous output
 * holds an orientation quaternion x y z w at +0x10) turns the aiming, cover, look-around and truck chase cameras by the
 * mouse directly, as U1's raw aim does: the aim is no longer limited to the stick's top speed (about 46 degrees/s in
 * 1.02). The updates are told apart by their first instructions; the walking camera keeps its own hook
 * (u2_follow_camera). U2 1.00 is compiled differently and keeps the stick. Only while the menu's "Raw mouse aiming and
 * cover camera" is on; BB_U2_NATIVE=0 turns these hooks off. */
static int safe_read(void *dst, uint64_t src, size_t size) {
    if (src<0x10000 || src>0x00007fffffffffffull) return 0;
#ifdef _WIN32
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(uintptr_t)src,dst,size,&got) && got==size;
#else
    memcpy(dst,(const void *)(uintptr_t)src,size); return 1;
#endif
}
static int safe_write(uint64_t dst, const void *src, size_t size) {
    if (dst<0x10000 || dst>0x00007fffffffffffull) return 0;
#ifdef _WIN32
    SIZE_T put=0;
    return WriteProcessMemory(GetCurrentProcess(),(LPVOID)(uintptr_t)dst,src,size,&put) && put==size;
#else
    memcpy((void *)(uintptr_t)dst,src,size); return 1;
#endif
}
enum { U2_CAM_OTHER, U2_CAM_FOLLOW, U2_CAM_LOOK, U2_CAM_ZOOM, U2_CAM_COVER, U2_CAM_CHASE };
/* The first bytes of each update in U2 1.02 (US 1.02: 0x42d940, 0x432450, 0x452c50, 0x417450, 0x4496b0), RIP-relative
 * displacements masked (-1), -2 ends. Each is found once in the 1.02 image. */
#define U2_PROLOGUE 0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec
static const short u2_update_code[][52]={
    [U2_CAM_FOLLOW]={U2_PROLOGUE,0xb8,0x01,0x00,0x00,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfe,0x4c,0x8b,0x25,-1,-1,-1,-1,
                     0x49,0x8b,0x04,0x24,0x48,0x89,0x45,0xd0,0x49,0x8b,0x07,-2},
    [U2_CAM_LOOK]={U2_PROLOGUE,0xc8,0x01,0x00,0x00,0x49,0x89,0xd6,0x49,0x89,0xf4,0x49,0x89,0xff,0x48,0x8b,0x05,-1,-1,-1,-1,
                   0x48,0x8b,0x00,0x48,0x89,0x45,0xd0,0x41,0x8b,0xbc,0x24,0x78,-2},
    [U2_CAM_ZOOM]={U2_PROLOGUE,0x98,0x01,0x00,0x00,0x49,0x89,0xd6,0x49,0x89,0xf7,0x48,0x89,0xfb,0x4c,0x8b,0x2d,-1,-1,-1,-1,
                   0x49,0x8b,0x45,0x00,0x48,0x89,0x45,0xd0,0x4c,0x8d,0x25,-2},
    [U2_CAM_COVER]={U2_PROLOGUE,0x68,0x01,0x00,0x00,0x49,0x89,0xd7,0x48,0x89,0xf3,0x49,0x89,0xfe,0x4c,0x8b,0x2d,-1,-1,-1,-1,
                    0x49,0x8b,0x45,0x00,0x48,0x89,0x45,0xd0,0x8a,0x8b,0x74,0x01,-2},
    [U2_CAM_CHASE]={U2_PROLOGUE,0x88,0x01,0x00,0x00,0x49,0x89,0xd7,0x49,0x89,0xf4,0x48,0x8b,0x0d,-1,-1,-1,-1,
                    0x48,0x8b,0x01,0x48,0x89,0x45,0xd0,0x41,0x83,0xbc,0x24,0xd8,0x09,0x00,0x00,-2},
};
static uint64_t u2_dt_holder;   /* the game's clock holder (US 1.02: image+0x1161ee8), from the cover update's lea of it */
static int u2_camera_kind(uint64_t update) {
    static struct { uint64_t update; int kind; } known[16];
    static int n;
    for (int i=0;i<n;++i) if (known[i].update==update) return known[i].kind;
    unsigned char code[52];
    int kind=U2_CAM_OTHER;
    if (safe_read(code,update,sizeof(code)))
        for (int k=U2_CAM_FOLLOW;k<=U2_CAM_CHASE && kind==U2_CAM_OTHER;++k) {
            int same=1;
            for (int i=0;i<52 && same && u2_update_code[k][i]!=-2;++i) same=u2_update_code[k][i]<0 || code[i]==u2_update_code[k][i];
            if (same) kind=k;
        }
    if (kind==U2_CAM_COVER && !u2_dt_holder) {   /* lea rdx, [rip+holder] at +0x424 */
        unsigned char lea[7];
        if (safe_read(lea,update+0x424,7) && lea[0]==0x48 && lea[1]==0x8d && lea[2]==0x15) {
            int32_t disp; memcpy(&disp,lea+3,4);
            u2_dt_holder=update+0x424+7+(uint64_t)(int64_t)disp;
        }
    }
    if (n<16) { known[n].update=update; known[n].kind=kind; ++n; }
    if (mouse_debug()) printf("Mouse: u2 camera update %#llx kind %d\n",(unsigned long long)update,kind);
    return kind;
}
/* The game's frame time (seconds; 0 while paused): float at [[holder]+0x70]+0x14. 1/60 when not known. */
static float u2_frame_dt(void) {
    uint64_t holder=0, clock=0;
    float dt=0.0f;
    if (!u2_dt_holder || !safe_read(&holder,u2_dt_holder,8) || holder<0x10000 || !safe_read(&clock,holder+0x70,8) ||
        clock<0x10000 || !safe_read(&dt,clock+0x14,4)) return 1.0f/60.0f;
    return dt;
}
/* The mouse's turn (degrees) for this frame from a queue; the same camera updated twice in one frame gets the same
 * step. Returns 0 while paused (the motion is dropped). */
typedef struct { float frame_yaw, frame_pitch; uint64_t frame_us, frame_camera; } U2Turn;
static int u2_mouse_turn(U2Turn *t, int queue, uint64_t camera, float *yaw, float *pitch) {
    const uint64_t now=now_us();
    if (t->frame_us && now-t->frame_us<1500 && t->frame_camera==camera) { *yaw=t->frame_yaw; *pitch=t->frame_pitch; return 1; }
    float dx, dy;
    camera_take(queue,&dx,&dy);
    if (!(u2_frame_dt()>0.0f)) return 0;
    const float k=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    *yaw=dx*k; *pitch=(input.mouse_invert_y ? -dy : dy)*k;
    t->frame_us=now; t->frame_camera=camera; t->frame_yaw=*yaw; t->frame_pitch=*pitch;
    return 1;
}
static void quat_mul(const float *a, const float *b, float *o) { /* quaternions x y z w, o = a * b */
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
static volatile uint64_t u2_follow_seen_us, u2_cover_seen_us, u2_look_seen_us, u2_chase_us;
static volatile uint64_t u2_zoom_us;              /* last frame the zoom (aiming) camera was updated */
static volatile uint64_t u2_zoom_top_us;          /* it ran on top of the stack (the aim, also the truck turret with the aim released) */
static volatile uint64_t u2_zoom_under_us;        /* under the walking camera, blending out (L2 held in a roll): not an aim */
static volatile uint64_t u2_zoom_below_cover_us;  /* under the cover or the shield's look camera, blending out: not an aim */
static volatile uint64_t u2_zoom_below_look_us;
/* Cover camera: it turns its direction (x y z w at camera+0x1c0) about the vertical by the stick and moves its pitch
 * (degrees, camera+0x1b0, limits -[shape+0xc]..[shape+8], shape = *(camera+0x1290)); the mouse turns both directly, and
 * the camera's position (a spring at +0x1d0) about its pivot (+0x230) by the same angle, so there is nothing left to
 * catch up. While the cover blends in (the stack entry's weight at +0x34 stays at 0 for a fifth of a second, then rises
 * in about a second) the cover is brought in from the first frame, linearly in 0.35 s, and the state is turned by the
 * mouse's turn since the cover began divided by the weight (at most twice), so the picture follows the mouse. */
static U2Turn u2_cover_turn;
static void u2_cover_camera(uint64_t camera, uint64_t entry) {
    u2_cover_seen_us=now_us();
    if (aim_native || look_held) { float ix, iy; camera_take(3,&ix,&iy); return; }   /* (the aim or look camera has this motion) */
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_cover_turn,3,camera,&step,&step_pitch)) return;
    float *dir=(float *)(uintptr_t)(camera+0x1c0);
    float *pitch=(float *)(uintptr_t)(camera+0x1b0);
    uint64_t shape=0;
    safe_read(&shape,camera+0x1290,8);
    { /* a camera that has just been made (the cover camera of the next spot) does not hold usable numbers yet */
        const float *pos=(const float *)(uintptr_t)(camera+0x1d0), *pivot=(const float *)(uintptr_t)(camera+0x230);
        const float sum=dir[0]+dir[1]+dir[2]+*pitch+pos[0]+pos[1]+pos[2]+pivot[0]+pivot[1]+pivot[2];
        if (!(sum>-1e6f && sum<1e6f) || shape<0x10000) return;
    }
    float limits[2];   /* pitch limits: up to [shape+8], down to -[shape+0xc] (a bad shape pointer: no turn) */
    if (!safe_read(limits,shape+8,sizeof(limits)) || !(limits[0]>-360.0f && limits[0]<360.0f && limits[1]>-360.0f && limits[1]<360.0f)) return;
    u2_native_us=now_us();
    {
        /* two cover cameras are updated together when one cover leads to another: one slot per camera */
        static struct { uint64_t camera, seen_us, start_us, frame_us; float total_yaw, total_pitch, done_yaw, done_pitch; int entering; } slot[2];
        const uint64_t now=now_us();
        int si=-1;
        for (int i=0;i<2;++i) if (slot[i].camera==camera && now-slot[i].seen_us<300000) si=i;
        float w=1.0f;
        if (entry>0x10000) { float f; if (safe_read(&f,entry+0x34,4) && f>=0.0f && f<=1.0f) w=f; }
        if (si<0) {   /* a cover camera seen for the first time (or after a pause): the motion queued before it is dropped */
            si=slot[0].seen_us<=slot[1].seen_us ? 0 : 1;
            memset(&slot[si],0,sizeof(slot[si]));
            slot[si].camera=camera; slot[si].start_us=now;
            slot[si].entering=w<0.05f;   /* (a camera already in the mix, blending out, is not entering) */
            step=step_pitch=0.0f;
        }
        slot[si].seen_us=now;
        if (u2_cover_turn.frame_us!=slot[si].frame_us) { slot[si].frame_us=u2_cover_turn.frame_us; slot[si].total_yaw+=step; slot[si].total_pitch+=step_pitch; }
        if (w>=0.999f) slot[si].entering=0;
        if (slot[si].entering) {
            float ramp=(float)(now-slot[si].start_us)*(1.0f/350000.0f); ramp=ramp>1.0f ? 1.0f : ramp;
            if (ramp>w) { w=ramp; if (entry>0x10000) { safe_write(entry+0x34,&w,4); safe_write(entry+0x38,&w,4); } }
        }
        const float k=slot[si].entering && w<0.999f ? 1.0f/(w<0.5f ? 0.5f : w) : 1.0f;
        const float want_yaw=slot[si].total_yaw*k, want_pitch=slot[si].total_pitch*k;
        step=want_yaw-slot[si].done_yaw; slot[si].done_yaw=want_yaw;
        step_pitch=want_pitch-slot[si].done_pitch; slot[si].done_pitch=want_pitch;
    }
    if (step!=0.0f) {
        const float a=step*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        float *pos=(float *)(uintptr_t)(camera+0x1d0);
        const float *pivot=(const float *)(uintptr_t)(camera+0x230);
        const float px=pos[0]-pivot[0], pz=pos[2]-pivot[2];
        pos[0]=pivot[0]+px*c-pz*s;
        pos[2]=pivot[2]+px*s+pz*c;
    }
    if (step_pitch!=0.0f) {
        const float v=*pitch-step_pitch, lo=-limits[1], hi=limits[0];
        *pitch=v<lo ? lo : v>hi ? hi : v;
    }
    if (mouse_debug() && (step!=0.0f || step_pitch!=0.0f))
        printf("Mouse: u2 cover t=%.4f step=%.3f pitch step=%.3f pitch=%.3f\n",now_us()*1e-6,step,step_pitch,*pitch);
}
/* Look-around camera (R3; update 0x432450 in US 1.02): its yaw and pitch come from the previous orientation quaternion
 * (input block +0x10); the mouse turns that quaternion, yaw about the vertical on the left, pitch about the side on the
 * right. It is also the shield's camera: without R3 (and its blend-out) it keeps the stick. */
static U2Turn u2_look_turn;
static void u2_look_camera(uint64_t camera, uint64_t input_block) {
    u2_look_seen_us=now_us();
    if (!look_held && now_us()>=look_blend_until_us) return;
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_look_turn,2,camera,&step,&step_pitch)) return;
    u2_native_us=now_us();
    if (step==0.0f && step_pitch==0.0f) return;
    float *q=(float *)(uintptr_t)(input_block+0x10);
    const float yaw[4]={0.0f,SDL_sinf(-step*0.5f*0.017453293f),0.0f,SDL_cosf(-step*0.5f*0.017453293f)};
    const float pitch[4]={SDL_sinf(step_pitch*0.5f*0.017453293f),0.0f,0.0f,SDL_cosf(step_pitch*0.5f*0.017453293f)};
    float turned[4], result[4];
    quat_mul(yaw,q,turned);
    quat_mul(turned,pitch,result);
    const float n=SDL_sqrtf(result[0]*result[0]+result[1]*result[1]+result[2]*result[2]+result[3]*result[3]);
    if (!(n>0.5f)) return;
    for (int i=0;i<4;++i) q[i]=result[i]/n;
}
/* Chapter 5 truck chase (update 0x4496b0 in US 1.02): a yaw and a pitch offset (degrees, camera+0x188 and +0x18c) let the
 * player look around a little (about -15..15 degrees; the picture turns 0.88 degrees per degree, and 1.7 % of the offset
 * goes back to zero each frame). The mouse sets the offsets: the target is kept here and written before each update, a
 * little above to make up for what the update takes back, so the look holds where the mouse leaves it. */
static U2Turn u2_chase_turn;
static void u2_chase_camera(uint64_t camera) {
    if (aim_native || look_held) return;
    u2_chase_us=now_us();
    float *off=(float *)(uintptr_t)(camera+0x188);
    static uint64_t sess_us, sess_camera; static float target_yaw, target_pitch;
    const uint64_t now=now_us();
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_chase_turn,2,camera,&step,&step_pitch)) return;
    if (!(off[0]>-60.0f && off[0]<60.0f && off[1]>-60.0f && off[1]<60.0f)) return;   /* (a camera not set yet) */
    if (camera!=sess_camera || now-sess_us>300000) { sess_camera=camera; target_yaw=off[0]; target_pitch=off[1]; step=step_pitch=0.0f; }
    sess_us=now;
    u2_native_us=now;
    target_yaw-=step/0.88f; target_pitch+=step_pitch/0.88f;
    target_yaw=target_yaw<-15.0f ? -15.0f : target_yaw>15.0f ? 15.0f : target_yaw;
    target_pitch=target_pitch<-13.0f ? -13.0f : target_pitch>13.0f ? 13.0f : target_pitch;
    off[0]=target_yaw/0.983f; off[1]=target_pitch/0.983f;
}
/* Zoom camera (aiming with L2, the grenade with L1; update 0x452c50 in US 1.02): the stick turns its unit aim direction
 * at camera+0x1f30; the mouse turns it directly (to the right raises atan2(z, x), down lowers the elevation). Two zoom
 * cameras (one per shoulder) are updated while the shoulder switches: both get the same turn. The first frame of an aim
 * drops the motion queued before it. After the aim is released the game blends the camera out for about a second and it
 * keeps the mouse meanwhile, unless it blends out under the cover camera (the cover takes the mouse at once) or under
 * the shield's look camera (the stick turns the picture). */
static U2Turn u2_zoom_turn;
static void u2_zoom_camera(uint64_t camera) {
    static uint64_t last_frame_us;
    const uint64_t now=now_us();
    const int first_frame=now-last_frame_us>300000;
    last_frame_us=now;
    if ((!aim_held && now>=aim_blend_until_us) || look_held) return;
    if (!aim_held && u2_zoom_below_look_us && now-u2_zoom_below_look_us<100000) return;
    if (!aim_held && u2_zoom_below_cover_us && now-u2_zoom_below_cover_us<100000) return;
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_zoom_turn,2,1,&step,&step_pitch)) return;
    u2_native_us=now;
    if (first_frame) {
        step=step_pitch=0.0f;
        u2_zoom_turn.frame_yaw=u2_zoom_turn.frame_pitch=0.0f;   /* (the second shoulder camera of the frame reads these too) */
    }
    if (step==0.0f && step_pitch==0.0f) return;
    float *d=(float *)(uintptr_t)(camera+0x1f30);
    if (!(d[0]>-1.5f && d[0]<1.5f && d[1]>-1.5f && d[1]<1.5f && d[2]>-1.5f && d[2]<1.5f)) return; /* a new camera: not set yet */
    if (step!=0.0f) {
        const float a=step*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=d[0], z=d[2];
        d[0]=x*c-z*s;
        d[2]=x*s+z*c;
    }
    if (step_pitch!=0.0f) {
        const float h=SDL_sqrtf(d[0]*d[0]+d[2]*d[2]);
        if (h>1e-4f) {
            float e=SDL_atan2f(d[1],h)*57.29578f-step_pitch;
            e=e>70.0f ? 70.0f : e<-85.0f ? -85.0f : e;
            const float r=e*0.017453293f, k=SDL_cosf(r)/h;
            d[0]*=k; d[2]*=k; d[1]=SDL_sinf(r);
        }
    }
    if (mouse_debug()) printf("Mouse: u2 aim t=%.4f step=%.3f pitch step=%.3f dir=(%.3f %.3f %.3f)\n",now*1e-6,step,step_pitch,d[0],d[1],d[2]);
}
static void __attribute__((ms_abi)) u2_camera_manager(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    static int disabled=-1;
    if (disabled<0) { const char *e=getenv("BB_U2_NATIVE"); disabled=e && atoi(e)==0; }
    const uint64_t update=regs->r8, camera=regs->r10;
    if (disabled || !gyro_on() || camera<0x10000 || update<0x10000) return;
    /* the thunk moves rsp down 0x80 (the red zone) before it saves the block: the guest's rsp is 0x80 above the block */
    const uint64_t rsp=(uint64_t)(uintptr_t)(regs+1)+0x80;
    const uint64_t now=now_us();
    if (!u2_cams_us) printf("Mouse: U2 native cameras on (aim, cover, look around, truck chase)\n");
    u2_cams_us=now;
    switch (u2_camera_kind(update)) {
    case U2_CAM_FOLLOW: u2_follow_seen_us=now; break;   /* turned by u2_follow_camera */
    case U2_CAM_COVER: u2_cover_camera(camera,regs->r13); break;
    case U2_CAM_LOOK: u2_look_camera(camera,rsp+0xe0); break;
    case U2_CAM_CHASE: u2_chase_camera(camera); break;
    case U2_CAM_ZOOM:
        u2_zoom_us=now;
        if (u2_follow_seen_us && now-u2_follow_seen_us<3000) u2_zoom_under_us=now;
        else if (u2_cover_seen_us && now-u2_cover_seen_us<3000) u2_zoom_below_cover_us=now;
        else if (u2_look_seen_us && now-u2_look_seen_us<3000) { u2_zoom_below_cover_us=now; u2_zoom_below_look_us=now; }
        else u2_zoom_top_us=now;
        u2_zoom_camera(camera);
        break;
    default: break;
    }
}
/* Gyro aiming, from Umar-Qayyum-dev's raw mouse aiming (Supermedo/uncharted_pc#561), U1 1.00 and 1.02.
 * The cover camera (CameraControlCover) turns with the right stick in its own update: its horizontal
 * direction (x, 0, z) turns around the vertical, and its pitch (degrees) moves within limits (up to
 * limits[0], down to -limits[1]). At the start of the update the mouse turns both directly, the same
 * amount as the follow camera. */
static void cover_camera(float *dir, float *pitch, const float *limits) {
    if (!gyro_on()) return;
    const uint64_t now=now_us();
    if (cover_hook_us && now-cover_hook_us>100000 && !(camera_hook_us && now-camera_hook_us<100000)) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
    }
    cover_hook_us=now;
    follow_push=0.0f;
    float dx, dy;
    frame_motion(now,&dx,&dy);
    if (aim_held && !aim_starting(now)) return; /* the aiming camera takes the mouse */
    if (mouse_debug()) printf("Mouse: cover t=%.4f dx=%.1f dy=%.1f heading=%.2f pitch=%.2f limits=%.1f..%.1f\n",
                              now*1e-6,dx,dy,heading_of(dir),*pitch,limits ? -limits[1] : 0.0f,limits ? limits[0] : 0.0f);
    const float turn=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
    }
    cover_view=heading_of(dir);
    if (dy!=0.0f && limits) {
        float v=*pitch-(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        if (v>limits[0]) v=limits[0];
        if (v<-limits[1]) v=-limits[1];
        *pitch=v;
    }
}
/* Turns an aim direction (x, y, z) by yaw and pitch (degrees), keeping its length; pitch stays
 * within +-70 degrees. Same yaw sense as the follow camera: a positive yaw lowers the heading. */
static void aim_turn(float *v, float yaw, float pitch) {
    const float x=v[0], y=v[1], z=v[2];
    const float len=SDL_sqrtf(x*x+y*y+z*z), h=SDL_sqrtf(x*x+z*z);
    if (!(len>0.5f && len<1.5f) || h<1e-4f) return;
    const float a=yaw*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
    const float hx=(x*c-z*s)/h, hz=(x*s+z*c)/h;
    float p=SDL_atan2f(y,h)+pitch*0.017453293f;
    if (p>1.2217f) p=1.2217f;
    if (p<-1.2217f) p=-1.2217f;
    v[0]=hx*SDL_cosf(p)*len; v[1]=SDL_sinf(p)*len; v[2]=hz*SDL_cosf(p)*len;
}
/* Popping out keeps the aim: the first aim direction of a press turns to press_view, unless that
 * is more than this far. Further away Drake turns his body to aim, and the game itself swings the
 * aim round (40-110 degrees within 0.3 s, measured): a turn of ours on top doubled it. */
#define POP_OUT_MAX_DEGREES 30.0f
/* Raw aim. The aim controller (U1 1.02 0x276a10; rsi = the controller) gets the aiming camera's
 * remembered aim direction (camera+0x1830) in rcx: it turns it by the stick, the camera saves it
 * back, and the shown direction adds aim assist and recoil on top. Called from the aiming camera's
 * own call site (return address main_return), the mouse turns that direction directly; other
 * callers are left alone and take no mouse motion. The return address is above the
 * registers the hook saved (the hook is at the function's first instruction). */
static void raw_aim(const char *game, uint64_t controller, const RuntimeGuestRegs *regs, uint64_t image,
                    uint64_t main_return, uint64_t main_return_alt) {
    if (!gyro_on()) return;
    /* the thunk (probe.c) moves rsp down 0x80 (the red zone) before it saves the block, so the return address is
     * the word 0x80 bytes above the block */
    const uint64_t ret=*(const uint64_t *)((uintptr_t)(regs+1)+0x80)-image;
    float *dir=(float *)(uintptr_t)regs->rcx;
    static uint64_t other_logged;
    if (ret!=main_return && ret!=main_return_alt) {
        if (other_logged!=ret) {
            other_logged=ret;
            printf("Gyro: %s aim controller from another caller (return %#llx), left alone\n",game,
                   (unsigned long long)ret);
        }
        return;
    }
    static int first=1;
    if (first) {
        first=0;
        printf("Gyro: %s raw aim on: controller %#llx, remembered direction %#llx (controller %+lld)\n",
               game,(unsigned long long)controller,(unsigned long long)(uintptr_t)dir,
               (long long)((uintptr_t)dir-controller));
    }
    const uint64_t now=now_us();
    pthread_mutex_lock(&camera_lock);
    const float dx=aim_dx, dy=aim_dy;
    aim_dx=aim_dy=0.0f;
    pthread_mutex_unlock(&camera_lock);
    aim_direct_us=now;
    const float k=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    if (mouse_debug()) printf("Mouse: aim t=%.4f ctl=%#llx yaw=%.3f pitch=%.3f dx=%.1f dy=%.1f\n",now*1e-6,(unsigned long long)controller,
                              heading_of(dir),SDL_atan2f(dir[1],SDL_sqrtf(dir[0]*dir[0]+dir[2]*dir[2]))*57.29578f,dx,dy);
    /* While aim is held the game may swap in new aiming cameras (after a roll: a new one about
     * every 100 ms while Drake gets up), which threw away the turns given to the one before. A new
     * controller in the same aim press gets the press's turns on top of the game's own direction; on
     * the same controller the game's own changes (aim assist, recoil) stay. Two aiming cameras can
     * also take turns within one frame (blending): each controller keeps how much of this press's
     * turns it already has, so each gets every turn exactly once. The pop-out turn (press_view) is
     * the press's first turn. */
    static uint64_t session_press;
    static float turned_yaw, turned_pitch; /* this press's turns (degrees) */
    static struct { uint64_t controller, last_us; float yaw, pitch; } given[4];
    static float last_dir[3];
    static int have_last;
    /* The pop-out turn still to give: in one frame it was a snap (2-25 degrees, at times 60-80), so it
     * eases in, most of it within 0.2 s (a 0.06 s time constant). */
    static float pop_left;
    static uint64_t pop_us;
    if (session_press!=aim_press_us) {
        session_press=aim_press_us;
        turned_yaw=turned_pitch=0.0f;
        pop_left=0.0f;
        pop_us=now;
        memset(given,0,sizeof(given));
        have_last=0;
        /* the mouse turned the view during the shoulder move: start where it looks now */
        float view;
        if (view_now(now,&view)) { press_view=view; press_view_ok=1; }
        if (press_view_ok) {
            const float pop=wrap_degrees(heading_of(dir)-press_view);
            const int kept=SDL_fabsf(pop)<=POP_OUT_MAX_DEGREES;
            if (kept) pop_left=pop;
            printf("Gyro: %s aim press: view %.1f, aim %.1f, turn %.1f%s\n",game,press_view,heading_of(dir),pop,
                   kept ? "" : " (too far, the game's aim kept)");
        } else {
            printf("Gyro: %s aim press: no view known, the game's aim kept\n",game);
        }
    }
    if (!aim_held) return;
    int slot=0;
    for (int i=0;i<4;++i) {
        if (given[i].controller==controller) { slot=i; break; }
        if (given[i].last_us<given[slot].last_us) slot=i;
    }
    /* A new controller that starts where the last one ended (U1 swaps aiming cameras while aiming)
     * copied the turns already; one that starts elsewhere (after a roll: the game's own direction)
     * gets them. */
    if (given[slot].controller!=controller) {
        given[slot].controller=controller;
        given[slot].yaw=given[slot].pitch=0.0f;
        if (have_last && dir[0]*last_dir[0]+dir[1]*last_dir[1]+dir[2]*last_dir[2]>0.9986f) { /* within 3 degrees */
            given[slot].yaw=turned_yaw; given[slot].pitch=turned_pitch;
        }
    }
    given[slot].last_us=now;
    if (pop_left!=0.0f) {
        const float give=SDL_fabsf(pop_left)<0.05f ? pop_left
                         : pop_left*(1.0f-SDL_expf(-(float)(now-pop_us)*1e-6f/0.06f));
        turned_yaw+=give;
        pop_left-=give;
    }
    pop_us=now;
    if (dx!=0.0f || dy!=0.0f) {
        turned_yaw+=dx*k;
        turned_pitch+=-(input.mouse_invert_y ? -dy : dy)*k;
    }
    const float yaw=turned_yaw-given[slot].yaw, pitch=turned_pitch-given[slot].pitch;
    if (yaw!=0.0f || pitch!=0.0f) aim_turn(dir,yaw,pitch);
    given[slot].yaw=turned_yaw; given[slot].pitch=turned_pitch;
    last_dir[0]=dir[0]; last_dir[1]=dir[1]; last_dir[2]=dir[2]; have_last=1;
}
static void __attribute__((ms_abi)) u1_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                      uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    /* the aiming camera's update calls it with rcx = camera+0x1830: 1.02 at 0xc4c52 (the function is 0x276a10, the
     * update 0xc43b0), 1.00 at 0xc4b62 (0x2764c0; the code around is the same). Another build is left alone. */
    raw_aim("U1",controller,regs,image,0xc4c57,0xc4b67);
}
/* U1 1.02 cover camera update (vtable 0x927320): rsi = the camera, direction +0x240, pitch +0x230,
 * its settings at +0x1300 with the pitch limits at +0x8 (up) and +0xc (down). */
static void __attribute__((ms_abi)) u1_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                    uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    const float *settings=*(const float *const *)(uintptr_t)(camera+0x1300);
    cover_camera((float *)(uintptr_t)(camera+0x240),(float *)(uintptr_t)(camera+0x230),settings ? settings+2 : NULL);
}
/* U1: the game's pad update (image+0x117200) fills the caller's joypad state (its first argument):
 * the sticks as floats lx ly rx ry at +0 and again at +0x10, converted from the pad's bytes. Cameras
 * other than the follow camera (aiming: about 45 degrees/s * x|x|, no speed ramp) get the mouse as a
 * right stick value there, not limited to the stick's range. */
static uint64_t joypad_state;         /* the state the pad update fills (its first argument) */
static uint64_t sticks_last_us;
static float aim_test=-1.0f;          /* BB_MOUSE_AIM_TEST=<x>: right stick x while not following */
/* The aiming camera's response to the right stick: degrees/s at stick values 0.1.. (measured in real aiming
 * with a gun, per game and build, using BB_MOUSE_CAL). Version 1.02 turns the stick up to 1 and nothing above is
 * faster (about 46 degrees/s sideways, 30 or 20 up and down); 1.00 keeps getting faster above 1 and is slower
 * in the middle. U2 and U3 turn alike in 1.02; U2 1.00 is not measured (its dev menu task jump fails) and uses
 * the U3 1.00 numbers. The mouse asks for a rate; the stick that gives it is looked up here. */
typedef struct { int n; float x[12], rate[12]; } StickTable;
typedef struct { StickTable h, v; } AimCurve;
static const AimCurve u1_100_aim={
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,1.1f,3.1f,6.0f,10.1f,15.3f,21.6f,29.3f,38.3f,47.8f,74.1f,157}},
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,.5f,1.4f,2.5f,4.3f,6.4f,9.1f,12.3f,16.1f,19.8f,31,39.6f}}};
static const AimCurve u1_102_aim={
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,3.3f,7.1f,11.4f,16.2f,21.5f,27.0f,33.0f,39.1f,45.0f}},
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,1.5f,3.1f,5.1f,7.1f,9.5f,11.9f,14.6f,17.1f,19.9f}}};
static const AimCurve u23_100_aim={
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,1.1f,3.0f,5.9f,9.8f,14.8f,21.0f,28.3f,37.0f,46.3f,72.1f,153.9f}},
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,.65f,1.85f,3.65f,6.15f,9.4f,13.4f,18.1f,23.6f,29.5f,46.5f,62.6f}}};
static const AimCurve u23_102_aim={
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,3.4f,6.8f,11.2f,16.3f,21.5f,26.9f,33.1f,40.5f,46.0f}},
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,2.1f,4.5f,7.4f,10.5f,14.0f,17.7f,21.6f,25.8f,30.0f}}};
static float stick_for_rate(const StickTable *t, float rate) {
    const float r=rate<0 ? -rate : rate;
    float x=t->x[t->n-1];
    if (r<t->rate[t->n-1]) {
        int i=0;
        while (i<t->n-2 && r>t->rate[i+1]) ++i;
        x=t->x[i]+(r-t->rate[i])/(t->rate[i+1]-t->rate[i])*(t->x[i+1]-t->x[i]);
    }
    if (r<=0.0f) x=0.0f;
    return rate<0 ? -x : x;
}
/* The follow camera's response to the right stick value (measured in U1): about 144 x^2 degrees/s up
 * to x = 0.5, then a speed that builds up over a second towards 240 degrees/s from x = 0.7 (and stays
 * there above 0.9): fast turns reach it late. */
static float follow_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    float x=r<=36.0f ? SDL_sqrtf(r/144.0f) : 0.5f+(r-36.0f)/(240.0f-36.0f)*0.4f;
    if (x>0.9f) x=0.9f;
    return rate<0 ? -x : x;
}
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)r14;
    if (mouse_debug() && joypad_state!=state)
        printf("Mouse: joypad state %#llx (image %#llx, offset %#llx)\n",(unsigned long long)state,
               (unsigned long long)image,(unsigned long long)(state-image));
    joypad_state=state;
    u1_image=image;
}
static float aim_rate[2];             /* the smoothed aiming turn rate (degrees/s) */
/* U1 1.02: while aiming (L2) the camera manager is sent a message every frame (handler image+0xb5780,
 * message 0xa605802) that aligns the first camera stack entry with a transform: position at +0x00 and
 * orientation quaternion (x y z w) at +0x10, apparently the player's. The aiming camera's yaw comes
 * from it (image+0xb1fd0 turns the quaternion into angles), so whatever is written into the camera
 * is brought back to it next frame; the stick turns it by 45 x|x| degrees/s at most (the stick is
 * limited to the unit circle before the game sees it). The mouse's yaw is multiplied into this
 * quaternion, about the vertical axis, just before the handler reads it (image+0xb5c62: rax = the
 * message argument, a pointer to the transform's pointer, as the thunk saved it at guest_callback_rsp-16),
 * and the stick's x is left out while this runs. The turn is spread over a few frames so that single
 * pixels do not step. A positive turn is a turn to the right (the yaw angle lowers). */
/* U1 1.02: the camera manager (image+0xb5e50, per entry image+0xb60d0) runs a stack of three cameras
 * while aiming (L2): the follow camera, the aiming camera (update image+0xb7430) and, on top with a
 * blend weight of 1, the zoom camera (update image+0xc43b0), whose output is the picture; the lower
 * ones do not count. A camera's output block (rsp + 0x120 of the manager's function, rsp as the callback
 * was entered is guest_callback_rsp) is a position (+0x00) and Euler angles in degrees: yaw +0x20,
 * pitch +0x24. With the stick the camera turns 45 x|x| degrees/s at most (the stick is limited to the
 * unit circle before the game sees it). The mouse's yaw is added to the zoom camera's output right
 * after its update (image+0xb62a7: vmovaps xmm0, [rsp+0x120] ...), and to the angles the manager keeps
 * for it (stack entry = the thunk's saved rbx, yaw at +0x70) for the next frame; the stick's x is left
 * out while this runs. The turn is spread over a few frames so that single pixels do not step. A
 * positive turn is a turn to the right (the yaw angle lowers). */
static uint64_t aim_native_us;        /* last frame the zoom camera was turned by the mouse */
static volatile int aim_session_reset = 1; /* set while not aiming: the next aim starts from a zero turn */
static float aim_pending_degrees;     /* the mouse's yaw not given to the zoom camera yet */
static float aim_pending_pitch;       /* its pitch (degrees, positive looks down) */
/* The share of the mouse's pending turn given to the aiming / look-around camera this frame. A slow mouse reports a pixel or
 * two now and then, which would turn the camera in steps, so a small pending turn is spread over about 20 ms; the time constant
 * falls smoothly towards 0 as the pending turn grows (no threshold, or the camera would seem to accelerate), so quick
 * movements are followed at once. aim_raw gives it all at once (the steps of a slow mouse are then visible). The angle turned
 * in total is the same in every case. */
#define AIM_SMOOTH_SECONDS 0.02f
#define AIM_SMOOTH_PIXELS 6.0f
static float aim_follow(float dt, float pending_yaw, float pending_pitch) {
    if (input.aim_raw) return 1.0f;
    const float pixels=SDL_sqrtf(pending_yaw*pending_yaw+pending_pitch*pending_pitch)/
                       (MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity);
    const float k=pixels/AIM_SMOOTH_PIXELS;
    return 1.0f-SDL_expf(-dt*(1.0f+k*k)/AIM_SMOOTH_SECONDS);
}
static void __attribute__((ms_abi)) u1b_aim_camera(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t camera) {
    (void)unused0; (void)unused1;
    if (mouse_debug() && camera>=0x10000) { /* which cameras the manager updates, per input state (V, cover, aim) */
        const uint64_t vt=*(const uint64_t *)(uintptr_t)camera;
        if (vt>=0x10000) {
            const uint64_t upd=((const uint64_t *)(uintptr_t)vt)[0x98/8];
            /* the set of cameras updated in one frame (calls less than 2 ms apart), printed when it changes */
            static uint64_t cur[8], last[8], last_us; static int n_cur, n_last;
            const uint64_t t=now_us();
            if (last_us && t-last_us>2000) {
                int same=n_cur==n_last;
                for (int i=0;same && i<n_cur;++i) if (cur[i]!=last[i]) same=0;
                if (!same) {
                    printf("Mouse: cameras [");
                    for (int i=0;i<n_cur;++i) printf(" %#llx",(unsigned long long)cur[i]);
                    printf(" ] aim=%d look=%d follow=%d t=%.4f\n",aim_held,look_held,
                           camera_hook_us && t-camera_hook_us<50000,t*1e-6);
                    memcpy(last,cur,sizeof(cur)); n_last=n_cur;
                }
                n_cur=0;
            }
            last_us=t;
            if (n_cur<8) cur[n_cur++]=upd-image;
        }
    }
    if (camera<0x10000) return;
    const uint64_t vtable=*(const uint64_t *)(uintptr_t)camera;
    if (vtable<0x10000 || ((const uint64_t *)(uintptr_t)vtable)[0x98/8]!=image+0xc43b0) return;
    const uint64_t now=now_us();
    zoom_camera_us=now;   /* the zoom camera runs: L1 (grenade) counts as an aim, see sample() */
    if (!aim_held) return;
    const uint64_t entry=*(const uint64_t *)(uintptr_t)(guest_callback_rsp-72);
    /* The picture while the aim blends in (about 0.5 s) is w x zoom camera + (1-w) x the cameras below it (w = the stack
     * entry's weight, float at entry+0x34, 0 -> 1 linearly; the manager's angle interpolation image+0xb2f00), and the
     * lower cameras ignore the mouse. Two more things hold the zoom camera back: for the first part of the blend its aim
     * vector is worked out again from constants every frame (image+0xc43b0), so a turn added each frame never added up.
     * So the camera is given the mouse's whole turn since the aim began, divided by w (so that w x that = the turn), and
     * what we put into the aim vector last frame is taken off again when the game kept it (it continues from it). */
    float w=1.0f;
    if (entry>0x10000) { w=*(const volatile float *)(uintptr_t)(entry+0x34); if (!(w<1.0f)) w=1.0f; else if (w<0.2f) w=0.2f; }
    const int fade=0;
    const float f=1.0f;
    /* two instances of the zoom camera (one per shoulder) are updated in the same frame while the shoulder
     * is switched: both get the same turn, the mouse's motion being taken once per frame */
    static uint64_t frame_us; static float frame_turn, frame_turn_pitch;
    static float embedded_yaw, embedded_pitch;   /* the turn written into the aim vector (the camera's state) so far (degrees) */
    static float total_yaw, total_pitch;         /* the mouse's turn since the aim began (degrees) */
    if (aim_session_reset) {
        aim_session_reset=0; total_yaw=total_pitch=embedded_yaw=embedded_pitch=0.0f;
        aim_pending_degrees=aim_pending_pitch=0.0f; frame_us=0;
    }
    const int new_frame=!(frame_us && now-frame_us<1500);
    if (new_frame) {
        if (aim_native_us && now-aim_native_us>100000) aim_pending_degrees=aim_pending_pitch=0.0f;
        float dx, dy;
        camera_take(2,&dx,&dy);
        if (camera_stopped(image,U1B_CAMERA_DT)) { aim_pending_degrees=aim_pending_pitch=0.0f; return; } /* paused */
        aim_pending_degrees+=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity;
        aim_pending_pitch+=(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity;
        float dt=*(const volatile float *)(uintptr_t)(image+U1B_CAMERA_DT);
        if (!(dt>0.0005f && dt<0.05f)) dt=1.0f/60.0f;
        const float follow=aim_follow(dt,aim_pending_degrees,aim_pending_pitch);
        const float step=aim_pending_degrees*follow, step_pitch=aim_pending_pitch*follow;
        aim_pending_degrees-=step;
        aim_pending_pitch-=step_pitch;
        total_yaw+=step; total_pitch+=step_pitch;
        frame_us=now;
        /* the turn the camera's state still lacks (none while the aim blends in: only the output is turned then) */
        frame_turn=fade ? 0.0f : total_yaw-embedded_yaw;
        frame_turn_pitch=fade ? 0.0f : total_pitch-embedded_pitch;
        aim_target_total=wrap_degrees(aim_target_total+step); aim_target_us=now;
    }
    aim_native_us=now;
    float *aim=(float *)(uintptr_t)(camera+0x1830);
    float *yaw=(float *)(uintptr_t)(guest_callback_rsp+0x140);
    float *pitch=yaw+1; /* the output's pitch angle follows (degrees, positive looks down) */
    if (fade) {
        /* the state is left alone: the game works it out again partly from the previous one (a turn put into it fed back and ran away) */
        *yaw=wrap_degrees(*yaw-total_yaw*f);
        float p=*pitch+total_pitch*f;
        *pitch=p>70.0f ? 70.0f : p<-70.0f ? -70.0f : p;
    } else {
        if (frame_turn!=0.0f) {
            const float a=frame_turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
            const float ax=aim[0], az=aim[2];
            aim[0]=ax*c-az*s;
            aim[2]=ax*s+az*c;
            *yaw=wrap_degrees(*yaw-frame_turn);
        }
        float achieved=frame_turn_pitch;
        if (frame_turn_pitch!=0.0f) {
            /* the aim vector looks from the camera to the player, so looking down lowers its elevation */
            const float h=SDL_sqrtf(aim[0]*aim[0]+aim[2]*aim[2]);
            if (h>1e-4f) {
                float e=SDL_atan2f(aim[1],h)*57.29578f;
                float e2=e-frame_turn_pitch;
                e2=e2>70.0f ? 70.0f : e2<-70.0f ? -70.0f : e2;
                const float r=e2*0.017453293f, k=SDL_cosf(r)/h;
                aim[0]*=k; aim[2]*=k; aim[1]=SDL_sinf(r);
                *pitch-=e2-e;
                achieved=e-e2;
            }
        }
        if (new_frame) { embedded_yaw=total_yaw; embedded_pitch+=achieved; total_pitch=embedded_pitch; }
    }
    if (new_frame && fade) { embedded_yaw=0.0f; embedded_pitch=0.0f; }
    if (entry>0x10000) { *(float *)(uintptr_t)(entry+0x70)=*yaw; *(float *)(uintptr_t)(entry+0x74)=*pitch; }
    if (mouse_debug()) {
        float shown=0.0f; double age=0.0;
        const int have=bbgpu_camera_yaw(&shown,&age);
        printf("Mouse: aimturn t=%.4f camera=%#llx w=%.2f fade=%d total=%.3f turn=%.3f pitch total=%.3f turn=%.3f yaw=%.3f pitch=%.3f aim=(%.3f %.3f %.3f) shown=%.2f%s\n",now*1e-6,(unsigned long long)camera,w,fade,total_yaw,frame_turn,total_pitch,frame_turn_pitch,*yaw,*pitch,aim[0],aim[1],aim[2],shown,have ? "" : "?");
    }
}static uint64_t saved_register(int n);
static int safe_write(uint64_t dst, const void *src, size_t size);
/* U1 1.02: entry of the zoom camera's update (image+0xc43b0; rdx = its input block, whose float at +0x1c is the stack entry's blend weight).
 * While that weight is below 1 the update works the aim vector out again from constants and from the previous one, which dropped the mouse's
 * turns for the first part of the blend-in. With the weight shown as 1 it takes the path it takes once the blend is over (the mouse works
 * from the first frame); the manager still blends the picture with its own weight, so there is no cut. */
static void __attribute__((ms_abi)) u1b_zoom_entry(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t unused3) {
    (void)unused0; (void)unused1; (void)image; (void)unused3;
    if (!aim_held || pad_only_mode) return;
    const uint64_t block=saved_register(4);
    if (block<0x10000) return;
    const float one=1.0f;
    safe_write(block+0x1c,&one,4);
}
static const unsigned char u1b_zoom_entry_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x68,0x02,0x00,0x00,
    0x49,0x89,0xd7,0x48,0x89,0xf3,0x49,0x89,0xfe,0x4c,0x8b,0x2d,0x7c,0x67,0x88,0x00,0x49,0x8b,0x45,0x00,
};/* U1 1.02: the look-around camera (d-pad up held) is the orbit camera at image+0xb3930 (rdi the output block, rsi the
 * camera, rdx the state whose first qword points to the previous angles: yaw +0x20, pitch +0x24, degrees). It computes
 * yaw = previous - stick x * dt * speed and pitch = previous + stick y * dt * speed (limits applied after), so the
 * mouse turns the previous angles directly, spread over a few frames like the aiming camera. */
static int safe_read(void *dst, uint64_t src, size_t size);
static uint64_t saved_register(int n);
static int safe_write(uint64_t dst, const void *src, size_t size);
/* U1 1.02: the message 0xa605802 (handler image+0xb5780) hands the camera manager the player's pose (a pointer to a slot
 * that holds the pointer to the pose: position at +0x00, quaternion x y z w at +0x10); the handler turns it into the
 * entry's target (image+0xb2120, stored at entry+0x50) and the aiming cameras are pulled to it. Hooked at image+0xb5c5f
 * (just before the pose pointer is loaded; rax is the slot, saved by the thunk): while aiming, the slot is pointed to a
 * copy of the pose turned about the vertical by the mouse's yaw since the aim began, so the target moves with the
 * mouse instead of holding the camera where the player looked before. */
/* NOT INSTALLED (removed from guest_callbacks): the aim's blend-in still held the camera with it. Kept for the next try. */
static void __attribute__((ms_abi, unused)) u1b_aim_target(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t unused3) {
    (void)unused0; (void)unused1; (void)image; (void)unused3;
    static float copy[32] __attribute__((aligned(16)));
    static uint64_t original;
    if (!aim_held || aim_target_total==0.0f) return;
    const uint64_t slot=saved_register(2);
    uint64_t pose=0;
    if (!safe_read(&pose,slot,8) || pose<0x10000) return;
    if (pose==(uint64_t)(uintptr_t)copy) pose=original; /* the slot was not written again since our last turn */
    else original=pose;
    if (!safe_read(copy,pose,sizeof(copy))) return;
    const float a=aim_target_total*0.017453293f*0.5f, c=SDL_cosf(a), s=SDL_sinf(a); /* measured: the opposite sign turned the picture to the left */
    const float qx=copy[4], qy=copy[5], qz=copy[6], qw=copy[7];
    copy[4]=c*qx+s*qz;
    copy[5]=c*qy+s*qw;
    copy[6]=c*qz-s*qx;
    copy[7]=c*qw-s*qy;
    const uint64_t where=(uint64_t)(uintptr_t)copy;
    safe_write(slot,&where,8);
    if (mouse_debug()) printf("Mouse: aimtarget total=%.2f quaternion=(%.3f %.3f %.3f %.3f) -> (%.3f %.3f %.3f %.3f)\n",aim_target_total,qx,qy,qz,qw,copy[4],copy[5],copy[6],copy[7]);
}
__attribute__((unused)) static const unsigned char u1b_aim_target_code[]={
    0x4c,0x8b,0x38,0x49,0x8b,0x9e,0xf0,0x00,0x00,0x00,0x48,0x8d,0xbd,0xd0,0xfe,0xff,0xff,0x4c,0x89,0xfe,0xe8,
};
static uint64_t look_native_us;       /* last frame the look camera was turned by the mouse */
static float look_pending_yaw, look_pending_pitch;
static void __attribute__((ms_abi)) u1b_look_camera(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t unused3) {
    (void)unused0; (void)unused1; (void)unused3;
    if (!look_held) return;
    const uint64_t now=now_us();
    static uint64_t frame_us; static float frame_yaw, frame_pitch;
    float step, step_pitch;
    if (frame_us && now-frame_us<1500) {
        step=frame_yaw; step_pitch=frame_pitch;
    } else {
        if (look_native_us && now-look_native_us>100000) look_pending_yaw=look_pending_pitch=0.0f;
        float dx, dy;
        camera_take(2,&dx,&dy);
        if (camera_stopped(image,U1B_CAMERA_DT)) { look_pending_yaw=look_pending_pitch=0.0f; return; } /* paused */
        look_pending_yaw+=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity;
        look_pending_pitch+=(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity;
        float dt=*(const volatile float *)(uintptr_t)(image+U1B_CAMERA_DT);
        if (!(dt>0.0005f && dt<0.05f)) dt=1.0f/60.0f;
        const float follow=aim_follow(dt,look_pending_yaw,look_pending_pitch);
        step=look_pending_yaw*follow; step_pitch=look_pending_pitch*follow;
        look_pending_yaw-=step; look_pending_pitch-=step_pitch;
        frame_us=now; frame_yaw=step; frame_pitch=step_pitch;
    }
    look_native_us=now;
    if (step==0.0f && step_pitch==0.0f) return;
    uint64_t prev=0;
    if (!safe_read(&prev,saved_register(4),8) || prev<0x10000) return;
    float *yaw=(float *)(uintptr_t)(prev+0x20);
    float *pitch=yaw+1;
    *yaw=wrap_degrees(*yaw-step);
    *pitch+=step_pitch;
    if (mouse_debug()) printf("Mouse: lookturn t=%.4f step=%.3f pitch step=%.3f yaw=%.3f pitch=%.3f\n",now*1e-6,step,step_pitch,*yaw,*pitch);
}
/* U1 1.02: the camera of the cover (not aiming) is the one whose update (image+0xa4670) calls the stick handler at
 * image+0xa3f40 (rdi the camera, rsi the input). The handler turns the camera's horizontal direction (x y z w floats at
 * +0x240) by stick x * a speed that ramps up (+0x270) and lowers the pitch (float at +0x230, between -[shape+0xc] and
 * [shape+0x8], shape = *(camera+0x1300)) by stick y * speed (shape+0x30). It is the source of the joystick feel; the
 * mouse turns the two values directly just before, spread over a few frames like the aiming camera. */
static float cover_pending_yaw, cover_pending_pitch;
static void __attribute__((ms_abi)) u1b_cover_camera(uint64_t camera, uint64_t unused1, uint64_t image, uint64_t unused3) {
    (void)unused1; (void)unused3;
    if (aim_held || look_held || camera<0x10000) return;
    const uint64_t now=now_us();
    static uint64_t frame_us; static uint64_t frame_camera; static float frame_yaw, frame_pitch;
    float step, step_pitch;
    if (frame_us && now-frame_us<1500 && frame_camera==camera) {
        step=frame_yaw; step_pitch=frame_pitch;
    } else {
        if (cover_native_us && now-cover_native_us>100000) cover_pending_yaw=cover_pending_pitch=0.0f;
        float dx, dy;
        camera_take(3,&dx,&dy);
        if (camera_stopped(image,U1B_CAMERA_DT)) { cover_pending_yaw=cover_pending_pitch=0.0f; return; } /* paused */
        cover_pending_yaw+=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        cover_pending_pitch+=(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        float dt=*(const volatile float *)(uintptr_t)(image+U1B_CAMERA_DT);
        if (!(dt>0.0005f && dt<0.05f)) dt=1.0f/60.0f;
        const float follow=1.0f-SDL_expf(-dt/0.02f);
        step=cover_pending_yaw*follow; step_pitch=cover_pending_pitch*follow;
        cover_pending_yaw-=step; cover_pending_pitch-=step_pitch;
        frame_us=now; frame_camera=camera; frame_yaw=step; frame_pitch=step_pitch;
    }
    cover_native_us=now;
    float *dir=(float *)(uintptr_t)(camera+0x240);
    float *pitch=(float *)(uintptr_t)(camera+0x230);
    const uint64_t shape=*(const uint64_t *)(uintptr_t)(camera+0x1300);
    if (mouse_debug()) {
        static int shown;
        if (shown<5 && shape>0x10000) {
            ++shown;
            printf("Mouse: cover camera %#llx dir=(%.3f %.3f %.3f %.3f) pitch=%.3f limits=-%.3f..%.3f speed=%.3f\n",(unsigned long long)camera,
                   dir[0],dir[1],dir[2],dir[3],*pitch,*(const float *)(uintptr_t)(shape+0xc),*(const float *)(uintptr_t)(shape+8),
                   *(const float *)(uintptr_t)(shape+0x30));
        }
    }
    if (step==0.0f && step_pitch==0.0f) return;
    if (step!=0.0f) {
        const float a=step*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
    }
    if (step_pitch!=0.0f && shape>0x10000) {
        float v=*pitch-step_pitch; /* degrees (limits +-45, speed 90 degrees/s) */
        const float lo=-*(const float *)(uintptr_t)(shape+0xc), hi=*(const float *)(uintptr_t)(shape+8);
        *pitch=v<lo ? lo : v>hi ? hi : v;
    }
    if (mouse_debug()) printf("Mouse: coverturn t=%.4f step=%.3f pitch step=%.3f pitch=%.3f\n",now*1e-6,step,step_pitch,*pitch);
}
/* U1 1.02: the gun turret of the rail vehicle (chapter 7): the camera's update (image+0x2a7530) calls image+0x2a9690
 * (rdi the camera, rsi the angle state, rdx the input) which does state[1] = (state[1] * K1 - stick x * speed * dt) * K2
 * (yaw) and state[2] likewise (pitch, speed shape+0x1c, x speed shape+0x20, shape = *(camera + 0x1e8 + 8 * *(camera+0x204))),
 * K1 = float at image+0x7c9df0 and K2 = float at image+0x7c9df4 (degrees and radians), the sign flipped when the game's
 * invert flags (ints at image+0x95ca28 / +0x95ca2c) are set, then the limits shape+0x2c..0x30 (yaw) and 0x24..0x28 (pitch)
 * times K2. The mouse changes the two angles directly just before, the same way as the cover camera. */
static float turret_pending_yaw, turret_pending_pitch;
static void __attribute__((ms_abi)) u1b_turret_camera(uint64_t camera, uint64_t state, uint64_t image, uint64_t unused3) {
    (void)unused3;
    if (camera<0x10000 || state<0x10000) return;
    const uint64_t now=now_us();
    static uint64_t frame_us; static uint64_t frame_camera; static float frame_yaw, frame_pitch;
    float step, step_pitch;
    if (frame_us && now-frame_us<1500 && frame_camera==camera) {
        step=frame_yaw; step_pitch=frame_pitch;
    } else {
        if (cover_native_us && now-cover_native_us>100000) turret_pending_yaw=turret_pending_pitch=0.0f;
        float dx, dy;
        camera_take(3,&dx,&dy);
        if (camera_stopped(image,U1B_CAMERA_DT)) { turret_pending_yaw=turret_pending_pitch=0.0f; return; } /* paused */
        turret_pending_yaw+=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        turret_pending_pitch+=(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        float dt=*(const volatile float *)(uintptr_t)(image+U1B_CAMERA_DT);
        if (!(dt>0.0005f && dt<0.05f)) dt=1.0f/60.0f;
        const float follow=1.0f-SDL_expf(-dt/0.02f);
        step=turret_pending_yaw*follow; step_pitch=turret_pending_pitch*follow;
        turret_pending_yaw-=step; turret_pending_pitch-=step_pitch;
        frame_us=now; frame_camera=camera; frame_yaw=step; frame_pitch=step_pitch;
    }
    cover_native_us=now;
    const float k2=*(const float *)(uintptr_t)(image+0x7c9df4);
    const int flip_x=*(const int *)(uintptr_t)(image+0x95ca28)!=0, flip_y=*(const int *)(uintptr_t)(image+0x95ca2c)!=0;
    float *angles=(float *)(uintptr_t)state;
    const uint64_t shape=*(const uint64_t *)(uintptr_t)(camera+0x1e8+8u*(uint64_t)*(const uint32_t *)(uintptr_t)(camera+0x204));
    if (mouse_debug()) {
        static int shown;
        if (shown<5 && shape>0x10000) {
            ++shown;
            printf("Mouse: turret camera %#llx yaw=%.4f pitch=%.4f k1=%.5f k2=%.5f flips=%d,%d speeds=%.1f,%.1f yaw limits=%.1f..%.1f pitch limits=%.1f..%.1f\n",
                   (unsigned long long)camera,angles[1],angles[2],*(const float *)(uintptr_t)(image+0x7c9df0),k2,flip_x,flip_y,
                   *(const float *)(uintptr_t)(shape+0x20),*(const float *)(uintptr_t)(shape+0x1c),
                   *(const float *)(uintptr_t)(shape+0x2c),*(const float *)(uintptr_t)(shape+0x30),
                   *(const float *)(uintptr_t)(shape+0x24),*(const float *)(uintptr_t)(shape+0x28));
        }
    }
    if (step==0.0f && step_pitch==0.0f) return;
    if (!(k2>0.0f && k2<1.0f) || shape<0x10000) return;
    /* the stick's effect: angle = (angle - delta) * K2 with delta the stick's turn (the flag flips it); the mouse is the same stick */
    angles[1]-=step*k2*(flip_x ? -1.0f : 1.0f);
    angles[2]-=step_pitch*k2*(flip_y ? -1.0f : 1.0f);
    const float yaw_lo=*(const float *)(uintptr_t)(shape+0x2c)*k2, yaw_hi=*(const float *)(uintptr_t)(shape+0x30)*k2;
    const float pit_lo=*(const float *)(uintptr_t)(shape+0x24)*k2, pit_hi=*(const float *)(uintptr_t)(shape+0x28)*k2;
    angles[1]=angles[1]<yaw_lo ? yaw_lo : angles[1]>yaw_hi ? yaw_hi : angles[1];
    angles[2]=angles[2]<pit_lo ? pit_lo : angles[2]>pit_hi ? pit_hi : angles[2];
    if (mouse_debug()) printf("Mouse: turretturn t=%.4f step=%.3f pitch step=%.3f yaw=%.4f pitch=%.4f\n",now*1e-6,step,step_pitch,angles[1],angles[2]);
}
static const unsigned char u1b_turret_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xd7,0x49,0x89,0xf4,
    0x49,0x89,0xfe,0x45,0x8b,0xae,0x04,0x02,0x00,0x00,0x48,0x8d,0x0d,0xc3,0xc8,0x7f,0x00,0x48,0x8b,0x01,
};
static const unsigned char u1b_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x30,0x49,0x89,0xf6,0x48,0x89,
    0xfb,0x4c,0x8b,0x3d,0xf4,0x6b,0x8a,0x00,0x49,0x8b,0x07,0x48,0x89,0x45,0xd8,0x49,0x8b,0x7e,0x10,0xe8,
};
static const unsigned char u1b_look_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x18,0x01,0x00,0x00,
    0x48,0x89,0xd3,0x49,0x89,0xf4,0x49,0x89,0xfe,0x48,0x8b,0x05,0xfc,0x71,0x89,0x00,0x48,0x8b,0x00,0x48,
};
static void __attribute__((ms_abi)) u1_joypad_sticks(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1;
    if (!joypad_state || r14-0x62!=joypad_state) return; /* pad 0 only */
    const uint64_t now=now_us();
    sticks_hook_us=now;
    float dt=sticks_last_us ? (float)(now-sticks_last_us)*1e-6f : 1.0f/60.0f;
    sticks_last_us=now;
    if (dt<1.0f/480.0f) dt=1.0f/480.0f;
    if (dt>0.05f) dt=0.05f;
    /* the game turns by rate * its own camera time step: use it, the wall clock jitters */
    if (u1_camera_dt_offset) {
        const float game_dt=*(const volatile float *)(uintptr_t)(image+u1_camera_dt_offset);
        if (game_dt>0.0005f && game_dt<0.05f) dt=game_dt;
    }
    static float follow_test=-1.0f;   /* BB_MOUSE_FOLLOW_TEST=<x>: right stick x everywhere (measuring) */
    if (follow_test<0.0f) { const char *t=getenv("BB_MOUSE_FOLLOW_TEST"); follow_test=t ? (float)atof(t) : 0.0f; }
    if (follow_test>0.0f) { pad[2]+=follow_test; pad[6]+=follow_test; return; }
    float dx, dy;
    camera_take(1,&dx,&dy);
    if (camera_stopped(image,u1_camera_dt_offset)) return; /* paused */
    if (aim_test<0.0f) { const char *t=getenv("BB_MOUSE_AIM_TEST"); aim_test=t ? (float)atof(t) : 0.0f; }
    float *pad=(float *)(uintptr_t)joypad_state;
    if (!aim_held && !look_held) { float ix, iy; camera_take(2,&ix,&iy); }
    const int cover_native=cover_native_us && now-cover_native_us<100000;
    if (!cover_native) { float ix, iy; camera_take(3,&ix,&iy); } /* the cover camera's queue fills outside of the cover too */ /* the aiming camera's queue fills in free look too */
    if (mouse_debug() && getenv("BB_BUTTON_WATCH")) { /* TEMPORARY: who reads the persistent pad's button words (pressed +0x94, held +0x9c) */
        static uint64_t t0, last_summary;
        if (!t0) t0=now;
        const uint64_t pad_struct=joypad_state-0x608;
        if (getenv("BB_WATCH_LSTICK")) stickwatch_arm(pad_struct+0x0,pad_struct+0x48,image); /* left stick x: raw copy and the second copy */
        else stickwatch_arm(pad_struct+0x94,pad_struct+0x9c,image);
        stickwatch_enable(now-t0>3000000);
        if (now-t0>3000000 && now-last_summary>5000000) { last_summary=now; stickwatch_summary(); }
    }
    float x=0.0f, y=0.0f;
    static int cal=-1;                /* BB_MOUSE_CAL=<pixels/s per stick unit>: the mouse is a raw stick value (measuring) */
    static float cal_scale;
    if (cal<0) { const char *t=getenv("BB_MOUSE_CAL"); cal_scale=t ? (float)atof(t) : 0.0f; cal=cal_scale>0.0f; }
    if (cal) { x=dx/dt/cal_scale; y=dy/dt/cal_scale; }
    else if (aim_test>0.0f) x=aim_test;
    else {
        /* the turn rate (degrees/s) the mouse asks for, through the inverse of the aiming curve
         * measured in U1: 45 x^2 degrees/s up to x = 1, then about 54 x - 9 (x = 3: 152, x = 6: 324) */
        const int following=camera_hook_us && now-camera_hook_us<50000;
        /* aiming, the look around and any camera other than the follow camera (cover) turn the same way */
        const int aim_like=aim_held || look_held || !following;
        const float scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*(aim_like ? input.mouse_aim_sensitivity : 1.0f)/dt;
        float push=0.0f;
        if (!aim_like && following && follow_direct) { /* the follow camera turned itself */
            dx=dy=0.0f;
            push=follow_push;
        }
        float (*curve)(float)=aim_like ? aim_stick : follow_stick;
        /* following through the joypad: the shortfall is made up within about a quarter second */
        const float extra=!aim_like && following && !follow_direct ? follow_correction*4.0f : 0.0f;
        float rate_x=dx*scale+extra, rate_y=(input.mouse_invert_y ? -dy : dy)*scale;
        if (aim_like) {
            /* the mouse reports a pixel or two now and then: at low speeds that is one-frame pulses and
             * the aim moved in steps. The turn rate is smoothed over about 30 ms (the angle turned stays
             * the same). */
            const float alpha=input.aim_raw ? 1.0f : 1.0f-SDL_expf(-dt/0.03f);
            aim_rate[0]+=alpha*(rate_x-aim_rate[0]);
            aim_rate[1]+=alpha*(rate_y-aim_rate[1]);
            if (aim_rate[0]>-0.3f && aim_rate[0]<0.3f && rate_x==0.0f) aim_rate[0]=0.0f;
            if (aim_rate[1]>-0.3f && aim_rate[1]<0.3f && rate_y==0.0f) aim_rate[1]=0.0f;
            rate_x=aim_rate[0]; rate_y=aim_rate[1];
        } else {
            aim_rate[0]=aim_rate[1]=0.0f;
        }
        /* the zoom camera is not updated for a few frames when the shoulder is switched: keep the stick out of it */
        const int native_yaw=(aim_held && aim_native_us && now-aim_native_us<600000) ||
                             (look_held && look_native_us && now-look_native_us<100000) ||
                             cover_native;
        x=native_yaw ? 0.0f : curve(rate_x)+push;
        y=native_yaw ? 0.0f : curve(rate_y);
    }
    if (mouse_debug()) printf("Mouse: aim t=%.4f dt=%.4f dx=%.1f x=%.3f game=%.3f aim=%d look=%d following=%d direct=%d\n",now*1e-6,dt,dx,x,pad[2],aim_held,look_held,camera_hook_us && now-camera_hook_us<50000,follow_direct);
    if (x!=0.0f) { pad[2]+=x; pad[6]+=x; }
    if (y!=0.0f) { pad[3]+=y; pad[7]+=y; }
}
static const unsigned char u1_pad_update_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,
    0x49,0x89,0xfe,0x4c,0x8b,0x2d,0x92,0xf6,0x86,0x00,0x49,0x8b,
};
static const unsigned char u1_joypad_sticks_code[]={
    0x80,0x7c,0x01,0x6d,0x00,0x74,0x3b,0xc5,0xe2,0x59,0x64,0x01,0x40,0xc4,0xc1,0x7a,0x11,0x66,0xbe,0xc5,
    0xe2,0x59,0x64,0x01,
};
/* U1 of CUSA02320 / CUSA02344 (1.02): the same three functions, moved (+0xf0, +0x150, +0x150) with other
 * rip-relative operands; the code and the structures they use are the same. */
static const unsigned char u1b_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x00,0xff,0xff,0xff,0x49,0x89,0xcd,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfc,
    0x4c,0x8b,0x35,0x42,0xbd,0x89,0x00,0x49,0x8b,
};
static const unsigned char u1b_pad_update_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,
    0x49,0x89,0xfe,0x4c,0x8b,0x2d,0xe2,0x37,0x83,0x00,0x49,0x8b,
};
/* image+0xb62a7 in the camera manager, right after the call of the camera's update (r14 = the camera):
 * vmovaps xmm0, [rsp+0x120]; vmovaps [rsp+0x160], xmm0 */
static const unsigned char u1b_aim_camera_code[]={
    0xc5,0xf8,0x28,0x84,0x24,0x20,0x01,0x00,0x00,0xc5,0xf8,0x29,0x84,0x24,0x60,0x01,0x00,0x00,
};
/* U2 UpdateFocus (player-base.cpp:1101): the camera's look-at spring went NaN once at 240 Hz and
 * the game's assertion ended it. The assertion's call now restarts the spring at the player's
 * target ([rbp-0x60]): position (+0x2b0, y +0x2a4) and velocities (+0x2c0, +0x2a8); a NaN target
 * still stops at the trap. */
#define U2_FOCUS_RESET \
    0xc5,0xf8,0x28,0x45,0xa0,                            /* vmovaps xmm0, [rbp-0x60] (the target) */ \
    0x0f,0x2e,0xc0,0x7a,0x2c,                            /* ucomiss xmm0, xmm0; jp trap */ \
    0xc5,0xf8,0x29,0x83,0xb0,0x02,0x00,0x00,             /* vmovaps [rbx+0x2b0], xmm0 */ \
    0xc4,0xe3,0x79,0x17,0x83,0xa4,0x02,0x00,0x00,0x01,   /* vextractps [rbx+0x2a4], xmm0, 1 */ \
    0xc5,0xf8,0x29,0x45,0xd0,                            /* vmovaps [rbp-0x30], xmm0 */ \
    0xc5,0xf0,0x57,0xc9,                                 /* vxorps xmm1, xmm1, xmm1 */ \
    0xc5,0xf8,0x29,0x8b,0xc0,0x02,0x00,0x00,             /* vmovaps [rbx+0x2c0], xmm1 */ \
    0x83,0xa3,0xa8,0x02,0x00,0x00,0x00,                  /* and dword [rbx+0x2a8], 0 */ \
    0xeb,0x02,                                           /* jmp (store the focus) */ \
    0xcd,0x41                                            /* trap: int 0x41 */
static const unsigned char u2_focus_reset_100[]={U2_FOCUS_RESET};              /* replaces 56 bytes */
static const unsigned char u2_focus_reset_102[]={0x66,0x90,U2_FOCUS_RESET};    /* 58 bytes: a nop first */
_Static_assert(sizeof(u2_focus_reset_100)==56 && sizeof(u2_focus_reset_102)==58,"patch sizes");
/* U2 game/character-leg-ik.cpp:391, assertion IsFinite(deltaTransform): the root delta the leg IK
 * gets from its caller can be NaN for a frame (train wreck, chapter 1: issues #88, #253) and the
 * game stopped. The leg IK only adds its y to two accumulators, so a non-finite delta now counts as
 * no movement: it is zeroed (also in the caller's vector at [r15]) and the update goes on. Build 100
 * (US and EU 1.00) calls the assertion handler (5 bytes), build 102 loads it (7 bytes). */
#define U2_LEG_IK_ZERO(jump)     0xc5,0xf8,0x57,0xc0,                          /* vxorps xmm0, xmm0, xmm0 */     0xc4,0xc1,0x78,0x29,0x07,                     /* vmovaps [r15], xmm0 */     0xeb,jump                                     /* jmp (the update) */
#define CC8 0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc
static const unsigned char u2_leg_ik_zero_100[]={U2_LEG_IK_ZERO(0x2d),CC8,CC8,CC8,CC8,CC8};
static const unsigned char u2_leg_ik_zero_102[]={U2_LEG_IK_ZERO(0x2f),CC8,CC8,CC8,CC8,CC8,0xcc,0xcc};
/* U1 and U2 "pAction != NULL" (moveactioncontroller.cpp:40), from Mevlock's dev fork: the
 * player's move actions come from a pool of 64 slots. Two move states swapping every frame
 * (landing into a fight, often right after a jump) keep one action each swap; at 144 FPS bursts
 * took 16-30 slots, and a longer one filled the pool and stopped the game. The slot count passed
 * where the character is set up becomes 256 (+144 KB from the character's heap). One site in U1
 * and one in U2, in every build; U3 has no such pool. */
static const unsigned char move_pool_256[]={0xb9,0x00,0x01,0x00,0x00}; /* mov ecx, 256 */
_Static_assert(sizeof(u2_leg_ik_zero_100)==51 && sizeof(u2_leg_ik_zero_102)==53,"patch sizes");
/* U3 Player::UpdateFocus (player.cpp:1897, assertion m_lookAt.X() == m_lookAt.X(): the crash
 * climbing the fort in chapter 8). The point Nate looks at ([player+0xef0], velocity +0xf00)
 * springs towards a target; once NaN it stays NaN, and the function's own reset (taken when the
 * point is far off) never runs for it: NaN compares false. The assertion's path now takes that
 * reset when the target is a number (the velocity register zeroed first, as on the reset's usual
 * way in); a NaN target still stops the game. BilalReloaded's fix for 1.02 (his Uncharted port,
 * GPL v2, GitHub #13 there), and the same for 1.00 (reset at 0x390779, target at rbp-0xa0, zero in
 * xmm4). */
static const unsigned char u3_focus_reset_100[]={
    0xc5,0xd8,0x57,0xe4,                          /* vxorps xmm4, xmm4, xmm4 */
    0xc5,0xf8,0x28,0x85,0x60,0xff,0xff,0xff,      /* vmovaps xmm0, [rbp-0xa0] (the target) */
    0xc5,0xf8,0xc2,0xc8,0x00,                     /* vcmpeqps xmm1, xmm0, xmm0 */
    0xc5,0xf8,0x50,0xc1,                          /* vmovmskps eax, xmm1 */
    0x83,0xf8,0x0f,                               /* cmp eax, 15 */
    0x0f,0x84,0xa3,0xfe,0xff,0xff,                /* je the reset (0x390779) */
    0xcd,0x41,                                    /* the target is NaN too: trap as before */
    CC8,CC8,0xcc,0xcc,0xcc,
};
static const unsigned char u3_focus_reset_102[]={
    0xc5,0xd0,0x57,0xed,0xc5,0xf8,0x28,0x45,0x80,0xc5,0xf8,0xc2,0xc8,0x00,0xc5,0xf8,0x50,0xc1,
    0x83,0xf8,0x0f,0x0f,0x84,0xa5,0xfe,0xff,0xff,0xcd,0x41,CC8,CC8,CC8,
};
_Static_assert(sizeof(u2_leg_ik_zero)==sizeof(u2_leg_ik_assert_code),"same size");
/* U1 1.02: the options menu "Game" (difficulty, subtitles, hints, tutorial tips, friend stats, photo mode) is
 * built in three places, each row a pair of objects (text id at +8) given to the menu by the function at
 * 0x132f00 (rdi the menu, rsi the row). The photo mode row (text id 0xa3da9f13, "Photo Mode Enable
 * |D-PAD-DOWN|") is created like the others, but its last call is turned into a 5-byte nop: the row
 * never reaches the menu. The mode itself needs the d-pad down in play; sample_host drops that button
 * while the game runs (menu_mode off), so the option left on in an old save does nothing. */
static const unsigned char u1_photo_row_1_code[]={0x4c,0x89,0xf7,0x4c,0x89,0xee,0xe8,0xba,0xe0,0xfe,0xff};
static const unsigned char u1_photo_row_1_patch[]={0x4c,0x89,0xf7,0x4c,0x89,0xee,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_row_2_code[]={0x48,0x8b,0xbd,0x10,0xff,0xff,0xff,0x48,0x89,0xde,0xe8,0x1d,0xde,0xfe,0xff};
static const unsigned char u1_photo_row_2_patch[]={0x48,0x8b,0xbd,0x10,0xff,0xff,0xff,0x48,0x89,0xde,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_row_3_code[]={0x48,0x8b,0xbd,0x08,0xff,0xff,0xff,0x48,0x89,0xde,0xe8,0x06,0xdc,0xfe,0xff};
static const unsigned char u1_photo_row_3_patch[]={0x48,0x8b,0xbd,0x08,0xff,0xff,0xff,0x48,0x89,0xde,0x0f,0x1f,0x44,0x00,0x00};
/* Where the menu texts are drawn (U1 1.02): the text drawing routine at 0x13acf0 takes the style (rdx), the
 * laid-out text (rcx: width at +0x14, height at +0x18), the rectangle to put it in (r8: x y w h floats), the
 * horizontal (r9d) and vertical (7th argument) alignment (0 start, 2 end, else centre) and puts the text at the
 * rectangle's corner / end / middle plus the style's offset (+0x20, +0x24). The menu draws each item by
 * calling its virtual draw (0x133fb4, 0x133fe3): hooks before and after those calls say which item the texts
 * drawn in between belong to. The registers the thunk saved (probe.c) are read off the guest stack. */
#ifdef _WIN32
#include <windows.h>
#endif
extern volatile uint64_t guest_callback_rsp;
static int safe_read(void *dst, uint64_t src, size_t size) {
    if (src<0x10000 || src>0x00007fffffffffffull) return 0;
#ifdef _WIN32
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(uintptr_t)src,dst,size,&got) && got==size;
#else
    memcpy(dst,(const void *)(uintptr_t)src,size); return 1;
#endif
}
static int safe_write(uint64_t dst, const void *src, size_t size) {
    if (dst<0x10000 || dst>0x00007fffffffffffull) return 0;
#ifdef _WIN32
    SIZE_T put=0;
    return WriteProcessMemory(GetCurrentProcess(),(LPVOID)(uintptr_t)dst,src,size,&put) && put==size;
#else
    memcpy((void *)(uintptr_t)dst,src,size); return 1;
#endif
}
static uint64_t saved_register(int n) { /* the n-th register the thunk pushed: 2 rax, 3 rcx, 4 rdx, 5 r8, 6 r9 */
    uint64_t v=0;
    return safe_read(&v,guest_callback_rsp-8u*(uint64_t)n,8) ? v : 0;
}
typedef struct { uint64_t item; float x, y, w, h, rx, ry, rw, rh; uint64_t us; int ordinal; float style[8]; } MenuText;
static int menu_text_ordinal;             /* the n-th text of the item being drawn */
#define MENU_TEXTS 256
static MenuText menu_texts[MENU_TEXTS];
static int menu_text_count;
static uint64_t menu_text_item;           /* the item whose draw is running, 0 outside */
static pthread_mutex_t menu_text_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t menu_text_frame_us;       /* the clock of the last text of the first item: a new frame starts */
static void __attribute__((ms_abi)) u1_menu_item_begin(uint64_t item, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    menu_text_item=item;
    menu_text_ordinal=0;
}
static void __attribute__((ms_abi)) u1_menu_item_end(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image; (void)r14;
    menu_text_item=0;
}
static int menu_mode, menu_guard;   /* defined with the menu keys below */
/* Texts drawn in a menu outside of any menu item (the scrolling lists, the carousels, titles, footers, arrows): the
 * rectangle of each, kept by position, to find out what such a screen is made of (F9) and, for the lists, which line is
 * highlighted (its style differs). */
#define FREE_TEXTS 160
static MenuText free_texts[FREE_TEXTS];
static int free_text_count;
/* The credits (U1 1.02): the credits task updates and draws itself in image+0x3b41e0 (strings "CREDITS:: ..." and "credits-roll"), called every
 * frame while they are on screen; they draw no menu text the port sees, so the menu keys and the mouse would otherwise be off there. */
static volatile uint64_t u1_credits_us;
static int u1_credits_recent(uint64_t now) { return u1_credits_us && now-u1_credits_us<300000 && u1_credits_us<=now+1000000; }
static void __attribute__((ms_abi)) u1_credits_tick(uint64_t task, uint64_t unused1, uint64_t image, uint64_t r14) {   /* task = rdi: its first word is the state, 7..11 while the credits roll */
    (void)unused1; (void)image; (void)r14;
    uint32_t state=0;
    static uint32_t last_state=0xffffffffu;
    if (!safe_read(&state,task,4)) return;
    if (mouse_debug() && state!=last_state) { last_state=state; printf("Runtime: U1 credits task state %u\n",state); }
    if (state-7u<5u) u1_credits_us=now_us();
}
static const unsigned char u1_credits_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,0x01,0x00,0x00};
static void __attribute__((ms_abi)) u1_text_draw(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image; (void)r14;
    const uint64_t item=menu_text_item;
    if (!item && !menu_mode) return;
    const uint64_t style=saved_register(4), layout=saved_register(3), rect=saved_register(5);
    const uint32_t halign=(uint32_t)saved_register(6);
    uint32_t valign=0;
    float r[4], size[2], offset[2];
    uint64_t stack_arg=0;
    if (!safe_read(&stack_arg,guest_callback_rsp+8,8)) return;
    valign=(uint32_t)stack_arg;
    if (!safe_read(r,rect,16) || !safe_read(&size[0],layout+0x14,4) || !safe_read(&size[1],layout+0x18,4) ||
        !safe_read(&offset[0],style+0x20,4) || !safe_read(&offset[1],style+0x24,4)) return;
    const float x=halign==2 ? r[0]+r[2]-size[0] : halign==0 ? r[0] : r[0]+(r[2]-size[0])*0.5f;
    const float y=valign==2 ? r[1]+r[3]-size[1] : valign==0 ? r[1] : r[1]+(r[3]-size[1])*0.5f;
    pthread_mutex_lock(&menu_text_lock);
    const uint64_t now=now_us();
    if (!item) {
        MenuText free_text={0,x+offset[0],y+offset[1],size[0],size[1],r[0],r[1],r[2],r[3],now,0,{0}};
        if (!safe_read(free_text.style,style,sizeof(free_text.style))) memset(free_text.style,0,sizeof(free_text.style));
        int fslot=-1;
        for (int i=0;i<free_text_count;++i) {
            const MenuText *o=&free_texts[i];
            if (fabsf(o->rx-r[0])<0.5f && fabsf(o->ry-r[1])<0.5f && fabsf(o->rw-r[2])<0.5f && fabsf(o->rh-r[3])<0.5f &&
                fabsf(o->w-free_text.w)<3.0f) { fslot=i; break; }
        }
        if (fslot<0) for (int i=0;i<free_text_count;++i) if (now-free_texts[i].us>300000) { fslot=i; break; }
        if (fslot<0 && free_text_count<FREE_TEXTS) fslot=free_text_count++;
        if (fslot>=0) free_texts[fslot]=free_text;
        pthread_mutex_unlock(&menu_text_lock);
        return;
    }
    menu_text_frame_us=now;
    const int ordinal=menu_text_ordinal++;
    MenuText text={item,x+offset[0],y+offset[1],size[0],size[1],r[0],r[1],r[2],r[3],now,ordinal,{0}};
    if (!safe_read(text.style,style,sizeof(text.style))) memset(text.style,0,sizeof(text.style));
    int slot=-1;
    for (int i=0;i<menu_text_count;++i)  /* the same text of the same item: the newest draw replaces the old one */
        if (menu_texts[i].item==item && menu_texts[i].ordinal==ordinal) { slot=i; break; }
    if (slot<0) { /* an old entry (not drawn for 200 ms: a closed menu) makes room first */
        for (int i=0;i<menu_text_count;++i) if (now-menu_texts[i].us>200000) { slot=i; break; }
    }
    if (slot<0 && menu_text_count<MENU_TEXTS) slot=menu_text_count++;
    if (slot>=0) menu_texts[slot]=text;
    pthread_mutex_unlock(&menu_text_lock);
}
static const unsigned char u1_text_draw_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58,0x44,0x89,0xcb};
static const unsigned char u1_menu_item_begin_code[]={0x48,0x8b,0x07,0x4c,0x89,0xf6,0x4c,0x89,0xfa};
static const unsigned char u1_menu_item_end1_code[]={0x41,0x8b,0x8d,0x98,0x00,0x00,0x00};
static const unsigned char u1_menu_item_end2_code[]={0x49,0x8b,0x04,0x24,0x48,0x3b,0x45,0xd0};
/* U1 1.02: the prompt bar draws its button prompts by kind; kind 4 is "COMPARER LES AMIS" (text id 0x9a8d0b3e, drawn at
 * 0x1291a3..0x129267), the compare-with-friends action of the statistics screens, which does not work here. A hook at 0x128f64
 * (the kind 4 test) notes the moment the prompt is drawn: while it is, Enter, the click and the cross do nothing (the screen is
 * a statistics one), and two patches make the prompt's text and its jump skip the block. */
static volatile uint64_t u1_compare_us;
static void __attribute__((ms_abi)) u1_compare_prompt(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image; (void)r14;
    u1_compare_us=now_us();
}
static const unsigned char u1_compare_code[]={0x41,0x83,0x7d,0x48,0x00,0x0f,0x84};
static const unsigned char u1_compare_je_code[]={0x0f,0x84,0x34,0x02,0x00,0x00};
static const unsigned char u1_compare_je_patch[]={0x0f,0x84,0xf8,0x02,0x00,0x00};   /* to 0x129267, past the prompt */
static const unsigned char u1_compare_jmp_code[]={0xe9,0x2f,0x02,0x00,0x00};
static const unsigned char u1_compare_jmp_patch[]={0xe9,0xec,0x02,0x00,0x00};
/* U1 1.02: a new row in the "Commandes" menu, "Touches clavier / souris" (opens the port's own bindings window). The menu builder
 * makes its last two rows (the layout screen, then "Rétablir paramètres par défaut") with the same few steps: allocate the view
 * (0x108 bytes) and a descriptor (0x18 bytes: owner, text id, 0), run the action-row constructor 0x12a020 (view, context, action
 * code, descriptor, 0), add it to the menu with 0x132f00. A hook before the "Rétablir" row (0x145881) does the same for a row of
 * ours, using the game's own functions, so the row sits between the two. Its text is the unused text 0x1b124bb7 ("Joysticks
 * gauche/droit", "Left/Right Sticks"), reworded in memory by keys_label_thread. The row's action code is one nothing handles; the
 * cross, the click and the left/right keys are taken while it is selected (sample_host). */
typedef uint64_t (__attribute__((sysv_abi)) *guest_fn1)(uint64_t);
typedef uint64_t (__attribute__((sysv_abi)) *guest_fn2)(uint64_t,uint64_t);
typedef uint64_t (__attribute__((sysv_abi)) *guest_fn5)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static volatile uint64_t u1_keys_row_view, u1_keys_row_menu;
/* The label of that row, in the language the game has loaded now: the game's own text lookup (0x5caac0: text table 0x9687d8, id) gives
 * where the text of id 0x1b124bb7 lives; it is rewritten in place (same length) each time the Commandes menu is built, so a language
 * change in the game (which loads a fresh text bank) keeps it. French, Spanish and English have their own wording, the others English. */
typedef const char *(__attribute__((sysv_abi)) *guest_text_fn)(uint64_t, uint64_t);
static void keys_label_here(uint64_t image) {
    const char *s=((guest_text_fn)(uintptr_t)(image+0x5caac0))(image+0x9687d8,0x1b124bb7u);
    uint64_t standard=0;
    char old[96]={0};
    if (!s || !safe_read(&standard,image+0x9687d8+0x10,8) || (uint64_t)(uintptr_t)s==standard) return;
    size_t got=sizeof(old)-1;
    while (got>0 && !safe_read(old,(uint64_t)(uintptr_t)s,got)) --got;
    old[got]=0;
    const size_t len=strlen(old);
    if (len<4 || len>=sizeof(old)-1) return;
    {   /* the game's language, for the bindings window (0 English, 1 French, 2 Spanish, 3 Portuguese; "Teclado/" is either of the last two) */
        static int language=-1;
        int found=-1;
        if (strstr(old,"gauche") || !strncmp(old,"Touches clavier",15)) found=1;
        else if (strstr(old,"izquierdo")) found=2;
        else if (strstr(old,"direito")) found=3;
        else if (!strncmp(old,"Teclado/",8)) found=(language==2 || language==3) ? language : 2;
        else found=0;
        if (found!=language) { language=found; bbgpu_set_language(language); printf("Runtime: game language for the bindings window: %d\n",language); }
    }
    const char *candidates[4];
    int n=0;
    if (strstr(old,"gauche") || !strncmp(old,"Touches clavier",15)) candidates[n++]="Touches clavier/souris";
    else if (strstr(old,"izquierdo") || strstr(old,"direito") || !strncmp(old,"Teclado/",8)) candidates[n++]="Teclado/Mouse";   /* Latin American Spanish and Brazilian Portuguese */
    candidates[n++]="Keyboard/Mouse"; candidates[n++]="Keys/Mouse"; candidates[n++]="Keys";
    const char *label=candidates[n-1];
    for (int i=0;i<n;++i) if (strlen(candidates[i])<=len) { label=candidates[i]; break; }
    char padded[96];
    memset(padded,' ',len);
    memcpy(padded,label,strlen(label)<len ? strlen(label) : len);
    if (memcmp(padded,old,len) && safe_write((uint64_t)(uintptr_t)s,padded,len)) printf("Runtime: keys row label set in place (was \"%s\")\n",old);
}
static void __attribute__((ms_abi)) u1_add_keys_row(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t menu) {
    (void)unused0; (void)unused1;
    if (pad_only_mode) return;   /* a controller is in use: the menu stays as the game builds it */
    keys_label_here(image);
    int32_t count=0; uint64_t last=0, desc=0, owner=0;
    const uint64_t context=saved_register(9);                    /* rbx: the menu's context, as the other rows are given */
    if (!menu || !context || !safe_read(&count,menu+0x98,4) || count<1 || count>64 ||
        !safe_read(&last,menu+0xa0+8u*(uint64_t)(count-1),8) || !safe_read(&desc,last+8,8) || !safe_read(&owner,desc,8) || !owner) {
        printf("Runtime: keys row: menu not understood, row not added\n");
        return;
    }
    const guest_fn1 alloc=(guest_fn1)(uintptr_t)(image+0x4542b0);
    const guest_fn5 make_action_row=(guest_fn5)(uintptr_t)(image+0x12a020);
    const guest_fn2 add_item=(guest_fn2)(uintptr_t)(image+0x132f00);
    const uint64_t view=alloc(0x108), descriptor=alloc(0x18);
    if (!view || !descriptor) return;
    uint64_t *d=(uint64_t *)(uintptr_t)descriptor;
    d[0]=owner; d[1]=0x1b124bb7u; *(uint32_t *)(uintptr_t)(descriptor+0x10)=0;
    make_action_row(view,context,0x4f0,descriptor,0);
    add_item(menu,view);
    u1_keys_row_view=view; u1_keys_row_menu=menu;
    printf("Runtime: keys row added to menu %#llx (view %#llx)\n",(unsigned long long)menu,(unsigned long long)view);
}
static const unsigned char u1_add_keys_row_code[]={0xbf,0x08,0x01,0x00,0x00,0xe8};
static volatile uint64_t u1_menu_last;   /* the menu object last given an item (see menu objects below) */
static void __attribute__((ms_abi)) u1_menu_add_item(uint64_t menu, uint64_t item, uint64_t image, uint64_t r14) {
    (void)item; (void)image; (void)r14;
    u1_menu_last=menu;
}
static const unsigned char u1_menu_add_item_code[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x49,0x89,0xf6,0x48,0x89,0xfb};
/* U1 1.02: the menu's event handler (0x1336c0: rdi the menu, rsi the event {context ptr, type (1 up, 2 down), float}). Up and down
 * reach a scrolling list through it, one row per event. To take the highlight of a list to the row under the cursor in one go, the
 * pad thread arms a number of extra steps and presses the direction once; this hook, on the game's own thread and with the real
 * event, then replays the handler that many more times with a copy of the event (the context pointer is only good during the
 * call, so it cannot be kept for later). */
static volatile uint64_t u1_jump_menu, u1_jump_us, u1_event_last_menu;
static volatile int u1_jump_steps, u1_jump_type, u1_jump_busy, u1_event_last_type, u1_event_count;
static volatile uint32_t u1_jump_done, u1_measure_done;
static volatile uint64_t u1_measure_item;
static volatile int u1_measure_n;
static int list_top_read(uint64_t item);
static void list_top_write(uint64_t item, int top);
static int list_index_raw(uint64_t item) {   /* the highlighted row of a list: the u32 at model+0x14, model = *(item+8) */
    uint64_t model=0; uint32_t idx=0;
    if (!safe_read(&model,item+8,8) || !safe_read(&idx,model+0x14,4) || idx>4096) return -1;
    return (int)idx;
}
static void __attribute__((ms_abi)) u1_menu_event(uint64_t menu, uint64_t ev, uint64_t image, uint64_t r14) {
    (void)r14;
    u1_event_last_menu=menu;
    { uint32_t ty=0; if (safe_read(&ty,ev+8,4) && ty) { u1_event_last_type=(int)ty; ++u1_event_count; } }
    if (u1_jump_busy || (u1_jump_steps<=0 && !u1_measure_item) || menu!=u1_jump_menu) return;
    if (now_us()-u1_jump_us>250000) { u1_jump_steps=0; u1_measure_item=0; return; }
    uint64_t copy[4];
    if (!safe_read(copy,ev,sizeof(copy))) return;
    if ((int32_t)(copy[1]&0xffffffffu)!=u1_jump_type) {
        if ((copy[1]&0xffffffffu) && mouse_debug()) printf("Runtime: list jump: event type %u ignored (wanted %d), value %08x, ctx %#llx\n",(unsigned)(copy[1]&0xffffffffu),u1_jump_type,(unsigned)(copy[1]>>32),(unsigned long long)copy[0]);
        return;
    }
    const guest_fn2 handler=(guest_fn2)(uintptr_t)(image+0x1336c0);
    if (u1_measure_item) {
        /* The length of a list, found without the player seeing the highlight run: all inside this one call, press down until the
         * highlighted row stops growing or wraps to the top, then go back to just before where it was (the real press that is
         * still to come moves it one down) and put the view back. */
        const uint64_t item=u1_measure_item;
        u1_measure_item=0;
        u1_jump_busy=1;
        const int idx0=list_index_raw(item), top0=list_top_read(item);
        uint64_t down[4]={copy[0],copy[1],copy[2],copy[3]}, up[4]={copy[0],copy[1],copy[2],copy[3]};
        down[1]=(copy[1]&~0xffffffffull)|2u; up[1]=(copy[1]&~0xffffffffull)|1u;
        int prev=idx0, cur=idx0, wrapped=0, total=0;
        for (int i=0;i<400 && idx0>=0;++i) {
            handler(menu,(uint64_t)(uintptr_t)down);
            cur=list_index_raw(item);
            if (cur<0) break;
            if (cur<=prev) { total=prev+1; wrapped=cur<prev; break; }
            prev=cur;
        }
        if (total>0) {
            const int back=idx0>=1 ? idx0-1 : idx0;
            if (wrapped) for (int i=0;i<back;++i) handler(menu,(uint64_t)(uintptr_t)down);
            else for (int i=0;i<cur-back;++i) handler(menu,(uint64_t)(uintptr_t)up);
        }
        if (top0>=0) list_top_write(item,top0);
        u1_jump_busy=0;
        u1_measure_n=total;
        ++u1_measure_done;
        return;
    }
    const int n=u1_jump_steps;
    u1_jump_steps=0;
    u1_jump_busy=1;
    for (int i=0;i<n;++i) handler(menu,(uint64_t)(uintptr_t)copy);
    u1_jump_busy=0;
    ++u1_jump_done;
}
static const unsigned char u1_menu_event_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
static const RuntimeGuestCallback guest_callbacks[]={
    {"U1 menu event (list jump)",0x1336c0,u1_menu_event_code,sizeof(u1_menu_event_code),6,u1_menu_event,NULL},
    {"U1 follow camera (mouse)",0xaecf0,u1_follow_camera_code,sizeof(u1_follow_camera_code),6,u1_follow_camera,NULL},
    {"U1 pad update (mouse)",0x117200,u1_pad_update_code,sizeof(u1_pad_update_code),6,u1_pad_update,NULL},
    {"U1 joypad sticks (mouse)",0x11784e,u1_joypad_sticks_code,sizeof(u1_joypad_sticks_code),5,u1_joypad_sticks,NULL},
    {"U1 follow camera (mouse, 1.02)",0xaede0,u1b_follow_camera_code,sizeof(u1b_follow_camera_code),6,u1b_follow_camera,NULL},
    {"U1 pad update (mouse, 1.02)",0x117350,u1b_pad_update_code,sizeof(u1b_pad_update_code),6,u1_pad_update,NULL},
    {"U1 joypad sticks (mouse, 1.02)",0x11799e,u1_joypad_sticks_code,sizeof(u1_joypad_sticks_code),5,u1_joypad_sticks,NULL},
    {"U1 aiming camera (mouse, 1.02)",0xb62a7,u1b_aim_camera_code,sizeof(u1b_aim_camera_code),9,u1b_aim_camera,NULL},
    {"U1 zoom camera entry (mouse, 1.02)",0xc43b0,u1b_zoom_entry_code,sizeof(u1b_zoom_entry_code),6,u1b_zoom_entry,NULL},
    {"U1 look-around camera (mouse, 1.02)",0xb3930,u1b_look_camera_code,sizeof(u1b_look_camera_code),6,u1b_look_camera,NULL},
    {"U1 cover camera (mouse, 1.02)",0xa3f40,u1b_cover_camera_code,sizeof(u1b_cover_camera_code),6,u1b_cover_camera,NULL},
    {"U1 turret camera (mouse, 1.02)",0x2a9690,u1b_turret_camera_code,sizeof(u1b_turret_camera_code),6,u1b_turret_camera,NULL},
    {"U2 follow camera (mouse)",0x171fe0,u2_follow_camera_code,sizeof(u2_follow_camera_code),6,u2_follow_camera,NULL},
    {"U3 follow camera (mouse)",0x121b90,u3_follow_camera_code,sizeof(u3_follow_camera_code),6,u3_follow_camera,NULL},
    {"U1 controls menu: keyboard row",0x145881,u1_add_keys_row_code,sizeof(u1_add_keys_row_code),5,u1_add_keys_row,NULL},
    {"U1 compare-with-friends prompt (hook)",0x128f64,u1_compare_code,sizeof(u1_compare_code),5,u1_compare_prompt,NULL},
    {"U1 compare-with-friends prompt (skip 1)",0x128f69,u1_compare_je_code,sizeof(u1_compare_je_code),sizeof(u1_compare_je_patch),NULL,u1_compare_je_patch},
    {"U1 compare-with-friends prompt (skip 2)",0x128f76,u1_compare_jmp_code,sizeof(u1_compare_jmp_code),sizeof(u1_compare_jmp_patch),NULL,u1_compare_jmp_patch},
    {"U1 menu add item",0x132f00,u1_menu_add_item_code,sizeof(u1_menu_add_item_code),6,u1_menu_add_item,NULL},
    {"U1 credits tick",0x3b41e0,u1_credits_code,sizeof(u1_credits_code),6,u1_credits_tick,NULL},
    {"U1 menu text draw",0x13acf0,u1_text_draw_code,sizeof(u1_text_draw_code),6,u1_text_draw,NULL},
    {"U1 menu item draw (1)",0x133fbc,u1_menu_item_begin_code,sizeof(u1_menu_item_begin_code),9,u1_menu_item_begin,NULL},
    {"U1 menu item draw end (1)",0x133fc8,u1_menu_item_end1_code,sizeof(u1_menu_item_end1_code),7,u1_menu_item_end,NULL},
    {"U1 menu item draw (2)",0x133feb,u1_menu_item_begin_code,sizeof(u1_menu_item_begin_code),9,u1_menu_item_begin,NULL},
    {"U1 menu item draw end (2)",0x133ff7,u1_menu_item_end2_code,sizeof(u1_menu_item_end2_code),8,u1_menu_item_end,NULL},
    {"U1 options menu without photo mode (1)",0x144e3b,u1_photo_row_1_code,sizeof(u1_photo_row_1_code),
     sizeof(u1_photo_row_1_patch),NULL,u1_photo_row_1_patch},
    {"U1 options menu without photo mode (2)",0x1450d4,u1_photo_row_2_code,sizeof(u1_photo_row_2_code),
     sizeof(u1_photo_row_2_patch),NULL,u1_photo_row_2_patch},
    {"U1 options menu without photo mode (3)",0x1452eb,u1_photo_row_3_code,sizeof(u1_photo_row_3_code),
     sizeof(u1_photo_row_3_patch),NULL,u1_photo_row_3_patch},
    {"U2 camera focus NaN reset",0x33ba13,u2_focus_assert_code,sizeof(u2_focus_assert_code),
     sizeof(u2_focus_reset),NULL,u2_focus_reset},
    {"U2 leg IK NaN delta",0x8ec618,u2_leg_ik_assert_code,sizeof(u2_leg_ik_assert_code),
     sizeof(u2_leg_ik_zero),NULL,u2_leg_ik_zero},
};
int runtime_guest_callback_wanted(const RuntimeGuestCallback *c, uint64_t offset) {
    return aim_assist_gate(c,offset);
}
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    *count=sizeof(guest_callbacks)/sizeof(*guest_callbacks);
    return guest_callbacks;
}
void runtime_guest_callback_installed(const RuntimeGuestCallback *callback) {
    if (callback->callback==u1_aim_controller) aim_hooked=1;
}

/* The button pictures for this controller: 1 PlayStation (DualShock, DualSense: the game's own),
 * 2 Xbox (everything else; the buttons are named by position, so a Switch pad is shown as the
 * Xbox layout). */
static int prompts_pad_device(SDL_Gamepad *g) {
    const SDL_GamepadType type=SDL_GetGamepadType(g);
    if (type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5) return 1;
    return SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)==SDL_GAMEPAD_BUTTON_LABEL_CROSS ? 1 : 2;
}

/* Turning the left stick in circles (valves, wheels, cranks: the games detect the turn from the
 * stick passing the four directions up, left, down, right in order, each at most 0.31 s after the
 * last, and rolling the eight movement-key directions never does). While a rotate key is held the
 * left stick circles at full deflection, 1.25 turns a second, a smooth sweep that starts where the
 * stick points (the movement keys held, or up). `rotate_dir`: -1 counter-clockwise, 1 clockwise. */
#define ROTATE_TURNS_PER_SECOND 1.25f
static int host_rotate;               /* the rotate keys held right now (set in sample_host) */
static int host_sideways;             /* the movement keys left or right held (set in sample_host) */
static float rotate_angle;            /* radians clockwise from up */
static uint64_t rotate_last_us;
static void rotate_stick(PadData *d, int rotate_dir, uint64_t now) {
    if (!rotate_dir) { rotate_last_us=0; return; }
    if (!rotate_last_us || now-rotate_last_us>100000) {
        const float x=((float)d->left_x-127.5f)/127.5f, y=((float)d->left_y-127.5f)/127.5f;
        rotate_angle=x*x+y*y>0.25f ? SDL_atan2f(x,-y) : 0.0f;
    } else {
        float dt=(float)(now-rotate_last_us)*1e-6f;
        if (dt>0.05f) dt=0.05f;
        rotate_angle+=(float)rotate_dir*dt*ROTATE_TURNS_PER_SECOND*6.2831853f;
        if (rotate_angle>6.2831853f) rotate_angle-=6.2831853f;
        if (rotate_angle<0.0f) rotate_angle+=6.2831853f;
    }
    rotate_last_us=now;
    const float x=127.5f+127.5f*SDL_sinf(rotate_angle), y=127.5f-127.5f*SDL_cosf(rotate_angle);
    d->left_x=(uint8_t)(x<0.0f ? 0 : x>255.0f ? 255 : (int)(x+0.5f));
    d->left_y=(uint8_t)(y<0.0f ? 0 : y>255.0f ? 255 : (int)(y+0.5f));
}

/* ---- menu objects (U1 1.02, found in the disassembly) ---------------------------------------
 * (the callback is defined with its table below) A game menu object: item count (int) at +0x98, the item pointers from +0xa0, the selected item (int) at
 * +0x150. 0x132f00 adds an item (rdi menu, rsi item), 0x1335a0 selects an index (rdi menu, esi index; it asks
 * the item whether it can be selected, vfunc +0x40, and calls vfunc +0x30 with 0 / 1 on the old / new item).
 * The hook keeps the menus that were built; the last one is the screen the player is on (probe: F9 in a menu). */
static int guest_readable(uint64_t p) { return p>0x10000 && p<0x00007fffffffffffull; }
/* The label of the keyboard row: the texts of the game live in memory as NUL-terminated strings (a run of them: nothing after one may
 * move), so a text is replaced by one of the same length, padded with spaces. Looked for a few times after the start (the text bank
 * may load late). Only the two languages of the dump. */
#ifdef _WIN32
__attribute__((unused)) static int keys_label_patch(const char *from, const char *to) {
    const size_t len=strlen(from);
    int patched=0;
    char padded[64];
    memset(padded,0,sizeof(padded));
    memset(padded,' ',len);
    memcpy(padded,to,strlen(to)<len ? strlen(to) : len);
    char *buf=malloc(4u<<20);
    if (!buf) return 0;
    uintptr_t addr=0x1000000000ull;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr<0x1400000000ull && VirtualQuery((LPCVOID)addr,&mbi,sizeof(mbi))) {
        const uintptr_t base=(uintptr_t)mbi.BaseAddress, size=mbi.RegionSize;
        addr=base+size;
        if (mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(mbi.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READWRITE))) continue;
        for (uintptr_t off=0;off<size;) {
            size_t n=size-off<(4u<<20) ? size-off : (4u<<20);
            SIZE_T got=0;
            if (!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(base+off),buf,n,&got) || got==0) break;
            for (size_t i=0;i+len<=got;) {
                const char *p=memchr(buf+i,from[0],got-len-i+1);
                if (!p) break;
                i=(size_t)(p-buf);
                if (!memcmp(p,from,len) && (i+len>=got || p[len]==0) && (i==0 || p[-1]==0)) {
                    SIZE_T written=0;
                    if (WriteProcessMemory(GetCurrentProcess(),(LPVOID)(base+off+i),padded,len,&written) && written==len) ++patched;
                }
                ++i;
            }
            off+=n>len ? n-(len-1) : n;
            if (n<(4u<<20)) break;
        }
    }
    free(buf);
    return patched;
}
__attribute__((unused)) static void *keys_label_thread(void *unused) {
    (void)unused;
    for (int round=0;round<8;++round) {
        SDL_Delay(round==0 ? 4000 : 6000);
        const int a=keys_label_patch("Joysticks gauche/droit","Touches clavier/souris");
        const int b=keys_label_patch("Left/Right Sticks","Keyboard/Mouse");
        if (a || b) printf("Runtime: keys row label reworded (%d French, %d English)\n",a,b);
        else printf("Runtime: keys row label not found (round %d, BB_LANGUAGE=%s, row view %#llx)\n",round,getenv("BB_LANGUAGE") ? getenv("BB_LANGUAGE") : "-",(unsigned long long)u1_keys_row_view);
    }
    return NULL;
}
#endif
/* F7 in a menu: where does the game keep the sprite descriptors of its icons? The atlas cell of an icon gives UV
 * floats (u = x/512, v = y/512): two adjacent words equal to those of a known icon mark its descriptor, and the
 * words around it show the size it is drawn at. Prints the surroundings of the first hits for the neutral d-pad
 * frame (6,5) and the cross icon (202,85) of the U1 atlas. */
static void menu_scan_sprites(void) {
#ifdef _WIN32
    static const struct { const char *name; uint32_t a, b; } wanted[]={
        {"dpad neutral (6,5) normalized",0x3c400000u,0x3c200000u}, {"cross (202,85) normalized",0x3ec80000u,0x3e2a0000u},
        {"dpad neutral (6,5) pixels as floats",0x40c00000u,0x40a00000u}, {"cross (202,85) pixels as floats",0x434a0000u,0x42aa0000u},
        {"dpad neutral (6,5) pixels as ints",6u,5u}, {"cross (202,85) pixels as ints",202u,85u},
        {"dpad neutral size (63,65) as floats",0x427c0000u,0x42820000u}, {"cross size (44,44) as floats",0x42300000u,0x42300000u}};
    for (size_t w=0;w<sizeof(wanted)/sizeof(*wanted);++w) {
        int hits=0;
        uint64_t addr=0x1000000ull;
        MEMORY_BASIC_INFORMATION mbi;
        while (addr<0x1400000000ull && hits<4 && VirtualQuery((LPCVOID)(uintptr_t)addr,&mbi,sizeof(mbi))) {
            const uint64_t base=(uint64_t)(uintptr_t)mbi.BaseAddress, size=mbi.RegionSize;
            addr=base+size;
            if (mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(mbi.Protect&(PAGE_READWRITE|PAGE_READONLY|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY))) continue;
            if (size>(64u<<20)) continue;
            uint32_t *buf=malloc(size);
            if (!buf) continue;
            if (safe_read(buf,base,size)) {
                for (size_t i=0;i+1<size/4 && hits<4;++i) {
                    if ((buf[i]==wanted[w].a && buf[i+1]==wanted[w].b) || (wanted[w].a!=wanted[w].b && buf[i]==wanted[w].b && buf[i+1]==wanted[w].a)) {
                        const uint64_t at=base+i*4;
                        printf("Runtime: sprite scan %s hit at %#llx (image+%#llx)\n",wanted[w].name,(unsigned long long)at,(unsigned long long)(at-u1_image));
                        for (int j=-12;j<20;j+=8) {
                            uint32_t row[8];
                            if (safe_read(row,at+(uint64_t)(int64_t)(j*4),32)) {
                                float f[8]; memcpy(f,row,32);
                                printf("Runtime: sprite scan   %+d: %08x %08x %08x %08x %08x %08x %08x %08x | %g %g %g %g %g %g %g %g\n",j,
                                       row[0],row[1],row[2],row[3],row[4],row[5],row[6],row[7],f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]);
                            }
                        }
                        ++hits;
                    }
                }
            }
            free(buf);
        }
        printf("Runtime: sprite scan %s: %d hits\n",wanted[w].name,hits);
    }
#endif
}
/* F8 in a menu: a snapshot of the game's data segment (image + 0x94c000, 0x8a9000 bytes: the settings are
 * globals there, e.g. the brightness float). The first two snapshots, taken with nothing changed, tell which words
 * move by themselves; each later snapshot reports the other words that changed since the one before. Change one
 * setting by one step between two F8 to find where it is kept. */
#define DATA_OFFSET 0x94c000u
#define DATA_SIZE 0x8a9000u
static void menu_data_snapshot(void) {
    static uint32_t *prev, *noise_a, *noise_b; static int count; static uint8_t *noisy;
    if (!u1_image) return;
    const size_t words=DATA_SIZE/4;
    uint32_t *now=malloc(DATA_SIZE);
    if (!now) return;
    if (!safe_read(now,u1_image+DATA_OFFSET,DATA_SIZE)) {
        /* one page at a time: some pages of the segment may not be mapped */
        for (size_t off=0;off<DATA_SIZE;off+=4096) if (!safe_read((uint8_t *)now+off,u1_image+DATA_OFFSET+off,4096)) memset((uint8_t *)now+off,0,4096);
    }
    if (!noisy) noisy=calloc(words,1);
    (void)noise_a; (void)noise_b;
    if (count==1 && prev && noisy) { /* the second snapshot: what moved by itself */
        size_t moving=0;
        for (size_t i=0;i<words;++i) if (prev[i]!=now[i]) { noisy[i]=1; ++moving; }
        printf("Runtime: menu data: noise snapshot, %zu words move by themselves\n",moving);
    } else if (count>=2 && prev && noisy) {
        int shown=0;
        for (size_t i=0;i<words;++i) if (prev[i]!=now[i] && !noisy[i]) {
            if (shown++<80) {
                float fo, fn; memcpy(&fo,&prev[i],4); memcpy(&fn,&now[i],4);
                printf("Runtime: menu data: image+%#zx %08x -> %08x (%d -> %d, %g -> %g)\n",(size_t)DATA_OFFSET+i*4,prev[i],now[i],(int)prev[i],(int)now[i],fo,fn);
            }
        }
        printf("Runtime: menu data: %d words changed (snapshot %d)\n",shown,count);
    } else printf("Runtime: menu data: first snapshot taken\n");
    free(prev); prev=now; ++count;
}
static void menu_probe(void) {
    uint64_t probe_menu=0;
    {
        float mx=0, my=0; int ww=0, wh=0, pw=0, ph=0;
        SDL_Window *w=SDL_GetMouseFocus();
        SDL_GetMouseState(&mx,&my);
        if (w) { SDL_GetWindowSize(w,&ww,&wh); SDL_GetWindowSizeInPixels(w,&pw,&ph); }
        printf("Runtime: menu probe cursor=(%.1f,%.1f) window=%dx%d pixels=%dx%d\n",mx,my,ww,wh,pw,ph);
        pthread_mutex_lock(&menu_text_lock);
        printf("Runtime: menu probe texts %d (age %.0f ms)\n",menu_text_count,(double)(now_us()-menu_text_frame_us)/1000.0);
        for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (now_us()-t->us>200000) continue; /* not drawn lately */
            uint64_t menu=0; int32_t index=-1, count=0;
            if (safe_read(&menu,t->item+0x80,8) && safe_read(&count,menu+0x98,4)) {
                for (int j=0;j<count && j<64;++j) { uint64_t p=0; if (safe_read(&p,menu+0xa0+8u*(uint64_t)j,8) && p==t->item) { index=j; break; } }
            }
            printf("Runtime: menu probe text item=%#llx index=%d n=%d text=(%.1f %.1f %.1f %.1f) rect=(%.1f %.1f %.1f %.1f) style=(%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f)\n",
                   (unsigned long long)t->item,index,t->ordinal,t->x,t->y,t->w,t->h,t->rx,t->ry,t->rw,t->rh,
                   t->style[0],t->style[1],t->style[2],t->style[3],t->style[4],t->style[5],t->style[6],t->style[7]);
            if (!probe_menu) probe_menu=menu;
        }
        for (int i=0;i<free_text_count;++i) {   /* texts outside the items: lists, carousels, titles, footers */
            const MenuText *t=&free_texts[i];
            if (now_us()-t->us>300000) continue;
            printf("Runtime: menu probe free text text=(%.1f %.1f %.1f %.1f) rect=(%.1f %.1f %.1f %.1f) style=(%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f)\n",
                   t->x,t->y,t->w,t->h,t->rx,t->ry,t->rw,t->rh,t->style[0],t->style[1],t->style[2],t->style[3],t->style[4],t->style[5],t->style[6],t->style[7]);
        }
        pthread_mutex_unlock(&menu_text_lock);
    }
    if (u1_image) {   /* the volumes: the places found with the data snapshots (1.0 -> 0.95 -> 0.9 for one step down) */
        static const uint32_t places[]={0x9549f8,0xcc3db8,0xcc3df0,0x9549c0};
        for (size_t p=0;p<sizeof(places)/sizeof(*places);++p) {
            float v[16];
            if (!safe_read(v,u1_image+places[p]-0x10,sizeof(v))) continue;
            printf("Runtime: menu probe volumes image+%#x -0x10:",places[p]);
            for (int j=0;j<16;++j) printf(" %.3g",v[j]);
            printf("\n");
        }
    }
    const uint64_t menu=probe_menu ? probe_menu : u1_menu_last;
    if (!guest_readable(menu)) { printf("Runtime: menu probe: no menu seen\n"); return; }
    {   /* what changed in the items since the last probe of the same menu (a value changed between two F9) */
        static uint32_t snap[32][0x1c0/4];
        static uint64_t snap_menu; static int snap_count;
        int32_t n=0;
        if (safe_read(&n,menu+0x98,4) && n>0 && n<=32) {
            uint32_t now_words[32][0x1c0/4];
            memset(now_words,0,sizeof(now_words));
            for (int i=0;i<n;++i) { uint64_t p=0; if (safe_read(&p,menu+0xa0+8u*(uint64_t)i,8)) safe_read(now_words[i],p,0x1c0); }
            if (snap_menu==menu && snap_count==n) {
                int changes=0;
                for (int i=0;i<n;++i) for (int j=0;j<0x1c0/4;++j)
                    if (snap[i][j]!=now_words[i][j] && changes++<60)
                        printf("Runtime: menu probe DIFF item %d +%03x %08x -> %08x\n",i,j*4,snap[i][j],now_words[i][j]);
                printf("Runtime: menu probe DIFF total %d\n",changes);
            }
            memcpy(snap,now_words,sizeof(snap)); snap_menu=menu; snap_count=n;
        }
    }
    {   /* the memory reachable from the items by pointers (depth 4), compared with the last probe: a value kept in
         * a settings object far from the row shows up as a changed word */
        enum { DEEP_BLOCKS=900, DEEP_BYTES=0x100 };
        static uint64_t old_addr[DEEP_BLOCKS], new_addr[DEEP_BLOCKS];
        static uint32_t old_data[DEEP_BLOCKS][DEEP_BYTES/4], new_data[DEEP_BLOCKS][DEEP_BYTES/4];
        static int old_n, new_n, new_depth[DEEP_BLOCKS], new_parent[DEEP_BLOCKS], new_poff[DEEP_BLOCKS];
        static uint64_t old_menu;
        int32_t n=0;
        if (safe_read(&n,menu+0x98,4) && n>0 && n<=32) {
            new_n=0;
            for (int i=0;i<n && new_n<DEEP_BLOCKS;++i) {
                uint64_t p=0;
                if (safe_read(&p,menu+0xa0+8u*(uint64_t)i,8) && safe_read(new_data[new_n],p,DEEP_BYTES)) { new_addr[new_n]=p; new_depth[new_n]=0; new_parent[new_n]=-1; new_poff[new_n]=i; ++new_n; }
            }
            for (int b=0;b<new_n;++b) {
                if (new_depth[b]>=4) continue;
                for (int j=0;j<DEEP_BYTES/4-1 && new_n<DEEP_BLOCKS;j+=2) {
                    const uint64_t v=((uint64_t)new_data[b][j+1]<<32)|new_data[b][j];
                    if (v<0x1000000ull || v>=0x1400000000ull || (v&3)) continue;
                    int seen=0;
                    for (int q=0;q<new_n;++q) if (new_addr[q]<=v && v<new_addr[q]+DEEP_BYTES) { seen=1; break; }
                    if (seen || v==menu) continue;
                    if (safe_read(new_data[new_n],v,DEEP_BYTES)) { new_addr[new_n]=v; new_depth[new_n]=new_depth[b]+1; new_parent[new_n]=b; new_poff[new_n]=j*4; ++new_n; }
                }
            }
            if (old_menu==menu && old_n>0) {
                int changes=0;
                for (int b=0;b<new_n;++b) for (int a=0;a<old_n;++a) {
                    if (old_addr[a]!=new_addr[b]) continue;
                    for (int j=0;j<DEEP_BYTES/4;++j)
                        if (old_data[a][j]!=new_data[b][j] && new_depth[b]<=2 && changes++<80) {
                            float fo, fn; memcpy(&fo,&old_data[a][j],4); memcpy(&fn,&new_data[b][j],4);
                            printf("Runtime: menu probe DEEP depth %d block %#llx +%03x %08x -> %08x (%g -> %g) path:",new_depth[b],
                                   (unsigned long long)new_addr[b],j*4,old_data[a][j],new_data[b][j],fo,fn);
                            for (int c=b;c>=0;c=new_parent[c]) printf(" [%#llx via %s%x]",(unsigned long long)new_addr[c],new_parent[c]<0 ? "item#" : "+",new_poff[c]);
                            printf("\n");
                        }
                    break;
                }
                printf("Runtime: menu probe DEEP total %d (blocks %d -> %d)\n",changes,old_n,new_n);
            }
            {   /* the row count of a list: where does the number BB_SCAN_VALUE (default 47, the medals) sit in what the item reaches? */
                const char *sv=getenv("BB_SCAN_VALUE");
                const uint32_t want=sv ? (uint32_t)atoi(sv) : 47u;
                int shown=0;
                for (int b=0;b<new_n && shown<24;++b) for (int j=0;j<DEEP_BYTES/4 && shown<24;++j)
                    if (new_data[b][j]==want) {
                        printf("Runtime: menu probe SCAN %u at depth %d block %#llx +%03x path:",want,new_depth[b],(unsigned long long)new_addr[b],j*4);
                        for (int c=b;c>=0;c=new_parent[c]) printf(" [%#llx via %s%x]",(unsigned long long)new_addr[c],new_parent[c]<0 ? "item#" : "+",new_poff[c]);
                        printf("\n"); ++shown;
                    }
            }
            memcpy(old_addr,new_addr,sizeof(old_addr)); memcpy(old_data,new_data,sizeof(old_data)); old_n=new_n; old_menu=menu;
        }
    }    const int32_t count=*(const volatile int32_t *)(uintptr_t)(menu+0x98), selected=*(const volatile int32_t *)(uintptr_t)(menu+0x150);
    printf("Runtime: menu probe menu=%#llx count=%d selected=%d\n",(unsigned long long)menu,count,selected);
    for (int i=0;i<count && i<10;++i) {
        const uint64_t item=*(const volatile uint64_t *)(uintptr_t)(menu+0xa0+8u*(uint64_t)i);
        if (!guest_readable(item)) { printf("Runtime: menu probe item %d pointer %#llx\n",i,(unsigned long long)item); continue; }
        printf("Runtime: menu probe item %d at %#llx\n",i,(unsigned long long)item);
        {   /* the slider guess: the item's model (+0x08) points at the value at +0x18 */
            uint64_t model=0, vp=0; float value=-1.0f;
            if (safe_read(&model,item+8,8) && safe_read(&vp,model+0x18,8)) safe_read(&value,vp,4);
            printf("Runtime: menu probe   slider guess: model=%#llx value ptr=%#llx float=%g\n",(unsigned long long)model,(unsigned long long)vp,value);
            static const uint32_t ptr_offsets[2]={0x28u,0xc0u};
            for (int po=0;po<2;++po) {   /* the blocks the model points to (a list keeps its rows there) */
                const uint32_t ptr_off=ptr_offsets[po];
                uint64_t blockp=0;
                if (!safe_read(&blockp,model+ptr_off,8) || blockp<0x1000000ull || blockp>=0x1400000000ull) continue;
                for (uint32_t off=0;off<0x100;off+=32) {
                    uint32_t bw[8];
                    if (!safe_read(bw,blockp+off,32)) break;
                    if (!(bw[0]|bw[1]|bw[2]|bw[3]|bw[4]|bw[5]|bw[6]|bw[7])) continue;
                    printf("Runtime: menu probe   model->%#x block +%03x %08x %08x %08x %08x %08x %08x %08x %08x\n",ptr_off,off,bw[0],bw[1],bw[2],bw[3],bw[4],bw[5],bw[6],bw[7]);
                }
            }
            for (uint32_t off=0;off<0x100;off+=32) {   /* the model itself: a value may sit in it */
                uint32_t mw[8];
                if (!safe_read(mw,model+off,32)) break;
                if (!(mw[0]|mw[1]|mw[2]|mw[3]|mw[4]|mw[5]|mw[6]|mw[7])) continue;
                printf("Runtime: menu probe   model +%03x %08x %08x %08x %08x %08x %08x %08x %08x\n",off,mw[0],mw[1],mw[2],mw[3],mw[4],mw[5],mw[6],mw[7]);
            }
        }
        for (uint32_t off=0;off<0x1c0;off+=32) {
            uint32_t w[8];
            if (!safe_read(w,item+off,32)) break;
            if (!(w[0]|w[1]|w[2]|w[3]|w[4]|w[5]|w[6]|w[7])) continue; /* all zero: not printed */
            printf("Runtime: menu probe   +%03x %08x %08x %08x %08x %08x %08x %08x %08x\n",off,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
        }
    }
}
/* A confirmation (return to the checkpoint, to the main menu): a line of exactly two short texts (Oui / Non) near the middle of the
 * screen, the bottom prompts (three on their line) and the list rows (many pairs) being told apart by those counts. The texts may be
 * the row's own or free ones, so both tables are read. Returns the left and the right text. */
typedef struct { float lx, ly, lw, lh, rx, ry, rw, rh; } DialogInfo;
static int menu_dialog_find(uint64_t now, DialogInfo *d) {
    float cx[48], cy[48], cw[48], ch[48], titx[48], tity[48];
    int n=0, nt=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int pass=0;pass<2;++pass) {
        const int total=pass ? free_text_count : menu_text_count;
        for (int i=0;i<total;++i) {
            const MenuText *t=pass ? &free_texts[i] : &menu_texts[i];
            if (now-t->us>100000 || t->w<=1.0f) continue;
            if (t->w>150.0f) { if (nt<48) { titx[nt]=t->x+t->w*0.5f; tity[nt]=t->y+t->h*0.5f; ++nt; } continue; }
            if (t->w>90.0f || t->h<18.0f || t->h>40.0f || n>=48) continue;
            cx[n]=t->x; cy[n]=t->y; cw[n]=t->w; ch[n]=t->h; ++n;
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    int found=0;
    for (int a=0;a<n;++a) for (int b=0;b<n;++b) {
        if (cx[a]>=cx[b]-20.0f) continue;
        const float ya=cy[a]+ch[a]*0.5f;
        if (fabsf(ya-(cy[b]+ch[b]*0.5f))>=4.0f) continue;
        int on_line=0;
        for (int c=0;c<n;++c) if (fabsf(ya-(cy[c]+ch[c]*0.5f))<4.0f) ++on_line;
        const float mid=((cx[a]+cw[a]*0.5f)+(cx[b]+cw[b]*0.5f))*0.5f;
        if (on_line!=2 || fabsf(mid-640.0f)>=120.0f || cx[b]-(cx[a]+cw[a])>120.0f) continue;
        int title=0;   /* the question: a wide text centred over the pair, 40..130 above it */
        for (int t=0;t<nt;++t) if (fabsf(titx[t]-mid)<50.0f && ya-tity[t]>40.0f && ya-tity[t]<130.0f) title=1;
        if (!title) continue;
        if (found) return 0;   /* a second pair: a list, not a confirmation */
        *d=(DialogInfo){cx[a],cy[a],cw[a],ch[a],cx[b],cy[b],cw[b],ch[b]};
        found=1;
    }
    return found;
}
/* A carousel screen (cinematics, chapters): its item draws a 13x13 arrow text. There the wheel and the left/right keys move
 * sideways, not up and down. A confirmation has the same wheel. */
static int menu_carousel_recent(uint64_t now) {
    DialogInfo dialog;
    if (menu_dialog_find(now,&dialog)) return 1;
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal>=1 && now-t->us<100000 && t->w>=12.0f && t->w<=14.0f && t->h>=12.0f && t->h<=14.0f && t->style[0]>0.0f) found=1;
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* A scrolling list (treasures, medals, statistics): ONE menu item that draws the rows itself, a label column of texts 25.2 tall
 * (27.7 for the highlighted row, whose style[5] is 3.0) with a pitch of 29.2. Returns the highlighted row's centre, the pitch,
 * the label column and the vertical extent of the rows. */
typedef struct { float sel_cy, pitch, colx, top, bottom; uint64_t item; } ListInfo;
static int menu_list_find_ex(uint64_t now, ListInfo *info, int need_highlight) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    float best_x=0.0f; int best=0; uint64_t best_item=0;
    for (int i=0;i<menu_text_count;++i) {
        const MenuText *a=&menu_texts[i];
        if (a->ordinal<1 || now-a->us>100000 || a->w<=1.0f || a->h<22.0f || a->h>29.0f) continue;
        int n=0;
        for (int j=0;j<menu_text_count;++j) {
            const MenuText *b=&menu_texts[j];
            if (b->item==a->item && b->ordinal>=1 && now-b->us<=100000 && b->w>1.0f && b->h>=22.0f && b->h<=29.0f && fabsf(b->x-a->x)<1.0f) ++n;
        }
        if (n>best) { best=n; best_x=a->x; best_item=a->item; }
    }
    if (best>=2) {
        float ys[64]; int nys=0; float sel=-1.0f, top=1e9f, bottom=-1e9f;
        for (int j=0;j<menu_text_count && nys<64;++j) {
            const MenuText *b=&menu_texts[j];
            if (b->ordinal<1 || now-b->us>100000 || b->w<=1.0f || b->h<22.0f || b->h>29.0f || fabsf(b->x-best_x)>=1.0f) continue;
            ys[nys++]=b->y+b->h*0.5f;
            if (b->y<top) top=b->y;
            if (b->y+b->h>bottom) bottom=b->y+b->h;
            if (b->style[5]>2.5f) sel=b->y+b->h*0.5f;
        }
        float pitch=1e9f;
        for (int a=0;a<nys;++a) for (int b=0;b<nys;++b) { const float d=ys[a]-ys[b]; if (d>5.0f && d<pitch) pitch=d; }
        if ((sel>=0.0f || !need_highlight) && pitch>=25.0f && pitch<=35.0f) { info->sel_cy=sel; info->pitch=pitch; info->colx=best_x; info->top=top; info->bottom=bottom; info->item=best_item; found=1; }
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* ---- menu keys (U1) -----------------------------------------------------------------------
 * While a game menu is open the game's time step is 0 (the same sign as the pause above): the
 * keyboard then has its own keys, Enter = select (cross), Esc = back (circle), Alt = options
 * (resume, or the options of the game selection), Backspace = title menu (R1). They replace Space,
 * Ctrl and the other keys of those four buttons; the mouse buttons and the wheel do nothing there
 * (the mouse has its own menu code). The other keys (WASD, arrows) keep working. */
enum { A_CROSS=0, A_CIRCLE=1, A_TRIANGLE=3, A_R1=5, A_OPTIONS=10, A_UP=12, A_DOWN=13, A_LEFT=14, A_RIGHT=15 };
static int menu_mode, menu_guard;
/* A menu is open when the game's clock stands still (the pause: time step 0) or when no play camera has run
 * yet (title screen, game selection, main menu, loading: the time step is not known before the first play
 * camera, measured in game). The main menu reached again from a game cannot be told from the play by the
 * clock (it keeps running there): once the menu keys are on, they stay on after the pause ends until the
 * follow camera of the play runs again, or the pause was left with Alt or Esc (resume). "Back to the main
 * menu" and the title screen after it are then in menu mode; resuming in a game section without the follow
 * camera (vehicles) leaves it at once through Alt/Esc. */
static int menu_exit_key;             /* 1: the last menu key pressed was Alt or Esc (resume), 0: Enter */
/* Play sections with a fixed camera (the German submarine) never run the follow camera: after a skipped cutscene the menu
 * keys would stay on for ever. 1: forced off because the clock runs and no menu item text has been drawn for a while;
 * 2: forced back on because menu items are drawn again (loading screen followed by the main menu). Set by menu_update. */
static int menu_idle_off;
static int menu_open(void) {
    if (!u1_image) return 0;
    if (u1_credits_recent(now_us())) return 1;   /* (the credits have no menu item: the keys and the mouse must be on for the way back) */
    if (u1_camera_dt_offset && camera_stopped(u1_image,u1_camera_dt_offset)) return 1;
    if (menu_idle_off==2) return 1;
    if (menu_idle_off==1) return 0;
    if (!u1_camera_dt_offset) return 1;
    if (!menu_mode) return 0;
    const uint64_t now=now_us();
    const int follow=camera_hook_us && now-camera_hook_us<100000;
    return !(follow || menu_exit_key);
}
/* No menu text drawn for 400 ms while the menu keys are on = not a menu but a cutscene opening a chapter (no play camera has run
 * yet): Esc and Alt then press the options button, which opens the pause. Set by menu_update. */
static int menu_no_text;
static int menu_esc_latch;            /* Esc was used as options: it must not also press circle until released */
static int menu_texts_recent(uint64_t now) {
    int seen=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !seen;++i) if (now-menu_texts[i].us<400000) seen=1;
    for (int i=0;i<free_text_count && !seen;++i) if (now-free_texts[i].us<400000) seen=1;
    pthread_mutex_unlock(&menu_text_lock);
    return seen;
}
static int menu_keys_held(const bool *k) {
    return k && (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || k[SDL_SCANCODE_ESCAPE] ||
                 k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT] || k[SDL_SCANCODE_BACKSPACE]);
}
static int menu_key_action_down(int a, const bool *k) {
    if (!k) return 0;
    switch (a) {
    case A_CROSS: return k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER];
    case A_CIRCLE: return k[SDL_SCANCODE_ESCAPE] && !menu_no_text && !menu_esc_latch;
    /* Alt is the triangle of the game selection screen ("Options") before any game was played, and the
     * options button (resume) of the pause after it: both at once were ignored by the selection screen
     * (measured: 0x1008 reached the game and nothing happened). */
    case A_TRIANGLE: return !u1_camera_dt_offset && !menu_no_text && (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]);
    case A_OPTIONS:
        if (menu_no_text) return k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT] || k[SDL_SCANCODE_ESCAPE];
        return u1_camera_dt_offset && (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]);
    case A_R1: return k[SDL_SCANCODE_BACKSPACE];
    default: return 0;
    }
}
/* The keyboard and mouse state of one button, by the mode. At a change of mode the four buttons stay
 * released until their keys are (the Esc that opened the pause must not also go back). */
static int key_action_down(int a, const bool *k, uint32_t mouse, uint64_t now) {
    const int menu_button=a==A_CROSS || a==A_CIRCLE || a==A_TRIANGLE || a==A_R1 || a==A_OPTIONS;
    if (menu_button && menu_guard) return 0;
    if (!menu_mode) return action_down(a,k,mouse,now);
    /* The mouse in a menu: the wheel moves up and down (d-pad), the lower thumb button (Mouse 4, "back") goes
     * back (circle). The left click will validate (cross) once the cursor can select the text under it: not
     * yet, a click anywhere would validate whatever is selected. */
    if (menu_carousel_recent(now)) {   /* a carousel: the wheel goes sideways */
        if (a==A_LEFT && now<wheel_until_us[0]) return 1;
        if (a==A_RIGHT && now<wheel_until_us[1]) return 1;
    } else {
        if (a==A_UP && now<wheel_until_us[0]) return 1;
        if (a==A_DOWN && now<wheel_until_us[1]) return 1;
    }
    if (a==A_CIRCLE && ((mouse & 8u) || ((mouse & 2u) && u1_credits_recent(now)))) return 1;   /* (the credits have nothing to click: the right button goes back) */
    if (menu_button) {
        if (menu_key_action_down(a,k)) return 1;
        /* No menu text while the menu keys are on, but a button icon on screen (a hint): a scripted scene with its own prompt
         * (the fight with the diver, "press triangle after the first punch"): the keys of the play press these buttons too,
         * or the prompt can never be answered. A plain cutscene draws no icon: E stays inert there (it opens the skip menu). */
        if (!menu_no_text || !bbgpu_atlas_recent(500)) return 0;
        for (int i=0;i<2;++i) if (input.keys[a][i].kind==BIND_KEY && bind_down(&input.keys[a][i],k,mouse,now)) return 1;
        return 0;
    }
    for (int i=0;i<2;++i) if (input.keys[a][i].kind==BIND_KEY && bind_down(&input.keys[a][i],k,mouse,now)) return 1;
    return 0;
}
/* Diagnosis for the title screen and the main menu (BB_MENU_DEBUG=1 or always for now): the signs the pause
 * uses, printed when one changes, to find what tells the other screens apart (title, main menu, loading,
 * cutscene, play). dt: the camera time step (-1 before the first play camera); follow, sticks: the camera
 * and joypad callbacks ran in the last 100 ms. */
static void menu_trace(uint64_t now) {
    static int last=-2, started;
    float dt=-1.0f;
    if (u1_image && u1_camera_dt_offset) dt=*(const volatile float *)(uintptr_t)(u1_image+u1_camera_dt_offset);
    const int follow=camera_hook_us && now-camera_hook_us<100000, sticks=sticks_hook_us && now-sticks_hook_us<100000;
    const int state=(u1_image!=0)|((dt>0.0f)<<1)|(follow<<2)|(sticks<<3)|((dt<0.0f)<<4);
    if (state==last) return;
    last=state;
    if (!started) started=1;
    printf("Runtime: menu trace t=%.1f image=%d dt=%.4f follow=%d sticks=%d\n",now*1e-6,u1_image!=0,dt,follow,sticks);
}
static void menu_update(const bool *k, uint32_t mouse) {
    if (mouse_debug()) {   /* BB_MOUSE_DEBUG: menu trace and the F7/F8/F9 probes */
        menu_trace(now_us());
        static int f9_was_down;
        const int f9=k && k[SDL_SCANCODE_F9];
        if (f9 && !f9_was_down && menu_mode) menu_probe();
        f9_was_down=f9;
        static int f7_was_down;
        const int f7=k && k[SDL_SCANCODE_F7];
        if (f7 && !f7_was_down && menu_mode) menu_scan_sprites();
        f7_was_down=f7;
        static int f8_was_down;
        const int f8=k && k[SDL_SCANCODE_F8];
        if (f8 && !f8_was_down && menu_mode) menu_data_snapshot();
        f8_was_down=f8;
    }
    if (k && menu_mode) {
        /* Alt/Esc only mean "resume" while the pause is open (clock stopped); in the main menu the clock runs and Esc just goes back */
        if (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT] || k[SDL_SCANCODE_ESCAPE]) { if (u1_image && u1_camera_dt_offset && camera_stopped(u1_image,u1_camera_dt_offset)) menu_exit_key=1; }
        else if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) menu_exit_key=0;
    }
    /* a click validates like Enter (Esc opened the pause, then the mouse chose "Oui" of "back to the main menu") */
    if (menu_mode && (mouse & 1u)) menu_exit_key=0;
    {   /* the label of the keys row, rewritten when a language change has loaded a fresh text bank (checked a few times a second) */
        static uint64_t label_checked;
        const uint64_t t=now_us();
        if (u1_image && menu_mode && t-label_checked>300000) { label_checked=t; keys_label_here(u1_image); }
    }
    {   /* the Alt prompt (REPRENDRE / Options) is the bottom bar's left text; without it the triangle icon stays the key of the play */
        static uint64_t alt_seen; static int alt_last=-1;
        const uint64_t now=now_us();
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0;i<free_text_count;++i) {
            const MenuText *t=&free_texts[i];
            if (now-t->us<100000 && fabsf(t->x-100.0f)<6.0f && t->y>430.0f && t->y<460.0f) { alt_seen=now; break; }
        }
        pthread_mutex_unlock(&menu_text_lock);
        const int alt=now-alt_seen<400000;
        if (alt!=alt_last) { alt_last=alt; bbgpu_menu_alt(alt); printf("Runtime: menu Alt prompt %s\n",alt ? "shown" : "hidden"); }
    }
    {   /* cutscene detection: menu keys on but nothing menu-like drawn (see menu_no_text) */
        static uint64_t on_since;
        const uint64_t t=now_us();
        if (!menu_mode) on_since=0; else if (!on_since) on_since=t;
        const int idle=menu_mode && t-on_since>1000000 && !menu_texts_recent(t) && !u1_credits_recent(t);   /* (the credits draw no menu text, yet they are no cutscene: Esc goes back there) */
        if (idle!=menu_no_text) { menu_no_text=idle; bbgpu_menu_cursor_hidden(idle); printf("Runtime: menu no text (cutscene) %s\n",idle ? "yes" : "no"); }
        const int esc=k && k[SDL_SCANCODE_ESCAPE];
        if (!esc) menu_esc_latch=0; else if (menu_no_text) menu_esc_latch=1;
    }
    {   /* clock running, no follow camera, no menu item text: not a menu (fixed-camera section after a skipped cutscene) */
        static uint64_t quiet_since, busy_since;
        const uint64_t t=now_us();
        /* dt offset unknown (no play camera yet: title, loading, or a fixed-camera section loaded from a checkpoint): any text,
         * even a free one (the title screen's), keeps the menu keys on; once it is known only menu item texts do */
        const int stopped=!u1_image || (u1_camera_dt_offset && camera_stopped(u1_image,u1_camera_dt_offset));
        const int follow=camera_hook_us && t-camera_hook_us<100000;
        int texts=0;
        if (!u1_camera_dt_offset) texts=menu_texts_recent(t);
        else {
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<menu_text_count && !texts;++i) if (t-menu_texts[i].us<300000) texts=1;
            pthread_mutex_unlock(&menu_text_lock);
        }
        if (u1_credits_recent(t)) texts=1;
        if (stopped || follow) { quiet_since=busy_since=0; menu_idle_off=0; }
        else if (!texts) {
            busy_since=0; if (!quiet_since) quiet_since=t;
            if (menu_mode && menu_idle_off!=1 && t-quiet_since>1500000) { menu_idle_off=1; printf("Runtime: menu keys idle off (no menu text, clock running)\n"); }
        } else {
            quiet_since=0; if (!busy_since) busy_since=t;
            if (menu_idle_off==1 && t-busy_since>500000) menu_idle_off=2;
        }
    }
    const int open=menu_open();
    if (open!=menu_mode) {
        menu_mode=open; menu_guard=1;
        bbgpu_menu_state(open);
        printf("Runtime: menu keys %s\n",open ? "on" : "off");
    }
    if (menu_guard && !menu_keys_held(k)) menu_guard=0;
}

/* ---- the mouse in the menus (U1) ------------------------------------------------------------
 * The game's menu texts are laid out in a 1280x720 space (measured: a 5-row menu has its rows at x 367.5, width
 * 600, height 33.6 and a pitch of 33.6, which matches the screenshots at 1125 lines). The cursor, put in that
 * space, tells which row of which menu it is on (u1_text_draw records the row rectangles). Hovering a row
 * presses the d-pad up or down, one step every 100 ms, until the menu's selected index (menu+0x150) is that
 * row; a left click on a row selects it the same way and then presses cross. With no menu text on screen (the
 * "press a key to start" screen) a left click presses cross. The wheel and the keys stop the hover until the
 * cursor moves again. */
#define UI_WIDTH 1280.0f
#define UI_HEIGHT 720.0f
/* Rows the game never selects (the section titles, the text over a picture): a hover that cannot reach one gives up and the
 * row is ignored for 30 s, instead of pressing up and down for ever. */
typedef struct { uint64_t menu; int index; uint64_t until; } MenuSkip;
static MenuSkip menu_skips[16];
static int menu_skipped(uint64_t menu, int index, uint64_t now) {
    for (int i=0;i<16;++i) if (menu_skips[i].menu==menu && menu_skips[i].index==index && now<menu_skips[i].until) return 1;
    return 0;
}
static void menu_skip(uint64_t menu, int index, uint64_t now) {
    int slot=0;
    for (int i=0;i<16;++i) { if (now>=menu_skips[i].until) { slot=i; break; } if (menu_skips[i].until<menu_skips[slot].until) slot=i; }
    menu_skips[slot]=(MenuSkip){menu,index,now+30000000u};
    printf("Runtime: menu row %d of %#llx cannot be selected: ignored\n",index,(unsigned long long)menu);
}
static int menu_row_at(float ux, float uy, uint64_t now, uint64_t *menu_out, int *index_out, int *count_out) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal!=0 || now-t->us>100000) continue;
        if (ux<t->rx || ux>t->rx+t->rw || uy<t->ry || uy>t->ry+t->rh) continue;
        uint64_t menu=0; int32_t count=0;
        if (!safe_read(&menu,t->item+0x80,8) || !safe_read(&count,menu+0x98,4)) continue;
        for (int j=0;j<count && j<64;++j) {
            uint64_t p=0;
            if (safe_read(&p,menu+0xa0+8u*(uint64_t)j,8) && p==t->item) {
                if (menu_skipped(menu,j,now)) break;
                *menu_out=menu; *index_out=j; *count_out=count; found=1; break;
            }
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* How many texts were drawn lately, and how many of those belong to a real menu item (a text drawn while the
 * hooks think an item is being drawn may be a stale tag: the screens that are not menus). */
static void menu_texts_visible(uint64_t now, int *texts, int *rows) {
    *texts=*rows=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count;++i) {
        const MenuText *t=&menu_texts[i];
        if (now-t->us>=100000) continue;
        ++*texts;
        uint64_t menu=0; int32_t count=0;
        if (t->ordinal==0 && safe_read(&menu,t->item+0x80,8) && safe_read(&count,menu+0x98,4) && count>0 && count<64) ++*rows;
    }
    pthread_mutex_unlock(&menu_text_lock);
}
/* The controls inside a row (U1 1.02, from the recorded texts of the row): the two small 10x10 draws are the left
 * and right arrows (the smaller x is the left one), small value texts (height 20..30, width up to 100, from the
 * 4th text on) are the choices of an Oui/Non style row (the left choice is a left press, the right one a right
 * press), a thin wide bar (height up to 12, width from 100) is a slider. kind: 1 left, 2 right, 3 slider. */
typedef struct { int kind; uint64_t menu; int index; uint64_t item; float tx, tw; } MenuControl;
static int menu_control_at(float ux, float uy, uint64_t now, MenuControl *c) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal<1 || now-t->us>100000) continue;
        int kind=0;
        if (t->w<=14.0f && t->h<=14.0f) {                       /* an arrow (10x10 in the rows, 13x13 in the carousels) */
            if (ux<t->x-8 || ux>t->x+t->w+8 || uy<t->y-8 || uy>t->y+t->h+8) continue;
            int partners=0;
            kind=1;   /* the left arrow, unless another arrow of the row lies further left */
            for (int j=0;j<menu_text_count;++j) {
                const MenuText *o=&menu_texts[j];
                if (j!=i && o->item==t->item && o->ordinal>=1 && o->w<=14.0f && o->h<=14.0f) { ++partners; if (o->x<t->x) kind=2; }
            }
            if (!partners) kind=t->x+t->w*0.5f<640.0f ? 1 : 2;   /* one arrow alone (a carousel end): left half or right half */
            if (t->style[0]<=0.0f) continue;
        } else if (t->ordinal>=3 && t->w<=100.0f && t->h>=20.0f && t->h<=30.0f) {   /* a choice */
            if (ux<t->x || ux>t->x+t->w || uy<t->y || uy>t->y+t->h) continue;
            int left_of_me=0, right_of_me=0;
            for (int j=0;j<menu_text_count;++j) {
                const MenuText *o=&menu_texts[j];
                if (j==i || o->item!=t->item || o->ordinal<3 || o->w>100.0f || o->h<20.0f || o->h>30.0f) continue;
                if (o->x<t->x-1.0f) ++left_of_me; else if (o->x>t->x+1.0f) ++right_of_me;
            }
            kind=right_of_me>0 && left_of_me==0 ? 1 : left_of_me>0 && right_of_me==0 ? 2 : 0;
            if (!kind) continue;
        } else if (t->ordinal>=3 && t->h<12.0f && t->w>=100.0f) { /* a slider */
            if (ux<t->x-6 || ux>t->x+t->w+6 || uy<t->y-12 || uy>t->y+t->h+12) continue;
            kind=3;
        } else continue;
        uint64_t menu=0; int32_t count=0;
        if (!safe_read(&menu,t->item+0x80,8) || !safe_read(&count,menu+0x98,4)) continue;
        for (int j=0;j<count && j<64;++j) {
            uint64_t p=0;
            if (safe_read(&p,menu+0xa0+8u*(uint64_t)j,8) && p==t->item) { c->kind=kind; c->menu=menu; c->index=j; c->item=t->item; c->tx=t->x; c->tw=t->w; found=1; break; }
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* The scrollbar rail of a list: a text draw 8 wide and ~438 tall (the first draws of the list item); returns its rectangle and the
 * list item. The thumb is not readable, so the bar is followed by the list's selected row: the item's model (+8) keeps it at +0x14
 * (counted over all the rows, the section titles too). The number of rows is measured once by scrolling to the end. */
static int menu_rail_find(uint64_t now, float *rx, float *ry, float *rw, float *rh, uint64_t *item) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal<1 || now-t->us>100000 || t->w<6.0f || t->w>10.0f || t->h<200.0f) continue;
        *rx=t->x; *ry=t->y; *rw=t->w; *rh=t->h; *item=t->item; found=1;
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* The first visible row of a list (found by watching the list item in game): the int at item+0x238 (0 at the top, one more each
 * time the highlight goes past the last visible row), with a scroll animation (item+0x23c a flag, item+0x240 a float that runs
 * from 0 and rests at 1.0). Writing the row and resting the animation moves the list at once without touching the highlight. */
static int list_top_read(uint64_t item) {
    int32_t v=0;
    return safe_read(&v,item+0x238,4) && v>=0 && v<4096 ? (int)v : -1;
}
static void list_top_write(uint64_t item, int top) {
    const int32_t v=top, flag=0; const float rest=1.0f;
    safe_write(item+0x238,&v,4); safe_write(item+0x23c,&flag,4); safe_write(item+0x240,&rest,4);
}
static int menu_list_index(uint64_t item) {
    uint64_t model=0; uint32_t idx=0;
    if (!safe_read(&model,item+8,8) || !safe_read(&idx,model+0x14,4) || idx>4096) return -1;
    return (int)idx;
}
/* The menu's own "select this row" (0x1335a0: menu, index): it asks the row if it can be selected, deselects the old one and selects
 * the new, like a press of up or down but at any distance. Nothing happens for a row that cannot be selected. */
static void menu_select_index(uint64_t menu, int index) {
    if (!u1_image || !menu || index<0) return;
    ((guest_fn2)(uintptr_t)(u1_image+0x1335a0))(menu,(uint64_t)(uint32_t)index);
}
static int menu_hover;   /* the cursor is over something that answers a click (the golden arrow lights up) */
static uint32_t menu_mouse_buttons(const bool *k, uint32_t mouse, uint64_t now) {
    menu_hover=0;
    static struct {
        int active, target, presses, click_pending, click_action, last_left, drag, queue, last_dir, last_sel, last_target, last_press, list_active, list_click, list_presses,
            sb_drag, sb_measure, sb_same, sb_last, sb_n, sb_cmp, direct_tries, list_armed, jump_fail, dlg_sel;
        uint32_t jump_done_at_arm;
        int press_dir, freeze_on, freeze_top, sb_m_armed, sb_m_fallback; uint32_t sb_m_done; uint64_t press_until, freeze_item, freeze_end, sb_m_until, sb_m_deadline;
        uint64_t sb_item, sb_cached_item, list_hold, dlg_seen, dlg_cross_at;
        float sb_y, sb_h;
        float last_x, last_y;
        /* the slider being dragged: track x and width (UI space), number of steps, pointer to its value when the
         * game keeps it as a float (0..100 or 0..1), the position we put it at (-1: not known yet) */
        float sl_x, sl_w; int sl_steps, sl_est, sl_extra_max; uint64_t sl_vp;
        uint64_t menu, click_until, click_deadline, step_until, next_step, list_deadline, list_last_step;
        int step_dir;
    } h;
    uint32_t out=0;
    float mx=0, my=0;
    SDL_Window *w=SDL_GetMouseFocus();
    int ww=0, wh=0;
    if (!w) return 0;
    SDL_GetMouseState(&mx,&my);
    SDL_GetWindowSize(w,&ww,&wh);
    if (ww<=0 || wh<=0) return 0;
    const float scale=fminf((float)ww/UI_WIDTH,(float)wh/UI_HEIGHT);
    const float ux=(mx-((float)ww-UI_WIDTH*scale)*0.5f)/scale, uy=(my-((float)wh-UI_HEIGHT*scale)*0.5f)/scale;
    const int moved=fabsf(mx-h.last_x)>=1.0f || fabsf(my-h.last_y)>=1.0f;
    h.last_x=mx; h.last_y=my;
    if (moved && !h.drag) { h.active=1; h.presses=0; }
    if (now<wheel_until_us[0] || now<wheel_until_us[1] ||
        (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN]))) h.active=0;
    const int left=(mouse & 1u)!=0, clicked=left && !h.last_left;
    h.last_left=left;
    if (!left) { h.drag=0; h.queue=0; }
    /* A confirmation (Oui / Non): the cursor over a choice selects it (once, from where the selection is believed to be: "Non" when
     * the box opens), a click selects it and then presses cross, the wheel and the keys go left and right. Nothing else on the
     * screen reacts to the mouse there (a click elsewhere in the box must not validate the selected choice). */
    {
        DialogInfo dlg;
        if (menu_dialog_find(now,&dlg)) {
            if (now-h.dlg_seen>300000) { h.dlg_sel=2; h.dlg_cross_at=0; h.step_until=0; h.click_until=0; h.click_pending=0; h.queue=0; }
            h.dlg_seen=now;
            int which=0, pressed=0;
            if (ux>=dlg.lx-12.0f && ux<=dlg.lx+dlg.lw+12.0f && uy>=dlg.ly-12.0f && uy<=dlg.ly+dlg.lh+12.0f) which=1;
            else if (ux>=dlg.rx-12.0f && ux<=dlg.rx+dlg.rw+12.0f && uy>=dlg.ry-12.0f && uy<=dlg.ry+dlg.rh+12.0f) which=2;
            if (k) {
                if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) h.dlg_sel=1;
                if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) h.dlg_sel=2;
            }
            if (now<wheel_until_us[0]) h.dlg_sel=1; else if (now<wheel_until_us[1]) h.dlg_sel=2;
            if (which) menu_hover=1;
            if (which && (moved || clicked) && which!=h.dlg_sel) { h.step_dir=which; h.step_until=now+100000; h.dlg_sel=which; pressed=1; }
            if (clicked && which) {
                h.dlg_cross_at=now+(pressed ? 260000u : 30000u);
                printf("Runtime: menu confirmation click on the %s choice\n",which==1 ? "left" : "right");
            }
            if (h.dlg_cross_at && now>=h.dlg_cross_at) { h.dlg_cross_at=0; h.click_until=now+120000; }
            uint32_t o=0;
            if (now<h.step_until) o|=h.step_dir==1 ? BTN_LEFT : BTN_RIGHT;
            if (now<h.click_until) o|=BTN_CROSS;
            return o;
        }
    }
    /* The scrollbar of a list: press on the rail and drag. The first time on a list its length is measured (down to the end:
     * the selected row stops growing or wraps), then the selected row follows the cursor along the rail. */
    int sb_out=0;
    {
        float rx, ry, rw, rh; uint64_t ritem=0;
        const int have_rail=menu_rail_find(now,&rx,&ry,&rw,&rh,&ritem);
        if (!left) h.sb_drag=0;
        if (have_rail && ux>=rx-10.0f && ux<=rx+rw+10.0f && uy>=ry-4.0f && uy<=ry+rh+4.0f) menu_hover=1;
        if (have_rail && clicked && ux>=rx-10.0f && ux<=rx+rw+10.0f && uy>=ry-4.0f && uy<=ry+rh+4.0f && menu_list_index(ritem)>=0) {
            h.sb_drag=1; h.sb_item=ritem; h.sb_y=ry; h.sb_h=rh; h.sb_cmp=0; h.sb_same=0; h.sb_last=menu_list_index(ritem);
            h.sb_measure=(h.sb_cached_item!=ritem);
            if (h.sb_measure) h.sb_n=0;
            h.sb_m_armed=0; h.sb_m_fallback=0;
            printf("Runtime: scrollbar drag on list item %#llx, rows %s\n",(unsigned long long)ritem,h.sb_measure ? "to measure" : "known");
        }
        if (h.sb_drag) {
            const int idx=menu_list_index(h.sb_item);
            if (idx<0) h.sb_drag=0;
            else if (h.sb_measure && !h.sb_m_fallback) {
                /* one press carries the whole measure (see u1_menu_event); no result in half a second: the slow way below */
                uint64_t lmenu=0;
                if (!h.sb_m_armed) {
                    if (safe_read(&lmenu,h.sb_item+0x80,8) && lmenu) {
                        u1_measure_n=0; u1_jump_menu=lmenu; u1_jump_type=2; u1_jump_us=now; u1_measure_item=h.sb_item;
                        h.sb_m_armed=1; h.sb_m_done=u1_measure_done; h.sb_m_until=now+50000; h.sb_m_deadline=now+500000;
                    } else h.sb_m_fallback=1;
                } else if (u1_measure_done!=h.sb_m_done) {
                    if (u1_measure_n>0) { h.sb_n=u1_measure_n; h.sb_measure=0; h.sb_cached_item=h.sb_item; printf("Runtime: scrollbar: the list has %d rows (measured in one go)\n",h.sb_n); }
                    else h.sb_m_fallback=1;
                } else if (now>h.sb_m_deadline) { u1_measure_item=0; h.sb_m_fallback=1; }
                if (h.sb_measure && !h.sb_m_fallback && now<h.sb_m_until) sb_out=BTN_DOWN;
            }
            else if (h.sb_measure) {
                /* down until the row stops growing or wraps round to the top */
                if (idx>h.sb_last && idx!=h.sb_last) { h.sb_same=0; h.sb_last=idx; }
                else if (idx<h.sb_last) { h.sb_n=h.sb_last+1; h.sb_measure=0; h.sb_cached_item=h.sb_item; }
                else if (++h.sb_same>24) { h.sb_n=idx+1; h.sb_measure=0; h.sb_cached_item=h.sb_item; }
                if (!h.sb_measure) printf("Runtime: scrollbar: the list has %d rows\n",h.sb_n);
                else if ((now/16000u)&1u) sb_out=BTN_DOWN;
            }
            if (!h.sb_measure && h.sb_drag) {
                float f=(uy-h.sb_y)/h.sb_h; f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
                ListInfo li;
                if (menu_list_find_ex(now,&li,0) && li.pitch>1.0f && list_top_read(h.sb_item)>=0) {   /* the highlighted row may be out of view: not needed here */
                    /* the bar scrolls the view (first visible row), the highlight stays where it is */
                    const int visible=(int)floorf((li.bottom-li.top)/li.pitch+0.5f);
                    const int max_top=h.sb_n-visible>0 ? h.sb_n-visible : 0;
                    const int want=(int)floorf(f*(float)max_top+0.5f);
                    if (list_top_read(h.sb_item)!=want) list_top_write(h.sb_item,want);
                } else {
                const int target=(int)floorf(f*(float)(h.sb_n>1 ? h.sb_n-1 : 1)+0.5f);
                if (idx!=target) {
                    const int dir=idx<target ? 1 : -1;
                    if (h.sb_cmp!=0 && dir!=h.sb_cmp) h.sb_cmp=0;   /* the cursor moved to the other side: go the other way */
                    h.sb_cmp=dir;
                    if ((now/16000u)&1u) sb_out=dir>0 ? BTN_DOWN : BTN_UP;
                }
                }
            }
        }
    }
    /* A scrolling list: the cursor over its rows moves the highlight there (up or down presses until the highlighted row
     * is at the cursor), a click then validates. */
    ListInfo list={0};
    int list_ctl=0, list_delta=0;
    int list_found=menu_list_find_ex(now,&list,0);
    if (list_found && list.sel_cy<0.0f) {   /* the highlighted row is out of view (scrolled away with the bar): where it would be */
        const int top=list_top_read(list.item), idx=menu_list_index(list.item);
        if (top>=0 && idx>=0) list.sel_cy=list.top+12.6f+(float)(idx-top)*list.pitch; else list_found=0;
    }
    if (list_found && ux>=list.colx-20.0f && ux<=list.colx+560.0f && uy>=list.top-4.0f && uy<=list.bottom+4.0f) {
        list_ctl=1;
        list_delta=(int)floorf((uy-list.sel_cy)/list.pitch+0.5f);
    }
    uint64_t menu=0; int index=-1, count=0;
    const int on_row=list_ctl ? 0 : menu_row_at(ux,uy,now,&menu,&index,&count);
    MenuControl ctl={0,0,-1,0,0,0};
    const int on_control=!h.drag && !list_ctl && menu_control_at(ux,uy,now,&ctl);
    if (list_ctl || on_row || on_control) menu_hover=1;
    if (list_ctl) {
        if (moved) { h.list_active=1; h.list_presses=0; }
        if (now<wheel_until_us[0] || now<wheel_until_us[1] ||
            (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN]))) h.list_active=0;
        if (clicked) { h.list_click=1; h.list_deadline=now+1500000; h.list_active=1; h.list_presses=0; }
    } else h.list_click=0;
    /* Hovering a row at the edge of a list must not scroll it: the first row shown is noted when the cursor starts to move the
     * highlight, and put back whenever the game moves it meanwhile (until a moment after the highlight is under the cursor, or the
     * wheel or the keys take over). */
    if (list.item) {
        const int top=list_top_read(list.item);
        const int user_scroll=now<wheel_until_us[0] || now<wheel_until_us[1] ||
            (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN]));
        if (user_scroll || h.sb_drag || (h.freeze_on && h.freeze_item!=list.item)) h.freeze_on=0;
        if (!h.freeze_on && list_ctl && (h.list_active || h.list_click) && list_delta!=0 && top>=0 && !user_scroll && !h.sb_drag) {
            h.freeze_on=1; h.freeze_top=top; h.freeze_item=list.item; h.freeze_end=0;
        }
        if (h.freeze_on) {
            if (top>=0 && top!=h.freeze_top) {
                list_top_write(list.item,h.freeze_top);
                printf("Runtime: list view kept at row %d (the game moved it to %d)\n",h.freeze_top,top);
            }
            if (list_delta==0) { if (!h.freeze_end) h.freeze_end=now+400000; } else h.freeze_end=0;
            if (h.freeze_end && now>h.freeze_end) h.freeze_on=0;
        }
    } else h.freeze_on=0;
    if (clicked) {
        int texts, rows;
        menu_texts_visible(now,&texts,&rows);
        if (mouse_debug()) printf("Runtime: menu click ui=(%.0f %.0f) on_row=%d control=%d texts=%d rows=%d\n",ux,uy,on_row,on_control ? ctl.kind : 0,texts,rows);
        if (mouse_debug() && texts<=4) {   /* a confirmation box has few texts: list them (to tell it from the other screens) */
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<menu_text_count;++i) { const MenuText *t=&menu_texts[i]; if (now-t->us<100000) printf("Runtime:   text item ord=%d x=%.1f y=%.1f w=%.1f h=%.1f\n",t->ordinal,t->x,t->y,t->w,t->h); }
            for (int i=0;i<free_text_count;++i) { const MenuText *t=&free_texts[i]; if (now-t->us<100000) printf("Runtime:   text free x=%.1f y=%.1f w=%.1f h=%.1f\n",t->x,t->y,t->w,t->h); }
            pthread_mutex_unlock(&menu_text_lock);
        }
        if (h.sb_drag) {
            /* the press is on a scrollbar: nothing else takes it (the selected row's sub-menu must not open) */
        } else if (on_control) {
            h.target=ctl.index; h.menu=ctl.menu; h.click_pending=1; h.click_deadline=now+1500000; h.active=1; h.presses=0;
            h.click_action=ctl.kind;
            if (ctl.kind==3) {
                uint64_t model=0, vp=0, steps=0;
                h.drag=1; h.sl_x=ctl.tx; h.sl_w=ctl.tw; h.sl_steps=10; h.sl_est=-1; h.sl_vp=0; h.queue=0;
                if (safe_read(&model,ctl.item+8,8) && safe_read(&vp,model+0x18,8)) {
                    if (vp>=0x1000000ull) h.sl_vp=vp;               /* a float behind a pointer (brightness, sensitivity) */
                    else if (vp>=2 && vp<=64) {                       /* the volumes: the number of steps; the four values are floats 0..1 */
                        uint32_t slot=99;                               /* in an array of the data segment (1.02), in the order of the rows */
                        h.sl_steps=(int)vp;
                        if (u1_image && safe_read(&slot,model+0x14,4) && slot<4) h.sl_vp=u1_image+0xcc3db8u+4u*slot;
                    }
                }
                (void)steps;
                printf("Runtime: menu slider start steps=%d value ptr=%#llx track=(%.1f %.1f)\n",h.sl_steps,(unsigned long long)h.sl_vp,h.sl_x,h.sl_w);
            }
        } else if (on_row) {
            h.target=index; h.menu=menu; h.click_pending=1; h.click_action=0; h.click_deadline=now+1500000; h.active=1; h.presses=0;
        } else if (rows==0 && !list_ctl) h.click_until=now+150000;   /* no menu rows: "press a key to start", the game selection */
    }
    if (on_row && h.active && !h.click_pending && !h.drag) { h.target=index; h.menu=menu; }
    if (!on_row && !on_control && !h.click_pending && !h.drag) h.menu=0;
    int arrived=0;
    if (h.target!=h.last_target) { h.last_target=h.target; h.last_dir=0; h.last_press=0; h.last_sel=-1; h.presses=0; h.direct_tries=0; }
    if (h.menu && (h.active || h.click_pending || h.drag)) {
        int32_t sel=-1;
        if (safe_read(&sel,h.menu+0x150,4) && sel>=0) {
            if (sel==h.target) {
                h.presses=0; arrived=1; h.last_dir=0; h.last_press=0; h.direct_tries=0;
                if (h.click_pending) {
                    h.click_pending=0;
                    if (h.click_action==0) h.click_until=now+120000;
                    else if (h.click_action==1 || h.click_action==2) { h.step_dir=h.click_action; h.step_until=now+120000; }
                }
            } else if ((h.last_dir!=0 && ((sel<h.target) ? 1 : -1)!=h.last_dir && sel!=h.last_sel) ||
                       (h.last_press!=0 && sel!=h.last_sel && (sel-h.last_sel)*h.last_press<0)) {
                /* the selection went past the row without stopping on it, or wrapped round the other way (a row at the
                 * start or the end of the menu): the game does not select this row */
                menu_skip(h.menu,h.target,now);
                h.active=0; h.click_pending=0; h.drag=0; h.presses=0; h.last_dir=0; h.menu=0;
            } else if (h.presses<24 && u1_image && h.direct_tries<4) {
                menu_select_index(h.menu,h.target);   /* straight to the row; a row that does not take it falls back to the presses below */
                ++h.direct_tries;
            } else if (h.presses<24 && ((now/50000u)&1u)) {
                h.last_dir=(sel<h.target) ? 1 : -1; h.last_sel=sel; h.last_press=h.last_dir;
                out|=(sel<h.target) ? BTN_DOWN : BTN_UP;
                static uint64_t last_step;
                if (now-last_step>90000) { last_step=now; ++h.presses; }
            } else if (h.presses>=24) { h.active=0; h.click_pending=0; h.drag=0; }
        }
        if (h.click_pending && now>h.click_deadline) h.click_pending=0;
    }
    /* A slider dragged with the left button held: the bar goes where the cursor is. The cursor's place on the track
     * gives the wanted step (0..steps). When the game keeps the value as a float (brightness 0..100, sensitivity
     * 0..1) it is read and the difference is pressed; otherwise (the volumes) the bar is first taken to the left end
     * with steps+1 presses, then the wanted number of right presses is added, and later moves press the
     * difference from where we put it. */
    if (h.drag && arrived && h.sl_w>1.0f) {
        float fraction=(ux-h.sl_x)/h.sl_w;
        fraction=fraction<0.0f ? 0.0f : fraction>1.0f ? 1.0f : fraction;
        const int want=(int)floorf(fraction*(float)h.sl_steps+0.5f);
        int est;
        if (h.sl_vp) {
            float v=0.0f;
            if (safe_read(&v,h.sl_vp,4)) {
                const float range=v>1.0001f ? 100.0f : 1.0f;
                est=(int)floorf(v/range*(float)h.sl_steps+0.5f)+h.queue;
            } else est=want;
        } else {
            if (h.sl_est<0) { h.queue-=h.sl_steps+1; h.sl_est=0; }
            est=h.sl_est+h.queue+(h.sl_steps+1 > 0 ? 0 : 0);
            est=h.sl_est;
        }
        if (h.sl_vp) h.queue+=want-est;
        else {
            h.queue+=want-h.sl_est; h.sl_est=want;
            if (want==h.sl_steps && h.sl_extra_max==0) { h.queue+=4; h.sl_extra_max=1; }   /* a press may be lost: be sure of the end */
            if (want!=h.sl_steps) h.sl_extra_max=0;
        }
        if (h.queue>60) h.queue=60;
        if (h.queue<-60) h.queue=-60;
    }    if (h.queue!=0 && now>=h.step_until && now>=h.next_step) {
        h.step_dir=h.queue>0 ? 2 : 1;
        h.step_until=now+35000; h.next_step=now+70000;
        h.queue+=h.queue>0 ? -1 : 1;
    }
    if (now<h.step_until) out|=h.step_dir==1 ? BTN_LEFT : BTN_RIGHT;
    out|=(uint32_t)sb_out;
    if (!h.sb_drag && list_ctl && (h.list_active || h.list_click)) {
        if (list_delta==0) {
            h.list_presses=0; u1_jump_steps=0; h.list_armed=0;
            if (h.list_click) { h.list_click=0; h.click_until=now+120000; }
        } else if (h.list_presses<40 && ((now/16000u)&1u) && now>=h.list_hold) {
            /* more than one row away: the first press carries the other steps (see u1_menu_event); until the highlight is seen
             * to have moved, no further press is sent. A jump that never happens (3 times) leaves the plain presses. */
            const int distance=list_delta<0 ? -list_delta : list_delta;
            uint64_t lmenu=0;
            if (h.jump_fail>=3 && now-h.list_hold>3000000) h.jump_fail=0;   /* give the jump another chance later */
            if (distance>1 && h.jump_fail<3 && list.item && safe_read(&lmenu,list.item+0x80,8) && lmenu) {
                if (h.list_armed) {
                    if (u1_jump_done==h.jump_done_at_arm) {
                        ++h.jump_fail;
                        if (mouse_debug()) printf("Runtime: list jump not taken (%d): armed menu %#llx, last event menu %#llx, last non-zero event type %d (%d seen), wanted %d, steps left %d\n",h.jump_fail,(unsigned long long)u1_jump_menu,(unsigned long long)u1_event_last_menu,u1_event_last_type,u1_event_count,u1_jump_type,u1_jump_steps);
                    } else h.jump_fail=0;
                }
                h.jump_done_at_arm=u1_jump_done; h.list_armed=1;
                u1_jump_menu=lmenu; u1_jump_type=list_delta>0 ? 2 : 1; u1_jump_us=now;
                u1_jump_steps=(distance>24 ? 24 : distance)-1;
                h.list_hold=now+160000;
                h.press_until=now+50000; h.press_dir=list_delta>0 ? 1 : -1;   /* the press is held long enough for the game to see it */
            }
            out|=list_delta>0 ? BTN_DOWN : BTN_UP;
            if (now-h.list_last_step>90000) { h.list_last_step=now; ++h.list_presses; }
        } else if (h.list_presses>=40) { h.list_active=0; h.list_click=0; }
        if (h.list_click && now>h.list_deadline) h.list_click=0;
    }
    if (now<h.press_until && list_ctl && list_delta!=0) out|=h.press_dir>0 ? BTN_DOWN : BTN_UP;
    if (now<h.click_until) out|=BTN_CROSS;
    return out;
}
/* U1 1.02: the player (the pointer at image+0xaa5f78 points to it, valid while its int at +100 equals the one at
 * image+0xaa5f80) keeps its current mode in a record: player+0x70 -> +0x10 -> record whose int at +0x18 is the mode's id.
 * The "Jetski" mode record (image+0xaa1e28, set up the first time the game puts the player on the jetski) holds its id
 * at +0x18 (image+0xaa1e40). Elena is on the jetski when the two are equal. */
static int u1_on_jetski(void) {
    if (!u1_image) return 0;
    static int last=-1;
    int on=0;
    uint64_t holder=0, player=0, a=0, b=0;
    int sid=0, check=0, want=0, mode=0;
    if (safe_read(&sid,u1_image+0xaa1e40,4) && sid!=0 &&
        safe_read(&holder,u1_image+0xaa5f78,8) && safe_read(&player,holder,8) && player>0x10000 &&
        safe_read(&check,player+100,4) && safe_read(&want,u1_image+0xaa5f80,4) && check==want &&
        safe_read(&a,player+0x70,8) && a>0x10000 && safe_read(&b,a+0x10,8) && b>0x10000 &&
        safe_read(&mode,b+0x18,4))
        on=mode==sid;
    if (on!=last) {
        last=on;
        bbgpu_jetski_state(on); /* the icons of the hints show the jetski's keys */
        if (mouse_debug()) printf("Mouse: jetski %s (mode %#x, jetski id %#x) t=%.4f\n",on ? "on" : "off",(unsigned)mode,(unsigned)sid,now_us()*1e-6);
    }
    return on;
}
static void sample_host(PadData *d) {
    host_rotate=host_sideways=0;
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    static int loaded;
    if (!loaded) { load_input(); loaded=1; }
    SDL_Gamepad *g=current_gamepad();
    float mdx=0, mdy=0;
    uint32_t mouse=0;
    int wheel=0, captured=bbgpu_mouse_sample(&mdx,&mdy,&mouse,&wheel);
    /* The window tells which device was used last (keyboard / mouse, PlayStation pad, Xbox pad). With a controller, everything made for
     * the PC (keys, mouse look, menu mouse, walk key, the keyboard row) is out: the game gets what a PS4 pad gives. */
    const int pad_only=g && bbgpu_input_mode()!=0;
    { static int was_pad_only; if (was_pad_only && !pad_only && g) SDL_RumbleGamepad(g,0,0,0); was_pad_only=pad_only; } /* back to keyboard / mouse: stop shaking */
    pad_only_mode=pad_only;
    if (pad_only) { mdx=mdy=0; mouse=0; wheel=0; captured=0; }
    if (bbgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    /* Scripted runs (BB_PAD_FILE) ignore the player's controller and keyboard: a pad in use on
     * the same PC reached the test window (BB_PAD_HOST=1 keeps them). */
    static int scripted=-1;
    if (scripted<0) {
        const char *file=getenv("BB_PAD_FILE"), *host=getenv("BB_PAD_HOST");
        scripted=file && *file && !(host && host[0]=='1');
    }
    if (scripted) return;
    const uint64_t now=d->timestamp;
    if (wheel>0) wheel_until_us[0]=now+80000;
    if (wheel<0) wheel_until_us[1]=now+80000;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) && !pad_only ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        for (int a=0;a<16;++a) {
            const int b=input.pad[a];
            if (b>=0 && SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) d->buttons|=actions[a].ps;
            else if (b==PAD_TRIGGER_LEFT || b==PAD_TRIGGER_RIGHT) {
                const uint8_t v=trigger(SDL_GetGamepadAxis(g,b==PAD_TRIGGER_LEFT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                              : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
                if (v>30) d->buttons|=actions[a].ps;
                if (actions[a].ps==BTN_L2 && v>d->l2) d->l2=v;
                if (actions[a].ps==BTN_R2 && v>d->r2) d->r2=v;
            }
        }
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_TOUCHPAD)) d->buttons|=BTN_TOUCHPAD; /* DualSense/DS4 click */
        if ((d->buttons & BTN_L2) && !d->l2) d->l2=255;
        if ((d->buttons & BTN_R2) && !d->r2) d->r2=255;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        if (input.pad_invert_y) d->right_y=(uint8_t)(255-d->right_y);
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* The device used last picks the button prompts: the game's PlayStation buttons or pictures
     * of the bound keys (bbgpu_prompts_controller). A stick counts past half way, the mouse when
     * it moves more than a few pixels. */
    const int pad_used=g && (d->buttons || d->l2>60 || d->r2>60 || abs(d->left_x-128)>64 ||
                             abs(d->left_y-128)>64 || abs(d->right_x-128)>64 || abs(d->right_y-128)>64);
    int keys_used=mouse || wheel || (captured && SDL_fabsf(mdx)+SDL_fabsf(mdy)>3.0f);
    for (int a=0;a<A_OVERLAY && !keys_used;++a) keys_used=action_down(a,k,mouse,now);
    static int prompts_device=-1;
    if (pad_used!=keys_used) {
        const int device=pad_used ? prompts_pad_device(g) : 0;
        if (device!=prompts_device) {
            prompts_device=device;
            bbgpu_prompts_controller(device);
        }
    }
    /* Keyboard and mouse, together with the controller. */
    menu_update(k,mouse);
    for (int a=0;a<16;++a) if (key_action_down(a,k,mouse,now)) d->buttons|=actions[a].ps;
    if (u1_image && u1_camera_dt_offset && !menu_mode && !pad_only) d->buttons&=~BTN_DOWN; /* U1 in play: no photo mode (a controller gets its d-pad as it is) */
    if (menu_mode && !menu_guard && !pad_only) d->buttons|=menu_mouse_buttons(k,mouse,now);
    else menu_hover=0;
    bbgpu_set_cursor(menu_mode && !pad_only ? ((mouse & 1u) ? 2 : menu_hover ? 1 : 0) : 0);
    if (menu_mode && now-u1_compare_us<250000) d->buttons&=~BTN_CROSS;   /* statistics: no "compare with friends" */
    if (menu_mode && !pad_only) {   /* the row "Touches clavier / souris" of the Commandes menu: the cross (Enter, click) opens the bindings window */
        static int cross_was_down;
        int selected=0;
        const uint64_t view=u1_keys_row_view, menu=u1_keys_row_menu;
        int32_t sel=-1; uint64_t item=0;
        if (view && menu && safe_read(&sel,menu+0x150,4) && sel>=0 && sel<64 && safe_read(&item,menu+0xa0+8u*(uint64_t)sel,8) && item==view) {
            /* the freed menu keeps its old contents: only a row drawn in the last 100 ms (the screen is on) counts */
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<menu_text_count;++i) if (menu_texts[i].item==view && now-menu_texts[i].us<100000) { selected=1; break; }
            pthread_mutex_unlock(&menu_text_lock);
        }
        static int selected_before;   /* the row must have been selected before the press: the cross that confirmed the language only just rebuilt the menu */
        if (selected) {
            const int cross=(d->buttons & BTN_CROSS)!=0;
            if (cross && !cross_was_down && selected_before) {
                printf("Runtime: keys window requested: buttons=%#x mouse=%#x enter=%d lclick=%d\n",d->buttons,mouse,k ? (int)(k[SDL_SCANCODE_RETURN]||k[SDL_SCANCODE_KP_ENTER]) : -1,(int)(mouse&1u));
                bbgpu_open_controls();
            }
            cross_was_down=cross;
            d->buttons&=~(BTN_CROSS|BTN_LEFT|BTN_RIGHT);
        } else cross_was_down=(d->buttons & BTN_CROSS)!=0;
        selected_before=selected;
    }
    if (bbgpu_controls_changed()) { load_input(); printf("Runtime: key bindings read again\n"); }
    if (menu_mode) { /* diagnosis of the menu buttons (Alt did not open the options of the game selection) */
        static uint32_t last_buttons=~0u;
        if (d->buttons!=last_buttons) { last_buttons=d->buttons; printf("Runtime: menu buttons %#x\n",d->buttons); }
    }
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    // The touchpad button without a finger on a touch surface is a left-side click.
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    const int lx=action_down(A_LS_RIGHT,k,mouse,now)-action_down(A_LS_LEFT,k,mouse,now);
    int ly=action_down(A_LS_DOWN,k,mouse,now)-action_down(A_LS_UP,k,mouse,now);
    if (lx) d->left_x=(uint8_t)(lx<0 ? 0 : 255);
    if (u1_on_jetski()) {
        /* on the jetski, forward / back (the keys of the left stick's up / down) are the throttle (R1) and the brake (Square) */
        if (action_down(A_LS_UP,k,mouse,now)) d->buttons|=BTN_R1;
        if (action_down(A_LS_DOWN,k,mouse,now)) d->buttons|=BTN_SQUARE;
        ly=0;
    }
    if (ly) d->left_y=(uint8_t)(ly<0 ? 0 : 255);
    /* The walk key (configurable, none by default) held with the movement keys, outside the menus: the stick is pushed only part way */
    if (!menu_mode && (lx||ly) && action_down(A_WALK,k,mouse,now)) {
        /* stick deflection in percent of full (BB_WALK_STICK to tune): below the game's dead zone Drake stops (36 % did) */
        static int percent;
        if (!percent) { const char *e=getenv("BB_WALK_STICK"); percent=e && atoi(e)>0 && atoi(e)<=100 ? atoi(e) : 55; }
        const int amp=(percent*127/100)*((lx && ly) ? 707 : 1000)/1000; /* diagonal: same length as a straight push */
        if (lx) d->left_x=(uint8_t)(128+lx*amp);
        if (ly) d->left_y=(uint8_t)(128+ly*amp);
    }
    const int rx=action_down(A_RS_RIGHT,k,mouse,now)-action_down(A_RS_LEFT,k,mouse,now);
    const int ry=action_down(A_RS_DOWN,k,mouse,now)-action_down(A_RS_UP,k,mouse,now);
    if (rx) d->right_x=(uint8_t)(rx<0 ? 0 : 255);
    if (ry) d->right_y=(uint8_t)(ry<0 ? 0 : 255);
    /* Where the game's camera or joypad state takes the mouse directly (their callbacks ran in the
     * last 100 ms), the motion goes there: the follow camera turns with the mouse (vertically too where
     * its arc is known), with the right stick held a little the same way (follow_push); aiming, other
     * cameras and a follow camera that keeps no turns get the mouse as a right stick value in the joypad
     * state (all three games). Where a build's hooks were not found, the mouse is a right stick byte here. */
    pthread_mutex_lock(&camera_lock);
    const int following=camera_hook_us && now-camera_hook_us<100000;
    const int joypad=sticks_hook_us && now-sticks_hook_us<100000;
    for (int q=0;q<CAMERA_QUEUES;++q) {
        /* queue 2 is the zoom camera's (U1 aiming): nothing else reads it, so it stays empty outside the aim */
        if (captured && (following || joypad) && (q!=2 || aim_held || look_held)) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    /* gyro aiming: kept while aiming (at low frame rates the aim controller runs only every 100-200 ms) */
    const int raw_aiming=raw_aim_armed(now);
    if (captured && raw_aiming) { aim_dx+=mdx; aim_dy+=mdy; }
    else { aim_dx=aim_dy=0.0f; }
    pthread_mutex_unlock(&camera_lock);
    const int direct_x=joypad || (following && !aim_held && !look_held && follow_direct);
    const int direct_y=joypad || (following && !aim_held && !look_held && follow_direct && follow_has_arc);
    if (captured && direct_x && direct_y) {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    } else if (captured) {
        float dt=mouse_last_us ? (float)(now-mouse_last_us)*1e-6f : 0.0f;
        mouse_last_us=now;
        if (dt>0.0f) {
            if (dt>0.1f) dt=0.1f;
            /* turn rate in degrees/s, smoothed over 12 ms (mouse reports arrive in bursts) */
            const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
            mouse_vx+=alpha*(mdx/dt*scale-mouse_vx);
            mouse_vy+=alpha*((input.mouse_invert_y ? -mdy : mdy)/dt*scale-mouse_vy);
            if (mdx==0 && mouse_vx>-1.0f && mouse_vx<1.0f) mouse_vx=0;
            if (mdy==0 && mouse_vy>-1.0f && mouse_vy<1.0f) mouse_vy=0;
        }
        if (mouse_vx!=0 && !rx && !direct_x) d->right_x=mouse_axis(mouse_vx);
        if (mouse_vy!=0 && !ry && !direct_y) d->right_y=mouse_axis(mouse_vy);
    } else {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    }
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; touch_ul touch_ur touch=X,Y click with the finger at that point; lx= ly= rx= ry= (0..255) override
 * the sticks; rot_ccw rot_cw turn the left stick round (the rotate keys). An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; float mouse[2]; int touch_x, touch_y; int rotate; } injected={0,{-1,-1,-1,-1},-1,{0,0},-1,-1,0};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    injected.touch_x=injected.touch_y=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    injected.mouse[0]=injected.mouse[1]=0.0f;
    injected.rotate=0;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"rot_ccw")) injected.rotate=-1; /* the rotate keys, scripted */
        if (!strcmp(token,"rot_cw")) injected.rotate=1;
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        /* touch=X,Y (0..1919, 0..942) / touch_ul / touch_ur: a click with the finger there (the games read the pad's zones: upper left / right) */
        if (!strcmp(token,"touch_ul") || !strcmp(token,"touch_ur")) { injected.touch_x=!strcmp(token,"touch_ur") ? 1440 : 480; injected.touch_y=100; }
        if (!strncmp(token,"touch=",6)) { int x,y; if (sscanf(token+6,"%d,%d",&x,&y)==2) { injected.touch_x=x; injected.touch_y=y; } }
        if (injected.touch_x>=0) injected.buttons|=BTN_TOUCHPAD;
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        if (!strncmp(token,"mx=",3)) injected.mouse[0]=(float)atof(token+3); /* mouse motion, pixels/s */
        if (!strncmp(token,"my=",3)) injected.mouse[1]=(float)atof(token+3);
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d mouse %g %g (t=%.4f)\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3],injected.mouse[0],
           injected.mouse[1],now*1e-6);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    read_inject();
    /* scripted mouse motion (mx= my=) goes where the player's mouse would */
    static uint64_t mouse_inject_us;
    const uint64_t inject_now=now_us();
    if ((injected.mouse[0]!=0.0f || injected.mouse[1]!=0.0f) && mouse_inject_us) {
        float dt=(float)(inject_now-mouse_inject_us)*1e-6f;
        if (dt>0.5f) dt=0.5f; /* a loaded PC polls the pad rarely: the scripted motion is not lost */
        pthread_mutex_lock(&camera_lock);
        for (int q=0;q<CAMERA_QUEUES;++q) { camera_dx[q]+=injected.mouse[0]*dt; camera_dy[q]+=injected.mouse[1]*dt; }
        pthread_mutex_unlock(&camera_lock);
    }
    mouse_inject_us=inject_now;
    replay_sample(d);
    d->buttons|=injected.buttons;
    /* L2 aims; L1 (grenade) aims with the same zoom camera, but only once that camera runs (L1 without a grenade does nothing) */
    aim_held=(d->buttons & BTN_L2)!=0 || ((d->buttons & BTN_L1) && zoom_camera_us && now_us()-zoom_camera_us<100000);
    look_held=(d->buttons & (BTN_R3|BTN_UP))!=0; /* U1: the d-pad up (V) looks around */
    if (!aim_held) { aim_target_total=0.0f; aim_session_reset=1; }
    if (mouse_debug()) { /* the pad the game gets, when it changes (jetski: which buttons stop it) */
        static uint32_t last_buttons=~0u; static uint8_t last_l2, last_r2, last_ly=128;
        if (d->buttons!=last_buttons || d->l2!=last_l2 || d->r2!=last_r2 || (d->left_y>128+30)!=(last_ly>128+30) || (d->left_y<128-30)!=(last_ly<128-30)) {
            last_buttons=d->buttons; last_l2=d->l2; last_r2=d->r2; last_ly=d->left_y;
            printf("Mouse: pad buttons=%#x l2=%u r2=%u ly=%u lx=%u t=%.4f\n",d->buttons,d->l2,d->r2,d->left_y,d->left_x,now_us()*1e-6);
        }
    }
    if (mouse_debug()) {
        static int was_look=-1;
        if (was_look!=look_held) { was_look=look_held; printf("Mouse: R3 look %s t=%.4f\n",look_held ? "down" : "up",now_us()*1e-6); }
    }
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    {
        int dir=injected.rotate ? injected.rotate : host_rotate;
        const uint64_t at=now_us();
        /* At a wheel: left / right (keys, or the pad-file lx= at an end) and the rotate keys turn
         * the stick the way the wheel wants (u1_gesture_query). */
        if (wheel_asked_us && at-wheel_asked_us<300000 &&
            (dir || host_sideways || injected.stick[0]==0 || injected.stick[0]==255)) dir=wheel_dir;
        rotate_stick(d,dir,at);
    }
    /* Builds without a joypad callback: the right stick held a little while the mouse turns the follow
     * camera's direction (follow_push, see follow_camera), unless the stick is in use */
    const uint64_t now=now_us();
    if (!pad_only_mode && follow_push!=0.0f && camera_hook_us && now-camera_hook_us<100000 &&
        !(sticks_hook_us && now-sticks_hook_us<100000) && !aim_held && !look_held && d->right_x>=118 && d->right_x<=138)
        d->right_x=(uint8_t)(128.0f+follow_push*127.5f);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    /* Only the controller in use shakes: with the keyboard and mouse as the last device, the pads lying around stay still. */
    if (g && bbgpu_input_mode()!=0) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    else if (g) SDL_RumbleGamepad(g,0,0,0);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Light bar {r, g, b}: shown on controllers that have one (DualShock 4, DualSense). */
static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    pthread_mutex_lock(&lock);
    SDL_Gamepad *pad=current_gamepad();
    if (pad) SDL_SetGamepadLED(pad,r,g,b);
    pthread_mutex_unlock(&lock);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!rgb) return ERR_INVALID_ARG;
    set_led(rgb[0],rgb[1],rgb[2]);
    return 0;
}
static ABI int32_t pad_reset_light_bar(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    set_led(0,0,255); /* user 1 colour */
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }

/* A loader command used by the launcher before the game starts: no Vulkan or guest memory. */
int runtime_pad_tool(int test, int strength) {
    const int already_ready=SDL_WasInit(SDL_INIT_GAMEPAD)!=0;
    if (!already_ready && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr,"Controller detection failed: %s\n",SDL_GetError());
        return 1;
    }
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    SDL_Gamepad *pad=ids && count>0 ? SDL_OpenGamepad(ids[0]) : NULL;
    SDL_free(ids);
    if (!pad) {
        puts("connected=0\nrumble=0");
        if (!already_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return 0;
    }
    const char *name=SDL_GetGamepadName(pad);
    const bool rumble=SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                             SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false);
    puts("connected=1");
    printf("name=%s\n",name ? name : "Game controller");
    printf("rumble=%d\n",rumble ? 1 : 0);
    if (test) {
        if (strength<0) strength=0;
        if (strength>100) strength=100;
        const uint16_t amount=(uint16_t)((65535u*(unsigned)strength+50u)/100u);
        const bool started=rumble && SDL_RumbleGamepad(pad,amount,amount,500);
        if (started) SDL_Delay(500);
        SDL_RumbleGamepad(pad,0,0,0);
        printf("tested=%d\n",started ? 1 : 0);
    }
    SDL_CloseGamepad(pad);
    if (!already_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return 0;
}
