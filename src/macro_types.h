#ifndef MACRO_TYPES_H
#define MACRO_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// ── Capacity caps ────────────────────────────────────────────────────────────
// Chosen so one profile stays comfortably under 16 KB (see the static asserts):
// 16 macros is more than any real setup uses, 8 sequence steps covers every
// canned combo, 16 script steps covers the Cronus-style sequencer patterns
// people actually write (press/wait/release/loop). Raising these is a
// flash-layout change - bump PROFILE_STORE_VERSION if you do.
#define MACROS_PER_PROFILE      16
#define MACRO_MAX_SEQ_STEPS      8
#define MACRO_MAX_SCRIPT_STEPS  16
#define MACRO_NAME_LEN          24

// ── GamepadButton - bit-identical to ReflexX.Domain.Enums.GamepadButton ──────
// Kept as a flags mask (not an index) because the source stores button sets in
// a single field and macros test/set them with bitwise ops. Value 0 (None)
// doubles as "not configured", which is exactly how the C# `GamepadButton?`
// nullable behaved at every call site in MacroProcessor.
#define GP_NONE            0x00000000u
#define GP_DPAD_UP         0x00000001u
#define GP_DPAD_DOWN       0x00000002u
#define GP_DPAD_LEFT       0x00000004u
#define GP_DPAD_RIGHT      0x00000008u
#define GP_START           0x00000010u
#define GP_BACK            0x00000020u
#define GP_LEFT_THUMB      0x00000040u
#define GP_RIGHT_THUMB     0x00000080u
#define GP_LEFT_SHOULDER   0x00000100u
#define GP_RIGHT_SHOULDER  0x00000200u
#define GP_GUIDE           0x00000400u
#define GP_A               0x00001000u
#define GP_B               0x00002000u
#define GP_X               0x00004000u
#define GP_Y               0x00008000u

#define GP_MISC            0x00100000u

// ── Ordinal enums (values match C# exactly) ──────────────────────────────────
typedef enum {
    MACRO_AIM_SNAP,
    MACRO_TYPE_COUNT
} macro_type_t;

typedef enum {
    TRIG_SRC_NONE = 0,
    TRIG_SRC_LEFT_TRIGGER,
    TRIG_SRC_RIGHT_TRIGGER,
    TRIG_SRC_LEFT_SHOULDER,
    TRIG_SRC_RIGHT_SHOULDER,
    TRIG_SRC_DUAL_TRIGGER,
    TRIG_SRC_DUAL_SHOULDER
} trigger_source_t;

typedef enum {
    SHAPE_FLICK = 0,
    SHAPE_CIRCLE,
    SHAPE_HORIZONTAL_OVAL,
    SHAPE_VERTICAL_OVAL,
    SHAPE_DIAGONAL_OVAL
} shape_kind_t;

typedef enum {
    EASE_LINEAR = 0,
    EASE_OUT_QUAD,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_SINE,
    EASE_OUT_BACK,
    EASE_SMOOTHSTEP
} easing_kind_t;

typedef enum { STICK_LEFT = 0, STICK_RIGHT } stick_target_t;

typedef enum { DIST_SHORT = 0, DIST_MEDIUM, DIST_LONG } distance_level_t;

typedef enum {
    DSRC_TRIGGER_HOLD_TIME = 0,
    DSRC_AIM_STICK_DEFLECTION,
    DSRC_RECOIL_MAGNITUDE,
    DSRC_MANUAL,
    DSRC_AUTO
} distance_source_t;

typedef enum {
    AXIS_LEFT_STICK_X = 0,
    AXIS_LEFT_STICK_Y,
    AXIS_RIGHT_STICK_X,
    AXIS_RIGHT_STICK_Y,
    AXIS_LEFT_TRIGGER,
    AXIS_RIGHT_TRIGGER
} analog_axis_t;

typedef enum {
    SACT_PRESS_BUTTON = 0,
    SACT_RELEASE_BUTTON,
    SACT_SET_AXIS,
    SACT_SET_TRIGGER,
    SACT_WAIT,
    SACT_LOOP_BACK,
    SACT_LOOP_START
} script_action_t;

typedef enum {
    STRIG_WHILE_HELD = 0,
    STRIG_ON_PRESS,
    STRIG_TOGGLE
} script_trigger_t;

// ── Gamepad state the engine operates on ─────────────────────────────────────
// The C# GamepadState is a class with StickPosition/TriggerValue value objects.
// Flattened here: hid_app.c fills one of these from its Phase-3 locals, hands it
// to macro_engine_process(), then copies the result back. Same int16 stick /
// uint8 trigger domain as the XInput wire format on both ends, so the engine
// never introduces a quantisation step of its own.
typedef struct {
    uint32_t buttons;   // GP_* mask
    int16_t  lx, ly;
    int16_t  rx, ry;
    uint8_t  lt, rt;
    uint8_t  _pad[2];
} macro_gamepad_state_t;

// TriggerValue.IsPressed(threshold = 30) - the source's default, replicated so
// every trigger gate in the engine trips at the same point it did on Windows.
#define TRIGGER_PRESS_THRESHOLD 30

static inline bool gs_pressed(const macro_gamepad_state_t *s, uint32_t btn)
{
    return btn != 0u && (s->buttons & btn) != 0u;
}

static inline void gs_set_button(macro_gamepad_state_t *s, uint32_t btn, bool pressed)
{
    if (btn == 0u) return;                  // GP_NONE is a no-op, as in C#
    if (pressed) s->buttons |= btn;
    else         s->buttons &= ~btn;
}

static inline bool gs_lt_pressed(const macro_gamepad_state_t *s) { return s->lt >= TRIGGER_PRESS_THRESHOLD; }
static inline bool gs_rt_pressed(const macro_gamepad_state_t *s) { return s->rt >= TRIGGER_PRESS_THRESHOLD; }

// int16 saturating add - the C# code does Math.Clamp(x + d, short.MinValue,
// short.MaxValue) on every additive stick write. Note the source clamps to
// short.MinValue (-32768) not -32767, so we match that exactly.
static inline int16_t clamp_s16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ── MotionScript ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t shape;          // shape_kind_t
    uint8_t target;         // stick_target_t
    uint8_t easing;         // easing_kind_t
    uint8_t clockwise;      // bool
    uint8_t additive;       // bool
    uint8_t _pad[3];
    float   radius_x_norm;
    float   radius_y_norm;
    float   rotation_deg;
    float   period_ms;
    float   duration_ms;
    float   direction_deg;
    float   amplitude_norm;
    float   start_phase_deg;
    float   intensity_mul;
} motion_script_t;

// ── HeadAssistConfig ─────────────────────────────────────────────────────────
typedef struct {
    motion_script_t short_range;
    motion_script_t medium_range;
    motion_script_t long_range;

    uint8_t  distance_source;    // distance_source_t
    uint8_t  fire_on_press;      // bool
    uint8_t  fire_once;          // bool
    uint8_t  _pad;

    uint32_t cycle_button;       // GP_* mask, GP_NONE = unset

    float    short_hold_ms_max;
    float    medium_hold_ms_max;
    float    deflection_short_max;
    float    deflection_medium_max;
    float    recoil_short_max;
    float    recoil_medium_max;
    float    weight_trigger;
    float    weight_deflection;
    float    weight_recoil;

    int32_t  refire_cooldown_ms;
    int32_t  min_trigger_hold_ms;
} head_assist_config_t;

// ── ProgressiveRecoilConfig ──────────────────────────────────────────────────
typedef struct {
    int32_t total_ammo;
    float   full_mag_duration_ms;

    int32_t start_comp_x, start_comp_y;
    int32_t mid_comp_x,   mid_comp_y;
    int32_t end_comp_x,   end_comp_y;

    uint8_t second_stage_enabled;        // bool
    uint8_t second_stage_trigger_source; // trigger_source_t
    uint8_t phase_easing;                // easing_kind_t
    uint8_t _pad;
    uint32_t second_stage_activation_button; // GP_* mask, GP_NONE = unset
    int32_t second_stage_comp_x, second_stage_comp_y;

    float   noise_factor;
    float   sensitivity_scale;
} progressive_recoil_config_t;

// ── TrackingAssistConfig ─────────────────────────────────────────────────────
typedef struct {
    uint8_t shape;        // shape_kind_t (informational; the orbit is always circular)
    uint8_t target;       // stick_target_t
    uint8_t easing;       // easing_kind_t (informational, kept for parity)
    uint8_t clockwise;    // bool
    uint8_t free_orbit;   // bool
    uint8_t _pad[3];
    float   base_radius_norm;
    float   max_radius_norm;
    float   period_ms;
    float   deflection_threshold;
    float   scale_curve;
    float   intensity_mul;
} tracking_assist_config_t;

// ── CrowBarConfig ────────────────────────────────────────────────────────────
typedef struct {
    uint8_t mode;         // crowbar_mode_t
    uint8_t _pad[3];
    int32_t base_htg_value;
    int32_t max_compensation;
    float   assist_factor;
    float   deflection_threshold;
    float   deflection_curve;
    float   noise_factor;
    float   htg_scale_padrao;
} crowbar_config_t;

// ── AdaptiveRecoilConfig ─────────────────────────────────────────────────────
typedef struct {
    int32_t min_compensation_x, min_compensation_y;
    int32_t max_compensation_x, max_compensation_y;
    float   intensity;
    float   randomization_factor;
} adaptive_recoil_config_t;

// ── AimAssistConfig (shared by the three Tier-3 AI macros) ───────────────────
// Fully carried across even though the vision front-end is a stub on this
// hardware (see aim_context.h) - the whole point is that a future vision
// coprocessor drops in without touching this file, the flash layout, or the
// configurator's wire format.
typedef struct {
    int32_t max_fov_pixels;
    int32_t max_target_age_ms;
    float   min_confidence;
    float   max_target_center_y;
    float   min_target_center_y;
    float   headshot_bias_fraction;
    float   humanization_sigma;

    int32_t snap_max_impulse_stick_units;
    int32_t snap_impulse_duration_ms;
    int32_t snap_cooldown_ms;

    float   magnetic_strength;
    float   magnetic_strength_fire;
    int32_t magnetic_max_pull;
    float   magnetic_curve;
    uint8_t magnetic_invert_y;      // bool
    uint8_t magnetic_sticky_lock;   // bool
    uint8_t trigger_release_on_target_lost; // bool
    uint8_t _pad;

    float   prediction_ms;
    float   output_smoothing;
    float   ego_motion_gain;
    float   tracking_feed_forward;
    int32_t trigger_burst_ms;
} aim_assist_config_t;

// ── Sequence step (MacroStep) ────────────────────────────────────────────────
typedef struct {
    uint32_t button_press;    // GP_* mask, GP_NONE = none
    uint32_t button_release;
    int32_t  delay_after_ms;
} macro_step_t;

// ── Custom-script step (ScriptStep) ──────────────────────────────────────────
typedef struct {
    uint8_t  action;            // script_action_t
    uint8_t  axis;              // analog_axis_t
    uint8_t  has_axis;          // bool - mirrors C# `AnalogAxis?`
    uint8_t  disabled;          // bool
    uint32_t button;            // GP_* mask, GP_NONE = none
    int16_t  value;             // SetAxis: -32767..32767; SetTrigger: 0..255
    int16_t  _pad;
    int32_t  duration_ms;
    int32_t  loop_target_index;
    int32_t  repeat_count;
} script_step_t;

// ── ScriptDefinition ─────────────────────────────────────────────────────────
typedef struct {
    int32_t      step_count;
    uint8_t      trigger_mode;   // script_trigger_t
    uint8_t      auto_loop;      // bool
    uint8_t      _pad[2];
    float        speed_multiplier;
    int32_t      random_delay_min_ms;
    int32_t      random_delay_max_ms;
    int32_t      max_repeat_count;
    int32_t      stop_after_ms;
    script_step_t steps[MACRO_MAX_SCRIPT_STEPS];
} script_definition_t;

// ── MacroDefinition ──────────────────────────────────────────────────────────

typedef struct {
    char     name[MACRO_NAME_LEN];

    uint8_t  type;               // macro_type_t
    uint8_t  enabled;            // bool
    uint8_t  toggle_mode;        // bool
    uint8_t  trigger_source;     // trigger_source_t
    int32_t  priority;

    uint32_t activation_button;  // GP_* mask, GP_NONE = unset ("always active")

    // Timing
    int32_t  delay_ms;
    int32_t  interval_ms;
    int32_t  duration_ms;
    uint8_t  loop;               // bool
    uint8_t  activate_on_ads;    // bool
    uint8_t  _pad0[2];

    // Sequence
    int32_t  step_count;
    macro_step_t steps[MACRO_MAX_SEQ_STEPS];

    // Type-specific config blocks
    aim_assist_config_t          aim_assist;
    script_definition_t          script;
} macro_definition_t;

// ── MacroRuntime ─────────────────────────────────────────────────────────────
typedef struct {
    int64_t last_fire_tick;
    int64_t pulse_until_tick;
    uint8_t toggle_state;
    uint8_t was_pressed;
    int32_t step_index;

    // Custom script sequencer
    int32_t script_step_index;
    int64_t script_step_start_tick;
    uint8_t script_completed;
    uint8_t script_was_triggered;
    int32_t script_loop_count;
    int64_t script_first_start_tick;
    int32_t script_current_wait_jitter;
    // C# lazily allocates int[Steps.Count]; a fixed array is the MCU equivalent.
    int32_t loop_counters[MACRO_MAX_SCRIPT_STEPS];

    // LuaScript - the C# runtime holds an IScriptInstance here. There is no
    // embedded Lua VM in this pass (see macro_engine.c), so only the tick anchor
    // survives; it is what a future VM would need to compute its delta.
    int64_t lua_last_tick_ms;

    // TriggerBot
    int64_t trigger_bot_burst_start_tick;

    // AimSmooth
    float   aim_smooth_velocity_x;
    float   aim_smooth_velocity_y;
    uint8_t aim_smooth_holding;
    uint8_t aim_smooth_has_target;
    int64_t aim_smooth_last_lock_id;     // -1 = none seen yet
    int64_t aim_smooth_engaged_lock_id;  // -1 = nothing committed

    // AimSnap
    uint8_t aim_snap_button_was_pressed;
    int64_t aim_snap_impulse_end_tick;   // microseconds (sub-ms window)
    int64_t aim_snap_cooldown_end_tick;  // microseconds
    int32_t aim_snap_impulse_dx;
    int32_t aim_snap_impulse_dy;
} macro_runtime_t;

// Layout locks. If one of these fires you changed the wire format: bump
// PROFILE_STORE_VERSION in profile_store.h and update
// tools/configurator/PROTOCOL.md's offset tables to match.
_Static_assert(sizeof(motion_script_t)    == 44,  "motion_script_t layout changed");
_Static_assert(sizeof(macro_step_t)       == 12,  "macro_step_t layout changed");
_Static_assert(sizeof(script_step_t)      == 24,  "script_step_t layout changed");
_Static_assert(sizeof(macro_gamepad_state_t) == 16, "macro_gamepad_state_t layout changed");
_Static_assert(sizeof(aim_assist_config_t)         ==  80, "aim_assist_config_t layout changed");
_Static_assert(sizeof(script_definition_t)         == 412, "script_definition_t layout changed");
_Static_assert(sizeof(macro_definition_t)          == 1100, "macro_definition_t layout changed");

#endif // MACRO_TYPES_H
