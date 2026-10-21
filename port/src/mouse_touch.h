/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The mouse as a finger on the DualShock 4 touchpad (runtime_pad.c, "touchpad mode").
 *
 * While the mode is on, mouse motion moves a virtual finger over the touchpad (1920 x 942
 * units, the area the game reads) and the buttons are the finger:
 *   - nothing held: the finger hovers, the game sees no touch (like a lifted finger);
 *   - left held: the finger lands (and the pad is clicked, once the finger has been down
 *     MOUSE_TOUCH_LEAD_US): hold it still to hold the touchpad (absorb a power), a tap is a click;
 *   - dragging the finger more than `drag` units while the left is down turns it into a swipe:
 *     the click is released and the finger keeps sliding (Open Door, the hand scanner);
 *   - middle held: a finger without a click (a swipe from the first unit).
 * A touch comes MOUSE_TOUCH_LEAD_US before the click, the click lasts at least
 * MOUSE_TOUCH_HOLD_US and the finger stays MOUSE_TOUCH_TAIL_US after the release, the timing the
 * game needs to see a tap (same as runtime_pad.c's play_click).
 * Kept free of SDL and the runtime so tests/test_mouse_touch.c checks the conversions alone. */
#ifndef ISS_MOUSE_TOUCH_H
#define ISS_MOUSE_TOUCH_H
#include <math.h>
#include <stdint.h>

#define MOUSE_TOUCH_W 1919.0f
#define MOUSE_TOUCH_H 942.0f
#define MOUSE_TOUCH_LEAD_US 50000u
#define MOUSE_TOUCH_HOLD_US 50000u
#define MOUSE_TOUCH_TAIL_US 100000u
#define MOUSE_TOUCH_UNITS_PER_COUNT 1.6f /* at sensitivity 1: ~1200 counts (800 dpi, 1.5") cross the pad */

typedef struct {
    float sensitivity; /* 0.1 .. 10, times MOUSE_TOUCH_UNITS_PER_COUNT */
    float drag;        /* touchpad units of travel before a click becomes a swipe; 0: never */
} MouseTouchParams;

typedef struct {
    float x, y;            /* the finger, touchpad units (kept while the mode is off) */
    int held, with_click;  /* button state of the gesture; with_click: left (not middle) */
    int active;            /* a gesture is running, including its tail */
    int swiping;
    int id;                /* touch id of the current gesture, grows with each one */
    float start_x, start_y, last_x, last_y;
    uint64_t press_us, release_us;
} MouseTouch;

typedef struct {
    int touch;      /* the game sees a finger */
    int click;      /* and the touchpad button pressed */
    int id;
    float x, y;     /* where the finger is (also while hovering, for the on-screen marker) */
} MouseTouchOut;

static inline float mouse_touch_clamp(float v, float lo, float hi) { return v<lo ? lo : v>hi ? hi : v; }
static inline uint16_t mouse_touch_unit(float v, float max) { return (uint16_t)(mouse_touch_clamp(v,0.0f,max)+0.5f); }

/* The finger starts in the middle of the pad. */
static inline void mouse_touch_init(MouseTouch *t) {
    *t=(MouseTouch){0};
    t->x=MOUSE_TOUCH_W/2; t->y=MOUSE_TOUCH_H/2;
    t->id=29;
}

/* The mode was left or the mouse was released: the finger lifts at once. */
static inline void mouse_touch_lift(MouseTouch *t) {
    t->held=0; t->with_click=0; t->active=0; t->swiping=0;
}

/* One pad sample. dx, dy: mouse counts since the last one; left, middle: buttons held now;
 * now: microseconds. */
static inline MouseTouchOut mouse_touch_update(MouseTouch *t, const MouseTouchParams *p, float dx, float dy,
                                               int left, int middle, uint64_t now) {
    const float scale=MOUSE_TOUCH_UNITS_PER_COUNT*mouse_touch_clamp(p->sensitivity,0.1f,10.0f);
    t->x=mouse_touch_clamp(t->x+dx*scale,0.0f,MOUSE_TOUCH_W);
    t->y=mouse_touch_clamp(t->y+dy*scale,0.0f,MOUSE_TOUCH_H);
    const int held=left || middle;
    if (held && !t->held) { /* the finger lands (or lands again during the tail of the last gesture) */
        t->id=30+(t->id-29)%90;
        t->active=1; t->swiping=0; t->release_us=0;
        t->press_us=now; t->start_x=t->x; t->start_y=t->y;
        t->with_click=left!=0;
    } else if (!held && t->held) {
        t->release_us=now;
        t->last_x=t->x; t->last_y=t->y;
    }
    t->held=held;
    if (held && !t->swiping && p->drag>0.0f && hypotf(t->x-t->start_x,t->y-t->start_y)>p->drag) t->swiping=1;
    MouseTouchOut out={0,0,t->id,t->x,t->y};
    if (!t->active) return out;
    const uint64_t click_start=t->press_us+MOUSE_TOUCH_LEAD_US;
    const int clickable=t->with_click && !t->swiping;
    /* A click that started stays at least HOLD, also when the button came up sooner. */
    const uint64_t click_end=held ? UINT64_MAX : (t->release_us>click_start+MOUSE_TOUCH_HOLD_US ? t->release_us : click_start+MOUSE_TOUCH_HOLD_US);
    uint64_t touch_end=UINT64_MAX;
    if (!held) {
        const uint64_t last=clickable && click_end>t->release_us ? click_end : t->release_us;
        touch_end=last+MOUSE_TOUCH_TAIL_US;
    }
    if (now>=touch_end) { t->active=0; return out; }
    out.touch=1;
    if (!held) { out.x=t->last_x; out.y=t->last_y; }
    out.click=clickable && now>=click_start && now<click_end;
    return out;
}

#endif
