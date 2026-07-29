// trigger_utils.h - trigger processing, 8-bit throughout.
//
// Xbox 360 triggers are 8-bit (0..255) from source to wire: the XInput host
// report delivers p->bLeftTrigger/bRightTrigger as uint8_t, and the
// tusb_gamepad XInput output report sends lt/rt back out as uint8_t. There is
// no wider intermediate domain and no quantization step - process in 8-bit,
// commit in 8-bit.
#ifndef TRIGGER_UTILS_H
#define TRIGGER_UTILS_H
#include <stdint.h>

// Ceiling clamp - the UI's "trigger sensitivity limit".
// max_val = 0..255; values above the ceiling are hard-clamped.
static inline uint8_t apply_trigger_limit(uint8_t t, uint8_t max_val) {
    if (t > max_val) t = max_val;
    return t;
}

// Instant (digital-feel) trigger - maps any press past the threshold to 255,
// else 0. threshold is in the same 0..255 domain.
static inline uint8_t apply_trigger_instant(uint8_t t, uint8_t threshold) {
    return (t >= threshold) ? 255 : 0;
}

#endif // TRIGGER_UTILS_H
