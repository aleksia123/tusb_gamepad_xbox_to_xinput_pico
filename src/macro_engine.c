// macro_engine.c - the port of ReflexX.Application.MacroEngine.MacroProcessor
// (2408 lines of C#) to RP2350 firmware.
//
// ═══ HOW TO READ THIS FILE AGAINST THE SOURCE ════════════════════════════════
// The structure is 1:1 with MacroProcessor.cs: the same shared helpers
// (trigger-source resolution, fire-input drivers, toggle resolution), then one
// static function per MacroType in the same order as the C# dispatch switch,
// then the dispatch itself. Every handler keeps the source's gating order,
// timing constants and arithmetic. Where behaviour had to change, the reason is
// commented inline. The four systemic changes are:
//
//  1. double -> float. Cortex-M33 has a single-precision FPU; double is software
//     emulated. See the tradeoff note in mono_clock.h. Output resolution is
//     unaffected (everything lands in int16 stick units / uint8 trigger units).
//
//  2. GamepadState is mutated in place instead of being cloned and returned.
//     The C# `state = ProcessX(state, macro)` chain exists because GamepadState
//     is a class and the pipeline wanted value semantics per stage; on an MCU a
//     clone per macro per tick is pure waste. Composition order is identical -
//     each handler still sees the accumulated effect of the higher-priority
//     macros, which is what makes additive stacking (NoRecoil + AimSmooth +
//     TrackingAssist all writing RS) behave the same.
//
//  3. `Xxx?` nullable buttons become a GP_NONE (0) sentinel; `HasValue` becomes
//     `!= GP_NONE`. This is exact: GamepadButton.None is 0 in the source and
//     every nullable-button call site treats null as "not configured".
//
//  4. No _weaponProfile. In ReflexX that object comes from
//     TemplateWeaponDetectionService, which OpenCV-template-matches the weapon
//     name off the game's HUD. There is no screen to capture here, so
//     _weaponProfile is permanently null and every `if (_weaponProfile != null)`
//     branch resolves to its else. The macro's own RecoilCompensationX/Y,
//     IntervalMs and Intensity are used instead - which is precisely the
//     source's behaviour with weapon detection disabled, i.e. its default.
//
// Also absent: the ILogger calls (no log sink in the input path; a printf here
// would add hundreds of microseconds inside a 1 kHz callback), the FeatureGate
// licence check (no licensing on-device - see PORTING.md), and the
// MacroToggleChanged event (its only consumer was the WebView2 overlay toast).
#include <math.h>
#include <string.h>

#include "macro_engine.h"
#include "mono_clock.h"
#include "motion_sampler.h"
#include "aim_context.h"

// Single shared PRNG state, defined here because this is the only consumer.
uint32_t g_mono_prng_state = 0x12345678u;

#define STICK_FULLf 32767.0f

// ── Bound profile + runtime state ────────────────────────────────────────────
static const macro_definition_t *s_macros;
static int  s_macro_count;
static int  s_order[MACROS_PER_PROFILE]; // slot indices, ascending priority
static int  s_order_count;
static int  s_enabled_count;
static macro_runtime_t s_runtimes[MACROS_PER_PROFILE];

// Fields whose "unset" value is not zero. The C# MacroRuntime declares these
// with initialisers (ManualLevel = Medium, AimSmoothLastLockId = -1, ...), so a
// plain memset would silently change behaviour: lock id 0 would collide with a
// real lock, and the manual head-assist level would start at Short.
static void runtime_init(macro_runtime_t *r)
{
    memset(r, 0, sizeof(*r));
    r->manual_level              = DIST_MEDIUM;
    r->current_head_assist_level = DIST_MEDIUM;
    r->aim_smooth_last_lock_id    = -1;
    r->aim_smooth_engaged_lock_id = -1;
}

void macro_engine_load(const macro_definition_t *macros, int count)
{
    s_macros      = macros;
    s_macro_count = (count < 0) ? 0 : (count > MACROS_PER_PROFILE ? MACROS_PER_PROFILE : count);

    for (int i = 0; i < MACROS_PER_PROFILE; i++)
        runtime_init(&s_runtimes[i]);

    // Cache the priority order once per profile load rather than sorting every
    // tick. Insertion sort: n <= 16 and it is stable, so macros with equal
    // priority keep their editor order - which users rely on when two macros
    // write the same stick.
    s_order_count   = 0;
    s_enabled_count = 0;
    if (!macros) return;

    for (int i = 0; i < s_macro_count; i++) {
        if (!macros[i].enabled) continue;
        if (macros[i].type >= MACRO_TYPE_COUNT) continue; // corrupt slot: ignore
        int32_t p = macros[i].priority;
        int j = s_order_count++;
        while (j > 0 && macros[s_order[j - 1]].priority > p) {
            s_order[j] = s_order[j - 1];
            j--;
        }
        s_order[j] = i;
        s_enabled_count++;
    }
}

int macro_engine_active_count(void) { return s_enabled_count; }

// ═════════════════════════════════════════════════════════════════════════════
//  Shared helpers (MacroProcessor's private statics)
// ═════════════════════════════════════════════════════════════════════════════

// TriggerSourceHeld. Returns -1 for TriggerSource.None so each caller can apply
// its own default via the `?? default` idiom the source uses - the defaults
// differ per macro (some fall back to RT, some to LT, some to "always on") and
// collapsing them would change several macros' behaviour.
static int trigger_src_held(const macro_gamepad_state_t *s, uint8_t src)
{
    switch (src) {
        case TRIG_SRC_LEFT_TRIGGER:   return gs_lt_pressed(s) ? 1 : 0;
        case TRIG_SRC_RIGHT_TRIGGER:  return gs_rt_pressed(s) ? 1 : 0;
        case TRIG_SRC_LEFT_SHOULDER:  return gs_pressed(s, GP_LEFT_SHOULDER) ? 1 : 0;
        case TRIG_SRC_RIGHT_SHOULDER: return gs_pressed(s, GP_RIGHT_SHOULDER) ? 1 : 0;
        case TRIG_SRC_DUAL_TRIGGER:   return (gs_lt_pressed(s) || gs_rt_pressed(s)) ? 1 : 0;
        case TRIG_SRC_DUAL_SHOULDER:  return (gs_pressed(s, GP_LEFT_SHOULDER) ||
                                              gs_pressed(s, GP_RIGHT_SHOULDER)) ? 1 : 0;
        default: return -1; // TriggerSource.None
    }
}

// EffectiveMagneticStrength - in the dual sources the fire trigger (RT/RB) gets
// its own strength and wins when both are held, because that is the shooting
// moment. Single sources keep the aim strength for backward compatibility.
static float effective_magnetic_strength(const macro_gamepad_state_t *s,
                                         const macro_definition_t *m,
                                         const aim_assist_config_t *cfg)
{
    switch (m->trigger_source) {
        case TRIG_SRC_DUAL_TRIGGER:
            return gs_rt_pressed(s) ? cfg->magnetic_strength_fire : cfg->magnetic_strength;
        case TRIG_SRC_DUAL_SHOULDER:
            return gs_pressed(s, GP_RIGHT_SHOULDER) ? cfg->magnetic_strength_fire
                                                    : cfg->magnetic_strength;
        default:
            return cfg->magnetic_strength;
    }
}

// SetFireInput - drives the configured fire channel. Trigger sources write the
// analog axis; shoulder sources press the bumper, so RB/LB shooters get
// rapid-fire on the button they actually use. None defaults to RT.
static void set_fire_input(macro_gamepad_state_t *s, uint8_t src, bool pressed)
{
    switch (src) {
        case TRIG_SRC_LEFT_TRIGGER:   s->lt = pressed ? 255 : 0; break;
        case TRIG_SRC_LEFT_SHOULDER:  gs_set_button(s, GP_LEFT_SHOULDER,  pressed); break;
        case TRIG_SRC_RIGHT_SHOULDER: gs_set_button(s, GP_RIGHT_SHOULDER, pressed); break;
        case TRIG_SRC_RIGHT_TRIGGER:
        default:                      s->rt = pressed ? 255 : 0; break;
    }
}

// RaiseFireInput - additive: only ever raises the fire input, never lowers it
// below what the player or a higher-priority macro already wrote. TriggerBot's
// composition rule.
static void raise_fire_input(macro_gamepad_state_t *s, uint8_t src)
{
    switch (src) {
        case TRIG_SRC_LEFT_TRIGGER:   if (s->lt < 255) s->lt = 255; break;
        case TRIG_SRC_LEFT_SHOULDER:  gs_set_button(s, GP_LEFT_SHOULDER,  true); break;
        case TRIG_SRC_RIGHT_SHOULDER: gs_set_button(s, GP_RIGHT_SHOULDER, true); break;
        case TRIG_SRC_RIGHT_TRIGGER:
        default:                      if (s->rt < 255) s->rt = 255; break;
    }
}

// ResolveToggleActivation - press-edge flips a latch. The C# version also raises
// MacroToggleChanged for the UI toast; there is no overlay here so the event is
// dropped, the latch semantics are identical.
static bool resolve_toggle_activation(const macro_gamepad_state_t *s,
                                      const macro_definition_t *m,
                                      macro_runtime_t *rt)
{
    bool pressed = gs_pressed(s, m->activation_button);
    if (pressed && !rt->was_pressed)
        rt->toggle_state = !rt->toggle_state;
    rt->was_pressed = pressed;
    return rt->toggle_state;
}

// The plain hold/toggle activation gate shared by NoRecoil, AdaptiveRecoil,
// ProgressiveRecoil, AutoSprint and HoldBreath. No trigger component.
static bool button_activation(const macro_gamepad_state_t *s,
                              const macro_definition_t *m,
                              macro_runtime_t *rt)
{
    if (m->toggle_mode && m->activation_button != GP_NONE)
        return resolve_toggle_activation(s, m, rt);
    return m->activation_button == GP_NONE || gs_pressed(s, m->activation_button);
}

// IsMacroActive - trigger gate AND button gate. Trigger-bound activation takes
// precedence when TriggerSource names an axis or bumper; None -> not gated.
static bool is_macro_active(const macro_gamepad_state_t *s,
                            const macro_definition_t *m,
                            macro_runtime_t *rt)
{
    int t = trigger_src_held(s, m->trigger_source);
    bool trigger_active = (t < 0) ? true : (t != 0);
    return trigger_active && button_activation(s, m, rt);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ApplyPhaseEasing - note this is NOT the same table as MotionSampler's Ease():
// ProgressiveRecoil only supports four kinds and defaults unknown values to
// smoothstep rather than linear. Kept separate to preserve that difference.
static float apply_phase_easing(uint8_t kind, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    switch (kind) {
        case EASE_SMOOTHSTEP:    return t * t * (3.0f - 2.0f * t);
        case EASE_IN_OUT_SINE:   return 0.5f - 0.5f * cosf(M_PIf * t);
        case EASE_OUT_CUBIC:     return 1.0f - powf(1.0f - t, 3.0f);
        case EASE_LINEAR:        return t;
        default:                 return t * t * (3.0f - 2.0f * t);
    }
}

// Additive-or-override write of a motion sample to the target stick.
static void apply_motion_sample(macro_gamepad_state_t *s, const motion_script_t *motion,
                                motion_sample_t sample, float macro_intensity)
{
    float scale = macro_intensity * STICK_FULLf;
    int16_t dx = clamp_s16((int32_t)(sample.x * scale));
    int16_t dy = clamp_s16((int32_t)(sample.y * scale));

    if (motion->target == STICK_LEFT) {
        if (motion->additive) {
            s->lx = clamp_s16((int32_t)s->lx + dx);
            s->ly = clamp_s16((int32_t)s->ly + dy);
        } else {
            s->lx = dx; s->ly = dy;
        }
    } else {
        if (motion->additive) {
            s->rx = clamp_s16((int32_t)s->rx + dx);
            s->ry = clamp_s16((int32_t)s->ry + dy);
        } else {
            s->rx = dx; s->ry = dy;
        }
    }
}

// Box-Muller, single-precision. Replaces SampleGaussian(double sigma); the
// distribution is what matters here (AimSnap humanisation), not the last bit.
static int gaussian_int(float sigma)
{
    float u1 = 1.0f - mono_rand_f01(); // (0, 1] - logf(0) would be -inf
    float u2 = mono_rand_f01();
    float z  = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * M_PIf * u2);
    return (int)(z * sigma);
}

// ═════════════════════════════════════════════════════════════════════════════
//  NoRecoil
// ═════════════════════════════════════════════════════════════════════════════
static void process_no_recoil(macro_gamepad_state_t *s, const macro_definition_t *m,
                              macro_runtime_t *rt)
{
    int t = trigger_src_held(s, m->trigger_source);
    bool is_shooting = (t < 0) ? gs_rt_pressed(s) : (t != 0);

    if (!(button_activation(s, m, rt) && is_shooting)) return;

    // No weapon profile on this hardware (see file header note 4) -> the macro's
    // own compensation values, scaled by intensity.
    int32_t comp_x = (int32_t)((float)m->recoil_compensation_x * m->intensity);
    int32_t comp_y = (int32_t)((float)m->recoil_compensation_y * m->intensity);

    if (m->randomization_factor > 0.0f) {
        int range = (int)(m->randomization_factor * 100.0f);
        comp_x += mono_rand_range(-range, range);
        comp_y += mono_rand_range(-range, range);
    }

    // Additive, not replacing - the player keeps authority over the stick.
    s->rx = clamp_s16((int32_t)s->rx + comp_x);
    s->ry = clamp_s16((int32_t)s->ry + comp_y);
}

// ═════════════════════════════════════════════════════════════════════════════
//  AdaptiveRecoil (AI-distance-scaled) - degrades to Min compensation
// ═════════════════════════════════════════════════════════════════════════════
// This is the source's own graceful-degradation path, and the precedent the rest
// of the Tier-3 stubs follow: with no valid AI lock distFactor stays 0, which
// selects the "far" (minimum) compensation, so recoil control never drops to
// zero mid-fight just because vision is unavailable. On this hardware
// aim_context_try_acquire always reports no target, so AdaptiveRecoil behaves as
// a fixed NoRecoil at the Min values - useful, honest, and exactly what the C#
// code does with AI Vision switched off.
static void process_adaptive_recoil(macro_gamepad_state_t *s, const macro_definition_t *m,
                                    macro_runtime_t *rt)
{
    int t = trigger_src_held(s, m->trigger_source);
    bool is_shooting = (t < 0) ? gs_rt_pressed(s) : (t != 0);

    if (!button_activation(s, m, rt) || !is_shooting) return;

    const adaptive_recoil_config_t *cfg = &m->adaptive_recoil;

    float dist_factor = 0.0f; // 0 = far -> Min comp
    aim_target_t target;
    if (aim_context_try_acquire(&m->aim_assist, &target)) {
        int fov = target.fov_ring_pixels > 0 ? target.fov_ring_pixels : 200;
        float bbox_max = fmaxf(target.width, target.height);
        dist_factor = clampf(bbox_max / fmaxf(1.0f, (float)fov), 0.0f, 1.0f);
    }

    float lx = (float)cfg->min_compensation_x +
               (float)(cfg->max_compensation_x - cfg->min_compensation_x) * dist_factor;
    float ly = (float)cfg->min_compensation_y +
               (float)(cfg->max_compensation_y - cfg->min_compensation_y) * dist_factor;

    int32_t comp_x = (int32_t)(lx * cfg->intensity);
    int32_t comp_y = (int32_t)(ly * cfg->intensity);

    if (cfg->randomization_factor > 0.0f) {
        int range = (int)(cfg->randomization_factor * 100.0f);
        comp_x += mono_rand_range(-range, range);
        comp_y += mono_rand_range(-range, range);
    }

    s->rx = clamp_s16((int32_t)s->rx + comp_x);
    s->ry = clamp_s16((int32_t)s->ry + comp_y);
}

// ═════════════════════════════════════════════════════════════════════════════
//  AutoFire
// ═════════════════════════════════════════════════════════════════════════════
static void process_auto_fire(macro_gamepad_state_t *s, const macro_definition_t *m,
                              macro_runtime_t *rt)
{
    // Where the toggle reads its gate and writes its output:
    //   LT/RT -> analog axis, LB/RB -> bumper, None -> ActivationButton (RB
    //   default) treated as a plain button.
    int t = trigger_src_held(s, m->trigger_source);
    bool held;
    bool fire_button_mode = false;
    uint32_t fire_button = m->activation_button != GP_NONE ? m->activation_button
                                                          : GP_RIGHT_SHOULDER;
    if (t >= 0) {
        held = (t != 0);
    } else {
        fire_button_mode = true;
        held = gs_pressed(s, fire_button);
    }

    int32_t interval_ms = m->interval_ms; // no weapon profile override on-device

    if (held) {
        int64_t now = now_ms();
        if (now - rt->last_fire_tick >= interval_ms) {
            rt->toggle_state = !rt->toggle_state;
            rt->last_fire_tick = now;
        }
        if (fire_button_mode) gs_set_button(s, fire_button, rt->toggle_state);
        else                  set_fire_input(s, m->trigger_source, rt->toggle_state);
    } else {
        rt->toggle_state = false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  AutoPing
// ═════════════════════════════════════════════════════════════════════════════
static void process_auto_ping(macro_gamepad_state_t *s, const macro_definition_t *m,
                              macro_runtime_t *rt)
{
    uint32_t ping_button = m->ping_button != GP_NONE ? m->ping_button : GP_DPAD_UP;
    // AutoPing is hard-wired to RT in the source (it is a "ping what I'm
    // shooting at" macro), so TriggerSource is deliberately not consulted.
    bool held = gs_rt_pressed(s) &&
                (m->activation_button == GP_NONE || gs_pressed(s, m->activation_button));

    if (held) {
        int64_t now = now_ms();
        int32_t interval = m->interval_ms > 0 ? m->interval_ms : 200;
        if (now >= rt->pulse_until_tick && now - rt->last_fire_tick >= interval) {
            gs_set_button(s, ping_button, true);
            rt->last_fire_tick   = now;
            rt->pulse_until_tick = now + 50; // 50 ms pulse
        } else if (now < rt->pulse_until_tick) {
            gs_set_button(s, ping_button, true);
        }
    } else {
        // Cut an in-flight pulse, but never stomp a manual press of the button.
        if (rt->pulse_until_tick > 0 && !gs_pressed(s, ping_button))
            gs_set_button(s, ping_button, false);
        rt->pulse_until_tick = 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Remap
// ═════════════════════════════════════════════════════════════════════════════
static void process_remap(macro_gamepad_state_t *s, const macro_definition_t *m)
{
    if (m->source_button != GP_NONE && m->target_button != GP_NONE) {
        bool pressed = gs_pressed(s, m->source_button);
        gs_set_button(s, m->source_button, false);
        gs_set_button(s, m->target_button, pressed);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Sequence
// ═════════════════════════════════════════════════════════════════════════════
static void process_sequence(macro_gamepad_state_t *s, const macro_definition_t *m,
                             macro_runtime_t *rt)
{
    uint32_t trigger = m->activation_button != GP_NONE ? m->activation_button : GP_A;
    bool held = gs_pressed(s, trigger);
    int count = m->step_count;
    if (count > MACRO_MAX_SEQ_STEPS) count = MACRO_MAX_SEQ_STEPS;

    if (held && count > 0) {
        // Clamp: the step list can shrink under us when the configurator writes
        // a new profile between ticks.
        if (rt->step_index >= count) rt->step_index = 0;

        int64_t now = now_ms();
        const macro_step_t *step = &m->steps[rt->step_index];

        if (now - rt->last_fire_tick >= step->delay_after_ms) {
            if (step->button_press   != GP_NONE) gs_set_button(s, step->button_press,   true);
            if (step->button_release != GP_NONE) gs_set_button(s, step->button_release, false);

            rt->last_fire_tick = now;
            rt->step_index = (rt->step_index + 1) % count;

            // Non-looping sequences park on the last step instead of restarting.
            if (rt->step_index == 0 && !m->loop) rt->step_index = count - 1;
        }
    } else if (!held) {
        rt->step_index = 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Toggle
// ═════════════════════════════════════════════════════════════════════════════
static void process_toggle(macro_gamepad_state_t *s, const macro_definition_t *m,
                           macro_runtime_t *rt)
{
    uint32_t trigger = m->activation_button != GP_NONE ? m->activation_button : GP_A;
    bool pressed = gs_pressed(s, trigger);

    if (pressed && !rt->was_pressed) rt->toggle_state = !rt->toggle_state;
    rt->was_pressed = pressed;

    uint32_t target = m->target_button != GP_NONE ? m->target_button : trigger;
    gs_set_button(s, target, rt->toggle_state);
}

// ═════════════════════════════════════════════════════════════════════════════
//  AimAssistBuff - rotational-aim-assist abuse wiggle on LS X
// ═════════════════════════════════════════════════════════════════════════════
static void process_aim_assist_buff(macro_gamepad_state_t *s, const macro_definition_t *m,
                                    macro_runtime_t *rt)
{
    int t = trigger_src_held(s, m->trigger_source);
    bool gate_held = (t < 0) ? true : (t != 0);

    if (!gate_held) {
        rt->toggle_state   = false;
        rt->last_fire_tick = 0;
        return;
    }

    int64_t now = now_ms();
    int32_t base_interval = m->flick_interval_ms > 0 ? m->flick_interval_ms : 16;

    // Jitter the period so the wiggle is not perfectly metronomic. Real human
    // input never is; a fixed period is the signature ML anti-cheat flags.
    // WiggleJitter in the source is a computed property: clamp(RandomizationFactor, 0, 0.5).
    float jitter = clampf(m->randomization_factor, 0.0f, 0.5f);
    int64_t interval = (jitter > 0.0f)
        ? (int64_t)((float)base_interval * (1.0f + mono_rand_sym() * jitter))
        : base_interval;

    if (now - rt->last_fire_tick >= interval) {
        rt->toggle_state = !rt->toggle_state;
        rt->last_fire_tick = now;
    }

    int16_t strength = clamp_s16((int32_t)((float)m->flick_strength * m->intensity));

    // Additive in spirit but implemented as "whoever pushes harder wins" - if we
    // simply added, we would kneecap a player already strafing past our
    // amplitude, and RAA strength is binary above its threshold anyway so a
    // bigger number buys nothing.
    int16_t player_x = s->lx;
    int16_t flick_x  = rt->toggle_state ? strength : (int16_t)(-strength);
    int32_t apx = player_x < 0 ? -(int32_t)player_x : player_x;
    int32_t afx = flick_x  < 0 ? -(int32_t)flick_x  : flick_x;
    s->lx = (apx > afx) ? player_x : flick_x;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ScriptedShape
// ═════════════════════════════════════════════════════════════════════════════
static void process_scripted_shape(macro_gamepad_state_t *s, const macro_definition_t *m,
                                   macro_runtime_t *rt)
{
    const motion_script_t *motion = &m->motion;
    bool held = is_macro_active(s, m, rt);
    int64_t now = now_ms();

    if (!held) {
        rt->motion_activation_tick = 0;
        rt->was_pressed = false;
        return;
    }

    // Press edge - stamp activation so elapsed walks from zero.
    if (rt->motion_activation_tick == 0) rt->motion_activation_tick = now;

    float elapsed_ms = (float)(now - rt->motion_activation_tick);
    motion_sample_t sample = motion_sampler_evaluate(motion, elapsed_ms);

    // A completed Flick stays silent until the button is released (no re-arm),
    // and orbital shapes with DurationMs == 0 never complete.
    if (sample.completed) return;

    apply_motion_sample(s, motion, sample, m->intensity);
}

// ═════════════════════════════════════════════════════════════════════════════
//  HeadAssist - distance-adaptive flick + the distance estimator
// ═════════════════════════════════════════════════════════════════════════════
static float weighted_mean3(float v0, float w0, float v1, float w1, float v2, float w2)
{
    float ws = 0.0f, ss = 0.0f;
    if (w0 > 0.0f) { ss += v0 * w0; ws += w0; }
    if (w1 > 0.0f) { ss += v1 * w1; ws += w1; }
    if (w2 > 0.0f) { ss += v2 * w2; ws += w2; }
    return ws > 0.0f ? ss / ws : 0.5f;
}

static uint8_t estimate_distance(const macro_gamepad_state_t *s,
                                 const head_assist_config_t *cfg,
                                 const macro_runtime_t *rt,
                                 float held_ms)
{
    if (cfg->distance_source == DSRC_MANUAL) return rt->manual_level;

    // Each signal produces a scalar in [0, 1] (0 = close, 1 = long).
    float score_trigger;
    if (held_ms <= cfg->short_hold_ms_max)       score_trigger = 0.0f;
    else if (held_ms >= cfg->medium_hold_ms_max) score_trigger = 1.0f;
    else score_trigger = (held_ms - cfg->short_hold_ms_max) /
                         (cfg->medium_hold_ms_max - cfg->short_hold_ms_max);

    float nx = (float)s->rx / STICK_FULLf;
    float ny = (float)s->ry / STICK_FULLf;
    float mag = sqrtf(nx * nx + ny * ny);
    float score_defl;
    if (mag <= cfg->deflection_short_max)       score_defl = 0.0f;
    else if (mag >= cfg->deflection_medium_max) score_defl = 1.0f;
    else score_defl = (mag - cfg->deflection_short_max) /
                      (cfg->deflection_medium_max - cfg->deflection_short_max);

    // ScoreRecoil reads the weapon profile's recoil magnitude. With no weapon
    // detection on this hardware the source returns the neutral 0.5, so the
    // Auto fusion simply weights the other two signals against a midpoint -
    // identical to running ReflexX with weapon detection off.
    float score_recoil = 0.5f;

    float score;
    switch (cfg->distance_source) {
        case DSRC_TRIGGER_HOLD_TIME:    score = score_trigger; break;
        case DSRC_AIM_STICK_DEFLECTION: score = score_defl;    break;
        case DSRC_RECOIL_MAGNITUDE:     score = score_recoil;  break;
        case DSRC_AUTO:
            score = weighted_mean3(score_trigger, cfg->weight_trigger,
                                   score_defl,    cfg->weight_deflection,
                                   score_recoil,  cfg->weight_recoil);
            break;
        default: score = 0.5f; break;
    }

    if (score <= 0.34f) return DIST_SHORT;
    if (score <= 0.66f) return DIST_MEDIUM;
    return DIST_LONG;
}

static void process_head_assist(macro_gamepad_state_t *s, const macro_definition_t *m,
                                macro_runtime_t *rt)
{
    const head_assist_config_t *cfg = &m->head_assist;
    int64_t now = now_ms();

    int t = trigger_src_held(s, m->trigger_source);
    bool fire_held = (t < 0) ? gs_rt_pressed(s) : (t != 0);

    // LT (ADS) dwell is tracked independently of the fire trigger - the
    // TriggerHoldTime estimator asks "how long were you aiming before you shot?"
    if (gs_lt_pressed(s)) {
        if (rt->ads_down_since_tick == 0) rt->ads_down_since_tick = now;
    } else {
        rt->ads_down_since_tick = 0;
    }

    if (fire_held) {
        if (rt->trigger_down_since_tick == 0) rt->trigger_down_since_tick = now;
    } else {
        rt->trigger_down_since_tick    = 0;
        rt->head_assist_activation_tick = 0;
    }

    // Manual cycle button rotates short -> medium -> long -> short.
    if (cfg->cycle_button != GP_NONE) {
        bool cycle_pressed = gs_pressed(s, cfg->cycle_button);
        if (cycle_pressed && !rt->was_cycle_button_pressed) {
            rt->manual_level = (rt->manual_level == DIST_SHORT)  ? DIST_MEDIUM
                             : (rt->manual_level == DIST_MEDIUM) ? DIST_LONG
                                                                 : DIST_SHORT;
        }
        rt->was_cycle_button_pressed = cycle_pressed;
    }

    bool active_flick = rt->head_assist_activation_tick != 0;
    bool fire_edge    = fire_held && !rt->was_fire_pressed;
    rt->was_fire_pressed = fire_held;

    if (!active_flick && fire_held) {
        float held_ms = (float)(now - rt->trigger_down_since_tick);
        bool cooldown_ok = (now - rt->last_head_assist_tick) >= cfg->refire_cooldown_ms;
        bool min_hold_ok = held_ms >= (float)cfg->min_trigger_hold_ms;
        // FireOnPress is the ADS-snap. min_hold_ok cannot gate the edge frame
        // (held_ms is 0 there) or it would veto the snap forever, so it only
        // gates the cooldown re-fire path.
        bool press_fire    = cfg->fire_on_press && fire_edge;
        bool cooldown_fire = !cfg->fire_once && cooldown_ok && min_hold_ok && !fire_edge;

        if (press_fire || cooldown_fire) {
            float ads_held_ms = rt->ads_down_since_tick > 0
                ? (float)(now - rt->ads_down_since_tick)
                : held_ms;
            rt->current_head_assist_level  = estimate_distance(s, cfg, rt, ads_held_ms);
            rt->head_assist_activation_tick = now;
            rt->last_head_assist_tick       = now;
        }
    }

    if (rt->head_assist_activation_tick == 0) return;

    const motion_script_t *script =
        (rt->current_head_assist_level == DIST_SHORT)  ? &cfg->short_range :
        (rt->current_head_assist_level == DIST_MEDIUM) ? &cfg->medium_range
                                                       : &cfg->long_range;

    float elapsed = (float)(now - rt->head_assist_activation_tick);
    motion_sample_t sample = motion_sampler_evaluate(script, elapsed);
    if (sample.completed) {
        rt->head_assist_activation_tick = 0;
        return;
    }

    apply_motion_sample(s, script, sample, m->intensity);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ProgressiveRecoil - 3-phase, ammo-proportioned, eased
// ═════════════════════════════════════════════════════════════════════════════
static bool is_second_stage_recoil_active(const macro_gamepad_state_t *s,
                                          const progressive_recoil_config_t *cfg)
{
    int t = trigger_src_held(s, cfg->second_stage_trigger_source);
    bool trigger_held = (t < 0) ? true : (t != 0);
    bool button_held = cfg->second_stage_activation_button == GP_NONE ||
                       gs_pressed(s, cfg->second_stage_activation_button);
    return trigger_held && button_held;
}

static void process_progressive_recoil(macro_gamepad_state_t *s, const macro_definition_t *m,
                                       macro_runtime_t *rt)
{
    const progressive_recoil_config_t *cfg = &m->progressive_recoil;
    int64_t now = now_ms();

    // NOTE the default here differs from NoRecoil: TriggerSource.None means NOT
    // shooting (`?? false`), so a ProgressiveRecoil macro with no trigger source
    // is inert rather than always-on. Preserved deliberately.
    int t = trigger_src_held(s, m->trigger_source);
    bool is_shooting = (t < 0) ? false : (t != 0);

    if (!button_activation(s, m, rt) || !is_shooting) {
        rt->progressive_fire_start_tick = 0;
        return;
    }

    if (rt->progressive_fire_start_tick == 0) rt->progressive_fire_start_tick = now;

    // Sensitivity scales the mag duration inversely AND the compensation
    // inversely: a higher-sens player empties the mag in the same real time but
    // needs less stick travel per unit of recoil.
    float sens_mul = cfg->sensitivity_scale > 0.0f ? cfg->sensitivity_scale : 1.0f;
    float effective_duration = cfg->full_mag_duration_ms / sens_mul;
    float comp_scale = 1.0f / sens_mul;
    if (effective_duration <= 0.0f) effective_duration = 1.0f; // guard /0

    float elapsed_ms = (float)(now - rt->progressive_fire_start_tick);
    float progress = clampf(elapsed_ms / effective_duration, 0.0f, 1.0f);

    // Phase boundaries follow the ammo split, not equal thirds - a 30-round mag
    // and a 100-round belt get different curves out of the same config.
    int32_t total = cfg->total_ammo > 3 ? cfg->total_ammo : 3;
    int32_t start_count = total / 3;
    int32_t mid_count   = (total - start_count) / 2;
    float p1 = (float)start_count / (float)total;
    float p2 = (float)(start_count + mid_count) / (float)total;

    float comp_x, comp_y;
    if (progress <= p1) {
        float tt = (p1 > 0.0f) ? progress / p1 : 1.0f;
        float eased = apply_phase_easing(cfg->phase_easing, tt);
        comp_x = lerpf(0.0f, (float)cfg->start_comp_x, eased);
        comp_y = lerpf(0.0f, (float)cfg->start_comp_y, eased);
    } else if (progress <= p2) {
        float denom = p2 - p1;
        float tt = (denom > 0.0f) ? (progress - p1) / denom : 1.0f;
        float eased = apply_phase_easing(cfg->phase_easing, tt);
        comp_x = lerpf((float)cfg->start_comp_x, (float)cfg->mid_comp_x, eased);
        comp_y = lerpf((float)cfg->start_comp_y, (float)cfg->mid_comp_y, eased);
    } else {
        float denom = 1.0f - p2;
        float tt = (denom > 0.0f) ? (progress - p2) / denom : 1.0f;
        float eased = apply_phase_easing(cfg->phase_easing, tt);
        comp_x = lerpf((float)cfg->mid_comp_x, (float)cfg->end_comp_x, eased);
        comp_y = lerpf((float)cfg->mid_comp_y, (float)cfg->end_comp_y, eased);
    }

    comp_x *= comp_scale * m->intensity;
    comp_y *= comp_scale * m->intensity;

    if (cfg->second_stage_enabled && is_second_stage_recoil_active(s, cfg)) {
        comp_x += (float)cfg->second_stage_comp_x * m->intensity;
        comp_y += (float)cfg->second_stage_comp_y * m->intensity;
    }

    if (cfg->noise_factor > 0.0f) {
        float noise_range = cfg->noise_factor * 100.0f;
        comp_x += mono_rand_sym() * noise_range;
        comp_y += mono_rand_sym() * noise_range;
    }

    s->rx = clamp_s16((int32_t)s->rx + (int32_t)comp_x);
    s->ry = clamp_s16((int32_t)s->ry + (int32_t)comp_y);
}

// ═════════════════════════════════════════════════════════════════════════════
//  TrackingAssist - orbital overlay whose radius follows stick deflection
// ═════════════════════════════════════════════════════════════════════════════
static void process_tracking_assist(macro_gamepad_state_t *s, const macro_definition_t *m,
                                    macro_runtime_t *rt)
{
    const tracking_assist_config_t *cfg = &m->tracking_assist;
    int64_t now = now_ms();

    int t = trigger_src_held(s, m->trigger_source);
    bool is_shooting = (t < 0) ? true : (t != 0);

    if (!is_shooting) {
        rt->tracking_start_tick = 0;
        return;
    }

    float raw_x, raw_y;
    if (cfg->target == STICK_LEFT) {
        raw_x = (float)s->lx / STICK_FULLf;
        raw_y = (float)s->ly / STICK_FULLf;
    } else {
        raw_x = (float)s->rx / STICK_FULLf;
        raw_y = (float)s->ry / STICK_FULLf;
    }
    float magnitude = sqrtf(raw_x * raw_x + raw_y * raw_y);

    // FreeOrbit keeps the orbit running with an idle stick (passive assist);
    // without it the macro is silent until the player actually moves the stick.
    if (!cfg->free_orbit && magnitude < cfg->deflection_threshold) {
        rt->tracking_start_tick = 0;
        return;
    }

    if (rt->tracking_start_tick == 0) rt->tracking_start_tick = now;

    float radius;
    if (magnitude >= cfg->deflection_threshold) {
        float span = 1.0f - cfg->deflection_threshold;
        float norm_mag = clampf(span > 0.0f ? (magnitude - cfg->deflection_threshold) / span : 0.0f,
                                0.0f, 1.0f);
        float radius_factor = powf(norm_mag, cfg->scale_curve);
        radius = lerpf(cfg->base_radius_norm, cfg->max_radius_norm, radius_factor);
    } else {
        radius = cfg->base_radius_norm; // FreeOrbit with idle stick
    }
    radius *= cfg->intensity_mul * m->intensity;

    float period_ms = cfg->period_ms > 0.0f ? cfg->period_ms : 120.0f;
    float elapsed_ms = (float)(now - rt->tracking_start_tick);
    float sign = cfg->clockwise ? 1.0f : -1.0f;
    float theta = sign * (elapsed_ms / period_ms) * 2.0f * M_PIf;

    int16_t dx = clamp_s16((int32_t)(radius * cosf(theta) * STICK_FULLf));
    int16_t dy = clamp_s16((int32_t)(radius * sinf(theta) * STICK_FULLf));

    if (cfg->target == STICK_LEFT) {
        s->lx = clamp_s16((int32_t)s->lx + dx);
        s->ly = clamp_s16((int32_t)s->ly + dy);
    } else {
        s->rx = clamp_s16((int32_t)s->rx + dx);
        s->ry = clamp_s16((int32_t)s->ry + dy);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  AutoFireNoRecoil
// ═════════════════════════════════════════════════════════════════════════════
static void process_auto_fire_no_recoil(macro_gamepad_state_t *s, const macro_definition_t *m,
                                        macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;

    int t = trigger_src_held(s, m->trigger_source);
    bool is_shooting = (t < 0) ? false : (t != 0);
    if (!is_shooting) return;

    int64_t now = now_ms();
    int32_t interval = m->interval_ms > 0 ? m->interval_ms : 40;

    if (now - rt->last_fire_tick >= interval) {
        rt->toggle_state = !rt->toggle_state;
        rt->last_fire_tick = now;
    }

    set_fire_input(s, m->trigger_source, rt->toggle_state);

    // Compensation only while the synthesized press is down - between shots the
    // gun is not recoiling, so pulling then would just drag the aim.
    if (rt->toggle_state) {
        float rand_mul = (m->randomization_factor > 0.0f)
            ? 1.0f + mono_rand_sym() * m->randomization_factor
            : 1.0f;

        int32_t final_x = (int32_t)((float)m->recoil_compensation_x * m->intensity * rand_mul);
        int32_t final_y = (int32_t)((float)m->recoil_compensation_y * m->intensity * rand_mul);

        s->rx = clamp_s16((int32_t)s->rx + final_x);
        s->ry = clamp_s16((int32_t)s->ry + final_y);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  InstaDropShot / JumpShot / StrafeShot / HoldBreath / SlideCancel / FastDrop
// ═════════════════════════════════════════════════════════════════════════════
static void process_insta_drop_shot(macro_gamepad_state_t *s, const macro_definition_t *m,
                                    macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;
    gs_set_button(s, m->crouch_button, true); // held -> game skips crouch->prone delay
}

static void process_jump_shot(macro_gamepad_state_t *s, const macro_definition_t *m,
                              macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;

    int t = trigger_src_held(s, m->trigger_source);
    bool trigger_held = (t < 0) ? false : (t != 0);
    if (!trigger_held) return;

    int64_t now = now_ms();
    int32_t interval = m->jump_interval_ms > 0 ? m->jump_interval_ms : 500;

    if (now - rt->last_fire_tick >= interval) {
        rt->last_fire_tick   = now;
        rt->pulse_until_tick = now + 80; // 80 ms press
    }
    if (now <= rt->pulse_until_tick) gs_set_button(s, m->jump_button, true);
}

static void process_strafe_shot(macro_gamepad_state_t *s, const macro_definition_t *m,
                                macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;

    int t = trigger_src_held(s, m->trigger_source);
    bool trigger_held = (t < 0) ? false : (t != 0);
    if (!trigger_held) return;

    int64_t now = now_ms();
    int32_t interval = m->strafe_interval_ms > 0 ? m->strafe_interval_ms : 120;

    if (now - rt->last_fire_tick >= interval) {
        rt->toggle_state = !rt->toggle_state;
        rt->last_fire_tick = now;
    }

    // The source casts the scaled amplitude straight to short. That is safe in C#
    // (a checked-context-free conversion wraps) but a float-to-int16 cast out of
    // range is undefined behaviour in C, and nothing stops a hand-edited profile
    // from carrying strafe_amplitude > 1. Clamp through int32 instead.
    float amp = m->strafe_amplitude * STICK_FULLf;
    int16_t strafe_x = clamp_s16((int32_t)(rt->toggle_state ? amp : -amp));
    s->lx = clamp_s16((int32_t)s->lx + strafe_x);
}

static void process_hold_breath(macro_gamepad_state_t *s, const macro_definition_t *m,
                                macro_runtime_t *rt)
{
    // Deliberately NOT is_macro_active(): its trigger gate would block on the
    // RightTrigger default and gate breath-holding on shooting, which is what
    // made this macro appear dead in the source's earlier releases.
    if (!button_activation(s, m, rt)) return;

    // Holding breath is an ADS action. TriggerSource defaults to RightTrigger
    // (shared with NoRecoil/AutoFire), so both None AND RightTrigger are read as
    // "use ADS"; only an explicitly chosen LT/LB/RB overrides that, for
    // bumper-ADS players.
    bool is_aiming;
    switch (m->trigger_source) {
        case TRIG_SRC_LEFT_TRIGGER:   is_aiming = gs_lt_pressed(s); break;
        case TRIG_SRC_LEFT_SHOULDER:  is_aiming = gs_pressed(s, GP_LEFT_SHOULDER);  break;
        case TRIG_SRC_RIGHT_SHOULDER: is_aiming = gs_pressed(s, GP_RIGHT_SHOULDER); break;
        default:                      is_aiming = gs_lt_pressed(s); break;
    }

    if (is_aiming && m->breath_button != GP_NONE)
        gs_set_button(s, m->breath_button, true);
}

static void process_slide_cancel(macro_gamepad_state_t *s, const macro_definition_t *m,
                                 macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;

    int64_t now = now_ms();
    bool slide_pressed = gs_pressed(s, m->slide_button);

    if (slide_pressed && !rt->was_slide_pressed) rt->slide_start_tick = now;
    rt->was_slide_pressed = slide_pressed;

    if (rt->slide_start_tick > 0) {
        int64_t elapsed = now - rt->slide_start_tick;
        int32_t delay = m->slide_cancel_delay_ms > 0 ? m->slide_cancel_delay_ms : 180;

        if (elapsed >= delay && elapsed < delay + 80) {
            gs_set_button(s, m->slide_cancel_button, true);
        } else if (elapsed >= delay + 80) {
            rt->slide_start_tick = 0; // re-arm for the next slide
        }
    }
}

static void process_fast_drop(macro_gamepad_state_t *s, const macro_definition_t *m,
                              macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;

    int64_t now = now_ms();
    int t = trigger_src_held(s, m->trigger_source);
    bool trigger_held = (t < 0) ? gs_lt_pressed(s) : (t != 0);

    bool fire_edge = trigger_held && !rt->was_fire_pressed;
    rt->was_fire_pressed = trigger_held;

    if (fire_edge) rt->fast_drop_start_tick = now;

    if (trigger_held && rt->fast_drop_start_tick > 0)
        gs_set_button(s, m->crouch_button, true);

    if (!trigger_held) rt->fast_drop_start_tick = 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  AutoSprint - single click on the forward-threshold rising edge
// ═════════════════════════════════════════════════════════════════════════════
static void process_auto_sprint(macro_gamepad_state_t *s, const macro_definition_t *m,
                                macro_runtime_t *rt)
{
    int64_t now = now_ms();

    if (!button_activation(s, m, rt)) {
        rt->was_sprint_armed   = false;
        rt->sprint_release_tick = 0;
        return;
    }

    float threshold = clampf(m->sprint_threshold, 0.05f, 1.0f);
    float forward = (float)s->ly / STICK_FULLf;
    bool armed_now = forward >= threshold;

    // Rising edge -> schedule ONE press of configurable length. Modern shooters
    // expect sprint as a tap, not a hold.
    if (armed_now && !rt->was_sprint_armed) {
        int32_t duration = m->sprint_press_duration_ms;
        if (duration < 10)  duration = 10;
        if (duration > 500) duration = 500;
        rt->sprint_release_tick = now + duration;
    }

    // Re-arm only once the player lets go of forward, or we would re-click
    // continuously while they hold it (the in-game sprint state persists on its
    // own).
    rt->was_sprint_armed = armed_now;

    if (rt->sprint_release_tick > 0 && now < rt->sprint_release_tick)
        gs_set_button(s, m->sprint_button, true);
    else
        rt->sprint_release_tick = 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  CrowBar - cooperative anti-recoil that AMPLIFIES manual stick-down input
// ═════════════════════════════════════════════════════════════════════════════
static void process_crowbar(macro_gamepad_state_t *s, const macro_definition_t *m,
                            macro_runtime_t *rt)
{
    if (!is_macro_active(s, m, rt)) return;

    int t = trigger_src_held(s, m->trigger_source);
    bool is_shooting = (t < 0) ? gs_rt_pressed(s) : (t != 0);
    if (!is_shooting) return;

    const crowbar_config_t *cfg = &m->crowbar;

    // XInput convention: negative Y = stick pushed DOWN.
    float player_y = (float)s->ry / STICK_FULLf;

    // Cooperative: compensate ONLY while the player is actively pulling down
    // past the threshold. Idle stick -> no assist at all, which is the whole
    // point of the mechanic.
    if (player_y >= -cfg->deflection_threshold) return;

    float deflection_mag = clampf(fabsf(player_y) - cfg->deflection_threshold, 0.0f, 1.0f);
    float max_range = 1.0f - cfg->deflection_threshold;
    float normalized = max_range > 0.0f ? deflection_mag / max_range : 0.0f;

    float curved = powf(normalized, fmaxf(cfg->deflection_curve, 0.1f));

    float effective_htg = (float)cfg->base_htg_value;
    if (cfg->mode == CROWBAR_PADRAO) effective_htg *= cfg->htg_scale_padrao; // 16 * 1.125 = 18

    // x100 converts HTG game-units into stick-axis units.
    float compensation = effective_htg * cfg->assist_factor * curved * 100.0f;
    compensation = clampf(compensation, 0.0f, (float)cfg->max_compensation);

    if (cfg->noise_factor > 0.0f) {
        float noise_range = cfg->noise_factor * compensation * 0.1f;
        compensation += mono_rand_sym() * noise_range;
    }

    compensation *= m->intensity;

    // Push DOWN (negative Y) to counter upward recoil.
    s->ry = clamp_s16((int32_t)s->ry - (int32_t)compensation);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Custom - Cronus-Zen-style step sequencer
// ═════════════════════════════════════════════════════════════════════════════
static void reset_script(macro_runtime_t *rt)
{
    rt->script_step_index        = 0;
    rt->script_step_start_tick   = 0;
    rt->script_completed         = false;
    rt->script_loop_count        = 0;
    rt->script_first_start_tick  = 0;
    rt->script_current_wait_jitter = 0;
    memset(rt->loop_counters, 0, sizeof(rt->loop_counters));
}

static void apply_script_axis_value(macro_gamepad_state_t *s, uint8_t axis, int16_t value)
{
    switch (axis) {
        case AXIS_LEFT_STICK_X:  s->lx = value; break;
        case AXIS_LEFT_STICK_Y:  s->ly = value; break;
        case AXIS_RIGHT_STICK_X: s->rx = value; break;
        case AXIS_RIGHT_STICK_Y: s->ry = value; break;
        default: break;
    }
}

static void apply_script_trigger_value(macro_gamepad_state_t *s, uint8_t axis, uint8_t value)
{
    switch (axis) {
        case AXIS_LEFT_TRIGGER:  s->lt = value; break;
        case AXIS_RIGHT_TRIGGER: s->rt = value; break;
        default: break;
    }
}

static void process_custom_script(macro_gamepad_state_t *s, const macro_definition_t *m,
                                  macro_runtime_t *rt)
{
    const script_definition_t *script = &m->script;
    int step_count = script->step_count;
    if (step_count > MACRO_MAX_SCRIPT_STEPS) step_count = MACRO_MAX_SCRIPT_STEPS;
    if (step_count <= 0) return;

    int64_t now = now_ms();

    // ── Trigger mode ─────────────────────────────────────────────────────────
    bool should_run;
    switch (script->trigger_mode) {
        case STRIG_ON_PRESS: {
            bool active = is_macro_active(s, m, rt);
            bool edge = active && !rt->script_was_triggered;
            rt->script_was_triggered = active;
            if (edge) reset_script(rt);
            else if (rt->script_completed) return;
            should_run = !rt->script_completed;
            break;
        }
        case STRIG_WHILE_HELD:
        case STRIG_TOGGLE:
        default:
            // Toggle shares WhileHeld's shape here because is_macro_active()
            // already resolves the toggle latch when ToggleMode is set - the
            // distinction lives in the activation gate, not in this switch.
            should_run = is_macro_active(s, m, rt);
            if (!should_run) { reset_script(rt); return; }
            break;
    }

    if (!should_run || rt->script_completed) return;

    if (rt->script_first_start_tick == 0) rt->script_first_start_tick = now;

    if (script->stop_after_ms > 0 &&
        now - rt->script_first_start_tick >= script->stop_after_ms) {
        rt->script_completed = true;
        return;
    }

    float speed_mul = script->speed_multiplier > 0.0f ? script->speed_multiplier : 1.0f;

    // Non-blocking steps execute back-to-back within one tick; Wait and LoopBack
    // yield. The safety counter is the source's guard against a pathological
    // script (e.g. a LoopStart/LoopBack pair with no Wait) spinning forever - on
    // an MCU that guard is the difference between a stutter and a dead USB
    // device, so it is kept verbatim.
    int safety = 0;
    while (rt->script_step_index < step_count && safety++ < 256) {
        if (rt->script_step_index >= step_count) { rt->script_step_index = 0; break; }

        const script_step_t *step = &script->steps[rt->script_step_index];

        if (step->disabled) { rt->script_step_index++; continue; }

        switch (step->action) {
            case SACT_PRESS_BUTTON:
                if (step->button != GP_NONE) gs_set_button(s, step->button, true);
                rt->script_step_index++;
                continue;

            case SACT_RELEASE_BUTTON:
                if (step->button != GP_NONE) gs_set_button(s, step->button, false);
                rt->script_step_index++;
                continue;

            case SACT_SET_AXIS:
                if (step->has_axis) apply_script_axis_value(s, step->axis, step->value);
                rt->script_step_index++;
                continue;

            case SACT_SET_TRIGGER: {
                if (step->has_axis) {
                    int v = step->value;
                    if (v < 0) v = 0;
                    if (v > 255) v = 255;
                    apply_script_trigger_value(s, step->axis, (uint8_t)v);
                }
                rt->script_step_index++;
                continue;
            }

            case SACT_WAIT: {
                if (rt->script_step_start_tick == 0) {
                    rt->script_step_start_tick = now;
                    rt->script_current_wait_jitter = 0;
                    if (script->random_delay_max_ms > 0) {
                        int range = script->random_delay_max_ms - script->random_delay_min_ms;
                        if (range < 0) range = 0;
                        rt->script_current_wait_jitter =
                            script->random_delay_min_ms + mono_rand_range(0, range);
                    }
                }
                int32_t wait_ms = (int32_t)((float)step->duration_ms / speed_mul)
                                  + rt->script_current_wait_jitter;
                if (now - rt->script_step_start_tick >= wait_ms) {
                    rt->script_step_start_tick = 0;
                    rt->script_current_wait_jitter = 0;
                    rt->script_step_index++;
                    continue;
                }
                return; // still waiting - yield the tick
            }

            case SACT_LOOP_START:
                rt->script_step_index++;
                continue; // marker only

            case SACT_LOOP_BACK: {
                int target = step->loop_target_index;
                if (target < 0) target = 0;
                if (target > step_count - 1) target = step_count - 1;

                if (step->repeat_count <= 0) {
                    rt->script_step_index = target;
                    return; // infinite loop - yield so we cannot spin
                }
                rt->loop_counters[rt->script_step_index]++;
                if (rt->loop_counters[rt->script_step_index] < step->repeat_count) {
                    rt->script_step_index = target;
                    return; // yield
                }
                rt->loop_counters[rt->script_step_index] = 0; // exhausted
                rt->script_step_index++;
                continue;
            }

            default:
                rt->script_step_index++;
                continue;
        }
    }

    // Fell off the end of the script.
    rt->script_loop_count++;

    if (script->max_repeat_count > 0 && rt->script_loop_count >= script->max_repeat_count) {
        rt->script_completed = true;
    } else if (script->auto_loop) {
        rt->script_step_index = 0;
        rt->script_step_start_tick = 0;
        memset(rt->loop_counters, 0, sizeof(rt->loop_counters));
    } else {
        rt->script_completed = true;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  LuaScript - documented no-op on this hardware
// ═════════════════════════════════════════════════════════════════════════════
// In ReflexX this macro runs user Lua per tick under a sandboxed MoonSharp
// (pure-C#) interpreter with a tick budget. There is no Lua VM in this firmware
// pass: MoonSharp is .NET-only, and an embedded Lua would need ~100 KB of flash
// plus a heap on the 1 kHz input path, plus a way to get the script text onto
// the device (the config CDC channel could carry it, but the VM has to exist
// first). The source's own fallback for a missing engine is
// `if (_scriptEngine is null) return state;` - a silent pass-through - and that
// is exactly what happens here. The dispatch case, the config struct slot and
// MacroRuntime.lua_last_tick_ms are all wired, so dropping in an embedded VM
// later is a change to this one function.
//
// The bumper-layout channel swap (SwapTriggerShoulder around the tick, so one
// portable script serves both RT/LT and RB/LB players) is not reproduced because
// there is no tick to wrap; it belongs with the VM that lands later.
static void process_lua_script(macro_gamepad_state_t *s, const macro_definition_t *m,
                               macro_runtime_t *rt)
{
    (void)s; (void)m;
    rt->lua_last_tick_ms = now_ms(); // keep the delta anchor warm for a future VM
}

// ═════════════════════════════════════════════════════════════════════════════
//  AimSnap - one-shot flick impulse toward the AI-locked target
// ═════════════════════════════════════════════════════════════════════════════
// The state machine (button edge -> acquire -> arm impulse -> hold -> cooldown)
// is fully live; only the acquire step can never succeed on this hardware. Uses
// microseconds rather than ms because the source read Stopwatch ticks directly
// here - a 60 ms impulse window quantised to whole ms would be visibly chunky.
static void process_aim_snap(macro_gamepad_state_t *s, const macro_definition_t *m,
                             macro_runtime_t *rt)
{
    const aim_assist_config_t *cfg = &m->aim_assist;
    int64_t now = now_us();

    // AimSnap requires an explicit activation button; without one we skip
    // silently (the user must pick one in the editor).
    if (m->activation_button == GP_NONE) return;

    bool btn_now  = gs_pressed(s, m->activation_button);
    bool btn_edge = btn_now && !rt->aim_snap_button_was_pressed;
    rt->aim_snap_button_was_pressed = btn_now;

    // Inside the flick window: keep emitting the stored delta.
    if (rt->aim_snap_impulse_end_tick > 0) {
        if (now < rt->aim_snap_impulse_end_tick) {
            s->rx = clamp_s16((int32_t)s->rx + rt->aim_snap_impulse_dx);
            s->ry = clamp_s16((int32_t)s->ry + rt->aim_snap_impulse_dy);
            return;
        }
        rt->aim_snap_impulse_end_tick  = 0;
        rt->aim_snap_cooldown_end_tick = now + (int64_t)cfg->snap_cooldown_ms * 1000;
        rt->aim_snap_impulse_dx = 0;
        rt->aim_snap_impulse_dy = 0;
    }

    if (now < rt->aim_snap_cooldown_end_tick) return;
    if (!btn_edge) return;

    aim_target_t target;
    if (!aim_context_try_acquire(cfg, &target)) return;

    int fov = target.fov_ring_pixels > 0 ? target.fov_ring_pixels : 200;
    float cross_x = target.capture_width  * 0.5f;
    float cross_y = target.capture_height * 0.5f;

    // Aim point: bbox centre, shifted up by HeadshotBiasFraction x bbox height.
    float aim_x = target.x + target.width  * 0.5f;
    float aim_y = target.y + target.height * (0.5f - cfg->headshot_bias_fraction);

    float err_x = aim_x - cross_x;
    float err_y = aim_y - cross_y;
    float err_mag = sqrtf(err_x * err_x + err_y * err_y);

    if (err_mag <= 0.0f || err_mag > (float)fov) return;

    float normalised = fminf(err_mag / (float)fov, 1.0f);
    float stick_mag  = normalised * (float)cfg->snap_max_impulse_stick_units;

    float ux = err_x / err_mag;
    float uy = err_y / err_mag;

    int dx = (int)(ux * stick_mag);
    int dy_raw = (int)(uy * stick_mag);
    // Screen +Y is down; FPS right stick +Y is look-up. Invert unless the game
    // is configured Y-inverted.
    int dy = cfg->magnetic_invert_y ? dy_raw : -dy_raw;

    if (cfg->humanization_sigma > 0.0f) {
        dx += gaussian_int(cfg->humanization_sigma);
        dy += gaussian_int(cfg->humanization_sigma);
    }

    rt->aim_snap_impulse_dx = dx;
    rt->aim_snap_impulse_dy = dy;
    rt->aim_snap_impulse_end_tick = now + (int64_t)cfg->snap_impulse_duration_ms * 1000;

    s->rx = clamp_s16((int32_t)s->rx + dx);
    s->ry = clamp_s16((int32_t)s->ry + dy);
}

// ═════════════════════════════════════════════════════════════════════════════
//  AimSmooth - continuous magnetic pull with EMA output smoothing
// ═════════════════════════════════════════════════════════════════════════════

// Round away from zero. The source originally truncated with (int), which
// floored the [0.5, 1) band to 0 and made the assist stall half a pixel shy of
// the target on every tick; rounding keeps the sub-pixel error symmetric.
static int round_to_stick(float v)
{
    return (int)(v >= 0.0f ? floorf(v + 0.5f) : ceilf(v - 0.5f));
}

// The C# local function ComputeRawCorrection(), lifted to a static. Returns
// (0, 0) on any gate rejection - the EMA above then decays toward neutral rather
// than cutting hard.
static void aim_smooth_raw_correction(const macro_gamepad_state_t *s,
                                      const macro_definition_t *m,
                                      macro_runtime_t *rt,
                                      float *out_dx, float *out_dy)
{
    const aim_assist_config_t *cfg = &m->aim_assist;
    *out_dx = 0.0f; *out_dy = 0.0f;

    // Activation gate: whichever input the user chose arms the magnetic pull.
    // None -> always active, relying on the FOV-ring / confidence gates alone.
    int t = trigger_src_held(s, m->trigger_source);
    bool gate_pass = (t < 0) ? true : (t != 0);
    if (!gate_pass) {
        rt->aim_smooth_has_target = false;
        rt->aim_smooth_holding    = false;
        return;
    }

    aim_target_t target;
    if (!aim_context_try_acquire(cfg, &target)) {
        rt->aim_smooth_has_target = false;
        rt->aim_smooth_holding    = false;
        return;
    }

    // ── Coast guard ──────────────────────────────────────────────────────────
    // The tracker keeps a lock alive through a brief occlusion so re-acquire is
    // instant, but while coasting the bbox is FROZEN at its last position.
    // Pulling toward it steers the camera at where the target *was*, which the
    // player feels as the stick yanking itself. Emit nothing and let the EMA ease
    // off; the lock and LockId are untouched so the pull resumes from rest the
    // instant a fresh detection lands.
    if (target.is_coasting) {
        rt->aim_smooth_has_target   = true; // a 1-frame flicker is not a switch
        rt->aim_smooth_last_lock_id = target.lock_id;
        return;
    }

    // ── Target-switch guard ──────────────────────────────────────────────────
    // Keyed off the tracker's LockId, which is bumped only on a genuine switch -
    // NOT on a per-frame position jump. The old jump heuristic misfired during a
    // manual aim swing (the player's own motion races the SAME target's bbox
    // across the capture), zeroing the momentum every tick so the pull never
    // built while dragging the FOV onto a target.
    bool had_tracked_target = rt->aim_smooth_has_target;
    if (rt->aim_smooth_has_target && target.lock_id != rt->aim_smooth_last_lock_id) {
        rt->aim_smooth_velocity_x = 0.0f;
        rt->aim_smooth_velocity_y = 0.0f;
        rt->aim_smooth_holding    = false; // new target -> must re-settle
    }
    rt->aim_smooth_last_lock_id = target.lock_id;
    rt->aim_smooth_has_target   = true;

    int fov = target.fov_ring_pixels > 0 ? target.fov_ring_pixels : 200;
    float cross_x = target.capture_width  * 0.5f;
    float cross_y = target.capture_height * 0.5f;

    // Distance heuristic: bbox big relative to the ring -> close (1), tiny -> far (0).
    float bbox_max = fmaxf(target.width, target.height);
    float bbox_min = fminf(target.width, target.height);
    float dist_factor = clampf(bbox_max / fmaxf(1.0f, (float)fov), 0.0f, 1.0f);

    // Prediction: project the bbox forward to cover capture->inference->HID
    // latency. Far targets need more lead (a small bbox in px means a larger
    // angular error per frame of latency), so scale up to 1.5x when far.
    float pred_scale = 1.0f + (1.0f - dist_factor) * 0.5f;
    float pred_ms = fmaxf(0.0f, cfg->prediction_ms) * pred_scale;
    float bbox_x = target.x + target.velocity_x * pred_ms;
    float bbox_y = target.y + target.velocity_y * pred_ms;

    // Ego-motion feed-forward: the bbox is 1-2 frames stale but the player's own
    // stick input is known instantly, so shifting the box by it puts the target
    // where it actually is NOW. Independent of PredictionMs so the two do not
    // double-count. Off when EgoMotionGain is 0 (the default).
    float ego_gain = fmaxf(0.0f, cfg->ego_motion_gain);
    if (ego_gain > 0.0f) {
        float rsx = clampf((float)s->rx / STICK_FULLf, -1.0f, 1.0f);
        float rsy = clampf((float)s->ry / STICK_FULLf, -1.0f, 1.0f);
        bbox_x += -rsx * ego_gain;
        bbox_y += (cfg->magnetic_invert_y ? -rsy : rsy) * ego_gain;
    }

    float aim_x = bbox_x + target.width  * 0.5f;
    float aim_y = bbox_y + target.height * (0.5f - cfg->headshot_bias_fraction);

    float err_x = aim_x - cross_x;
    float err_y = aim_y - cross_y;
    float err_mag = sqrtf(err_x * err_x + err_y * err_y);

    // ── Acquisition gate, measured to the AIM POINT ─────────────────────────
    // Gating on the nearest bbox EDGE let a close target whose body merely
    // clipped the ring trigger a full-strength yank toward a centre well outside
    // it. Retention, by contrast, is identity-based: once committed, the lock is
    // held while the LockId matches and detections stay fresh, no matter how far
    // the error opens. A radius-based release dropped the lock on any hard strafe
    // and then demanded the aim point back inside the ring, which never happens
    // on its own - the assist went permanently silent.
    bool committed = rt->aim_smooth_engaged_lock_id == target.lock_id;
    if (!committed) {
        // Small rescue band helps an already-tracked target survive one fast
        // frame at the boundary; exactly 0 on a cold first sight, so nothing
        // engages outside the visible ring.
        float acquire_rescue = had_tracked_target
            ? clampf(bbox_max * 0.35f, 8.0f, fmaxf(8.0f, (float)fov * 0.25f))
            : 0.0f;
        if (err_mag > (float)fov + acquire_rescue) return;
        rt->aim_smooth_engaged_lock_id = target.lock_id;
    }

    // ── Velocity feed-forward - ride the target, don't pump ─────────────────
    // A stick deflection is a camera VELOCITY, so centring it stops the camera.
    // An error-only magnet therefore oscillates on a strafing target: pull ->
    // error collapses -> stick centres -> target moves on -> pull again. A mouse
    // player instead holds a steady drag matching the target's speed. This term
    // reproduces that using the px<->stick calibration, and holds it for as long
    // as the lock is engaged - including inside the sticky-hold zone where the
    // magnet is silent. Inert when uncalibrated or when the gain is 0.
    float ff_x = 0.0f, ff_y = 0.0f;
    float calib = target.calibration_px_per_ms;
    float ff_gain = clampf(cfg->tracking_feed_forward, 0.0f, 2.0f);
    if (calib > 0.0f && ff_gain > 0.0f) {
        float frac_x = clampf(target.velocity_x / calib * ff_gain, -1.0f, 1.0f);
        float frac_y = clampf(target.velocity_y / calib * ff_gain, -1.0f, 1.0f);
        // Velocity deadband: the tracker's EMA velocity never settles to exactly
        // zero, and inside the sticky-hold zone that jitter is the ONLY thing
        // driving the stick - it walks the crosshair off the aim point toward the
        // bbox edge. Treat sub-deadband motion as stationary so a still target
        // parks dead-centre.
        const float FF_DEADBAND = 0.02f; // 2% of full deflection
        if (fabsf(frac_x) < FF_DEADBAND) frac_x = 0.0f;
        if (fabsf(frac_y) < FF_DEADBAND) frac_y = 0.0f;
        ff_x = frac_x * STICK_FULLf;
        float ff_y_raw = frac_y * STICK_FULLf;
        ff_y = cfg->magnetic_invert_y ? ff_y_raw : -ff_y_raw;
    }

    // ── Sticky lock: SMALL absolute hold zone at the aim point + hysteresis ──
    // Holding across the whole bbox silenced the magnet over a 40 px+ dead
    // region where only the noisy feed-forward carried the aim, so the crosshair
    // random-walked out the far side. A small deadzone (5-12 px) keeps the magnet
    // re-centring right up to the aim point. Schmitt release at 2x stops
    // boundary chatter on bbox noise.
    float hold_radius = fmaxf(5.0f, fminf(bbox_min * 0.12f, 12.0f));
    if (cfg->magnetic_sticky_lock) {
        if (rt->aim_smooth_holding && err_mag > hold_radius * 2.0f) {
            rt->aim_smooth_holding = false;      // drifted out -> re-centre
        } else if (!rt->aim_smooth_holding && err_mag <= hold_radius) {
            rt->aim_smooth_holding = true;
        }
        if (rt->aim_smooth_holding) { *out_dx = ff_x; *out_dy = ff_y; return; }
    }

    if (err_mag <= 0.0f) { *out_dx = ff_x; *out_dy = ff_y; return; }

    float eff_strength = effective_magnetic_strength(s, m, cfg);
    float stick_mag;

    if (calib > 0.0f) {
        // ── Calibrated pull: plant-inverted P controller ─────────────────────
        // The open-loop magnet scales by (err/ring)^curve x MaxPull - stick units
        // with no physical meaning, so the real correction rate depends on the
        // game's sensitivity curve and the error closes slower than a strafe
        // re-opens it. With the calibration we invert the plant: demand the
        // camera speed that closes the error at strength/33 ms, convert px/ms to
        // a stick fraction, and convergence becomes sensitivity-independent.
        const float SETTLE_MS_AT_FULL_STRENGTH = 33.0f;
        float demand_px_per_ms = err_mag * eff_strength / SETTLE_MS_AT_FULL_STRENGTH;
        float frac = clampf(demand_px_per_ms / calib, 0.0f, 1.0f);
        stick_mag = fminf(frac * STICK_FULLf, (float)cfg->magnetic_max_pull);
    } else {
        // Legacy open-loop pull. Far targets give a small err/ring ratio, so a
        // linear curve collapses the pull and it stalls before arriving; close
        // targets overshoot under any aggressive curve. Blend a strong
        // far-emphasis curve (0.45) toward the user's configured curve as the
        // target gets closer - the user's value is the close-range ceiling and is
        // never overridden.
        float user_curve = clampf(cfg->magnetic_curve, 0.1f, 2.0f);
        float far_curve  = fminf(user_curve, 0.45f);
        float curve = far_curve + (user_curve - far_curve) * dist_factor;
        float normalised = powf(clampf(err_mag / fmaxf(1.0f, (float)fov), 0.0f, 1.0f), curve);
        stick_mag = eff_strength * (float)cfg->magnetic_max_pull * normalised;
    }

    // Fine-approach taper - legacy path ONLY. The calibrated demand is already
    // proportional to the error, so it eases toward zero on its own; applying the
    // taper too would double-damp it exactly where the crosshair should be gluing
    // to centre.
    if (calib <= 0.0f) {
        float fine_radius = fmaxf(1.0f, bbox_min * 0.25f);
        if (err_mag < fine_radius) stick_mag *= err_mag / fine_radius;
    }

    float ux = err_x / err_mag;
    float uy = err_y / err_mag;

    float corr_x = ux * stick_mag;
    float corr_y_raw = uy * stick_mag;
    float corr_y = cfg->magnetic_invert_y ? corr_y_raw : -corr_y_raw;

    // Magnet corrects the residual; feed-forward carries the target's own motion.
    *out_dx = clampf(corr_x + ff_x, -STICK_FULLf, STICK_FULLf);
    *out_dy = clampf(corr_y + ff_y, -STICK_FULLf, STICK_FULLf);
}

static void process_aim_smooth(macro_gamepad_state_t *s, const macro_definition_t *m,
                               macro_runtime_t *rt)
{
    const aim_assist_config_t *cfg = &m->aim_assist;

    float raw_dx, raw_dy;
    aim_smooth_raw_correction(s, m, rt, &raw_dx, &raw_dy);

    // ── Temporal output smoothing ────────────────────────────────────────────
    // OutputSmoothing maps to an exponential time-constant in ms, not a per-tick
    // fraction: the old (alpha = 1 - OutputSmoothing) mapping was calibrated for
    // ~16 ms polling, and at 1 ms even max smoothing converged in tens of ms so
    // the pull felt instant. Mapping to tau gives a genuinely slow, humanised
    // ramp:  0 -> instant;  0.4 -> tau ~80 ms;  0.8 -> ~320 ms;  0.95 -> ~451 ms.
    // On a gate-fail tick raw is (0,0) and the velocity decays over the same tau,
    // so the stick eases back to neutral instead of cutting.
    //
    // The tau constants assume a ~1 ms tick, which is what the C# app ran at. The
    // physical XInput controller here reports at ~1 kHz (250-1000 Hz depending on
    // the pad), so the calibration carries over; a slower pad simply smooths
    // slightly less than the label suggests.
    float alpha;
    float os = clampf(cfg->output_smoothing, 0.0f, 0.95f);
    if (os <= 0.0f) {
        alpha = 1.0f;
    } else {
        float tau_ms = os * os * 500.0f;
        alpha = 1.0f - expf(-1.0f / tau_ms);
    }
    rt->aim_smooth_velocity_x = rt->aim_smooth_velocity_x * (1.0f - alpha) + raw_dx * alpha;
    rt->aim_smooth_velocity_y = rt->aim_smooth_velocity_y * (1.0f - alpha) + raw_dy * alpha;

    int dx = round_to_stick(rt->aim_smooth_velocity_x);
    int dy = round_to_stick(rt->aim_smooth_velocity_y);

    if (dx == 0 && dy == 0) {
        // Easing out and the residual is sub-pixel: snap the accumulators so a
        // stale fraction cannot seed the next engagement with phantom momentum.
        // During ramp-up (raw != 0) keep them so the EMA can climb past 0.5.
        if (raw_dx == 0.0f && raw_dy == 0.0f) {
            rt->aim_smooth_velocity_x = 0.0f;
            rt->aim_smooth_velocity_y = 0.0f;
        }
        return;
    }

    // Additive on RS - never overwrite the player's own input, so they can fight
    // or override the assist. Same composition as NoRecoil and TrackingAssist.
    s->rx = clamp_s16((int32_t)s->rx + dx);
    s->ry = clamp_s16((int32_t)s->ry + dy);
}

// ═════════════════════════════════════════════════════════════════════════════
//  TriggerBot
// ═════════════════════════════════════════════════════════════════════════════
static void process_trigger_bot(macro_gamepad_state_t *s, const macro_definition_t *m,
                                macro_runtime_t *rt)
{
    const aim_assist_config_t *cfg = &m->aim_assist;
    int64_t now = now_ms();

    bool burst_active = rt->trigger_bot_burst_start_tick != 0 &&
                        (now - rt->trigger_bot_burst_start_tick) < cfg->trigger_burst_ms;

    aim_target_t target;
    bool gate_ok = aim_context_try_acquire(cfg, &target);
    bool crosshair_on_target = gate_ok && aim_context_crosshair_inside_bbox(&target);

    if (burst_active) {
        // ReleaseOnTargetLost decides whether a transient lock-loss aborts the
        // burst. Most users want that (no stray shots into empty air); false
        // makes every started burst complete, which suits full-auto weapons
        // where releasing early stutters the fire.
        if (cfg->trigger_release_on_target_lost && !gate_ok) {
            rt->trigger_bot_burst_start_tick = 0;
            return; // leave the fire input to the player / other macros
        }
        raise_fire_input(s, m->trigger_source);
        return;
    }

    if (gate_ok && crosshair_on_target) {
        rt->trigger_bot_burst_start_tick = now;
        raise_fire_input(s, m->trigger_source);
    } else {
        // Burst ended naturally last tick; clear the marker so the next
        // acquisition can fire immediately. We never lower the fire input - the
        // player is back in control.
        rt->trigger_bot_burst_start_tick = 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Dispatch
// ═════════════════════════════════════════════════════════════════════════════
void macro_engine_process(macro_gamepad_state_t *state)
{
    if (!s_macros || s_order_count == 0) return;

    for (int k = 0; k < s_order_count; k++) {
        int slot = s_order[k];
        const macro_definition_t *m = &s_macros[slot];
        macro_runtime_t *rt = &s_runtimes[slot];

        switch (m->type) {
            case MACRO_NO_RECOIL:           process_no_recoil(state, m, rt);           break;
            case MACRO_AUTO_FIRE:           process_auto_fire(state, m, rt);           break;
            case MACRO_AUTO_PING:           process_auto_ping(state, m, rt);           break;
            case MACRO_REMAP:               process_remap(state, m);                   break;
            case MACRO_SEQUENCE:            process_sequence(state, m, rt);            break;
            case MACRO_TOGGLE:              process_toggle(state, m, rt);              break;
            case MACRO_AIM_ASSIST_BUFF:     process_aim_assist_buff(state, m, rt);     break;
            case MACRO_HEAD_ASSIST:         process_head_assist(state, m, rt);         break;
            case MACRO_SCRIPTED_SHAPE:      process_scripted_shape(state, m, rt);      break;
            case MACRO_PROGRESSIVE_RECOIL:  process_progressive_recoil(state, m, rt);  break;
            case MACRO_TRACKING_ASSIST:     process_tracking_assist(state, m, rt);     break;
            case MACRO_AUTO_FIRE_NO_RECOIL: process_auto_fire_no_recoil(state, m, rt); break;
            case MACRO_INSTA_DROP_SHOT:     process_insta_drop_shot(state, m, rt);     break;
            case MACRO_JUMP_SHOT:           process_jump_shot(state, m, rt);           break;
            case MACRO_STRAFE_SHOT:         process_strafe_shot(state, m, rt);         break;
            case MACRO_HOLD_BREATH:         process_hold_breath(state, m, rt);         break;
            case MACRO_SLIDE_CANCEL:        process_slide_cancel(state, m, rt);        break;
            case MACRO_FAST_DROP:           process_fast_drop(state, m, rt);           break;
            case MACRO_AUTO_SPRINT:         process_auto_sprint(state, m, rt);         break;
            case MACRO_CROWBAR:             process_crowbar(state, m, rt);             break;
            case MACRO_CUSTOM:              process_custom_script(state, m, rt);       break;
            case MACRO_LUA_SCRIPT:          process_lua_script(state, m, rt);          break;
            case MACRO_AIM_SNAP:            process_aim_snap(state, m, rt);            break;
            case MACRO_AIM_SMOOTH:          process_aim_smooth(state, m, rt);          break;
            case MACRO_TRIGGER_BOT:         process_trigger_bot(state, m, rt);         break;
            case MACRO_ADAPTIVE_RECOIL:     process_adaptive_recoil(state, m, rt);     break;
            default: break;
        }
    }
}
