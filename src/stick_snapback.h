// stick_correction.h - right-stick correction for tusb_gamepad_xbox_to_xinput_pico
//
// Anti-snapback: ported from DS4Windows (Mapping.cs: CalcAntiSnapbackStick /
// ProfilePropGroups.cs: StickAntiSnapbackInfo). Keeps a short time-windowed
// history of stick samples. For each new sample, checks whether the line
// segment from any still-fresh historical point to the current point passes
// close to center AND the two points are far enough apart. If so, the stick
// physically snapped back through center on release/flick - the sample is
// forced to center to suppress that transient.
#pragma once
#include <stdint.h>
#include "bsp/board_api.h"   // board_millis()

// 1 = CORRECTION LIVE, 0 = no-op passthrough
#define ENABLE_RIGHT_STICK_ANTISNAPBACK 1

// Stick center, in raw int16 XInput units.
#define ASB_CENTER_X (976)
#define RSC_CENTER_Y (-128)

#define ASB_CENTER_Y 0

// Minimum travel distance (int16 units) between the historical point and the
// current point before a crossing is even considered. DS4Windows default is
// 135 out of a 0..255 axis (full range ~127 radius) -> ~53% of full radius.
#define ASB_DELTA_SQ ((float)(17270.0 * 17270.0))

// Radius (int16 units) of the "near center" zone a crossing must pass through.
// DS4Windows default is 15 out of the same 0..255 axis.
#define ASB_RADIUS_SQ ((float)(3840.0 * 3840.0))

// History window: only samples within this many ms of "now" are considered.
// DS4Windows default timeout is 50 ms.
#define ASB_TIMEOUT_MS 50u

// Ring buffer capacity - must comfortably hold every sample seen within
// ASB_TIMEOUT_MS at the firmware's report rate (~850Hz -> ~30 samples/50ms).
#define ASB_HISTORY_LEN 64

typedef struct {
    int16_t  x, y;
    uint32_t timestamp_ms;
} asb_sample_t;

typedef struct {
    asb_sample_t buf[ASB_HISTORY_LEN];
    int head;   // index to write the next sample into
    int count;  // number of valid samples currently buffered
} asb_history_t;

// Drop samples older than ASB_TIMEOUT_MS relative to now_ms.
static inline void asb_history_prune(asb_history_t *h, uint32_t now_ms){
    while (h->count > 0){
        int oldest_idx = (h->head - h->count + ASB_HISTORY_LEN) % ASB_HISTORY_LEN;
        if (now_ms - h->buf[oldest_idx].timestamp_ms <= ASB_TIMEOUT_MS) break;
        h->count--;
    }
}

static inline void asb_history_push(asb_history_t *h, int16_t x, int16_t y, uint32_t now_ms){
    h->buf[h->head].x = x;
    h->buf[h->head].y = y;
    h->buf[h->head].timestamp_ms = now_ms;
    h->head = (h->head + 1) % ASB_HISTORY_LEN;
    if (h->count < ASB_HISTORY_LEN) h->count++;
}

// True if segment (ox,oy)->(x,y) is long enough (>= ASB_DELTA) and passes
// within ASB_RADIUS of center.
static inline int asb_segment_crosses_center(int16_t x, int16_t y, int16_t ox, int16_t oy){
    float dx = (float)(x - ox);
    float dy = (float)(y - oy);
    float distanceSquared = dx * dx + dy * dy;
    if (distanceSquared < ASB_DELTA_SQ) return 0;

    float t = (((float)ASB_CENTER_X - x) * (ox - x) + ((float)ASB_CENTER_Y - y) * (oy - y)) / distanceSquared;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float closestX = x + t * (ox - x);
    float closestY = y + t * (oy - y);
    float toCenterX = (float)ASB_CENTER_X - closestX;
    float toCenterY = (float)ASB_CENTER_Y - closestY;
    float distToCenterSquared = toCenterX * toCenterX + toCenterY * toCenterY;

    return distToCenterSquared <= ASB_RADIUS_SQ;
}

// Correct ONE right-stick sample in place via anti-snapback detection.
static inline void correct_right_stick(int16_t *px, int16_t *py){
#if ENABLE_RIGHT_STICK_ANTISNAPBACK
    static asb_history_t history = {0};

    uint32_t now_ms = board_millis();
    asb_history_prune(&history, now_ms);

    int snapped = 0;
    for (int i = 0; i < history.count; i++){
        int idx = (history.head - history.count + i + ASB_HISTORY_LEN) % ASB_HISTORY_LEN;
        asb_sample_t *old = &history.buf[idx];
        if (asb_segment_crosses_center(*px, *py, old->x, old->y)){
            snapped = 1;
            break;
        }
    }

    asb_history_push(&history, *px, *py, now_ms);

    if (snapped){
        *px = ASB_CENTER_X;
        *py = ASB_CENTER_Y;
    }
#else
    (void)px; (void)py;
#endif
}
