// mono_clock.h - monotonic millisecond clock + a cheap deterministic PRNG.
//
// WHY THIS EXISTS
// The macro engine ported from ReflexX (a Windows C# middleware) leans on two
// host facilities that simply do not exist on an MCU:
//
//   1. MonotonicClock.NowMs  - a QPC-backed, epoch-offset monotonic clock. Every
//      macro's timing state (LastFireTick, PulseUntilTick, SlideStartTick, ...)
//      uses 0 as the sentinel for "never fired". On Windows the clock is offset
//      by Environment.TickCount64 precisely so a freshly-zeroed field is NOT
//      mistaken for "fired just now". On the RP2350 the same hazard exists in a
//      worse form: to_ms_since_boot() genuinely starts at 0, so during the first
//      few hundred ms after power-on a zeroed timestamp really does look recent.
//      We therefore bias by MONO_EPOCH_MS so (now - 0) is always a huge number
//      and every "never fired" check behaves exactly as the C# original did.
//
//   2. Random.Shared / Box-Muller gaussian - used for anti-detection jitter
//      (NoRecoil randomisation, AimAssistBuff wiggle jitter, CrowBar noise,
//      ProgressiveRecoil noise, AimSnap humanisation, script wait jitter).
//      A libc rand() would work but is not reentrant and is needlessly slow;
//      xorshift32 is 3 instructions and plenty for "make it not metronomic".
//      Seeded once from the hardware RNG so two boards do not share a jitter
//      pattern - that pattern being predictable is exactly what the jitter is
//      meant to defeat.
//
// TRADEOFF NOTE (applies to the whole port): the C# source computes in `double`.
// The RP2350's Cortex-M33 has a single-precision FPU only - `double` is software
// emulated and roughly 40x slower, which we cannot afford in a 1 kHz input path.
// Every `double` in the source became `float` here. At stick-unit granularity
// (1/32767 of full deflection) float carries ~7 significant digits, three orders
// of magnitude more resolution than the wire format can express, so the
// substitution is not observable in the output.
#ifndef MONO_CLOCK_H
#define MONO_CLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/rand.h"

// Bias applied to every timestamp. One hour of milliseconds: large enough that
// (now - 0) can never be read as a recent event, small enough that int64 ms
// arithmetic stays trivially in range.
#define MONO_EPOCH_MS 3600000LL

// Monotonic milliseconds, same semantics as MonotonicClock.NowMs.
static inline int64_t now_ms(void)
{
    return MONO_EPOCH_MS + (int64_t)to_ms_since_boot(get_absolute_time());
}

// Monotonic microseconds - used where the source read Stopwatch ticks directly
// (AimSnap's impulse/cooldown windows) and needed sub-ms resolution.
static inline int64_t now_us(void)
{
    return MONO_EPOCH_MS * 1000LL + (int64_t)to_us_since_boot(get_absolute_time());
}

// ── xorshift32 ───────────────────────────────────────────────────────────────
// State is deliberately file-local-per-TU-free (a single shared global) because
// the macro engine only ever runs from one core (core 1, inside the XInput
// report callback). No locking needed; if that ever changes the only symptom is
// a slightly less random jitter stream, never a crash.
extern uint32_t g_mono_prng_state;

static inline void mono_prng_seed(void)
{
    uint32_t s = get_rand_32();
    g_mono_prng_state = s ? s : 0xA5A5A5A5u; // xorshift dies on a zero seed
}

static inline uint32_t mono_rand_u32(void)
{
    uint32_t x = g_mono_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_mono_prng_state = x;
    return x;
}

// Uniform [0, 1). 24-bit mantissa's worth of resolution, which is all a float
// can hold anyway.
static inline float mono_rand_f01(void)
{
    return (float)(mono_rand_u32() >> 8) * (1.0f / 16777216.0f);
}

// Uniform [-1, 1] - the shape most of the ported jitter code wants.
static inline float mono_rand_sym(void)
{
    return mono_rand_f01() * 2.0f - 1.0f;
}

// Uniform integer in [lo, hi] inclusive. Mirrors Random.Next(lo, hi+1).
static inline int mono_rand_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    uint32_t span = (uint32_t)(hi - lo) + 1u;
    return lo + (int)(mono_rand_u32() % span);
}

#endif // MONO_CLOCK_H
