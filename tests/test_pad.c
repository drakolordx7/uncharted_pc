#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
static int rumble_calls;
static uint16_t rumble_low, rumble_high;
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_mouse_configure(int look) { (void)look; }
int bbgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel) {
    *dx=*dy=0; *buttons=0; *wheel=0;
    return 0;
}
int bbgpu_camera_yaw(float *yaw_degrees, double *age_seconds) { (void)yaw_degrees; (void)age_seconds; return 0; }
void bbgpu_prompts_controller(int device) { (void)device; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static bool SDLCALL rumble(void *userdata, Uint16 low, Uint16 high) {
    (void)userdata;
    ++rumble_calls;
    rumble_low=low;
    rumble_high=high;
    return true;
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    assert(_putenv_s(name,value)==0);
#else
    assert(setenv(name,value,1)==0);
#endif
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    char path[]="bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    char config[]="bbport-input-test-XXXXXX";
    fd=mkstemp(config);
    assert(fd>=0);
    close(fd);
    inject(config,"pad_vibration=0\npad_vibration_strength=50\n");
    set_env("BB_PAD_FILE",path);
    set_env("BB_PAD_HOST","1");
    set_env("BB_INPUT_CONFIG",config);
    set_env("SDL_VIDEODRIVER","dummy");
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert(input.pad_vibration==0);
    assert(input.pad_vibration_strength==50);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    /* Rotate keys (scripted as rot_ccw / rot_cw): the left stick circles at full deflection, a quarter
     * turn well inside the games' 0.31 s window, counter-clockwise first to the left. */
    for (int dir=-1; dir<=1; dir+=2) {
        inject(path,dir<0 ? "rot_ccw" : "rot_cw");
        int min_x=255, max_x=0, min_y=255, max_y=0, first_x=128;
        uint64_t quarter_us[4]={0,0,0,0}, last_q=0;
        for (int i=0;i<150;++i) {
            assert(pad_read_state(1,&data)==0);
            if (i==3) first_x=data.left_x;
            const int x=data.left_x, y=data.left_y;
            if (x<min_x) min_x=x;
            if (x>max_x) max_x=x;
            if (y<min_y) min_y=y;
            if (y>max_y) max_y=y;
            const int q=y<20 ? 0 : x>235 ? 1 : y>235 ? 2 : x<20 ? 3 : -1; /* up right down left */
            if (q>=0) {
                if (quarter_us[q]==0 || data.timestamp-quarter_us[q]>200000) {
                    if (last_q) assert(data.timestamp-last_q<312500);
                    last_q=data.timestamp;
                }
                quarter_us[q]=data.timestamp;
            }
            usleep(10000);
        }
        assert(min_x<=4 && max_x>=251 && min_y<=4 && max_y>=251);
        assert(dir<0 ? first_x<128 : first_x>128);
        assert(quarter_us[0] && quarter_us[1] && quarter_us[2] && quarter_us[3]);
    }
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.left_x==128 && data.left_y==128);
    /* The real keys: T held (default rotate_cw), the left stick circles; W alone is still plain
     * forward, and R (rotate_ccw) turns the other way. */
    {
        bool *keys=(bool *)SDL_GetKeyboardState(NULL);
        assert(keys);
        keys[SDL_SCANCODE_W]=true;
        assert(pad_read_state(1,&data)==0 && data.left_x==128 && data.left_y==0);
        keys[SDL_SCANCODE_W]=false;
        for (int dir=-1; dir<=1; dir+=2) {
            keys[dir<0 ? SDL_SCANCODE_R : SDL_SCANCODE_T]=true;
            int min_x=255, max_x=0, min_y=255, max_y=0, first_x=128;
            for (int i=0;i<150;++i) {
                assert(pad_read_state(1,&data)==0);
                if (i==3) first_x=data.left_x;
                if (data.left_x<min_x) min_x=data.left_x;
                if (data.left_x>max_x) max_x=data.left_x;
                if (data.left_y<min_y) min_y=data.left_y;
                if (data.left_y>max_y) max_y=data.left_y;
                usleep(10000);
            }
            keys[SDL_SCANCODE_R]=keys[SDL_SCANCODE_T]=false;
            assert(min_x<=4 && max_x>=251 && min_y<=4 && max_y>=251);
            assert(dir<0 ? first_x<128 : first_x>128);
        }
        assert(pad_read_state(1,&data)==0 && data.left_x==128 && data.left_y==128);
    }

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.Rumble=rumble;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    const uint8_t vibration[]={64,192};
    assert(pad_vibration(1,vibration)==0 && rumble_calls==0);
    input.pad_vibration=1;
    assert(pad_vibration(1,vibration)==0 && rumble_calls==1);
    assert(rumble_low==(64*257+1)/2 && rumble_high==(192*257+1)/2);
    input.pad_vibration_strength=100;
    assert(pad_vibration(1,vibration)==0 && rumble_calls==2);
    assert(rumble_low==64*257 && rumble_high==192*257);
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, rotate keys, SDL touch, vibration controls, overlay capture");
}
