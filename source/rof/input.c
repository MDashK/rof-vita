/*
 * THE RHYTHM OF FIGHTERS - PS Vita port
 * Input: front touch panel + buttons, delivered to the game exactly like
 * MainActivity.onTouchEvent() did on Android:
 *
 *   int[]   { action, actionIndex, pointerCount, id0, id1, ... }
 *   float[] { x0, y0, x1, y1, ... }   (screen pixels / RealViewScale)
 *   action: 1 DOWN, 2 POINTER_DOWN, 3 UP, 4 POINTER_UP, 5 MOVE, 0 CANCEL
 *
 * Mapping:
 *   Cross, Circle, Square, Triangle, D-pad, L, R -> tap / hold
 *   Left and right analog sticks                 -> flick in that direction
 *     (while a button holds a long note, the flick moves *that* finger, so
 *      long notes that end with a flick work)
 *   D-pad up/down in Free Mode                   -> scroll the song list
 *   START / SELECT                               -> Android BACK key
 *
 * How the game sees touches (NoteControl / TouchManager): every frame each
 * pointer slot remembers its previous position, and a press records the
 * slot's *previous* position as the flick origin. On a phone that is where
 * the last finger with that id was lifted. So a synthetic flick must start
 * exactly where its slot was last left, or the game measures a bogus
 * movement (the second flick in a row used to fail). We keep track of
 * where every slot was left and start flicks there.
 */

#include "rof/rof.h"

#include <math.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI_ImplBridge.h>

#include "utils/logger.h"

#define MAX_POINTERS 11 // TouchEvent keeps 11 slots
#define GAME_SLOTS   6  // the game has a TouchManager for ids 0..5
#define ANDROID_KEY_BACK_TRIGGER 0x80000

#define ACT_CANCEL       0
#define ACT_DOWN         1
#define ACT_POINTER_DOWN 2
#define ACT_UP           3
#define ACT_POINTER_UP   4
#define ACT_MOVE         5

// Distance of a stick flick, in game units (960x640 space).
#define FLICK_DIST 120.0f

typedef struct {
    int key;       // who owns it: touch report id, button, flick
    int id;        // Android pointer id handed to the game
    float x, y;    // game units
    int release;   // release on the next poll (flicks)
} Pointer;

static Pointer active[MAX_POINTERS];
static int n_active = 0;

// Where each pointer id was last seen by the game (its "previous position").
static float slot_x[MAX_POINTERS], slot_y[MAX_POINTERS];
static uint8_t slot_known[MAX_POINTERS];

static JavaDynArray *ev_ints = NULL;
static JavaDynArray *ev_floats = NULL;

static float view_scale = 1.0f;
static float center_x, center_y;

static uint32_t prev_buttons = 0;
static uint32_t consumed_buttons = 0; // held buttons whose finger already flicked away
static int stick_armed[2] = {1, 1};

// Keys for non-touch pointers
#define KEY_TOUCH(i)  (0x100 + (i))
#define KEY_BUTTON(b) (0x10000 + (b))
#define KEY_FLICK(n)  (0x20000 + (n))

/* ---- game state ----------------------------------------------------------- */

#define APP_SCENE_OFFSET  0x1fb8 // AppMain: current "loop" scene id
#define APP_SCROLL_OFFSET 0x90   // AppMain: song list scroller in Free Mode
#define FREE_LIST_STATE_OFFSET   0xdc28 // 0 idle, 1 settled (preview), 2 moving
#define FREE_LIST_COUNTER_OFFSET 0xdc2c // frames without movement

static void **app_instance = NULL;          // &AppMain::pApp
static void (*scroll_set_add)(void *, int); // TouchManagerScrollYPlus::setAdd

static int current_scene(void) {
    if (!app_instance || !*app_instance)
        return -1;
    return *(int *)((char *)*app_instance + APP_SCENE_OFFSET);
}

// Game main (also the tutorial's practice song) and timing test
static int scene_is_gameplay(int s) {
    return s >= 0x4a && s <= 0x51;
}

static int scene_is_free_menu(int s) {
    return s >= 0x40 && s <= 0x42;
}

/* ---- events ----------------------------------------------------------------- */

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    float wscale = ROF_SCREEN_W / ROF_GAME_W;
    float hscale = ROF_SCREEN_H / ROF_GAME_H;
    view_scale = wscale > hscale ? hscale : wscale;
    center_x = (ROF_SCREEN_W / 2.0f) / view_scale;
    center_y = (ROF_SCREEN_H / 2.0f) / view_scale;

    ev_ints = jda_alloc(MAX_POINTERS + 3, FIELD_TYPE_INT);
    ev_floats = jda_alloc(MAX_POINTERS * 2, FIELD_TYPE_FLOAT);

    app_instance = (void **)so_symbol(&so_mod, "_ZN7AppMain4pAppE");
    scroll_set_add = (void (*)(void *, int))so_symbol(&so_mod, "_ZN23TouchManagerScrollYPlus6setAddEi");
    if (!app_instance || !scroll_set_add)
        l_warn("input: game state symbols missing, scene-aware controls disabled");
}

static void send_event(int action, int action_index) {
    jint *ints = ev_ints->array;
    jfloat *floats = ev_floats->array;
    memset(ints, 0, (MAX_POINTERS + 3) * sizeof(jint));
    memset(floats, 0, MAX_POINTERS * 2 * sizeof(jfloat));
    ints[0] = action;
    ints[1] = action_index;
    ints[2] = n_active;
    for (int i = 0; i < n_active; i++) {
        ints[3 + i] = active[i].id;
        floats[i * 2] = active[i].x;
        floats[i * 2 + 1] = active[i].y;
        slot_x[active[i].id] = active[i].x;
        slot_y[active[i].id] = active[i].y;
        slot_known[active[i].id] = 1;
    }
    rof_native.onTouchEvent(rof_env, rof_clazz, (jintArray)ev_ints, (jfloatArray)ev_floats);
}

static int find_pointer(int key) {
    for (int i = 0; i < n_active; i++)
        if (active[i].key == key)
            return i;
    return -1;
}

static int id_in_use(int id) {
    for (int i = 0; i < n_active; i++)
        if (active[i].id == id)
            return 1;
    return 0;
}

// Android hands out the lowest pointer id not in use.
static int lowest_free_id(void) {
    for (int id = 0; id < MAX_POINTERS; id++)
        if (!id_in_use(id))
            return id;
    return -1;
}

static int slot_is_centered(int id) {
    return slot_known[id] && fabsf(slot_x[id] - center_x) < 1.0f && fabsf(slot_y[id] - center_y) < 1.0f;
}

static int pointer_down_id(int key, int id, float x, float y) {
    if (n_active >= MAX_POINTERS || id < 0 || find_pointer(key) >= 0)
        return -1;
    int idx = n_active++;
    active[idx].key = key;
    active[idx].id = id;
    active[idx].x = x;
    active[idx].y = y;
    active[idx].release = 0;
    send_event(idx == 0 ? ACT_DOWN : ACT_POINTER_DOWN, idx);
    return idx;
}

static void pointer_up(int idx) {
    send_event(n_active == 1 ? ACT_UP : ACT_POINTER_UP, idx);
    for (int i = idx; i < n_active - 1; i++)
        active[i] = active[i + 1];
    n_active--;
}

/* ---- buttons ---------------------------------------------------------------- */

// A tap/hold from a button. In gameplay any touch counts (position only
// matters for the pause button), so press where the slot already is: no
// movement, no phantom flick. Prefer slots that sit at the centre.
static void button_down(int key, int gameplay, int want_id0) {
    int id = -1;
    // Tip windows only listen to pointer id 0 (the game's main touch slot).
    if (want_id0 && !id_in_use(0))
        id = 0;
    if (id < 0 && gameplay) {
        for (int i = 0; i < GAME_SLOTS && id < 0; i++)
            if (!id_in_use(i) && slot_is_centered(i))
                id = i;
        if (id < 0) {
            for (int i = 0; i < GAME_SLOTS && id < 0; i++)
                if (!id_in_use(i) && slot_known[i])
                    id = i;
        }
    }
    if (id < 0)
        id = lowest_free_id();
    if (id < 0)
        return;
    float x = center_x, y = center_y;
    if (gameplay && slot_known[id]) {
        x = slot_x[id];
        y = slot_y[id];
    }
    pointer_down_id(key, id, x, y);
}

/* ---- flicks ----------------------------------------------------------------- */

static int flick_counter = 0;

// Flick with a free finger: press where the chosen slot was left and move
// FLICK_DIST in the same frame, release next frame. Pick the slot that ends
// up closest to the centre so positions never wander off screen.
static void free_flick(float dx, float dy) {
    int best = -1;
    float best_d = 1e9f;
    for (int id = 0; id < GAME_SLOTS; id++) {
        if (id_in_use(id) || !slot_known[id])
            continue;
        float ex = slot_x[id] + dx * FLICK_DIST - center_x;
        float ey = slot_y[id] + dy * FLICK_DIST - center_y;
        float d = ex * ex + ey * ey;
        if (d < best_d) {
            best_d = d;
            best = id;
        }
    }
    float sx = center_x, sy = center_y;
    if (best < 0) {
        best = lowest_free_id();
    } else {
        sx = slot_x[best];
        sy = slot_y[best];
    }
    if (best < 0)
        return;
    int key = KEY_FLICK(flick_counter++ & 0xFF);
    int idx = pointer_down_id(key, best, sx, sy);
    if (idx < 0)
        return;
    active[idx].x = sx + dx * FLICK_DIST;
    active[idx].y = sy + dy * FLICK_DIST;
    active[idx].release = 1;
    send_event(ACT_MOVE, 0);
}

static void do_flick(float dx, float dy, int gameplay) {
    // A button is holding a (long) note: flick with that finger.
    if (gameplay) {
        for (int i = n_active - 1; i >= 0; i--) {
            int key = active[i].key;
            if (key < KEY_BUTTON(0) || key >= KEY_FLICK(0) || active[i].release)
                continue;
            active[i].x += dx * FLICK_DIST;
            active[i].y += dy * FLICK_DIST;
            active[i].release = 1;
            consumed_buttons |= (uint32_t)(key - KEY_BUTTON(0));
            send_event(ACT_MOVE, 0);
            return;
        }
    }
    free_flick(dx, dy);
}

static void stick_flick(int which, unsigned char ax, unsigned char ay, int gameplay) {
    float x = (ax - 128) / 128.0f;
    float y = (ay - 128) / 128.0f;
    float mag = sqrtf(x * x + y * y);
    if (stick_armed[which] && mag > 0.6f) {
        stick_armed[which] = 0;
        do_flick(x / mag, y / mag, gameplay);
    } else if (mag < 0.3f) {
        stick_armed[which] = 1;
    }
}

// Tap/hold buttons. D-pad directions are taps too (gameplay only).
static const uint32_t tap_buttons[] = {
    SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
    SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
    SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT,
};
#define DPAD_MASK (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)

// Free Mode: D-pad up/down drive the song list like its on-screen arrows.
static int list_repeat = 0;

static void free_menu_scroll(uint32_t buttons, uint32_t pressed) {
    if (!scroll_set_add || !app_instance || !*app_instance)
        return;
    void *scroller = *(void **)((char *)*app_instance + APP_SCROLL_OFFSET);
    if (!scroller)
        return;
    int dir = 0;
    if (buttons & SCE_CTRL_UP) dir = 1;
    else if (buttons & SCE_CTRL_DOWN) dir = -1;
    if (!dir) {
        list_repeat = 0;
        return;
    }
    // first step at once, then auto-repeat while held
    if ((pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) || ++list_repeat >= 8) {
        if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
            list_repeat = 5;
        else
            list_repeat = 0;
        scroll_set_add(scroller, dir * 0x32);
        // Same as the on-screen arrows (SceneFreeMenuMain): when the list
        // has settled (state 1) the scroller is not updated any more, so
        // put it back in "moving" (2) and restart the settle counter.
        int *state = (int *)((char *)*app_instance + FREE_LIST_STATE_OFFSET);
        if (*state == 1) {
            *state = 2;
            *(int *)((char *)*app_instance + FREE_LIST_COUNTER_OFFSET) = 0;
        }
    }
}

/* ---- tutorial ---------------------------------------------------------------- */

// Tutorial windows: left/right turn pages, Cross goes on (PLAY on the last
// page of the first part), Circle closes the second part.
// Returns 1 when the buttons belong to the tutorial window.
static int tutorial_buttons(uint32_t pressed) {
    int win = tutorial_window();
    if (!win)
        return 0;
    int last = win == 1 ? 2 : 14;
    if (pressed & SCE_CTRL_LEFT)
        tutorial_turn_page(-1);
    if (pressed & SCE_CTRL_RIGHT)
        tutorial_turn_page(1);
    if (pressed & SCE_CTRL_CROSS) {
        if (tutorial_page() == last)
            tutorial_activate(last, win == 1 ? 3 : 15);
        else
            tutorial_turn_page(1);
    }
    if ((pressed & SCE_CTRL_CIRCLE) && win == 2)
        tutorial_activate(14, 15);
    return 1;
}

/* ---- polling ---------------------------------------------------------------- */

typedef struct { int key; float x, y; } Contact;

// Make every game slot's "previous position" the centre before the first
// scene, so the very first flicks start from a known place.
void input_prime(void) {
    for (int id = 0; id < GAME_SLOTS; id++)
        pointer_down_id(KEY_FLICK(0xF00 + id), id, center_x, center_y);
    while (n_active > 0)
        pointer_up(n_active - 1);
}

void input_poll(void) {
    tutorial_frame();
    int scene = current_scene();
    int gameplay = scene_is_gameplay(scene);

    // 1) Release last frame's flicks
    for (int i = n_active - 1; i >= 0; i--)
        if (active[i].release)
            pointer_up(i);

    // 2) Gather held contacts: touch reports and tap buttons
    Contact cur[MAX_POINTERS + 16];
    int n_cur = 0;

    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0) {
        for (int i = 0; i < (int)touch.reportNum && n_cur < MAX_POINTERS; i++) {
            float sx = touch.report[i].x * (float)ROF_SCREEN_W / 1920.0f;
            float sy = touch.report[i].y * (float)ROF_SCREEN_H / 1088.0f;
            cur[n_cur].key = KEY_TOUCH(touch.report[i].id);
            cur[n_cur].x = sx / view_scale;
            cur[n_cur].y = sy / view_scale;
            n_cur++;
        }
    }

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t pressed = pad.buttons & ~prev_buttons;
    uint32_t released = ~pad.buttons & prev_buttons;
    prev_buttons = pad.buttons;
    consumed_buttons &= pad.buttons; // forget buttons that were let go

    // Tutorial windows take the buttons; tip pop-ups in the practice song
    // also advance with Cross / Circle.
    int tutorial = tutorial_buttons(pressed);
    int tip = tutorial_popup();

    for (unsigned i = 0; !tutorial && i < sizeof(tap_buttons) / sizeof(tap_buttons[0]); i++) {
        uint32_t b = tap_buttons[i];
        if (!(pad.buttons & b) || (consumed_buttons & b))
            continue;
        if ((b & DPAD_MASK) && !gameplay)
            continue; // D-pad only taps in gameplay
        cur[n_cur].key = KEY_BUTTON(b);
        cur[n_cur].x = center_x;
        cur[n_cur].y = center_y;
        n_cur++;
    }

    // 3) Lifted pointers (flick-driven ones are released in step 1)
    for (int i = n_active - 1; i >= 0; i--) {
        if (active[i].release)
            continue;
        int still = 0;
        for (int j = 0; j < n_cur; j++)
            if (cur[j].key == active[i].key) { still = 1; break; }
        if (!still)
            pointer_up(i);
    }

    // 4) New pointers, then movement of touch pointers
    int moved = 0;
    for (int j = 0; j < n_cur; j++) {
        int idx = find_pointer(cur[j].key);
        if (idx < 0) {
            if (cur[j].key >= KEY_BUTTON(0))
                button_down(cur[j].key, gameplay,
                            tip && (cur[j].key == KEY_BUTTON(SCE_CTRL_CROSS) ||
                                    cur[j].key == KEY_BUTTON(SCE_CTRL_CIRCLE)));
            else
                pointer_down_id(cur[j].key, lowest_free_id(), cur[j].x, cur[j].y);
        } else if (cur[j].key < KEY_BUTTON(0) &&
                   (active[idx].x != cur[j].x || active[idx].y != cur[j].y)) {
            active[idx].x = cur[j].x;
            active[idx].y = cur[j].y;
            moved = 1;
        }
    }
    if (moved)
        send_event(ACT_MOVE, 0);

    // 5) Flicks from both analog sticks
    stick_flick(0, pad.lx, pad.ly, gameplay);
    stick_flick(1, pad.rx, pad.ry, gameplay);

    // 6) Free Mode list navigation
    if (scene_is_free_menu(scene))
        free_menu_scroll(pad.buttons, pressed);

    // 7) BACK key (Android sends it on key up)
    if (released & (SCE_CTRL_START | SCE_CTRL_SELECT))
        rof_native.SetKeyTrigger(rof_env, rof_clazz, ANDROID_KEY_BACK_TRIGGER);
}
