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
enum { BIND_KEY=1, BIND_MOUSE, BIND_WHEEL };
typedef struct { uint8_t kind; int16_t code; } Bind;
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

static Bind parse_bind(const char *name) {
    static const struct { const char *name; uint8_t kind; int16_t code; } mouse[]={
        {"Mouse Left",BIND_MOUSE,1}, {"Mouse Right",BIND_MOUSE,2}, {"Mouse Middle",BIND_MOUSE,4},
        {"Mouse 4",BIND_MOUSE,8}, {"Mouse 5",BIND_MOUSE,16}, {"Wheel Up",BIND_WHEEL,1}, {"Wheel Down",BIND_WHEEL,-1},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i)
        if (!SDL_strcasecmp(name,mouse[i].name)) return (Bind){mouse[i].kind,mouse[i].code};
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? (Bind){0,0} : (Bind){BIND_KEY,(int16_t)code};
}
static void set_keys(int action, const char *value) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    input.keys[action][0]=input.keys[action][1]=(Bind){0,0};
    char *second=strchr(text,'|');
    if (second) *second++=0;
    if (*text) input.keys[action][0]=parse_bind(text);
    if (second && *second) input.keys[action][1]=parse_bind(second);
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
static volatile int u1_rail_wheel;   /* U1: a list with a scrollbar is on screen: the wheel scrolls its view (the bar's thumb) instead of moving the highlight */
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
    case BIND_MOUSE: return (mouse & (uint32_t)b->code)!=0;
    case BIND_WHEEL: return now<wheel_until_us[b->code>0 ? 0 : 1];
    default: return 0;
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
#define COVER_RECENT_US 30000 /* the cover camera counts as running for this long after its last frame (a few frames: when its blend-out ends the mouse goes back to the walking camera at once) */
static volatile float cover_dir_x, cover_dir_z; /* the cover camera's direction (x z), kept for the follow camera */
static volatile float cover_last_step;          /* the turn (degrees) the cover camera was given this frame, and when: the camera manager's hook adds the rest while it blends in */
static volatile uint64_t cover_last_us;
static volatile float cover_out_yaw;          /* the cover camera's output yaw (degrees) of this frame, and when: the walking camera's output is made the same */
static volatile uint64_t cover_out_us;
static volatile float cover_pivot_x, cover_pivot_z;   /* the cover camera's pivot (where the player is in the cover) */
static float orbit_r=3.0f, orbit_s=1.0f;              /* the walking camera's distance to the player and on which side of its yaw it sits (measured during a cover) */
static volatile uint64_t zoom_camera_us;  /* last frame the zoom (aiming) camera was updated */
static volatile uint64_t u2_zoom_top_us;   /* U2: the zoom camera runs on top of the stack (the aim, but also the whole of the truck turret sequence, with the aim button released) */
static volatile uint64_t u2_zoom_below_cover_us;
static volatile uint64_t u2_look_seen_us, u2_zoom_below_look_us;
static volatile uint64_t u2_follow_seen_us, u2_zoom_under_us; /* U2: the walking camera was updated just before the zoom camera = the zoom camera is under it, blending out (L2 held in a roll): not an aim */
#define U2_FOLLOW_RESYNC_US 150000u /* the walking camera away for longer than this = an animation held the picture: no catch-up afterwards (its normal call interval measured 30 to 60 ms in the climbing sequences) */
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
 * CUSA02343 1.00 and others with the same code; U2 and U3 have the same function, found by its
 * instructions): its horizontal direction (x, y, z) at +0x1310 (U2 +0x9ec0, U3 +0x78c0) and the
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
static volatile uint64_t aim_blend_until_us; /* the aim is blended out for about a second after L2 is released: the zoom camera keeps the mouse until then */
static volatile int aim_native;   /* aim_held and the zoom (aiming) camera runs: L2 held during a roll aims nothing yet, and the mouse must keep turning the free view then */
static volatile int aim_held;             /* L2 is held: the aiming camera (also while it blends in) */
static volatile uint64_t look_blend_until_us; /* the look-around camera (V) is blended out for about a second after the key is released: it keeps the mouse until then */
static volatile int look_held;            /* R3 is held: first-person look around, which turns like the aim */
static float follow_target;               /* wanted heading (degrees) */
static uint64_t follow_moved_us;          /* last frame the mouse moved */
static uint64_t follow_tilt_us;           /* last frame the mouse moved vertically (follow_moved_us is the turn) */
static float follow_prev_total;           /* degrees the previous frame turned the direction by (0 after a gap, a pause or the joypad route) */
static volatile float follow_correction;  /* degrees the direction is short of the wanted heading */
static volatile float follow_push;        /* the right stick while the direction turns (-0.3, 0, 0.3) */
static volatile int follow_has_arc;       /* the follow camera's vertical arc is known */
static float follow_applied;              /* degrees the last follow_camera() call turned the direction by (0 if it did not) */
static int follow_arc_moved;              /* the last follow_camera() call changed the vertical arc *//* arc_degrees: the camera pitch one unit of arc covers (U1 61; measured in U2 about 135, in U3 about 105, steeper upwards) */
static uint32_t follow_gap_us;            /* time between the last two follow_camera() calls: long after an animation held the picture */
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, float arc_degrees, int stopped, uint32_t resync_gap_us) {
    const uint64_t now=now_us();
    follow_applied=0.0f;
    follow_arc_moved=0;
    follow_has_arc=arc_offset!=0;
    if (camera_hook_us && now-camera_hook_us>100000) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        follow_moved_us=0;
    }
    camera_hook_us=now;
    float *dir=(float *)(uintptr_t)(camera+dir_offset);
    const float heading=heading_of(dir);
    {   /* BB_U1_WALKPROBE=<file>: the camera object's first 0x1330 bytes at every call (time, address, bytes), to find the fields that beat when the camera trembles in depth */
        static FILE *probe_f; static int probe_init; static unsigned probe_n;
        if (!probe_init) { probe_init=1; const char *p=getenv("BB_U1_WALKPROBE"); if (p && *p) probe_f=fopen(p,"wb"); }
        if (probe_f && probe_n<12000) {
            const double pt=now*1e-6; const uint64_t pc=camera;
            fwrite(&pt,8,1,probe_f); fwrite(&pc,8,1,probe_f); fwrite((const void *)(uintptr_t)camera,1,0x1330,probe_f);
            if ((++probe_n&63)==0) fflush(probe_f);
        }
    }
    /* The walking camera was away for a few frames (grab, vault, ledge drop, knock-out: the animation camera holds the
     * picture alone). The game's own camera turned meanwhile; the wanted heading it left behind would then be made up at
     * 4 degrees a frame, which swung the picture just after every animation. After such a gap nothing is made up: the
     * wanted heading is taken from where the camera is now, and the mouse's turn made during the gap is dropped. */
    static uint64_t follow_call_us;
    const uint32_t gap_us=(uint32_t)(follow_call_us && now>follow_call_us ? now-follow_call_us : 0);
    follow_gap_us=gap_us;
    if (mouse_debug() && resync_gap_us) { /* the walking camera's rhythm: what is a normal interval, what is an animation gap? */
        static uint64_t hist_us; static unsigned h[6];
        ++h[gap_us<10000?0:gap_us<20000?1:gap_us<35000?2:gap_us<60000?3:gap_us<150000?4:5];
        if (now-hist_us>10000000) {
            hist_us=now;
            printf("Mouse: follow intervals <10 10-20 20-35 35-60 60-150 >150 ms: %u %u %u %u %u %u\n",h[0],h[1],h[2],h[3],h[4],h[5]);
            h[0]=h[1]=h[2]=h[3]=h[4]=h[5]=0;
        }
    }
    /* The walking camera was away for the animation (grab, vault, ledge drop, knock-out): the animation camera holds the
     * picture alone and the game's own camera turns meanwhile. The wanted heading it leaves behind would then be made up at
     * 4 degrees a frame, which swung the picture just after every animation. After such a gap nothing is made up: the wanted
     * heading is taken from where the camera is now, and the mouse's turn made during the gap is dropped. */
    if (resync_gap_us && gap_us>resync_gap_us) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        follow_moved_us=0; follow_pending=0; follow_push=0.0f; follow_correction=0.0f; follow_target=heading; follow_prev_total=0.0f;
        if (mouse_debug()) printf("Mouse: follow resync after a %.0f ms gap (heading %.2f) t=%.4f\n",gap_us*1e-3,(double)heading,now*1e-6);
    }
    follow_call_us=now;
    float dx, dy;
    camera_take(0,&dx,&dy);
    if (dy!=0.0f) follow_tilt_us=now;
    /* While a cover camera runs the walking camera is kept at the cover camera's yaw by the same turns it makes for a mouse turn (a few degrees
     * a frame, with the push that makes the game keep them): the game puts the old direction back every frame otherwise, and the picture jumped
     * there when the cover ended. Measured: writing the direction does not hold. The mouse goes to the cover camera meanwhile. */
    const int cover_follow=!aim_native && !look_held &&
        ((cover_native_us && now-cover_native_us<COVER_RECENT_US) || (cover_out_us && now-cover_out_us<COVER_RECENT_US));
    if (cover_follow) {
        const float dd[3]={-cover_dir_x,0.0f,-cover_dir_z};
        follow_target=(cover_out_us && now-cover_out_us<COVER_RECENT_US) ? cover_out_yaw : heading_of(dd);
        follow_moved_us=now;
        follow_direct=1; follow_kept=0.5f;   /* the turns of the direct mode with the push are the ones the game keeps (in the joypad mode it pulled the direction back at once) */
        dx=0.0f; dy=0.0f;
    }
    if (aim_native || look_held) { follow_moved_us=0; follow_pending=0; follow_push=0.0f; follow_prev_total=0.0f; return; } /* the aiming / look camera takes the mouse */
    if (stopped) { /* paused (or a frame time out of range): the mouse is taken but not applied, the camera goes on as the game wants */
        follow_moved_us=0; follow_pending=0; follow_push=0.0f; follow_correction=0.0f; follow_prev_total=0.0f;
        if (mouse_debug()) { static unsigned n; if ((n++&31)==0) printf("Mouse: u2 follow camera %#llx stopped (dt out of range) t=%.4f\n",(unsigned long long)camera,now*1e-6); }
        return;
    }
    if (follow_pending) {
        float kept=wrap_degrees(heading-follow_before)/wrap_degrees(follow_after-follow_before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        follow_kept+=0.2f*(kept-follow_kept);
        const int was_direct=follow_direct, kept_pct=(int)(kept*100.0f);
        if (follow_kept<0.12f) follow_direct=0;
        else if (follow_kept>0.3f) follow_direct=1;
        if (mouse_debug() && was_direct!=follow_direct) /* the mouse changes route: a jolt here is a bug or a sequence whose camera keeps no turns */
            printf("Mouse: u2 follow %s (was %s) kept=%d%% avg=%.2f t=%.4f\n",follow_direct?"DIRECT":"JOYPAD (emulated right stick)",
                   was_direct?"direct":"joypad",kept_pct,follow_kept,now*1e-6);
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
    /* Mouse still: a shortfall under 0.3 degree is left alone (making it up flipped the push and the turn every frame for 0.15 s after a stop: a slight tremble) */
    if (turn==0.0f && correction>-0.3f && correction<0.3f) correction=0.0f;
    /* Only what the game undid of our own last turn is made up: it cannot be more than that turn. Against a wall the game turns the direction away by
     * itself (3 to 6 degrees a frame); a small mouse move kept the rest of the 0.5 s chase going against it and the picture trembled. That turn is accepted. */
    {
        const float cap=SDL_fabsf(follow_prev_total)+0.1f;
        if (follow_direct && (correction>cap || correction<-cap)) {
            correction=correction>0.0f ? cap : -cap;
            follow_target=wrap_degrees(heading-correction);
        }
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
    const float catch_up=(cover_follow || (cover_out_us && now-cover_out_us<150000)) ? 45.0f : 4.0f;   /* (while a cover runs: the cover camera turns faster than a mouse turn is made up) */
    if (correction<-catch_up) correction=-catch_up;
    if (correction>catch_up) correction=catch_up;
    const float total=turn+correction;
    follow_prev_total=total;
    follow_applied=total;
    follow_push=total>0.05f ? 0.3f : total<-0.05f ? -0.3f : 0.0f;
    if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f turn=%.2f corr=%.2f%s total=%.2f cam=%llx\n",
                              now*1e-6,dx,heading,shown,follow_target,turn,correction,
                              (correction>8.0f || correction<-8.0f) ? " CATCHUP" : "",total,(unsigned long long)camera);
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
        float v=*arc+(input.mouse_invert_y ? -dy : dy)*(MOUSE_DEGREES_PER_PIXEL/arc_degrees)*input.mouse_sensitivity;
        *arc=v<0.0f ? 0.0f : v>1.0f ? 1.0f : v;
        follow_arc_moved=1;
    }
}
static uint32_t u1_camera_dt_offset;  /* set by the callbacks of the versions whose time step is known */
static uint64_t u1_image;             /* U1 image base, seen by the pad update callback */
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    follow_camera(camera,0x1310,0x12bc,61.0f,camera_stopped(image,u1_camera_dt_offset),0);
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
    /* The quick spring only while the mouse is in use (and 0.6 s after): kept all the time it passed every swing of the game's camera near a wall
     * to the picture (a tremble when walking towards the camera against a wall); the game's own spring damps it. */
    const uint64_t spring_now=now_us();
    const int mouse_recent=follow_moved_us && spring_now>=follow_moved_us && spring_now-follow_moved_us<600000;
    static int parts=-1;   /* BB_SPRING_PARTS (test): 1 = the two global constants, 2 = the camera shape's speed; default 3 = both */
    if (parts<0) { const char *e=getenv("BB_SPRING_PARTS"); parts=e ? atoi(e) : 0; }
    /* Lag-free turning while the mouse is in use. Raising the spring's speed made the camera beat by +-0.4 m to and from the player against a wall
     * (measured with BB_U1_WALKPROBE); putting the whole state on the target removed the smoothing of the wall's pushes too. BB_SPRING_RIGID=0 turns it off. */
    {
        static int rigid=-1;
        if (rigid<0) { const char *e=getenv("BB_SPRING_RIGID"); rigid=!(e && atoi(e)==0); }
        /* only while the mouse moves (0.15 s after): longer, the player's small moves against a wall showed unsmoothed after the mouse had stopped */
        /* each axis on its own: the heading is made up while the mouse turns, the height while it tilts. Making up the height during a turn too
         * passed the wall's small up and down pushes to the picture (the tremble came back). */
        const int turning=follow_moved_us && spring_now>=follow_moved_us && spring_now-follow_moved_us<150000;
        const int tilting=follow_tilt_us && spring_now>=follow_tilt_us && spring_now-follow_tilt_us<150000;
        if (rigid && !input.free_view_smoothing && input.mouse_look && !pad_only_mode && (turning || tilting)) {
            /* What the data say (BB_U1_WALKPROBE + BB_CAMERA_LOG): +0x1230 is the camera's wanted position, which turns about the player's point
             * (+0x12f0) at a fixed radius (3.65 m) when the mouse turns it and is pushed towards the player by a wall; +0x1250 is the position
             * the game's spring shows, +0x1240 its velocity. The spring's lag behind a turn is what made the mouse feel smoothed; its lag in the
             * radius is what absorbs the wall pushing the camera in and out. So only the angle is made up: the state is turned about the player's
             * point (and its velocity with it) to the wanted position's angle; radius and height are left to the game's spring. */
            float t[3], s[3], v[3], p[3];
            if (safe_read(t,camera+0x1230,12) && safe_read(s,camera+0x1250,12) && safe_read(v,camera+0x1240,12) && safe_read(p,camera+0x12f0,12)) {
                int ok=1;
                for (int i=0;i<3;++i) ok&=(t[i]>-5000.0f && t[i]<5000.0f && s[i]>-5000.0f && s[i]<5000.0f && p[i]>-5000.0f && p[i]<5000.0f && v[i]>-500.0f && v[i]<500.0f);
                const float tx=t[0]-p[0], ty=t[1]-p[1], tz=t[2]-p[2], sx=s[0]-p[0], sy=s[1]-p[1], sz=s[2]-p[2];
                const float tr2=tx*tx+ty*ty+tz*tz, sr2=sx*sx+sy*sy+sz*sz;
                if (ok && tr2>0.09f && tr2<400.0f && sr2>0.09f && sr2<400.0f) {   /* beyond ~20 m or under 0.3 m it is a cut: left alone */
                    /* the state takes the wanted position's heading (while turning) and elevation (while tilting) about the player's point, and keeps its own distance */
                    const float tl=SDL_sqrtf(tr2), sl=SDL_sqrtf(sr2);
                    const float th=SDL_sqrtf(tx*tx+tz*tz), sh=SDL_sqrtf(sx*sx+sz*sz);
                    float saz=SDL_atan2f(sz,sx), taz=SDL_atan2f(tz,tx);
                    const float sel=SDL_atan2f(sy,sh), tel=SDL_atan2f(ty,th);
                    float daz=turning && th>0.1f && sh>0.1f ? taz-saz : 0.0f;
                    daz=daz>3.14159265f ? daz-6.2831853f : daz<-3.14159265f ? daz+6.2831853f : daz;
                    const float del=tilting ? tel-sel : 0.0f;
                    if (daz>-1.5f && daz<1.5f && del>-0.8f && del<0.8f && (daz!=0.0f || del!=0.0f)) {   /* larger is a cut */
                        const float naz=saz+daz, nel=sel+del, ch=SDL_cosf(nel);
                        const float ns[3]={p[0]+sl*ch*SDL_cosf(naz), p[1]+sl*SDL_sinf(nel), p[2]+sl*ch*SDL_sinf(naz)};
                        const float c=SDL_cosf(daz), sn=SDL_sinf(daz);   /* the velocity is turned in heading only */
                        const float nv[3]={v[0]*c-v[2]*sn, v[1], v[0]*sn+v[2]*c};
                        safe_write(camera+0x1250,ns,12);
                        safe_write(camera+0x1240,nv,12);
                        if (mouse_debug()) { static unsigned n; if ((n++&63)==0) printf("Mouse: u1 spring turned %.2f tilted %.2f deg (distance state %.2f wanted %.2f)\n",daz*57.29578f,del*57.29578f,sl,tl); }
                    }
                }
            }
        }
    }
    u1b_free_view_constants(image,!input.free_view_smoothing && input.mouse_look && !pad_only_mode && mouse_recent && (parts&1),speed);
    uint64_t now_shape=0;
    if (!safe_read(&now_shape,camera+0x1e0,8) || now_shape<0x10000) return;
    if (now_shape!=shape) {   /* another camera object or shape: the old one gets its value back */
        if (raised && shape) safe_write(shape+0x74,&original,4);
        shape=now_shape; raised=0;
    }
    float current=0.0f;
    if (!safe_read(&current,shape+0x74,4)) return;
    const int want=!input.free_view_smoothing && input.mouse_look && !pad_only_mode && mouse_recent && (parts&2);
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
    follow_camera(camera,0x1310,0x12bc,61.0f,camera_stopped(image,U1B_CAMERA_DT),0);
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,135.0f,0,U2_FOLLOW_RESYNC_US);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,105.0f,0,0);
}
static uint64_t saved_register(int n);
static int safe_read(void *dst, uint64_t src, size_t size);
static int safe_write(uint64_t dst, const void *src, size_t size);
/* U2 of CUSA02320 (the collection): the camera manager (image+0x4371c0) calls each camera of its stack as update(rdi output block,
 * rsi camera, rdx input block), the update being the slot +0xe0 of the camera's vtable. The hook sits just before that call (image+
 * 0x4374cc; the thunk saved r8 = the update, r10 = the camera; the manager's frame gives the blocks: rbp = rsp + 0x240, input block at
 * rbp-0x160, output block at rbp-0xd0). The input block holds the previous output (position +0x00, orientation quaternion x y z w at
 * +0x10), the pad's sticks as floats lx ly rx ry at +0x54 and again at +0x64. Which update runs in which situation (measured with
 * BB_MOUSE_DEBUG): walking 0x42d940, V (look around) 0x432450, aiming and grenade 0x452c50, cover 0x417450. */
#define U2B_UPDATE_FOLLOW 0x42d940
#define U2B_UPDATE_LOOK   0x432450
#define U2B_UPDATE_ZOOM   0x452c50
#define U2B_UPDATE_COVER  0x417450
static volatile uint64_t u2_native_us;    /* last frame a U2 camera was turned by the mouse (cover, look around) */
/* the game's frame time (seconds; 0 while paused): float at [[image+0x1161ee8]+0x70]+0x14 */
static float u2_frame_dt(uint64_t image) {
    uint64_t holder=0, clock=0;
    float dt=0.0f;
    if (!safe_read(&holder,image+0x1161ee8,8) || holder<0x10000 || !safe_read(&clock,holder+0x70,8) || clock<0x10000 ||
        !safe_read(&dt,clock+0x14,4)) return 1.0f/60.0f;
    return dt;
}
/* The mouse's turn for this frame, spread over a few frames (about 20 ms behind the mouse), like the U1 cameras. The same camera
 * can be updated twice in one frame: the second call gets the same step. Returns 0 while paused. */
typedef struct { float pending_yaw, pending_pitch, frame_yaw, frame_pitch; uint64_t frame_us, frame_camera; } U2Turn;
static float aim_follow(float dt, float pending_yaw, float pending_pitch);
/* smooth: the smoothing time constant in seconds, 0 for none, negative for the dynamic smoothing of the aiming cameras (aim_follow) */
static int u2_mouse_turn(U2Turn *t, int queue, uint64_t camera, float dt, float scale, float smooth, float *yaw, float *pitch) {
    const uint64_t now=now_us();
    if (t->frame_us && now-t->frame_us<1500 && t->frame_camera==camera) { *yaw=t->frame_yaw; *pitch=t->frame_pitch; return 1; }
    if (u2_native_us && now-u2_native_us>100000) t->pending_yaw=t->pending_pitch=0.0f;
    float dx, dy;
    camera_take(queue,&dx,&dy);
    if (!(dt>0.0f)) { t->pending_yaw=t->pending_pitch=0.0f; return 0; }
    t->pending_yaw+=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*scale;
    t->pending_pitch+=(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*scale;
    if (!(dt>0.0005f && dt<0.05f)) dt=1.0f/60.0f;
    const float follow=smooth<0.0f ? aim_follow(dt,t->pending_yaw,t->pending_pitch) : smooth>0.0f ? 1.0f-SDL_expf(-dt/smooth) : 1.0f;
    *yaw=t->pending_yaw*follow; *pitch=t->pending_pitch*follow;
    t->pending_yaw-=*yaw; t->pending_pitch-=*pitch;
    t->frame_us=now; t->frame_camera=camera; t->frame_yaw=*yaw; t->frame_pitch=*pitch;
    return 1;
}
static void quat_mul(const float *a, const float *b, float *o) { /* quaternions x y z w, o = a * b */
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
/* Cover camera (update image+0x417450): it turns its direction (x y z w at camera+0x1c0) about the vertical by stick x * a speed
 * that ramps up (camera+0x1f0), and lowers its pitch (degrees, camera+0x1b0, limits -[shape+0xc]..[shape+8], shape = *(camera+0x1290))
 * by stick y; the mouse turns the two values directly just before, as in U1. */
static U2Turn u2_cover_turn;
static uint64_t u2_cover_seen_us, u2_cover_seen_camera;   /* (debug) the last cover camera update and its direction, to compare with where an aim starts */
static float u2_cover_seen_dir[3], u2_cover_seen_pitch;
static void u2_cover_camera(uint64_t camera, uint64_t image) {
    {
        const float *sd=(const float *)(uintptr_t)(camera+0x1c0);
        u2_cover_seen_us=now_us(); u2_cover_seen_camera=camera;
        u2_cover_seen_dir[0]=sd[0]; u2_cover_seen_dir[1]=sd[1]; u2_cover_seen_dir[2]=sd[2];
        u2_cover_seen_pitch=*(const float *)(uintptr_t)(camera+0x1b0);
    }
    if (aim_native || look_held) return;   /* (L2 held without the aiming camera running, in a roll or between two covers, is not an aim) */
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_cover_turn,3,camera,u2_frame_dt(image),1.0f,0.0f,&step,&step_pitch)) return;
    u2_native_us=now_us();
    float *dir=(float *)(uintptr_t)(camera+0x1c0);
    float *pitch=(float *)(uintptr_t)(camera+0x1b0);
    uint64_t shape=0;
    safe_read(&shape,camera+0x1290,8);
    { /* a camera that has just been made (the cover camera of the next spot) does not hold usable numbers yet: leave it alone */
        const float *pos=(const float *)(uintptr_t)(camera+0x1d0), *pivot=(const float *)(uintptr_t)(camera+0x230);
        const float sum=dir[0]+dir[1]+dir[2]+*pitch+pos[0]+pos[1]+pos[2]+pivot[0]+pivot[1]+pivot[2];
        if (!(sum>-1e6f && sum<1e6f) || shape<0x10000) return;
    }
    if (mouse_debug()) {
        static int shown;
        if (shown<5 && shape>0x10000) {
            ++shown;
            printf("Mouse: u2 cover camera %#llx dir=(%.3f %.3f %.3f %.3f) pitch=%.3f limits=-%.3f..%.3f speed=%.3f\n",(unsigned long long)camera,
                   dir[0],dir[1],dir[2],dir[3],*pitch,*(const float *)(uintptr_t)(shape+0xc),*(const float *)(uintptr_t)(shape+8),
                   *(const float *)(uintptr_t)(shape+0x30));
        }
    }
    /* While the cover is blended in (its weight w, the float at stack entry + 0x34, stays at 0 for a fifth of a second, then rises to 1 in about a second) the
     * picture is w x this camera + (1-w) x the walking camera, which the game holds still: a turn of this camera shows only w times. So the state is turned by the
     * mouse's whole turn since the cover began divided by w (at least 0.2): the picture then follows the mouse, and the state ends up where the mouse put it. */
    {
        /* two cover cameras are updated together when one cover leads to another: one slot per camera */
        static struct { uint64_t camera, seen_us, start_us, frame_us; float total_yaw, total_pitch, done_yaw, done_pitch; int entering; } slot[2];
        const uint64_t now=now_us();
        int si=-1;
        for (int i=0;i<2;++i) if (slot[i].camera==camera && now-slot[i].seen_us<300000) si=i;
        float w=1.0f; const uint64_t entry=saved_register(11);
        if (entry>0x10000) { float f; if (safe_read(&f,entry+0x34,4) && f>=0.0f && f<=1.0f) w=f; }
        if (si<0) {   /* a cover camera seen for the first time (or after a pause) */
            si=slot[0].seen_us<=slot[1].seen_us ? 0 : 1;
            memset(&slot[si],0,sizeof(slot[si]));
            slot[si].camera=camera; slot[si].start_us=now;
            slot[si].entering=w<0.05f;   /* (a camera already in the mix, blending out, is not entering) */
            step=step_pitch=0.0f;   /* (the first step holds all the mouse's motion since the walking began: the queue is only emptied by a cover camera) */
        }
        slot[si].seen_us=now;
        float *total_yaw=&slot[si].total_yaw, *total_pitch=&slot[si].total_pitch;
        if (u2_cover_turn.frame_us!=slot[si].frame_us) { slot[si].frame_us=u2_cover_turn.frame_us; *total_yaw+=step; *total_pitch+=step_pitch; }
        if (w>=0.999f) slot[si].entering=0;
        if (slot[si].entering) {   /* the game's weight stays at 0 for 0.2 s: the cover is brought in from the first frame, linearly in 0.35 s, until the game's own weight (S curve) has caught up */
            float ramp=(float)(now-slot[si].start_us)*(1.0f/350000.0f); ramp=ramp>1.0f ? 1.0f : ramp;
            if (ramp>w) { w=ramp; if (entry>0x10000) { safe_write(entry+0x34,&w,4); safe_write(entry+0x38,&w,4); } }
        }
        const float k=slot[si].entering && w<0.999f ? 1.0f/(w<0.5f ? 0.5f : w) : 1.0f;   /* (at most twice: the game blends the directions, a larger turn of the state goes round the circle) */
        const float want_yaw=*total_yaw*k, want_pitch=*total_pitch*k;
        step=want_yaw-slot[si].done_yaw; slot[si].done_yaw=want_yaw;
        step_pitch=want_pitch-slot[si].done_pitch; slot[si].done_pitch=want_pitch;
        if (mouse_debug() && slot[si].entering) printf("Mouse: u2 coverblend cam %#llx w=%.3f k=%.2f total=%.2f applied=%.2f\n",(unsigned long long)camera,w,k,*total_yaw,want_yaw);
    }
    if (step==0.0f && step_pitch==0.0f) return;
    if (step!=0.0f) {
        const float a=step*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        /* the camera's position (x y z w at +0x1d0, a spring) follows its target, which turns with the direction, about the
         * pivot (+0x230), a few frames late: it is turned the same way so that there is nothing left to catch up */
        float *pos=(float *)(uintptr_t)(camera+0x1d0);
        const float *pivot=(const float *)(uintptr_t)(camera+0x230);
        const float px=pos[0]-pivot[0], pz=pos[2]-pivot[2];
        pos[0]=pivot[0]+px*c-pz*s;
        pos[2]=pivot[2]+px*s+pz*c;
    }
    if (step_pitch!=0.0f && shape>0x10000) {
        const float v=*pitch-step_pitch, lo=-*(const float *)(uintptr_t)(shape+0xc), hi=*(const float *)(uintptr_t)(shape+8);
        *pitch=v<lo ? lo : v>hi ? hi : v;
    }
    if (mouse_debug()) {
        static unsigned n;
        const float *pos=(const float *)(uintptr_t)(camera+0x1d0), *pivot=(const float *)(uintptr_t)(camera+0x230);
        if ((n++&15)==0) printf("Mouse: u2 coverspring pos-pivot=(%.3f %.3f %.3f) pivot=(%.3f %.3f %.3f) dir=(%.3f %.3f %.3f) pitch=%.3f\n",
                                pos[0]-pivot[0],pos[1]-pivot[1],pos[2]-pivot[2],pivot[0],pivot[1],pivot[2],dir[0],dir[1],dir[2],*pitch);
    }
    if (mouse_debug()) printf("Mouse: u2 coverturn t=%.4f step=%.3f pitch step=%.3f pitch=%.3f\n",now_us()*1e-6,step,step_pitch,*pitch);
}
/* Look-around camera (V, update image+0x432450): its yaw and pitch (degrees) come from the previous orientation quaternion, input
 * block +0x10 (Euler order yaw about the vertical, then pitch about the camera's side: yaw = atan2(2(zx+wy), 1-2(x2+y2)), pitch =
 * asin(2(wx-yz)), as image+0xb2cb70 reads them); it lowers the yaw by stick x and raises the pitch by stick y, then limits the pitch.
 * The mouse turns the quaternion it is given: yaw about the vertical on the left, pitch about the side on the right. */
static U2Turn u2_look_turn;
static void u2_look_camera(uint64_t camera, uint64_t image) {
    if (!look_held && now_us()>=look_blend_until_us) return;   /* (blended out for about a second after the key is released: it keeps the mouse) */
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_look_turn,2,camera,u2_frame_dt(image),input.mouse_aim_sensitivity,-1.0f,&step,&step_pitch)) return;
    u2_native_us=now_us();
    if (step==0.0f && step_pitch==0.0f) return;
    float *q=(float *)(uintptr_t)(guest_callback_rsp+0xe0+0x10);
    float yaw[4]={0.0f,SDL_sinf(-step*0.5f*0.017453293f),0.0f,SDL_cosf(-step*0.5f*0.017453293f)};
    float pitch[4]={SDL_sinf(step_pitch*0.5f*0.017453293f),0.0f,0.0f,SDL_cosf(step_pitch*0.5f*0.017453293f)};
    float turned[4], result[4];
    quat_mul(yaw,q,turned);
    quat_mul(turned,pitch,result);
    const float n=SDL_sqrtf(result[0]*result[0]+result[1]*result[1]+result[2]*result[2]+result[3]*result[3]);
    if (!(n>0.5f)) return;
    for (int i=0;i<4;++i) q[i]=result[i]/n;
    if (mouse_debug()) printf("Mouse: u2 lookturn t=%.4f step=%.3f pitch step=%.3f q=(%.3f %.3f %.3f %.3f)\n",now_us()*1e-6,step,step_pitch,q[0],q[1],q[2],q[3]);
}
/* Chapter 5 truck chase (update image+0x4496b0, found with the "EMULATED stick" monitoring). The camera lets the player look around a little: a yaw and a pitch offset
 * (degrees, floats at camera+0x188 and +0x18c) that a filtered stick (floats +0x180 / +0x184) drives. Measured on 1000 frames of the chase: yaw offset += 0.46 x
 * filtered stick x per frame and 1.7 % of it goes back to zero each frame; the picture turns 0.88 degrees per degree of offset; the range seen is about -15..+15 degrees.
 * The mouse sets the offsets directly: the target is kept here and written before each update (a little above, to make up for the 1.7 % the update takes back),
 * so the look holds where the mouse leaves it. BB_U2_CHASE=0 turns this off (the mouse goes through the stick again). */
static U2Turn u2_chase_turn;
static volatile uint64_t u2_chase_us;   /* last frame the chase camera was updated (its mouse queue, 2, is filled meanwhile) */
static void u2_chase_camera(uint64_t camera, uint64_t image) {
    static int mode=-1;
    if (mode<0) { const char *e=getenv("BB_U2_CHASE"); mode=e && atoi(e)==0 ? 0 : 1; }
    if (!mode || aim_native || look_held) return;
    u2_chase_us=now_us();
    {   /* (debug) the window passage turns the camera's state (+0xb4) from 0x14 to 0x15 and the look no longer moves the picture; holding 0x14 did not free it */
        if (mouse_debug()) {   /* the state of every chase camera object, when it changes */
            static uint64_t seen[4]; static uint32_t seen_st[4]; static int ns;
            uint32_t st=0; int k=-1;
            if (safe_read(&st,camera+0xb4,4)) {
                for (int i=0;i<ns;++i) if (seen[i]==camera) k=i;
                if (k<0 && ns<4) { k=ns++; seen[k]=camera; seen_st[k]=~st; }
                if (k>=0 && seen_st[k]!=st) { printf("Mouse: u2 chase %#llx state +0xb4 %#x -> %#x t=%.4f\n",(unsigned long long)camera,(unsigned)(seen_st[k]==~st ? 0 : seen_st[k]),(unsigned)st,now_us()*1e-6); seen_st[k]=st; }
            }
        }
    }
    float *off=(float *)(uintptr_t)(camera+0x188);
    static uint64_t sess_us, sess_camera; static float target_yaw, target_pitch;
    const uint64_t now=now_us();
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_chase_turn,2,camera,u2_frame_dt(image),input.mouse_aim_sensitivity,-1.0f,&step,&step_pitch)) return;
    if (!(off[0]>-60.0f && off[0]<60.0f && off[1]>-60.0f && off[1]<60.0f)) return;   /* (a camera not set yet) */
    if (camera!=sess_camera || now-sess_us>300000) { sess_camera=camera; target_yaw=off[0]; target_pitch=off[1]; step=step_pitch=0.0f; }
    sess_us=now;
    u2_native_us=now;
    target_yaw-=step/0.88f; target_pitch+=step_pitch/0.88f;
    target_yaw=target_yaw<-15.0f ? -15.0f : target_yaw>15.0f ? 15.0f : target_yaw;
    target_pitch=target_pitch<-13.0f ? -13.0f : target_pitch>13.0f ? 13.0f : target_pitch;
    off[0]=target_yaw/0.983f; off[1]=target_pitch/0.983f;
    if (mouse_debug()) { static unsigned n; if ((n++&15)==0) printf("Mouse: u2 chase %#llx offsets yaw %.3f pitch %.3f (step %.3f %.3f)\n",(unsigned long long)camera,target_yaw,target_pitch,step,step_pitch); }
}/* Zoom camera (aiming with L2, the grenade with L1; update image+0x452c50): the stick turns its aim direction (x y z, unit vector
 * at camera+0x1f30; player-aiming-machine3's UpdateCameraDirection, image+0x71a1b0). Measured by moving the mouse as a stick: the
 * mouse to the right raises atan2(z, x), the mouse down lowers the elevation, as with the U1 aim vector. The mouse turns the vector
 * directly just before the update. Two zoom cameras (one per shoulder) are updated while the shoulder switches: both get the same turn. */
static U2Turn u2_zoom_turn;
static void u2_zoom_camera(uint64_t camera, uint64_t image) {
    if (mouse_debug()) { /* the first frame of an aim: the game's aim vector, the previous output's orientation, and the last cover camera's direction */
        static uint64_t last_us; const uint64_t t=now_us();
        if (t-last_us>300000) {
            const float *d0=(const float *)(uintptr_t)(camera+0x1f30), *q0=(const float *)(uintptr_t)(guest_callback_rsp+0xe0+0x10);
            const float *p0=(const float *)(uintptr_t)(guest_callback_rsp+0xe0);
            printf("Mouse: u2 AIMSTART cam %#llx d=(%.3f %.3f %.3f) angle=%.1f q=(%.3f %.3f %.3f %.3f) pos=(%.2f %.2f %.2f) cover age=%.3fs cam %#llx dir=(%.3f %.3f %.3f) angle=%.1f pitch=%.2f\n",
                   (unsigned long long)camera,d0[0],d0[1],d0[2],SDL_atan2f(d0[2],d0[0])*57.29578f,q0[0],q0[1],q0[2],q0[3],p0[0],p0[1],p0[2],
                   u2_cover_seen_us ? (t-u2_cover_seen_us)*1e-6 : -1.0,(unsigned long long)u2_cover_seen_camera,
                   u2_cover_seen_dir[0],u2_cover_seen_dir[1],u2_cover_seen_dir[2],SDL_atan2f(u2_cover_seen_dir[2],u2_cover_seen_dir[0])*57.29578f,u2_cover_seen_pitch);
        }
        last_us=t;
    }
    /* The first frame of an aim: the mouse motion queued meanwhile (the queue 2 is only emptied by an aiming or looking camera, so it held every turn made
     * in the cover just before: steps of 30 to 70 degrees measured) is thrown away, or the aim leaps away from where the picture was. */
    static uint64_t last_frame_us; int first_frame=0;
    { const uint64_t t=now_us(); first_frame=t-last_frame_us>300000; last_frame_us=t; }
    /* (after the aim the game blends the zoom camera out for about a second: it keeps the mouse meanwhile, or the picture holds still) */
    if ((!aim_held && now_us()>=aim_blend_until_us) || look_held) return;
    if (!aim_held && u2_zoom_below_look_us && now_us()-u2_zoom_below_look_us<100000) return;   /* hidden under the shield's look camera: the emulated stick turns the picture */
    float step, step_pitch;
    if (!u2_mouse_turn(&u2_zoom_turn,2,1,u2_frame_dt(image),input.mouse_aim_sensitivity,-1.0f,&step,&step_pitch)) return;
    u2_native_us=now_us();
    if (first_frame) {
        step=step_pitch=0.0f;
        u2_zoom_turn.pending_yaw=u2_zoom_turn.pending_pitch=u2_zoom_turn.frame_yaw=u2_zoom_turn.frame_pitch=0.0f;   /* (the second shoulder camera of the frame reads these too) */
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
            const float e=SDL_atan2f(d[1],h)*57.29578f;
            float e2=e-step_pitch;
            e2=e2>70.0f ? 70.0f : e2<-70.0f ? -70.0f : e2;
            const float r=e2*0.017453293f, k=SDL_cosf(r)/h;
            d[0]*=k; d[2]*=k; d[1]=SDL_sinf(r);
        }
    }
    if (mouse_debug()) printf("Mouse: u2 aimturn t=%.4f step=%.3f pitch step=%.3f dir=(%.3f %.3f %.3f)\n",now_us()*1e-6,step,step_pitch,d[0],d[1],d[2]);
}
static uint64_t u2_dbg_cams[12]; static int u2_dbg_ncams;   /* (debug) the cameras updated in the last frame that changed the set */

/* ---- Probe for the camera that holds the picture alone during an animation (BB_U2_PROBE=1) --------------
 * During a grab, a vault or a ledge drop the walking camera 0x42d940 leaves the stack and 0x4383d0 keeps the picture;
 * the mouse then goes through the emulated right stick and the animation camera turns at ~100 deg/s at most. To drive it
 * from the mouse like the truck chase camera, its fields must be known. This records the floats of the camera object
 * during each such gap and compares their change frame by frame with the stick the mouse sends: the float whose delta
 * follows the right stick is the yaw offset (the same with the vertical stick for the pitch). Analysis printed when the
 * walking camera comes back. Nothing runs without BB_U2_PROBE. */
#define U2_PROBE_OFFSETS 96
#define U2_PROBE_SAMPLES 400
static float u2_probe_v[U2_PROBE_SAMPLES][U2_PROBE_OFFSETS];
static float u2_probe_sx[U2_PROBE_SAMPLES], u2_probe_sy[U2_PROBE_SAMPLES]; /* the stick byte the game received (128 = centre) */
static unsigned char u2_probe_seg[U2_PROBE_SAMPLES];
static int u2_probe_n, u2_probe_seg_cur;
static uint64_t u2_probe_start, u2_probe_last, u2_probe_camera;
static uint64_t u2_probe_target(void) { /* BB_U2_PROBE=<hex update id> (default the base camera 0x4383d0) */
    static uint64_t id; static int done;
    if (!done) {
        done=1; id=0x4383d0;
        const char *e=getenv("BB_U2_PROBE");
        if (e && SDL_strlen(e)>2) id=(uint64_t)SDL_strtoull(e,NULL,16);
        printf("Mouse: PROBE target %#llx (BB_U2_PROBE=%s)\n",(unsigned long long)id,e ? e : "none");
    }
    return id;
}
/* ---- Snapshots of the cameras' memory (BB_U2_SNAP=1, with BB_MOUSE_DEBUG=1) ---------------------------------
 * The last seconds of the three cameras that matter in the window passage (base 0x4383d0, chase 0x4496b0, walking 0x42d940): at each update, the first
 * 0x300 bytes of the object. F6 writes them to user\camsnap.bin (record: u64 t_us, u64 update id, u64 camera, 192 u32 words) so the locked phase can be
 * compared with the free one offline against the timestamps of the log. */
#define U2_SNAP_WORDS 192
#define U2_SNAP_RING 2400
typedef struct { uint64_t t, id, camera; uint32_t w[U2_SNAP_WORDS]; } U2Snap;
static U2Snap *u2_snap_ring; static unsigned u2_snap_n; static pthread_mutex_t u2_snap_lock=PTHREAD_MUTEX_INITIALIZER;
static int u2_snap_on(void) { static int on=-1; if (on<0) on=mouse_debug() && getenv("BB_U2_SNAP")!=NULL; return on; }
static void u2_snap_frame(uint64_t id, uint64_t camera) {
    if (!u2_snap_on() || camera<0x10000) return;
    pthread_mutex_lock(&u2_snap_lock);
    if (!u2_snap_ring) u2_snap_ring=(U2Snap *)calloc(U2_SNAP_RING,sizeof(U2Snap));
    if (u2_snap_ring) {
        U2Snap *s=&u2_snap_ring[u2_snap_n%U2_SNAP_RING];
        s->t=now_us(); s->id=id; s->camera=camera;
        if (!safe_read(s->w,camera,sizeof(s->w))) memset(s->w,0,sizeof(s->w));
        ++u2_snap_n;
    }
    pthread_mutex_unlock(&u2_snap_lock);
}
static void u2_snap_dump(void) {
    pthread_mutex_lock(&u2_snap_lock);
    if (u2_snap_ring && u2_snap_n) {
        FILE *f=fopen("user\\camsnap.bin","wb");
        if (f) {
            const unsigned n=u2_snap_n<U2_SNAP_RING ? u2_snap_n : U2_SNAP_RING, first=u2_snap_n-n;
            for (unsigned i=0;i<n;++i) fwrite(&u2_snap_ring[(first+i)%U2_SNAP_RING],sizeof(U2Snap),1,f);
            fclose(f);
            printf("Mouse: SNAP dumped %u records to user\\camsnap.bin t=%.4f\n",n,now_us()*1e-6);
        }
    }
    pthread_mutex_unlock(&u2_snap_lock);
}
static uint32_t u2_probe_offset(int i) { return i<48 ? (uint32_t)(i*4) : (uint32_t)(0x140+(i-48)*4); }
static void u2_probe_report(void);
static void u2_probe_frame(uint64_t camera) {
    if (!mouse_debug() || !getenv("BB_U2_PROBE")) return;
    const uint64_t t=now_us();
    const int ax=mouse_axis(mouse_vx), ay=mouse_axis(mouse_vy);
    const int pushed=ax-128>3 || ax-128<-3 || ay-128>3 || ay-128<-3;
    { static int seen; if (!seen) { seen=1;
        printf("Mouse: PROBE target seen during a gap (vx=%.1f vy=%.1f axis=%d/%d pushed=%d)\n",
               (double)mouse_vx,(double)mouse_vy,ax,ay,pushed); } }
    if (!pushed) return;                                  /* only the frames where the mouse really presses the stick carry the signal */
    if (u2_probe_n>=U2_PROBE_SAMPLES) { u2_probe_report(); return; }
    if (u2_probe_n==0) { u2_probe_start=t; u2_probe_camera=camera; ++u2_probe_seg_cur; }
    else if (t-u2_probe_last>100000) ++u2_probe_seg_cur;  /* a pause or another animation: no delta across it */
    for (int i=0;i<U2_PROBE_OFFSETS;++i) {
        float f=0.0f;
        if (!safe_read(&f,camera+u2_probe_offset(i),4)) f=0.0f;
        u2_probe_v[u2_probe_n][i]=f;
    }
    u2_probe_sx[u2_probe_n]=(float)ax; u2_probe_sy[u2_probe_n]=(float)ay;
    u2_probe_seg[u2_probe_n]=(unsigned char)(u2_probe_seg_cur & 0xff);
    ++u2_probe_n;
    u2_probe_last=t;
}
static void u2_probe_report(void) {
    const int n=u2_probe_n;
    u2_probe_n=0;
    ++u2_probe_seg_cur;                                   /* whatever comes next is another segment */
    if (n<12) return;
    printf("Mouse: PROBE base %#llx samples=%d t=%.2f (stick byte 128 = centre)\n",
           (unsigned long long)u2_probe_camera,n,u2_probe_start*1e-6);
    for (int pass=0;pass<2;++pass) {
        float best=0.0f, best_last=0.0f, best_range=0.0f; int best_i=-1, best_lag=0;
        for (int lag=0;lag<=2;++lag) {
            for (int i=0;i<U2_PROBE_OFFSETS;++i) {
                float sxx=0.0f,syy=0.0f,sxy=0.0f;
                float lo=1e30f, hi=-1e30f;
                int m=0;
                for (int k=1+lag;k<n;++k) {
                    if (u2_probe_seg[k]!=u2_probe_seg[k-1]) continue;
                    const float st=(pass ? u2_probe_sy[k-lag] : u2_probe_sx[k-lag])-128.0f;
                    const float d=u2_probe_v[k][i]-u2_probe_v[k-1][i];
                    if (!(st==st) || !(d==d) || d>1e30f || d<-1e30f) continue;
                    sxx+=st*st; syy+=d*d; sxy+=st*d; ++m;
                    if (u2_probe_v[k][i]<lo) lo=u2_probe_v[k][i];
                    if (u2_probe_v[k][i]>hi) hi=u2_probe_v[k][i];
                }
                if (m<10 || sxx<10.0f || syy<1e-10f) continue;
                const float corr=sxy/SDL_sqrtf(sxx*syy);
                const float r=corr<0.0f ? -corr : corr;
                if (r>best) { best=r; best_i=i; best_lag=lag; best_last=u2_probe_v[n-1][i]; best_range=hi-lo; }
            }
        }
        if (best_i<0) { printf("Mouse: PROBE %s: nothing correlated\n",pass ? "pitch" : "yaw"); continue; }
        printf("Mouse: PROBE %s +0x%03x corr=%.2f lag=%d value=%.3f range=%.3f\n",pass ? "pitch" : "yaw",
               u2_probe_offset(best_i),(double)best,best_lag,(double)best_last,(double)best_range);
    }
}
static void u2_probe_finish(uint64_t camera) {
    (void)camera;
    ++u2_probe_seg_cur;
    if (u2_probe_n>=12) { u2_probe_report(); return; }
    if (mouse_debug()) { /* diagnostic: the base camera is updated only a few times per animation */
        static uint64_t last;
        const uint64_t t=now_us();
        if (t-last>2000000) { last=t; printf("Mouse: PROBE gap end, only %d samples so far (kept)\n",u2_probe_n); }
    }
}
/* BB_U2_BASE=1 (experiment, 2026-10-24): during an animation the picture is held by camera 0x4383d0. Its disassembly
 * (0x4383d0) shows camera+0x1a0 = position, camera+0x1b0 = quaternion, both copied to the output; the quaternion is built
 * from a relative yaw at camera+0x18c, accumulated from an input field (vmovss 0x18c(%r15)) and composed with the previous
 * orientation. So +0x18c is a look offset, not an output: this writes the mouse's turn there and logs what the object holds
 * at the next frame (a value that survives the update can be driven 1:1). Nothing runs without BB_U2_BASE. */
static int u2_base_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_U2_BASE"); on=e && *e && *e!='0'; }
    return on;
}
static float u2_base_written; /* (debug) radians this hook added to +0x18c since the animation began */
static uint64_t u2_t5_us; static float u2_t5_yaw0; static int u2_t5_frames;   /* test mode 5 */
static void u2_base_camera(uint64_t camera, int gap) {
    float dx,dy;
    camera_take(0,&dx,&dy);
    static int stepped;   /* test modes 2 and 4: one marked step per animation */
    if (!gap) { u2_base_written=0.0f; stepped=0; u2_t5_us=0; u2_t5_frames=0; return; }
    const char *mode=getenv("BB_U2_BASE");
    const int test2=mode && mode[0]=='2';
    if (!test2 && dx==0.0f && dy==0.0f) return;
    if (gap && mode && mode[0]=='5') { /* measure: one 0.5 rad step on +0x188, yaw read every frame (bbgpu_camera_yaw) */
        float *o=(float *)(uintptr_t)(camera+0x188);
        if (!u2_t5_us) {
            u2_t5_us=now_us(); u2_t5_frames=0; u2_t5_yaw0=0.0f;
            float y=0.0f; double a=0.0;
            if (bbgpu_camera_yaw(&y,&a)) u2_t5_yaw0=y;
            const float w=*o;
            if (w>-5.0f && w<5.0f) *o=w+0.5f;
            printf("Mouse: TEST5 write 0x188 %.3f -> %.3f yaw0=%.2f t=%.4f\n",(double)w,(double)*o,(double)u2_t5_yaw0,now_us()*1e-6);
            return;
        }
        if (++u2_t5_frames<=20) {
            float y=0.0f; double a=0.0; const int have=bbgpu_camera_yaw(&y,&a);
            printf("Mouse: TEST5 f=%d 0x188=%.3f yaw=%.2f%s d=%.2f t=%.4f\n",u2_t5_frames,(double)*o,(double)y,
                   have && a<0.25 ? "" : "?",(double)(have ? y-u2_t5_yaw0 : 0.0f),now_us()*1e-6);
        }
        return;
    }
    if (mode && mode[0]=='4') { /* +0x188: the yaw offset of the chase camera; does one step turn the picture? */
        if (!stepped) {
            float *o=(float *)(uintptr_t)(camera+0x188);
            const float w=*o;
            if (w>-5.0f && w<5.0f) *o=w+0.2f;
            stepped=1;
            if (mouse_debug()) {
                float shown=0.0f; double age=0.0; const int have=bbgpu_camera_yaw(&shown,&age);
                printf("Mouse: base TEST4 0x188 %.3f -> %.3f shown=%.2f%s t=%.4f\n",(double)w,(double)*o,
                       (double)shown,have && age<0.25 ? "" : "?",now_us()*1e-6);
            }
        }
        return;
    }
    if (mode && mode[0]=='3') { /* +0x17c/+0x180/+0x184: clamped and decayed by the game (the stick fields) */
        const float step=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*0.017453293f;
        float *a=(float *)(uintptr_t)(camera+0x17c), *b=(float *)(uintptr_t)(camera+0x180), *c=(float *)(uintptr_t)(camera+0x184);
        const float wa=*a, wb=*b, wc=*c;
        if (step>-0.3f && step<0.3f) {
            if (wa>-1.0f && wa<1.0f) *a=wa+step;
            if (wb>-1.0f && wb<1.0f) *b=wb+step;
            if (wc>-1.0f && wc<1.0f) *c=wc+step;
        }
        if (mouse_debug()) {
            float shown=0.0f; double age=0.0; const int have=bbgpu_camera_yaw(&shown,&age);
            printf("Mouse: base TEST3 0x17c %.3f->%.3f 0x180 %.3f->%.3f 0x184 %.3f->%.3f dx=%.0f shown=%.2f%s t=%.4f\n",
                   (double)wa,(double)*a,(double)wb,(double)*b,(double)wc,(double)*c,(double)dx,
                   (double)shown,have && age<0.25 ? "" : "?",now_us()*1e-6);
        }
        return;
    }
    if (test2) { /* 0.5 rad once per animation on +0x18c: does the picture turn at all? */
        float *yaw=(float *)(uintptr_t)(camera+0x18c);
        const float was=*yaw;
        if (!stepped) { *yaw=was+0.5f; u2_base_written=0.5f; stepped=1; }
        if (mouse_debug()) {
            float shown=0.0f; double age=0.0; const int have=bbgpu_camera_yaw(&shown,&age);
            printf("Mouse: base TEST2 0x18c %.4f -> %.4f dx=%.0f shown=%.2f%s t=%.4f\n",
                   (double)was,(double)*yaw,(double)dx,(double)shown,have && age<0.25 ? "" : "?",now_us()*1e-6);
        }
        return;
    }
    {   /* mode 1: the mouse's turn added to +0x18c */
        float *yaw=(float *)(uintptr_t)(camera+0x18c);
        const float was=*yaw;
        const float step=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*0.017453293f;
        if (step>-0.5f && step<0.5f && was>-7.0f && was<7.0f) { *yaw=was+step; u2_base_written+=step; }
        if (mouse_debug()) {
            float shown=0.0f; double age=0.0; const int have=bbgpu_camera_yaw(&shown,&age);
            printf("Mouse: base 0x18c %.4f -> %.4f (written %.3f rad, dx=%.1f) shown=%.2f%s t=%.4f\n",
                   (double)was,(double)*yaw,(double)u2_base_written,(double)dx,(double)shown,
                   have && age<0.25 ? "" : "?",now_us()*1e-6);
        }
    }
}
static void __attribute__((ms_abi)) u2b_camera_hook(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t unused3) {
    (void)unused0; (void)unused1; (void)unused3;
    const uint64_t update=saved_register(5), camera=saved_register(7);
    const uint64_t t0=now_us();
    const int gap=!(u2_follow_seen_us && t0-u2_follow_seen_us<U2_FOLLOW_RESYNC_US); /* the walking camera is away: an animation holds the picture */
    if (mouse_debug()) { /* map of a gap: which camera updates run while the walking camera is away, and does the picture turn? */
        static uint64_t ids[8]; static unsigned counts[8]; static int n, active; static uint64_t t; static float yaw0;
        if (gap) {
            if (!active) {
                active=1; t=t0; n=0;
                float y=0.0f; double a=0.0;
                yaw0=bbgpu_camera_yaw(&y,&a) ? y : 0.0f;
            }
            const uint64_t id=update-image;
            int i=0;
            for (;i<n;++i) if (ids[i]==id) break;
            if (i==n && n<8) { ids[n]=id; counts[n]=0; ++n; }
            if (i<n) ++counts[i];
        }
        if (update-image==U2B_UPDATE_FOLLOW && active) {
            active=0;
            float y=0.0f; double a=0.0; const int have=bbgpu_camera_yaw(&y,&a);
            printf("Mouse: GAP %.0f ms updates=",(t0-t)*1e-3);
            for (int i=0;i<n;++i) printf(" %#llx x%u",(unsigned long long)ids[i],counts[i]);
            printf(" yaw %.1f -> %.1f%s t=%.4f\n",(double)yaw0,have ? (double)y : 0.0,have ? "" : "?",t0*1e-6);
        }
    }
    { const uint64_t uid=update-image; if (uid==0x4383d0 || uid==0x4496b0 || uid==0x42d940) u2_snap_frame(uid,camera); }
    if (mouse_debug()) {   /* (debug, BB_U2_WEIGHTS=1) the blend weights (stack entry +0x30..+0x3c, entries named by registers 9 and 11) of the three cameras of the window passage */
        static int on=-1;
        if (on<0) on=getenv("BB_U2_WEIGHTS")!=NULL;
        const uint64_t uid=update-image;
        if (on && (uid==0x4383d0 || uid==0x4496b0 || uid==0x42d940)) {
            float w9[4]={-9,-9,-9,-9}, w11[4]={-9,-9,-9,-9};
            const uint64_t r9=saved_register(9), r11=saved_register(11);
            if (r9>0x10000) safe_read(w9,r9+0x30,16);
            if (r11>0x10000) safe_read(w11,r11+0x30,16);
            printf("Mouse: WEIGHTS %#llx cam %#llx r9 %#llx [%.3f %.3f %.3f %.3f] r11 %#llx [%.3f %.3f %.3f %.3f] t=%.4f\n",(unsigned long long)uid,(unsigned long long)camera,
                   (unsigned long long)r9,w9[0],w9[1],w9[2],w9[3],(unsigned long long)r11,w11[0],w11[1],w11[2],w11[3],t0*1e-6);
        }
    }
    {   /* BB_U2_PROBE=<hex update id>: the fields of the camera that holds the picture during an animation */
        static int on=-1;
        if (on<0) on=getenv("BB_U2_PROBE")!=NULL;
        if (on) {
            if (update-image==U2B_UPDATE_FOLLOW) { if (u2_probe_n) u2_probe_finish(camera); }
            else if (update-image==u2_probe_target() && gap) u2_probe_frame(camera);
        }
    }
    if (mouse_debug()) { /* the set of cameras updated in one frame (calls less than 2 ms apart), printed when it changes */
        static uint64_t cur[12], last[12], last_us; static int n_cur, n_last;
        const uint64_t t=now_us();
        if (last_us && t-last_us>2000) {
            int same=n_cur==n_last;
            for (int i=0;same && i<n_cur;++i) if (cur[i]!=last[i]) same=0;
            if (!same) {
                printf("Mouse: cameras [");
                for (int i=0;i<n_cur;++i) printf(" %#llx",(unsigned long long)cur[i]);
                printf(" ] aim=%d look=%d follow=%d t=%.4f\n",aim_held,look_held,camera_hook_us && t-camera_hook_us<50000,t*1e-6);
                memcpy(last,cur,sizeof(cur)); n_last=n_cur;
                memcpy(u2_dbg_cams,cur,sizeof(cur)); u2_dbg_ncams=n_cur;
            }
            n_cur=0;
        }
        last_us=t;
        if (n_cur<12) cur[n_cur++]=update-image;
    }
    static int disabled=-1;   /* BB_U2_NATIVE=0: the mouse goes through the stick again (to tell a crash of the game from one of these hooks) */
    if (disabled<0) { const char *e=getenv("BB_U2_NATIVE"); disabled=e && atoi(e)==0; }
    if (camera<0x10000 || pad_only_mode || disabled) return;
    if (update-image==U2B_UPDATE_FOLLOW) u2_follow_seen_us=now_us();
    {   /* After the chapter 5 window the walking camera comes back as a new stack entry whose weight (float at entry+0x34) takes about 0.9 s to rise from 0
         * while the chase camera, frozen by the game, still holds most of the picture: the mouse shows only w times. As with the cover, the weight is
         * brought up linearly in 0.3 s (written at the entry of the update). BB_U2_WINBLEND=0 turns it off. */
        static int mode=-1; static uint64_t chase_seen_us, ramp_entry, ramp_start_us, ramp_seen_us;
        if (mode<0) { const char *e=getenv("BB_U2_WINBLEND"); mode=!(e && atoi(e)==0); }
        const uint64_t now=now_us();
        if (update-image==0x4496b0) chase_seen_us=now;
        else if (mode && update-image==U2B_UPDATE_FOLLOW) {
            const uint64_t entry=saved_register(11);
            float w=1.0f;
            if (entry>0x10000 && safe_read(&w,entry+0x34,4) && w>=0.0f && w<0.999f) {
                if (entry!=ramp_entry || now-ramp_seen_us>300000) {
                    ramp_entry=entry; ramp_start_us=0;
                    if (w<0.05f && chase_seen_us && now-chase_seen_us<600000) ramp_start_us=now;
                }
                ramp_seen_us=now;
                if (ramp_start_us) {
                    float ramp=(float)(now-ramp_start_us)*(1.0f/300000.0f); ramp=ramp>1.0f ? 1.0f : ramp;
                    if (ramp>w) { safe_write(entry+0x34,&ramp,4); if (mouse_debug()) printf("Mouse: u2 winblend entry %#llx w %.3f -> %.3f t=%.4f\n",(unsigned long long)entry,w,ramp,now*1e-6); }
                }
            }
        }
    }
    switch (update-image) {
    case U2B_UPDATE_COVER: u2_cover_camera(camera,image); break;
    case U2B_UPDATE_LOOK: u2_look_seen_us=now_us(); u2_look_camera(camera,image); break;
    case 0x4496b0: u2_chase_camera(camera,image); break;
    case U2B_UPDATE_FOLLOW: /* walking: the follow camera's direction (+0x9ec0) and vertical arc (+0x9e60), as in the earlier U2 build */
    {
        /* A walking camera that has just been made (museum start, chapter or level change) holds numbers that are not set yet
         * (NaN, huge): writing the position from them made the game stop on IsFinite(m_smoothedCameraPos) / the camera
         * translation limit. So the camera is left alone for its first frames and while any of its numbers is unusable. */
        static struct { uint64_t camera, seen_us; unsigned frames; } seen_slot[4];   /* (after the chapter 5 window two walking cameras are updated alternately: one slot per camera, or the guard restarted at every call and the mouse stayed off for 1.2 s) */
        const uint64_t t_now=now_us();
        int sl=-1;
        for (int i=0;i<4;++i) if (seen_slot[i].camera==camera) sl=i;
        if (sl<0) { sl=0; for (int i=1;i<4;++i) if (seen_slot[i].seen_us<seen_slot[sl].seen_us) sl=i; seen_slot[sl].camera=camera; seen_slot[sl].frames=0; seen_slot[sl].seen_us=t_now; }
        unsigned seen_frames=seen_slot[sl].frames;
        /* A camera that has just been made holds numbers that are not set yet. The guard is for a NEW camera (another
         * pointer, or one that has been away for a second: a level change); after a short animation gap the same camera
         * comes back with its numbers intact, and resetting the guard there put the mouse back on the emulated stick for
         * 8 frames after every animation (measured 2026-10-24: following=0 while the walking camera ran 28 ms ago). */
        if (t_now-seen_slot[sl].seen_us>1000000) seen_frames=0;
        seen_slot[sl].seen_us=t_now;
        if (seen_frames<8) ++seen_frames;
        seen_slot[sl].frames=seen_frames;
        const float *chk_dir=(const float *)(uintptr_t)(camera+0x9ec0), *chk_pos=(const float *)(uintptr_t)(camera+0x9d70);
        const float *chk_pivot=(const float *)(uintptr_t)(camera+0x9e70), *chk_t1=(const float *)(uintptr_t)(camera+0x9d50);
        const float *chk_t0=(const float *)(uintptr_t)(camera+0x9eb0);
        int usable=seen_frames>=8;
        for (int i=0;usable && i<3;++i) {
            if (!(chk_dir[i]>-2.0f && chk_dir[i]<2.0f) || !(chk_pos[i]>-5000.0f && chk_pos[i]<5000.0f) ||
                !(chk_pivot[i]>-5000.0f && chk_pivot[i]<5000.0f) || !(chk_t1[i]>-5000.0f && chk_t1[i]<5000.0f) ||
                !(chk_t0[i]>-5000.0f && chk_t0[i]<5000.0f)) usable=0;
        }
        if (usable) {
            const float gx=chk_pos[0]-chk_pivot[0], gy=chk_pos[1]-chk_pivot[1], gz=chk_pos[2]-chk_pivot[2];
            if (!(gx*gx+gy*gy+gz*gz<400.0f*400.0f)) usable=0;
        }
        if (!usable) {
            float ignore_x, ignore_y;
            camera_take(0,&ignore_x,&ignore_y);
            follow_applied=0.0f; follow_arc_moved=0;
            if (mouse_debug()) { static unsigned skipped; if ((skipped++&63)==0) printf("Mouse: u2 follow camera %#llx skipped (frames=%u)\n",(unsigned long long)camera,seen_frames); }
            break;
        }
    }
        { const float frame_dt=u2_frame_dt(image);
          follow_camera(camera,0x9ec0,0x9e60,135.0f,!(frame_dt>0.0f && frame_dt<0.25f),U2_FOLLOW_RESYNC_US); }
        if ((follow_applied!=0.0f || follow_arc_moved) && !input.free_view_smoothing) {
            /* The camera's target position (+0x9d50, the one before it at +0x9eb0) follows the direction at once, on a circle about
             * the player (+0x9e70), but the position it shows (+0x9d70) follows that target through a spring and trails it by 6 to
             * 8 frames (measured in user\last_run.log). The spring's state is turned about the player by the same angle as the
             * direction, so that it has no gap to close. When the vertical arc moves too, the target rises or falls on its sphere:
             * the state is tilted about the player by the same change of elevation (the one the target made last frame). Its own
             * distance is kept, for it holds what the game's collisions did: putting it on the target put the camera into the walls,
             * which the game then pushed out of again (the picture shook). */
            float *pos=(float *)(uintptr_t)(camera+0x9d70);
            const float *pivot=(const float *)(uintptr_t)(camera+0x9e70);
            const float a=follow_applied*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
            float px=pos[0]-pivot[0], pz=pos[2]-pivot[2], py=pos[1]-pivot[1];
            if (follow_arc_moved) {
                const float *t1=(const float *)(uintptr_t)(camera+0x9d50), *t0=(const float *)(uintptr_t)(camera+0x9eb0);
                const float r1=SDL_sqrtf((t1[0]-pivot[0])*(t1[0]-pivot[0])+(t1[2]-pivot[2])*(t1[2]-pivot[2]));
                const float r0=SDL_sqrtf((t0[0]-pivot[0])*(t0[0]-pivot[0])+(t0[2]-pivot[2])*(t0[2]-pivot[2]));
                const float h=SDL_sqrtf(px*px+pz*pz);
                if (r1>0.3f && r0>0.3f && h>0.1f) {
                    float d=SDL_atan2f(t1[1]-pivot[1],r1)-SDL_atan2f(t0[1]-pivot[1],r0); /* change of elevation, radians */
                    d=d>0.35f ? 0.35f : d<-0.35f ? -0.35f : d;
                    if (mouse_debug() && (d>0.05f || d<-0.05f)) printf("Mouse: u2 arc tilt %.1f deg (gap %.0f ms)\n",(double)(d*57.29578f),follow_gap_us*1e-3);
                    /* After an animation the previous target (+0x9eb0) is the one from before it: the change of elevation
                     * it shows is the animation's, not the player's, and tilting the shown position by it jolted the picture.
                     * Measured 2026-10-24: the walking camera's normal call interval is 30 to 60 ms in the climbing
                     * sequences, so only a gap beyond that (an animation) is treated as one. */
                    if (follow_gap_us<U2_FOLLOW_RESYNC_US) {
                        const float e=SDL_atan2f(py,h), len=SDL_sqrtf(h*h+py*py), e2=e+d, k=len*SDL_cosf(e2)/h;
                        px*=k; pz*=k; py=len*SDL_sinf(e2);
                    } else if (mouse_debug()) printf("Mouse: u2 arc tilt dropped (gap %.0f ms)\n",follow_gap_us*1e-3);
                }
            }
            if (follow_applied!=0.0f) {
                const float x=px;
                px=x*c-pz*s;
                pz=x*s+pz*c;
            }
            if (px>-400.0f && px<400.0f && py>-400.0f && py<400.0f && pz>-400.0f && pz<400.0f) { /* false for NaN too */
                pos[0]=pivot[0]+px; pos[1]=pivot[1]+py; pos[2]=pivot[2]+pz;
            }
        }
        if (mouse_debug()) {
            static unsigned n;
            const float *d=(const float *)(uintptr_t)(camera+0x9ec0);
            if ((n++&31)==0) printf("Mouse: u2 follow camera %#llx dir=(%.3f %.3f %.3f) arc=%.3f\n",(unsigned long long)camera,d[0],d[1],d[2],*(const float *)(uintptr_t)(camera+0x9e60));
        }
        break;
    case U2B_UPDATE_ZOOM: /* L1 (grenade) aims with this camera once it runs, see sample() */
        zoom_camera_us=now_us();
        if (u2_follow_seen_us && zoom_camera_us-u2_follow_seen_us<3000) u2_zoom_under_us=zoom_camera_us;
        else if (u2_cover_seen_us && zoom_camera_us-u2_cover_seen_us<3000) u2_zoom_below_cover_us=zoom_camera_us;   /* (aim released in cover: the cover camera is on top, the zoom camera blends out beneath it) */
        else if (u2_look_seen_us && zoom_camera_us-u2_look_seen_us<3000) { u2_zoom_below_cover_us=zoom_camera_us; u2_zoom_below_look_us=zoom_camera_us; }   /* (aim released while holding a shield: the look camera 0x432450 is on top; it takes no native mouse, so the hidden zoom camera must not keep it either) */
        else u2_zoom_top_us=zoom_camera_us;
        u2_zoom_camera(camera,image);
        break;
    case 0x4383d0: /* the camera that holds the picture alone during an animation: relative yaw offset at +0x18c */
        if (u2_base_on()) u2_base_camera(camera,gap);
        break;
    default:
        if (mouse_debug()) { /* a camera this hook does not know: the mouse stays a stick while it runs (climbing, cutscenes, special views) */
            static uint64_t seen[16]; static int nseen;
            const uint64_t id=update-image;
            int known=0;
            for (int i=0;i<nseen && !known;++i) known=seen[i]==id;
            if (!known) {
                if (nseen<16) seen[nseen++]=id;
                printf("Mouse: u2 camera update %#llx not handled (mouse stays on the stick) t=%.4f\n",(unsigned long long)id,now_us()*1e-6);
            }
        }
        break;
    }
}
static const unsigned char u2b_camera_hook_code[]={
    0x48,0x8d,0xbd,0x30,0xff,0xff,0xff,0x48,0x8d,0x95,0xa0,0xfe,0xff,0xff,0x4c,0x89,0xd6,0x41,0xff,0xd0,
};
static const unsigned char u1_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x00,0xff,0xff,0xff,0x49,0x89,0xcd,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfc,
    0x4c,0x8b,0x35,0x92,0x7b,0x8d,0x00,0x49,0x8b,
};
static const unsigned char u2_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x48,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x18,0xff,0xff,0xff,0x48,0x89,0x8d,0xc8,0xfe,0xff,0xff,0x49,0x89,0xd5,0x4c,0x89,0xad,
    0x38,0xff,0xff,0xff,0x48,0x89,0xf3,0x49,
};
static const unsigned char u3_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x01,0x00,0x00,
    0x44,0x89,0x8d,0xa0,0xfe,0xff,0xff,0x48,0x89,0x8d,0x48,0xfe,0xff,0xff,0x49,0x89,0xd5,0x48,0x89,0xf3,
    0x49,0x89,0xff,0x4c,0x8b,0x65,0x20,0x4c,
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
static float aim_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    const float x=r<=45.0f ? SDL_sqrtf(r/45.0f) : (r+9.0f)/54.0f;
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
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14) {
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
/* The mouse's motion is taken once per frame at the moment the camera is updated, and those moments are uneven (measured: 7 and 13 ms apart in turn
 * while the game's own frame time stays 10 ms and the frames are shown evenly), so the turn per frame beat between small and large (18 / 26 px in
 * turn on a capture) and the player's body, a frame behind the camera, swung back and forth against it. The turn is given as it was at a steady
 * pace instead: the total turned since the aim began is kept as a function of time and read at a virtual time that moves by the game's frame time
 * and is held about AIM_RESAMPLE_DELAY_US behind real time. The angle turned in total is the same (1:1), no speed is capped.
 * Off by default (BB_AIM_RESAMPLE=1 turns it on, BB_AIM_DELAY_MS=<ms> sets the delay): measured on video it made the steps a little steadier (median
 * deviation 8.5 % to 7.2 %) but did not change the body's swing, which a perfectly steady turn (BB_AIM_TEST) shows as well. */
#define AIM_RESAMPLE_N 64
static int aim_resample_reset=1;
static void aim_resample(uint64_t now, float dt, float *dx, float *dy) {
    static struct { double t; float x, y; } ring[AIM_RESAMPLE_N];
    static int count, enabled=-1; static double v, delay_us=15000.0; static float cx, cy, ax, ay;
    if (enabled<0) {
        const char *e=getenv("BB_AIM_RESAMPLE"); enabled=e && *e=='1';
        const char *d=getenv("BB_AIM_DELAY_MS"); if (d && *d) delay_us=atof(d)*1000.0;
    }
    if (!enabled) return;
    const double t=(double)now;
    if (aim_resample_reset || count==0 || t-ring[count-1].t>100000.0) {
        aim_resample_reset=0; count=0; cx=cy=ax=ay=0.0f; v=t-delay_us;
        ring[0].t=t-delay_us-1000.0; ring[0].x=ring[0].y=0.0f; count=1;
    }
    cx+=*dx; cy+=*dy;
    if (count==AIM_RESAMPLE_N) { memmove(ring,ring+1,(AIM_RESAMPLE_N-1)*sizeof ring[0]); count--; }
    ring[count].t=t; ring[count].x=cx; ring[count].y=cy; count++;
    v+=(double)dt*1e6;
    v+=0.1*((t-delay_us)-v);
    if (v>t) v=t;
    if (v<ring[0].t) v=ring[0].t;
    float ox=ring[count-1].x, oy=ring[count-1].y;
    for (int i=count-1;i>0;--i) {
        if (ring[i-1].t<=v) {
            const double span=ring[i].t-ring[i-1].t, k=span>0.0 ? (v-ring[i-1].t)/span : 1.0;
            ox=ring[i-1].x+(float)k*(ring[i].x-ring[i-1].x); oy=ring[i-1].y+(float)k*(ring[i].y-ring[i-1].y);
            break;
        }
    }
    *dx=ox-ax; *dy=oy-ay; ax=ox; ay=oy;
}
static uint64_t aim_native_us;        /* last frame the zoom camera was turned by the mouse */
static volatile int aim_session_reset = 1; /* set while not aiming: the next aim starts from a zero turn */
static float aim_pending_degrees;     /* the mouse's yaw not given to the zoom camera yet */
static float aim_pending_pitch;       /* its pitch (degrees, positive looks down) */
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
    if (vtable<0x10000) return;
    if (((const uint64_t *)(uintptr_t)vtable)[0x98/8]==image+0xaff80) {   /* the walking camera, right after its update */
        /* While a cover camera runs (and is blended in or out, about a second) the walking camera's output stood still: the picture is
         * w x the cover camera + (1-w) x the walking camera, whose weight w is 0 at first, so the picture did not turn at the start of a cover.
         * Its output yaw is made the cover camera's: both layers agree, whatever the weight. */
        const uint64_t at=now_us();
        if (!cover_out_us || at-cover_out_us>80000) return;
        float *yaw_o=(float *)(uintptr_t)(guest_callback_rsp+0x140);
        /* While the cover camera runs, and for a few frames after it (the walking camera is still being turned back to its yaw, see follow_camera),
         * the output is the cover camera's: no frame shows the walking camera's own yaw in between (it flashed there for two frames) */
        float *pos_o=(float *)(uintptr_t)(guest_callback_rsp+0x120);   /* the output block: position x y z, then the angles at +0x20 */
        if (at-cover_out_us<=20000) {   /* the player is in the cover spot: the camera's distance to him and its side are measured here */
            const float ox=pos_o[0]-cover_pivot_x, oz=pos_o[2]-cover_pivot_z, yr=*yaw_o*0.017453293f;
            const float len=SDL_sqrtf(ox*ox+oz*oz);
            if (len>0.5f && len<30.0f) { orbit_r=len; orbit_s=(ox*SDL_sinf(yr)+oz*SDL_cosf(yr))>=0.0f ? 1.0f : -1.0f; }
        }
        if (at-cover_out_us<=20000 || SDL_fabsf(wrap_degrees(*yaw_o-cover_out_yaw))>1.5f) {
            /* The picture is the cover camera blended with this one. This one sits behind the player at the yaw the game put back (the old
             * one) while the mouse has turned the view elsewhere: its position is turned round the player to the yaw of the cover camera, so
             * the blend carries the camera in space from the cover to behind the player at the yaw the mouse gave. */
            const float yo=*yaw_o*0.017453293f, yu=cover_out_yaw*0.017453293f;
            pos_o[0]+=orbit_s*orbit_r*(SDL_sinf(yu)-SDL_sinf(yo));
            pos_o[2]+=orbit_s*orbit_r*(SDL_cosf(yu)-SDL_cosf(yo));
            *yaw_o=cover_out_yaw;
        }
        return;
    }
    if (((const uint64_t *)(uintptr_t)vtable)[0x98/8]==image+0xa4670) {   /* the cover camera, right after its update */
        cover_out_yaw=*(const float *)(uintptr_t)(guest_callback_rsp+0x140);
        cover_pivot_x=*(const float *)(uintptr_t)(camera+0x2a0);
        cover_pivot_z=*(const float *)(uintptr_t)(camera+0x2a8);
        cover_out_us=now_us();
        return;
    }
    if (((const uint64_t *)(uintptr_t)vtable)[0x98/8]!=image+0xc43b0) return;
    const uint64_t now=now_us();
    zoom_camera_us=now;   /* the zoom camera runs: L1 (grenade) counts as an aim, see sample() */
    /* After the aim the game blends the zoom camera out for about a second (the picture = w x walking camera + (1-w) x zoom camera): it keeps
     * the mouse's turns meanwhile, or it holds the picture where the player last looked while the walking camera, which has the mouse, turns. */
    const int fading=!aim_held && now<aim_blend_until_us;
    if (!aim_held && !fading) return;
    const uint64_t entry=*(const uint64_t *)(uintptr_t)(guest_callback_rsp-72);
    if (entry>0x10000) {   /* A roll with the aim held: the game blends the zoom camera out and the walking camera in (measured: the zoom entry's weight at +0x38 falls 1 -> 0 in a second while
                            * +0x34 stays 1, the walking entry's weight rises 0 -> 1). That is not an aim: the walking camera takes the mouse as well, or the picture freezes meanwhile. */
        float a=0.0f, b=0.0f;
        if (safe_read(&a,entry+0x34,4) && safe_read(&b,entry+0x38,4) && a>0.99f && b>=0.0f && b<0.99f) u2_zoom_under_us=now;
    }
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
        aim_session_reset=0; aim_resample_reset=1; total_yaw=total_pitch=embedded_yaw=embedded_pitch=0.0f;
        aim_pending_degrees=aim_pending_pitch=0.0f; frame_us=0;
    }
    const int new_frame=!(frame_us && now-frame_us<1500);
    if (new_frame) {
        if (aim_native_us && now-aim_native_us>100000) aim_pending_degrees=aim_pending_pitch=0.0f;
        float dx, dy;
        camera_take(2,&dx,&dy);
        if (camera_stopped(image,U1B_CAMERA_DT)) { aim_pending_degrees=aim_pending_pitch=0.0f; return; } /* paused */
        float dt=*(const volatile float *)(uintptr_t)(image+U1B_CAMERA_DT);
        if (!(dt>0.0005f && dt<0.05f)) dt=1.0f/60.0f;
        aim_resample(now,dt,&dx,&dy);
        aim_pending_degrees+=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity;
        aim_pending_pitch+=(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*input.mouse_aim_sensitivity;
        static float test_rate=-1.0f; if (test_rate<0.0f) { const char *e=getenv("BB_AIM_TEST"); test_rate=e ? (float)atof(e) : 0.0f; }   /* BB_AIM_TEST=<degrees per second>: a steady turn on top of the mouse (to see whether the unevenness is made after our code) */
        if (test_rate>0.0f) aim_pending_degrees+=test_rate*dt;
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
    const float aim_in=SDL_atan2f(aim[0],aim[2])*57.29578f, yaw_in=*yaw;   /* (what the game left, for BB_U1_AIMPROBE) */
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
    {   /* BB_U1_AIMPROBE=<file>: at every aim frame, time, the mouse's turn so far and this frame's, then the player object's first 0x800 bytes (to find the body's heading) */
        static FILE *probe_f; static int probe_init; static unsigned probe_n;
        if (!probe_init) { probe_init=1; const char *p=getenv("BB_U1_AIMPROBE"); if (p && *p) probe_f=fopen(p,"wb"); }
        uint64_t holder=0, player=0;
        if (!(safe_read(&holder,image+0xaa5f78,8) && safe_read(&player,holder,8))) player=0;
        if (player>0x10000 && new_frame && getenv("BB_ROOT_WATCH")) {   /* the same on the player's heading quaternion (+0x644 y, +0x64c w): who turns the body (use with BB_WATCH_WRITE=1) */
            static uint64_t t0, last_summary;
            if (!t0) t0=now;
            uint64_t xf=0;   /* (Ghidra: player+0xf0 -> the transform object, whose quaternion at +0x20 is the body's own orientation; player+0x640 is only the camera heading the controller latched) */
            (void)xf; stickwatch_arm(player+0x590,player+0x598,image);   /* a facing direction (x, z) that follows the camera's turn with one frame of delay (measured) */
            stickwatch_enable(now-t0>2000000);
            if (now-t0>2000000 && now-last_summary>4000000) { last_summary=now; stickwatch_summary(); }
        }
        if (player>0x10000 && new_frame && getenv("BB_ANIM_WATCH")) {   /* hardware watch on the skeleton's local matrices (Ghidra FUN_0008ca40: skeleton = *(*(player+0xf0)+0xb8), world matrices *(skel+8), local ones *(skel+0x10), 0x60 bytes per joint): who writes joint 15's local transform (use with BB_WATCH_WRITE=1) */
            static uint64_t t0, last_summary; uint64_t xf=0, skel=0, local=0;
            if (!t0) t0=now;
            if (safe_read(&xf,player+0xf0,8) && xf>0x10000 && safe_read(&skel,xf+0xb8,8) && skel>0x10000 && safe_read(&local,skel+0x10,8) && local>0x10000) {
                const char *e=getenv("BB_ANIM_JOINT"); const unsigned j=e ? (unsigned)atoi(e) : 15;
                stickwatch_arm(local+j*0x60,local+j*0x60+0x10,image);
                stickwatch_enable(now-t0>2000000);
                if (now-t0>2000000 && now-last_summary>4000000) { last_summary=now; stickwatch_summary(); }
            }
        }
        if (player>0x10000 && new_frame && getenv("BB_CLOCK_WATCH")) {   /* the same on the player's clock (player+0xe0): the delta time (+0x10) and the float after the 0x1c one */
            static uint64_t t0, last_summary; uint64_t clock=0;
            if (!t0) t0=now;
            if (safe_read(&clock,player+0xe0,8) && clock>0x10000) {
                stickwatch_arm(clock+0x10,clock+0x14,image);
                stickwatch_enable(now-t0>2000000);
                if (now-t0>2000000 && now-last_summary>4000000) { last_summary=now; stickwatch_summary(); }
            }
        }
        if (player>0x10000 && new_frame && getenv("BB_POSE_WATCH")) {   /* hardware watch on two floats of the pose buffer (player+0x100 -> joints): who writes the torso / arm rotations */
            static uint64_t t0, last_summary; uint64_t pose=0;
            if (!t0) t0=now;
            if (safe_read(&pose,player+0x100,8) && pose>0x10000) {
                stickwatch_arm(pose+0x3c0,pose+0x2dc,image);
                stickwatch_enable(now-t0>2000000);
                if (now-t0>2000000 && now-last_summary>4000000) { last_summary=now; stickwatch_summary(); }
            }
        }
        if (probe_f && new_frame && probe_n<6000 && player>0x10000) {
            static unsigned char buf[0x1000];
            if (safe_read(buf,player,sizeof buf)) {
                const double pt=now*1e-6; float head[8]={total_yaw,frame_turn,0.0f,(float)new_frame,aim_in,SDL_atan2f(aim[0],aim[2])*57.29578f,yaw_in,*yaw};
                safe_read(&head[2],image+U1B_CAMERA_DT,4);
                unsigned char clk[0x40]={0}; uint64_t clock=0;   /* the player process's clock (player+0xe0), whose float at +0x10 is the delta time its update uses (FUN_0029afd0) */
                if (safe_read(&clock,player+0xe0,8) && clock>0x10000) safe_read(clk,clock,sizeof clk);
                fwrite(&pt,8,1,probe_f); fwrite(head,4,8,probe_f); fwrite(clk,1,sizeof clk,probe_f); fwrite(buf,1,sizeof buf,probe_f);
                if ((++probe_n&63)==0) fflush(probe_f);
            }
        }
        /* BB_U1_AIMPROBE2=<file>: the same, but every pointer found in the player object's first 0x1000 bytes is followed and 0x400 bytes of what it points to are written
         * (time, count, then per pointer: offset in the player, pointer, bytes), to find the skeleton / animation data */
        static FILE *probe2_f; static int probe2_init; static unsigned probe2_n;
        if (!probe2_init) { probe2_init=1; const char *p=getenv("BB_U1_AIMPROBE2"); if (p && *p) probe2_f=fopen(p,"wb"); }
        if (probe2_f && new_frame && probe2_n<500 && player>0x10000) {
            static unsigned char pl[0x1000], target[0x400];
            if (safe_read(pl,player,sizeof pl)) {
                const double pt=now*1e-6; uint32_t count=0;
                long count_at=ftell(probe2_f);
                float extra[7]={total_yaw,frame_turn,0.0f,0,0,0,0};   /* the mouse's turn, game dt, the player's heading quaternion (+0x640) */
                safe_read(&extra[2],image+U1B_CAMERA_DT,4); memcpy(&extra[3],pl+0x640,16);
                fwrite(&pt,8,1,probe2_f); fwrite(&count,4,1,probe2_f); fwrite(extra,4,7,probe2_f);
                for (uint32_t off=0;off+8<=sizeof pl;off+=8) {
                    uint64_t v; memcpy(&v,pl+off,8);
                    if (v<0x10000 || v>0x7fffffffffffULL || (v&3)) continue;
                    if (!safe_read(target,v,sizeof target)) continue;
                    fwrite(&off,4,1,probe2_f); fwrite(&v,8,1,probe2_f); fwrite(target,1,sizeof target,probe2_f);
                    count++;
                }
                const long end_at=ftell(probe2_f);
                fseek(probe2_f,count_at+8,SEEK_SET);   /* (the count sits right after the time) */ fwrite(&count,4,1,probe2_f); fseek(probe2_f,end_at,SEEK_SET);
                if ((++probe2_n&15)==0) fflush(probe2_f);
            }
        }
    }
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
    {   /* The zoom camera coming in again after a roll with the aim held (the earlier zoom entry was blended out under the walking camera, see u1b_aim_camera): the walking camera under it
         * does not turn with the mouse, and the new zoom camera's weight (the stack entry that register 9 points at, floats +0x34 and +0x38, equal while it comes in) stays near 0 for a
         * moment, so the picture froze. Its weight is brought up linearly in a quarter of a second at most. */
        const uint64_t entry=saved_register(9), now=now_us();
        float a=0.0f, b=0.0f;
        if (entry>0x10000 && u2_zoom_under_us && now-u2_zoom_under_us<1500000 && safe_read(&a,entry+0x34,4) && safe_read(&b,entry+0x38,4) && a>=0.0f && a<0.999f && SDL_fabsf(a-b)<0.002f) {
            static uint64_t start_us, last_us;
            if (!last_us || now-last_us>100000) start_us=now;
            last_us=now;
            float ramp=(float)(now-start_us)*(1.0f/250000.0f); ramp=ramp>1.0f ? 1.0f : ramp;
            if (ramp>a) { safe_write(entry+0x34,&ramp,4); safe_write(entry+0x38,&ramp,4); }
        }
    }
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
    const uint64_t now=now_us();
    if (!look_held && now>=look_blend_until_us) return;
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
    if (aim_native || look_held || camera<0x10000) return;   /* (L2 held without the aiming camera running, between two covers, is not an aim: the cover camera keeps the mouse) */
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
        (void)dt;   /* no smoothing, as in U2's cover camera: the whole motion of the frame is applied at once */
        step=cover_pending_yaw; step_pitch=cover_pending_pitch;
        cover_pending_yaw=cover_pending_pitch=0.0f;
        frame_us=now; frame_camera=camera; frame_yaw=step; frame_pitch=step_pitch;
    }
    cover_native_us=now;
    cover_last_step=step; cover_last_us=now;
    float *dir=(float *)(uintptr_t)(camera+0x240);
    cover_dir_x=dir[0]; cover_dir_z=dir[2];
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
        /* as in U2 (same layout: the position spring at dir+0x10, 3 m from the pivot at dir+0x60 (the cover spot, measured), follows its target about the pivot a few frames late):
         * turned the same way so that nothing is left to catch up (the lag was the smoothing left in the cover camera) */
        float *pos=(float *)(uintptr_t)(camera+0x250);
        const float *pivot=(const float *)(uintptr_t)(camera+0x2a0);
        const float px=pos[0]-pivot[0], py=pos[1]-pivot[1], pz=pos[2]-pivot[2], len2=px*px+py*py+pz*pz;
        if (len2>0.01f && len2<900.0f) {
            pos[0]=pivot[0]+px*c-pz*s;
            pos[2]=pivot[2]+px*s+pz*c;
        }
        if (mouse_debug()) {
            static unsigned n;
            if (n++<1500) printf("Mouse: u1 coverspring t=%.4f step=%.3f lag=%.1f |pos|=%.2f y=%.2f pos=(%.2f %.2f) dir=(%.3f %.3f) pitch=%.2f\n",
                                 now*1e-6,step,(SDL_atan2f(pz,px)-SDL_atan2f(-dir[2],-dir[0]))*57.29578f,SDL_sqrtf(px*px+pz*pz),py,pos[0],pos[2],dir[0],dir[2],*pitch);
        }
    }
    if (step_pitch!=0.0f && shape>0x10000) {
        float v=*pitch-step_pitch; /* degrees (limits +-45, speed 90 degrees/s) */
        const float lo=-*(const float *)(uintptr_t)(shape+0xc), hi=*(const float *)(uintptr_t)(shape+8);
        *pitch=v<lo ? lo : v>hi ? hi : v;
    }
    if (mouse_debug()) { float shown=0.0f; double age=0.0; const int have=bbgpu_camera_yaw(&shown,&age); const float dd[3]={-dir[0],0.0f,-dir[2]}; printf("Mouse: coverturn t=%.4f step=%.3f pitch step=%.3f pitch=%.3f shown=%.2f%s coverheading=%.2f\n",now*1e-6,step,step_pitch,*pitch,shown,have ? "" : "?",heading_of(dd)); }
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
    cover_last_step=step; cover_last_us=now;
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
    if (follow_test>0.0f) { ((float *)(uintptr_t)joypad_state)[2]+=follow_test; ((float *)(uintptr_t)joypad_state)[6]+=follow_test; return; }
    float dx, dy;
    camera_take(1,&dx,&dy);
    if (camera_stopped(image,u1_camera_dt_offset)) return; /* paused */
    if (aim_test<0.0f) { const char *t=getenv("BB_MOUSE_AIM_TEST"); aim_test=t ? (float)atof(t) : 0.0f; }
    float *pad=(float *)(uintptr_t)joypad_state;
    if (!aim_held && !look_held && now_us()>=aim_blend_until_us && now_us()>=look_blend_until_us) { float ix, iy; camera_take(2,&ix,&iy); }   /* (the zoom camera keeps the queue while the aim is blended out) */
    const int cover_native=cover_native_us && now-cover_native_us<COVER_RECENT_US;
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
    if (aim_test>0.0f) x=aim_test;
    else {
        /* the turn rate (degrees/s) the mouse asks for, through the inverse of the aiming curve
         * measured in U1: 45 x^2 degrees/s up to x = 1, then about 54 x - 9 (x = 3: 152, x = 6: 324) */
        const int following=camera_hook_us && now-camera_hook_us<50000;
        /* aiming, the look around and any camera other than the follow camera (cover) turn the same way */
        const int aim_like=aim_native || look_held || !following;   /* (L2 held without the aiming camera running, in a roll or between two covers, is not an aim) */
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
        const int zoom_fading=u2_zoom_under_us && now-u2_zoom_under_us<100000;   /* (a roll with the aim held: the zoom camera is blending out, the walking camera has the mouse) */
        const int native_yaw=(aim_held && !zoom_fading && aim_native_us && now-aim_native_us<600000) ||
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
/* U2 (collection) menu text probe, BB_MOUSE_DEBUG only: the same routine as U1's text drawing, recompiled at 0x5b56d0 (rdx style,
 * rcx laid-out text: width at +0x3c, height at +0x40, r8 rectangle, r9d and the first stack argument the alignments). Logs each
 * text of a menu once, and again when its style changes (the highlighted row), to see how U2 marks the selection. */
typedef struct { float rect[4], w, h, style[12]; uint64_t us; } U2ProbeText;
/* The rows of a U2 menu, for the mouse (U2 has no menu objects to ask): every text drawn keeps its rectangle and style[5], which the
 * game takes from 0 to 3 on the selected row. Read by u2_menu_buttons (menu_text_lock). */
static volatile uint64_t u2_keys_host;   /* where the text of our Commandes row is in memory (u2_keys_label_patch) */
static volatile uint64_t u3_keys_row_us; static volatile float u3_keys_row_y;   /* U3: the moment our row was drawn and its top, found by that text's address in the layout object */
typedef struct { float x, y, w, h, s5; uint64_t us; float a; } U2Row;   /* a: style[7], 1 on the chosen one of the Oui / Non of a confirmation, 0 on the other */
#define U2_ROWS 256   /* the Commandes screen alone draws about 80 texts of row size (labels, values, icon cells) */
static U2Row u2_rows[U2_ROWS];
/* The rails of the sliders (Sensibilité, volumes): a text 100+ wide and under 12 tall laid out in a row's rectangle. The game keeps the
 * position of the knob out of the texts, so the mouse drags a slider by steps (u2_menu_buttons), it does not set it. */
static U2Row u2_sliders[8];
static void u2_slider_note(const float *r, float rail_w) {
    const uint64_t now=now_us();
    pthread_mutex_lock(&menu_text_lock);
    int slot=0;
    for (int i=0;i<8;++i) {
        if (fabsf(u2_sliders[i].x-r[0])<0.5f && fabsf(u2_sliders[i].y-r[1])<0.5f) { slot=i; break; }
        if (u2_sliders[i].us<u2_sliders[slot].us) slot=i;
    }
    u2_sliders[slot]=(U2Row){r[0],r[1],rail_w,r[3],0.0f,now,0.0f};
    pthread_mutex_unlock(&menu_text_lock);
}
/* U3 (collection): the same menu framework, but the rail of a slider is not sure to have U2's shape (100+ wide, 12 or less tall):
 * noted with a looser test into the same table. Only the U3 menu code reads it there, so U2 is untouched. */
static void u3_slider_note(const float *r, float rail_w) {
    const uint64_t now=now_us();
    pthread_mutex_lock(&menu_text_lock);
    int slot=0;
    for (int i=0;i<8;++i) {
        if (fabsf(u2_sliders[i].x-r[0])<0.5f && fabsf(u2_sliders[i].y-r[1])<0.5f) { slot=i; break; }
        if (u2_sliders[i].us<u2_sliders[slot].us) slot=i;
    }
    u2_sliders[slot]=(U2Row){r[0],r[1],rail_w,r[3],0.0f,now,0.0f};
    pthread_mutex_unlock(&menu_text_lock);
}
static int u3_slider_debug(void) { static int on=-1; if (on<0) on=getenv("BB_U3_SLIDER_DEBUG")!=NULL; return on; }
static int u3_slider_steps(void) {   /* BB_U3_SLIDER_STEPS: how many left / right presses carry a slider from one end to the other */
    static int steps=-1;
    if (steps<0) { const char *e=getenv("BB_U3_SLIDER_STEPS"); steps=e ? atoi(e) : 14; if (steps<2) steps=2; if (steps>100) steps=100; }
    return steps;
}
static float u3_empty_y[8]; static uint64_t u3_empty_us[8];   /* y of the rows with an empty label, seen lately (ghost rows) */
/* U3: the < > of a row are two texts 16 wide and 33.6 tall, side by side (x 378 and 394 on the audio volume rows); seen lately, kept here (width < 20 is not a row) */
static U2Row u3_arrows[16];
static volatile uint64_t u3_gallery_stick_until; static volatile int u3_gallery_stick_dir;   /* a click on its < >: the left stick pushed that way for a moment */
static volatile uint64_t u3_gallery_us;   /* the art gallery viewer's < > text seen (Bonus > Récompenses > Galeries d'art) */
static void u3_arrow_note(const float *r) {
    const uint64_t now=now_us();
    pthread_mutex_lock(&menu_text_lock);
    int slot=0;
    for (int i=0;i<16;++i) {
        if (fabsf(u3_arrows[i].x-r[0])<0.5f && fabsf(u3_arrows[i].y-r[1])<0.5f) { slot=i; break; }
        if (u3_arrows[i].us<u3_arrows[slot].us) slot=i;
    }
    u3_arrows[slot]=(U2Row){r[0],r[1],r[2],r[3],0.0f,now,0.0f};
    pthread_mutex_unlock(&menu_text_lock);
}
static void u2_row_note(const float *r, float s5, float a) {
    const uint64_t now=now_us();
    pthread_mutex_lock(&menu_text_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<U2_ROWS;++i) {
        const U2Row *o=&u2_rows[i];
        if (o->us && fabsf(o->x-r[0])<0.5f && fabsf(o->y-r[1])<0.5f && fabsf(o->w-r[2])<0.5f && fabsf(o->h-r[3])<0.5f) { slot=i; break; }
        if (u2_rows[i].us<u2_rows[oldest].us) oldest=i;
    }
    if (slot<0) slot=oldest;
    u2_rows[slot]=(U2Row){r[0],r[1],r[2],r[3],s5,now,a};
    pthread_mutex_unlock(&menu_text_lock);
}
static volatile uint64_t u2_credits_us;   /* the credits: three paragraphs as tall as the screen (700+), scrolling, never highlighted (measured 2026-10-23) */
static volatile uint64_t u2_popup_us;   /* the chapter carousel of the Chapitres page: its panel is a "text" 683.8 x 318.8 at 298.1, 156.2 (it has no highlighted row) */
static volatile uint64_t u2_image;   /* U2 image base, seen by the text hook (F8 probe) */
static void u2_host_probe(uint64_t image);
/* the hover of a normal menu page asks for a jump of `u2_mj_delta` rows; the text hook (the game's own thread) does it and answers in u2_mj_done (1 done, -1 not possible) */
static volatile int u2_mj_req, u2_mj_done;   /* done: 1 jumped, -1 no page, -2 no row there */
static volatile float u2_mj_cx;   /* centre x of the text under the cursor: the main menu has two columns, Nouvelle partie and Options share a line */
static volatile float u2_mj_cy, u2_mj_uy;   /* (and the cursor itself) */   /* centre of the row under the cursor, in the units of the text rows */
static void u2_menu_jump_service(uint64_t image);
static void __attribute__((ms_abi)) u2_text_probe(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)r14;
    u2_image=image;
    if (u2_mj_req) u2_menu_jump_service(image);
    {
        float r[4], st[8], sz[2];
        if (safe_read(r,saved_register(5),16) && r[3]>=700.0f && r[3]<=800.0f && r[2]>=40.0f) u2_credits_us=now_us();
        if (runtime_u3_game && mouse_debug() && safe_read(r,saved_register(5),16) && r[3]>=150.0f) {   /* probe: shape of the U3 credits text */
            static uint64_t last_us; const uint64_t t=now_us();
            if (t-last_us>500000) { last_us=t; printf("U3 big text rect x=%.1f y=%.1f w=%.1f h=%.1f\n",r[0],r[1],r[2],r[3]); }
        }
        if (safe_read(r,saved_register(5),16) && fabsf(r[0]-298.1f)<1.5f && fabsf(r[1]-156.2f)<1.5f && fabsf(r[2]-683.8f)<2.0f && fabsf(r[3]-318.8f)<2.0f) u2_popup_us=now_us();
        if (safe_read(r,saved_register(5),16) && safe_read(sz,saved_register(3)+0x3c,8) && sz[0]>=100.0f && sz[1]>0.0f && sz[1]<=12.0f && r[3]>=24.0f && r[3]<=42.0f)
            u2_slider_note(r,sz[0]);
        /* U3: the rail of a slider, with a shape of its own (looser test, only read by the U3 menu code) */
        if (runtime_u3_game && safe_read(r,saved_register(5),16) && safe_read(sz,saved_register(3)+0x3c,8) && sz[0]>=60.0f && sz[1]>0.0f && sz[1]<=12.0f && r[3]>=20.0f && r[3]<=44.0f)
            u3_slider_note(r,sz[0]);
        /* BB_U3_SLIDER_DEBUG=1: every short text of a U3 menu (a rail is one) is logged with its rectangle and its size, once per
         * shape (20 lines a second at most): tells where the rail of a slider really is when U2's shape does not match. */
        if (runtime_u3_game && u3_slider_debug() && safe_read(r,saved_register(5),16) && r[3]>0.0f && r[3]<=14.0f && r[2]>=20.0f && safe_read(sz,saved_register(3)+0x3c,8)) {
            static struct { float x, y, w, h, s0, s1; } seen[24];
            static int n, lines; static uint64_t sec;
            const uint64_t t=now_us();
            int dup=0;
            for (int i=0;i<n;++i) if (fabsf(seen[i].x-r[0])<0.5f && fabsf(seen[i].y-r[1])<0.5f && fabsf(seen[i].w-r[2])<0.5f && fabsf(seen[i].h-r[3])<0.5f && fabsf(seen[i].s0-sz[0])<0.5f) { dup=1; break; }
            if (!dup && t-sec>1000000) { sec=t; lines=0; }
            if (!dup && lines<20) {
                if (n<24) { seen[n].x=r[0]; seen[n].y=r[1]; seen[n].w=r[2]; seen[n].h=r[3]; seen[n].s0=sz[0]; seen[n].s1=sz[1]; ++n; }
                ++lines;
                printf("Mouse: U3 short text rect %.1f %.1f %.1f %.1f size %.1f %.1f%s\n",r[0],r[1],r[2],r[3],sz[0],sz[1],
                       sz[0]>=100.0f && sz[1]<=12.0f ? " (U2's rail shape: a slider)" : "");
            }
        }
        /* BB_U3_ARROWS=1: every distinct small text (an arrow of a < > row?) with its rect, size and style, once per rect */
        if (runtime_u3_game && getenv("BB_U3_ARROWS") && safe_read(r,saved_register(5),16) && r[3]>0.0f && r[3]<=60.0f && r[2]>0.0f && r[2]<60.0f) {
            static float seen[64][4]; static int n;
            int dup=0;
            for (int i=0;i<n;++i) if (fabsf(seen[i][0]-r[0])<0.5f && fabsf(seen[i][1]-r[1])<0.5f && fabsf(seen[i][2]-r[2])<0.5f && fabsf(seen[i][3]-r[3])<0.5f) { dup=1; break; }
            if (!dup) {
                if (n<64) { memcpy(seen[n],r,16); ++n; } else n=0;
                float szz[2]={0,0}; safe_read(szz,saved_register(3)+0x3c,8);
                float sty[8]={0}; safe_read(sty,saved_register(4),32);
                printf("Mouse: U3 small text rect %.1f %.1f %.1f %.1f size %.1f %.1f style5 %.1f style6 %.1f\n",r[0],r[1],r[2],r[3],szz[0],szz[1],sty[5],sty[6]);
            }
        }
        /* BB_U3_CALLERS=1: for each distinct row-shaped text rect of a U3 menu, the words of the stack that point into the code (who draws it), once per rect */
        if (runtime_u3_game && getenv("BB_U3_CALLERS") && safe_read(r,saved_register(5),16) && r[3]>=24.0f && r[3]<=42.0f && r[2]>=20.0f) {
            static float seen[96][4]; static int n;
            int dup=0;
            for (int i=0;i<n;++i) if (fabsf(seen[i][0]-r[0])<0.5f && fabsf(seen[i][1]-r[1])<0.5f && fabsf(seen[i][2]-r[2])<0.5f && fabsf(seen[i][3]-r[3])<0.5f) { dup=1; break; }
            if (!dup) {
                if (n<96) { memcpy(seen[n],r,16); ++n; } else n=0;
                printf("Mouse: U3 text callers rect %.1f %.1f %.1f %.1f:",r[0],r[1],r[2],r[3]);
                for (int k=0;k<96;++k) {
                    uint64_t wd=0;
                    if (safe_read(&wd,guest_callback_rsp+8u*(uint64_t)k,8) && wd>image+0x1000 && wd<image+0x16488f0) printf(" +%d:%#llx",k*8,(unsigned long long)(wd-image));
                }
                printf("\n");
            }
        }
        /* U3: our "Touches clavier / souris" row: the layout object of its text holds the address of that text */
        if (runtime_u3_game && u2_keys_host && safe_read(r,saved_register(5),16) && r[3]>=24.0f && r[3]<=42.0f) {
            uint64_t lay[32];
            if (safe_read(lay,saved_register(3),sizeof lay)) for (int k=0;k<32;++k) if (lay[k]>=u2_keys_host && lay[k]<u2_keys_host+4u) {
                u3_keys_row_y=r[1]; u3_keys_row_us=now_us();
                static int shown; if (mouse_debug() && !shown) { shown=1; printf("Mouse: U3 keys row drawn at y %.1f (layout word %d)\n",r[1],k); }
                break;
            }
        }
        /* U3: a row whose label text is empty (width 0, in the left column) is a ghost, drawn but not meant to be chosen (Sortie audio under "Avant"): the hover skips it */
        if (runtime_u3_game && safe_read(r,saved_register(5),16) && safe_read(sz,saved_register(3)+0x3c,8) && sz[0]<0.5f && fabsf(r[0]-96.0f)<3.0f && r[3]>=24.0f && r[3]<=42.0f) {
            const uint64_t t=now_us();
            pthread_mutex_lock(&menu_text_lock);
            int slot=-1, oldest=0;
            for (int i=0;i<8;++i) { if (fabsf(u3_empty_y[i]-r[1])<0.5f) { slot=i; break; } if (u3_empty_us[i]<u3_empty_us[oldest]) oldest=i; }
            if (slot<0) slot=oldest;
            u3_empty_y[slot]=r[1]; u3_empty_us[slot]=t;
            pthread_mutex_unlock(&menu_text_lock);
        }
        if (runtime_u3_game && safe_read(r,saved_register(5),16) && r[2]>=10.0f && r[2]<=20.0f && r[3]>=33.0f && r[3]<=34.2f) u3_arrow_note(r);
        /* U3 art gallery viewer: its < > are two 38.4 wide glyphs in one rect 100, 600, 1080 x 60 (left and right aligned), no row, no 16 wide pair */
        if (runtime_u3_game && safe_read(r,saved_register(5),16) && fabsf(r[0]-100.0f)<1.0f && fabsf(r[1]-600.0f)<1.0f && fabsf(r[2]-1080.0f)<1.0f && fabsf(r[3]-60.0f)<1.0f) u3_gallery_us=now_us();
        /* U3 marks the selected row with style[6] at 1 (it never touches style[5]): kept as 3 in s5, the value the menu code knows.
         * style[7] is 1 on every U3 row: it is not the "chosen one of Oui / Non" sign of U2 and is left at 0. */
        if (safe_read(r,saved_register(5),16) && safe_read(st,saved_register(4),32) && r[3]>=24.0f && r[3]<=42.0f && r[2]>=20.0f)
            u2_row_note(r,runtime_u3_game ? st[6]*3.0f : st[5],runtime_u3_game ? 0.0f : st[7]);
    }
    if (!mouse_debug()) return;
    u2_host_probe(image);
    static U2ProbeText seen[96];
    static int seen_count;
    const uint64_t style=saved_register(4), layout=saved_register(3), rect=saved_register(5);
    const uint32_t halign=(uint32_t)saved_register(6);
    uint64_t stack_arg=0;
    U2ProbeText t;
    memset(&t,0,sizeof(t));
    if (!safe_read(&stack_arg,guest_callback_rsp+8,8) || !safe_read(t.rect,rect,16) || !safe_read(&t.w,layout+0x3c,4) ||
        !safe_read(&t.h,layout+0x40,4) || !safe_read(t.style,style,sizeof(t.style))) return;
    t.us=now_us();
    int slot=-1;
    for (int i=0;i<seen_count;++i) {
        const U2ProbeText *o=&seen[i];
        if (t.us-o->us<600000 && fabsf(o->rect[0]-t.rect[0])<0.5f && fabsf(o->rect[1]-t.rect[1])<0.5f &&
            fabsf(o->rect[2]-t.rect[2])<0.5f && fabsf(o->rect[3]-t.rect[3])<0.5f && fabsf(o->w-t.w)<3.0f) { slot=i; break; }
    }
    if (slot<0) for (int i=0;i<seen_count;++i) if (t.us-seen[i].us>=600000) { slot=i; break; }
    if (slot<0 && seen_count<96) slot=seen_count++;
    const int fresh=slot<0 || t.us-seen[slot].us>=600000;
    /* BB_TEXT_LOG_SELECT=1: only a change of the colour or of the selection markers is logged (style[1..3], [5], [6], [7]): the
     * opacity and style[4] of a text pulse at every frame and would flood the log. */
    static int select_only=-1;
    if (select_only<0) select_only=getenv("BB_TEXT_LOG_SELECT")!=NULL;
    int marker_changed=0;
    if (!fresh) for (int k=0;k<8;++k) if (k!=0 && k!=4 && seen[slot].style[k]!=t.style[k]) marker_changed=1;
    const int changed=!fresh && (select_only ? marker_changed : memcmp(seen[slot].style,t.style,sizeof(t.style))!=0);
    if (slot>=0) {
        const uint64_t keep=t.us;
        const int log_it=fresh || changed;
        const float old_style5=fresh ? 0.0f : seen[slot].style[5];
        seen[slot]=t; seen[slot].us=keep;
        if (!log_it) return;
        {   /* at most 20 lines per second: a page sliding in changes every rect at every frame */
            static uint64_t log_second; static int log_lines;
            if (t.us-log_second>1000000) { log_second=t.us; log_lines=0; }
            if (++log_lines>20) return;
        }
        printf("Mouse: U2 text %s rect %.1f %.1f %.1f %.1f size %.1f %.1f align %u/%u style",changed ? "CHANGED" : "new",
               t.rect[0],t.rect[1],t.rect[2],t.rect[3],t.w,t.h,halign,(uint32_t)stack_arg);
        for (int k=0;k<8;++k) printf(" %.2f",t.style[k]);   /* the 32 bytes read from the style */
        if (changed) printf(" (style[5] was %.2f)",old_style5);
        printf("\n");
    }
}
static const unsigned char u2_text_probe_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56};
/* U3 (collection, same menu framework as U2 but compiled differently): the text drawing routine is at 0x63d6f0, the same arguments
 * (rdx style, rcx laid-out text, r8 rectangle, r9d alignment, the alignment on the stack). The same 8 bytes move there. */
/* U2 text lookup probe (BB_MOUSE_DEBUG): the game's text function at 0xcddc60 (table, id) -> the string; every new id it is asked for is
 * logged with its text, to find the ids of the rows of a menu (the Commandes menu: "Rétablir les paramètres par défaut"...) and so the code
 * that builds them (a row is made with its id as an immediate). */
typedef const char *(__attribute__((sysv_abi)) *u2_text_fn)(uint64_t, uint64_t);
/* The "Commandes" row of the Commandes menu (it opens the picture of the controller; text id 0xdd40dc7b, its own: the title, "COMMANDES", and
 * the Options entry have other ids). The port gives it the label "Touches clavier / souris" and its own action (u2_keys_label_patch,
 * u2_menu_buttons): the table where the game looks the text up is the one this hook sees asked for that id. */
#define U2_KEYS_TEXT_ID 0xdd40dc7bu
#define U2_KEYS_HOST_TEXT_ID 0x62501513u
/* The screen of the controller's picture (Options > Commandes > "Commandes") draws its labels through this same lookup, every frame: the
 * time of the last one of a few of them says the screen is up. It has no menu rows, so the mouse code would not know it is a menu. */
static volatile uint64_t u2_layout_us;
/* "COMPARER LES AMIS" (text id 0x9a8d0b3e), the prompt of the statistics screens and of the speedrun lists under them: the online comparison does not work here.
 * The prompt bar (function 0x5a4f10) draws it in the branch of kind 7 (test at 0x5a54b6): the hook at 0x5a54c7 only runs in that branch (it notes the time), and the jump at 0x5a54cc
 * is made unconditional so that the branch ends there, past the drawing of icon and text (the way U1 does it). While the prompt would be drawn, the cross is held back, except on
 * the rows that open a sub menu (Facile, Normal... : no value column). */
static volatile uint64_t u2_compare_us;
static volatile int u2_sel_plain_row;   /* the selected row is a 768 wide row of a list (no value column) */
static void __attribute__((ms_abi)) u2_compare_prompt(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) { (void)unused0; (void)unused1; (void)image; (void)r14; u2_compare_us=now_us(); }
static const unsigned char u2_compare_cmp_code[]={0x41,0x83,0x7f,0x48,0x00,0x0f,0x84};   /* 0x5a54c7: cmp dword [r15+0x48],0 (5 bytes moved) */
static const unsigned char u2_compare_jz_code[]={0x0f,0x84,0x8b,0x04,0x00,0x00};   /* 0x5a54cc: jz 0x5a595d */
static const unsigned char u2_compare_jz_patch[]={0xe9,0x11,0x06,0x00,0x00,0x90};   /* jmp 0x5a5ae2 */
/* U3 (collection): the same "COMPARER LES AMIS" prompt (same text id 0x9a8d0b3e), drawn by the prompt bar at 0x627520. Its branch is the kind 0x80
 * (test at 0x627d65, jump to the block at 0x62830b which builds the label); the kind is compared against -1 so the branch is never taken and the bar
 * goes on with the next prompt (0x6284e3), as if the item were unknown. No callback: a plain 7-byte patch. */
static const unsigned char u3_compare_cmp_code[]={0x41,0x81,0xf9,0x80,0x00,0x00,0x00};   /* 0x627d65: cmp r9d,0x80 */
static const unsigned char u3_compare_cmp_patch[]={0x41,0x81,0xf9,0xff,0xff,0xff,0xff};  /* cmp r9d,-1 */
/* U3: like U1 (u1_bar_flags), the prompt bar tests bit 0 of the flags in r9d for "SÉLECTIONNER" (0x6277e8: mov edx,r9d; and edx,1). The hook clears that bit in
 * the saved r9 while a statistics line with its value on the right is selected (u3_sel_value, noted by u3_menu_buttons). */
static volatile uint64_t u3_hide_select_us;
static void __attribute__((ms_abi)) u3_bar_flags(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image; (void)r14;
    if (now_us()-u3_hide_select_us>=150000) return;
    uint64_t r9=saved_register(6);
    r9&=~(uint64_t)1;
    safe_write(guest_callback_rsp-8u*6u,&r9,8);
}
static const unsigned char u3_bar_flags_code[]={0x44,0x89,0xca,0x83,0xe2,0x01};   /* mov edx, r9d; and edx, 1 */
static volatile int u3_sel_value;   /* the selected U3 row has its value on the right: the cross is held back (the comparison with friends does not work offline) */
static volatile uint64_t u2_keys_tables[8];   /* the text tables seen asked for it (a table asks its parent first: all of them are patched) */
static void __attribute__((ms_abi)) u2_text_lookup_probe(uint64_t table, uint64_t id, uint64_t image, uint64_t r14) {
    (void)r14;
    {
        const uint32_t k=(uint32_t)id;   /* "Réinitialiser la caméra", "Mode photo", "Changer d'épaule en visant": only that picture has them */
        if (k==0x4158a30du || k==0x9ef63173u || k==0x36fc7ed5u) u2_layout_us=now_us();
    }
    if ((uint32_t)id==U2_KEYS_TEXT_ID) {
        for (int i=0;i<8;++i) { if (u2_keys_tables[i]==table) break; if (!u2_keys_tables[i]) { u2_keys_tables[i]=table; break; } }
    }
    static uint32_t seen[8192]; static int seen_count, busy;
    if (busy || !mouse_debug() || image<0x10000 || runtime_u3_game) return;   /* (the debug lookup below is U2's address) */
    if (!u2_image) u2_image=image;
    const uint32_t id32=(uint32_t)id;
    for (int i=0;i<seen_count;++i) if (seen[i]==id32) return;
    if (seen_count>=8192) return;
    seen[seen_count++]=id32;
    busy=1;
    const char *s=((u2_text_fn)(uintptr_t)(image+0xcddc60))(table,id32);
    char buf[72]={0};
    if (s && (uint64_t)(uintptr_t)s>0x10000) { size_t n=sizeof(buf)-1; while (n>0 && !safe_read(buf,(uint64_t)(uintptr_t)s,n)) --n; buf[n]=0; for (char *p=buf;*p;++p) if ((unsigned char)*p<32) *p=' '; }
    uint64_t ret=0;   /* the code that asked (at the hook the return address is on top of the stack) */
    safe_read(&ret,guest_callback_rsp,8);
    printf("Mouse: U2 text id %08x table %#llx caller image+%#llx \"%s\"\n",id32,(unsigned long long)table,(unsigned long long)(ret-image),buf);
    if (id32==U2_KEYS_TEXT_ID || id32==0xb69a538au || id32==0xc9b64fcdu) {   /* the stack, for the chain of callers: words that point into the code */
        printf("Mouse: U2 stack for %08x:",id32);
        for (int k=0;k<96;++k) {
            uint64_t w=0;
            if (safe_read(&w,guest_callback_rsp+8u*(uint64_t)k,8) && w>image+0x1000 && w<image+0xfe44b0) printf(" +%d:%#llx",k*8,(unsigned long long)(w-image));
        }
        printf("\n");
    }
    busy=0;
}
/* U2's menu builder (0x5c53a0, BB_MOUSE_DEBUG): it builds a menu from a definition the game loads as data (func 0xa15e70 gives it by the
 * hash 0x74ffceeb: {count, array of 0x70-byte records}, each record with its own list of the same shape at +0x38). Logged here, to see how a
 * row is described (and so how one more could be given). */
typedef uint64_t (__attribute__((sysv_abi)) *u2_get_data_fn)(uint64_t);
__attribute__((unused)) static void u2_dump_records(uint64_t p, int depth) {
    int32_t count=0; uint64_t arr=0;
    {
        uint8_t head[0x30]={0};
        const int got=safe_read(head,p,sizeof(head));
        printf("Mouse: U2 menudef %*shead %#llx (read %d):",depth*2,"",(unsigned long long)p,got);
        for (int b=0;b<(int)sizeof(head);++b) printf(" %02x",head[b]);
        printf("\n");
    }
    if (!safe_read(&count,p,4) || !safe_read(&arr,p+8,8) || count<=0 || count>400 || arr<0x10000) return;
    for (int i=0;i<count;++i) {
        uint8_t rec[0x70];
        if (!safe_read(rec,arr+0x70u*(uint64_t)i,0x70)) break;
        printf("Mouse: U2 menudef %*s[%d] at %#llx:",depth*2,"",i,(unsigned long long)(arr+0x70u*(uint64_t)i));
        for (int b=0;b<0x70;++b) printf(" %02x",rec[b]);
        printf("\n");
        uint64_t sub=0;
        memcpy(&sub,rec+0x38,8);
        if (depth<2 && sub>0x10000) u2_dump_records(sub,depth+1);
    }
}
/* a list head: {int count, 0, ptr to the array of 0x70-byte records} */
static int u2_is_list_head(uint64_t p) {
    int32_t count=0, zero=-1; uint64_t arr=0;
    return p>0x10000000ull && safe_read(&count,p,4) && safe_read(&zero,p+4,4) && safe_read(&arr,p+8,8) && count>0 && count<=64 && zero==0 && arr>0x10000000ull && arr<0x1400000000ull;
}
static int u2_tree_lines;
static void u2_dump_tree(uint64_t p, int depth) {
    int32_t count=0; uint64_t arr=0;
    safe_read(&count,p,4); safe_read(&arr,p+8,8);
    for (int i=0;i<count && u2_tree_lines<400;++i) {
        uint8_t rec[0x70];
        const uint64_t at=arr+0x70u*(uint64_t)i;
        if (!safe_read(rec,at,0x70)) break;
        ++u2_tree_lines;
        printf("Mouse: U2 tree %*s[%d] %#llx:",depth*2,"",i,(unsigned long long)at);
        for (int b=0;b<0x70;++b) printf(" %02x",rec[b]);
        printf("\n");
        if (depth<6) for (int off=0;off<=0x68;off+=8) {
            uint64_t q=0; memcpy(&q,rec+off,8);
            if (u2_is_list_head(q)) { printf("Mouse: U2 tree %*s  -> list at +%#x\n",depth*2,"",off); u2_dump_tree(q,depth+1); }
            else if (q>0x1000000000ull && q<0x1400000000ull) {   /* some other pointer: what it points at, as bytes and as text */
                uint8_t t[64];
                if (safe_read(t,q,sizeof(t))) {
                    printf("Mouse: U2 tree %*s  -> +%#x = %#llx:",depth*2,"",off,(unsigned long long)q);
                    for (int b=0;b<64;++b) printf(" %02x",t[b]);
                    printf("  \"");
                    for (int b=0;b<64;++b) printf("%c",t[b]>=32 && t[b]<127 ? t[b] : '.');
                    printf("\"\n");
                }
            }
        }
    }
}
/* every record of the definition (whole tree) that holds the text id of the "Commandes" row or of "Rétablir" in one of its 4-byte words:
 * where they live (the list they are in, its name, the record bytes) says where a row of ours could be added */
static int u2_find_lines;
static void u2_find_ids(uint64_t list, int depth, const char *owner) {
    int32_t count=0; uint64_t arr=0;
    if (depth>8 || !safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<=0 || count>400) return;
    char name[40]={0};
    if (!safe_read(name,list+0x14,32) || name[0]<'A' || name[0]>'Z') snprintf(name,sizeof(name),"%s",owner);
    for (int i=0;i<count;++i) {
        uint8_t rec[0x70];
        const uint64_t at=arr+0x70u*(uint64_t)i;
        if (!safe_read(rec,at,0x70)) break;
        for (int off=0;off<=0x6c;off+=4) {
            uint32_t w; memcpy(&w,rec+off,4);
            if ((w==U2_KEYS_TEXT_ID || w==0xb69a538au) && u2_find_lines<60) {
                ++u2_find_lines;
                printf("Mouse: U2 find %08x depth %d list %#llx (%s) head count %d record %d at %#llx +%#x:",w,depth,(unsigned long long)list,name,count,i,(unsigned long long)at,off);
                for (int b=0;b<0x70;++b) printf(" %02x",rec[b]);
                printf("\n");
            }
        }
        for (int off=0;off<=0x68;off+=8) {
            uint64_t q; memcpy(&q,rec+off,8);
            if (u2_is_list_head(q)) u2_find_ids(q,depth+1,name);
        }
    }
}
/* A row of ours in the Commandes menu: the builder (0x5c53a0) makes every menu from the definition (func 0xa15e70, hash 0x74ffceeb), one
 * 0x70-byte record per row, and the page PAGE_CONTROLLER_LAYOUT is that menu (aiming, sensitivity... then the "Commandes" row, text
 * id dd40dc7b, then "Rétablir"). At the entry of the builder a copy of that array with one more record is put in place: a clone of the
 * "Commandes" record, with an id of its own and the text id 0x62501513, which carries the label (u2_keys_label_patch). The clone sits right
 * after "Commandes". Off with BB_U2_ROW_INSERT=0. Records are never freed by the game (it keeps pointers to them). */
static volatile int u2_row_inserted;
#define U2_ROW_NEW_ID 0x62501514u
static void *u2_def_alloc(size_t size, uint64_t near_to) {
#ifdef _WIN32
    /* the game's own pointers are all below 2^40: the new array is put in the same range, close to the old one */
    (void)near_to;
    /* the emulator owns the range around the definition: the free regions below 2^40 are walked, the first one big enough is taken */
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t addr=0x100000000ull;addr<0x10000000000ull && VirtualQuery((LPCVOID)addr,&mbi,sizeof(mbi));) {
        const uintptr_t base=(uintptr_t)mbi.BaseAddress, end=base+mbi.RegionSize;
        if (mbi.State==MEM_FREE) {
            const uintptr_t at=(base+0xffffu)&~(uintptr_t)0xffffu;
            if (at+size+0x10000u<=end) {
                void *p=VirtualAlloc((LPVOID)at,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
                if (p) return p;
            }
        }
        addr=end;
    }
    return VirtualAlloc(NULL,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#else
    (void)near_to;
    return malloc(size);
#endif
}
static void u2_menu_def_patch(uint64_t image) {
    static int enabled=-1, tries;
    if (enabled<0) { const char *e=getenv("BB_U2_ROW_INSERT"); enabled=!(e && e[0]=='0'); }
    if (!enabled || tries>=40) return;
    const uint64_t def=((u2_get_data_fn)(uintptr_t)(image+0xa15e70))(0x74ffceebu);
    int32_t pages=0; uint64_t parr=0;
    if (def<=0x10000 || !safe_read(&pages,def,4) || !safe_read(&parr,def+8,8) || pages<=0 || pages>400) return;
    ++tries;
    if (mouse_debug()) {   /* debug (once): the pages of the definition, their records (type +0x10, id +8, text id +0x28) */
        static int listed;
        if (!listed) {
            listed=1;
            for (int i=0;i<pages;++i) {
                uint64_t list=0, arr=0; char name[32]={0}; int32_t count=0;
                if (!safe_read(&list,parr+0x70u*(uint64_t)i+0x38,8) || list<0x10000 || !safe_read(name,list+0x14,31) || !safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<=0 || count>64) continue;
                printf("Runtime: U2 def page %d %.31s (%d records at %#llx):",i,name,count,(unsigned long long)arr);
                for (int r=0;r<count;++r) { uint32_t type=0, id=0, text=0; safe_read(&type,arr+0x70u*(uint64_t)r+0x10,4); safe_read(&id,arr+0x70u*(uint64_t)r+8,4); safe_read(&text,arr+0x70u*(uint64_t)r+0x28,4); printf(" [%d t%u %x/%x]",r,type,id,text); }
                printf("\n");
            }
        }
    }
    {   /* The photo mode is off in this port (the keys do nothing): the last row of the Jeu pages ("Mode photo activé avec", text id a3da9f13) is dropped by
         * one less record in the list's count; the game builds only the records it is told about. Off with BB_U2_PHOTO_ROW=1. */
        const char *e=getenv("BB_U2_PHOTO_ROW");
        if (!(e && e[0]=='1')) for (int i=0;i<pages;++i) {
            uint64_t list=0, arr=0; char name[32]={0}; int32_t count=0;
            if (!safe_read(&list,parr+0x70u*(uint64_t)i+0x38,8) || list<0x10000 || !safe_read(name,list+0x14,31) || strncmp(name,"PAGE_GAMEPLAY",13)!=0) continue;
            if (!safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<2 || count>64 || arr<0x10000) continue;
            uint32_t type=0, text=0;
            if (!safe_read(&type,arr+0x70u*(uint64_t)(count-1)+0x10,4) || !safe_read(&text,arr+0x70u*(uint64_t)(count-1)+0x28,4) || type!=3 || text!=0xa3da9f13u) continue;
            const int32_t ncount=count-1;
            if (safe_write(list,&ncount,4)) printf("Runtime: U2 photo mode row removed from %.31s (%d rows left)\n",name,ncount);
        }
    }
    for (int i=0;i<pages;++i) {
        uint64_t list=0; char name[32]={0};
        if (!safe_read(&list,parr+0x70u*(uint64_t)i+0x38,8) || list<0x10000 || !safe_read(name,list+0x14,31)) continue;
        if (strncmp(name,"PAGE_CONTROLLER_LAYOUT",23)!=0) continue;
        int32_t count=0; uint64_t arr=0;
        if (!safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<=0 || count>64 || arr<0x10000) return;
        uint8_t *old=malloc((size_t)count*0x70u);
        if (!old) return;
        int at=-1, have=0;
        if (safe_read(old,arr,(size_t)count*0x70u)) {
            for (int r=0;r<count;++r) {
                uint32_t type, text;
                memcpy(&type,old+0x70u*(size_t)r+0x10,4); memcpy(&text,old+0x70u*(size_t)r+0x28,4);
                if (text==U2_KEYS_HOST_TEXT_ID) have=1;
                if (text==U2_KEYS_TEXT_ID && type==2) at=r;
            }
        }
        if (have) { u2_row_inserted=1; free(old); return; }   /* the definition was kept: done already */
        if (at<0) { printf("Runtime: U2 keys row: the Commandes record was not found in the page\n"); free(old); tries=40; return; }
        uint8_t *fresh=u2_def_alloc((size_t)(count+1)*0x70u,arr);
        if (!fresh) { printf("Runtime: U2 keys row: no memory near the definition\n"); free(old); tries=40; return; }
        memcpy(fresh,old,0x70u*(size_t)(at+1));
        memcpy(fresh+0x70u*(size_t)(at+1),old+0x70u*(size_t)at,0x70);   /* the clone, then the rest */
        memcpy(fresh+0x70u*(size_t)(at+2),old+0x70u*(size_t)(at+1),0x70u*(size_t)(count-at-1));
        const uint32_t new_id=U2_ROW_NEW_ID, new_text=U2_KEYS_HOST_TEXT_ID;
        memcpy(fresh+0x70u*(size_t)(at+1)+8,&new_id,4);
        memcpy(fresh+0x70u*(size_t)(at+1)+0x28,&new_text,4);
        const int32_t ncount=count+1;
        const uint64_t nptr=(uint64_t)(uintptr_t)fresh;
        if (safe_write(list+8,&nptr,8) && safe_write(list,&ncount,4)) {
            u2_row_inserted=1;
            printf("Runtime: U2 keys row inserted in %s (%d rows, after row %d; new array %#llx)\n",name,ncount,at,(unsigned long long)nptr);
        } else printf("Runtime: U2 keys row: the definition cannot be written\n");
        free(old);
        return;
    }
}
static volatile uint64_t u2_host_cand;   /* first argument of the menu builder (0x5c53a0): the menu host? */
static void __attribute__((ms_abi)) u2_menu_builder_probe(uint64_t menu, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    u2_host_cand=menu;
    if (image>=0x10000) u2_menu_def_patch(image);
    if (!mouse_debug() || image<0x10000) return;
    static int calls;
    if (calls++>=1) return;
    const uint64_t p=((u2_get_data_fn)(uintptr_t)(image+0xa15e70))(0x74ffceebu);
    printf("Mouse: U2 menu builder: menu %#llx definition %#llx\n",(unsigned long long)menu,(unsigned long long)p);
    if (p>0x10000) u2_find_ids(p,0,"(root)");
    int32_t count=0; uint64_t arr=0;
    if (p<=0x10000 || !safe_read(&count,p,4) || !safe_read(&arr,p+8,8)) return;
    for (int i=0;i<count && i<200;++i) {   /* the page named PAGE_CONTROLS, whole tree below it */
        uint64_t sub=0; char name[40]={0};
        if (!safe_read(&sub,arr+0x70u*(uint64_t)i+0x38,8) || sub<0x10000) continue;
        if (!safe_read(name,sub+0x14,32)) continue;
        if (i==76 || !strncmp(name,"PAGE_CONTROLLER_LAYOUT",23) || !strncmp(name,"PAGE_CONTROLS",14)) {
            printf("Mouse: U2 tree page %s: page record %#llx, list %#llx\n",name,(unsigned long long)(arr+0x70u*(uint64_t)i),(unsigned long long)sub);
            u2_tree_lines=0;
            u2_dump_tree(sub,0);
        }
    }
}
/* U3: the same photo mode row ("Mode photo activé avec", text id a3da9f13, last record of its page) dropped from the menu definition (func 0x11f2e30, hash 0x74ffceeb),
 * from U3's menu builder (0x649660). Off with BB_U3_PHOTO_ROW=1. */
/* U3's definition tree does not have U2's page layout: walk every list of the tree (same 0x70-byte records) and find the record holding the text id in any word */
static void u3_photo_walk(uint64_t list, int depth) {
    int32_t count=0; uint64_t arr=0;
    if (depth>8 || !safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<=0 || count>400) return;
    for (int r=0;r<count;++r) {
        uint8_t rec[0x70];
        if (!safe_read(rec,arr+0x70u*(uint64_t)r,0x70)) break;
        for (int off=0;off<=0x6c;off+=4) {
            uint32_t w; memcpy(&w,rec+off,4);
            if (w!=0xa3da9f13u) continue;
            printf("Runtime: U3 photo mode row: list %#llx depth %d record %d of %d word +%#x\n",(unsigned long long)list,depth,r,count,off);
            if (r==count-1) { const int32_t nc=count-1; if (safe_write(list,&nc,4)) printf("Runtime: U3 photo mode row removed (%d rows left)\n",nc); return; }
        }
        for (int off=0;off<=0x68;off+=8) {
            uint64_t q; memcpy(&q,rec+off,8);
            if (u2_is_list_head(q)) u3_photo_walk(q,depth+1);
        }
    }
}
/* U3 probe (BB_MOUSE_DEBUG=1): the lists holding the "Commandes" (dd40dc7b) or "Rétablir" (b69a538a) text ids of U2, with all their records */
static void u3_cmd_walk(uint64_t list, int depth) {
    int32_t count=0; uint64_t arr=0;
    if (depth>8 || !safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<=0 || count>400) return;
    int hit=0;
    for (int r=0;r<count && !hit;++r) {
        uint8_t rec[0x70];
        if (!safe_read(rec,arr+0x70u*(uint64_t)r,0x70)) break;
        for (int off=0;off<=0x6c;off+=4) { uint32_t w; memcpy(&w,rec+off,4); if (w==0xdd40dc7bu || w==0xb69a538au) { hit=1; printf("Runtime: U3 cmd word %x at record %d +%#x\n",w,r,off);
            static int dumps; if (dumps++<6) { const uint64_t a=arr+0x70u*(uint64_t)r+(uint64_t)off-0x70u; uint32_t d[96]; if (safe_read(d,a,sizeof d)) { printf("Runtime: U3 cmd dump from %#llx (word at +0x70):",(unsigned long long)a); for (int k=0;k<96;++k) printf("%s%x",k%8?" ":"\n  ",d[k]); printf("\n"); } } } }
    }
    if (hit) {
        printf("Runtime: U3 cmd list %#llx depth %d (%d records at %#llx):",(unsigned long long)list,depth,count,(unsigned long long)arr);
        for (int r=0;r<count;++r) { uint32_t type=0, id=0, text=0; safe_read(&type,arr+0x70u*(uint64_t)r+0x10,4); safe_read(&id,arr+0x70u*(uint64_t)r+8,4); safe_read(&text,arr+0x70u*(uint64_t)r+0x28,4); printf(" [%d t%u %x/%x]",r,type,id,text); }
        printf("\n");
    }
    for (int r=0;r<count;++r) {
        uint8_t rec[0x70];
        if (!safe_read(rec,arr+0x70u*(uint64_t)r,0x70)) break;
        for (int off=0;off<=0x68;off+=8) {
            uint64_t q; memcpy(&q,rec+off,8);
            if (u2_is_list_head(q)) u3_cmd_walk(q,depth+1);
        }
    }
}
/* U3: the same row of ours in Options > Commandes as U2 (see u2_menu_def_patch), but U3's records are 0x68 bytes long (the fields keep their offsets:
 * id +8, type +0x10, text id +0x28, target page +0x48). The list holding the "Commandes" record (type 2, text dd40dc7b) is found by walking the tree;
 * its array is copied with a clone of that record added right after it. Off with BB_U2_ROW_INSERT=0. */
#define U3_REC 0x68u
static int u3_row_walk(uint64_t list, int depth, int *budget) {
    int32_t count=0; uint64_t arr=0;
    if (depth>8 || --*budget<0 || !safe_read(&count,list,4) || !safe_read(&arr,list+8,8) || count<=0 || count>400 || arr<0x10000) return 0;
    uint8_t *old=malloc((size_t)count*0x70u);
    if (!old) return 0;
    if (!safe_read(old,arr,(size_t)count*0x70u)) { free(old); return 0; }
    int at=-1, have=0;
    for (int r=0;r<count;++r) {
        uint32_t type, text;
        memcpy(&type,old+U3_REC*(size_t)r+0x10,4); memcpy(&text,old+U3_REC*(size_t)r+0x28,4);
        if (text==U2_KEYS_HOST_TEXT_ID) have=1;
        if (text==U2_KEYS_TEXT_ID && type==2) at=r;
    }
    if (have) { u2_row_inserted=1; free(old); return 1; }
    if (at>=0) {
        uint8_t *fresh=u2_def_alloc((size_t)(count+1)*U3_REC,arr);
        if (!fresh) { printf("Runtime: U3 keys row: no memory near the definition\n"); free(old); return 1; }
        memcpy(fresh,old,U3_REC*(size_t)(at+1));
        memcpy(fresh+U3_REC*(size_t)(at+1),old+U3_REC*(size_t)at,U3_REC);
        memcpy(fresh+U3_REC*(size_t)(at+2),old+U3_REC*(size_t)(at+1),U3_REC*(size_t)(count-at-1));
        const uint32_t new_id=U2_ROW_NEW_ID, new_text=U2_KEYS_HOST_TEXT_ID;
        memcpy(fresh+U3_REC*(size_t)(at+1)+8,&new_id,4);
        memcpy(fresh+U3_REC*(size_t)(at+1)+0x28,&new_text,4);
        const int32_t ncount=count+1;
        const uint64_t nptr=(uint64_t)(uintptr_t)fresh;
        if (safe_write(list+8,&nptr,8) && safe_write(list,&ncount,4)) {
            u2_row_inserted=1;
            printf("Runtime: U3 keys row inserted (%d rows, after row %d; list %#llx, new array %#llx)\n",ncount,at,(unsigned long long)list,(unsigned long long)nptr);
        } else printf("Runtime: U3 keys row: the definition cannot be written\n");
        free(old);
        return 1;
    }
    for (int r=0;r<count;++r) for (int off=0;off<=0x68;off+=8) {   /* children: scanned on both strides */
        uint64_t q; memcpy(&q,old+U3_REC*(size_t)r+off,8);
        if (u2_is_list_head(q) && u3_row_walk(q,depth+1,budget)) { free(old); return 1; }
    }
    for (int r=0;r<count;++r) for (int off=0;off<=0x68;off+=8) {
        uint64_t q; memcpy(&q,old+0x70u*(size_t)r+off,8);
        if (u2_is_list_head(q) && u3_row_walk(q,depth+1,budget)) { free(old); return 1; }
    }
    free(old);
    return 0;
}
static void __attribute__((ms_abi)) u3_menu_builder_hook(uint64_t menu, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)menu; (void)unused; (void)r14;
    static int tries, probed, row_tries;
    {
        static int enabled=-1;
        if (enabled<0) { const char *e=getenv("BB_U2_ROW_INSERT"); enabled=!(e && e[0]=='0'); }
        if (enabled && image>=0x10000 && !u2_row_inserted && row_tries<40) {
            const uint64_t d=((u2_get_data_fn)(uintptr_t)(image+0x11f2e30))(0x74ffceebu);
            if (d>0x10000) { ++row_tries; int budget=20000; if (!u3_row_walk(d,0,&budget) && row_tries==40) printf("Runtime: U3 keys row: the Commandes record was not found\n"); }
        }
    }
    if (mouse_debug() && image>=0x10000 && !probed) {
        const uint64_t d=((u2_get_data_fn)(uintptr_t)(image+0x11f2e30))(0x74ffceebu);
        if (d>0x10000) { probed=1; u3_cmd_walk(d,0); printf("Runtime: U3 cmd probe done\n"); }
    }
    const char *e=getenv("BB_U3_PHOTO_ROW");
    if (image<0x10000 || tries>=40 || (e && e[0]=='1')) return;
    const uint64_t def=((u2_get_data_fn)(uintptr_t)(image+0x11f2e30))(0x74ffceebu);
    int32_t pages=0; uint64_t parr=0;
    if (def<=0x10000 || !safe_read(&pages,def,4) || !safe_read(&parr,def+8,8) || pages<=0 || pages>400) return;
    ++tries;
    (void)pages; (void)parr;
    u3_photo_walk(def,0);
}
static const unsigned char u2_menu_builder_code[]={0x55,0x48,0x89,0xe5,0x41,0x57};
static const unsigned char u2_text_lookup_code[]={0x55,0x48,0x89,0xe5,0x41,0x56};   /* 0xcddc00, the search the callers use (0xcddc60 wraps it with a default, no caller) */
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
/* U1 1.02: the same function (the prompt bar, 0x128c70, flags in r9d = param_7) draws the "SÉLECTIONNER" prompt when bit 0 of the flags is
 * set. In Statistiques that prompt has nothing to do on a statistic (a click does nothing there, see menu_mouse_buttons): the hook at the
 * start of the function clears the bit in the saved r9 (restored by the thunk) while u1_hide_select_us is recent. */
static volatile uint64_t u1_hide_select_us;
static void __attribute__((ms_abi)) u1_bar_flags(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image; (void)r14;
    if (now_us()-u1_hide_select_us>=150000) return;
    uint64_t r9=saved_register(6);
    r9&=~(uint64_t)1;
    safe_write(guest_callback_rsp-8u*6u,&r9,8);
}
static const unsigned char u1_bar_flags_code[]={0x45,0x89,0xcf,0x44,0x89,0xbd,0xa8,0xfd,0xff,0xff};   /* mov r15d, r9d; mov [rbp-0x258], r15d */
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
typedef int64_t (__attribute__((sysv_abi)) *u2_vcall_fn)(uint64_t);
/* U2 list widget update (0x57e410, found with the write watch and Ghidra): FUN_0057e410(widget, frame) runs every frame for each selector / list. The widget keeps
 * its value in a data source object at widget+0x18 whose virtual functions are: +0 get, +8 set(value), +0x30 minimum, +0x38 maximum. The game moves the selection
 * of a list by calling set; the widget then notices the change by itself (+0x22c) and updates the picture. So the scroll bar does what the game does: set(value). */
typedef struct { uint64_t w, ds; int cur, lo, hi; uint64_t us; float ry, rh; } U2Widget;   /* ry, rh: y and height of the control, the units of the text rows (+0x94, +0x9c like the children of a page) */
#define U2_WIDGETS 32
static U2Widget u2_widgets[U2_WIDGETS];
static volatile uint64_t u2_jump_ds;
static volatile int32_t u2_jump_val;
static volatile int u2_jump_pending;
/* the scroll bar moves the view only (the first row seen, widget +0x248), not the selection: written by the widget hook, on the game's thread */
static volatile uint64_t u2_scroll_w;
static volatile int32_t u2_scroll_top;
static volatile int u2_scroll_pending;
static uint64_t u2_pick_self;   /* the widget u2_widget_pick has chosen */
/* Putting the first row seen (widget+0x248) back after a jump to a row at the edge was tried (2026-10-23) and failed: the list view went to the other end
 * and no row was highlighted any more (the menu mouse went off). What is done now: see u2_widget_hook. BB_U2_EDGE=0 turns it off. */
static int u2_edge_on(void) { static int on=-1; if (on<0) { const char *e=getenv("BB_U2_EDGE"); on=!(e && atoi(e)==0); } return on; }
static int u2_keep_view_on(void) { return u2_edge_on(); }
static volatile int u2_jump_keep;  /* the jump is a hover: the list must not scroll (value = rows seen) */
static volatile int u2_probe_row=-1;   /* debug: row (0..) of the selected line on screen, and when it was seen */
static volatile uint64_t u2_probe_us;
typedef void (__attribute__((sysv_abi)) *u2_set_fn)(uint64_t, int);
/* debug (BB_MOUSE_DEBUG): a Control that holds children (count at +0xb4, array at +0xb8; the focused child has the byte at +8 set, Ghidra 0x5cd5c0): list them,
 * and again each time the focused child changes */
static void u2_container_probe(uint64_t self, uint64_t image) {
    static struct { uint64_t self; int focus; } seen[128];
    int32_t cnt=0; uint64_t arr=0;
    if (!safe_read(&cnt,self+0xb4,4) || cnt<2 || cnt>64 || !safe_read(&arr,self+0xb8,8) || arr<0x10000) return;
    uint64_t ch[64];
    if (!safe_read(ch,arr,(size_t)cnt*8)) return;
    int focus=-1, nfocus=0;
    uint8_t flag[64];
    for (int i=0;i<cnt;++i) {
        uint64_t vt=0;
        if (ch[i]<0x10000 || !safe_read(&vt,ch[i],8) || vt<image || vt>=image+0x2000000) return;
        flag[i]=0; safe_read(&flag[i],ch[i]+8,1);
        if (flag[i]) { if (focus<0) focus=i; ++nfocus; }
    }
    {   /* which bytes of the children (and of the container) go 0 <-> 1 while the focus moves? */
        static struct { uint64_t self; uint8_t b[17][0x200]; int n; } snap[96];
        int s=-1;
        for (int i=0;i<96;++i) if (snap[i].self==self) { s=i; break; }
        if (s<0) for (int i=0;i<96;++i) if (!snap[i].self) { s=i; snap[i].self=self; snap[i].n=-1; break; }
        if (s>=0) {
            uint8_t now_b[17][0x200];
            int ok=safe_read(now_b[0],self,0x200);
            for (int i=0;i<cnt && i<16 && ok;++i) ok=safe_read(now_b[i+1],ch[i],0x200);
            if (ok) {
                if (snap[s].n>=0) {
                    int printed=0;
                    for (int o=0;o<0x200;o+=4) {   /* small ints of the container itself: an index of the focused child? */
                        int32_t a, b; memcpy(&a,&snap[s].b[0][o],4); memcpy(&b,&now_b[0][o],4);
                        if (a!=b && a>=-1 && a<64 && b>=-1 && b<64 && printed<6) { printf("Mouse: U2 flip container %#llx self int +%#x: %d->%d\n",(unsigned long long)self,o,a,b); ++printed; }
                    }
                    for (int k=0;k<=cnt && k<=16 && printed<12;++k) for (int o=0;o<0x200 && printed<12;++o)
                        if (snap[s].b[k][o]!=now_b[k][o] && (snap[s].b[k][o]<=1 && now_b[k][o]<=1) && (o<0x60 || o>=0x80)) {
                            printf("Mouse: U2 flip container %#llx %s%d +%#x: %d->%d\n",(unsigned long long)self,k ? "child " : "self ",k ? k-1 : 0,o,snap[s].b[k][o],now_b[k][o]); ++printed;
                        }
                }
                memcpy(snap[s].b,now_b,sizeof(snap[s].b)); snap[s].n=cnt;
            }
        }
    }
    if (cnt==6 || cnt==7) {   /* steady state dump of the children (once the page has not changed for ~1 s): to compare two focus positions */
        static struct { uint64_t self; uint32_t w[8][0x80]; int stable; uint32_t lasth; } dump[48];
        int d=-1;
        for (int i=0;i<48;++i) if (dump[i].self==self) { d=i; break; }
        if (d<0) for (int i=0;i<48;++i) if (!dump[i].self) { d=i; dump[i].self=self; break; }
        if (d>=0) {
            uint32_t w[8][0x80]; int ok=1; uint32_t h=2166136261u;
            for (int i=0;i<cnt && i<8 && ok;++i) { ok=safe_read(w[i],ch[i],0x200); if (ok) for (int k=0;k<0x80;++k) if (k*4!=0x8c && !(k*4>=0x70 && k*4<0x80)) h=(h^w[i][k])*16777619u; }
            if (ok) {
                if (h==dump[d].lasth) ++dump[d].stable; else { dump[d].stable=0; dump[d].lasth=h; }
                memcpy(dump[d].w,w,sizeof(w));
                if (dump[d].stable==3) {
                    printf("Mouse: U2 steady %#llx (hash %08x):",(unsigned long long)self,h);
                    for (int i=0;i<cnt && i<8;++i) { printf("\n  c%d:",i); for (int k=0;k<0x80;++k) if (w[i][k]) printf(" %x=%x",k*4,w[i][k]); }
                    printf("\n");
                }
            }
        }
    }
    int slot=-1;
    for (int i=0;i<128;++i) if (seen[i].self==self) { slot=i; break; }
    if (slot<0) {
        for (int i=0;i<128 && slot<0;++i) if (!seen[i].self) slot=i;
        if (slot<0) return;
        seen[slot].self=self; seen[slot].focus=-2;
        printf("Mouse: U2 container %#llx: %d children (array %#llx):",(unsigned long long)self,cnt,(unsigned long long)arr);
        for (int i=0;i<cnt && i<24;++i) { int32_t id=0, f4=0; safe_read(&id,ch[i]+0x10,4); safe_read(&f4,ch[i]+0xe4,4); printf(" [%d %#llx f%d id%x e4=%x]",i,(unsigned long long)ch[i],flag[i],(unsigned)id,(unsigned)f4); }
        printf("\n");
    }
    if (seen[slot].focus!=focus) { seen[slot].focus=focus; printf("Mouse: U2 container %#llx focus child %d (%d flagged)\n",(unsigned long long)self,focus,nfocus); }
    {   /* test (BB_U2_FOCUSTEST=1): 4 s after a focus is seen on child 0, the flag is moved to child 3 by hand */
        static int on=-1; static uint64_t since, since_self; static int done;
        if (on<0) { const char *e=getenv("BB_U2_FOCUSTEST"); on=e && atoi(e)==1; }
        if (on && !done && focus==0 && nfocus==1 && cnt>=5) {
            const uint64_t t=now_us();
            if (since_self!=self) { since_self=self; since=0; }
            if (!since) since=t;
            if (t-since>4000000) {
                done=1;
                /* the game's own call: vtable +0x30 of a control (state 0 / 1; the constructor of 0x57a600 uses it), which also runs the highlight fade */
                uint64_t vt0=0, vt3=0, f0=0, f3=0;
                safe_read(&vt0,ch[0],8); safe_read(&vt3,ch[3],8);
                if (safe_read(&f0,vt0+0x30,8) && safe_read(&f3,vt3+0x30,8) && f0>=image && f0<image+0xfe44b0 && f3>=image && f3<image+0xfe44b0) {
                    ((u2_set_fn)(uintptr_t)f0)(ch[0],0); ((u2_set_fn)(uintptr_t)f3)(ch[3],1);
                    printf("Mouse: U2 focus test: vtable+0x30 called (0 on child 0 via %#llx, 1 on child 3 via %#llx) in %#llx\n",(unsigned long long)(f0-image),(unsigned long long)(f3-image),(unsigned long long)self);
                } else printf("Mouse: U2 focus test: no usable vtable+0x30\n");
            }
        } else if (since_self==self && focus!=0) since=0;
    }
}
/* The page shown by the menu host (+0x9b8) holds its rows as children (count +0xb4, array +0xb8); the focused one has the byte at +8 set. The game's own
 * call, vtable +0x30 of a row (0 = lose focus, 1 = take it), moves the highlight at once (validated in game 2026-10-23, BB_U2_FOCUSTEST). */
static void u2_menu_jump_service(uint64_t image) {
    const float cy=u2_mj_cy;
    u2_mj_req=0;
    int ok=0;
    uint64_t page=0, ch[64]; int32_t cnt=0;
    uint64_t q[0x220];
    int found=0;
    if (u2_host_cand>0x10000 && safe_read(q,u2_host_cand,sizeof(q))) {   /* the page on screen: the one object of the host with children of which exactly one is focused */
        for (int i=0;i<0x220;++i) {
            uint64_t p=q[i], a=0, c[64]; int32_t n=0;
            if (p<0x10000 || !safe_read(&n,p+0xb4,4) || n<2 || n>64 || !safe_read(&a,p+0xb8,8) || a<0x10000 || !safe_read(c,a,(size_t)n*8)) continue;
            int nf=0, ok2=1;
            for (int k=0;k<n && ok2;++k) { uint8_t f=0; uint64_t v=0; if (c[k]<0x10000 || !safe_read(&v,c[k],8) || v<image || v>=image+0x2000000 || !safe_read(&f,c[k]+8,1)) ok2=0; else nf+=f!=0; }
            if (ok2 && nf==1) { if (!found) { page=p; cnt=n; memcpy(ch,c,sizeof(c)); } ++found; }
        }
    }
    if (found>1) printf("Runtime: U2 menu jump: %d pages have a focused row, the first is used\n",found);
    if (found) {
        int focus=-1, nfocus=0, valid=1;
        uint64_t vt[64];
        for (int i=0;i<cnt && valid;++i) {
            uint8_t f=0;
            if (ch[i]<0x10000 || !safe_read(&vt[i],ch[i],8) || vt[i]<image || vt[i]>=image+0x2000000 || !safe_read(&f,ch[i]+8,1)) valid=0;
            else if (f) { if (focus<0) focus=i; ++nfocus; }
        }
        /* the row asked for is found by where it is drawn: every child holds its rectangle (x +0x90, y +0x94, width +0x98, height +0x9c, same units as the text
         * rows) and a page has children that are not rows of the screen (titles, texts), so counting rows would land on the wrong child */
        int target=-1; float best=1e9f, best_all=1e9f; int near_hidden=0;
        const float cx=u2_mj_cx;
        for (int i=0;i<cnt && valid;++i) {
            float rc[4]; uint32_t id=0;
            /* rows are 28 to 36 tall; the rest is a help box (60), spacers, titles */
            if (!safe_read(rc,ch[i]+0x90,16)) continue;
            /* two controls of the Chapitres page are bigger than a row: the chapter picker (a 238 tall box that holds the label "Chapitre" and the picture, vtable 0x1007220) and the
             * "Commencer chapitre" button (42 tall, vtable 0x1006de0) */
            const uint64_t vo=vt[i]-image;
            const int picker=vo==0x1007220u && rc[3]>200.0f, button=vo==0x1006de0u && rc[3]>=38.0f && rc[3]<=46.0f;
            if (!picker && !button && (rc[3]<28.0f || rc[3]>36.0f)) continue;
            safe_read(&id,ch[i]+0x10,4);
            const float d=picker ? (fabsf(rc[1]+rc[3]*0.5f-cy)<=rc[3]*0.5f ? 0.0f : 1e3f) : fabsf(rc[1]+rc[3]*0.5f-cy);
            /* Sortie audio (stereo) has two more rows of the same kind that are not shown (the texts are drawn invisible and the game does not select them) */
            if (id==0x905ec3adu || id==0xbea3cef1u) { if (d<best_all) { best_all=d; near_hidden=1; } continue; }
            if (d<best_all) { best_all=d; near_hidden=0; }
            const float score=d+0.02f*fabsf(rc[0]+rc[2]*0.5f-cx);   /* two rows on one line: the nearer column wins */
            if (d<10.0f && score<best) { best=score; target=i; }
        }
        if (near_hidden && valid) {   /* over a hidden row: the zone goes to the nearest drawn row, by the cursor (above its middle: the row above, below: the row below) */
            const float uy=u2_mj_uy;
            float bd=1e9f; target=-1;
            for (int i=0;i<cnt;++i) {
                float rc[4]; uint32_t id=0;
                if (!safe_read(rc,ch[i]+0x90,16) || rc[3]<28.0f || rc[3]>36.0f) continue;
                safe_read(&id,ch[i]+0x10,4);
                if (id==0x905ec3adu || id==0xbea3cef1u) continue;
                const float d=fabsf(rc[1]+rc[3]*0.5f-uy);
                if (d<bd && rc[1]+rc[3]*0.5f>=cy-140.0f && rc[1]+rc[3]*0.5f<=cy+140.0f) { bd=d; target=i; }
            }
        }
        {   /* debug: the children of a page, once */
            static uint64_t shown[16]; int seen=0, slot=-1;
            for (int i=0;i<16;++i) { if (shown[i]==page) seen=1; if (!shown[i] && slot<0) slot=i; }
            if (!seen && slot>=0 && mouse_debug()) {
                shown[slot]=page;
                printf("Runtime: U2 page %#llx children (vt, id, y, h):",(unsigned long long)page);
                for (int i=0;i<cnt;++i) { float rc[4]={0}; uint32_t id=0; safe_read(rc,ch[i]+0x90,16); safe_read(&id,ch[i]+0x10,4); printf(" [%d %llx %x y%.0f h%.0f]",i,(unsigned long long)(vt[i]-image),id,rc[1]+rc[3]*0.5f,rc[3]); }
                printf("\n");
                for (int i=0;i+1<cnt;++i) {   /* debug: how two neighbouring 30 tall rows differ (which one is hidden?) */
                    float a4[4]={0}, b4[4]={0};
                    safe_read(a4,ch[i]+0x90,16); safe_read(b4,ch[i+1]+0x90,16);
                    if (a4[3]<28.0f || a4[3]>31.0f || b4[3]<28.0f || b4[3]>31.0f) continue;
                    uint32_t wa[0x80], wb[0x80];
                    if (!safe_read(wa,ch[i],0x200) || !safe_read(wb,ch[i+1],0x200)) continue;
                    printf("Runtime: U2 page child %d vs %d differ:",i,i+1);
                    for (int k=0;k<0x80;++k) if (wa[k]!=wb[k] && k*4!=0x10 && k*4!=0x94 && k*4!=0xa4 && k*4!=0x138 && k*4!=0x180 && k*4!=0x188) printf(" %x:%x/%x",k*4,wa[k],wb[k]);
                    printf("\n");
                }
            }
        }
        if (target<0) { if (valid) printf("Runtime: U2 menu jump: no row at y %.0f on page %#llx (%d rows)\n",cy,(unsigned long long)page,cnt); u2_mj_done=-2; return; }
        uint64_t f0=0, f1=0;
        if (valid && nfocus==1 && target!=focus && safe_read(&f0,vt[focus]+0x30,8) && safe_read(&f1,vt[target]+0x30,8) &&
            f0>=image && f0<image+0xfe44b0 && f1>=image && f1<image+0xfe44b0) {
            ((u2_set_fn)(uintptr_t)f0)(ch[focus],0); ((u2_set_fn)(uintptr_t)f1)(ch[target],1);
            ok=1;
            printf("Runtime: U2 menu jump: row %d -> %d (page %#llx, %d rows)\n",focus,target,(unsigned long long)page,cnt);
        } else if (valid && nfocus==1 && target==focus) ok=1;
        else printf("Runtime: U2 menu jump refused (page %#llx, %d rows, %d focused, valid %d)\n",(unsigned long long)page,cnt,nfocus,valid);
    }
    if (!ok && !page) printf("Runtime: U2 menu jump: no page (host %#llx)\n",(unsigned long long)u2_host_cand);
    u2_mj_done=ok ? 1 : -1;
}
static void u2_host_probe(uint64_t image) {   /* the objects the menu host points to (its pages): called by the text hook while a menu is drawn */
    if (u2_host_cand<=0x10000) return;
    static uint64_t last; const uint64_t t=now_us();
    if (t-last<300000) return;
    last=t;
    uint64_t q[0x220];
    if (!safe_read(q,u2_host_cand,sizeof(q))) return;
    static int shown;
    if (shown++<3) {
        printf("Mouse: U2 host %#llx +0x9b8..:",(unsigned long long)u2_host_cand);
        for (int i=0x9b8/8;i<0x9b8/8+8;++i) printf(" %llx",(unsigned long long)q[i]);
        printf(" | +0:"); for (int i=0;i<8;++i) printf(" %llx",(unsigned long long)q[i]);
        for (int i=0x9b8/8;i<0x9b8/8+2;++i) {
            uint64_t w[0x20];
            if (q[i]>0x10000 && safe_read(w,q[i]+0xa0,sizeof(w))) { printf(" || page %llx +0xa0:",(unsigned long long)q[i]); for (int k=0;k<0x20;++k) printf(" %llx",(unsigned long long)w[k]); }
        }
        printf("\n");
    }
    for (int i=0;i<0x220;++i) if (q[i]>0x10000) u2_container_probe(q[i],image);
}
static void __attribute__((ms_abi)) u2_widget_hook(uint64_t self, uint64_t frame, uint64_t image, uint64_t r14) {
    (void)frame; (void)r14;
    uint64_t ds=0, vt=0, f0=0, f8=0, f30=0, f38=0;
    if (self<0x10000 || !safe_read(&ds,self+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&f30,vt+0x30,8) || !safe_read(&f38,vt+0x38,8)) return;
    const uint64_t lo_code=image, hi_code=image+0xfe44b0;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || f30<lo_code || f30>=hi_code || f38<lo_code || f38>=hi_code) return;
    int cur=(int32_t)((u2_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int32_t)((u2_vcall_fn)(uintptr_t)f30)(ds), hi=(int32_t)((u2_vcall_fn)(uintptr_t)f38)(ds);
    if (u2_jump_pending && u2_jump_ds==ds) {
        int v=u2_jump_val; if (v<lo) v=lo; if (v>hi) v=hi;
        int32_t top0=0;
        /* Ghidra 2026-10-23: when the value changes, 0x57e410 calls the widget's scroll function (vtable +0x68 = 0x582610) which keeps the selection one row
         * away from the edges of the rows seen (+0x248 first row, +0x244 rows seen): the first and last rows make the list scroll. That call only happens if the
         * value differs from the one the widget saw last (+0x22c). For a hover on such a row the value is set, then +0x22c (and +0x228) are set to it too: the widget
         * thinks it has already followed, the list does not scroll and the highlight is drawn from the data source. */
        int32_t vis0=0;
        const int edge=u2_jump_keep && u2_edge_on() && safe_read(&top0,self+0x248,4) && safe_read(&vis0,self+0x244,4) && vis0>=4 && vis0<=16 &&
                       ((v==top0 && top0>lo) || (v==top0+vis0-1 && v<hi));
        ((u2_set_fn)(uintptr_t)f8)(ds,v);
        if (edge) { const int32_t w=v; safe_write(self+0x22c,&w,4); safe_write(self+0x228,&w,4); if (mouse_debug()) printf("Mouse: U2 list edge row %d selected without scroll (first row %d, %d seen)\n",v,top0,vis0); }
        u2_jump_pending=0; u2_jump_keep=0;
        if (mouse_debug()) printf("Mouse: U2 list set(%d) on %#llx (was %d, range %d..%d)\n",v,(unsigned long long)ds,cur,lo,hi);
        cur=v;
    }
    if (u2_scroll_pending && u2_scroll_w==self) {
        int32_t t=u2_scroll_top, vis0=0;
        if (safe_read(&vis0,self+0x244,4) && vis0>=1 && vis0<=32) { if (t>hi+1-vis0) t=hi+1-vis0; if (t<lo) t=lo; safe_write(self+0x248,&t,4); }
        u2_scroll_pending=0;
        if (mouse_debug()) { static uint64_t dbg; const uint64_t n=now_us(); if (n-dbg>200000) { dbg=n; printf("Mouse: U2 list view scrolled to first row %d (selection %d, %d seen)\n",t,cur,vis0); } }
    }
    const uint64_t now=now_us();
    if (mouse_debug() && hi-lo>12) {
        static uint64_t shown_vt;
        uint64_t wvt=0, slot[0x20];
        if (shown_vt!=self && safe_read(&wvt,self,8) && wvt>0x10000 && safe_read(slot,wvt,sizeof(slot))) {
            shown_vt=self;
            printf("Mouse: U2 list widget vtable slots (offset from image):");
            for (int i=0;i<0x20;++i) printf(" +%#x:%#llx",i*8,(unsigned long long)(slot[i]-image));
            printf("\n");
        }
    }
    if (mouse_debug() && hi-lo>12 && u2_probe_row>=0 && now-u2_probe_us<120000) {   /* where does the widget keep the first row seen? (value == cur - selected row on screen) */
        static unsigned hits[4][3][0x100]; static unsigned samples; static uint64_t shown;   /* [self int, self float, ds int, widget->+0x20 object int][f-1, f, f+1] */
        const int f=cur-u2_probe_row;
        uint32_t buf[4][0x100];
        uint64_t sub=0; safe_read(&sub,self+0x20,8);
        if (safe_read(buf[0],self,sizeof(buf[0])) && safe_read(buf[2],ds,sizeof(buf[2]))) {
            if (!(sub>0x10000 && safe_read(buf[3],sub,sizeof(buf[3])))) memset(buf[3],0,sizeof(buf[3]));
            ++samples;
            for (int i=0;i<0x100;++i) {
                float fl; memcpy(&fl,&buf[0][i],4);
                for (int v=0;v<3;++v) {
                    const int t=f-1+v;
                    if ((int32_t)buf[0][i]==t) ++hits[0][v][i];
                    if (fl==(float)t) ++hits[1][v][i];
                    if ((int32_t)buf[2][i]==t) ++hits[2][v][i];
                    if ((int32_t)buf[3][i]==t) ++hits[3][v][i];
                }
            }
        }
        if (now-shown>4000000 && samples>20) {
            shown=now;
            printf("Mouse: U2 probe first-row (%u samples), best:",samples);
            for (int n=0;n<6;++n) {
                unsigned bh=0; int bs=-1, bv=0, bi=0;
                for (int s=0;s<4;++s) for (int v=0;v<3;++v) for (int i=0;i<0x100;++i) if (!(hits[s][v][i]&0x80000000u) && hits[s][v][i]>bh) { bh=hits[s][v][i]; bs=s; bv=v; bi=i; }
                if (bs<0 || bh*20<samples) break;
                printf(" [%d f%+d +%#x:%u]",bs,bv-1,bi*4,bh);
                hits[bs][bv][bi]|=0x80000000u;   /* (shown once: the high bit is cleared by the mask below) */
            }
            for (int s=0;s<4;++s) for (int v=0;v<3;++v) for (int i=0;i<0x100;++i) hits[s][v][i]&=0x7fffffffu;
            printf("\n");
        }
    }
    float rc[4]={0,0,0,0};
    safe_read(rc,self+0x90,16);
    pthread_mutex_lock(&menu_text_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<U2_WIDGETS;++i) { if (u2_widgets[i].w==self) { slot=i; break; } if (u2_widgets[i].us<u2_widgets[oldest].us) oldest=i; }
    if (slot<0) { slot=oldest; if (mouse_debug()) printf("Mouse: U2 widget %#llx data source %#llx range %d..%d value %d rect x %.1f y %.1f w %.1f h %.1f\n",(unsigned long long)self,(unsigned long long)ds,lo,hi,cur,rc[0],rc[1],rc[2],rc[3]); }
    u2_widgets[slot]=(U2Widget){self,ds,cur,lo,hi,now,rc[1],rc[3]};
    pthread_mutex_unlock(&menu_text_lock);
}
/* the list on screen: the fresh widget with the widest range (more values than rows seen) */
static int u2_widget_pick(int vis, uint64_t now, uint64_t *ds, int *lo, int *hi, int *cur) {
    int best=-1;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<U2_WIDGETS;++i) {
        const U2Widget *w=&u2_widgets[i];
        if (!w->us || now-w->us>150000 || w->hi-w->lo+1<vis || w->cur<w->lo || w->cur>w->hi) continue;
        if (best<0 || w->hi-w->lo>u2_widgets[best].hi-u2_widgets[best].lo) best=i;
    }
    if (best>=0) { *ds=u2_widgets[best].ds; *lo=u2_widgets[best].lo; *hi=u2_widgets[best].hi; *cur=u2_widgets[best].cur; u2_pick_self=u2_widgets[best].w; }
    pthread_mutex_unlock(&menu_text_lock);
    return best>=0;
}
/* the control of a slider: a fresh widget with a range of a few values, laid out in the row (28 to 45 tall) whose centre is where the rail is */
static int u2_slider_widget(float cy, uint64_t now, uint64_t *ds, int *lo, int *hi) {
    int best=-1; float bd=10.0f;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<U2_WIDGETS;++i) {
        const U2Widget *w=&u2_widgets[i];
        if (!w->us || now-w->us>150000 || w->hi-w->lo<2 || w->rh<20.0f || w->rh>45.0f) continue;
        const float d=fabsf(w->ry+w->rh*0.5f-cy);
        if (d<bd) { bd=d; best=i; }
    }
    if (best>=0) { *ds=u2_widgets[best].ds; *lo=u2_widgets[best].lo; *hi=u2_widgets[best].hi; }
    pthread_mutex_unlock(&menu_text_lock);
    return best>=0;
}
/* U3 menu rows (0x5dca50, found 2026-10-28 from the return addresses of the text draw): each row is a button of its own, no list and no data source; the selected one has the
 * byte widget+8 at 1 (the others 0). The hover sets that flag at once on the row under the cursor (and clears it on the others of the same column) instead of stepping the
 * d-pad row by row. Written by u3_menu_buttons, applied by the hook of each row for 150 ms. */
static volatile float u3_jf_x, u3_jf_y, u3_jf_w;
static volatile uint64_t u3_jf_us;
static volatile float u3_jf_h;
static volatile uint64_t u3_jf_hit_us;   /* the widget of the row under the cursor was drawn lately: only then are the others cleared (a title, a description, a prompt is no row: clearing every flag sends the selection back to the first line) */
/* Rows are matched by their vertical centre: the widget of an option row (26.9 tall, y 140.9) is not the rectangle of its text (33.6 tall, y 137.5). loose: the width of the
 * widget (592) is not the one of the text (302...), the column is then told by its left edge alone. */
static void u3_row_jump(uint64_t self, uint64_t image, const float *rc, uint64_t now, int loose) {
    const int hit=fabsf((rc[1]+rc[3]*0.5f)-(u3_jf_y+u3_jf_h*0.5f))<1.0f;
    if (u3_jf_us && now-u3_jf_us<150000 && rc[3]>=24.0f && rc[3]<=42.0f &&
        (hit || (fabsf(rc[0]-u3_jf_x)<6.0f && (loose || fabsf(rc[2]-u3_jf_w)<12.0f)))) {   /* the row under the cursor, or another row of the column of the selected one */
        uint8_t cur_flag=0, want=hit ? 1 : 0;
        if (hit) u3_jf_hit_us=now;
        if (!hit && now-u3_jf_hit_us>=100000) return;
        if (safe_read(&cur_flag,self+8,1) && cur_flag<=1 && cur_flag!=want) {
            ((u2_set_fn)(uintptr_t)(image+0x5ddc80))(self,want);   /* the game's own setFocused(widget, bool): flag, data source notice, colour fade */
            if (mouse_debug()) printf("Mouse: U3 row %#llx selected flag %d -> %d (hover jump)\n",(unsigned long long)self,cur_flag,want);
        }
    }
}
/* The rows of the option screens (0x5de7c0, drawn from 0x5deae5: label + value of Jeu, Audio, Affichage...): the same kind of widget (rect +0x90, flag +8, data source +0x18),
 * but its data source has no usable min/max, so no check on them. */
static void __attribute__((ms_abi)) u3_option_row_hook(uint64_t self, uint64_t frame, uint64_t image, uint64_t r14) {
    (void)frame; (void)r14;
    float rc[4]={0,0,0,0};
    if (self<0x10000 || !safe_read(rc,self+0x90,16)) return;
    u3_row_jump(self,image,rc,now_us(),1);
    static uint64_t last; const uint64_t now=now_us();
    if (mouse_debug() && now-last>2000000) { last=now; printf("Mouse: U3 option row hook alive: %#llx rect %.1f %.1f %.1f %.1f\n",(unsigned long long)self,rc[0],rc[1],rc[2],rc[3]); }
}
/* U3 slider draw (0x5e2190, found by disassembly 2026-10-28; (widget, ?, draw context), runs every frame: percent = (value - min) * 100 / (max - min)). Data source at
 * widget+0x18 as in U2, but its virtual slots moved: +0 get, +8 set(value), +0x38 minimum, +0x40 maximum. The hook registers the widget like U2's and carries a pending set(value). */
static void __attribute__((ms_abi)) u3_widget_hook(uint64_t self, uint64_t frame, uint64_t image, uint64_t r14) {
    (void)frame; (void)r14;
    uint64_t ds=0, vt=0, f0=0, f8=0, f30=0, f38=0;
    if (self<0x10000 || !safe_read(&ds,self+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&f30,vt+0x38,8) || !safe_read(&f38,vt+0x40,8)) return;
    const uint64_t lo_code=image, hi_code=image+0x16488f0;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || f30<lo_code || f30>=hi_code || f38<lo_code || f38>=hi_code) return;
    int cur=(int32_t)((u2_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int32_t)((u2_vcall_fn)(uintptr_t)f30)(ds), hi=(int32_t)((u2_vcall_fn)(uintptr_t)f38)(ds);
    if (u2_jump_pending && u2_jump_ds==ds) {
        int v=u2_jump_val; if (v<lo) v=lo; if (v>hi) v=hi;
        ((u2_set_fn)(uintptr_t)f8)(ds,v);
        u2_jump_pending=0; u2_jump_keep=0;
        if (mouse_debug()) printf("Mouse: U3 widget set(%d) on %#llx (was %d, range %d..%d)\n",v,(unsigned long long)ds,cur,lo,hi);
        cur=v;
    }
    const uint64_t now=now_us();
    float rc[4]={0,0,0,0};
    safe_read(rc,self+0x90,16);
    if (hi==lo) u3_row_jump(self,image,rc,now,1);
    pthread_mutex_lock(&menu_text_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<U2_WIDGETS;++i) { if (u2_widgets[i].w==self) { slot=i; break; } if (u2_widgets[i].us<u2_widgets[oldest].us) oldest=i; }
    if (slot<0) { slot=oldest; if (mouse_debug()) printf("Mouse: U3 widget %#llx data source %#llx range %d..%d value %d rect x %.1f y %.1f w %.1f h %.1f\n",(unsigned long long)self,(unsigned long long)ds,lo,hi,cur,rc[0],rc[1],rc[2],rc[3]); }
    u2_widgets[slot]=(U2Widget){self,ds,cur,lo,hi,now,rc[1],rc[3]};
    pthread_mutex_unlock(&menu_text_lock);
}
static const unsigned char u2_bind_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56};
/* U3 list (0x5e3e60, found 2026-10-29 with a probe; Bonus > Cinématiques / Trésors / Statistiques): one widget draws all the rows and keeps the selected row in its data source
 * (widget+0x18; +0 get, +8 set, +0x38 minimum, +0x40 maximum, as the sliders), rows of 28.55 from the top of its rectangle (+0x90); the scroll bar is its left edge. The hook
 * runs u3_widget_hook (the pending set(value)) and notes the list for u3_menu_buttons. */
static struct { uint64_t w, ds, us; int cur, lo, hi, first, vis; float rc[4]; } u3_list;
static volatile int u3_keep_view;   /* the pending jump is a hover / click: the view stays */
/* Widget+0x184 = rows seen, +0x188 = first row seen (found 2026-10-29 by correlating a dump with the highlighted row: the view follows the selection with the highlight held on the
 * 6th row, so first = cur - 5 until the ends). The wheel and the bar write +0x188 (u2_scroll_*, on the game's thread, as U2 does with +0x248): the selection stays where it is. */
static void __attribute__((ms_abi)) u3_list_hook(uint64_t self, uint64_t frame, uint64_t image, uint64_t r14) {
    /* a hover / click jump: the view must not follow the new selection (the game would bring it to the 6th row and the list would run under the cursor): the widget is told it
     * has already followed (+0x16c holds the selection it last saw) and the first row seen is put back. BB_U3_KEEP=0 turns this off. */
    uint64_t ds_pre=0; int32_t first_pre=-1;
    const int keep=u2_jump_pending && u3_keep_view && safe_read(&ds_pre,self+0x18,8) && ds_pre==u2_jump_ds && safe_read(&first_pre,self+0x188,4);
    u3_widget_hook(self,frame,image,r14);
    int32_t vis0=0, first0=-1;
    if (keep && !u2_jump_pending) { const int32_t v=u2_jump_val; safe_write(self+0x16c,&v,4); safe_write(self+0x188,&first_pre,4); u3_keep_view=0; }
    safe_read(&vis0,self+0x184,4); safe_read(&first0,self+0x188,4);
    if (u2_scroll_pending && u2_scroll_w==self && vis0>=3 && vis0<=32) {
        int32_t t=u2_scroll_top;
        if (u3_list.w==self) { if (t>u3_list.hi+1-vis0) t=u3_list.hi+1-vis0; if (t<u3_list.lo) t=u3_list.lo; }
        safe_write(self+0x188,&t,4); first0=t;
        u2_scroll_pending=0;
    }
    uint64_t ds=0, vt=0, f0=0, f30=0, f38=0; float rc[4]={0,0,0,0};
    if (self<0x10000 || !safe_read(rc,self+0x90,16) || rc[3]<100.0f) return;
    if (!safe_read(&ds,self+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f30,vt+0x38,8) || !safe_read(&f38,vt+0x40,8)) return;
    if (f0<image || f0>=image+0x16488f0 || f30<image || f30>=image+0x16488f0 || f38<image || f38>=image+0x16488f0) return;
    const int cur=(int32_t)((u2_vcall_fn)(uintptr_t)f0)(ds), lo=(int32_t)((u2_vcall_fn)(uintptr_t)f30)(ds), hi=(int32_t)((u2_vcall_fn)(uintptr_t)f38)(ds);
    if (hi-lo<1) return;   /* (Récompenses > Modes de rendu has 3 rows) */
    if (getenv("BB_U3_LISTDUMP")) {   /* (probe) the widget's bytes each time the selection changes, with the y of the highlighted row: the first row seen is cur - (y - top) / 28.55 */
        static int last_cur=-99; static unsigned n;
        if (cur!=last_cur && n<200) {
            last_cur=cur; ++n;
            uint32_t buf[0x180]; float hy=-1.0f;
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<U2_ROWS;++i) { const U2Row *r=&u2_rows[i]; if (r->us && now_us()-r->us<100000 && fabsf(r->h-28.6f)<0.4f && r->s5>=2.5f) hy=r->y; }
            pthread_mutex_unlock(&menu_text_lock);
            if (safe_read(buf,self,sizeof(buf))) { printf("Mouse: U3 LISTDUMP cur %d hy %.1f top %.1f:",cur,hy,rc[1]); for (int i=0;i<0x180;++i) if (buf[i]) printf(" %x=%x",i*4,buf[i]); printf("\n"); }
        }
    }
    u3_list.first=(vis0>=3 && vis0<=32 && first0>=lo && first0<=hi) ? first0 : -1; u3_list.vis=vis0;
    u3_list.w=self; u3_list.ds=ds; u3_list.cur=cur; u3_list.lo=lo; u3_list.hi=hi; memcpy(u3_list.rc,rc,sizeof(rc)); u3_list.us=now_us();
}
/* U2 Trésors, the 3D model (menutreasurescene.cpp, FUN_005d74f0(scene), every frame; Ghidra 2026-10-23). The scene keeps the model's orientation as a 4x4 matrix
 * (floats 12..27) and two angular speeds, float 0x1c (+0x70, around the vertical) and 0x1d (+0x74), that the right stick adds to (twice its deadzoned value, per frame) and that
 * the game clamps to +-2 rad/s and lets die down. Each frame the matrix is turned by speed x dt (dt: the float at image+0x111628c). Native mouse: while the left button is held on the
 * model, the speeds are set from the mouse motion since the last frame (no emulated stick: the menu read the stick's vertical as "next text"). */
static volatile int u2_model_drag;     /* the left button was pressed on the 3D model and is still down (the mouse turns the model) */
static volatile float u2_model_ax, u2_model_ay;   /* mouse motion since the last frame of the scene, in radians */
/* Hooked at 0x5d7c05, just after the game clamped both speeds to +-2 rad/s and before it turns the matrix by speed x dt (r14 is the scene there): the speeds written here are
 * not limited, so the model follows the mouse one to one. */
static const unsigned char u2_model_code[]={0xc5,0xfa,0x10,0x85,0x70,0xfd,0xff,0xff};   /* vmovss xmm0, [rbp-0x290] */
static void __attribute__((ms_abi)) u2_model_hook(uint64_t rdi, uint64_t frame, uint64_t image, uint64_t self) {
    (void)frame; (void)rdi;
    /* After a release the model goes on turning, slowing down (U2_MODEL_COAST seconds to lose most of its speed): the speed of the last 60 ms of the gesture decays. */
    static float sx, sy;
    static int coast_on=-1; static float coast_tau=0.5f;
    if (coast_on<0) { const char *e=getenv("BB_U2_MODEL_COAST"); coast_tau=e && atof(e)>=0.0 ? (float)atof(e) : 0.5f; coast_on=coast_tau>0.0f; }
    float dt=0.0f;
    if (!safe_read(&dt,image+0x111628cu,4) || !(dt>0.0005f) || dt>0.2f) dt=1.0f/30.0f;
    float vx, vy;
    if (u2_model_drag) {
        const float ax=u2_model_ax, ay=u2_model_ay;
        u2_model_ax-=ax; u2_model_ay-=ay;
        vx=ax/dt; vy=-ay/dt;
        if (vx>60.0f) vx=60.0f; if (vx<-60.0f) vx=-60.0f;   /* (a sanity limit only) */
        if (vy>60.0f) vy=60.0f; if (vy<-60.0f) vy=-60.0f;
        const float a=1.0f-expf(-dt/0.06f);
        sx+=a*(vx-sx); sy+=a*(vy-sy);
    } else {
        u2_model_ax=u2_model_ay=0.0f;
        if (!coast_on || fabsf(sx)+fabsf(sy)<0.05f) { sx=sy=0.0f; return; }
        const float k=expf(-dt/coast_tau);
        sx*=k; sy*=k; vx=sx; vy=sy;
    }
    const float v[2]={vx,vy};
    safe_write(self+0x70,v,8);
    if (mouse_debug()) { static uint64_t last; const uint64_t now=now_us(); if (now-last>250000) { last=now; printf("Mouse: U2 model hook: scene %#llx dt %.4f drag %d speed %.3f %.3f\n",(unsigned long long)self,dt,u2_model_drag,vx,vy); } }
}
static volatile float u2_anim_sx, u2_anim_sy;   /* the mouse's turn rate (degrees/s) while it goes through the emulated stick */
static volatile uint64_t u2_anim_us;
static float anim_fb_err, anim_fb_yaw; static int anim_fb_have; static uint64_t anim_fb_moved_us;   /* the shortfall of the turn against the mouse, see sample_host */
static double anim_cal_req, anim_cal_got; static float anim_cal_yaw; static int anim_cal_have;   /* (debug) degrees the mouse asked for vs degrees the picture turned in one spell on the emulated stick */
/* U2 of CUSA02320 (the collection): image+0x431580 turns a pad object (rsi, image+0x1116950 + 0x130 * n) into the "game stick" block (rdi, a
 * caller's stack): dead zone, then the length of each stick clamped to 1, left stick at +0, right stick at +8 / +0xc. Every system that
 * wants the sticks (cameras, the player's animations) calls it. The hook sits after the right stick is stored (image+0x4316cc, rdi still
 * the block). During a grab, a vault or a ledge the picture is held by the game's animation, which turns at a rate proportional to this
 * stick (about 120 degrees/s per unit once its ramp-up is counted, measured), and the clamp to 1 was the cap on the mouse. The mouse's rate
 * while it goes through the emulated stick is put here as a value above 1 when the pad's own right stick is at rest (BB_U2_ANIM_RATE=<deg/s
 * per unit> tunes it). The walking camera is not affected: it does not use this block for the mouse. */
static void __attribute__((ms_abi)) u2_stick_filter(uint64_t block, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused1; (void)image; (void)r14;
    static float full_rate=0.0f;
    if (full_rate<=0.0f) { const char *e=getenv("BB_U2_ANIM_RATE"); full_rate=e && atof(e)>10.0 ? (float)atof(e) : 120.0f; }
    if (block<0x10000) return;
    float *out=(float *)(uintptr_t)block;
    const uint64_t now=now_us();
    static unsigned dbg_calls, dbg_inj; static uint64_t dbg_last;
    const float game_sx=out[2], game_sy=out[3];
    ++dbg_calls;
    {   /* (debug, BB_U2_CALLERS=1) who asks for the stick: the return addresses (a call just before them) found on the stack, counted per 250 ms */
        static int on=-1; static uint64_t addr[48]; static unsigned cnt[48]; static int n; static uint64_t last_print;
        if (on<0) on=mouse_debug() && getenv("BB_U2_CALLERS")!=NULL;
        if (on && guest_callback_rsp>0x10000) {
            uint64_t stack[96];
            if (safe_read(stack,guest_callback_rsp,sizeof(stack))) {
                for (int i=0;i<96;++i) {
                    const uint64_t v=stack[i];
                    if (v<image+0x100000 || v>=image+0xe00000) continue;
                    unsigned char op=0;
                    if (!safe_read(&op,v-5,1) || op!=0xe8) continue;
                    int k=-1; for (int j=0;j<n;++j) if (addr[j]==v) k=j;
                    if (k<0 && n<48) { k=n++; addr[k]=v; cnt[k]=0; }
                    if (k>=0) ++cnt[k];
                }
            }
            const uint64_t t=now_us();
            if (t-last_print>250000) {
                last_print=t;
                printf("Mouse: STICKCALLERS calls %u:",dbg_calls);
                for (int j=0;j<n;++j) if (cnt[j]>=3) printf(" %#llx:%u",(unsigned long long)(addr[j]-image),cnt[j]);
                printf(" t=%.4f\n",t*1e-6);
                n=0;
            }
        }
    }
    if (u2_anim_us && now-u2_anim_us<60000 && (u2_anim_sx!=0.0f || u2_anim_sy!=0.0f)) {
        float sx=u2_anim_sx/full_rate, sy=u2_anim_sy/full_rate;
        sx=sx>8.0f ? 8.0f : sx<-8.0f ? -8.0f : sx; sy=sy>8.0f ? 8.0f : sy<-8.0f ? -8.0f : sy;
        out[2]=sx; out[3]=sy;
        ++dbg_inj;
    }
    if (mouse_debug() && ((u2_anim_us && now-u2_anim_us<500000) || game_sx>0.3f || game_sx<-0.3f) && now-dbg_last>100000) {   /* is the stick block asked for, and does the mouse (or the pad) get in, while the stick is on? */
        dbg_last=now;
        float yaw=0.0f; double age=0.0;
        const int have=bbgpu_camera_yaw(&yaw,&age);
        printf("Mouse: STICKFILTER calls %u injected %u game stick %.2f %.2f -> %.2f %.2f yaw %.1f%s t=%.4f\n",dbg_calls,dbg_inj,(double)game_sx,(double)game_sy,(double)out[2],(double)out[3],(double)yaw,have && age<0.25 ? "" : "?",now*1e-6);
        dbg_calls=dbg_inj=0;
    }
}
static const unsigned char u2_stick_filter_code[]={0xc5,0xfa,0x10,0x46,0x10,0xc5,0xfa,0x11,0x47,0x10,0xc5,0xfa,0x10,0x46,0x14};
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
    {"U2 camera update (mouse, collection)",0x4374cc,u2b_camera_hook_code,sizeof(u2b_camera_hook_code),7,u2b_camera_hook,NULL},
    {"U2 stick filter (collection)",0x4316cc,u2_stick_filter_code,sizeof(u2_stick_filter_code),5,u2_stick_filter,NULL},
    {"U2 list widget update (collection)",0x57e410,u2_bind_code,sizeof(u2_bind_code),8,u2_widget_hook,NULL},
    {"U2 treasure model (mouse, collection)",0x5d7c05,u2_model_code,sizeof(u2_model_code),8,u2_model_hook,NULL},
    {"U2 prompt bar: compare-with-friends prompt (hook, collection)",0x5a54c7,u2_compare_cmp_code,sizeof(u2_compare_cmp_code),5,u2_compare_prompt,NULL},
    {"U2 prompt bar: no compare-with-friends prompt (collection)",0x5a54cc,u2_compare_jz_code,sizeof(u2_compare_jz_code),sizeof(u2_compare_jz_patch),NULL,u2_compare_jz_patch},
    {"U2 menu text probe (collection)",0x5b56d0,u2_text_probe_code,sizeof(u2_text_probe_code),8,u2_text_probe,NULL},
    {"U3 menu text probe (collection)",0x63d6f0,u2_text_probe_code,sizeof(u2_text_probe_code),8,u2_text_probe,NULL},
    {"U3 slider draw (collection)",0x5e2190,u2_bind_code,sizeof(u2_bind_code),8,u3_widget_hook,NULL},
    {"U3 list widget draw (collection)",0x5dca50,u2_bind_code,sizeof(u2_bind_code),8,u3_widget_hook,NULL},
    {"U3 list widget draw (rows, scroll bar)",0x5e3e60,u2_bind_code,sizeof(u2_bind_code),8,u3_list_hook,NULL},
    {"U3 menu builder (photo row)",0x649660,u2_menu_builder_code,sizeof(u2_menu_builder_code),6,u3_menu_builder_hook,NULL},
    {"U3 list widget draw (option rows)",0x5de7c0,u2_bind_code,sizeof(u2_bind_code),8,u3_option_row_hook,NULL},
    {"U3 prompt bar: no compare-with-friends prompt (collection)",0x627d65,u3_compare_cmp_code,sizeof(u3_compare_cmp_code),sizeof(u3_compare_cmp_patch),NULL,u3_compare_cmp_patch},
    {"U3 prompt bar flags (collection)",0x6277e8,u3_bar_flags_code,sizeof(u3_bar_flags_code),6,u3_bar_flags,NULL},
    {"U2 menu builder probe (collection)",0x5c53a0,u2_menu_builder_code,sizeof(u2_menu_builder_code),6,u2_menu_builder_probe,NULL},
    {"U2 text lookup probe (collection)",0xcddc00,u2_text_lookup_code,sizeof(u2_text_lookup_code),6,u2_text_lookup_probe,NULL},
    {"U3 text lookup probe (collection)",0xf32ae0,u2_text_lookup_code,sizeof(u2_text_lookup_code),6,u2_text_lookup_probe,NULL},
    {"U1 controls menu: keyboard row",0x145881,u1_add_keys_row_code,sizeof(u1_add_keys_row_code),5,u1_add_keys_row,NULL},
    {"U1 compare-with-friends prompt (hook)",0x128f64,u1_compare_code,sizeof(u1_compare_code),5,u1_compare_prompt,NULL},
    {"U1 compare-with-friends prompt (skip 1)",0x128f69,u1_compare_je_code,sizeof(u1_compare_je_code),sizeof(u1_compare_je_patch),NULL,u1_compare_je_patch},
    {"U1 compare-with-friends prompt (skip 2)",0x128f76,u1_compare_jmp_code,sizeof(u1_compare_jmp_code),sizeof(u1_compare_jmp_patch),NULL,u1_compare_jmp_patch},
    {"U1 menu add item",0x132f00,u1_menu_add_item_code,sizeof(u1_menu_add_item_code),6,u1_menu_add_item,NULL},
    {"U1 prompt bar flags",0x128c8a,u1_bar_flags_code,sizeof(u1_bar_flags_code),10,u1_bar_flags,NULL},
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
static int u2_menu_on, u2_menu_key_guard;   /* defined with the U2 menus below; the guard keeps the four buttons released at a change until their keys are */
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
    static int u2_keys_off=-1;   /* BB_U2_MENU_KEYS=0: the menus of U2 keep the keys of the play (diagnosis) */
    if (u2_keys_off<0) { const char *e=getenv("BB_U2_MENU_KEYS"); u2_keys_off=e && atoi(e)==0; }
    if (!menu_mode && !u1_image && !u2_keys_off) {   /* U2: the menus have the keys of the U1 menus (Enter, Esc, Alt, Backspace), the same ones the pastilles show */
        if (u2_menu_key_guard && !menu_keys_held(k)) u2_menu_key_guard=0;
        if (menu_button && u2_menu_key_guard) return 0;
        if (u2_menu_on && menu_button) {
            switch (a) {
            case A_CROSS: return k && (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]);
            case A_CIRCLE: return k && k[SDL_SCANCODE_ESCAPE];
            case A_OPTIONS: return k && (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]);
            case A_R1: return k && k[SDL_SCANCODE_BACKSPACE];
            default: return 0;
            }
        }
    }
    if (!menu_mode) return action_down(a,k,mouse,now);
    /* The mouse in a menu: the wheel moves up and down (d-pad), the lower thumb button (Mouse 4, "back") goes
     * back (circle). The left click will validate (cross) once the cursor can select the text under it: not
     * yet, a click anywhere would validate whatever is selected. */
    if (menu_carousel_recent(now)) {   /* a carousel: the wheel goes sideways */
        if (a==A_LEFT && now<wheel_until_us[0]) return 1;
        if (a==A_RIGHT && now<wheel_until_us[1]) return 1;
    } else {
        if (a==A_UP && now<wheel_until_us[0] && !u1_rail_wheel) return 1;
        if (a==A_DOWN && now<wheel_until_us[1] && !u1_rail_wheel) return 1;
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
            sb_drag, sb_measure, sb_same, sb_last, sb_n, sb_cmp, direct_tries, list_armed, jump_fail, dlg_sel, sb_by_wheel, sb_pending;
        uint64_t wseen[2];
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
        if (!left && !h.sb_by_wheel) h.sb_drag=0;
        u1_rail_wheel=have_rail;
        if (h.sb_by_wheel && !have_rail) { h.sb_drag=0; h.sb_by_wheel=0; h.sb_pending=0; }
        {   /* the wheel scrolls the list's view (the bar's thumb) like a drag of the bar would: a notch is one row; the highlight stays where it is */
            int notch=0;
            for (int i=0;i<2;++i) if (wheel_until_us[i]!=h.wseen[i]) { h.wseen[i]=wheel_until_us[i]; notch+=i==0 ? -1 : 1; }
            if (have_rail && notch && !left && menu_list_index(ritem)>=0) {
                if (!h.sb_drag) {
                    h.sb_drag=1; h.sb_by_wheel=1; h.sb_pending=0; h.sb_item=ritem; h.sb_y=ry; h.sb_h=rh; h.sb_cmp=0; h.sb_same=0; h.sb_last=menu_list_index(ritem);
                    h.sb_measure=(h.sb_cached_item!=ritem);
                    if (h.sb_measure) h.sb_n=0;
                    h.sb_m_armed=0; h.sb_m_fallback=0;
                }
                if (h.sb_by_wheel) h.sb_pending+=notch;
            }
        }
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
            if (!h.sb_measure && h.sb_drag && h.sb_by_wheel) {
                ListInfo li; const int top=list_top_read(h.sb_item);
                if (menu_list_find_ex(now,&li,0) && li.pitch>1.0f && top>=0) {
                    const int visible=(int)floorf((li.bottom-li.top)/li.pitch+0.5f);
                    const int max_top=h.sb_n-visible>0 ? h.sb_n-visible : 0;
                    int want=top+h.sb_pending;
                    want=want<0 ? 0 : want>max_top ? max_top : want;
                    if (want!=top) list_top_write(h.sb_item,want);
                }
                h.sb_pending=0; h.sb_drag=0; h.sb_by_wheel=0;
            } else if (!h.sb_measure && h.sb_drag) {
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
    int list_ctl=0, list_delta=0, list_title=0, list_noclick=0;
    int list_found=menu_list_find_ex(now,&list,0);
    if (list_found && list.sel_cy<0.0f) {   /* the highlighted row is out of view (scrolled away with the bar): where it would be */
        const int top=list_top_read(list.item), idx=menu_list_index(list.item);
        if (top>=0 && idx>=0) list.sel_cy=list.top+12.6f+(float)(idx-top)*list.pitch; else list_found=0;
    }
    if (list_found && ux>=list.colx-20.0f && ux<=list.colx+560.0f && uy>=list.top-4.0f && uy<=list.bottom+4.0f) {
        list_ctl=1;
        list_delta=(int)floorf((uy-list.sel_cy)/list.pitch+0.5f);
        /* Statistiques (a list with a column of values on the right): only the difficulties of Contre-la-montre (rows with an empty value) open
         * something. A click on a statistic does nothing, and the section titles (dimmer: style[1] 0.5) are never selected by the game, so
         * the cursor over one must not chase it (the highlight shook between the rows around it). */
        int stats=0, label=-1, value_w=0, bright_label=0, nvals=0;
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (t->item!=list.item || now-t->us>100000 || t->ordinal<1) continue;
            if (t->x>list.colx+300.0f && t->w>0.5f && t->h<30.0f) ++nvals;
            if (t->h>=22.0f && t->h<=29.0f && fabsf(t->x-list.colx)<1.0f && t->style[1]>=0.7f) bright_label=1;   /* (Récompenses: every label, the highlighted one too, is at style[1] 0.5) */
            if (fabsf(t->x-list.colx)<1.0f && t->h>=22.0f && t->h<=29.0f && uy>=t->y-2.0f && uy<t->y+list.pitch-2.0f) label=i;
        }
        stats=nvals>=3;   /* (Médailles has one lone icon on the right, not a column of values) */
        if (!bright_label) stats=0;  /* Récompenses: every label is dim (style[1] 0.5) next to a value column; only Statistiques mixes dim titles with normal rows */
        if (label>=0) {
            const MenuText *l=&menu_texts[label];
            for (int i=0;i<menu_text_count;++i) {
                const MenuText *t=&menu_texts[i];
                if (t->item==list.item && now-t->us<=100000 && t->ordinal>=1 && t->x>list.colx+300.0f && t->h<30.0f && fabsf(t->y-l->y)<2.0f && t->w>0.5f) value_w=1;
            }
            if (stats) {
                if (l->style[1]<0.7f && l->style[5]<2.5f) { list_title=1; list_noclick=1; list_delta=0; }
                else if (value_w) list_noclick=1;
            }
        }
        pthread_mutex_unlock(&menu_text_lock);
    }
    if (list_found) {   /* Statistiques, highlight on a statistic (a row with a value): no "SÉLECTIONNER" in the prompt bar, see u1_bar_flags */
        int stats=0, sel_value=0, bright_label=0, nvals=0;
        pthread_mutex_lock(&menu_text_lock);
        const MenuText *sl=NULL;
        for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (t->item!=list.item || now-t->us>100000 || t->ordinal<1) continue;
            if (t->x>list.colx+300.0f && t->w>0.5f && t->h<30.0f) ++nvals;
            if (t->h>=22.0f && t->h<=29.0f && fabsf(t->x-list.colx)<1.0f && t->style[1]>=0.7f) bright_label=1;
            if (fabsf(t->x-list.colx)<1.0f && t->h>=22.0f && t->h<=29.0f && t->style[5]>2.5f) sl=t;
        }
        stats=nvals>=3 && bright_label;
        if (sl) for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (t->item==list.item && now-t->us<=100000 && t->ordinal>=1 && t->x>list.colx+300.0f && t->h<30.0f && fabsf(t->y-sl->y)<2.0f && t->w>0.5f) sel_value=1;
        }
        pthread_mutex_unlock(&menu_text_lock);
        if (stats && sel_value) u1_hide_select_us=now;
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
        if (clicked && !list_noclick) { h.list_click=1; h.list_deadline=now+1500000; h.list_active=1; h.list_presses=0; }
        if (list_title) h.list_active=0;
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
        const int n=h.queue>0 ? h.queue : -h.queue;
        if (h.drag && n>1 && h.menu && !u1_jump_steps && !u1_jump_busy) {   /* a slider far from the cursor: one real press (event 3 left, 4 right) carries the other steps, see u1_menu_event */
            u1_jump_menu=h.menu; u1_jump_type=h.queue>0 ? 4 : 3; u1_jump_us=now; u1_jump_steps=n-1;
            h.queue=h.queue>0 ? 1 : -1;
            h.next_step=now+45000;
        }
        h.queue+=h.queue>0 ? -1 : 1;
    }
    if (now<h.step_until) out|=h.step_dir==1 ? BTN_LEFT : BTN_RIGHT;
    out|=(uint32_t)sb_out;
    if (list_ctl && list_delta!=0 && !h.sb_drag) {
        /* never press past either end of the list: Up on the first row or Down on the last wraps round to the other end and the scroll bar's
         * thumb leaps across (the cursor moving fast while the highlight lags behind a frame or two asks for exactly that) */
        const int idx=menu_list_index(list.item);
        if (idx>=0) {
            if (list_delta<0 && -list_delta>idx) list_delta=-idx;
            else if (list_delta>0 && h.sb_cached_item==list.item && h.sb_n>0 && list_delta>h.sb_n-1-idx) list_delta=h.sb_n-1-idx>0 ? h.sb_n-1-idx : 0;
        }
    }
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
/* ---- the mouse in the menus of U2 (collection) ---------------------------------------------------
 * U2's menus are not objects the port can ask (the item drawing differs from U1's), but its text drawing is the same routine: each
 * row text comes with its rectangle (1280x720 space, rows 33.6 tall) and the selected row has style[5] at 3 (probe, 2026-10-21).
 * A menu is on screen while such a row was drawn lately. Hover: the d-pad is pressed (one step per 220 ms, 16 at most per move of the
 * cursor) until the selected row is the one under the cursor; a click does the same, then presses cross. Not yet: wheel, sliders,
 * lists, Yes/No boxes. */
static int u2_menu_on, u2_hover, u2_menu_reset;
static volatile uint64_t u3_credits_sel_us;   /* U3: the last row of the Options panel (Crédits) was the selected one at this moment */
static volatile int u2_credits_on;     /* the credits are on screen (u2_credits_us) */
static volatile int u2_layout_on;      /* the controller's picture is on screen (u2_layout_us) */
static volatile int u2_keys_row_sel;   /* the Commandes row (the first of the two wide rows of that screen) is the selected one */
static int u2_list_rows_on(uint64_t now);   /* (below, with the table of the lists: called with menu_text_lock held) */
static int u2_menu_active(uint64_t now) {
    static uint64_t seen;
    static int disabled=-1;   /* BB_U2_MENU=0: no mouse in the menus (to tell a crash of the game from this code) */
    if (disabled<0) { const char *e=getenv("BB_U2_MENU"); disabled=e && atoi(e)==0; }
    if (disabled) return 0;
    int hi=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<U2_ROWS && !hi;++i) if (u2_rows[i].us && now-u2_rows[i].us<150000 && u2_rows[i].s5>=2.5f) hi=1;
    if (!hi) hi=u2_list_rows_on(now);   /* (the bar can scroll the highlighted row out of sight: the rows of the list still say it is a menu) */
    pthread_mutex_unlock(&menu_text_lock);
    const int layout=u2_layout_us && now-u2_layout_us<400000 && u2_layout_us<=now+1000000;
    u2_layout_on=layout;
    const int credits=u2_credits_us && now-u2_credits_us<300000 && u2_credits_us<=now+1000000;   /* the credits have no row to highlight */
    u2_credits_on=credits;
    const int carousel=u2_popup_us && now-u2_popup_us<700000 && u2_popup_us<=now+1000000;   /* (the carousel has no row to highlight either) */
    if (hi || layout || credits || carousel) seen=now;
    int on=seen && now-seen<300000;
    /* U3 credits: drawn without any text the hook sees, so the menu would go off. Right after the cross on the last row of the Options
     * panel (u3_credits_sel_us) the menu is kept on (Esc and the side button go back) until a menu row is drawn again. */
    if (runtime_u3_game) {
        static int guess; static uint64_t guess_us;
        if (hi || layout || carousel) guess=0;
        else if (!guess && !on && u2_menu_on && u3_credits_sel_us && now-u3_credits_sel_us<2000000) { guess=1; guess_us=now; printf("Runtime: U3 credits assumed on screen\n"); }
        if (guess && now-guess_us>600000000) guess=0;
        if (guess) { on=1; seen=now; u2_credits_on=1; }
    }
    if (on!=u2_menu_on) { u2_menu_on=on; u2_menu_reset=on; u2_menu_key_guard=1; bbgpu_menu_state(on); printf("Runtime: %s menu mouse %s\n",runtime_u3_game ? "U3" : "U2",on ? "on" : "off"); }
    return on;
}
/* ---- U2 sliders set by position ---------------------------------------------------------------
 * The game keeps the value of a slider in its data segment but the port does not know where, and it differs from one slider to the
 * next. The first time a slider is clicked it is found the way F8 does by hand: two snapshots with nothing changed tell the words that
 * move by themselves, then a press of left and a press of right: the word that goes down by a round step and comes back is the value
 * (a float, or an int for a percentage). The two ends are found by pressing left, then right, until the value stops, and the slider is
 * put back where it was. After that the value is read at each frame and left / right is pressed until it is the one under the cursor. */
#define U2_DATA_OFFSET 0x104c000u
#define U2_DATA_SIZE 0x6f0000u
typedef struct { float x, y; uint64_t addr; int is_int, ready; float step, vmin, vmax; } U2SliderMem;   /* ready: 0 unknown, 1 found, -1 not found */
static U2SliderMem u2_smem[8];
static struct { int phase, count, stuck, restore, dir_main, retried; uint64_t t, until, next; int dir; uint32_t *s[3]; uint8_t *noisy; U2SliderMem *m; float b0, last; } u2_sd;
static float u2_sval(const U2SliderMem *m, int *ok) {
    uint32_t w=0;
    *ok=safe_read(&w,m->addr,4);
    if (!*ok) return 0.0f;
    if (m->is_int) return (float)(int32_t)w;
    float f; memcpy(&f,&w,4);
    return f;
}
static uint32_t *u2_snap_take(void) {
    const uint64_t image=u2_image;
    if (!image) return NULL;
    uint32_t *b=malloc(U2_DATA_SIZE);
    if (!b) return NULL;
    for (size_t off=0;off<U2_DATA_SIZE;off+=4096) if (!safe_read((uint8_t *)b+off,image+U2_DATA_OFFSET+off,4096)) memset((uint8_t *)b+off,0,4096);
    return b;
}
static void u2_sd_free(void) {
    for (int i=0;i<3;++i) { free(u2_sd.s[i]); u2_sd.s[i]=NULL; }
    free(u2_sd.noisy); u2_sd.noisy=NULL;
}
static int u2_nice_step(float s, int is_int) {
    static const float f_steps[]={0.01f,0.02f,0.025f,0.05f,0.1f,0.125f,0.2f,0.25f,0.5f,1.0f,2.0f,5.0f,10.0f};
    static const float i_steps[]={1.0f,2.0f,5.0f,10.0f,20.0f,25.0f};
    const float *t=is_int ? i_steps : f_steps;
    const int n=is_int ? 6 : 13;
    for (int i=0;i<n;++i) if (fabsf(s-t[i])<=0.004f*t[i]+1e-5f) return 1;
    return 0;
}
/* base: before; a, b: after a first and a second press in the same direction (the value moves by the same round step twice) */
static int u2_sd_analyze(const uint32_t *base, const uint32_t *a, const uint32_t *b, const uint8_t *noisy, U2SliderMem *m) {
    const size_t words=U2_DATA_SIZE/4;
    int best_score=0;
    for (size_t i=0;i<words;++i) {
        if (noisy[i] || base[i]==a[i]) continue;
        float f0, f1, f2; memcpy(&f0,&base[i],4); memcpy(&f1,&a[i],4); memcpy(&f2,&b[i],4);
        int score=0, is_int=0; float step=0.0f;
        if (isfinite(f0) && isfinite(f1) && isfinite(f2) && f0>=0.0f && f0<=1000.0f && f1>=0.0f && f1<=1000.0f && f2>=0.0f && f2<=1000.0f) {
            const float s1=f1-f0, s2=f2-f1;
            if (fabsf(s1)>1e-4f && fabsf(s1-s2)<1e-5f*(1.0f+fabsf(f1)) && u2_nice_step(fabsf(s1),0)) { step=fabsf(s1); score=f0<=1.0001f ? 3 : 2; }
        }
        if (!score) {
            const int32_t i0=(int32_t)base[i], i1=(int32_t)a[i], i2=(int32_t)b[i];
            if (i0>=0 && i0<=1000 && i1>=0 && i1<=1000 && i2>=0 && i2<=1000 && i1!=i0 && i1-i0==i2-i1 && u2_nice_step((float)abs(i1-i0),1)) { step=(float)abs(i1-i0); score=1; is_int=1; }
        }
        if (!score) continue;
        if (mouse_debug()) printf("Mouse: U2 slider candidate image+%#zx %s base %08x a %08x b %08x step %g score %d\n",(size_t)U2_DATA_OFFSET+i*4,is_int ? "int" : "float",base[i],a[i],b[i],step,score);
        if (score>best_score) { best_score=score; m->addr=u2_image+U2_DATA_OFFSET+i*4; m->is_int=is_int; m->step=step; }
    }
    return best_score;   /* 3 a float up to 1, 2 a float, 1 an int (often a counter of presses: only taken when no float shows up) */
}
static void u2_sd_start(U2SliderMem *m, uint64_t now, int dir) {
    u2_sd_free();
    memset(&u2_sd,0,sizeof(u2_sd));
    u2_sd.m=m; u2_sd.phase=1; u2_sd.t=now; u2_sd.dir_main=dir;   /* the way the cursor is: the slider moves that way first, as it will anyway */
}
static void u2_sd_abort(int failed) {
    if (u2_sd.m) u2_sd.m->ready=failed ? -1 : 0;
    u2_sd_free();
    u2_sd.phase=0; u2_sd.restore=0;
}
/* one step of the search (called at each frame while u2_sd.phase is not 0); dir: 2 left, 3 right, pulsed through u2_sd.until */
static void u2_sd_step(uint64_t now, int row_ok) {
    U2SliderMem *m=u2_sd.m;
    if (!u2_sd.phase || !m) return;
    if (!row_ok) { printf("Runtime: U2 slider search stopped (the row is no longer the selected one)\n"); u2_sd_abort(0); return; }
    if (now<u2_sd.t) return;
    switch (u2_sd.phase) {
    case 1: u2_sd.s[0]=u2_snap_take(); if (!u2_sd.s[0]) { u2_sd_abort(1); return; } u2_sd.t=now+70000; u2_sd.phase=2; break;
    case 2: {
        u2_sd.s[1]=u2_snap_take();
        const size_t words=U2_DATA_SIZE/4;
        u2_sd.noisy=calloc(words,1);
        if (!u2_sd.s[1] || !u2_sd.noisy) { u2_sd_abort(1); return; }
        for (size_t i=0;i<words;++i) u2_sd.noisy[i]=u2_sd.s[0][i]!=u2_sd.s[1][i];
        u2_sd.dir=u2_sd.dir_main; u2_sd.until=now+70000; u2_sd.t=now+170000; u2_sd.phase=3;
        break; }
    case 3: u2_sd.s[2]=u2_snap_take(); if (!u2_sd.s[2]) { u2_sd_abort(1); return; } u2_sd.dir=u2_sd.dir_main; u2_sd.until=now+70000; u2_sd.t=now+170000; u2_sd.phase=4; break;
    case 4: {
        uint32_t *up=u2_snap_take();
        const int score=up ? u2_sd_analyze(u2_sd.s[1],u2_sd.s[2],up,u2_sd.noisy,m) : 0;
        const int ok=score>=2 || (score==1 && u2_sd.retried);
        free(up);
        if (!ok && !u2_sd.retried) {   /* at the end of the slider that way (an int that moves is a counter): the other way */
            u2_sd_free();
            u2_sd.retried=1; u2_sd.dir_main=u2_sd.dir_main==3 ? 2 : 3; u2_sd.phase=1; u2_sd.t=now+150000;
            printf("Runtime: U2 slider value not found, trying the other way\n");
            return;
        }
        u2_sd_free();
        if (!ok) { printf("Runtime: U2 slider value not found (the slider will be dragged by steps)\n"); u2_sd_abort(1); return; }
        int vok; const float v=u2_sval(m,&vok);
        /* the ends are guessed (0..1 for a float up to 1, else 0..1000) and corrected when a press does nothing (u2_menu_buttons) */
        /* (a value up to 100 is a percentage: Luminosité goes 0 .. 100 by 10, with a guess of 1000 the cursor had no effect until the far end was hit) */
        m->vmin=0.0f; m->vmax=(!m->is_int && v<=1.0001f) ? 1.0f : v<=100.0001f ? 100.0f : 1000.0f;
        m->ready=1; u2_sd.b0=v; u2_sd.restore=1; u2_sd.phase=0;
        printf("Runtime: U2 slider value found at %#llx (%s, step %g, now %g)\n",(unsigned long long)m->addr,m->is_int ? "int" : "float",m->step,v);
        break; }
    }
}
/* ---- U2 scroll bar of the long lists (Trésors, Médailles, Récompenses) -----------------------------------
 * The bar is a drawing, not a text: the port sees only the rows (29.2 tall, in the columns of u2_lists), the rail being on their left, as tall as the rows seen.
 * A click on the rail, or a drag, puts the selection on the row under the cursor by the game's own call (u2_widget_hook, set(value) on the data source of the
 * list: the way the game moves a selection), so the thumb jumps to the cursor as in U1. */
#define U2_LIST_ROW 29.2f
static const struct { float x, w; int vis; } u2_lists[]={{270.0f,768.0f,12},{334.2f,378.8f,12},{320.0f,628.8f,8},{359.2f,589.6f,8},{245.0f,418.0f,11},{342.0f,386.0f,5},{292.0f,656.8f,8}};   /* x, width, rows seen; the last two: Bonus > Cinématiques (11 rows from y 199.4), Récompenses > Catégories (5 rows from y 306.8, a value column at x 732; all rows seen, no scroll) (measured 2026-10-23); Récompenses: rows 320 / 628.8 or 359.2 / 589.6 wide, same right edge */
static int u2_list_rows_on(uint64_t now) {
    int n[7]={0,0,0,0,0,0,0};
    for (int i=0;i<U2_ROWS;++i) {
        const U2Row *r=&u2_rows[i];
        if (!r->us || now-r->us>=150000 || fabsf(r->h-U2_LIST_ROW)>=0.3f) continue;
        for (int q=0;q<7;++q) if ((fabsf(r->x-u2_lists[q].x)<1.0f && fabsf(r->w-u2_lists[q].w)<1.0f) || (q==2 && fabsf(r->x-359.2f)<1.0f && fabsf(r->w-589.6f)<1.0f) || (q==0 && fabsf(r->x-270.0f)<1.0f && fabsf(r->w-497.8f)<1.0f)) ++n[q];   /* (the same rows as the count of u2_menu_buttons: Statistiques has narrower rows where there is a value) */
    }
    for (int q=0;q<7;++q) if (n[q]>=(q==6 ? 3 : u2_lists[q].vis-3)) return 1;
    return 0;
}
static int u2_ls_drag;
/* one frame: list_on = the rows of a list are on screen, top = y of its first row, rowx = their x */
/* a click that moved the selection of a list: once the list reports the value asked for (the rows may have scrolled under the cursor), cross is pressed */
static volatile int u2_ls_cross_armed, u2_ls_cross_want, u2_ls_cross_fire;
static volatile uint64_t u2_ls_cross_ds, u2_ls_cross_deadline, u2_ls_cross_landed;
static volatile int u2_ls_wheel;  /* a list with a scroll bar is on screen: the wheel is handled by u2_ls_update, not by d-pad presses */
static int u2_ls_update(uint64_t now, int list_on, float top, float rowx, float roww, int vis, float ux, float uy, int left, int clicked, int hover_go, int have_sel, float sel_y) {
    uint64_t ds=0; int lo=0, hi=0, cur=0;
    if (!list_on || !u2_widget_pick(vis,now,&ds,&lo,&hi,&cur)) {
        if (list_on && mouse_debug()) { static uint64_t last; if (now-last>2000000) { last=now; printf("Mouse: U2 list on screen (rows x %.1f, %d seen) but no list widget\n",rowx,vis); } }
        u2_ls_drag=0; u2_ls_wheel=0; return 0;
    }
    /* a request left for the control of another page (the page was closed before the game's thread ran it) would block every jump of this list for good */
    if (u2_jump_pending && u2_jump_ds!=ds) { u2_jump_pending=0; u2_jump_keep=0; }
    if (u2_scroll_pending && u2_scroll_w!=u2_pick_self) u2_scroll_pending=0;
    {   /* the left button is held on the 3D model (Trésors): the selected text stays the one it was; if the game moves it (it reads the stick's vertical as "next text"), it is put back */
        static int lock_cur=-1; static uint64_t lock_us;
        if (!u2_model_drag) lock_cur=-1;
        else if (lock_cur<0) lock_cur=cur;
        else if (cur!=lock_cur && !u2_jump_pending && now-lock_us>40000) { u2_jump_ds=ds; u2_jump_val=lock_cur; u2_jump_keep=vis; u2_jump_pending=1; lock_us=now; if (mouse_debug()) printf("Mouse: U2 model drag: selection put back to %d (was %d)\n",lock_cur,cur); }
    }
    const float rail_h=(float)vis*U2_LIST_ROW;
    const int in_rail=ux>=rowx-70.0f && ux<=rowx-12.0f && uy>=top && uy<=top+rail_h;
    if (!left) u2_ls_drag=0;
    if (clicked && in_rail) u2_ls_drag=1;
    /* the cursor over a row of the list: the selection jumps under it by the same call (no d-pad steps) */
    const int over_rows=ux>=rowx-4.0f && ux<=rowx+roww+4.0f && uy>=top && uy<top+rail_h;
    if (have_sel) { u2_probe_row=(int)floorf((sel_y-top)/U2_LIST_ROW+0.5f); u2_probe_us=now; }
    static int pend, click_pend;   /* the cursor moved / was clicked and the selection has not caught up yet */
    static uint64_t last_us; static int last_want=-1;
    static float exp_sel_y; static int exp_ok;   /* after a hover jump to a row that is not at an edge the list does not scroll: the highlight is on the row jumped to, drawn or not yet */
    if (hover_go) pend=1;
    if (clicked) click_pend=1;
    if (!over_rows) pend=click_pend=0;
    /* the first row seen (widget +0x248): the row under the cursor is counted from it, whatever the highlight shows (it lags behind a jump, or is out of view after
     * the bar moved the view). BB_U2_LIST_TOP=0: counted from the highlighted row only, as before. */
    static int top_off=-1; if (top_off<0) { const char *e=getenv("BB_U2_LIST_TOP"); top_off=e && atoi(e)==0; }
    int top0=-1;
    if ((!have_sel || !top_off) && u2_pick_self) { int32_t t0=0; if (safe_read(&t0,u2_pick_self+0x248,4) && t0>=lo && t0<=hi) top0=t0; }
    if (top_off && have_sel) top0=-1;
    if (mouse_debug() && over_rows && hover_go && !(over_rows && !u2_ls_drag && (have_sel || top0>=0) && (pend || click_pend))) {   /* diagnosis: the hover is on the rows of a list and nothing is done */
        static uint64_t dbg2; if (now-dbg2>300000) { dbg2=now; printf("Mouse: U2 list hover idle: drag %d have_sel %d top0 %d pend %d click_pend %d jump_pending %d scroll_pending %d cur %d range %d..%d\n",u2_ls_drag,have_sel,top0,pend,click_pend,u2_jump_pending,u2_scroll_pending,cur,lo,hi); }
    }
    if (over_rows && !u2_ls_drag && (have_sel || top0>=0) && (pend || click_pend)) {
        const int fresh=have_sel && exp_ok && cur==last_want && now-last_us<400000;
        const int row=(int)floorf((uy-top)/U2_LIST_ROW);
        const int want=top0>=0 ? top0+row : cur+(int)floorf((uy-(fresh ? exp_sel_y : sel_y))/U2_LIST_ROW);
        /* the first and last rows seen are left alone by the hover (the game scrolls the list when they are selected: only a click goes there);
         * and nothing is computed from the selected row's position until the last jump has landed and the picture has caught up */
        const int landed=top0>=0 ? (want!=last_want || now-last_us>150000) : last_want<0 || cur==last_want || now-last_us>400000;   /* (counted from the first row: nothing to wait for but the game's own frame) */
        const int edge=(row<=0 || row>=vis-1) && hi-lo+1>vis;   /* (a list that shows all its rows never scrolls) */
        if (mouse_debug()) { static uint64_t dbg; if (now-dbg>150000) { dbg=now; printf("Mouse: U2 list hover row %d/%d top %.1f sel_y %.1f uy %.1f cur %d want %d range %d..%d edge %d click %d\n",row,vis,top,sel_y,uy,cur,want,lo,hi,edge,click_pend); } }
        if (want!=cur && want>=lo && want<=hi && !u2_jump_pending && landed && (top0>=0 || now-last_us>(exp_ok ? 50000 : 180000)) && (click_pend || !edge || u2_keep_view_on())) {   /* (edge rows by hover: only with BB_U2_KEEP=1, see below) */
            exp_ok=!edge && !click_pend && !u2_keep_view_on(); exp_sel_y=top+(float)row*U2_LIST_ROW;
            if (u2_keep_view_on()) u2_jump_keep=vis;   /* (the view stays where it is: no scroll when the row is at the edge) */
            u2_jump_ds=ds; u2_jump_val=want; u2_jump_pending=1; last_us=now; last_want=want;
            if (click_pend) { u2_ls_cross_want=want; u2_ls_cross_ds=ds; u2_ls_cross_deadline=now+1500000; u2_ls_cross_armed=1; }
            pend=click_pend=0;
        } else if (want==cur) pend=click_pend=0;
    }
    if (u2_ls_cross_armed) {
        if (now>u2_ls_cross_deadline) u2_ls_cross_armed=0;
        else if (cur==u2_ls_cross_want && ds==u2_ls_cross_ds) {
            if (!u2_ls_cross_landed) u2_ls_cross_landed=now;
            else if (now-u2_ls_cross_landed>=120000) { u2_ls_cross_armed=0; u2_ls_cross_landed=0; u2_ls_cross_fire=1; }
        } else u2_ls_cross_landed=0;
    }
    /* the wheel moves the list by three values per notch by the same call (the thumb runs along the rail), instead of one row of d-pad */
    {
        static uint64_t seen[2];
        for (int i=0;i<2;++i) {
            if (wheel_until_us[i]==seen[i]) continue;
            seen[i]=wheel_until_us[i];
            /* the wheel scrolls the view like the bar (the first row seen), the selected text stays where it is */
            if (hi-lo+1>vis) {
                int32_t t0=0, base=-1;
                if (u2_scroll_pending && u2_scroll_w==u2_pick_self) base=u2_scroll_top;
                else if (u2_pick_self && safe_read(&t0,u2_pick_self+0x248,4) && t0>=lo && t0<=hi) base=t0;
                if (base>=0) {
                    int t=base+(i==0 ? -3 : 3); if (t>hi+1-vis) t=hi+1-vis; if (t<lo) t=lo;
                    u2_scroll_w=u2_pick_self; u2_scroll_top=t; u2_scroll_pending=1; exp_ok=0;
                }
            } else {
                int want=cur+(i==0 ? -3 : 3); if (want<lo) want=lo; if (want>hi) want=hi;
                if (want!=cur && !u2_jump_pending) { u2_jump_ds=ds; u2_jump_val=want; u2_jump_pending=1; last_us=now; last_want=want; exp_ok=0; }
            }
        }
        u2_ls_wheel=1;
    }
    if (!u2_ls_drag || !left) return over_rows;
    const int n=hi-lo+1;
    float th=rail_h*(float)vis/(float)n;
    if (th<12.0f) th=12.0f;
    float f=(uy-th*0.5f-top)/(rail_h-th);
    f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
    /* the bar moves the view, not the selection: the last selected row stays selected (out of sight if need be) until the cursor hovers another */
    if (!u2_scroll_pending && n>vis) { u2_scroll_w=u2_pick_self; u2_scroll_top=lo+(int)(f*(float)(n-vis)+0.5f); u2_scroll_pending=1; exp_ok=0; }
    return over_rows;
}
/* ---- debug probe (BB_U2_SELPROBE=1): where does a menu page keep its selected row? ---------------------------------------------
 * Two presses of Down then one of Up, a snapshot of the game's data segment before and after each: an int that goes v, v+1, v+2, v+1 (and did not move by itself) is
 * a candidate. Lines "Mouse: U2 selprobe ..." in the journal. Runs once per menu opening, three times at most. */
static struct { int phase, runs, blocked; uint64_t t, press_until, on_since, done_us, cand[8]; int ncand; int dir; uint32_t *s[5]; uint8_t *noisy; } u2_sp;
static void u2_selprobe_free(void) { for (int i=0;i<5;++i) { free(u2_sp.s[i]); u2_sp.s[i]=NULL; } free(u2_sp.noisy); u2_sp.noisy=NULL; }
static void u2_selprobe_step(uint64_t now) {
    static int enabled=-1;
    if (enabled<0) { const char *e=getenv("BB_U2_SELPROBE"); enabled=e && atoi(e)>=1; }
    if (!enabled) return;
    if (!u2_menu_on) { u2_sp.on_since=0; u2_sp.blocked=0; if (u2_sp.phase) { u2_selprobe_free(); u2_sp.phase=0; } return; }
    if (!u2_sp.on_since) u2_sp.on_since=now;
    if (u2_sp.phase==0) { if (u2_sp.blocked && now-u2_sp.done_us>12000000) { u2_sp.blocked=0; u2_sp.on_since=now; }   /* (also without closing the menu: another page) */
        if (!u2_sp.blocked && u2_sp.runs<6 && now-u2_sp.on_since>4000000) { u2_sp.phase=1; u2_sp.t=now; } return; }
    if (now<u2_sp.t) return;
    const size_t words=U2_DATA_SIZE/4;
    switch (u2_sp.phase) {
    case 1: u2_sp.s[0]=u2_snap_take(); u2_sp.t=now+80000; u2_sp.phase=2; break;
    case 2: u2_sp.s[1]=u2_snap_take(); u2_sp.dir=1; u2_sp.press_until=now+70000; u2_sp.t=now+350000; u2_sp.phase=3; break;
    case 3: u2_sp.s[2]=u2_snap_take(); u2_sp.dir=1; u2_sp.press_until=now+70000; u2_sp.t=now+350000; u2_sp.phase=4; break;
    case 4: u2_sp.s[3]=u2_snap_take(); u2_sp.dir=0; u2_sp.press_until=now+70000; u2_sp.t=now+350000; u2_sp.phase=5; break;
    case 6: {   /* test (BB_U2_SELPROBE=2): the value 3 is written in every candidate; does the highlight go to the 4th row? */
        for (int k=0;k<u2_sp.ncand;++k) { const int32_t v=3; safe_write(u2_sp.cand[k],&v,4); }
        printf("Mouse: U2 selprobe wrote 3 in %d candidates\n",u2_sp.ncand);
        u2_sp.phase=0; u2_sp.blocked=1; u2_sp.done_us=now;
        break; }
    case 5: {
        u2_sp.s[4]=u2_snap_take();
        int n=0;
        u2_sp.ncand=0;
        if (u2_sp.s[0] && u2_sp.s[1] && u2_sp.s[2] && u2_sp.s[3] && u2_sp.s[4]) {
            for (size_t i=0;i<words && n<60;++i) {
                const int32_t a=(int32_t)u2_sp.s[1][i], b=(int32_t)u2_sp.s[2][i], c=(int32_t)u2_sp.s[3][i], d=(int32_t)u2_sp.s[4][i];
                if (u2_sp.s[0][i]!=u2_sp.s[1][i]) continue;
                if (a>=0 && a<200 && b==a+1 && c==a+2 && d==a+1) {
                    printf("Mouse: U2 selprobe image %#llx +%#zx: %d %d %d %d\n",(unsigned long long)u2_image,(size_t)U2_DATA_OFFSET+i*4,a,b,c,d);
                    if (u2_sp.ncand<8) u2_sp.cand[u2_sp.ncand++]=u2_image+U2_DATA_OFFSET+i*4;
                    ++n;
                    uint64_t q[16];
                    if (n<=2 && safe_read(q,u2_image+U2_DATA_OFFSET+i*4-0x40,sizeof(q))) {
                        printf("Mouse: U2 selprobe words from -0x40:");
                        for (int k=0;k<16;++k) printf(" %llx",(unsigned long long)q[k]);
                        printf("\n");
                    }
                }
            }
        }
        printf("Mouse: U2 selprobe done (%d candidates)\n",n);
        u2_selprobe_free(); ++u2_sp.runs; u2_sp.blocked=1; u2_sp.done_us=now;   /* (until the menu closes) */
        { const char *e=getenv("BB_U2_SELPROBE"); if (e && atoi(e)==2 && n>0) { u2_sp.phase=6; u2_sp.t=now+2500000; } else u2_sp.phase=0; }
        break; }
    }
}
/* microseconds between two d-pad presses of the hover in the menus (BB_U2_STEP_MS, default 120; the press is held half of it) */
static int u2_direct_off(void) { static int off=-1; if (off<0) { const char *e=getenv("BB_U2_SLIDER_DIRECT"); off=e && atoi(e)==0; } return off; }   /* BB_U2_SLIDER_DIRECT=0: the old search of the value in memory */
static int u2_mj_on(void) { static int on=-1; if (on<0) { const char *e=getenv("BB_U2_MJUMP"); on=!(e && atoi(e)==0); } return on; }   /* BB_U2_MJUMP=0: d-pad steps only */
static uint64_t u2_step_us(void) { static int ms=-1; if (ms<0) { const char *e=getenv("BB_U2_STEP_MS"); ms=e ? atoi(e) : 120; if (ms<30) ms=30; if (ms>400) ms=400; } return (uint64_t)ms*1000u; }
/* ---- U3 menus ---------------------------------------------------------------------------------
 * Same drawn rows as U2 (1280 x 720; 33.6 tall, 36 for a prompt, 40 for a dialog button), but the selected row is marked by style[6]
 * at 1 instead of style[5] (u2_row_note keeps it as s5 = 3, see u2_text_probe). Nothing else of U2's menus exists here yet (no list
 * widget hook, no model, no carousel): the cursor picks the row under it, the d-pad steps the selection onto that row (one press per
 * BB_U2_STEP_MS, 16 at most for one move), a click on the selected row presses cross, the wheel goes one row up / down, the right
 * button goes back. */
static uint32_t u3_menu_buttons(const bool *k, uint32_t mouse, uint64_t now) {
    static float last_x=-1e9f, last_y=-1e9f;
    static int last_left, dir=-1, presses;
    static uint64_t next_step, press_until, cross_until;
    static int sl_drag, sl_dir, sl_est, sl_queue, sl_zero, sl_pend;
    static float sl_f;
    static int sl_direct, sl_lo, sl_hi, sl_lastv; static uint64_t sl_ds;
    static float sl_x, sl_w;
    static uint64_t sl_press_until, sl_next_step;
    /* The sliders already moved on this screen: their place (in steps) is kept, so a second click does not go back to the left end
     * first. Cleared as soon as the top rail changes (another screen). The game's value is not read (no address known for U3). */
    static int sl_mem[8]={-1,-1,-1,-1,-1,-1,-1,-1}, sl_slot=-1;
    static float sl_mx[8], sl_my[8], sl_top=-1e9f;
    uint32_t out=0;
    const int wheel=now<wheel_until_us[0] || now<wheel_until_us[1];
    static int ls_pend, ls_click;   /* the cursor moved / was clicked over the rows of a list and the selection has not followed yet */
    static int ls_wlock, ls_wlock_set; static float ls_wx, ls_wy;   /* after a wheel notch, a hand tremor of a few pixels is not a hover move (the rows run under the cursor) */
    const int list_on=u3_list.us && now-u3_list.us<150000;   /* a list with a scroll bar (u3_list_hook): the wheel moves its selection by set(value), three rows a notch */
    if (now<wheel_until_us[0] && !list_on) out|=BTN_UP;          /* one row up / down per notch, as U2 */
    if (now<wheel_until_us[1] && !list_on) out|=BTN_DOWN;
    if (list_on) {
        static uint64_t seen[2];
        for (int i=0;i<2;++i) {
            if (wheel_until_us[i]==seen[i]) continue;
            seen[i]=wheel_until_us[i];
            if (now>=wheel_until_us[i]) continue;
            ls_pend=ls_click=0;   /* the rows move under a still cursor: no hover jump back */
            ls_wlock=1; ls_wlock_set=0;
            if (mouse_debug()) printf("Mouse: U3 list wheel %s (cur %d, first %d, vis %d, range %d..%d)\n",i==0 ? "up" : "down",u3_list.cur,u3_list.first,u3_list.vis,u3_list.lo,u3_list.hi);
            if (u3_list.first>=0 && u3_list.hi-u3_list.lo+1>u3_list.vis) {   /* the view scrolls, the selection stays */
                int t=(u2_scroll_pending && u2_scroll_w==u3_list.w ? u2_scroll_top : u3_list.first)+(i==0 ? -3 : 3);
                if (t>u3_list.hi+1-u3_list.vis) t=u3_list.hi+1-u3_list.vis;
                if (t<u3_list.lo) t=u3_list.lo;
                u2_scroll_w=u3_list.w; u2_scroll_top=t; u2_scroll_pending=1;
                continue;
            }
            if (u3_list.first>=0 && u3_list.hi-u3_list.lo+1<=u3_list.vis) continue;   /* the whole list is on screen (Modes de rendu): nothing to scroll, the selection stays */
            int base=u2_jump_pending && u2_jump_ds==u3_list.ds ? u2_jump_val : u3_list.cur;
            base+=i==0 ? -3 : 3;
            if (base<u3_list.lo) base=u3_list.lo;
            if (base>u3_list.hi) base=u3_list.hi;
            u2_jump_ds=u3_list.ds; u2_jump_val=base; u2_jump_keep=0; u2_jump_pending=1;
        }
    }
    if (mouse & 8u) out|=BTN_CIRCLE;                 /* right button: back */
    SDL_Window *w=SDL_GetMouseFocus();
    if (!w) { sl_drag=0; return out; }
    float mx=0, my=0; int ww=0, wh=0;
    SDL_GetMouseState(&mx,&my);
    SDL_GetWindowSize(w,&ww,&wh);
    if (ww<=0 || wh<=0) { sl_drag=0; return out; }
    const float scale=fminf((float)ww/UI_WIDTH,(float)wh/UI_HEIGHT);
    const float ux=(mx-((float)ww-UI_WIDTH*scale)*0.5f)/scale, uy=(my-((float)wh-UI_HEIGHT*scale)*0.5f)/scale;
    const int moved=fabsf(mx-last_x)>=1.0f || fabsf(my-last_y)>=1.0f;
    last_x=mx; last_y=my;
    const int left=(mouse & 1u)!=0, clicked=left && !last_left;
    last_left=left;
    U2Row hov={0}, sel={0}, slid={0};
    int have_hov=0, have_sel=0, sel_value=0, have_slid=0;
    float rail_top=1e9f;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<8;++i) {   /* the rails of the sliders (u2_sliders, filled by the text hook for U3 too) */
        const U2Row *s=&u2_sliders[i];
        if (!s->us || now-s->us>=150000 || s->w<20.0f) continue;
        if (s->y<rail_top) rail_top=s->y;
        if (!have_slid && ux>=s->x-10.0f && ux<=s->x+s->w+10.0f && uy>=s->y-2.0f && uy<s->y+s->h+2.0f) { slid=*s; have_slid=1; }
    }
    for (int i=0;i<U2_ROWS;++i) {
        const U2Row *r=&u2_rows[i];
        if (!r->us || now-r->us>=100000 || r->h<24.0f || r->h>42.0f || r->w<60.0f) continue;
        if (fabsf(r->h-34.2f)<0.2f) continue;                     /* the section titles, never chosen */
        if (r->s5>=2.5f && (!have_sel || r->s5>sel.s5)) { sel=*r; have_sel=1; }
        if (r->w>750.0f && !(fabsf(r->x-380.0f)<1.0f && fabsf(r->w-520.0f)<1.0f)) continue;   /* a wide banner above the menu (1000 wide) is not a row: hovering it must not clear the selection */
        if (ux>=r->x-4.0f && ux<=r->x+r->w+4.0f && uy>=r->y && uy<r->y+r->h && (!have_hov || r->w<hov.w)) { hov=*r; have_hov=1; }
    }
    /* A statistics line (28.6 tall) has its value in a second text to its right (the same line, past the right edge of the label): the cross there opens
     * the comparison with friends, which does not work offline (U1 and U2 remove it the same way). Sub menu lines (Facile, Normal...: no value) keep it. */
    if (have_sel && sel.h>=26.0f && sel.h<=30.0f && (sel.x<288.0f || sel.x>350.0f))   /* (the Récompenses categories and their lists, labels at x 292..347, show a count or a mark on the right but can be chosen; the statistics are at x 258) */
        for (int i=0;i<U2_ROWS;++i) {
            const U2Row *r=&u2_rows[i];
            if (!r->us || now-r->us>=100000 || r->w<5.0f) continue;
            if (fabsf(r->y-sel.y)<2.0f && r->x>=sel.x+sel.w-1.0f) { sel_value=1; break; }
        }
    if (have_hov)
        for (int i=0;i<8;++i)
            if (u3_empty_us[i] && now-u3_empty_us[i]<300000 && fabsf(u3_empty_y[i]-hov.y)<1.0f) { have_hov=0; break; }   /* a ghost row: not a target */
    u3_sel_value=sel_value;
    if (have_sel && fabsf(sel.x-96.0f)<3.0f && fabsf(sel.w-400.0f)<3.0f && fabsf(sel.y-282.0f)<1.0f) u3_credits_sel_us=now;   /* (measured 2026-10-29: the Crédits row of the Options panel; a row "Rétablir" is below it) */
    u2_keys_row_sel=have_sel && u3_keys_row_us && now-u3_keys_row_us<150000 && fabsf(sel.y-u3_keys_row_y)<1.0f;
    if (sel_value) u3_hide_select_us=now_us();
    /* The < > of a row (u3_arrows: two 16 wide texts side by side): a click on the left one presses left, on the right one right, once the row is the selected one
     * (the row is jumped to first, as for a hover, then the press). */
    static int ar_dir; static float ar_cy, ar_x, ar_y, ar_h, ar_w; static uint64_t ar_until, ar_seen, ar_press_until;
    int arrow_click=0;
    if (clicked && u3_gallery_us && now-u3_gallery_us<4000000 && uy>=596.0f && uy<=664.0f && ((ux>=92.0f && ux<=146.0f) || (ux>=1134.0f && ux<=1188.0f))) {
        ar_dir=ux<640.0f ? 2 : 3; ar_until=0; arrow_click=1;   /* gallery viewer: no row to select; it reads the left stick (A / D work), not the d-pad */
        u3_gallery_stick_dir=ar_dir==2 ? -1 : 1; u3_gallery_stick_until=now+150000;        if (mouse_debug()) printf("Mouse: U3 gallery arrow press: %s\n",ar_dir==2 ? "left" : "right");
    }
    if (clicked && !arrow_click) {
        for (int i=0;i<16 && !arrow_click;++i) {
            const U2Row *a=&u3_arrows[i];
            if (!a->us || now-a->us>=150000) continue;
            for (int j=0;j<16;++j) {
                const U2Row *b=&u3_arrows[j];
                if (i==j || !b->us || now-b->us>=150000 || fabsf(a->y-b->y)>=1.0f || b->x<=a->x || b->x>a->x+a->w+8.0f) continue;
                if (ux>=a->x-4.0f && ux<=b->x+b->w+4.0f && uy>=a->y && uy<a->y+a->h) {
                    arrow_click=1; ar_dir=ux<b->x ? 2 : 3; ar_cy=a->y+a->h*0.5f; ar_h=a->h;
                    ar_x=1e9f; ar_w=0.0f; ar_y=a->y;
                    for (int q=0;q<U2_ROWS;++q) {   /* the row's column: the leftmost row-shaped text on that line */
                        const U2Row *r=&u2_rows[q];
                        if (!r->us || now-r->us>=100000 || r->h<24.0f || r->h>42.0f || r->w<60.0f || r->w>750.0f || fabsf(r->y+r->h*0.5f-ar_cy)>=4.0f) continue;
                        if (r->x<ar_x) { ar_x=r->x; ar_w=r->w; }
                    }
                    break;
                }
            }
        }
        if (arrow_click) { ar_until=now+1500000; ar_seen=0; ar_press_until=0; u3_jf_hit_us=0; if (mouse_debug()) printf("Mouse: U3 arrow click: %s (line %.1f, column x %.0f)\n",ar_dir==2 ? "left" : "right",ar_cy,ar_x); }
    }
    if (mouse_debug() && have_sel) {   /* debug: every text on the selected line (where are the < > arrows of U3?), when the line changes */
        static float last_cy; static uint64_t last_us;
        const float scy=sel.y+sel.h*0.5f;
        if (fabsf(scy-last_cy)>1.0f && now-last_us>600000) {
            last_cy=scy; last_us=now;
            printf("Mouse: U3 selected line y %.1f:",scy);
            for (int i=0;i<U2_ROWS;++i) { const U2Row *r=&u2_rows[i]; if (r->us && now-r->us<150000 && fabsf(r->y+r->h*0.5f-scy)<12.0f) printf(" [x%.0f y%.0f w%.0f h%.1f s%.1f]",r->x,r->y,r->w,r->h,r->s5); }
            printf("\n");
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    {   /* once, when the cursor reaches a rail or leaves it: to check that U3 draws one where U2 does */
        static int last_slid;
        if (have_slid!=last_slid) {
            last_slid=have_slid;
            if (mouse_debug()) printf("Mouse: U3 slider rail %s rect %.1f %.1f %.1f %.1f\n",have_slid ? "under the cursor" : "left",slid.x,slid.y,slid.w,slid.h);
        }
    }
    /* A slider: the button held on its rail presses left / right, the game's own steps. The game's value is not read in memory (no
     * address known for U3), so the bar is carried by counting steps: to its left end once (the reference), then to the cursor's
     * place on the rail (BB_U3_SLIDER_STEPS, 10 by default). The queue keeps going after the button is released, otherwise a quick
     * click would drop the end of the move. The row is already the selected one: no stepping, no cross. */
    if (rail_top<1e8f && fabsf(rail_top-sl_top)>0.5f) {   /* another screen: the remembered places mean nothing here */
        for (int i=0;i<8;++i) sl_mem[i]=-1;
        sl_top=rail_top; sl_slot=-1;
    }
    if (sl_drag && !left) sl_drag=0;   /* the button is up: the move goes on to its target */
    if (clicked && have_slid && !sl_drag && !arrow_click) {
        sl_drag=1; sl_pend=1; sl_x=slid.x; sl_w=slid.w; sl_queue=0; sl_next_step=now; sl_press_until=0; sl_dir=0;
        int slot=-1, free_slot=-1;
        for (int i=0;i<8;++i) {
            if (sl_mem[i]>=0 && fabsf(sl_mx[i]-slid.x)<0.5f && fabsf(sl_my[i]-slid.y)<0.5f) { slot=i; break; }
            if (sl_mem[i]<0 && free_slot<0) free_slot=i;
        }
        if (slot<0) slot=free_slot>=0 ? free_slot : 0;
        sl_slot=slot; sl_mx[slot]=slid.x; sl_my[slot]=slid.y;
        if (sl_mem[slot]>=0) { sl_est=sl_mem[slot]; sl_zero=0; }   /* this one was moved before: straight to the cursor */
        else { sl_est=0; sl_zero=u3_slider_steps()+3; sl_mem[slot]=0; }
        printf("Runtime: U3 slider drag start rail %.1f %.1f %.1f %.1f steps %d from %d\n",slid.x,slid.y,slid.w,slid.h,u3_slider_steps(),sl_est);
        sl_direct=0;   /* the control behind the rail (u3_widget_hook): its value is set at once, no presses */
        if (!u2_direct_off() && u2_slider_widget(slid.y+slid.h*0.5f,now,&sl_ds,&sl_lo,&sl_hi)) {
            sl_direct=1; sl_lastv=-99999; sl_zero=0; sl_queue=0; sl_pend=0;
            printf("Runtime: U3 slider direct: range %d..%d\n",sl_lo,sl_hi);
        }
    }
    if (sl_direct && sl_drag) {
        const int v=sl_lo+(int)floorf(sl_f*(float)(sl_hi-sl_lo)+0.5f);
        if (v!=sl_lastv && !u2_jump_pending) { u2_jump_ds=sl_ds; u2_jump_val=v; u2_jump_keep=0; u2_jump_pending=1; sl_lastv=v; }
    }
    if (sl_drag) {   /* the target follows the cursor while the button is down; a quick click releases before the bar reached its left end */
        float f=(ux-sl_x)/(sl_w>1.0f ? sl_w : 1.0f);
        sl_f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
    }
    if (!sl_direct && (sl_drag || sl_pend) && sl_zero<=0) {   /* aiming: the last cursor place on the rail is the target (not while the bar is still going to its left end) */
        if (!sl_drag) sl_pend=0;
        const int want=(int)floorf(sl_f*(float)u3_slider_steps()+0.5f);
        if (want!=sl_est) {
            sl_queue+=want-sl_est; sl_est=want;
            if (sl_queue>60) sl_queue=60;
            if (sl_queue<-60) sl_queue=-60;
        }
    }
    if (sl_zero>0) {   /* first, the bar to its left end: the position counts from there */
        if (now<sl_press_until) out|=BTN_LEFT;
        else if (now>=sl_next_step) { --sl_zero; sl_est=0; sl_press_until=now+45000; sl_next_step=now+90000; out|=BTN_LEFT; }
    } else if (sl_queue) {
        if (now<sl_press_until) out|=sl_dir>0 ? BTN_RIGHT : BTN_LEFT;
        else if (now>=sl_next_step) {
            sl_dir=sl_queue>0; sl_queue+=sl_dir ? -1 : 1;
            sl_press_until=now+45000; sl_next_step=now+90000;
            out|=sl_dir>0 ? BTN_RIGHT : BTN_LEFT;
            if (!sl_queue && sl_slot>=0) sl_mem[sl_slot]=sl_est;   /* arrived: the place is now known for this rail */
        }
    }
    if (sl_drag || sl_zero>0 || sl_queue) { u2_hover=0; presses=0; dir=-1; return out; }
    if (now<ar_until) {   /* an arrow was clicked: select its row, then press */
        u3_jf_x=ar_x; u3_jf_y=ar_y; u3_jf_h=ar_h; u3_jf_w=ar_w; u3_jf_us=now;
        if (have_sel && fabsf(sel.y+sel.h*0.5f-ar_cy)<8.0f) {
            if (!ar_seen) ar_seen=now;
            if (now-ar_seen>=100000) { ar_press_until=now+70000; ar_until=0; ar_seen=0; if (mouse_debug()) printf("Mouse: U3 arrow press: %s\n",ar_dir==2 ? "left" : "right"); }
        } else ar_seen=0;
    }
    if (now<ar_press_until) out|=ar_dir==2 ? BTN_LEFT : BTN_RIGHT;
    if (now<ar_until || now<ar_press_until || now<u3_gallery_stick_until) { u2_hover=0; presses=0; dir=-1; return out; }
    if (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_A] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_D] ||
              k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_RIGHT])) {
        u2_hover=0; presses=0; return out;   /* the keys are being used: the cursor does not move the selection */
    }
    /* A list with a scroll bar (Bonus > Cinématiques, Trésors, Statistiques): the rows are not widgets of their own, the list keeps the selected row in a data source.
     * Cursor over the rows: the selection jumps under it (set(value), counted from the highlighted row); click: jump, then cross once the list has landed; button held on the
     * bar (the left edge of the list): the selection goes to the place of the cursor along the bar (the game scrolls the view to it and the thumb follows). */
    static int ls_drag, ls_want=-1, ls_cross_want=-1; static uint64_t ls_last_us, ls_cross_deadline, ls_landed;
    if (!list_on || !left) ls_drag=0;
    if (list_on) {
        const float P=28.55f, lx=u3_list.rc[0], ly=u3_list.rc[1], lw=u3_list.rc[2];
        const int n=u3_list.hi-u3_list.lo+1;
        int vis=(int)floorf(u3_list.rc[3]/P+0.5f); if (vis<1) vis=1;
        const float rail_h=(float)vis*P;
        /* the highlighted row of the list itself (28.6 tall, in the list's first column): the texts of the side panel (x 705) are drawn "selected" too.
         * It also gives the top of the first row seen (Trésors: 119, not the rectangle's 114), kept while the highlight is out of view. */
        U2Row lsel={0}; int have_lsel=0;
        static float ls_y0; static uint64_t ls_y0_w;
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0;i<U2_ROWS;++i) {
            const U2Row *r=&u2_rows[i];
            if (!r->us || now-r->us>=100000 || fabsf(r->h-28.6f)>=0.4f || r->x<lx || r->x>lx+120.0f || r->s5<2.5f) continue;
            if (!have_lsel || r->s5>lsel.s5) { lsel=*r; have_lsel=1; }
        }
        pthread_mutex_unlock(&menu_text_lock);
        if (ls_y0_w!=u3_list.w) { ls_y0_w=u3_list.w; ls_y0=ly; }
        if (have_lsel && !u2_jump_pending && now-ls_last_us>250000 && u3_list.first>=0 && u3_list.cur>=u3_list.first && u3_list.cur<u3_list.first+vis) ls_y0=lsel.y-(float)(u3_list.cur-u3_list.first)*P;
        const int in_rail=ux>=lx-8.0f && ux<=lx+22.0f && uy>=ly && uy<ly+rail_h;
        const int over_rows=ux>lx+22.0f && ux<=lx+lw && uy>=ls_y0 && uy<ls_y0+rail_h;
        if (clicked && in_rail) { ls_drag=1; if (mouse_debug()) printf("Mouse: U3 list bar press (rect %.1f %.1f %.1f %.1f, %d values, %d rows seen)\n",lx,ly,lw,u3_list.rc[3],n,vis); }
        if (ls_wlock && !ls_wlock_set) { ls_wx=mx; ls_wy=my; ls_wlock_set=1; }
        if (ls_wlock && hypotf(mx-ls_wx,my-ls_wy)>=24.0f) ls_wlock=0;
        if (moved && !ls_wlock) ls_pend=1;
        if (clicked) { ls_click=1; ls_wlock=0; }
        if (!over_rows) ls_pend=ls_click=0;
        if (ls_drag && left) {
            float th=rail_h*(float)vis/(float)n; if (th<12.0f) th=12.0f;
            float f=(uy-th*0.5f-ly)/(rail_h-th); f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
            if (u3_list.first>=0 && n>u3_list.vis) {   /* the bar moves the view, not the selection */
                const int t=u3_list.lo+(int)floorf(f*(float)(n-u3_list.vis)+0.5f);
                if (!u2_scroll_pending) { u2_scroll_w=u3_list.w; u2_scroll_top=t; u2_scroll_pending=1; }
            } else {
                const int v=u3_list.lo+(int)floorf(f*(float)(n-1)+0.5f);
                if (v!=u3_list.cur && !u2_jump_pending) { u2_jump_ds=u3_list.ds; u2_jump_val=v; u2_jump_keep=0; u2_jump_pending=1; }
            }
        } else if (over_rows && (ls_pend || ls_click)) {
            const int row=(int)floorf((uy-ls_y0)/P);
            static int keep_off=-1; if (keep_off<0) { const char *e=getenv("BB_U3_KEEP"); keep_off=e && e[0]=='0'; }
            const int edge=(row<=0 || row>=vis-1) && n>vis;   /* (the first and last rows seen make the game scroll: only a click goes there) */
            const int landed=ls_want<0 || u3_list.cur==ls_want || now-ls_last_us>300000;
            const int want=u3_list.first>=0 ? u3_list.first+row : u3_list.cur+(int)floorf((uy-lsel.y)/P);   /* (counted from the first row seen: the bar may have scrolled the highlight out of view) */
            if (!have_lsel && u3_list.first<0) { /* (the highlight is not drawn yet: wait) */ }
            else if (want>=u3_list.lo && want<=u3_list.hi && want!=u3_list.cur && landed && !u2_jump_pending && (ls_click || !edge || !keep_off)) {   /* (the view stays put now: edge rows are fine too) */
                u3_keep_view=!keep_off;
                u2_jump_ds=u3_list.ds; u2_jump_val=want; u2_jump_keep=0; u2_jump_pending=1; ls_last_us=now; ls_want=want;
                if (ls_click) { ls_cross_want=want; ls_cross_deadline=now+1500000; ls_landed=0; }
                if (mouse_debug()) printf("Mouse: U3 list jump %d -> %d (row %d/%d, selected row top %.1f, cursor %.1f, first %d, lock %d)%s\n",u3_list.cur,want,row,vis,lsel.y,uy,u3_list.first,ls_wlock,ls_click ? " click" : "");
                ls_pend=ls_click=0;
            } else if (want==u3_list.cur) {
                if (ls_click) cross_until=now+120000;
                ls_pend=ls_click=0;
            }
        }
        if (ls_cross_want>=0) {   /* a click moved the selection: cross once the list reports it (and has been there 120 ms) */
            if (now>ls_cross_deadline) ls_cross_want=-1;
            else if (u3_list.cur==ls_cross_want) { if (!ls_landed) ls_landed=now; else if (now-ls_landed>=120000) { ls_cross_want=-1; ls_landed=0; cross_until=now+120000; } }
            else ls_landed=0;
        }
        if (over_rows || in_rail || ls_drag) {
            u2_hover=1; presses=0; dir=-1;
            if (now<cross_until) out|=BTN_CROSS;
            return out;
        }
    }
    if (!have_hov || !have_sel) { u2_hover=0; presses=0; dir=-1; return out; }
    u2_hover=1;
    if (moved) { presses=0; dir=-1; }
    const float sy=sel.y+sel.h*0.5f, hy=hov.y+hov.h*0.5f;
    static int jump_off=-1; if (jump_off<0) { const char *e=getenv("BB_U3_JUMP"); jump_off=e && e[0]=='0'; }
    if (fabsf(sy-hy)>1.0f && !jump_off) {   /* the row under the cursor is not the selected one: its flag is set at once (u3_widget_hook) */
        float col_x=sel.x, col_w=sel.w;   /* the column comes from the hovered line's label (leftmost text of that line): the selected "row" may be its value text (Bonus, Audio), far to the right */
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0, found=0;i<U2_ROWS;++i) {
            const U2Row *r=&u2_rows[i];
            if (!r->us || now-r->us>=100000 || r->h<24.0f || r->h>42.0f || r->w<60.0f || r->w>750.0f || fabsf(r->y-hov.y)>=1.0f) continue;
            if (!found || r->x<col_x) { col_x=r->x; col_w=r->w; found=1; }
        }
        pthread_mutex_unlock(&menu_text_lock);
        if (fabsf(hov.y-u3_jf_y)>0.5f || now-u3_jf_us>150000) u3_jf_hit_us=0;   /* another target: its widget must be seen again */
        u3_jf_x=col_x; u3_jf_y=hov.y; u3_jf_h=hov.h; u3_jf_w=col_w; u3_jf_us=now;
        if (mouse_debug()) { static float ly; if (fabsf(ly-hov.y)>0.5f) { ly=hov.y; printf("Mouse: U3 hover jump to rect %.1f %.1f %.1f %.1f (selected %.1f %.1f %.1f %.1f)\n",hov.x,hov.y,hov.w,hov.h,sel.x,sel.y,sel.w,sel.h); } }
        presses=0; dir=-1;
        if (clicked) cross_until=0;
    } else if (fabsf(sy-hy)>1.0f) {   /* BB_U3_JUMP=0: the d-pad is stepped towards it */
        const int want=hy>sy;
        if (dir!=want) { dir=want; presses=0; next_step=now; }
        if (wheel) presses=0;
        if (now<press_until) out|=want ? BTN_DOWN : BTN_UP;
        else if (!wheel && now>=next_step && presses<16) { press_until=now+(u2_step_us()/2<60000 ? u2_step_us()/2 : 60000); next_step=now+u2_step_us(); ++presses; }
    } else {
        presses=0; dir=-1;
        if (clicked && !have_slid) cross_until=now+120000;          /* on the selected row: cross, once (not on a slider: that is a drag) */
    }
    if (now<cross_until) out|=BTN_CROSS;
    return out;
}
static uint32_t u2_menu_buttons(const bool *k, uint32_t mouse, uint64_t now) {
    /* rows the game never selected when asked to (the text of a choice box, a title): the hover gives up on them for 30 s */
    static struct { float x, y; uint64_t until; } skips[16];
    static struct { float sl_last, sl_acc, sl_w, sl_cx, sl_cy, f_last, last_f; float exp_cy; uint64_t jump_until, jump_next, nojump_until, skip_until; int drag, sl_dir, sl_dir0, sl_pending, deferred, choice, ch_dir, give, f_pulses, f_stuck; U2SliderMem *sm; int dsl, dsl_lo, dsl_hi, dsl_sent; uint64_t dsl_ds; uint64_t sl_next, sl_until, f_next, f_deadline, deferred_until, choice_until, choice_seen, ch_until; float choice_cy; float last_x, last_y, press_sel_cy, press_sel_x; int last_left, active, presses, budget, click, dir, last_dir, stuck; uint64_t click_deadline, press_until, next_press, cross_until, same_since; } h;
    u2_hover=0;
    u2_selprobe_step(now);
    SDL_Window *w=SDL_GetMouseFocus();
    float mx=0, my=0; int ww=0, wh=0;
    if (!w) return (mouse & 8u) ? BTN_CIRCLE : 0;
    SDL_GetMouseState(&mx,&my);
    SDL_GetWindowSize(w,&ww,&wh);
    if (ww<=0 || wh<=0) return 0;
    const float scale=fminf((float)ww/UI_WIDTH,(float)wh/UI_HEIGHT);
    const float ux=(mx-((float)ww-UI_WIDTH*scale)*0.5f)/scale, uy=(my-((float)wh-UI_HEIGHT*scale)*0.5f)/scale;
    const int moved=fabsf(mx-h.last_x)>=1.0f || fabsf(my-h.last_y)>=1.0f;
    h.last_x=mx; h.last_y=my;
    const int left=(mouse & 1u)!=0, clicked=left && !h.last_left;
    h.last_left=left;
    if (moved) { h.active=1; h.presses=0; h.budget=0; h.stuck=0; h.last_dir=-1; }
    if (u2_mj_done) { if (u2_mj_done<0) { h.nojump_until=now+(u2_mj_done==-2 ? 300000 : 3000000); h.jump_until=0; h.next_press=0; if (u2_mj_done==-2) h.skip_until=now+300000; }   /* (-2: nothing selectable is drawn there, e.g. the invisible rows of Sortie audio: no d-pad steps either) */ u2_mj_done=0; }   /* (the jump was not possible on this page: d-pad steps for 3 s) */
    if (now<wheel_until_us[0] || now<wheel_until_us[1] ||
        (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_A] || k[SDL_SCANCODE_D] ||
               k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_RIGHT]))) h.active=0;
    U2Row hov={0}, sel={0}, slid={0};
    const int popup=u2_popup_us && now-u2_popup_us<700000 && u2_popup_us<=now+1000000;   /* the chapter carousel of the Chapitres page is open */
    { static int popup_last; if (popup!=popup_last) { popup_last=popup; printf("Runtime: U2 popup %s\n",popup ? "in" : "out"); } }
    {   /* a request to set a value that no control picked up (its page closed, or another control took its place) must not block every later one: it expires */
        static uint64_t pend_since;
        if (!u2_jump_pending) pend_since=0;
        else if (!pend_since) pend_since=now;
        else if (now-pend_since>500000) { u2_jump_pending=0; u2_jump_keep=0; pend_since=0; if (mouse_debug()) printf("Mouse: U2 stale jump request dropped\n"); }
    }
    int have_hov=0, have_sel=0, hi_n=0, have_slid=0, wide_n=0, ls_wide_n=0, on_cmd=0, ls_n[7]={0,0,0,0,0,0,0}, ls_sel[7]={0,0,0,0,0,0,0};
    float ls_miny[7]={1e9f,1e9f,1e9f,1e9f,1e9f,1e9f,1e9f};
    float hi_cy[16], wide_min=1e9f, wide_ys[16]; int wide_ys_n=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<8;++i) {
        const U2Row *s=&u2_sliders[i];
        if (s->us && now-s->us<150000 && fabsf(s->x-717.0f)<1.5f) on_cmd=1;
        if (s->us && now-s->us<100000 && ux>=s->x-10.0f && ux<=s->x+s->w+10.0f && uy>=s->y && uy<s->y+s->h) { slid=*s; have_slid=1; }
    }
    for (int i=0;i<U2_ROWS;++i) {
        const U2Row *r=&u2_rows[i];
        if (!r->us || now-r->us>=100000) continue;
        if (r->x>300.0f && r->x<500.0f && r->h>=33.0f && r->h<=33.9f)   /* (the section titles are 34.2 tall and as wide) */ { ++wide_n; if (r->y<wide_min) wide_min=r->y; { int dup=0; for (int q=0;q<wide_ys_n;++q) if (fabsf(wide_ys[q]-r->y)<0.5f) dup=1; if (!dup && wide_ys_n<16) wide_ys[wide_ys_n++]=r->y; } }   /* the two last rows of Commandes (three with ours) */
        if (fabsf(r->h-U2_LIST_ROW)<0.3f && fabsf(r->x-270.0f)<1.0f && fabsf(r->w-497.8f)<1.0f) ++ls_wide_n;   /* (a Statistiques row with a value) */
        if (fabsf(r->h-U2_LIST_ROW)<0.3f) for (int q=0;q<7;++q) if ((fabsf(r->x-u2_lists[q].x)<1.0f && fabsf(r->w-u2_lists[q].w)<1.0f) || (q==2 && fabsf(r->x-359.2f)<1.0f && fabsf(r->w-589.6f)<1.0f) || (q==0 && fabsf(r->x-270.0f)<1.0f && fabsf(r->w-497.8f)<1.0f))   /* (Statistiques has the rows of Trésors; those with a value are narrower, the value being in a second column) */ {   /* (Récompenses: the same list, its rows are 320 / 628.8 or 359.2 / 589.6 wide, same right edge) */ ++ls_n[q]; if (r->s5>=2.5f) ls_sel[q]=1; if (r->y<ls_miny[q]) ls_miny[q]=r->y; }   /* the rows of a long list */
        if (fabsf(r->h-34.2f)<0.2f) continue;   /* the section titles (Visée, Caméra...) and their bars: never selected */
        if (r->s5>=2.5f && (!have_sel || r->s5>sel.s5)) { sel=*r; have_sel=1; }
        if (r->s5>=2.5f && hi_n<16) hi_cy[hi_n++]=r->y+r->h*0.5f;
        if (ux>=r->x-4.0f && ux<=r->x+r->w+4.0f && uy>=r->y && uy<r->y+r->h) {
            int skipped=0;
            for (int s=0;s<16;++s) if (now<skips[s].until && fabsf(skips[s].x-r->x)<0.5f && fabsf(skips[s].y-r->y)<0.5f) skipped=1;
            /* the most selected one under the cursor, and the narrowest of equals (the choice, not the box that holds both) */
            if (!skipped && (!have_hov || r->s5>hov.s5+0.01f || (fabsf(r->s5-hov.s5)<=0.01f && r->w<hov.w))) { hov=*r; have_hov=1; }
        }
    }
    /* A confirmation (Oui / Non): two rows of the same size side by side (210 x 40, at x 380 and 690: a gap of 100 between them). The menu behind it keeps its own selected row, so only the pair counts while it is there: the chosen one has style[7] at 1. */
    U2Row da={0}, db={0};
    int dlg=0;
    for (int i=0;i<U2_ROWS && !dlg;++i) {
        const U2Row *a=&u2_rows[i];
        if (!a->us || now-a->us>=100000 || a->h<38.0f || a->h>42.0f) continue;   /* 40 tall: not the 33.6 rows (a slider rail and a value column of one row have the same size too) */
        for (int j=0;j<U2_ROWS;++j) {
            const U2Row *b=&u2_rows[j];
            if (i==j || !b->us || now-b->us>=100000) continue;
            if (fabsf(a->y-b->y)<1.0f && fabsf(a->w-b->w)<1.0f && fabsf(a->h-b->h)<1.0f && b->x>=a->x+a->w-1.0f && b->x-(a->x+a->w)<=150.0f && a->w<=260.0f) { da=*a; db=*b; dlg=1; break; }
        }
    }
    if (!on_cmd && wide_n>=9) for (int i=0;i<8;++i) if (u2_sliders[i].us && now-u2_sliders[i].us<150000) on_cmd=1;   /* (the rail of the slider is not always at x 717: the language changes the layout; the screen is the one with many rows and a slider) */
    pthread_mutex_unlock(&menu_text_lock);
    if (dlg) {
        sel=da.a>db.a+0.05f ? da : db; have_sel=1;   /* (no sign yet: the right one, "Non", is the one the game starts on) */
        have_hov=0;
        if (uy>=da.y && uy<da.y+da.h) { if (ux>=da.x && ux<da.x+da.w) { hov=da; have_hov=1; } else if (ux>=db.x && ux<db.x+db.w) { hov=db; have_hov=1; } }
        if (have_hov) { int skipped=0; for (int s=0;s<16;++s) if (now<skips[s].until && fabsf(skips[s].x-hov.x)<0.5f && fabsf(skips[s].y-hov.y)<0.5f) skipped=1; if (skipped) have_hov=0; }
        on_cmd=0;
        if (fabsf(da.a-db.a)<0.5f) have_hov=0;   /* the chosen one is not clear (the box is opening, or a change is under way): never validate on a guess */
        have_slid=0; hi_n=0;
    }
    int ls_q=-1;   /* two lists can be drawn at once (Récompenses): the one with the highlighted row is the one on screen */
    for (int q=0;q<7 && ls_q<0;++q) if (ls_n[q]>=(q==6 ? 3 : u2_lists[q].vis-3) && ls_sel[q]) ls_q=q;
    for (int q=0;q<7 && ls_q<0;++q) if (ls_n[q]>=(q==6 ? 3 : u2_lists[q].vis-3)) ls_q=q;   /* (a row or two may not have been drawn in the last 100 ms) */
    /* Trésors: the 3D model is drawn over the right of the rows (they are 768 wide). Over it the selection stays where it is and a click selects nothing:
     * the mouse turns the model while the left button is held (Statistiques has the same rows, but narrower ones with a value and no model). */
    if (ls_q>=0 && !dlg) {   /* a text outside the rows of the list (the title bar above it, the description bar under it): never the selected row nor the one under the cursor */
        const float bottom=ls_miny[ls_q]+(float)u2_lists[ls_q].vis*U2_LIST_ROW;
        if (have_sel && (sel.y>bottom-U2_LIST_ROW*0.5f || sel.y<ls_miny[ls_q]-U2_LIST_ROW*0.5f)) {
            have_sel=0;
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<U2_ROWS;++i) {
                const U2Row *r=&u2_rows[i];
                if (!r->us || now-r->us>=100000 || fabsf(r->h-U2_LIST_ROW)>=0.3f || r->s5<2.5f || r->y>bottom-U2_LIST_ROW*0.5f || r->y<ls_miny[ls_q]-U2_LIST_ROW*0.5f) continue;
                if (!have_sel || r->s5>sel.s5) { sel=*r; have_sel=1; }
            }
            pthread_mutex_unlock(&menu_text_lock);
        }
        if (have_hov && (hov.y>bottom-U2_LIST_ROW*0.5f || hov.y<ls_miny[ls_q]-U2_LIST_ROW*0.5f)) have_hov=0;
    }
    const int in_model=(u2_model_drag && left) || (ls_q==0 && !dlg && !(ls_wide_n>0) && ux>=712.0f && uy>=130.0f && uy<=600.0f);   /* (while it is held the cursor is hidden and locked, yet its position drifts with the motion: the press decides; the zone is right of the longest names, over the whole height of the panel) */
    if (!left) u2_model_drag=0; else if (clicked && in_model) u2_model_drag=1;
    if (in_model) { have_hov=0; hi_n=0; }
    if (popup) {   /* the carousel: the rows behind it are not the cursor's business; its arrows (left and right of the pictures) and the pictures are clicked by place */
        have_hov=0; hi_n=0; have_slid=0;
        if (clicked) {
            if (ux>=298.0f && ux<=980.0f && uy>=156.0f && uy<=475.0f) {
                if (ux<=430.0f) { h.ch_dir=2; h.ch_until=now+70000; printf("Runtime: U2 chapter carousel click: left\n"); }
                else if (ux>=850.0f) { h.ch_dir=3; h.ch_until=now+70000; printf("Runtime: U2 chapter carousel click: right\n"); }
                else { h.cross_until=now+120000; printf("Runtime: U2 chapter carousel click: choose\n"); }
            }
        }
    }
    u2_hover=have_hov;
    int ls_vis=ls_q>=0 ? u2_lists[ls_q].vis : 12;
    if (ls_q==6 && ls_n[6]>=3 && ls_n[6]<ls_vis) ls_vis=ls_n[6];   /* (Récompenses sub menus with icons: Galeries shows 8 rows, Altérations 7, the panel has room for 8) */
    const int ls_hover=u2_ls_update(now,!dlg && ls_q>=0 && !u2_sd.phase,ls_q>=0 ? ls_miny[ls_q] : 0.0f,ls_q>=0 ? u2_lists[ls_q].x : 0.0f,ls_q>=0 ? u2_lists[ls_q].w : 0.0f,ls_vis,in_model ? -1000.0f : ux,uy,left,clicked && !in_model,(moved || clicked) && h.active && !in_model,have_sel && fabsf(sel.h-U2_LIST_ROW)<0.3f,sel.y);
    if (in_model || u2_model_drag) h.active=0;
    if (ls_hover) h.active=0;   /* (the hover on a list is done above: no d-pad steps; a click still presses cross once the row is the selected one) */
    if (u2_row_inserted) {   /* Commandes, then ours, then Rétablir: ours is the middle one of the three wide rows */
        float mid=1e9f;
        for (int a=0;a<wide_ys_n;++a) { int below=0; for (int b=0;b<wide_ys_n;++b) if (wide_ys[b]>wide_ys[a]+0.5f) ++below; if (below==1) mid=wide_ys[a]; }   /* (the rows widths depend on the language: the middle one of the three lowest rows) */
        u2_keys_row_sel=on_cmd && wide_n>=3 && have_sel && sel.x>300.0f && sel.x<500.0f && fabsf(sel.y-mid)<1.0f;
    } else
    u2_keys_row_sel=on_cmd && wide_n>=2 && have_sel && sel.x>300.0f && sel.x<500.0f && fabsf(sel.y-wide_min)<1.0f;
    u2_sel_plain_row=have_sel && fabsf(sel.h-U2_LIST_ROW)<0.3f && fabsf(sel.x-270.0f)<1.0f && fabsf(sel.w-768.0f)<1.5f;
    if (u2_menu_reset) { u2_menu_reset=0; memset(skips,0,sizeof(skips)); h.active=0; h.click=0; h.presses=0; h.press_until=h.cross_until=0; }   /* a rebuilt menu (a language was chosen): wait for the cursor to move */
    if (!left) h.drag=0;
    /* The < > arrows of a row are two texts (a 33.6 tall box each, not selected, 20 apart; the glyph sits 15 in the box): the click on the left half is "left", on the right
     * half "right", as for the choices of a row (the row is selected first, then the press). The box of a slider's rail ends well before them. */
    int arrow=0; float arrow_cy=0.0f;
    if (clicked && !dlg) {
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0;i<U2_ROWS && !arrow;++i) {
            const U2Row *a=&u2_rows[i];
            if (!a->us || now-a->us>=150000 || a->s5>0.5f || a->h<33.0f || a->h>34.2f || a->w<100.0f) continue;
            for (int j=0;j<U2_ROWS;++j) {
                const U2Row *b=&u2_rows[j];
                if (i==j || !b->us || now-b->us>=150000 || b->s5>0.5f || b->h<33.0f || b->h>34.2f || b->w<100.0f) continue;
                if (b->x>a->x+14.0f && b->x<a->x+26.0f && fabsf(a->y-b->y)<1.0f && uy>=a->y && uy<a->y+a->h && ux>=a->x && ux<a->x+50.0f) { arrow=ux<a->x+25.0f ? 2 : 3; arrow_cy=a->y+a->h*0.5f; break; }
            }
        }
        pthread_mutex_unlock(&menu_text_lock);
    }
    if (clicked && mouse_debug()) printf("Runtime: U2 click dbg: ux %.0f uy %.0f hov %d [x%.0f y%.0f w%.0f h%.1f s%.1f] sel %d cy %.1f dlg %d slid %d arrow %d popup %d model %d ls %d\n",ux,uy,have_hov,hov.x,hov.y,hov.w,hov.h,hov.s5,have_sel,sel.y+sel.h*0.5f,dlg,have_slid,arrow,popup,in_model,ls_q);
    if (arrow) {
        h.choice=arrow; h.choice_cy=arrow_cy; h.choice_until=now+1500000; h.choice_seen=0;
        h.active=1; h.presses=0; h.budget=0; h.stuck=0; h.last_dir=-1; h.click=0;
        printf("Runtime: U2 menu arrow click: %s\n",arrow==2 ? "left" : "right");
    } else
    if (clicked && have_slid) {   /* on a slider's rail: no cross (it does nothing there), the row is selected and the mouse drags the value by steps */
        h.drag=1; h.sl_last=ux; h.sl_acc=0.0f; h.sl_w=slid.w; h.sl_cx=slid.x; h.sl_cy=slid.y+slid.h*0.5f; h.sl_next=0;
        h.sl_pending=1; h.give=0; h.f_pulses=0; h.f_stuck=0;
        h.sl_dir0=(ux-slid.x)/slid.w>0.5f ? 3 : 2;
        h.deferred=1; h.deferred_until=now+6000000;   /* (the row must be selected first, then the search takes about a second, twice when the first way was the slider's end) */
        {   /* the memory of this rail (found once, see above) */
            U2SliderMem *m=NULL;
            for (int i=0;i<8 && !m;++i) if (u2_smem[i].x!=0.0f && fabsf(u2_smem[i].x-slid.x)<1.0f && fabsf(u2_smem[i].y-slid.y)<1.0f) m=&u2_smem[i];
            for (int i=0;i<8 && !m;++i) if (u2_smem[i].x==0.0f) m=&u2_smem[i];
            if (!m) m=&u2_smem[0];
            if (m->x==0.0f || fabsf(m->x-slid.x)>=1.0f || fabsf(m->y-slid.y)>=1.0f) { memset(m,0,sizeof(*m)); m->x=slid.x; m->y=slid.y; }
            h.sm=m;
        }
        h.active=1; h.presses=0; h.budget=0; h.stuck=0; h.last_dir=-1;
        h.dsl=0;
        {   /* the slider is a control with a data source, like the lists: its value is set by the game's own call, at once, wherever the rail is clicked */
            uint64_t sds=0; int slo=0, shi=0;
            if (!u2_direct_off() && u2_slider_widget(slid.y+slid.h*0.5f,now,&sds,&slo,&shi)) {
                float f=(ux-slid.x)/slid.w; f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
                h.dsl=1; h.dsl_ds=sds; h.dsl_lo=slo; h.dsl_hi=shi; h.last_f=f;
                h.sl_pending=0; h.deferred=0; h.sm=NULL;
                h.dsl_sent=slo+(int)floorf(f*(float)(shi-slo)+0.5f);
                if (!u2_jump_pending) { u2_jump_ds=sds; u2_jump_val=h.dsl_sent; u2_jump_keep=0; u2_jump_pending=1; }
                printf("Runtime: U2 slider direct: range %d..%d, value %d at %.0f%%\n",slo,shi,h.dsl_sent,f*100.0f);
            }
        }
    } else if (clicked && have_hov) {
        /* a click on one of the choices written in a row (Oui / Non, Normale / Inversée): the row is selected, then left (the first
         * choice) or right (the last one) is pressed once. Cross does not choose anything there. */
        int choice=0;
        if (hov.h<30.0f && hov.w<=130.0f && hov.x>=600.0f) {
            const float cy=hov.y+hov.h*0.5f;
            float minx=1e9f, maxx=-1e9f; int n=0;
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<U2_ROWS;++i) {
                const U2Row *r=&u2_rows[i];
                if (!r->us || now-r->us>=100000 || r->h>=30.0f || r->w>130.0f || r->x<600.0f || fabsf(r->y+r->h*0.5f-cy)>=4.0f) continue;
                ++n; if (r->x<minx) minx=r->x; if (r->x>maxx) maxx=r->x;
            }
            pthread_mutex_unlock(&menu_text_lock);
            if (n>=2) choice=hov.x<=minx+1.0f ? 2 : hov.x>=maxx-1.0f ? 3 : 0;
            if (choice) { h.choice=choice; h.choice_cy=cy; h.choice_until=now+1500000; h.choice_seen=0; }
        }
        if (!choice) { h.click=1; h.click_deadline=now+1500000; }
        h.active=1; h.presses=0; h.budget=0; h.stuck=0; h.last_dir=-1;
    }
    if (u2_ls_cross_armed) h.click=0;   /* (the click on a list row is carried by u2_ls_update) */
    if (u2_ls_cross_fire) { u2_ls_cross_fire=0; h.cross_until=now+120000; h.click=0; printf("Runtime: U2 list click: cross\n"); }
    if (h.click && (!have_hov || now>h.click_deadline)) h.click=0;
    /* the same line (a row can have several texts side by side, labels and values): only whole lines count, and only up / down are pressed */
    const float hov_cy=hov.y+hov.h*0.5f, sel_cy=sel.y+sel.h*0.5f;
    if (mouse_debug() && have_sel) {   /* debug: every text on the selected line (the < > arrows of the selected row?), when the line changes */
        static float last_cy; static uint64_t last_us;
        if (fabsf(sel_cy-last_cy)>1.0f && now-last_us>600000) {
            last_cy=sel_cy; last_us=now;
            pthread_mutex_lock(&menu_text_lock);
            printf("Runtime: U2 selected line y %.1f:",sel_cy);
            for (int i=0;i<U2_ROWS;++i) { const U2Row *r=&u2_rows[i]; if (r->us && now-r->us<150000 && fabsf(r->y+r->h*0.5f-sel_cy)<8.0f) printf(" [x%.0f w%.0f h%.1f s%.1f a%.1f]",r->x,r->w,r->h,r->s5,r->a); }
            printf("\n");
            pthread_mutex_unlock(&menu_text_lock);
        }
    }
    /* a grid of menu rows (the main menu, difficulty): side by side on one line, one of them selected: there left / right is pressed.
     * A line of a list (labels and values, two columns of bindings) has several texts selected or is not 33.6 tall: up / down only. */
    int line_hi=0;
    for (int i=0;i<hi_n;++i) if (fabsf(hi_cy[i]-sel_cy)<4.0f) ++line_hi;
    const int line=have_hov && have_sel && fabsf(hov_cy-sel_cy)<8.0f;
    /* (also the Oui / Non of a confirmation: two 210 x 40 halves of one box, the one on the left aligned to its end) */
    const int grid=dlg ? have_hov && fabsf(hov.x-sel.x)>=1.0f
                       : line && fabsf(hov_cy-sel_cy)<1.5f && line_hi==1 && hov.h>=33.0f && hov.h<=34.5f && sel.h>=33.0f && sel.h<=34.5f && fabsf(hov.x-sel.x)>=1.0f;
    const int same=dlg ? have_hov && !grid : line && !grid;
    if (!same) h.same_since=0; else if (!h.same_since) h.same_since=now;
    if (same && !h.click) h.active=0;   /* arrived: nothing more until the cursor moves again */
    /* (the jump does not need the drawn highlight: it knows the focused row from memory, so it works while the highlight of a page that has just opened is still fading in) */
    if (have_hov && (have_sel || (!dlg && u2_mj_on() && now>=h.nojump_until)) && (h.active || h.click) && !u2_sd.phase) {
        if (!same) {
            if (mouse_debug()) { static uint64_t dbg; if (now-dbg>400000) { dbg=now; printf("Runtime: U2 jump check: ls %d grid %d dlg %d presses %d req %d hov_cy %.0f sel_cy %.0f h %.1f\n",ls_hover,grid,dlg,h.presses,u2_mj_req,hov_cy,sel_cy,hov.h); } }
            if (!ls_hover && !dlg && now>=h.nojump_until && h.presses==0 && now>=h.jump_next && !u2_mj_req && u2_mj_on()) {
                /* a normal page: the highlight goes straight to the row under the cursor (one row per row height between the two). The drawn highlight
                 * fades for a moment after a jump, so meanwhile the row jumped to (exp_cy) is where the highlight is, not the drawn one. */
                const int fresh=now<h.jump_until;
                const float ref=fresh ? h.exp_cy : sel_cy;
                const int rows=grid ? 1 : have_sel || fresh ? (int)ceilf(fabsf(hov_cy-ref)/fmaxf(hov.h,20.0f)-0.05f) : 1;   /* (a grid: another column of the same line) */
                if (rows>=1) { u2_mj_cx=hov.x+hov.w*0.5f; u2_mj_cy=hov_cy; u2_mj_uy=uy; u2_mj_done=0; u2_mj_req=1; h.exp_cy=hov_cy; h.jump_until=now+400000; h.jump_next=now+50000; h.next_press=now+400000; }
            }
            if (!ls_hover && have_sel && now>=h.jump_until && now>=h.skip_until && (h.presses==0 || h.presses<h.budget) && now>=h.next_press) {
                const int dir=grid ? (hov.x>sel.x ? 3 : 2) : hov_cy>sel_cy ? 1 : 0;   /* 0 up, 1 down, 2 left, 3 right */
                /* the steps to make, counted once (the list may scroll under the cursor meanwhile): one per row height between the two */
                if (h.presses==0) h.budget=grid ? 2 : (int)ceilf(fabsf(hov_cy-sel_cy)/fmaxf(hov.h,20.0f)-0.05f);
                if (h.presses>0 && fabsf(sel_cy-h.press_sel_cy)<1.0f && fabsf(sel.x-h.press_sel_x)<1.0f) ++h.stuck; else h.stuck=0;
                /* the selection went the other way (past the last row, the game wraps round to the first), or did not move: the row under the
                 * cursor is not one the game selects */
                const int wrapped=h.presses>0 && ((h.last_dir==1 && sel_cy<h.press_sel_cy-4.0f) || (h.last_dir==0 && sel_cy>h.press_sel_cy+4.0f));
                if (wrapped || h.stuck>=3 || (h.last_dir>=0 && dir!=h.last_dir)) {
                    if (wrapped || h.stuck>=3) {
                        int slot=0;
                        for (int s=0;s<16;++s) if (skips[s].until<skips[slot].until) slot=s;
                        skips[slot].x=hov.x; skips[slot].y=hov.y; skips[slot].until=now+30000000u;
                        printf("Runtime: U2 menu row at %.0f,%.0f cannot be selected: ignored\n",hov.x,hov.y);
                    }
                    h.presses=999; h.click=0;
                }
                else { h.dir=dir; h.last_dir=dir; h.press_sel_cy=sel_cy; h.press_sel_x=sel.x; h.press_until=now+u2_step_us()/2; h.next_press=now+u2_step_us(); ++h.presses; }
            }
        } else if (h.click && now-h.same_since>=60000) {
            h.cross_until=now+120000; h.click=0;
            printf("Runtime: U2 menu click on the row at %.0f,%.0f\n",hov.x,hov.y);
        }
    }
    if (h.choice && now>=h.choice_until) h.choice=0;
    if (h.choice) {   /* the row of the clicked choice is the selected one for a moment: press */
        if (have_sel && fabsf(sel_cy-h.choice_cy)<8.0f) {
            if (!h.choice_seen) h.choice_seen=now;
            if (now-h.choice_seen>=100000 && !u2_sd.phase) {
                h.ch_dir=h.choice; h.ch_until=now+70000; h.choice=0;
                printf("Runtime: U2 menu choice click: %s\n",h.ch_dir==2 ? "left" : "right");
            }
        } else h.choice_seen=0;
    }
    /* dragging a slider: one left / right press per tenth of the rail the cursor has moved (the steps of the game are tenths, measured
     * on the sensitivity), once the row is the selected one */
    const int sl_row_ok=have_sel && fabsf(sel_cy-h.sl_cy)<8.0f;
    if (moved) { h.give=0; h.f_stuck=0; }
    if (h.drag && left && h.sl_w>0.0f) { float f=(ux-h.sl_cx)/h.sl_w; h.last_f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f; }
    if (h.dsl) {   /* (the click already set the value; a held button follows the cursor) */
        if (!left) h.dsl=0;
        else if (!u2_jump_pending) {
            const int want=h.dsl_lo+(int)floorf(h.last_f*(float)(h.dsl_hi-h.dsl_lo)+0.5f);
            if (want!=h.dsl_sent) { u2_jump_ds=h.dsl_ds; u2_jump_val=want; u2_jump_keep=0; u2_jump_pending=1; h.dsl_sent=want; }
        }
    }
    if (h.sl_pending && h.sm && sl_row_ok && !u2_sd.phase) {
        h.sl_pending=0;
        if (h.sm->ready==0) u2_sd_start(h.sm,now,h.sl_dir0);
    }
    if (u2_sd.phase) u2_sd_step(now,sl_row_ok);
    if (h.sm && h.sm->ready==1 && !u2_sd.phase && sl_row_ok) {   /* the value is known: the slider goes where the cursor is */
        const U2SliderMem *m=h.sm;
        float tgt=0.0f; int have_tgt=0;
        /* where the cursor is (held) or was when the button went up (a quick click: the row had to be selected and the value found first) */
        float f=-1.0f;
        if (h.drag && left) f=h.last_f; else if (h.deferred && now<h.deferred_until) f=h.last_f;
        if (f>=0.0f) {
            tgt=m->vmin+f*(m->vmax-m->vmin);
            const float n=floorf((tgt-m->vmin)/m->step+0.5f);
            tgt=m->vmin+n*m->step; have_tgt=1;
        }
        if (have_tgt) {
            int vok; const float v=u2_sval(m,&vok);
            if (!vok || v<m->vmin-m->step || v>m->vmax+m->step) { h.sm->ready=0; h.sm->x=0.0f; h.deferred=0; printf("Runtime: U2 slider memory dropped (value out of its range)\n"); }
            else if (fabsf(v-tgt)<m->step*0.5f) { h.f_stuck=0; if (!(h.drag && left)) h.deferred=0; }
            else if (now>=h.f_next && !h.give) {
                /* one press at a time: the next one only once the value has moved (the game may be slow to apply a press, two in flight
                 * run the slider past the cursor) or 350 ms have passed */
                const int changed=h.f_pulses>0 && fabsf(v-h.f_last)>=1e-6f;
                if (h.f_pulses>0 && !changed && now<h.f_deadline) { /* waiting */ }
                else {
                    if (h.f_pulses>0 && !changed) ++h.f_stuck; else h.f_stuck=0;
                    if (h.f_stuck>=2 && h.f_pulses>0) {   /* a press did nothing: that is an end of the slider, the real one */
                        if (h.sl_dir==3) h.sm->vmax=v; else h.sm->vmin=v;
                        printf("Runtime: U2 slider end found: %g .. %g\n",h.sm->vmin,h.sm->vmax);
                        h.f_stuck=0; h.f_pulses=0;
                    }
                    else if (h.f_stuck>=3) { h.give=1; h.deferred=0; }
                    else { h.sl_dir=v<tgt ? 3 : 2; h.sl_until=now+60000; h.f_next=now+80000; h.f_deadline=now+350000; h.f_last=v; ++h.f_pulses; }
                }
            }
        }
    } else if (h.drag && !h.dsl && (!h.sm || h.sm->ready==-1)) {   /* value not found: the mouse drags by steps */
        h.sl_acc+=ux-h.sl_last; h.sl_last=ux;
        const float step_px=h.sl_w/10.0f;
        if (h.sl_acc>2.0f*step_px) h.sl_acc=2.0f*step_px; else if (h.sl_acc<-2.0f*step_px) h.sl_acc=-2.0f*step_px;   /* no backlog: the knob must not run on after the mouse stops */
        if (have_sel && fabsf(sel_cy-h.sl_cy)<8.0f && fabsf(h.sl_acc)>=step_px && now>=h.sl_next) {
            h.sl_dir=h.sl_acc>0.0f ? 3 : 2;
            h.sl_acc-=h.sl_acc>0.0f ? step_px : -step_px;
            h.sl_until=now+60000; h.sl_next=now+100000;
            if (mouse_debug()) {   /* the sensitivity's value (image+0x104f5a4), to count the steps of a rail */
                float v=-1.0f;
                if (u2_image) safe_read(&v,u2_image+0x104f5a4u,4);
                printf("Mouse: U2 slider press %s cursor %.1f rail %.1f..%.1f value %g\n",h.sl_dir==3 ? "right" : "left",ux,h.sl_cx,h.sl_cx+h.sl_w,v);
            }
        }
    }
    if (!left && !h.deferred) h.sl_until=0;   /* the button is up: no press left over (a slider still going to where the cursor was keeps its own) */
    uint32_t out=0;
    if (now<h.sl_until) out|=h.sl_dir==3 ? BTN_RIGHT : BTN_LEFT;
    if (now<u2_sd.until) out|=u2_sd.dir==3 ? BTN_RIGHT : BTN_LEFT;
    if (now<h.ch_until) out|=h.ch_dir==3 ? BTN_RIGHT : BTN_LEFT;
    if (now<h.press_until) out|=h.dir==0 ? BTN_UP : h.dir==1 ? BTN_DOWN : h.dir==2 ? BTN_LEFT : BTN_RIGHT;
    if (now<h.cross_until) out|=BTN_CROSS;
    if (now<u2_sp.press_until) out|=u2_sp.dir ? BTN_DOWN : BTN_UP;
    /* the chapter picker of the Chapitres page (its line, y 193, is the selected one): the wheel changes the chapter, as in a confirmation */
    const int chap=!dlg && (popup || (have_sel && fabsf(sel_cy-193.0f)<3.0f && (fabsf(sel.x-380.0f)<1.5f || fabsf(sel.x-510.0f)<1.5f)));
    if (now<wheel_until_us[0] && (dlg || chap || !u2_ls_wheel)) out|=(dlg || chap) ? BTN_LEFT : BTN_UP;     /* the wheel: one row up / down per notch (left / right in a confirmation) */
    if (now<wheel_until_us[1] && (dlg || chap || !u2_ls_wheel)) out|=(dlg || chap) ? BTN_RIGHT : BTN_DOWN;
    if (mouse & 8u) out|=BTN_CIRCLE;
    if ((u2_layout_on || u2_credits_on) && (mouse & 2u)) out|=BTN_CIRCLE;   /* the controller's picture and the credits have nothing to click: the right button goes back */
    return out;
}
/* F8 in a U2 menu (BB_MOUSE_DEBUG): a snapshot of the data segment (image + 0x104c000, 0x6f0000 bytes) to find where a slider keeps its
 * value, as for U1 (menu_data_snapshot): two snapshots with nothing changed tell which words move by themselves, each later one reports
 * the other words that changed since the one before. */
static void u2_data_snapshot(void) {
    static uint32_t *prev; static int count; static uint8_t *noisy;
    const uint64_t image=u2_image;
    if (!image) return;
    const size_t words=U2_DATA_SIZE/4;
    uint32_t *now=malloc(U2_DATA_SIZE);
    if (!now) return;
    for (size_t off=0;off<U2_DATA_SIZE;off+=4096) if (!safe_read((uint8_t *)now+off,image+U2_DATA_OFFSET+off,4096)) memset((uint8_t *)now+off,0,4096);
    if (!noisy) noisy=calloc(words,1);
    if (count==1 && prev && noisy) {
        size_t moving=0;
        for (size_t i=0;i<words;++i) if (prev[i]!=now[i]) { noisy[i]=1; ++moving; }
        printf("Runtime: U2 data: noise snapshot, %zu words move by themselves\n",moving);
    } else if (count>=2 && prev && noisy) {
        int shown=0;
        for (size_t i=0;i<words;++i) if (prev[i]!=now[i] && !noisy[i]) {
            if (shown++<80) {
                float fo, fn; memcpy(&fo,&prev[i],4); memcpy(&fn,&now[i],4);
                printf("Runtime: U2 data: image+%#zx %08x -> %08x (%d -> %d, %g -> %g)\n",(size_t)U2_DATA_OFFSET+i*4,prev[i],now[i],(int)prev[i],(int)now[i],fo,fn);
            }
        }
        printf("Runtime: U2 data: %d words changed (snapshot %d)\n",shown,count);
    } else printf("Runtime: U2 data: first snapshot taken (image %#llx)\n",(unsigned long long)image);
    free(prev); prev=now; ++count;
}
/* U2: the Commandes menu has no row for the port's keyboard / mouse bindings window (U1 gets one built by a hook): a button is drawn in
 * the empty left panel of that screen (the overlay, bbgpu_controls_hint) and a click on it opens the window. The screen is the one whose
 * first slider (Sensibilité) has its rail at x 717. The window's language is the game's (BB_LANGUAGE, the console's number). */
#define U2_HINT_X 110.0f
#define U2_HINT_Y 300.0f
#define U2_HINT_W 250.0f
#define U2_HINT_H 46.0f
/* The game's language for the bindings window and the row label: 0 English, 1 French, 2 Spanish, 3 Portuguese (BB_LANGUAGE is the console's number). */
static int u2_text_lang=-1;   /* set from the game's own text of the Commandes row (the language is chosen in the game's Options > Language, live): 0 English, 1 French, 2 Spanish, 3 Portuguese */
static int u2_ui_language(void) {
    static int language=-1;
    if (u2_text_lang>=0) { if (language!=u2_text_lang) { language=u2_text_lang; bbgpu_set_language(language); } return language; }
    if (language<0) {
        const char *e=getenv("BB_LANGUAGE");
        const int id=e && *e ? atoi(e) : 1;
        language=(id==2 || id==22) ? 1 : (id==3 || id==20) ? 2 : (id==7 || id==17) ? 3 : 0;
        bbgpu_set_language(language);
    }
    return language;
}
/* The label of the row, put where the game looks its texts up (see u2_keys_label_patch). Done again every 300 ms (a language change loads fresh tables). */
static volatile int u2_keys_patched;
/* BB_U2_ID_SCAN=1: where do the rows of the Commandes menu keep their text ids? Every 4-byte occurrence of the ids of the "Commandes" row and
 * of "Rétablir les paramètres par défaut" in the committed writable memory of the guest, with the bytes around it (a row record, perhaps). */
static void u2_id_scan(void) {
#ifdef _WIN32
    static int done;
    const char *e=getenv("BB_U2_ID_SCAN");
    if (done || !e || e[0]!='1') return;
    done=1;
    static const uint32_t ids[2]={0xdd40dc7bu,0xb69a538au};
    char *buf=malloc(4u<<20);
    if (!buf) return;
    int hits[2]={0,0};
    uintptr_t addr=0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr<0x1400000000ull && VirtualQuery((LPCVOID)addr,&mbi,sizeof(mbi))) {
        const uintptr_t base=(uintptr_t)mbi.BaseAddress, size=mbi.RegionSize;
        addr=base+size;
        if (mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !mbi.Protect) continue;
        for (uintptr_t off=0;off<size;) {
            size_t n=size-off<(4u<<20) ? size-off : (4u<<20);
            SIZE_T got=0;
            if (!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(base+off),buf,n,&got) || got<4) break;
            for (size_t i=0;i+4<=got;i+=1) {   /* every byte: records may be packed */
                uint32_t w; memcpy(&w,buf+i,4);
                for (int k=0;k<2;++k) if (w==ids[k] && hits[k]<24) {
                    ++hits[k];
                    printf("Mouse: U2 id scan %08x at %#llx (image+%#llx):",w,(unsigned long long)(base+off+i),(unsigned long long)(base+off+i-u2_image));
                    for (size_t b=(i>=32 ? i-32 : 0);b<i+64 && b<got;++b) printf(" %02x",(unsigned char)buf[b]);
                    printf("\n");
                }
            }
            off+=n>4 ? n-3 : n;
            if (n<(4u<<20)) break;
        }
    }
    free(buf);
    printf("Mouse: U2 id scan done (%d, %d hits)\n",hits[0],hits[1]);
#endif
}
static void u2_keys_label_patch(uint64_t now) {
    static uint64_t last; static int logged;
    if (now-last<300000) return;
    last=now;
    if (u2_keys_tables[0]) { u2_id_scan(); }
    {   /* the "Commandes" row is renamed only when asked (BB_U2_KEYS_ROW=1): it replaces the controller picture, which must stay reachable */
        static int enabled=-1;
        if (enabled<0) { const char *e=getenv("BB_U2_KEYS_ROW"); enabled=e && e[0]=='1'; }
        if (!enabled && !u2_row_inserted) { u2_keys_patched=0; return; }
    }
    int ok=0;
    int lang=u2_ui_language();
    const char *label=NULL;
    for (int ti=0;ti<8;++ti) {
        uint64_t t=u2_keys_tables[ti];
        for (int depth=0;t && depth<6;++depth) {
            uint32_t count=0; uint64_t entries=0, base=0, parent=0; uint8_t valid=0;
            if (!safe_read(&count,t,4) || !safe_read(&entries,t+8,8) || !safe_read(&base,t+0x10,8) || !safe_read(&valid,t+0x38,1)) break;
            safe_read(&parent,t+0x58,8);
            if (mouse_debug() && logged<12) { ++logged; printf("Mouse: U2 text table %#llx: count %u entries %#llx base %#llx valid %u parent %#llx\n",(unsigned long long)t,count,(unsigned long long)entries,(unsigned long long)base,valid,(unsigned long long)parent); }
            if (valid && count && count<400000 && entries>0x10000 && base>0x10000) {
                uint32_t *arr=malloc((size_t)count*8);
                if (arr && safe_read(arr,entries,(size_t)count*8)) {
                    static uint64_t dumped[4]; static int dumped_n;
                    const char *dump_env=getenv("BB_U2_TEXT_DUMP");
                    int already=0;
                    for (int i=0;i<dumped_n;++i) if (dumped[i]==t) already=1;
                    if (dump_env && dump_env[0]=='1' && !already && dumped_n<4) {   /* every text of the table, to choose an unused long one */
                        dumped[dumped_n++]=t;
                        for (uint32_t i=0;i<count;++i) {
                            char b[100]={0}; size_t n=sizeof(b)-1;
                            while (n>0 && !safe_read(b,base+arr[2*i+1],n)) --n;
                            b[n]=0;
                            for (char *p=b;*p;++p) if ((unsigned char)*p<32) *p=' ';
                            printf("Mouse: U2 text dump %08x len %zu \"%s\"\n",arr[2*i],strlen(b),b);
                        }
                    }
                    for (uint32_t i=0;i<count;++i) if (arr[2*i]==U2_KEYS_TEXT_ID) {
                        {   /* the game's own text of the row tells whether the menus are in English, whatever BB_LANGUAGE says */
                            char orig[48]={0};
                            size_t on=sizeof(orig)-1;
                            while (on>0 && !safe_read(orig,base+arr[2*i+1],on)) --on;
                            orig[on]=0;
                            if (on>=4) {
                                const int found=!strncmp(orig,"Controller Layout",17) ? 0 : strstr(orig,"ommandes") ? 1 : !strncmp(orig,"Layout do",9) ? 3 : 2;   /* (the dump has four languages) */
                                if (found!=u2_text_lang) { u2_text_lang=found; printf("Runtime: U2 menu text \"%s\": language %d\n",orig,found); }
                                lang=u2_ui_language();
                            }
                        }
                        label=lang==1 ? "Touches clavier / souris" : lang>=2 ? "Teclado / Mouse" : "Keyboard / Mouse";
                        /* No memory of ours can be reached by a 32-bit offset from the table (the emulator owns the whole range around it): the label
                         * is written over the text of another id, one that is never shown here (the PlayStation Network chat restriction message, long
                         * enough in the four languages), and the entry of our id is pointed at it. */
                        uint32_t j=count;
                        for (uint32_t q=0;q<count;++q) if (arr[2*q]==U2_KEYS_HOST_TEXT_ID) { j=q; break; }
                        if (j==count) { if (logged<14) { ++logged; printf("Runtime: U2 keys row: the host text id is not in this table\n"); } break; }
                        const uint64_t host=base+arr[2*j+1];
                        u2_keys_host=host;
                        char cur[160]={0};
                        size_t n=sizeof(cur)-1;
                        while (n>0 && !safe_read(cur,host,n)) --n;
                        cur[n]=0;
                        const size_t need=strlen(label);
                        if (strcmp(cur,label)!=0) {
                            if (strlen(cur)>=need && safe_write(host,label,need+1)) { if (logged<14) { ++logged; printf("Runtime: U2 keys row: label written over the host text (table %#llx)\n",(unsigned long long)t); } }
                            else { if (logged<14) { ++logged; printf("Runtime: U2 keys row: the host text cannot take the label (%zu bytes, needs %zu)\n",strlen(cur),need); } break; }
                        }
                        if (u2_row_inserted) { ok=1; break; }   /* our own row carries the host text id: no entry to redirect */
                        if (arr[2*i+1]!=arr[2*j+1]) {
                            if (safe_write(entries+8u*i+4u,&arr[2*j+1],4)) { if (logged<14) { ++logged; printf("Runtime: U2 keys row label patched (table %#llx, entry %u)\n",(unsigned long long)t,i); } ok=1; }
                            else if (logged<14) { ++logged; printf("Runtime: U2 keys row label: the table cannot be written\n"); }
                        } else ok=1;                        break;
                    }
                }
                free(arr);
            }
            t=parent;
        }
    }
    u2_keys_patched=ok;
}
static void u2_controls_hint(uint64_t now, uint32_t mouse, int menu_on) {
    static int last_left;
    u2_ui_language();
    int on_commands=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<8;++i) if (u2_sliders[i].us && now-u2_sliders[i].us<150000 && fabsf(u2_sliders[i].x-717.0f)<1.5f) on_commands=1;
    pthread_mutex_unlock(&menu_text_lock);
    const int show=menu_on && on_commands && !u2_keys_patched;   /* the drawn button is only for when the row could not be made */
    int hot=0, clicked=0;
    SDL_Window *w=show ? SDL_GetMouseFocus() : NULL;
    if (w) {
        float mx=0, my=0; int ww=0, wh=0;
        SDL_GetMouseState(&mx,&my);
        SDL_GetWindowSize(w,&ww,&wh);
        if (ww>0 && wh>0) {
            const float scale=fminf((float)ww/UI_WIDTH,(float)wh/UI_HEIGHT);
            const float ux=(mx-((float)ww-UI_WIDTH*scale)*0.5f)/scale, uy=(my-((float)wh-UI_HEIGHT*scale)*0.5f)/scale;
            hot=ux>=U2_HINT_X && ux<=U2_HINT_X+U2_HINT_W && uy>=U2_HINT_Y && uy<=U2_HINT_Y+U2_HINT_H;
        }
    }
    const int left=(mouse & 1u)!=0;
    clicked=left && !last_left;
    last_left=left;
    bbgpu_controls_hint(show,hot,U2_HINT_X,U2_HINT_Y,U2_HINT_W,U2_HINT_H);
    if (show && hot && clicked) { printf("Runtime: U2 keys window requested from the Commandes menu\n"); bbgpu_open_controls(); }
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
    /* Keyboard and mouse, together with the controller. */
    menu_update(k,mouse);
    for (int a=0;a<16;++a) if (key_action_down(a,k,mouse,now)) d->buttons|=actions[a].ps;
    if (u1_image && u1_camera_dt_offset && !menu_mode && !pad_only) d->buttons&=~BTN_DOWN; /* U1 in play: no photo mode (a controller gets its d-pad as it is) */
    if (runtime_u3_game && !pad_only && !u2_menu_active(now)) d->buttons&=~BTN_DOWN;   /* U3 in play: the photo mode row is gone from Options, so no photo mode either (keyboard and mouse; a controller keeps its d-pad) */
    if (menu_mode && !menu_guard && !pad_only) d->buttons|=menu_mouse_buttons(k,mouse,now);
    else menu_hover=0;
    const int u2_menu=u2_menu_active(now) && !pad_only;
    u2_controls_hint(now,mouse,u2_menu);
    {   /* the controller's picture is of no use with a keyboard and a mouse: the bindings window opens over it, once each time the picture comes up */
        static int was_layout;
        const int layout=u2_menu && u2_layout_on;
        static int auto_open=-1;   /* only when asked (BB_U2_LAYOUT_KEYS=1): the picture is not to be taken over */
        if (auto_open<0) { const char *e=getenv("BB_U2_LAYOUT_KEYS"); auto_open=e && e[0]=='1'; }
        if (auto_open && layout && !was_layout) { printf("Runtime: U2 controller picture: keys window opened\n"); bbgpu_open_controls(); }
        was_layout=layout;
    }
    if (u2_snap_on()) { static int f6_was_down; const int f6=k && k[SDL_SCANCODE_F6]; if (f6 && !f6_was_down) u2_snap_dump(); f6_was_down=f6; }
    if (mouse_debug()) { static int f8_was_down; const int f8=k && k[SDL_SCANCODE_F8]; if (f8 && !f8_was_down && u2_menu) u2_data_snapshot(); f8_was_down=f8; }
    /* the wheel is the d-pad left / right of the play (weapon change): in a menu it moves up / down instead, not both at once */
    if (u2_menu && (now<wheel_until_us[0] || now<wheel_until_us[1])) d->buttons&=~(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT);
    u2_keys_label_patch(now);
    if (u2_menu) { d->buttons|=runtime_u3_game ? u3_menu_buttons(k,mouse,now) : u2_menu_buttons(k,mouse,now); 
        if (u2_model_drag && (mouse & 1u)) {   /* the model of Trésors turns with the native mouse (u2_model_hook), not with an emulated stick */
            const float k=0.004f*input.mouse_sensitivity;   /* radians per pixel */
            u2_model_ax+=mdx*k; u2_model_ay+=(input.mouse_invert_y ? -mdy : mdy)*k;
        }
        mdx=mdy=0; captured=0;
    } else u2_hover=0;
    bbgpu_menu_drag(u2_menu && (mouse & 1u) && u2_model_drag);
    if (u2_menu && u2_compare_us && now-u2_compare_us<300000 && !u2_sel_plain_row) d->buttons&=~BTN_CROSS;   /* statistics: no "compare with friends" (the rows that open a sub menu keep the cross) */
    if (u2_menu && runtime_u3_game && u3_sel_value) d->buttons&=~BTN_CROSS;   /* U3: the same, on a statistics line with its value on the right */
    {   /* the row that now reads "Touches clavier / souris": the cross (Space, the click) opens the bindings window instead of the controller picture */
        static int cross_was_down;
        const int cross=(d->buttons & BTN_CROSS)!=0;
        if (u2_menu && u2_keys_patched && u2_keys_row_sel) {
            if (cross && !cross_was_down) { printf("Runtime: U2 keys window requested from the Commandes row\n"); bbgpu_open_controls(); }
            d->buttons&=~BTN_CROSS;
        }
        cross_was_down=cross;
    }   /* the mouse turns the models of the menus only with the left button held */
    bbgpu_set_cursor((menu_mode || u2_menu) && !pad_only ? ((mouse & 1u) ? 2 : (menu_hover || u2_hover) ? 1 : 0) : 0);
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
    if (runtime_u3_game && now<u3_gallery_stick_until) d->buttons&=~BTN_R2;   /* the click itself is the mouse's fire button (R2): not with the stick push of a gallery arrow */
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    // The touchpad button without a finger on a touch surface is a left-side click.
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    const int lx=action_down(A_LS_RIGHT,k,mouse,now)-action_down(A_LS_LEFT,k,mouse,now);
    int ly=action_down(A_LS_DOWN,k,mouse,now)-action_down(A_LS_UP,k,mouse,now);
    if (lx) d->left_x=(uint8_t)(lx<0 ? 0 : 255);
    if (runtime_u3_game && now<u3_gallery_stick_until) d->left_x=(uint8_t)(u3_gallery_stick_dir<0 ? 0 : 255);
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
     * last 100 ms), the motion goes there. U2 and U3 have no joypad callback: the follow camera turns
     * with the mouse (vertically too where its arc is known), with the right stick held a little the
     * same way (follow_push); aiming, other cameras and a follow camera that keeps no turns get the
     * mouse as a right stick here. */
    pthread_mutex_lock(&camera_lock);
    const int following=camera_hook_us && now-camera_hook_us<100000;
    const int joypad=sticks_hook_us && now-sticks_hook_us<100000;
    const int native=u2_native_us && now-u2_native_us<100000; /* U2: the cover / look-around camera turns with the mouse */
    /* The animation camera (0x4383d0) takes the mouse while the walking camera is away, when BB_U2_BASE is on. */
    const int base_gap=u2_base_on() && !(u2_follow_seen_us && now-u2_follow_seen_us<U2_FOLLOW_RESYNC_US);
    if (mouse_debug()) { /* does the picture turn while no camera update runs (an animation holds the view)? */
        static int was_following, seen;
        static float yaw0;
        if (was_following && !following) {
            float y=0.0f; double a=0.0;
            seen=bbgpu_camera_yaw(&y,&a); yaw0=y;
        } else if (!was_following && following && seen) {
            float y=0.0f; double a=0.0;
            const int have=bbgpu_camera_yaw(&y,&a);
            printf("Mouse: GAPEND yaw %.1f -> %.1f%s (menu=%d) t=%.4f\n",(double)yaw0,have ? (double)y : 0.0,
                   have ? "" : "?",u2_menu,now*1e-6);
            seen=0;
        }
        was_following=following;
    }
    for (int q=0;q<CAMERA_QUEUES;++q) {
        /* queue 2 is the zoom camera's (U1 aiming): nothing else reads it, so it stays empty outside the aim */
        if (captured && (following || joypad || native || base_gap) && (q!=2 || aim_held || look_held || now_us()<aim_blend_until_us || now_us()<look_blend_until_us || (u2_chase_us && now_us()-u2_chase_us<200000))) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    /* gyro aiming: kept while aiming (at low frame rates the aim controller runs only every 100-200 ms) */
    const int raw_aiming=raw_aim_armed(now);
    if (captured && raw_aiming) { aim_dx+=mdx; aim_dy+=mdy; }
    else { aim_dx=aim_dy=0.0f; }
    pthread_mutex_unlock(&camera_lock);
    const int direct_x=base_gap || joypad || native || (following && !aim_native && !look_held && follow_direct);
    const int direct_y=base_gap || joypad || native || (following && !aim_native && !look_held && follow_direct && follow_has_arc);
    if (mouse_debug()) { /* state of the input loop: why the mouse does or does not reach the animation camera */
        static uint64_t last;
        if (now-last>1000000) {
            last=now;
            printf("Mouse: state menu=%d captured=%d base_gap=%d following=%d since_follow=%.0f ms mdx=%.0f t=%.4f\n",
                   u2_menu,captured,base_gap,following,u2_follow_seen_us ? (now-u2_follow_seen_us)*1e-3 : -1.0,
                   (double)mdx,now*1e-6);
        }
    }
    static uint64_t emulated_on_us; /* (debug) when the mouse went to the emulated right stick, 0 if it is native */
    if (captured && direct_x && direct_y) {
        if (mouse_debug() && emulated_on_us) { /* end of a spell on the emulated stick: how long was it, and did the picture turn meanwhile? */
            float yaw=0.0f; double age=0.0;
            const int have=bbgpu_camera_yaw(&yaw,&age);
            printf("Mouse: emulated stick off after %.0f ms%s yaw=%.2f t=%.4f\n",(now-emulated_on_us)*1e-3,
                   have && age<0.25 ? "" : "?",(double)yaw,now*1e-6);
            if (anim_cal_req>20.0 || anim_cal_req<-20.0)   /* the mouse turns right = a negative yaw change */
                printf("Mouse: ANIMCAL asked %.1f deg, the picture turned %.1f deg (ratio %.3f)\n",anim_cal_req,anim_cal_got,-anim_cal_got/anim_cal_req);
            anim_cal_req=anim_cal_got=0.0; anim_cal_have=0;
            anim_fb_err=0.0f; anim_fb_have=0;
        }
        emulated_on_us=0;
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    } else if (captured) {
        if (mouse_debug() && (mdx!=0 || mdy!=0)) {   /* monitoring: the mouse goes through the emulated right stick; which cameras are running then? */
            static uint64_t last_print; static unsigned n_since;
            if (!emulated_on_us) {
                emulated_on_us=now;
                float yaw=0.0f; double age=0.0;
                const int have=bbgpu_camera_yaw(&yaw,&age);
                printf("Mouse: emulated stick on%s yaw=%.2f cameras [",have && age<0.25 ? "" : "?",(double)yaw);
                for (int i=0;i<u2_dbg_ncams;++i) printf(" %#llx",(unsigned long long)u2_dbg_cams[i]);
                printf(" ] following=%d joypad=%d native=%d t=%.4f\n",following,joypad,native,now*1e-6);
            }
            ++n_since;
            if (now-last_print>1000000) {
                float yaw=0.0f; double age=0.0;
                const int have=bbgpu_camera_yaw(&yaw,&age);
                last_print=now;
                printf("Mouse: EMULATED stick (no native camera) %u mouse reports; cameras [",n_since);
                for (int i=0;i<u2_dbg_ncams;++i) printf(" %#llx",(unsigned long long)u2_dbg_cams[i]);
                printf(" ] following=%d joypad=%d native=%d aim=%d look=%d yaw=%.2f%s t=%.4f\n",following,joypad,native,aim_held,look_held,
                       (double)yaw,have && age<0.25 ? "" : "?",now*1e-6);
                n_since=0;
            }
        }
        const int anim_pad_idle=d->right_x>118 && d->right_x<138 && d->right_y>118 && d->right_y<138;   /* the pad's own right stick (or its keys) at rest, before the mouse takes it */
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
            /* U2 animations (grab, vault, ledge): the game's stick block is built by image+0x431580 with its length clamped to 1, which caps the turn at
             * the full stick's rate; the hook there (u2_stick_filter) takes the mouse's rate over the full stick's, unclamped */
            if (anim_pad_idle) {
                /* 1:1 with the free view: the game's animation ramps up and loses part of what it is asked, so the stick rate is the mouse's rate
                 * (smoothed) plus the shortfall (degrees the mouse asked for minus degrees the picture turned, measured on the main view matrix)
                 * taken up over 0.25 s. The shortfall is forgotten once the mouse has rested for 0.3 s (the animation's own drift is not fought). */
                static int feedback=-1;
                if (feedback<0) { const char *e=getenv("BB_U2_ANIM_FB"); feedback=!(e && e[0]=='0'); }
                if (!u2_anim_us || now-u2_anim_us>500000) { anim_fb_err=0.0f; anim_fb_have=0; anim_fb_moved_us=now; }   /* a new spell on the emulated stick */
                float y=0.0f; double a=0.0;
                const int have=bbgpu_camera_yaw(&y,&a) && a<0.1;
                const float ang=mdx*scale;                       /* degrees the mouse asks for in this report (right = +) */
                float turned=0.0f;
                if (have && anim_fb_have) turned=-wrap_degrees(y-anim_fb_yaw);   /* a turn to the right lowers the yaw */
                anim_fb_have=have; anim_fb_yaw=y;
                if (ang!=0.0f) anim_fb_moved_us=now;
                anim_fb_err+=ang-turned;
                anim_fb_err=anim_fb_err>180.0f ? 180.0f : anim_fb_err<-180.0f ? -180.0f : anim_fb_err;
                if (now-anim_fb_moved_us>300000) anim_fb_err*=SDL_expf(-dt/0.1f);
                u2_anim_sx=mouse_vx+(feedback && have ? anim_fb_err/0.25f : 0.0f);
                u2_anim_sy=mouse_vy; u2_anim_us=now;
                if (mouse_debug()) {
                    if (have && anim_cal_have) anim_cal_got+=wrap_degrees(y-anim_cal_yaw);
                    anim_cal_yaw=y; anim_cal_have=have;
                    anim_cal_req+=(double)ang;
                }
            }
        }
        if (mouse_vx!=0 && !rx && !direct_x) d->right_x=mouse_axis(mouse_vx);
        if (mouse_vy!=0 && !ry && !direct_y) d->right_y=mouse_axis(mouse_vy);
        if (mouse_debug()) { /* the requested rate against the rate the game gives: how much of the mouse the stick path keeps */
            static uint64_t last;
            if (now-last>100000) {
                float yaw=0.0f; double age=0.0;
                const int have=bbgpu_camera_yaw(&yaw,&age);
                last=now;
                printf("Mouse: stick vx=%.1f vy=%.1f axis=%d/%d yaw=%.2f%s t=%.4f\n",(double)mouse_vx,(double)mouse_vy,
                       d->right_x,d->right_y,(double)yaw,have && age<0.25 ? "" : "?",now*1e-6);
            }
        }
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
    aim_held=(d->buttons & BTN_L2)!=0 || ((d->buttons & BTN_L1) && zoom_camera_us && now_us()-zoom_camera_us<100000) || (u2_zoom_top_us && now_us()-u2_zoom_top_us<100000);
    if (aim_held) aim_blend_until_us=now_us()+1300000;
    aim_native=aim_held && zoom_camera_us && now_us()-zoom_camera_us<100000 && !(u2_zoom_under_us && now_us()-u2_zoom_under_us<100000) &&
               !(u2_zoom_below_cover_us && now_us()-u2_zoom_below_cover_us<100000);
    look_held=(d->buttons & (BTN_R3|BTN_UP))!=0; /* U1: the d-pad up (V) looks around */
    if (look_held) look_blend_until_us=now_us()+1300000;
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
    /* U2, U3 (no joypad callback): the right stick held a little while the mouse turns the follow
     * camera's direction (follow_push, see follow_camera), unless the stick is in use */
    const uint64_t now=now_us();
    if (!pad_only_mode && follow_push!=0.0f && camera_hook_us && now-camera_hook_us<100000 &&
        !(sticks_hook_us && now-sticks_hook_us<100000) && !aim_native && !look_held && d->right_x>=118 && d->right_x<=138)
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
