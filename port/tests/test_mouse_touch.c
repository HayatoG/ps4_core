/* The mouse as a finger on the touchpad (src/mouse_touch.h): motion, tap, hold, drag/swipe,
 * middle button, ids, edges. Time is passed in, so nothing sleeps. */
#include <assert.h>
#include <stdio.h>
#include "../src/mouse_touch.h"

#define MS(n) ((uint64_t)(n)*1000u)

static MouseTouch fresh(void) { MouseTouch t; mouse_touch_init(&t); return t; }
static int near(float a, float b) { return fabsf(a-b)<0.01f; }

int main(void) {
    const MouseTouchParams p={1.0f,80.0f};
    uint64_t now=1000000;

    /* The finger starts in the middle; motion moves it 1.6 units per count; no button, no touch. */
    MouseTouch t=fresh();
    MouseTouchOut o=mouse_touch_update(&t,&p,0,0,0,0,now);
    assert(near(o.x,959.5f) && near(o.y,471.0f) && !o.touch && !o.click);
    o=mouse_touch_update(&t,&p,100,-50,0,0,now+=MS(10));
    assert(near(o.x,959.5f+160.0f) && near(o.y,471.0f-80.0f) && !o.touch);
    const MouseTouchParams fast={2.5f,80.0f};
    MouseTouch t2=fresh();
    o=mouse_touch_update(&t2,&fast,10,0,0,0,now);
    assert(near(o.x,959.5f+40.0f));
    /* The pad is bounded: 0..1919 x 0..942. */
    o=mouse_touch_update(&t,&p,100000,100000,0,0,now+=MS(10));
    assert(near(o.x,MOUSE_TOUCH_W) && near(o.y,MOUSE_TOUCH_H));
    o=mouse_touch_update(&t,&p,-100000,-100000,0,0,now+=MS(10));
    assert(near(o.x,0) && near(o.y,0));
    assert(mouse_touch_unit(0.4f,MOUSE_TOUCH_W)==0 && mouse_touch_unit(1919.0f,MOUSE_TOUCH_W)==1919 && mouse_touch_unit(2500,MOUSE_TOUCH_W)==1919);

    /* Hold the left button still: the finger lands at once, the click LEAD later, and both stay. */
    t=fresh(); now=5000000;
    o=mouse_touch_update(&t,&p,0,0,1,0,now);
    assert(o.touch && !o.click);
    const int id=o.id;
    o=mouse_touch_update(&t,&p,2,1,1,0,now+MS(30)); /* mouse jitter stays a click */
    assert(o.touch && !o.click && o.id==id);
    o=mouse_touch_update(&t,&p,0,0,1,0,now+MS(55));
    assert(o.touch && o.click);
    o=mouse_touch_update(&t,&p,0,0,1,0,now+MS(1500));
    assert(o.touch && o.click && o.id==id);
    /* Released: the click ends, the finger lingers TAIL, then it lifts. */
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(1600));
    assert(o.touch && !o.click); /* a long press ends with the button */
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(1600)+MOUSE_TOUCH_TAIL_US+MS(1));
    assert(!o.touch && !o.click);

    /* A tap shorter than the lead still clicks for HOLD, with the finger around it. */
    t=fresh(); now=9000000;
    o=mouse_touch_update(&t,&p,0,0,1,0,now);
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(20)); /* up after 20 ms */
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(60));
    assert(o.touch && o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(99));
    assert(o.touch && o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(101));
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(100)+MOUSE_TOUCH_TAIL_US+MS(2));
    assert(!o.touch && !o.click);
    /* The finger stays where the tap was while it lingers, whatever the mouse does. */
    t=fresh(); now=12000000;
    o=mouse_touch_update(&t,&p,0,0,1,0,now);
    const float tap_x=o.x;
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(20));
    o=mouse_touch_update(&t,&p,300,0,0,0,now+MS(40));
    assert(o.touch && near(o.x,tap_x));

    /* Drag with the left button: past `drag` units the click is released, the finger slides. */
    t=fresh(); now=20000000;
    o=mouse_touch_update(&t,&p,0,0,1,0,now);
    const float start_x=o.x;
    o=mouse_touch_update(&t,&p,20,0,1,0,now+MS(20));  /* 32 units: still under 80 */
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,20,0,1,0,now+MS(60));  /* 64 units, click time */
    assert(o.touch && o.click);
    o=mouse_touch_update(&t,&p,30,0,1,0,now+MS(80));  /* 112 units: a swipe */
    assert(o.touch && !o.click && near(o.x,start_x+112.0f));
    const int swipe_id=o.id;
    o=mouse_touch_update(&t,&p,-10,0,1,0,now+MS(100)); /* coming back does not click again */
    assert(o.touch && !o.click && o.id==swipe_id);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(120));
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(120)+MOUSE_TOUCH_TAIL_US+MS(1));
    assert(!o.touch);

    /* A swipe that starts before the click (fast) never clicks. */
    t=fresh(); now=30000000;
    o=mouse_touch_update(&t,&p,0,0,1,0,now);
    o=mouse_touch_update(&t,&p,100,0,1,0,now+MS(10));
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,1,0,now+MS(200));
    assert(o.touch && !o.click);

    /* drag = 0: never a swipe, always the click. */
    const MouseTouchParams click_only={1.0f,0.0f};
    t=fresh(); now=40000000;
    o=mouse_touch_update(&t,&click_only,0,0,1,0,now);
    o=mouse_touch_update(&t,&click_only,500,0,1,0,now+MS(100));
    assert(o.touch && o.click);

    /* Middle: a finger with no click, even held still. */
    t=fresh(); now=50000000;
    o=mouse_touch_update(&t,&p,0,0,0,1,now);
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,1,now+MS(500));
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(510));
    assert(o.touch && !o.click);
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(510)+MOUSE_TOUCH_TAIL_US+MS(1));
    assert(!o.touch);

    /* Each landing is a new touch id, also during the tail of the last one. */
    t=fresh(); now=60000000;
    o=mouse_touch_update(&t,&p,0,0,1,0,now);
    const int first=o.id;
    o=mouse_touch_update(&t,&p,0,0,0,0,now+MS(200));
    o=mouse_touch_update(&t,&p,0,0,1,0,now+MS(250));
    assert(o.touch && o.id!=first);
    for (int i=0;i<300;++i) { /* ids stay small and never repeat back to back */
        const int before=o.id;
        now+=MS(1000);
        o=mouse_touch_update(&t,&p,0,0,0,0,now);
        o=mouse_touch_update(&t,&p,0,0,1,0,now+MS(500));
        assert(o.id!=before && o.id>=30 && o.id<120);
    }

    /* Lifting (mode left, mouse released) ends everything at once but keeps the position. */
    t=fresh();
    o=mouse_touch_update(&t,&p,100,0,1,0,70000000);
    mouse_touch_lift(&t);
    o=mouse_touch_update(&t,&p,0,0,0,0,70000000+MS(10));
    assert(!o.touch && !o.click && near(o.x,959.5f+160.0f));
    puts("PASS: mouse touch");
    return 0;
}
